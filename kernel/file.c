// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/mpage.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "simplefs.h"
#include "simplefs_internal.h"

/* ------------------------------------------------------------------ */
/* address_space helpers                                              */
/* ------------------------------------------------------------------ */

static int simplefs_get_block(struct inode *inode, sector_t iblock,
			      struct buffer_head *bh_result, int create)
{
	struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
	unsigned int idx;

	if (inode->i_ino < 2)
		return -EIO;
	idx = inode->i_ino - 2;
	if (idx >= sbi->file_count)
		return -EIO;
	if (iblock >= sbi->files[idx].nsect) {
		if (create)
			return -EFBIG;
		return 0;
	}
	map_bh(bh_result, inode->i_sb,
	       sbi->files[idx].start + iblock);
	return 0;
}

static int simplefs_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, simplefs_get_block);
}

static int simplefs_writepages(struct address_space *mapping,
			       struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, simplefs_get_block);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static int simplefs_write_begin(struct file *file,
				struct address_space *mapping,
				loff_t pos, unsigned int len,
				struct folio **foliop, void **fsdata)
{
	return block_write_begin(mapping, pos, len, foliop,
				 simplefs_get_block);
}
#else
static int simplefs_write_begin(struct file *file,
				struct address_space *mapping,
				loff_t pos, unsigned int len,
				struct page **pagep, void **fsdata)
{
	return block_write_begin(mapping, pos, len, pagep,
				 simplefs_get_block);
}
#endif

static sector_t simplefs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, simplefs_get_block);
}

const struct address_space_operations simplefs_aops = {
	.read_folio       = simplefs_read_folio,
	.writepages       = simplefs_writepages,
	.write_begin      = simplefs_write_begin,
	.write_end        = generic_write_end,
	.bmap             = simplefs_bmap,
	.dirty_folio      = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.migrate_folio    = buffer_migrate_folio,
};

/* ------------------------------------------------------------------ */
/* IOCTL                                                              */
/* ------------------------------------------------------------------ */

static int simplefs_find_by_name(struct simplefs_sb_info *sbi,
				 const char *name, unsigned int *out_idx)
{
	unsigned int i;
	size_t nlen;

	nlen = strnlen(name, sbi->name_max);
	if (nlen == 0 || nlen > sbi->name_max)
		return -EINVAL;

	for (i = 0; i < sbi->file_count; i++) {
		size_t flen = strnlen(sbi->files[i].name, sbi->name_max);

		if (flen == nlen &&
		    !memcmp(sbi->files[i].name, name, nlen)) {
			*out_idx = i;
			return 0;
		}
	}
	return -ENOENT;
}

static void simplefs_drop_file_page_caches(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	unsigned int i;

	for (i = 0; i < sbi->file_count; i++) {
		struct inode *inode = ilookup(sb, (unsigned long)i + 2);

		if (!inode)
			continue;
		invalidate_mapping_pages(inode->i_mapping, 0, -1);
		iput(inode);
	}
}

static long simplefs_ioc_zero_all(struct super_block *sb)
{
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	ret = sync_filesystem(sb);
	if (ret)
		return ret;
	ret = simplefs_zero_all_files(sb);
	if (ret)
		return ret;
	simplefs_drop_file_page_caches(sb);
	invalidate_bdev(sb->s_bdev);
	return 0;
}

static long simplefs_ioc_erase(struct super_block *sb)
{
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	ret = sync_filesystem(sb);
	if (ret)
		return ret;
	invalidate_bdev(sb->s_bdev);
	return simplefs_erase_device(sb);
}

static long simplefs_ioc_get_hashes(struct super_block *sb,
				    void __user *argp)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct simplefs_hash_list req;
	struct simplefs_hash_entry __user *user_entries;
	struct simplefs_hash_entry *ent;
	unsigned int i;
	unsigned int n;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = sync_filesystem(sb);
	if (ret)
		return ret;

	user_entries = (struct simplefs_hash_entry __user *)
			(uintptr_t)req.entries_ptr;
	n = min_t(unsigned int, req.capacity, sbi->file_count);

	if (n) {
		if (!user_entries)
			return -EFAULT;
		ent = kzalloc(sizeof(*ent), GFP_KERNEL);
		if (!ent)
			return -ENOMEM;
		for (i = 0; i < n; i++) {
			u32 crc;

			ret = simplefs_compute_file_crc(sb, i, &crc);
			if (ret) {
				kfree(ent);
				return ret;
			}
			memset(ent, 0, sizeof(*ent));
			strscpy(ent->name, sbi->files[i].name,
				sizeof(ent->name));
			ent->crc32 = crc;
			if (copy_to_user(&user_entries[i], ent,
					 sizeof(*ent))) {
				kfree(ent);
				return -EFAULT;
			}
		}
		kfree(ent);
	}

	req.count = sbi->file_count;
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static long simplefs_ioc_get_mapping(struct super_block *sb,
				     void __user *argp)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct simplefs_mapping req;
	unsigned int idx;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.name[SIMPLEFS_NAME_MAX_CAP - 1] = '\0';

	ret = simplefs_find_by_name(sbi, req.name, &idx);
	if (ret)
		return ret;

	req.start_sector = sbi->files[idx].start;
	req.nsectors     = sbi->files[idx].nsect;
	req._pad         = 0;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

long simplefs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct super_block *sb = file_inode(filp)->i_sb;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case SIMPLEFS_IOC_ZERO_ALL:
		return simplefs_ioc_zero_all(sb);
	case SIMPLEFS_IOC_ERASE:
		return simplefs_ioc_erase(sb);
	case SIMPLEFS_IOC_GET_HASHES:
		return simplefs_ioc_get_hashes(sb, argp);
	case SIMPLEFS_IOC_GET_MAPPING:
		return simplefs_ioc_get_mapping(sb, argp);
	default:
		return -ENOTTY;
	}
}

/* ------------------------------------------------------------------ */
/* file_operations / inode_operations                                 */
/* ------------------------------------------------------------------ */

const struct file_operations simplefs_file_ops = {
	.owner          = THIS_MODULE,
	.llseek         = generic_file_llseek,
	.read_iter      = generic_file_read_iter,
	.write_iter     = generic_file_write_iter,
	.mmap           = generic_file_mmap,
	.fsync          = generic_file_fsync,
	.splice_read    = filemap_splice_read,
	.splice_write   = iter_file_splice_write,
	.unlocked_ioctl = simplefs_ioctl,
	.compat_ioctl   = simplefs_ioctl,
};

const struct inode_operations simplefs_file_inode_ops = {
	.getattr = simple_getattr,
};
