#!/usr/bin/env bash
set -euo pipefail

SRC_IMG="/tmp/blk-src.bin"
DST_FILE="/tmp/blk-dst-sparse.img"
DST_BLK_IMG="/tmp/blk-dst-dev.bin"
SMALL_IMG="/tmp/blk-small-dev.bin"
LOOP_SRC=""
LOOP_DST=""
LOOP_SMALL=""

cleanup() {
    [[ -n "$LOOP_SRC" ]]   && losetup -d "$LOOP_SRC"   2>/dev/null || true
    [[ -n "$LOOP_DST" ]]   && losetup -d "$LOOP_DST"   2>/dev/null || true
    [[ -n "$LOOP_SMALL" ]] && losetup -d "$LOOP_SMALL" 2>/dev/null || true
    rm -f "$SRC_IMG" "$DST_FILE" "$DST_BLK_IMG" "$SMALL_IMG"
}
trap cleanup EXIT

# 1. Create a 100 MiB source loop device with two 8 MiB non-zero regions
truncate -s 100M "$SRC_IMG"
dd if=/dev/urandom of="$SRC_IMG" bs=1M count=8 seek=10 conv=notrunc status=none
dd if=/dev/urandom of="$SRC_IMG" bs=1M count=8 seek=50 conv=notrunc status=none
LOOP_SRC=$(losetup --find --show "$SRC_IMG")

echo "=== Case 1: Loop block device -> sparse file (P2V) ==="
./blkstream --source "$LOOP_SRC" --nocache -v | \
  ./diffstream --apply --target "$DST_FILE" --nocache

read -r H_SRC _ < <(sha256sum "$LOOP_SRC")
read -r H_DST _ < <(sha256sum "$DST_FILE")
[[ "$H_SRC" == "$H_DST" ]] || { echo "FAIL [Case 1]: SHA-256 mismatch"; exit 1; }
echo "PASS [Case 1]: $H_SRC (sparse size: $(du -h "$DST_FILE" | cut -f1))"

echo "=== Case 2: Zeroing source blocks discards stale data on non-empty target ==="
dd if=/dev/zero of="$LOOP_SRC" bs=1M count=4 seek=52 conv=notrunc status=none
./blkstream --source "$LOOP_SRC" --nocache -v | \
  ./diffstream --apply --target "$DST_FILE" --nocache

read -r H_SRC _ < <(sha256sum "$LOOP_SRC")
read -r H_DST _ < <(sha256sum "$DST_FILE")
[[ "$H_SRC" == "$H_DST" ]] || { echo "FAIL [Case 2]: SHA-256 mismatch"; exit 1; }
echo "PASS [Case 2]: $H_SRC (sparse size: $(du -h "$DST_FILE" | cut -f1))"

echo "=== Case 3: Loop block device -> larger 120 MiB loop device (--noresize --taildiscard) ==="
dd if=/dev/urandom of="$DST_BLK_IMG" bs=1M count=120 status=none
LOOP_DST=$(losetup --find --show "$DST_BLK_IMG")

./blkstream --source "$LOOP_SRC" --nocache -v | \
  ./diffstream --apply --target "$LOOP_DST" --noresize --taildiscard --nocache

read -r H_DST_100M _ < <(head -c $((100 * 1024 * 1024)) "$LOOP_DST" | sha256sum)
[[ "$H_SRC" == "$H_DST_100M" ]] || { echo "FAIL [Case 3a]: first 100 MiB mismatch"; exit 1; }

NONZERO_TAIL=$(dd if="$LOOP_DST" bs=1M skip=100 count=20 status=none | tr -d '\0' | wc -c)
[[ "$NONZERO_TAIL" -eq 0 ]] || { echo "FAIL [Case 3b]: tail not discarded ($NONZERO_TAIL non-zero bytes)"; exit 1; }
echo "PASS [Case 3]: 100 MiB parity + 20 MiB tail discarded cleanly"

echo "=== Case 4: Smaller 80 MiB loop device fails cleanly under --noresize ==="
truncate -s 80M "$SMALL_IMG"
LOOP_SMALL=$(losetup --find --show "$SMALL_IMG")
if ./blkstream --source "$LOOP_SRC" | ./diffstream --apply --target "$LOOP_SMALL" --noresize 2>/dev/null; then
    echo "FAIL [Case 4]: expected failure on smaller block device"
    exit 1
fi
echo "PASS [Case 4]: smaller target device rejected as expected"

echo "=== Case 5: V2P --truncate 80M from 100 MiB source onto 80 MiB block device ==="
dd if=/dev/urandom of="$LOOP_SRC" bs=1M count=2 seek=95 conv=notrunc status=none
./blkstream --source "$LOOP_SRC" --truncate 80M --nocache -v | \
  ./diffstream --apply --target "$LOOP_SMALL" --noresize --nocache

read -r H_SRC_80M _ < <(head -c $((80 * 1024 * 1024)) "$LOOP_SRC" | sha256sum)
read -r H_SMALL_80M _ < <(sha256sum "$LOOP_SMALL")
[[ "$H_SRC_80M" == "$H_SMALL_80M" ]] || { echo "FAIL [Case 5]: 80 MiB truncated parity mismatch"; exit 1; }
echo "PASS [Case 5]: 100 MiB source truncated cleanly onto 80 MiB block device"
echo "ALL BLOCK STREAM TESTS PASSED!"
