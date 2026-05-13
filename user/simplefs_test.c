// SPDX-License-Identifier: GPL-2.0
/*
 * simplefs_test: walk every file in a SimpleFS mount, write a random
 * u64 to it, read it back and check that the value matches.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int rand_u64(uint64_t *out)
{
	ssize_t n = getrandom(out, sizeof(*out), 0);

	if (n != sizeof(*out)) {
		fprintf(stderr, "getrandom: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int test_one(const char *path)
{
	uint64_t want = 0, got = 0;
	int fd, rc = -1;

	if (rand_u64(&want))
		return -1;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		return -1;
	}

	if (pwrite(fd, &want, sizeof(want), 0) != (ssize_t)sizeof(want)) {
		fprintf(stderr, "write(%s): %s\n", path, strerror(errno));
		goto out;
	}
	if (fsync(fd) < 0) {
		fprintf(stderr, "fsync(%s): %s\n", path, strerror(errno));
		goto out;
	}
	if (pread(fd, &got, sizeof(got), 0) != (ssize_t)sizeof(got)) {
		fprintf(stderr, "read(%s): %s\n", path, strerror(errno));
		goto out;
	}
	if (got != want) {
		fprintf(stderr,
			"mismatch on %s: wrote 0x%016" PRIx64
			" read 0x%016" PRIx64 "\n",
			path, want, got);
		goto out;
	}
	printf("  %-40s 0x%016" PRIx64 "  OK\n", path, want);
	rc = 0;
out:
	close(fd);
	return rc;
}

static int collect_and_run(const char *mount_dir)
{
	DIR *d = opendir(mount_dir);
	struct dirent *de;
	char path[PATH_MAX];
	unsigned int ok = 0, fail = 0;

	if (!d) {
		fprintf(stderr, "opendir(%s): %s\n", mount_dir,
			strerror(errno));
		return 1;
	}

	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (de->d_type != DT_REG && de->d_type != DT_UNKNOWN)
			continue;
		snprintf(path, sizeof(path), "%s/%s", mount_dir, de->d_name);
		if (test_one(path) == 0)
			ok++;
		else
			fail++;
	}
	closedir(d);

	printf("\nsummary: %u passed, %u failed\n", ok, fail);
	return fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <mount_dir>\n", argv[0]);
		return 2;
	}
	return collect_and_run(argv[1]);
}
