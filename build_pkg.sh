#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-1.0.0}"
ARCH=$(dpkg --print-architecture)
PKG_NAME="diffstream-tools_${VERSION}_${ARCH}"
CXXFLAGS="-O3 -std=c++17 -Wall -Wextra -static-libstdc++ -static-libgcc"

echo "=== Compiling binaries (${ARCH}) ==="
g++ $CXXFLAGS -o diffstream diffstream.cpp
g++ $CXXFLAGS -o thindiffstream thindiffstream.cpp
g++ $CXXFLAGS -o zfsdiffstream zfsdiffstream.cpp
g++ $CXXFLAGS -o blkstream blkstream.cpp
strip --strip-unneeded diffstream thindiffstream zfsdiffstream blkstream

echo "=== Building ${PKG_NAME}.deb ==="
rm -rf "$PKG_NAME" "${PKG_NAME}.deb"
mkdir -p "$PKG_NAME/DEBIAN" "$PKG_NAME/usr/bin" "$PKG_NAME/usr/share/man/man1" "$PKG_NAME/usr/share/doc/diffstream-tools"

install -m 0755 diffstream thindiffstream zfsdiffstream blkstream "$PKG_NAME/usr/bin/"
install -m 0644 man/diffstream.1 man/thindiffstream.1 man/zfsdiffstream.1 man/blkstream.1 "$PKG_NAME/usr/share/man/man1/"
gzip -9n "$PKG_NAME/usr/share/man/man1/"*.1
install -m 0644 README.md LICENSE "$PKG_NAME/usr/share/doc/diffstream-tools/"

cat << EOF > "$PKG_NAME/DEBIAN/control"
Package: diffstream-tools
Version: ${VERSION}
Section: admin
Priority: optional
Architecture: ${ARCH}
Maintainer: Gemini <gemini@google.com>
Depends: libc6 (>= 2.28)
Recommends: thin-provisioning-tools, lvm2
Suggests: zfsutils-linux
Description: Zero-dependency Changed-Block Tracking (CBT) migration & backup tools
 Wire-compatible incremental diff/apply utilities for XFS, Btrfs, LVM-Thin, and ZFS.
EOF

dpkg-deb --build --root-owner-group "$PKG_NAME" >/dev/null
rm -rf "$PKG_NAME"

SRC_TAR="diffstream-tools-${VERSION}-src.tar.gz"
rm -f "$SRC_TAR"
tar --transform "s,^,diffstream-tools-${VERSION}/," -czf "$SRC_TAR" *.cpp man/*.1 tests/*.sh *.sh README.md LICENSE

echo "Done: ./${PKG_NAME}.deb and ./${SRC_TAR} ready (install via: sudo apt install ./${PKG_NAME}.deb)"
