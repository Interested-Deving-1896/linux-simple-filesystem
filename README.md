# SimpleFS

SimpleFS is a Linux loadable filesystem module that exposes a raw block
device as a flat directory of fixed-size files. It maintains two
CRC32-protected copies of its superblock, places files deterministically
on disk and supports four custom IOCTLs. Only `read` and `write` are
implemented; no file creation, renaming, removal or attribute change is
exposed to userspace.

The repository contains:

| Path                                   | Contents                                                          |
| -------------------------------------- | ----------------------------------------------------------------- |
| [`kernel/`](kernel/)                   | Kernel module sources and `Makefile`                              |
| [`kernel/simplefs.h`](kernel/simplefs.h)            | On-disk structures and IOCTL definitions shared with userspace |
| [`kernel/super.c`](kernel/super.c)     | Module init, `mount_bdev`, `fill_super`, CRC, format helpers      |
| [`kernel/inode.c`](kernel/inode.c)     | Root directory and per-file inodes, `readdir`/`lookup`            |
| [`kernel/file.c`](kernel/file.c)       | `address_space_operations`, `file_operations`, IOCTL dispatcher   |
| [`user/`](user/)                       | Userspace utilities and `Makefile`                                |
| [`user/simplefs_cli.c`](user/simplefs_cli.c) | Command-line driver for the four IOCTLs                     |
| [`user/simplefs_test.c`](user/simplefs_test.c) | Demonstration program: random write + read-back per file  |
| [`scripts/verify.sh`](scripts/verify.sh) | End-to-end verification script (17 checks)                      |
| [`Makefile`](Makefile)                 | Top-level build entry point                                       |

The target kernel is **Linux 6.12.x**. The source also compiles on 6.8
through a `LINUX_VERSION_CODE`-conditional on the `block_write_begin`
signature; no other compatibility shims are required.

---

## On-disk layout

```
sector sb_first         primary superblock          (512-byte sector, CRC32-protected)
   ...   file_0000  ...     M sectors
   ...   file_0001  ...     M sectors
   ...                      ...
sector sb_second        secondary superblock        (byte-identical copy)
   ...   more files    ...
   ...                      (tail is unused if not a multiple of M)
```

`sb_first` and `sb_second` are module parameters. File names, offsets and
sizes are computed deterministically at mount time:

* names are `file_NNNN`, indexed in order;
* every file occupies exactly `file_size_sectors` (the spec's `M`) sectors;
* a candidate slot that would overlap one of the superblock sectors is
  skipped, so the placement automatically jumps over both copies.

Because metadata is fully deterministic, there is **no on-disk inode
table** — the two superblocks are the only on-disk metadata.

---

## Module parameters

| Name                | Type   | Default | Constraints       | Purpose                                                        |
| ------------------- | ------ | ------- | ----------------- | -------------------------------------------------------------- |
| `device`            | charp  | —       | —                 | Backing device path; used when the `mount` source is empty.    |
| `sb_first`          | uint   | 0       | < device size     | Sector of the primary superblock.                              |
| `sb_second`         | uint   | 8       | ≠ `sb_first`      | Sector of the secondary superblock.                            |
| `name_max`          | uint   | 32      | 8 ≤ value < 256   | Maximum file name length stored in the superblock.             |
| `file_size_sectors` | uint   | 4       | 1 ≤ value ≤ 1024  | File size in 512-byte sectors (the spec's `M`).                |

---

## Build

Prerequisites: a recent GCC and the kernel headers for the running
kernel (`linux-headers-$(uname -r)` on Debian / Ubuntu).

```bash
$ cd SimpleFS
$ make
```

Captured output:

```
  CC [M]  .../SimpleFS/kernel/super.o
  CC [M]  .../SimpleFS/kernel/inode.o
  CC [M]  .../SimpleFS/kernel/file.o
  LD [M]  .../SimpleFS/kernel/simplefs.o
  MODPOST .../SimpleFS/kernel/Module.symvers
  CC [M]  .../SimpleFS/kernel/simplefs.mod.o
  LD [M]  .../SimpleFS/kernel/simplefs.ko
cc -O2 -Wall -Wextra -Wpedantic -std=gnu11 -o simplefs_cli simplefs_cli.c
cc -O2 -Wall -Wextra -Wpedantic -std=gnu11 -o simplefs_test simplefs_test.c
```

`make clean` removes all build artefacts from both sub-trees.

### Inspect the resulting module

```bash
$ modinfo kernel/simplefs.ko | egrep '^(description|license|parm):'
```

```
description:    SimpleFS: flat filesystem with two checksummed superblocks
license:        GPL
parm:           device:Backing block device path used when mount source is empty (charp)
parm:           sb_first:Primary superblock sector offset (default 0) (uint)
parm:           sb_second:Secondary superblock sector offset (must differ from sb_first) (uint)
parm:           name_max:Maximum file name length (default 32, max 255) (uint)
parm:           file_size_sectors:File size in sectors, the spec's M (default 4, max 1024) (uint)
```

---

## Bringing up an instance on a loop device

The full bring-up sequence uses a temporary backing image and an
auto-assigned loop device. Run every step as `root` (or via `sudo`).

### 1. Prepare a backing block device

```bash
$ truncate -s 64M /tmp/simplefs.img
$ LOOP=$(sudo losetup -f --show /tmp/simplefs.img)
$ echo "LOOP=$LOOP"
LOOP=/dev/loop6
$ sudo blockdev --getsz "$LOOP"
131072
```

### 2. Load the module

```bash
$ sudo insmod kernel/simplefs.ko \
       device="$LOOP" sb_first=0 sb_second=8 \
       name_max=32 file_size_sectors=4
$ lsmod | grep '^simplefs'
simplefs               28672  0
```

### 3. Mount

```bash
$ sudo mkdir -p /mnt/simplefs
$ sudo mount -t simplefs "$LOOP" /mnt/simplefs
$ sudo dmesg | tail -2
simplefs: registered
simplefs: mounted: 32766 files of 4 sectors each (sb_first=0 sb_second=8 name_max=32 total=131072)
```

### 4. Inspect the mount

```bash
$ ls /mnt/simplefs | head -5
file_0000
file_0001
file_0002
file_0003
file_0004

$ ls /mnt/simplefs | wc -l
32766

$ stat -c '%n -> size=%s, blocks=%b' /mnt/simplefs/file_0000
/mnt/simplefs/file_0000 -> size=2048, blocks=0
```

The number of files is `floor(total_sectors / M)` minus the slots that
would overlap one of the superblocks. With a 64 MiB image and `M = 4`,
this yields 32 766 files of 2 KiB each.

---

## Demonstration program: `simplefs_test`

`user/simplefs_test` walks the mounted filesystem and, for every file,
writes a fresh `uint64_t` from `getrandom(2)` at offset 0, calls `fsync`,
reads the value back and asserts equality. The program implements the
"per-file random write + read-back" requirement of the assignment.

```bash
$ sudo ./user/simplefs_test /mnt/simplefs | tail -5
```

```
  /mnt/simplefs/file_32763                 0xa8fa902f0dc39046  OK
  /mnt/simplefs/file_32764                 0xff3d9c8adbb84af3  OK
  /mnt/simplefs/file_32765                 0xd2b4d53065b751a1  OK

summary: 32766 passed, 0 failed
```

Exit status is `0` on full success, `1` on any failure.

---

## IOCTL driver: `simplefs_cli`

`user/simplefs_cli` is a thin command-line wrapper around the four
IOCTLs defined in [`kernel/simplefs.h`](kernel/simplefs.h). General usage:

```
simplefs_cli <path> <command> [args]
```

`<path>` is either the mount point or any file inside the mount; every
command operates on the filesystem as a whole. Destructive commands
(`zero-all`, `erase`) require `CAP_SYS_ADMIN`.

### `mapping <name>`

Returns the absolute sector range occupied by the named file.

```bash
$ sudo ./user/simplefs_cli /mnt/simplefs mapping file_0000
name=file_0000 start_sector=1 nsectors=4 (size=2048 bytes)
$ sudo ./user/simplefs_cli /mnt/simplefs mapping file_0001
name=file_0001 start_sector=9 nsectors=4 (size=2048 bytes)
$ sudo ./user/simplefs_cli /mnt/simplefs mapping file_0002
name=file_0002 start_sector=13 nsectors=4 (size=2048 bytes)
```

`file_0001` starts at sector 9 because the candidate slot `[5, 9)` would
overlap `sb_second = 8`; the layout skips the superblock sector and
continues at sector 9.

```bash
$ sudo ./user/simplefs_cli /mnt/simplefs mapping nope
ioctl GET_MAPPING: No such file or directory
```

### `hashes`

Returns one CRC32 per file, read through the file's address space so the
result is coherent with concurrent `read`/`write`.

```bash
$ sudo ./user/simplefs_cli /mnt/simplefs hashes | head -5
file_count=32766
  file_0000                        crc32=0x157eb0d7
  file_0001                        crc32=0x4d54ce82
  file_0002                        crc32=0x7e68aa6e
  file_0003                        crc32=0x28062a38
```

After `simplefs_test` every file holds independent random data, so all
hashes differ. After `zero-all` they collapse to the CRC32 of a buffer
of `M * 512` zero bytes — which for the canonical CRC32 of all-zero
input is itself `0x00000000`:

```bash
$ sudo ./user/simplefs_cli /mnt/simplefs hashes | head -3
file_count=32766
  file_0000                        crc32=0x00000000
  file_0001                        crc32=0x00000000
```

### `zero-all`

Overwrites every file's data with zeros and invalidates the affected
inode page caches so the change is immediately visible through normal
`read` calls. The two superblocks are not touched.

```bash
$ sudo ./user/simplefs_cli /mnt/simplefs zero-all
zeroed all files
$ sudo head -c 16 /mnt/simplefs/file_0000 | xxd
00000000: 0000 0000 0000 0000 0000 0000 0000 0000  ................
```

### `erase`

Zeros every file and invalidates both copies of the superblock. The
filesystem stays mounted (the in-memory layout is unaffected) but the
next `mount` will trigger a fresh format.

```bash
$ sudo ./user/simplefs_cli /mnt/simplefs erase
erased filesystem (superblocks invalidated)
$ sudo umount /mnt/simplefs
$ sudo mount -t simplefs "$LOOP" /mnt/simplefs
$ sudo dmesg | tail -2
simplefs: no valid superblock at sectors 0/8; formatting
simplefs: mounted: 32766 files of 4 sectors each (sb_first=0 sb_second=8 name_max=32 total=131072)
```

---

## Regular `read` and `write`

Files behave as ordinary regular files; standard utilities work as
expected. Writes need `sync` (or `fsync` on the file descriptor) before
the data is observable on the underlying block device.

```bash
$ echo "Hello SimpleFS" | sudo tee /mnt/simplefs/file_0000 > /dev/null
$ sync
$ sudo cat /mnt/simplefs/file_0000
Hello SimpleFS
```

Operations that mutate the directory structure or file attributes are
deliberately unimplemented per the assignment: `touch /mnt/simplefs/new`,
`mv`, `rm`, `chmod`, `chown` and similar will fail with `ENOSYS` or
`EPERM`.

---

## Superblock recovery

When the integrity check on one copy fails, `mount` repairs it from the
surviving copy and continues:

```bash
$ sudo umount /mnt/simplefs
$ sudo dd if=/dev/zero of="$LOOP" bs=512 count=1 seek=0 conv=notrunc status=none
$ echo "primary magic before mount: $(sudo xxd -l 4 -s 0 $LOOP | awk '{print $2$3}')"
primary magic before mount: 00000000
$ echo "secondary magic before mount: $(sudo xxd -l 4 -s 4096 $LOOP | awk '{print $2$3}')"
secondary magic before mount: 53534653
$ sudo mount -t simplefs "$LOOP" /mnt/simplefs
$ sudo dmesg | tail -2
simplefs: superblock copy primary is corrupted; repairing
simplefs: mounted: 32766 files of 4 sectors each (sb_first=0 sb_second=8 name_max=32 total=131072)
$ echo "primary magic after mount: $(sudo xxd -l 4 -s 0 $LOOP | awk '{print $2$3}')"
primary magic after mount: 53534653
```

If both copies are unreadable, the filesystem is reformatted with the
current module parameters. If a copy is valid but its parameters
disagree with the module parameters, `mount` refuses to proceed and
logs the mismatch:

```bash
$ sudo dmesg | tail -1
simplefs: on-disk superblock does not match module params (...)
```

---

## End-to-end verification

The bundled script [`scripts/verify.sh`](scripts/verify.sh) builds the
module, sets up an isolated loop device, exercises every code path
(auto-format, IOCTL trio, superblock corruption + repair from each
copy, parameter validation), tears everything down and prints a
per-step pass/fail summary.

```bash
$ cd SimpleFS
$ sudo bash scripts/verify.sh 2>&1 | tee /tmp/simplefs_verify.log | tail -5
================================================================
SUMMARY: 17 passed, 0 failed
ALL CHECKS PASSED

=== CLEANUP
```

The full log of a representative run is roughly 100 lines and covers
the following 17 checks: build, `modinfo`, loop device, `insmod`,
`mount` + auto-format, file size, `simplefs_test`, `mapping`,
`mapping` for a missing name, `hashes` count, `zero-all`, post-zero
hash collision, primary SB repair, secondary SB repair, `erase`,
`sb_first == sb_second` rejection, parameter-mismatch rejection.

---

## Tear-down

```bash
$ sudo umount /mnt/simplefs
$ sudo rmmod simplefs
$ sudo dmesg | tail -1
simplefs: unregistered
$ sudo losetup -d "$LOOP"
$ rm /tmp/simplefs.img
```

If anything is left holding a reference, the following commands narrow
down the leak:

```bash
$ mount | grep simplefs
$ lsmod  | grep simplefs
$ losetup -a | grep simplefs.img
```

---

## IOCTL reference

| Number                      | Direction | Argument                          | Purpose                                                |
| --------------------------- | --------- | --------------------------------- | ------------------------------------------------------ |
| `SIMPLEFS_IOC_ZERO_ALL`     | none      | —                                 | Zero every file's data, drop affected page caches.     |
| `SIMPLEFS_IOC_ERASE`        | none      | —                                 | Zero every file *and* invalidate both superblocks.     |
| `SIMPLEFS_IOC_GET_HASHES`   | in/out    | `struct simplefs_hash_list`       | Return `{name, crc32}` for every file.                 |
| `SIMPLEFS_IOC_GET_MAPPING`  | in/out    | `struct simplefs_mapping`         | Translate a file name to `(start_sector, nsectors)`.   |

`SIMPLEFS_IOC_GET_HASHES` uses a two-phase pattern: the caller invokes
it once with `capacity = 0` to obtain `count`, then a second time with a
caller-provided buffer of `count` entries. `simplefs_cli` does both
phases automatically.

---

## Design notes

* **Block size.** Hard-coded to 512 bytes. `sb_set_blocksize(sb, 512)`
  at mount time.
* **Page cache.** Reads and writes go through `generic_file_read_iter` /
  `generic_file_write_iter`. The accompanying `address_space_operations`
  maps every logical block to the absolute device sector via a small
  `get_block` helper.
* **Checksum.** `crc32_le` from `<linux/crc32.h>` computed over
  `offsetof(struct simplefs_sb_disk, checksum)` bytes, i.e. **every
  field except `checksum` itself**.
* **CRC reads coherently.** `SIMPLEFS_IOC_GET_HASHES` reads file content
  through the inode's address space (`read_mapping_page`), so it is
  always coherent with concurrent `write` / `fsync` activity.
* **Auto-format.** When both superblocks fail validation,
  `fill_super` writes a fresh pair using the current module parameters.
  The file area itself is left untouched; `read` returns zeros for the
  yet-untouched sectors and `simplefs_test` overwrites them anyway.
* **Concurrency.** The in-memory file table is read-only after mount,
  so no locking is required around `sbi->files[]`.

---

## License

GPL-2.0, as required for in-tree-style kernel modules.
