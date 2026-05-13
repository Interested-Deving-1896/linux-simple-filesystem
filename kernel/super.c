// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include "simplefs.h"
#include "simplefs_internal.h"

/* ------------------------------------------------------------------ */
/* Module parameters                                                  */
/* ------------------------------------------------------------------ */

static char *device_path;
module_param_named(device, device_path, charp, 0444);
MODULE_PARM_DESC(device,
		 "Backing block device path used when mount source is empty");

static unsigned int param_sb_first;
module_param_named(sb_first, param_sb_first, uint, 0444);
MODULE_PARM_DESC(sb_first, "Primary superblock sector offset (default 0)");

static unsigned int param_sb_second = 8;
module_param_named(sb_second, param_sb_second, uint, 0444);
MODULE_PARM_DESC(sb_second,
		 "Secondary superblock sector offset (must differ from sb_first)");

static unsigned int param_name_max = 32;
module_param_named(name_max, param_name_max, uint, 0444);
MODULE_PARM_DESC(name_max, "Maximum file name length (default 32, max 255)");

static unsigned int param_file_size_sectors = 4;
module_param_named(file_size_sectors, param_file_size_sectors, uint, 0444);
MODULE_PARM_DESC(file_size_sectors,
		 "File size in sectors, the spec's M (default 4, max 1024)");

/* ------------------------------------------------------------------ */
/* Superblock helpers                                                 */
/* ------------------------------------------------------------------ */

u32 simplefs_compute_crc(const struct simplefs_sb_disk *sb)
{
	return crc32_le(0, (const u8 *)sb,
			offsetof(struct simplefs_sb_disk, checksum));
}

bool simplefs_validate_sb(const struct simplefs_sb_disk *sb)
{
	if (le32_to_cpu(sb->magic) != SIMPLEFS_MAGIC)
		return false;
	if (le32_to_cpu(sb->version) != SIMPLEFS_VERSION)
		return false;
	if (le32_to_cpu(sb->sector_size) != SIMPLEFS_SECTOR)
		return false;
	if (le32_to_cpu(sb->checksum) != simplefs_compute_crc(sb))
		return false;
	return true;
}

static void simplefs_fill_disk_sb(struct simplefs_sb_disk *disk_sb,
				  const struct simplefs_sb_info *sbi)
{
	memset(disk_sb, 0, SIMPLEFS_SECTOR);
	disk_sb->magic             = cpu_to_le32(SIMPLEFS_MAGIC);
	disk_sb->version           = cpu_to_le32(SIMPLEFS_VERSION);
	disk_sb->sector_size       = cpu_to_le32(SIMPLEFS_SECTOR);
	disk_sb->sb_first          = cpu_to_le32(sbi->sb_first);
	disk_sb->sb_second         = cpu_to_le32(sbi->sb_second);
	disk_sb->file_size_sectors = cpu_to_le32(sbi->file_size_sectors);
	disk_sb->name_max          = cpu_to_le32(sbi->name_max);
	disk_sb->file_count        = cpu_to_le32(sbi->file_count);
	disk_sb->total_sectors     = cpu_to_le64(sbi->total_sectors);
	disk_sb->checksum          = cpu_to_le32(simplefs_compute_crc(disk_sb));
}

static int simplefs_write_sector(struct super_block *sb, sector_t sector,
				 const void *data)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh) {
		pr_err("failed to read sector %llu for write\n",
		       (unsigned long long)sector);
		return -EIO;
	}
	lock_buffer(bh);
	memcpy(bh->b_data, data, SIMPLEFS_SECTOR);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

int simplefs_write_superblocks(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct simplefs_sb_disk *disk_sb;
	int ret;

	disk_sb = kzalloc(SIMPLEFS_SECTOR, GFP_KERNEL);
	if (!disk_sb)
		return -ENOMEM;

	simplefs_fill_disk_sb(disk_sb, sbi);

	ret = simplefs_write_sector(sb, sbi->sb_first, disk_sb);
	if (ret)
		goto out;
	ret = simplefs_write_sector(sb, sbi->sb_second, disk_sb);
out:
	kfree(disk_sb);
	return ret;
}

int simplefs_zero_all_files(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	void *zero;
	unsigned int i;
	u32 j;
	int ret = 0;

	zero = kzalloc(SIMPLEFS_SECTOR, GFP_KERNEL);
	if (!zero)
		return -ENOMEM;

	for (i = 0; i < sbi->file_count; i++) {
		for (j = 0; j < sbi->files[i].nsect; j++) {
			ret = simplefs_write_sector(sb,
				sbi->files[i].start + j, zero);
			if (ret)
				goto out;
		}
	}
out:
	kfree(zero);
	return ret;
}

int simplefs_erase_device(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	void *zero;
	int ret;

	zero = kzalloc(SIMPLEFS_SECTOR, GFP_KERNEL);
	if (!zero)
		return -ENOMEM;

	ret = simplefs_zero_all_files(sb);
	if (ret)
		goto out;
	ret = simplefs_write_sector(sb, sbi->sb_first, zero);
	if (ret)
		goto out;
	ret = simplefs_write_sector(sb, sbi->sb_second, zero);
out:
	kfree(zero);
	return ret;
}

int simplefs_compute_file_crc(struct super_block *sb, unsigned int idx,
			      u32 *out)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;
	struct inode *inode;
	loff_t pos = 0, total;
	u32 crc = 0;
	int ret = 0;
	bool put_inode = true;

	if (idx >= sbi->file_count)
		return -EINVAL;

	inode = ilookup(sb, (unsigned long)idx + 2);
	if (!inode) {
		inode = simplefs_make_file_inode(sb, idx);
		if (IS_ERR(inode))
			return PTR_ERR(inode);
	}

	total = i_size_read(inode);

	while (pos < total) {
		pgoff_t pg_idx = pos >> PAGE_SHIFT;
		size_t off = pos & (PAGE_SIZE - 1);
		size_t avail = min_t(size_t, PAGE_SIZE - off, total - pos);
		struct page *page;
		void *kbuf;

		page = read_mapping_page(inode->i_mapping, pg_idx, NULL);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			goto out;
		}
		kbuf = kmap_local_page(page);
		crc = crc32_le(crc, (const u8 *)kbuf + off, avail);
		kunmap_local(kbuf);
		put_page(page);
		pos += avail;
	}
	*out = crc;
out:
	if (put_inode)
		iput(inode);
	return ret;
}

/* ------------------------------------------------------------------ */
/* File layout builder                                                */
/* ------------------------------------------------------------------ */

static int simplefs_build_file_layout(struct simplefs_sb_info *sbi)
{
	sector_t total = sbi->total_sectors;
	u32 M = sbi->file_size_sectors;
	u32 max_files;
	sector_t s = 0;
	unsigned int idx = 0;
	unsigned int name_len = sbi->name_max;

	if (M == 0)
		return -EINVAL;
	max_files = (u32)div_u64(total, M) + 2;

	sbi->files = kvmalloc_array(max_files, sizeof(*sbi->files),
				    GFP_KERNEL | __GFP_ZERO);
	if (!sbi->files)
		return -ENOMEM;

	while (s + M <= total) {
		sector_t end = s + M;

		if ((sector_t)sbi->sb_first >= s &&
		    (sector_t)sbi->sb_first < end) {
			s = (sector_t)sbi->sb_first + 1;
			continue;
		}
		if ((sector_t)sbi->sb_second >= s &&
		    (sector_t)sbi->sb_second < end) {
			s = (sector_t)sbi->sb_second + 1;
			continue;
		}

		sbi->files[idx].start = s;
		sbi->files[idx].nsect = M;
		snprintf(sbi->files[idx].name, name_len + 1,
			 "file_%04u", idx);
		idx++;
		s += M;
	}

	sbi->file_count = idx;
	if (idx == 0) {
		kvfree(sbi->files);
		sbi->files = NULL;
		pr_err("device too small to host any file slot\n");
		return -ENOSPC;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* fill_super                                                         */
/* ------------------------------------------------------------------ */

static int simplefs_check_params(void)
{
	if (param_sb_first == param_sb_second) {
		pr_err("sb_first (%u) and sb_second (%u) must differ\n",
		       param_sb_first, param_sb_second);
		return -EINVAL;
	}
	if (param_name_max < SIMPLEFS_NAME_MIN_CAP ||
	    param_name_max >= SIMPLEFS_NAME_MAX_CAP) {
		pr_err("name_max=%u out of range [%u,%u)\n", param_name_max,
		       SIMPLEFS_NAME_MIN_CAP, SIMPLEFS_NAME_MAX_CAP);
		return -EINVAL;
	}
	if (param_file_size_sectors < 1 ||
	    param_file_size_sectors > SIMPLEFS_M_MAX) {
		pr_err("file_size_sectors=%u out of range [1,%u]\n",
		       param_file_size_sectors, SIMPLEFS_M_MAX);
		return -EINVAL;
	}
	return 0;
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct simplefs_sb_info *sbi;
	struct buffer_head *bh;
	struct simplefs_sb_disk *disk_sb;
	bool sb1_ok = false, sb2_ok = false;
	sector_t total_sectors;
	struct inode *root;
	int ret;

	ret = simplefs_check_params();
	if (ret)
		return ret;

	if (!sb_set_blocksize(sb, SIMPLEFS_SECTOR)) {
		pr_err("failed to set block size to %u\n", SIMPLEFS_SECTOR);
		return -EINVAL;
	}

	sb->s_magic     = SIMPLEFS_MAGIC;
	sb->s_op        = &simplefs_super_ops;
	sb->s_maxbytes  = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;

	total_sectors = bdev_nr_sectors(sb->s_bdev);
	if (total_sectors < 4) {
		pr_err("device too small (%llu sectors)\n",
		       (unsigned long long)total_sectors);
		return -EINVAL;
	}
	if (param_sb_first >= total_sectors ||
	    param_sb_second >= total_sectors) {
		pr_err("superblock offset beyond device size (%llu sectors)\n",
		       (unsigned long long)total_sectors);
		return -EINVAL;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sbi->sb_first          = param_sb_first;
	sbi->sb_second         = param_sb_second;
	sbi->name_max          = param_name_max;
	sbi->file_size_sectors = param_file_size_sectors;
	sbi->total_sectors     = total_sectors;
	sb->s_fs_info          = sbi;

	bh = sb_bread(sb, sbi->sb_first);
	if (bh) {
		disk_sb = (struct simplefs_sb_disk *)bh->b_data;
		sb1_ok  = simplefs_validate_sb(disk_sb);
		brelse(bh);
	}
	bh = sb_bread(sb, sbi->sb_second);
	if (bh) {
		disk_sb = (struct simplefs_sb_disk *)bh->b_data;
		sb2_ok  = simplefs_validate_sb(disk_sb);
		brelse(bh);
	}

	if (!sb1_ok && !sb2_ok) {
		pr_info("no valid superblock at sectors %u/%u; formatting\n",
			sbi->sb_first, sbi->sb_second);
		ret = simplefs_build_file_layout(sbi);
		if (ret)
			goto out_free_sbi;
		ret = simplefs_write_superblocks(sb);
		if (ret)
			goto out_free_files;
	} else {
		sector_t valid_sec = sb1_ok ? sbi->sb_first : sbi->sb_second;

		bh = sb_bread(sb, valid_sec);
		if (!bh) {
			ret = -EIO;
			goto out_free_sbi;
		}
		disk_sb = (struct simplefs_sb_disk *)bh->b_data;

		if (le32_to_cpu(disk_sb->sb_first) != sbi->sb_first ||
		    le32_to_cpu(disk_sb->sb_second) != sbi->sb_second ||
		    le32_to_cpu(disk_sb->file_size_sectors) !=
				sbi->file_size_sectors ||
		    le32_to_cpu(disk_sb->name_max) != sbi->name_max) {
			pr_err("on-disk superblock does not match module params (sb_first=%u/%u sb_second=%u/%u M=%u/%u name_max=%u/%u)\n",
			       le32_to_cpu(disk_sb->sb_first), sbi->sb_first,
			       le32_to_cpu(disk_sb->sb_second), sbi->sb_second,
			       le32_to_cpu(disk_sb->file_size_sectors),
			       sbi->file_size_sectors,
			       le32_to_cpu(disk_sb->name_max), sbi->name_max);
			brelse(bh);
			ret = -EINVAL;
			goto out_free_sbi;
		}
		brelse(bh);

		ret = simplefs_build_file_layout(sbi);
		if (ret)
			goto out_free_sbi;

		if (!sb1_ok || !sb2_ok) {
			pr_warn("superblock copy %s is corrupted; repairing\n",
				sb1_ok ? "secondary" : "primary");
			ret = simplefs_write_superblocks(sb);
			if (ret)
				goto out_free_files;
		}
	}

	root = simplefs_make_root_inode(sb);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto out_free_files;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto out_free_files;
	}

	pr_info("mounted: %u files of %u sectors each (sb_first=%u sb_second=%u name_max=%u total=%llu)\n",
		sbi->file_count, sbi->file_size_sectors, sbi->sb_first,
		sbi->sb_second, sbi->name_max,
		(unsigned long long)sbi->total_sectors);
	return 0;

out_free_files:
	kvfree(sbi->files);
out_free_sbi:
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

/* ------------------------------------------------------------------ */
/* super ops                                                          */
/* ------------------------------------------------------------------ */

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct simplefs_sb_info *sbi = sb->s_fs_info;

	buf->f_type    = SIMPLEFS_MAGIC;
	buf->f_bsize   = SIMPLEFS_SECTOR;
	buf->f_blocks  = sbi->total_sectors;
	buf->f_bfree   = 0;
	buf->f_bavail  = 0;
	buf->f_files   = sbi->file_count;
	buf->f_ffree   = 0;
	buf->f_namelen = sbi->name_max;
	return 0;
}

static void simplefs_put_super(struct super_block *sb)
{
	struct simplefs_sb_info *sbi = sb->s_fs_info;

	if (sbi) {
		kvfree(sbi->files);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
}

const struct super_operations simplefs_super_ops = {
	.statfs      = simplefs_statfs,
	.put_super   = simplefs_put_super,
	.drop_inode  = generic_delete_inode,
};

/* ------------------------------------------------------------------ */
/* Mount / unmount                                                    */
/* ------------------------------------------------------------------ */

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
				     int flags, const char *dev_name,
				     void *data)
{
	const char *dev = dev_name;

	if ((!dev || !dev[0] || !strcmp(dev, "none")) && device_path)
		dev = device_path;
	if (!dev || !dev[0]) {
		pr_err("no device: pass it via mount source or device= module param\n");
		return ERR_PTR(-EINVAL);
	}
	return mount_bdev(fs_type, flags, dev, data, simplefs_fill_super);
}

static struct file_system_type simplefs_type = {
	.owner    = THIS_MODULE,
	.name     = "simplefs",
	.mount    = simplefs_mount,
	.kill_sb  = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void)
{
	int ret;

	ret = register_filesystem(&simplefs_type);
	if (ret) {
		pr_err("register_filesystem failed: %d\n", ret);
		return ret;
	}
	pr_info("registered\n");
	return 0;
}

static void __exit simplefs_exit(void)
{
	unregister_filesystem(&simplefs_type);
	pr_info("unregistered\n");
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SimpleFS");
MODULE_DESCRIPTION("SimpleFS: flat filesystem with two checksummed superblocks");
