#!/usr/bin/env bash
set -euo pipefail

ZPOOL_IMG="/tmp/test-zpool.bin"
XFS_IMG="/tmp/test-xfs-target.bin"
MNT="/tmp/test-xfs-mnt"
POOL="zpool_diffstream_test"
LOOP_XFS=""

cleanup() {
    mountpoint -q "$MNT" 2>/dev/null && umount "$MNT"
    zpool destroy -f "$POOL" >/dev/null 2>&1 || true
    [[ -n "$LOOP_XFS" ]] && losetup -d "$LOOP_XFS" 2>/dev/null || true
    rm -rf "$MNT" "$ZPOOL_IMG" "$XFS_IMG"
}
trap cleanup EXIT

# 1. Create temporary ZFS pool with a 100 MiB ZVOL
truncate -s 512M "$ZPOOL_IMG"
zpool create -f "$POOL" "$ZPOOL_IMG"
zfs create -V 100M -s "$POOL/vmvol"
udevadm settle

# 2. Write two 10 MiB blocks and take @snap1
dd if=/dev/urandom of="/dev/zvol/$POOL/vmvol" bs=1M count=10 seek=10 oflag=direct status=none
dd if=/dev/urandom of="/dev/zvol/$POOL/vmvol" bs=1M count=10 seek=50 oflag=direct status=none
zfs snapshot "$POOL/vmvol@snap1"

# 3. Create temporary XFS target filesystem
truncate -s 512M "$XFS_IMG"
LOOP_XFS=$(losetup --find --show "$XFS_IMG")
mkfs.xfs -q "$LOOP_XFS"
mkdir -p "$MNT"
mount "$LOOP_XFS" "$MNT"

# 4. Initial sparse seed from @snap1 -> XFS target
./zfsdiffstream --diff --change "$POOL/vmvol@snap1" --nocache -v | \
  ./diffstream --apply --target "$MNT/vm.img" --nocache

# 5. Overwrite 2 MiB, discard 4 MiB, grow ZVOL to 112 MiB, and take @snap2
dd if=/dev/urandom of="/dev/zvol/$POOL/vmvol" bs=1M count=2 seek=12 oflag=direct status=none
blkdiscard -o $((52 * 1024 * 1024)) -l $((4 * 1024 * 1024)) "/dev/zvol/$POOL/vmvol"
zfs set volsize=112M "$POOL/vmvol"
udevadm settle
zfs snapshot "$POOL/vmvol@snap2"

# 6. Incremental migration @snap1 -> @snap2 -> XFS target
./zfsdiffstream --diff --base "$POOL/vmvol@snap1" --change "$POOL/vmvol@snap2" --nocache -v | \
  ./diffstream --apply --target "$MNT/vm.img" --nocache

# 7. Test reverse migration: diffstream (XFS File) -> zfsdiffstream (ZVOL)
zfs create -V 100M -s "$POOL/revvol"
udevadm settle
./diffstream --diff --change "$MNT/vm.img" --nocache -v | \
  ./zfsdiffstream --apply --target "/dev/zvol/$POOL/revvol" --nocache

# 8. Verify byte-for-byte SHA-256 identity across ZVOL, XFS file, and reverse ZVOL
read -r H_SRC _ < <(sha256sum "/dev/zvol/$POOL/vmvol")
read -r H_FS  _ < <(sha256sum "$MNT/vm.img")
read -r H_REV _ < <(sha256sum "/dev/zvol/$POOL/revvol")
echo "zvol_src: $H_SRC"
echo "xfs_file: $H_FS"
echo "zvol_rev: $H_REV"
[[ "$H_SRC" == "$H_FS" && "$H_SRC" == "$H_REV" ]] || { echo "FAIL: SHA-256 mismatch!"; exit 1; }
du -h "$MNT/vm.img"
# 7B. Test full stream (no --base) onto dirty non-empty ZVOL
zfs create -V 112M -s "$POOL/dirtyvol"
udevadm settle
dd if=/dev/urandom of="/dev/zvol/$POOL/dirtyvol" bs=1M count=112 oflag=direct status=none
./zfsdiffstream --diff --change "$POOL/vmvol@snap2" --nocache -v | \
  ./zfsdiffstream --apply --target "/dev/zvol/$POOL/dirtyvol" --nocache
read -r H_ZDIRTY _ < <(sha256sum "/dev/zvol/$POOL/dirtyvol")
[[ "$H_SRC" == "$H_ZDIRTY" ]] || { echo "FAIL: dirtyvol SHA-256 mismatch!"; exit 1; }
echo "PASS: All ZFS and cross-filesystem targets match!"
