#!/usr/bin/env bash
set -euo pipefail

LOOP_IMG="/tmp/xfs-loop.bin"
MNT="/tmp/xfs-reflink-test"
LOOP_DEV=""

cleanup() {
    mountpoint -q "$MNT" 2>/dev/null && umount "$MNT"
    [[ -n "$LOOP_DEV" ]] && losetup -d "$LOOP_DEV" 2>/dev/null || true
    rm -rf "$MNT" "$LOOP_IMG"
}
trap cleanup EXIT

assert_match() {
    local a="$1" b="$2" label="$3"
    local ha hb
    read -r ha _ < <(sha256sum "$a")
    read -r hb _ < <(sha256sum "$b")
    if [[ "$ha" != "$hb" ]]; then
        echo "FAIL [$label]: SHA-256 mismatch ($ha != $hb)"
        exit 1
    fi
    echo "PASS [$label]: $ha"
}

# 1. Create a 256 MiB sparse backing file and format as XFS with reflink=1
truncate -s 256M "$LOOP_IMG"
LOOP_DEV=$(losetup --find --show "$LOOP_IMG")
mkfs.xfs -f -q -m reflink=1 "$LOOP_DEV"

mkdir -p "$MNT"
mount "$LOOP_DEV" "$MNT"

echo "=== Case 1: XFS reflink extents, partial overwrite, hole punch, growth ==="
truncate -s 100M "$MNT/live.img"
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=10 seek=10 conv=notrunc status=none
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=10 seek=30 conv=notrunc status=none
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=10 seek=50 conv=notrunc status=none
sync

cp --reflink=always "$MNT/live.img" "$MNT/base.img"
cp --sparse=always "$MNT/base.img" "$MNT/target.img"

# Mutate:
# - Leave 10M..20M extent untouched (skipped via shared physical offset)
# - Overwrite 2 MiB inside 30M..40M
# - Overwrite 64 KiB at 50M (splits reflinked extent)
# - Punch 5 MiB hole at 52M and grow file to 110 MiB
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=2 seek=32 conv=notrunc status=none
dd if=/dev/urandom of="$MNT/live.img" bs=64K count=1 seek=800 conv=notrunc status=none
fallocate -p -o 52M -l 5M "$MNT/live.img"
truncate -s 110M "$MNT/live.img"
sync
cp --reflink=always "$MNT/live.img" "$MNT/pend1.img"

./diffstream --diff --base "$MNT/base.img" --change "$MNT/pend1.img" --nocache -v | \
  ./diffstream --apply --target "$MNT/target.img" --nocache

assert_match "$MNT/pend1.img" "$MNT/target.img" "xfs reflink + split + punch + grow"

echo "=== Case 2: Zero-filled extent reclamation (is_all_zero -> OP_HOLE) ==="
dd if=/dev/zero of="$MNT/live.img" bs=1M count=8 seek=70 conv=notrunc status=none
sync
cp --reflink=always "$MNT/live.img" "$MNT/pend2.img"

./diffstream --diff --base "$MNT/pend1.img" --change "$MNT/pend2.img" --nocache -v | \
  ./diffstream --apply --target "$MNT/target.img" --nocache

assert_match "$MNT/pend2.img" "$MNT/target.img" "xfs zero-extent reclamation"

echo "=== Case 3: File shrinkage (OP_TRUNCATE downward) ==="
truncate -s 40M "$MNT/live.img"
sync
cp --reflink=always "$MNT/live.img" "$MNT/pend3.img"

./diffstream --diff --base "$MNT/pend2.img" --change "$MNT/pend3.img" --nocache -v | \
  ./diffstream --apply --target "$MNT/target.img" --nocache

assert_match "$MNT/pend3.img" "$MNT/target.img" "xfs shrink to 40M"

echo "=== Sparse Disk Usage Summary ==="
du -h "$MNT/pend3.img" "$MNT/target.img"
echo "=== Case 4: Full stream (no --base) overwrites dirty non-empty target ==="
dd if=/dev/urandom of="$MNT/dirty_target.img" bs=1M count=40 status=none
./diffstream --diff --change "$MNT/pend3.img" --nocache -v | \
  ./diffstream --apply --target "$MNT/dirty_target.img" --nocache
assert_match "$MNT/pend3.img" "$MNT/dirty_target.img" "xfs full stream onto dirty target"
echo "ALL XFS TESTS PASSED!"
