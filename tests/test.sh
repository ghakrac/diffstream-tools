#!/usr/bin/env bash
set -euo pipefail

LOOP_IMG="/tmp/btrfs-loop.bin"
MNT="/tmp/reflink-test"
LOOP_DEV=""

cleanup() {
    mountpoint -q "$MNT" 2>/dev/null && umount "$MNT"
    [[ -n "$LOOP_DEV" ]] && losetup -d "$LOOP_DEV" 2>/dev/null || true
    rm -rf "$MNT" "$LOOP_IMG"
}

trap cleanup EXIT

# 1. Create a 256 MiB sparse backing file and format as Btrfs
truncate -s 256M "$LOOP_IMG"
LOOP_DEV=$(losetup --find --show "$LOOP_IMG")
mkfs.btrfs -q "$LOOP_DEV"

mkdir -p "$MNT"
mount "$LOOP_DEV" "$MNT"

# 2. Create a 100 MiB sparse image with two 10 MiB data regions and seed target
truncate -s 100M "$MNT/live.img"
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=10 seek=10 conv=notrunc status=none
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=10 seek=50 conv=notrunc status=none
cp --reflink=always "$MNT/live.img" "$MNT/base.img"
cp --sparse=always "$MNT/base.img" "$MNT/target.img"

# 3. Simulate VM activity: overwrite 2 MiB, punch a 5 MiB hole (TRIM), and grow to 110 MiB
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=2 seek=12 conv=notrunc status=none
fallocate -p -o 52M -l 5M "$MNT/live.img"
truncate -s 110M "$MNT/live.img"
cp --reflink=always "$MNT/live.img" "$MNT/pend.img"

# 4. Stream diff via pipe and apply to target
./diffstream --diff --base "$MNT/base.img" --change "$MNT/pend.img" --nocache -v | \
  ./diffstream --apply --target "$MNT/target.img" --nocache

# 5. Verify byte-for-byte identity and sparse block count
sha256sum "$MNT/pend.img" "$MNT/target.img"
du -h "$MNT/pend.img" "$MNT/target.img"
