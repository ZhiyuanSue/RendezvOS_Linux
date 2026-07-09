#!/usr/bin/env bash
# Cross-build static busybox and install applets into rootfs/bin/.
#
# Prerequisites (host): busybox source tree, cross gcc for ARCH.
#
# Usage:
#   ARCH=x86_64 script/rootfs/build_busybox.sh
#   ARCH=aarch64 BUSYBOX_SRC=/path/to/busybox-1.36.1 script/rootfs/build_busybox.sh
#
# After this, run script/rootfs/build_cpio.sh to refresh rootfs.cpio.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
ROOTFS_DIR="${ROOTFS_DIR:-$ROOT_DIR/rootfs}"
BUSYBOX_SRC="${BUSYBOX_SRC:-$ROOT_DIR/third_party/busybox}"

case "$ARCH" in
x86_64) CROSS_PREFIX="${CROSS_PREFIX:-x86_64-linux-gnu-}" ;;
aarch64) CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}" ;;
riscv64) CROSS_PREFIX="${CROSS_PREFIX:-riscv64-linux-gnu-}" ;;
*)
	echo "ERROR: unsupported ARCH=$ARCH" >&2
	exit 1
	;;
esac

CC="${CC:-${CROSS_PREFIX}gcc}"

if [[ ! -d "$BUSYBOX_SRC" ]]; then
	cat >&2 <<EOF
ERROR: busybox source not found at: $BUSYBOX_SRC

Options:
  1. Download and extract busybox, e.g.:
       mkdir -p $ROOT_DIR/third_party
       curl -L -o /tmp/busybox.tar.bz2 https://busybox.net/downloads/busybox-1.36.1.tar.bz2
       tar -C $ROOT_DIR/third_party -xjf /tmp/busybox.tar.bz2
       mv $ROOT_DIR/third_party/busybox-1.36.1 $BUSYBOX_SRC
  2. Or set BUSYBOX_SRC=/path/to/your/busybox

Then re-run this script.
EOF
	exit 1
fi

if ! command -v "${CC}" >/dev/null 2>&1; then
	echo "ERROR: cross compiler not found: $CC" >&2
	exit 1
fi

mkdir -p "$ROOTFS_DIR/bin"
BB_BUILD="$ROOT_DIR/build/busybox-$ARCH"
mkdir -p "$BB_BUILD"

echo "Configuring busybox (static) for $ARCH ..."
make -C "$BUSYBOX_SRC" O="$BB_BUILD" distclean 2>/dev/null || true
make -C "$BUSYBOX_SRC" O="$BB_BUILD" defconfig

# Force static, no libm surprises for minimal demo.
sed -i.bak \
	-e 's/^CONFIG_STATIC=.*/CONFIG_STATIC=y/' \
	-e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
	"$BB_BUILD/.config" 2>/dev/null || \
	sed -i '' \
		-e 's/^CONFIG_STATIC=.*/CONFIG_STATIC=y/' \
		-e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
		"$BB_BUILD/.config"

make -C "$BUSYBOX_SRC" O="$BB_BUILD" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
	CROSS_COMPILE="$CROSS_PREFIX" CC="$CC"

install -m 755 "$BB_BUILD/busybox" "$ROOTFS_DIR/bin/busybox"
(
	cd "$ROOTFS_DIR/bin"
	./busybox --install -s .
)

echo "Installed busybox to $ROOTFS_DIR/bin/"
ls -la "$ROOTFS_DIR/bin" | head -20
