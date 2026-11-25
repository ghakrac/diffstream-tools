#!/usr/bin/env bash
set -euo pipefail

LVM_BIN="./thindiffstream"
FS_BIN="./diffstream"

LVM_IMG="/tmp/test-lvm-pv.bin"
FS_IMG="/tmp/test-btrfs-target.bin"
MNT="/tmp/test-btrfs-mnt"
VG="vg_diffstream_test"
LOOP_LVM=""
LOOP_FS=""

cleanup() {
    mountpoint -q "$MNT" 2>/dev/null && umount "$MNT"
    vgremove -ff "$VG" >/dev/null 2>&1 || true
    [[ -n "$LOOP_LVM" ]] && pvremove -ff "$LOOP_LVM" >/dev/null 2>&1 || true
    [[ -n "$LOOP_LVM" ]] && losetup -d "$LOOP_LVM" 2>/dev/null || true
    [[ -n "$LOOP_FS" ]] && losetup -d "$LOOP_FS" 2>/dev/null || true
    rm -rf "$MNT" "$LVM_IMG" "$FS_IMG"
}
trap cleanup EXIT

# 1. Create temporary LVM-Thin pool on loopback PV
truncate -s 512M "$LVM_IMG"
LOOP_LVM=$(losetup --find --show "$LVM_IMG")
pvcreate -q "$LOOP_LVM" >/dev/null
vgcreate -q "$VG" "$LOOP_LVM" >/dev/null
lvcreate -q -L 300M -T "$VG/tpool" >/dev/null

# 2. Create temporary Btrfs target filesystem on loopback
truncate -s 256M "$FS_IMG"
LOOP_FS=$(losetup --find --show "$FS_IMG")
mkfs.btrfs -q "$LOOP_FS"
mkdir -p "$MNT"
mount "$LOOP_FS" "$MNT"

# 3. Create 100 MiB source thin LV, write two 10 MiB regions, take base snap, and seed targets
lvcreate -q -V 100M -T "$VG/tpool" -n live_lv >/dev/null
dd if=/dev/urandom of="/dev/$VG/live_lv" bs=1M count=10 seek=10 oflag=direct status=none
dd if=/dev/urandom of="/dev/$VG/live_lv" bs=1M count=10 seek=50 oflag=direct status=none

lvcreate -q -s -n base_snap "$VG/live_lv" >/dev/null
lvchange -ay -K "$VG/base_snap"

# Seed initial Btrfs file target and initial LVM thin LV target
cp --sparse=always "/dev/$VG/base_snap" "$MNT/base.img"
cp --reflink=always "$MNT/base.img" "$MNT/target.img"
lvcreate -q -V 100M -T "$VG/tpool" -n target_lv >/dev/null
dd if="/dev/$VG/base_snap" of="/dev/$VG/target_lv" bs=4M conv=sparse status=none

# 4. Simulate live VM activity: overwrite 2 MiB, discard 4 MiB, and grow LV to 112 MiB
dd if=/dev/urandom of="/dev/$VG/live_lv" bs=1M count=2 seek=12 oflag=direct status=none
blkdiscard -o $((52 * 1024 * 1024)) -l $((4 * 1024 * 1024)) "/dev/$VG/live_lv"
lvresize -q -f -L 112M "$VG/live_lv" >/dev/null

lvcreate -q -s -n pend_snap "$VG/live_lv" >/dev/null
lvchange -ay -K "$VG/pend_snap"

# 5A. Test cross-tool migration: thindiffstream -> diffstream (Btrfs file)
echo "--- Testing LVM-Thin -> File migration ---"
"$LVM_BIN" --diff --base "/dev/$VG/base_snap" --change "/dev/$VG/pend_snap" --nocache -v | \
  "$FS_BIN" --apply --target "$MNT/target.img" --nocache

# 5B. Test LVM-to-LVM replication: thindiffstream -> thindiffstream (Thin LV)
echo "--- Testing LVM-Thin -> LVM-Thin replication ---"
"$LVM_BIN" --base "/dev/$VG/base_snap" --change "/dev/$VG/pend_snap" --target "/dev/$VG/target_lv" --nocache -v

# 5C. Test reverse migration: diffstream (File) -> thindiffstream (Thin LV)
echo "--- Testing File -> LVM-Thin reverse migration ---"
lvcreate -q -V 100M -T "$VG/tpool" -n rev_lv >/dev/null
dd if="/dev/$VG/base_snap" of="/dev/$VG/rev_lv" bs=4M conv=sparse status=none
"$FS_BIN" --diff --base "$MNT/base.img" --change "$MNT/target.img" --nocache -v | \
  "$LVM_BIN" --apply --target "/dev/$VG/rev_lv" --nocache

# 6. Verify byte-for-byte SHA-256 across all targets
echo "--- Verifying SHA-256 checksums ---"
read -r H_SRC _ < <(sha256sum "/dev/$VG/pend_snap")
read -r H_FS  _ < <(sha256sum "$MNT/target.img")
read -r H_LVM _ < <(sha256sum "/dev/$VG/target_lv")
read -r H_REV _ < <(sha256sum "/dev/$VG/rev_lv")
echo "pend_snap: $H_SRC"
echo "file_tgt:  $H_FS"
echo "lvm_tgt:   $H_LVM"
echo "rev_tgt:   $H_REV"
[[ "$H_SRC" == "$H_FS" && "$H_SRC" == "$H_LVM" && "$H_SRC" == "$H_REV" ]] || { echo "FAIL: SHA-256 mismatch!"; exit 1; }
# 5D. Test full stream (no --base) onto dirty non-empty LVM thin LV
echo "--- Testing LVM-Thin full stream (no --base) onto dirty target ---"
lvcreate -q -V 112M -T "$VG/tpool" -n dirty_lv >/dev/null
dd if=/dev/urandom of="/dev/$VG/dirty_lv" bs=1M count=112 oflag=direct status=none
"$LVM_BIN" --diff --change "/dev/$VG/pend_snap" --nocache -v | \
  "$LVM_BIN" --apply --target "/dev/$VG/dirty_lv" --nocache
read -r H_DIRTY _ < <(sha256sum "/dev/$VG/dirty_lv")
[[ "$H_SRC" == "$H_DIRTY" ]] || { echo "FAIL: dirty_lv SHA-256 mismatch!"; exit 1; }
echo "PASS: All LVM and cross-filesystem targets match!"
