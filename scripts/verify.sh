#!/usr/bin/env bash
# Full local verification of SimpleFS. Run as root:
#   sudo bash scripts/verify.sh
#
# The script never aborts on a single failed step — it records pass/fail
# for every check and prints a summary at the end.

set -u
cd "$(dirname "$0")/.." || exit 1

IMG=/tmp/simplefs_verify.img
MNT=/mnt/simplefs_verify
LOOP=""
PASS=0
FAIL=0
FAILED_STEPS=()

step() {
    echo
    echo "=== STEP $1: $2"
}
ok()   { echo "STATUS: OK   ($1)"; PASS=$((PASS+1)); }
fail() { echo "STATUS: FAIL ($1)"; FAIL=$((FAIL+1)); FAILED_STEPS+=("$1"); }

cleanup() {
    echo
    echo "=== CLEANUP"
    mountpoint -q "$MNT" && umount "$MNT" 2>/dev/null
    rmdir "$MNT" 2>/dev/null
    lsmod | grep -q '^simplefs ' && rmmod simplefs 2>/dev/null
    [[ -n "$LOOP" ]] && losetup -d "$LOOP" 2>/dev/null
    rm -f "$IMG"
}
trap cleanup EXIT

if [[ $(id -u) -ne 0 ]]; then
    echo "must run as root (sudo bash scripts/verify.sh)"
    exit 1
fi

# ---------------------------------------------------------------- #
step 0 "build kernel module and userspace"
make clean >/dev/null 2>&1
if make >/tmp/simplefs_build.log 2>&1; then
    ls kernel/simplefs.ko user/simplefs_cli user/simplefs_test
    ok "build"
else
    tail -30 /tmp/simplefs_build.log
    fail "build"
    exit 1
fi

# ---------------------------------------------------------------- #
step 1 "modinfo lists all 5 module params"
parms=$(modinfo kernel/simplefs.ko | grep -c '^parm:')
echo "parm lines: $parms"
[[ "$parms" -eq 5 ]] && ok "modinfo" || fail "modinfo"

# ---------------------------------------------------------------- #
step 2 "create 4 MiB backing image and bind to a free loop device"
truncate -s 4M "$IMG"
LOOP=$(losetup -f --show "$IMG")
echo "LOOP=$LOOP"
size=$(blockdev --getsz "$LOOP")
echo "size in sectors: $size"
[[ "$size" -eq 8192 ]] && ok "loopdev" || fail "loopdev"

# ---------------------------------------------------------------- #
step 3 "load module"
if insmod kernel/simplefs.ko device="$LOOP" sb_first=0 sb_second=8 \
       name_max=32 file_size_sectors=4 2>&1; then
    lsmod | grep '^simplefs '
    ok "insmod"
else
    fail "insmod"; exit 1
fi

# ---------------------------------------------------------------- #
step 4 "mount, expect auto-format + 2046 files"
mkdir -p "$MNT"
if mount -t simplefs "$LOOP" "$MNT" 2>&1; then
    dmesg | tail -2
    count=$(ls "$MNT" | wc -l)
    echo "file count: $count"
    [[ "$count" -eq 2046 ]] && ok "mount" || fail "mount"
else
    fail "mount"; exit 1
fi

# ---------------------------------------------------------------- #
step 5 "stat first/last file — must be 2048 bytes each"
first_size=$(stat -c '%s' "$MNT/file_0000")
last_size=$(stat  -c '%s' "$MNT/file_2045")
echo "file_0000 size: $first_size"
echo "file_2045 size: $last_size"
[[ "$first_size" -eq 2048 && "$last_size" -eq 2048 ]] \
    && ok "stat" || fail "stat"

# ---------------------------------------------------------------- #
step 6 "simplefs_test: random write/read on every file"
out=$(./user/simplefs_test "$MNT" | tail -1)
echo "$out"
echo "$out" | grep -q "2046 passed, 0 failed" && ok "test" || fail "test"

# ---------------------------------------------------------------- #
step 7 "ioctl mapping: f0->1, f1->9 (jumps over sb_second=8), f2->13"
m0=$(./user/simplefs_cli "$MNT" mapping file_0000)
m1=$(./user/simplefs_cli "$MNT" mapping file_0001)
m2=$(./user/simplefs_cli "$MNT" mapping file_0002)
echo "$m0"
echo "$m1"
echo "$m2"
echo "$m0" | grep -q "start_sector=1 nsectors=4"  || { fail "mapping_f0"; }
echo "$m1" | grep -q "start_sector=9 nsectors=4"  || { fail "mapping_f1"; }
echo "$m2" | grep -q "start_sector=13 nsectors=4" && ok "mapping" || fail "mapping"

# ---------------------------------------------------------------- #
step 8 "ioctl mapping for non-existent file -> ENOENT"
err=$(./user/simplefs_cli "$MNT" mapping nope 2>&1)
echo "$err"
echo "$err" | grep -qi "No such file" && ok "mapping_enoent" || fail "mapping_enoent"

# ---------------------------------------------------------------- #
step 9 "ioctl hashes: count=2046, lines=2047 (header + entries)"
lines=$(./user/simplefs_cli "$MNT" hashes | wc -l)
head_line=$(./user/simplefs_cli "$MNT" hashes | head -1)
echo "header: $head_line"
echo "total lines: $lines"
[[ "$head_line" == "file_count=2046" && "$lines" -eq 2047 ]] \
    && ok "hashes_count" || fail "hashes_count"

# ---------------------------------------------------------------- #
step 10 "ioctl zero-all then verify file_0000 is full of zero bytes"
./user/simplefs_cli "$MNT" zero-all
sync
hex=$(head -c 16 "$MNT/file_0000" | xxd -p)
echo "first 16 bytes: $hex"
[[ "$hex" == "00000000000000000000000000000000" ]] \
    && ok "zero_all" || fail "zero_all"

# ---------------------------------------------------------------- #
step 11 "after zero-all, all files share the same crc32 (=crc of zeros)"
uniq_crcs=$(./user/simplefs_cli "$MNT" hashes | tail -n +2 \
            | awk '{print $NF}' | sort -u | wc -l)
echo "unique crc32 values: $uniq_crcs"
[[ "$uniq_crcs" -eq 1 ]] && ok "hashes_after_zero" || fail "hashes_after_zero"

# ---------------------------------------------------------------- #
step 12 "corrupt primary SB on disk, remount, expect repair from secondary"
umount "$MNT"
dd if=/dev/zero of="$LOOP" bs=512 count=1 seek=0 conv=notrunc status=none
echo -n "primary  magic before mount: "; xxd -l 4 -s 0    "$LOOP" | awk '{print $2$3}'
echo -n "secondary magic before mount: "; xxd -l 4 -s 4096 "$LOOP" | awk '{print $2$3}'
mount -t simplefs "$LOOP" "$MNT" 2>&1
dmesg | tail -2
echo -n "primary magic after  mount: "; xxd -l 4 -s 0 "$LOOP" | awk '{print $2$3}'
mag=$(xxd -l 4 -s 0 "$LOOP" | awk '{print $2$3}')
[[ "$mag" == "53534653" ]] && ok "repair_primary" || fail "repair_primary"

# ---------------------------------------------------------------- #
step 13 "corrupt secondary SB on disk, remount, expect repair from primary"
umount "$MNT"
dd if=/dev/zero of="$LOOP" bs=512 count=1 seek=8 conv=notrunc status=none
mount -t simplefs "$LOOP" "$MNT" 2>&1
dmesg | tail -1
mag=$(xxd -l 4 -s 4096 "$LOOP" | awk '{print $2$3}')
echo "secondary magic after mount: $mag"
[[ "$mag" == "53534653" ]] && ok "repair_secondary" || fail "repair_secondary"

# ---------------------------------------------------------------- #
step 14 "ioctl erase: both SBs invalidated, next mount reformats"
./user/simplefs_cli "$MNT" erase
umount "$MNT"
m1=$(xxd -l 4 -s 0    "$LOOP" | awk '{print $2$3}')
m2=$(xxd -l 4 -s 4096 "$LOOP" | awk '{print $2$3}')
echo "primary after erase:   $m1"
echo "secondary after erase: $m2"
mount -t simplefs "$LOOP" "$MNT" 2>&1
dmesg | tail -1
re1=$(xxd -l 4 -s 0    "$LOOP" | awk '{print $2$3}')
re2=$(xxd -l 4 -s 4096 "$LOOP" | awk '{print $2$3}')
echo "primary after remount: $re1"
echo "secondary after remount: $re2"
[[ "$m1" == "00000000" && "$m2" == "00000000" \
   && "$re1" == "53534653" && "$re2" == "53534653" ]] \
    && ok "erase" || fail "erase"

# ---------------------------------------------------------------- #
step 15 "param validation: sb_first==sb_second must be rejected"
umount "$MNT"
rmmod simplefs
insmod kernel/simplefs.ko device="$LOOP" sb_first=0 sb_second=0 \
       name_max=32 file_size_sectors=4 2>&1
rc=0
mount -t simplefs "$LOOP" "$MNT" 2>/dev/null || rc=$?
dmesg | tail -1
echo "mount rc: $rc"
[[ "$rc" -ne 0 ]] && ok "param_same_sb" || fail "param_same_sb"
rmmod simplefs

# ---------------------------------------------------------------- #
step 16 "param validation: changing name_max on already-formatted FS must fail"
# Show what's on disk first so we know the previous run left a valid SB.
echo -n "on-disk magic   (sec 0): "; xxd -l  4 -s 0    "$LOOP" | awk '{print $2$3}'
echo -n "on-disk name_max(sec 0): "; xxd -l  4 -s 24   "$LOOP" | awk '{print $2}'
echo -n "on-disk magic   (sec 8): "; xxd -l  4 -s 4096 "$LOOP" | awk '{print $2$3}'
insmod kernel/simplefs.ko device="$LOOP" sb_first=0 sb_second=8 \
       name_max=64 file_size_sectors=4 2>&1
dmesg_before=$(dmesg | wc -l)
rc=0
mount -t simplefs "$LOOP" "$MNT" 2>/dev/null || rc=$?
echo "new dmesg lines:"
dmesg | tail -n $(( $(dmesg | wc -l) - dmesg_before ))
echo "mount rc: $rc"
[[ "$rc" -ne 0 ]] && ok "param_mismatch" || fail "param_mismatch"
mountpoint -q "$MNT" && umount "$MNT"
rmmod simplefs

# ---------------------------------------------------------------- #
echo
echo "================================================================"
echo "SUMMARY: $PASS passed, $FAIL failed"
if (( FAIL )); then
    echo "Failed steps: ${FAILED_STEPS[*]}"
    exit 1
fi
echo "ALL CHECKS PASSED"
