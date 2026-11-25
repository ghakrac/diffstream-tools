#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ $EUID -ne 0 ]]; then
    echo "Error: run with sudo (sudo ./run_tests.sh)" >&2
    exit 1
fi

for bin in ./diffstream ./thindiffstream ./zfsdiffstream ./blkstream; do
    [[ -x "$bin" ]] || { echo "Missing executable $bin — run ./build_pkg.sh first" >&2; exit 1; }
done

run_suite() {
    local name="$1"
    local script="$2"
    local req_cmd="$3"

    echo "=== Running ${name} (${script}) ==="
    if [[ ! -f "$script" ]]; then
        echo "SKIP: ${script} not found"
        return 0
    fi
    if ! command -v "$req_cmd" >/dev/null 2>&1; then
        echo "SKIP: '${req_cmd}' not installed on this host"
        return 0
    fi

    chmod +x "$script"
    "$script"
    echo "PASS: ${name}"
}

run_suite "Filesystem Reflink Test" "./tests/test.sh"       "mkfs.btrfs"
run_suite "LVM-Thin Test"           "./tests/test_lvm.sh"   "thin_delta"
run_suite "ZFS ZVOL Test"           "./tests/test_zfs.sh"   "zpool"
run_suite "BTRFS Test"              "./tests/test_btrfs.sh" "mkfs.btrfs"
run_suite "Block Device P2V Test"   "./tests/test_blk.sh"   "losetup"
run_suite "XFS Reflink Test"        "./tests/test_xfs.sh"   "mkfs.xfs"

echo "=== All available test suites passed! ==="
