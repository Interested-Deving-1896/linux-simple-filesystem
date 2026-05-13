/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SIMPLEFS_H
#define _SIMPLEFS_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef uint32_t __le32;
typedef uint64_t __le64;
#endif

#define SIMPLEFS_MAGIC           0x53465353u  /* 'SFSS' */
#define SIMPLEFS_VERSION         1u
#define SIMPLEFS_SECTOR          512u
#define SIMPLEFS_NAME_MAX_CAP    256u
#define SIMPLEFS_NAME_MIN_CAP    8u
#define SIMPLEFS_M_MAX           1024u

struct simplefs_sb_disk {
	__le32 magic;
	__le32 version;
	__le32 sector_size;
	__le32 sb_first;
	__le32 sb_second;
	__le32 file_size_sectors;	/* M */
	__le32 name_max;
	__le32 file_count;
	__le64 total_sectors;
	__u8   reserved[SIMPLEFS_SECTOR - (9 * 4) - 8 - 4];
	__le32 checksum;		/* CRC32 over preceding bytes */
};

struct simplefs_hash_entry {
	char  name[SIMPLEFS_NAME_MAX_CAP];
	__u32 crc32;
	__u32 _pad;
};

struct simplefs_hash_list {
	__u32 capacity;		/* in:  number of entries 'entries' can hold */
	__u32 count;		/* out: total entries available (may exceed cap) */
	__u64 entries_ptr;	/* in:  user pointer to simplefs_hash_entry[] */
};

struct simplefs_mapping {
	char  name[SIMPLEFS_NAME_MAX_CAP];	/* in  */
	__u64 start_sector;			/* out */
	__u32 nsectors;				/* out */
	__u32 _pad;
};

#define SIMPLEFS_IOC_MAGIC	'S'
#define SIMPLEFS_IOC_ZERO_ALL	_IO(SIMPLEFS_IOC_MAGIC, 1)
#define SIMPLEFS_IOC_ERASE	_IO(SIMPLEFS_IOC_MAGIC, 2)
#define SIMPLEFS_IOC_GET_HASHES	_IOWR(SIMPLEFS_IOC_MAGIC, 3, struct simplefs_hash_list)
#define SIMPLEFS_IOC_GET_MAPPING _IOWR(SIMPLEFS_IOC_MAGIC, 4, struct simplefs_mapping)

#endif /* _SIMPLEFS_H */
