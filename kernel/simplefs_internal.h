/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SIMPLEFS_INTERNAL_H
#define _SIMPLEFS_INTERNAL_H

#include <linux/fs.h>
#include <linux/types.h>

#include "simplefs.h"

struct simplefs_file_info {
	char     name[SIMPLEFS_NAME_MAX_CAP];
	sector_t start;
	u32      nsect;
};

struct simplefs_sb_info {
	u32 sb_first;
	u32 sb_second;
	u32 file_size_sectors;
	u32 name_max;
	u32 file_count;
	u64 total_sectors;
	struct simplefs_file_info *files;	/* file_count entries */
};

/* super.c */
u32  simplefs_compute_crc(const struct simplefs_sb_disk *sb);
bool simplefs_validate_sb(const struct simplefs_sb_disk *sb);
int  simplefs_write_superblocks(struct super_block *sb);
int  simplefs_zero_all_files(struct super_block *sb);
int  simplefs_erase_device(struct super_block *sb);
int  simplefs_compute_file_crc(struct super_block *sb, unsigned int idx,
			       u32 *out);

/* inode.c */
extern const struct super_operations  simplefs_super_ops;
extern const struct inode_operations  simplefs_dir_inode_ops;
extern const struct file_operations   simplefs_dir_ops;
struct inode *simplefs_make_root_inode(struct super_block *sb);
struct inode *simplefs_make_file_inode(struct super_block *sb,
				       unsigned int idx);

/* file.c */
extern const struct file_operations         simplefs_file_ops;
extern const struct inode_operations        simplefs_file_inode_ops;
extern const struct address_space_operations simplefs_aops;
long simplefs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

#endif /* _SIMPLEFS_INTERNAL_H */
