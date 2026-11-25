# diffstream-tools

Zero-dependency, wire-compatible Changed-Block Tracking (CBT) replication and backup utilities for **XFS**, **Btrfs**, **LVM-Thin**, **ZFS**, and raw block devices.

All four tools speak the identical little-endian `DIFFSTR1` wire protocol (`OP_TRUNCATE`, `OP_DATA`, `OP_HOLE`, `OP_EOF`), so **any `--diff` / `--source` producer can pipe into any `--apply` target** locally or over `zstd` / `ssh` / `age`.

Every tool emits `OP_HOLE` for unmapped or zero-filled regions even in full-stream mode (`--base` omitted), safely discarding stale blocks on pre-populated targets.

> **Warning:** `--apply --target` writes raw block extents directly in place (like `dd`). Always verify the `--target` path before running, and stage updates into a CoW snapshot (`cp --reflink=always`, `lvcreate -s`, or `zfs clone`) in production.

---

## Installation

Each tool is deliberately engineered as a **single, self-contained C++17 file** with zero external library dependencies beyond the Linux kernel headers and `libc`. Choose whichever deployment model fits your workflow:

### Option 1: Permanent System Install (`.deb` Package)
Recommended for production hypervisors, backup servers, and storage nodes:

```bash
./build_pkg.sh 1.0.0
sudo apt install ./diffstream-tools_1.0.0_$(dpkg --print-architecture).deb
```

### Option 2: Ephemeral One-Liner Compile (Rescue ISOs / Minimal Hosts)
Ideal for one-off migrations, live recovery environments, or restricted hosts where installing packages is undesirable—only `g++` is required:

```bash
# Fetch and compile just the tool(s) needed on a live rescue or source/target host
wget -qO blkstream.cpp https://raw.githubusercontent.com/ghakrac/diffstream-tools/main/blkstream.cpp \
  && g++ -O3 -std=c++17 -Wall -Wextra -o blkstream blkstream.cpp

# Or compile any individual tool directly from a local checkout
g++ -O3 -std=c++17 -Wall -Wextra -o diffstream diffstream.cpp
```

---

## Storage Compatibility Matrix

### 1. Source (`--diff` / `--source`) — Data & Changed-Block Discovery

| Source Storage | Tool to Use | Mode & How Blocks Are Tracked |
| :--- | :--- | :--- |
| **XFS** (reflink files) | `diffstream --diff` | **Incremental CBT & Full:** `FS_IOC_FIEMAP` physical extent comparison. |
| **Btrfs** (reflink / `compress=zstd`) | `diffstream --diff` | **Incremental CBT & Full:** `FS_IOC_FIEMAP` (skips untouched `ENCODED` extents; reads split/inline). |
| **LVM-Thin** (thin LVs & snapshots) | `thindiffstream --diff` | **Incremental CBT & Full:** Thin-pool metadata snapshot (`reserve_metadata_snap`) + `thin_delta` / `thin_dump`. |
| **ZFS ZVOL** (`pool/vol@snap`) | `zfsdiffstream --diff` | **Incremental CBT & Full:** Parses `zfs send` DMU replay records (`DRR_WRITE` / `DRR_FREE`). |
| **ZFS Dataset File** (`pool/fs@snap --file <path>`) | `zfsdiffstream --diff` | **Incremental CBT & Full:** Resolves object ID via `.zfs/snapshot` inode and filters `zfs send`. |
| **Raw Block Dev / MD RAID / Partition** | `blkstream --source <dev>` | **Full Sparse Scan:** Sequential read skipping 64 KiB zero blocks via coalesced `OP_HOLE` (ideal for P2V). |

### 2. Destination (`--apply`) — All Wire Ops Supported (`OP_TRUNCATE`, `OP_DATA`, `OP_HOLE`, `OP_EOF`)

| Destination Storage | Tool to Use | Resize & Hole Behavior |
| :--- | :--- | :--- |
| **XFS** (sparse file) | `diffstream --apply` *(or `thindiffstream` / `zfsdiffstream`)* | Auto-resizes via `ftruncate`; punches holes via `FALLOC_FL_PUNCH_HOLE`. |
| **Btrfs** (sparse file) | `diffstream --apply` *(or `thindiffstream` / `zfsdiffstream`)* | Auto-resizes via `ftruncate`; punches holes via `FALLOC_FL_PUNCH_HOLE`. |
| **ext4 / tmpfs** (sparse file) | `diffstream --apply` *(or `thindiffstream` / `zfsdiffstream`)* | Auto-resizes via `ftruncate`; punches holes via `FALLOC_FL_PUNCH_HOLE`. |
| **ZFS Dataset File** (OpenZFS 2.1+) | `diffstream --apply` *(or `thindiffstream` / `zfsdiffstream`)* | Requires OpenZFS 2.1+ for `FALLOC_FL_PUNCH_HOLE` (`OP_HOLE`). |
| **LVM-Thin** (thin LV `/dev/vg/lv`) | `thindiffstream --apply` | Default: unmaps tail (`BLKZEROOUT`) + auto-`lvresize`. Supports `--noresize` / `--taildiscard`. |
| **ZFS ZVOL** (`/dev/zvol/pool/vol`) | `zfsdiffstream --apply` | Default: unmaps tail (`BLKZEROOUT`) + auto-`zfs set volsize`. Supports `--noresize` / `--taildiscard`. |
| **Raw Partition / MD RAID / Fixed Block Dev** | `diffstream --apply` *(or `thindiffstream` / `zfsdiffstream`)* | Use `--noresize` (fails if target `<` source; add `--taildiscard` to zero `[size, target_size)`). |

---

## Real-World Workflows & Examples

### 1. Cross-Infrastructure Migrations (Heterogeneous Storage)
Because all tools share the same `DIFFSTR1` wire format, you can migrate VMs directly between completely different storage stacks while preserving thin provisioning and sending only allocated/changed blocks:

#### ZFS ZVOL -> XFS Sparse VM Image (e.g., Proxmox ZFS -> KVM/libvirt XFS)
```bash
# Initial seed from ZFS snapshot into a sparse XFS image
zfsdiffstream --diff --change tank/vm-101-disk-0@seed --nocache -v \
  | zstd -T0 \

  | ssh kvm-xfs "zstd -d | diffstream --apply --target /var/lib/libvirt/images/vm-101.raw --nocache"

# Final cutover delta after stopping the VM and taking @final
zfsdiffstream --diff --base tank/vm-101-disk-0@seed --change tank/vm-101-disk-0@final --nocache -v \
  | zstd -T0 \

  | ssh kvm-xfs "zstd -d | diffstream --apply --target /var/lib/libvirt/images/vm-101.raw --nocache"
```
#### LVM-Thin -> Btrfs Sparse VM Image (with transparent zstd storage)
```bash
# Stream incremental LVM-Thin snapshot delta into a Btrfs sparse file
thindiffstream --diff --base /dev/vg0/vm_snap1 --change /dev/vg0/vm_snap2 --nocache -v \
  | zstd -T0 \

  | ssh btrfs-node "zstd -d | diffstream --apply --target /srv/vms/vm.img --nocache"
```

#### Reverse Migration: XFS / Btrfs Sparse Image -> LVM-Thin or ZFS ZVOL
```bash
# XFS/Btrfs sparse file -> LVM-Thin LV (auto-resizes target LV to match source)
diffstream --diff --base snap1.img --change snap2.img --nocache -v \
  | zstd -T0 \

  | ssh lvm-node "zstd -d | thindiffstream --apply --target /dev/vg0/vm_disk --nocache"

# XFS/Btrfs sparse file -> ZFS ZVOL (auto-adjusts volsize on target)
diffstream --diff --change vm.img --nocache -v \
  | zstd -T0 \

  | ssh zfs-node "zstd -d | zfsdiffstream --apply --target /dev/zvol/tank/vm_disk --nocache"
```

### 2. Atomic Reflink Backup Rotation (XFS / Btrfs)
Capture a point-in-time CoW snapshot of a running VM image, stream only changed extents to a remote backup host, and rotate the baseline snapshot:

```bash
# 1. Freeze/quiesce guest (optional) and take an instant CoW snapshot
cp --reflink=always /var/lib/libvirt/images/vm.img /var/lib/libvirt/images/vm.snap.new

# 2. Stream delta between previous baseline and new snapshot over Zstd + SSH
diffstream --diff --base /var/lib/libvirt/images/vm.snap.prev \
                  --change /var/lib/libvirt/images/vm.snap.new --nocache -v \
  | zstd -T0 \

  | ssh backup-host "zstd -d | diffstream --apply --target /srv/backups/vm.img --nocache"

# 3. Rotate snapshot baseline for the next run
mv /var/lib/libvirt/images/vm.snap.new /var/lib/libvirt/images/vm.snap.prev
```

### 3. Safe CoW Staging Before Promoting on Target (LVM-Thin -> LVM-Thin)
Stage incoming deltas into a temporary thin clone on the destination so an interrupted transfer never corrupts your last known-good replica:

```bash
# 1. On target-host: create a writable staging clone from the current replica
ssh target-host "lvcreate -s -n vm_staging vg_bak/vm_replica && lvchange -ay -K vg_bak/vm_staging"

# 2. Stream LVM-Thin delta into the staging clone
thindiffstream --diff --base /dev/vg0/snap1 --change /dev/vg0/snap2 --nocache -v \
  | zstd -T0 \

  | ssh target-host "zstd -d | thindiffstream --apply --target /dev/vg_bak/vm_staging --nocache"

# 3. Verify and atomically swap staging clone into place
ssh target-host "lvremove -f vg_bak/vm_replica && lvrename vg_bak vm_staging vm_replica"
```

### 4. ZFS Dataset File & Fixed Block LUN Replication
Replicate an individual file (such as a QCOW2/RAW image) inside a ZFS dataset or mirror a ZVOL onto a fixed-size SAN LUN:

```bash
# Single VM image file inside a ZFS filesystem dataset -> remote Btrfs/XFS host
zfsdiffstream --diff --base tank/vms@snap1 --change tank/vms@snap2 --file prod-db.raw --nocache -v \
  | zstd -T0 \

  | ssh dr-host "zstd -d | diffstream --apply --target /mnt/dr/prod-db.raw --nocache"

# ZVOL -> fixed-size raw partition / SAN LUN (zeroes any trailing blocks past source EOF)
zfsdiffstream --diff --base tank/vm@snap1 --change tank/vm@snap2 --nocache -v \
  | diffstream --apply --target /dev/sdb1 --noresize --taildiscard --nocache
```

### 5. Local One-Command Sync (No Pipe Overhead)
When source and destination live on the same host, omit `--diff` / `--apply` to apply extents directly in a single process:

```bash
diffstream --base snap1.img --change snap2.img --target /mnt/backup/vm.img --nocache -v
```

### 6. Physical-to-Virtual (P2V) Migration
Convert a live or rescue-booted physical server (`/dev/md0` or `/dev/sda`) into a thin-provisioned sparse VM image:

```bash
# 1. Optional: TRIM mounted filesystems on the physical host so free space reads as zeros
fstrim -av

# 2. Stream physical disk (64 KiB zero-block detection -> OP_HOLE) to hypervisor
blkstream --source /dev/md0 --nocache -v \
  | zstd -T0 \

  | ssh vm-host "zstd -d | diffstream --apply --target /var/lib/libvirt/images/migrated-vm.img --nocache"
```

### 7. Virtual-to-Physical (V2P) Migration onto a Smaller Disk
Migrate a 100 GiB sparse VM image onto a smaller 80 GiB physical NVMe drive after shrinking the guest partitions to fit under 80 GiB:

```bash
# 1. Stream VM image clamped to 80G, zeroing any remaining tail space on the physical drive
diffstream --diff --change shrunk_100g_vm.img --truncate 80G --nocache -v \
  | zstd -T0 \

  | ssh target-host "zstd -d | diffstream --apply --target /dev/nvme0n1 --noresize --taildiscard --nocache"

# 2. Relocate the backup GPT header to the physical disk's actual 80G end
ssh target-host "sgdisk -e /dev/nvme0n1"
```

---

## Testing

Run the full cross-filesystem and block-device integration suite as `root` (automatically skips suites whose filesystem tools are not installed):

```bash
./build_pkg.sh
sudo ./tests/run_tests.sh
```

