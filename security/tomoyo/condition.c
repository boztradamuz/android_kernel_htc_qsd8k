/*
 * security/tomoyo/condition.c
 *
 * Copyright (C) 2005-2011  NTT DATA CORPORATION
 */

#include "common.h"
#include <linux/slab.h>

/* List of "struct tomoyo_condition". */
LIST_HEAD(tomoyo_condition_list);

/**
 * tomoyo_same_condition - Check for duplicated "struct tomoyo_condition" entry.
 *
 * @a: Pointer to "struct tomoyo_condition".
 * @b: Pointer to "struct tomoyo_condition".
 *
 * Returns true if @a == @b, false otherwise.
 */
static inline bool tomoyo_same_condition(const struct tomoyo_condition *a,
					 const struct tomoyo_condition *b)
{
	return a->size == b->size && a->condc == b->condc &&
		a->numbers_count == b->numbers_count &&
		!memcmp(a + 1, b + 1, a->size - sizeof(*a));
}

/**
 * tomoyo_condition_type - Get condition type.
 *
 * @word: Keyword string.
 *
 * Returns one of values in "enum tomoyo_conditions_index" on success,
 * TOMOYO_MAX_CONDITION_KEYWORD otherwise.
 */
static u8 tomoyo_condition_type(const char *word)
{
	u8 i;
	for (i = 0; i < TOMOYO_MAX_CONDITION_KEYWORD; i++) {
		if (!strcmp(word, tomoyo_condition_keyword[i]))
			break;
	}
	return i;
}

/* Define this to enable debug mode. */
/* #define DEBUG_CONDITION */

#ifdef DEBUG_CONDITION
#define dprintk printk
#else
#define dprintk(...) do { } while (0)
#endif

/**
 * tomoyo_commit_condition - Commit "struct tomoyo_condition".
 *
 * @entry: Pointer to "struct tomoyo_condition".
 *
 * Returns pointer to "struct tomoyo_condition" on success, NULL otherwise.
 *
 * This function merges duplicated entries. This function returns NULL if
 * @entry is not duplicated but memory quota for policy has exceeded.
 */
static struct tomoyo_condition *tomoyo_commit_condition
(struct tomoyo_condition *entry)
{
	struct tomoyo_condition *ptr;
	bool found = false;
	if (mutex_lock_interruptible(&tomoyo_policy_lock)) {
		dprintk(KERN_WARNING "%u: %s failed\n", __LINE__, __func__);
		ptr = NULL;
		found = true;
		goto out;
	}
	list_for_each_entry_rcu(ptr, &tomoyo_condition_list, head.list) {
		if (!tomoyo_same_condition(ptr, entry))
			continue;
		/* Same entry found. Share this entry. */
		atomic_inc(&ptr->head.users);
		found = true;
		break;
	}
	if (!found) {
		if (tomoyo_memory_ok(entry)) {
			atomic_set(&entry->head.users, 1);
			list_add_rcu(&entry->head.list,
				     &tomoyo_condition_list);
		} else {
			found = true;
			ptr = NULL;
		}
	}
	mutex_unlock(&tomoyo_policy_lock);
out:
	if (found) {
		tomoyo_del_condition(&entry->head.list);
		kfree(entry);
		entry = ptr;
	}
	return entry;
}

/**
 * tomoyo_get_condition - Parse condition part.
 *
 * @param: Pointer to "struct tomoyo_acl_param".
 *
 * Returns pointer to "struct tomoyo_condition" on success, NULL otherwise.
 */
struct tomoyo_condition *tomoyo_get_condition(struct tomoyo_acl_param *param)
{
	struct tomoyo_condition *entry = NULL;
	struct tomoyo_condition_element *condp = NULL;
	struct tomoyo_number_union *numbers_p = NULL;
	struct tomoyo_condition e = { };
	char * const start_of_string = param->data;
	char * const end_of_string = start_of_string + strlen(start_of_string);
	char *pos;
rerun:
	pos = start_of_string;
	while (1) {
		u8 left = -1;
		u8 right = -1;
		char *left_word = pos;
		char *cp;
		char *right_word;
		bool is_not;
		if (!*left_word)
			break;
		/*
		 * Since left-hand condition does not allow use of "path_group"
		 * or "number_group" and environment variable's names do not
		 * accept '=', it is guaranteed that the original line consists
		 * of one or more repetition of $left$operator$right blocks
		 * where "$left is free from '=' and ' '" and "$operator is
		 * either '=' or '!='" and "$right is free from ' '".
		 * Therefore, we can reconstruct the original line at the end
		 * of dry run even if we overwrite $operator with '\0'.
		 */
		cp = strchr(pos, ' ');
		if (cp) {
			*cp = '\0'; /* Will restore later. */
			pos = cp + 1;
		} else {
			pos = "";
		}
		right_word = strchr(left_word, '=');
		if (!right_word || right_word == left_word)
			goto out;
		is_not = *(right_word - 1) == '!';
		if (is_not)
			*(right_word++ - 1) = '\0'; /* Will restore later. */
		else if (*(right_word + 1) != '=')
			*right_word++ = '\0'; /* Will restore later. */
		else
			goto out;
		dprintk(KERN_WARNING "%u: <%s>%s=<%s>\n", __LINE__, left_word,
			is_not ? "!" : "", right_word);
		left = tomoyo_condition_type(left_word);
		dprintk(KERN_WARNING "%u: <%s> left=%u\n", __LINE__, left_word,
			left);
		if (left == TOMOYO_MAX_CONDITION_KEYWORD) {
			if (!numbers_p) {
				e.numbers_count++;
			} else {
				e.numbers_count--;
				left = TOMOYO_NUMBER_UNION;
				param->data = left_word;
				if (*left_word == '@' ||
				    !tomoyo_parse_number_union(param,
							       numbers_p++))
					goto out;
			}
		}
		if (!condp)
			e.condc++;
		else
			e.condc--;
		right = tomoyo_condition_type(right_word);
		if (right == TOMOYO_MAX_CONDITION_KEYWORD) {
			if (!numbers_p) {
				e.numbers_count++;
			} else {
				e.numbers_count--;
				right = TOMOYO_NUMBER_UNION;
				param->data = right_word;
				if (!tomoyo_parse_number_union(param,
							       numbers_p++))
					goto out;
			}
		}
		if (!condp) {
			dprintk(KERN_WARNING "%u: dry_run left=%u right=%u "
				"match=%u\n", __LINE__, left, right, !is_not);
			continue;
		}
		condp->left = left;
		condp->right = right;
		condp->equals = !is_not;
		dprintk(KERN_WARNING "%u: left=%u right=%u match=%u\n",
			__LINE__, condp->left, condp->right,
			condp->equals);
		condp++;
	}
	dprintk(KERN_INFO "%u: cond=%u numbers=%u\n",
		__LINE__, e.condc, e.numbers_count);
	if (entry) {
		BUG_ON(e.numbers_count | e.condc);
		return tomoyo_commit_condition(entry);
	}
	e.size = sizeof(*entry)
		+ e.condc * sizeof(struct tomoyo_condition_element)
		+ e.numbers_count * sizeof(struct tomoyo_number_union);
	entry = kzalloc(e.size, GFP_NOFS);
	if (!entry)
		return NULL;
	*entry = e;
	condp = (struct tomoyo_condition_element *) (entry + 1);
	numbers_p = (struct tomoyo_number_union *) (condp + e.condc);
	{
		bool flag = false;
		for (pos = start_of_string; pos < end_of_string; pos++) {
			if (*pos)
				continue;
			if (flag) /* Restore " ". */
				*pos = ' ';
			else if (*(pos + 1) == '=') /* Restore "!=". */
				*pos = '!';
			else /* Restore "=". */
				*pos = '=';
			flag = !flag;
		}
	}
	goto rerun;
out:
	dprintk(KERN_WARNING "%u: %s failed\n", __LINE__, __func__);
	if (entry) {
		tomoyo_del_condition(&entry->head.list);
		kfree(entry);
	}
	return NULL;
}

/**
 * tomoyo_get_attributes - Revalidate "struct inode".
 *
 * @obj: Pointer to "struct tomoyo_obj_info".
 *
 * Returns nothing.
 */
void tomoyo_get_attributes(struct tomoyo_obj_info *obj)
{
	u8 i;
	struct dentry *dentry = NULL;

	for (i = 0; i < TOMOYO_MAX_PATH_STAT; i++) {
		struct inode *inode;
		switch (i) {
		case TOMOYO_PATH1:
			dentry = obj->path1.dentry;
			if (!dentry)
				continue;
			break;
		case TOMOYO_PATH2:
			dentry = obj->path2.dentry;
			if (!dentry)
				continue;
			break;
		default:
			if (!dentry)
				continue;
			dentry = dget_parent(dentry);
			break;
		}
		inode = dentry->d_inode;
		if (inode) {
			struct tomoyo_mini_stat *stat = &obj->stat[i];
			stat->uid  = inode->i_uid;
			stat->gid  = inode->i_gid;
			stat->ino  = inode->i_ino;
			stat->mode = inode->i_mode;
			stat->dev  = inode->i_sb->s_dev;
			stat->rdev = inode->i_rdev;
			obj->stat_valid[i] = true;
		}
		if (i & 1) /* i == TOMOYO_PATH1_PARENT ||
			      i == TOMOYO_PATH2_PARENT */
			dput(dentry);
	}
}

/**
 * tomoyo_condition - Check condition part.
 *
 * @r:    Pointer to "struct tomoyo_request_info".
 * @cond: Pointer to "struct tomoyo_condition". Maybe NULL.
 *
 * Returns true on success, false otherwise.
 *
 * Caller holds tomoyo_read_lock().
 */
bool tomoyo_condition(struct tomoyo_request_info *r,
		      const struct tomoyo_condition *cond)
{
	u32 i;
	unsigned long min_v[2] = { 0, 0 };
	unsigned long max_v[2] = { 0, 0 };
	const struct tomoyo_condition_element *condp;
	const struct tomoyo_number_union *numbers_p;
	struct tomoyo_obj_info *obj;
	u16 condc;
	if (!cond)
		return true;
	condc = cond->condc;
	obj = r->obj;
	condp = (struct tomoyo_condition_element *) (cond + 1);
	numbers_p = (const struct tomoyo_number_union *) (condp + condc);
	for (i = 0; i < condc; i++) {
		const bool match = condp->equals;
		const u8 left = condp->left;
		const u8 right = condp->right;
		bool is_bitop[2] = { false, false };
		u8 j;
		condp++;
		/* Check numeric or bit-op expressions. */
		for (j = 0; j < 2; j++) {
			const u8 index = j ? right : left;
			unsigned long value = 0;
			switch (index) {
			case TOMOYO_TASK_UID:
				value = current_uid();
				break;
			case TOMOYO_TASK_EUID:
				value = current_euid();
				break;
			case TOMOYO_TASK_SUID:
				value = current_suid();
				break;
			case TOMOYO_TASK_FSUID:
				value = current_fsuid();
				break;
			case TOMOYO_TASK_GID:
				value = current_gid();
				break;
			case TOMOYO_TASK_EGID:
				value = current_egid();
				break;
			case TOMOYO_TASK_SGID:
				value = current_sgid();
				break;
			case TOMOYO_TASK_FSGID:
				value = current_fsgid();
				break;
			case TOMOYO_TASK_PID:
				value = tomoyo_sys_getpid();
				break;
			case TOMOYO_TASK_PPID:
				value = tomoyo_sys_getppid();
				break;
			case TOMOYO_TYPE_IS_SOCKET:
				value = S_IFSOCK;
				break;
			case TOMOYO_TYPE_IS_SYMLINK:
				value = S_IFLNK;
				break;
			case TOMOYO_TYPE_IS_FILE:
				value = S_IFREG;
				break;
			case TOMOYO_TYPE_IS_BLOCK_DEV:
				value = S_IFBLK;
				break;
			case TOMOYO_TYPE_IS_DIRECTORY:
				value = S_IFDIR;
				break;
			case TOMOYO_TYPE_IS_CHAR_DEV:
				value = S_IFCHR;
				break;
			case TOMOYO_TYPE_IS_FIFO:
				value = S_IFIFO;
				break;
			case TOMOYO_MODE_SETUID:
				value = S_ISUID;
				break;
			case TOMOYO_MODE_SETGID:
				value = S_ISGID;
				break;
			case TOMOYO_MODE_STICKY:
				value = S_ISVTX;
				break;
			case TOMOYO_MODE_OWNER_READ:
				value = S_IRUSR;
				break;
			case TOMOYO_MODE_OWNER_WRITE:
				value = S_IWUSR;
				break;
			case TOMOYO_MODE_OWNER_EXECUTE:
				value = S_IXUSR;
				break;
			case TOMOYO_MODE_GROUP_READ:
				value = S_IRGRP;
				break;
			case TOMOYO_MODE_GROUP_WRITE:
				value = S_IWGRP;
				break;
			case TOMOYO_MODE_GROUP_EXECUTE:
				value = S_IXGRP;
				break;
			case TOMOYO_MODE_OTHERS_READ:
				value = S_IROTH;
				break;
			case TOMOYO_MODE_OTHERS_WRITE:
				value = S_IWOTH;
				break;
			case TOMOYO_MODE_OTHERS_EXECUTE:
				value = S_IXOTH;
				break;
			case TOMOYO_NUMBER_UNION:
				/* Fetch values later. */
				break;
			default:
				if (!obj)
					goto out;
				if (!obj->validate_done) {
					tomoyo_get_attributes(obj);
					obj->validate_done = true;
				}
				{
					u8 stat_index;
					struct tomoyo_mini_stat *stat;
					switch (index) {
					case TOMOYO_PATH1_UID:
					case TOMOYO_PATH1_GID:
					case TOMOYO_PATH1_INO:
					case TOMOYO_PATH1_MAJOR:
					case TOMOYO_PATH1_MINOR:
					case TOMOYO_PATH1_TYPE:
					case TOMOYO_PATH1_DEV_MAJOR:
					case TOMOYO_PATH1_DEV_MINOR:
					case TOMOYO_PATH1_PERM:
						stat_index = TOMOYO_PATH1;
						break;
					case TOMOYO_PATH2_UID:
					case TOMOYO_PATH2_GID:
					case TOMOYO_PATH2_INO:
					case TOMOYO_PATH2_MAJOR:
					case TOMOYO_PATH2_MINOR:
					case TOMOYO_PATH2_TYPE:
					case TOMOYO_PATH2_DEV_MAJOR:
					case TOMOYO_PATH2_DEV_MINOR:
					case TOMOYO_PATH2_PERM:
						stat_index = TOMOYO_PATH2;
						break;
					case TOMOYO_PATH1_PARENT_UID:
					case TOMOYO_PATH1_PARENT_GID:
					case TOMOYO_PATH1_PARENT_INO:
					case TOMOYO_PATH1_PARENT_PERM:
						stat_index =
							TOMOYO_PATH1_PARENT;
						break;
					case TOMOYO_PATH2_PARENT_UID:
					case TOMOYO_PATH2_PARENT_GID:
					case TOMOYO_PATH2_PARENT_INO:
					case TOMOYO_PATH2_PARENT_PERM:
						stat_index =
							TOMOYO_PATH2_PARENT;
						break;
					default:
						goto out;
					}
					if (!obj->stat_valid[stat_index])
						goto out;
					stat = &obj->stat[stat_index];
					switch (index) {
					case TOMOYO_PATH1_UID:
					case TOMOYO_PATH2_UID:
					case TOMOYO_PATH1_PARENT_UID:
					case TOMOYO_PATH2_PARENT_UID:
						value = stat->uid;
						break;
					case TOMOYO_PATH1_GID:
					case TOMOYO_PATH2_GID:
					case TOMOYO_PATH1_PARENT_GID:
					case TOMOYO_PATH2_PARENT_GID:
						value = stat->gid;
						break;
					case TOMOYO_PATH1_INO:
					case TOMOYO_PATH2_INO:
					case TOMOYO_PATH1_PARENT_INO:
					case TOMOYO_PATH2_PARENT_INO:
						value = stat->ino;
						break;
					case TOMOYO_PATH1_MAJOR:
					case TOMOYO_PATH2_MAJOR:
						value = MAJOR(stat->dev);
						break;
					case TOMOYO_PATH1_MINOR:
					case TOMOYO_PATH2_MINOR:
						value = MINOR(stat->dev);
						break;
					case TOMOYO_PATH1_TYPE:
					case TOMOYO_PATH2_TYPE:
						value = stat->mode & S_IFMT;
						break;
					case TOMOYO_PATH1_DEV_MAJOR:
					case TOMOYO_PATH2_DEV_MAJOR:
						value = MAJOR(stat->rdev);
						break;
					case TOMOYO_PATH1_DEV_MINOR:
					case TOMOYO_PATH2_DEV_MINOR:
						value = MINOR(stat->rdev);
						break;
					case TOMOYO_PATH1_PERM:
					case TOMOYO_PATH2_PERM:
					case TOMOYO_PATH1_PARENT_PERM:
					case TOMOYO_PATH2_PARENT_PERM:
						value = stat->mode & S_IALLUGO;
						break;
					}
				}
				break;
			}
			max_v[j] = value;
			min_v[j] = value;
			switch (index) {
			case TOMOYO_MODE_SETUID:
			case TOMOYO_MODE_SETGID:
			case TOMOYO_MODE_STICKY:
			case TOMOYO_MODE_OWNER_READ:
			case TOMOYO_MODE_OWNER_WRITE:
			case TOMOYO_MODE_OWNER_EXECUTE:
			case TOMOYO_MODE_GROUP_READ:
			case TOMOYO_MODE_GROUP_WRITE:
			case TOMOYO_MODE_GROUP_EXECUTE:
			case TOMOYO_MODE_OTHERS_READ:
			case TOMOYO_MODE_OTHERS_WRITE:
			case TOMOYO_MODE_OTHERS_EXECUTE:
				is_bitop[j] = true;
			}
		}
		if (left == TOMOYO_NUMBER_UNION) {
			/* Fetch values now. */
			const struct tomoyo_number_union *ptr = numbers_p++;
			min_v[0] = ptr->values[0];
			max_v[0] = ptr->values[1];
		}
		if (right == TOMOYO_NUMBER_UNION) {
			/* Fetch values now. */
			const struct tomoyo_number_union *ptr = numbers_p++;
			if (ptr->group) {
				if (tomoyo_number_matches_group(min_v[0],
								max_v[0],
								ptr->group)
				    == match)
					continue;
			} else {
				if ((min_v[0] <= ptr->values[1] &&
				     max_v[0] >= ptr->values[0]) == match)
					continue;
			}
			goto out;
		}
		/*
		 * Bit operation is valid only when counterpart value
		 * represents permission.
		 */
		if (is_bitop[0] && is_bitop[1]) {
			goto out;
		} else if (is_bitop[0]) {
			switch (right) {
			case TOMOYO_PATH1_PERM:
			case TOMOYO_PATH1_PARENT_PERM:
			case TOMOYO_PATH2_PERM:
			case TOMOYO_PATH2_PARENT_PERM:
				if (!(max_v[0] & max_v[1]) == !match)
					continue;
			}
			goto out;
		} else if (is_bitop[1]) {
			switch (left) {
			case TOMOYO_PATH1_PERM:
			case TOMOYO_PATH1_PARENT_PERM:
			case TOMOYO_PATH2_PERM:
			case TOMOYO_PATH2_PARENT_PERM:
				if (!(max_v[0] & max_v[1]) == !match)
					continue;
			}
			goto out;
		}
		/* Normal value range comparison. */
		if ((min_v[0] <= max_v[1] && max_v[0] >= min_v[1]) == match)
			continue;
out:
		return false;
	}
	return true;
}
