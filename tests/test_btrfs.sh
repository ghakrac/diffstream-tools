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

# 1. Create a 256 MiB sparse backing file and mount Btrfs with zstd compression
truncate -s 256M "$LOOP_IMG"
LOOP_DEV=$(losetup --find --show "$LOOP_IMG")
mkfs.btrfs -q "$LOOP_DEV"

mkdir -p "$MNT"
mount -o compress=zstd "$LOOP_DEV" "$MNT"

echo "=== Case 1: Compressed extents, partial overwrite, hole punch, growth ==="
truncate -s 100M "$MNT/live.img"
python3 -c 'f=open("/tmp/reflink-test/live.img","r+b"); f.seek(10<<20); f.write(b"COMPRESSIBLE_BLOCK__"*(10<<16))'
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=10 seek=30 conv=notrunc status=none
python3 -c 'f=open("/tmp/reflink-test/live.img","r+b"); f.seek(50<<20); f.write(b"SECOND_COMP_BLOCK___"*(10<<16))'
sync

filefrag -v "$MNT/live.img" | grep -q "encoded" && echo "OK: encoded extents verified"

cp --reflink=always "$MNT/live.img" "$MNT/base.img"
cp --sparse=always "$MNT/base.img" "$MNT/target.img"

# Mutate:
# - Leave 10M..20M compressed region untouched (skipped via ENCODED equality)
# - Overwrite 2 MiB inside uncompressed 30M..40M
# - Overwrite 64 KiB at 50M inside compressed 50M..60M (splits encoded extent)
# - Punch 5 MiB hole at 52M and grow file to 110 MiB
dd if=/dev/urandom of="$MNT/live.img" bs=1M count=2 seek=32 conv=notrunc status=none
dd if=/dev/urandom of="$MNT/live.img" bs=64K count=1 seek=800 conv=notrunc status=none
fallocate -p -o 52M -l 5M "$MNT/live.img"
truncate -s 110M "$MNT/live.img"
sync
cp --reflink=always "$MNT/live.img" "$MNT/pend1.img"

./diffstream --diff --base "$MNT/base.img" --change "$MNT/pend1.img" --nocache -v | \
  ./diffstream --apply --target "$MNT/target.img" --nocache

assert_match "$MNT/pend1.img" "$MNT/target.img" "compressed + split + punch + grow"

echo "=== Case 2: File shrinkage (OP_TRUNCATE downward) ==="
truncate -s 40M "$MNT/live.img"
sync
cp --reflink=always "$MNT/live.img" "$MNT/pend2.img"

./diffstream --diff --base "$MNT/pend1.img" --change "$MNT/pend2.img" --nocache -v | \
  ./diffstream --apply --target "$MNT/target.img" --nocache

assert_match "$MNT/pend2.img" "$MNT/target.img" "shrink to 40M"

echo "=== Case 3: Inline small files (FIEMAP_EXTENT_DATA_INLINE) ==="
printf "small-inline-payload-v1" > "$MNT/inline_live.txt"
sync
filefrag -v "$MNT/inline_live.txt" | grep -q "inline" && echo "OK: inline extent verified"

cp --reflink=always "$MNT/inline_live.txt" "$MNT/inline_base.txt"
cp "$MNT/inline_base.txt" "$MNT/inline_target.txt"

# 3a. Unchanged inline file -> should transfer 0.00 MiB
./diffstream --diff --base "$MNT/inline_base.txt" --change "$MNT/inline_base.txt" -v | \
  ./diffstream --apply --target "$MNT/inline_target.txt"
assert_match "$MNT/inline_base.txt" "$MNT/inline_target.txt" "inline unchanged"

# 3b. Modified inline file -> metadata offset changes, must update target
printf "small-inline-payload-v2-modified" > "$MNT/inline_live.txt"
sync
cp --reflink=always "$MNT/inline_live.txt" "$MNT/inline_pend.txt"

./diffstream --diff --base "$MNT/inline_base.txt" --change "$MNT/inline_pend.txt" -v | \
  ./diffstream --apply --target "$MNT/inline_target.txt"
assert_match "$MNT/inline_pend.txt" "$MNT/inline_target.txt" "inline modified"

echo "=== Sparse Disk Usage Summary ==="
du -h "$MNT/pend2.img" "$MNT/target.img"
echo "ALL BTRFS TESTS PASSED!"
