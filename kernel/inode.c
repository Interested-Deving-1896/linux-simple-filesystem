// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/iversion.h>
#include <linux/string.h>
#include <linux/time.h>

#include "simplefs.h"
#include "simplefs_internal.h"

#define SIMPLEFS_ROOT_INO 1U

static struct inode *simplefs_iget(struct super_block *sb, unsigned long ino)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct inode *inode;
	struct timespec64 now;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	now = current_time(inode);
	inode_set_atime_to_ts(inode, now);
	inode_set_mtime_to_ts(inode, now);
	inode_set_ctime_to_ts(inode, now);
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);

	if (ino == SIMPLEFS_ROOT_INO) {
		inode->i_mode  = S_IFDIR | 0755;
		inode->i_op    = &simplefs_dir_inode_ops;
		inode->i_fop   = &simplefs_dir_ops;
		set_nlink(inode, 2);
		inode->i_size  = sbi->file_count;
	} else {
		unsigned int idx = ino - 2;

		if (idx >= sbi->file_count) {
			iget_failed(inode);
			return ERR_PTR(-ENOENT);
		}
		inode->i_mode    = S_IFREG | 0644;
		inode->i_op      = &simplefs_file_inode_ops;
		inode->i_fop     = &simplefs_file_ops;
		inode->i_mapping->a_ops = &simplefs_aops;
		set_nlink(inode, 1);
		inode->i_size    = (loff_t)sbi->files[idx].nsect *
				   SIMPLEFS_SECTOR;
		inode->i_blocks  = sbi->files[idx].nsect;
	}

	unlock_new_inode(inode);
	return inode;
}

struct inode *simplefs_make_root_inode(struct super_block *sb)
{
	return simplefs_iget(sb, SIMPLEFS_ROOT_INO);
}

struct inode *simplefs_make_file_inode(struct super_block *sb,
				       unsigned int idx)
{
	return simplefs_iget(sb, (unsigned long)idx + 2);
}

/* ------------------------------------------------------------------ */
/* Directory operations                                               */
/* ------------------------------------------------------------------ */

static int simplefs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
	unsigned int idx;

	if (!dir_emit_dots(file, ctx))
		return 0;

	idx = ctx->pos - 2;
	while (idx < sbi->file_count) {
		const char *name = sbi->files[idx].name;
		size_t nlen = strnlen(name, sbi->name_max);

		if (!dir_emit(ctx, name, nlen,
			      (unsigned long)idx + 2, DT_REG))
			break;
		ctx->pos++;
		idx++;
	}
	return 0;
}

static struct dentry *simplefs_lookup(struct inode *dir,
				      struct dentry *dentry,
				      unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct inode *inode = NULL;
	unsigned int i;

	if (dentry->d_name.len > sbi->name_max)
		return ERR_PTR(-ENAMETOOLONG);

	for (i = 0; i < sbi->file_count; i++) {
		size_t nlen = strnlen(sbi->files[i].name, sbi->name_max);

		if (nlen == dentry->d_name.len &&
		    !memcmp(sbi->files[i].name, dentry->d_name.name, nlen)) {
			inode = simplefs_make_file_inode(sb, i);
			if (IS_ERR(inode))
				return ERR_CAST(inode);
			break;
		}
	}
	return d_splice_alias(inode, dentry);
}

const struct file_operations simplefs_dir_ops = {
	.owner          = THIS_MODULE,
	.read           = generic_read_dir,
	.iterate_shared = simplefs_readdir,
	.llseek         = generic_file_llseek,
	.unlocked_ioctl = simplefs_ioctl,
	.compat_ioctl   = simplefs_ioctl,
};

const struct inode_operations simplefs_dir_inode_ops = {
	.lookup = simplefs_lookup,
};
