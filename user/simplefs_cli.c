// SPDX-License-Identifier: GPL-2.0
/*
 * simplefs_cli: command-line driver for SimpleFS IOCTLs.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../kernel/simplefs.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s <path> zero-all\n"
		"  %s <path> erase\n"
		"  %s <path> hashes\n"
		"  %s <path> mapping <name>\n"
		"\n"
		"  <path> is the mount point (e.g. /mnt/simplefs) or any file\n"
		"  inside the mount. The ioctls operate on the whole FS.\n",
		prog, prog, prog, prog);
}

static int open_target(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);

	if (fd >= 0)
		return fd;
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		return -1;
	}
	return fd;
}

static int cmd_zero_all(int fd)
{
	if (ioctl(fd, SIMPLEFS_IOC_ZERO_ALL) < 0) {
		fprintf(stderr, "ioctl ZERO_ALL: %s\n", strerror(errno));
		return 1;
	}
	puts("zeroed all files");
	return 0;
}

static int cmd_erase(int fd)
{
	if (ioctl(fd, SIMPLEFS_IOC_ERASE) < 0) {
		fprintf(stderr, "ioctl ERASE: %s\n", strerror(errno));
		return 1;
	}
	puts("erased filesystem (superblocks invalidated)");
	return 0;
}

static int cmd_hashes(int fd)
{
	struct simplefs_hash_list probe = { 0 };
	struct simplefs_hash_list req;
	struct simplefs_hash_entry *entries = NULL;
	unsigned int i;
	int rc = 1;

	if (ioctl(fd, SIMPLEFS_IOC_GET_HASHES, &probe) < 0) {
		fprintf(stderr, "ioctl GET_HASHES (probe): %s\n",
			strerror(errno));
		return 1;
	}
	if (probe.count == 0) {
		puts("(no files)");
		return 0;
	}

	entries = calloc(probe.count, sizeof(*entries));
	if (!entries) {
		perror("calloc");
		return 1;
	}

	memset(&req, 0, sizeof(req));
	req.capacity    = probe.count;
	req.entries_ptr = (uint64_t)(uintptr_t)entries;

	if (ioctl(fd, SIMPLEFS_IOC_GET_HASHES, &req) < 0) {
		fprintf(stderr, "ioctl GET_HASHES: %s\n", strerror(errno));
		goto out;
	}

	printf("file_count=%u\n", req.count);
	for (i = 0; i < req.count; i++) {
		entries[i].name[SIMPLEFS_NAME_MAX_CAP - 1] = '\0';
		printf("  %-32s crc32=0x%08x\n",
		       entries[i].name, entries[i].crc32);
	}
	rc = 0;
out:
	free(entries);
	return rc;
}

static int cmd_mapping(int fd, const char *name)
{
	struct simplefs_mapping req;

	memset(&req, 0, sizeof(req));
	strncpy(req.name, name, SIMPLEFS_NAME_MAX_CAP - 1);

	if (ioctl(fd, SIMPLEFS_IOC_GET_MAPPING, &req) < 0) {
		fprintf(stderr, "ioctl GET_MAPPING: %s\n", strerror(errno));
		return 1;
	}
	printf("name=%s start_sector=%llu nsectors=%u (size=%llu bytes)\n",
	       name,
	       (unsigned long long)req.start_sector,
	       req.nsectors,
	       (unsigned long long)req.nsectors * SIMPLEFS_SECTOR);
	return 0;
}

int main(int argc, char **argv)
{
	int fd, rc;
	const char *path, *cmd;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}
	path = argv[1];
	cmd  = argv[2];

	fd = open_target(path);
	if (fd < 0)
		return 1;

	if (!strcmp(cmd, "zero-all")) {
		rc = cmd_zero_all(fd);
	} else if (!strcmp(cmd, "erase")) {
		rc = cmd_erase(fd);
	} else if (!strcmp(cmd, "hashes")) {
		rc = cmd_hashes(fd);
	} else if (!strcmp(cmd, "mapping")) {
		if (argc < 4) {
			usage(argv[0]);
			rc = 2;
		} else {
			rc = cmd_mapping(fd, argv[3]);
		}
	} else {
		usage(argv[0]);
		rc = 2;
	}

	close(fd);
	return rc;
}
