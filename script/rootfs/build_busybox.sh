#!/usr/bin/env bash
# Cross-build static busybox and install applets into rootfs/bin/.
#
# Configuration (see busybox.net FAQ + Buildroot/OE practice):
#   make O=$BB_BUILD defconfig
#   enable CONFIG_STATIC=y
#   disable CONFIG_TC (Linux 6.8+ kernel headers dropped CBQ UAPI; tc.c won't build)
#   make O=$BB_BUILD CROSS_COMPILE=... CC=...   (silentoldconfig runs during build)
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
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}"
BUSYBOX_AUTO_FETCH="${BUSYBOX_AUTO_FETCH:-0}"

fetch_busybox_source() {
	local tarball="/tmp/busybox-${BUSYBOX_VERSION}.tar.bz2"
	local url="https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2"
	local extract_dir="$ROOT_DIR/third_party/busybox-${BUSYBOX_VERSION}"

	echo "Fetching busybox ${BUSYBOX_VERSION} ..."
	mkdir -p "$ROOT_DIR/third_party"
	if ! curl -fsSL -o "$tarball" "$url"; then
		echo "ERROR: failed to download $url" >&2
		return 1
	fi
	rm -rf "$extract_dir"
	tar -C "$ROOT_DIR/third_party" -xjf "$tarball"
	rm -f "$tarball"
	mv "$extract_dir" "$BUSYBOX_SRC"
}

if [[ ! -d "$BUSYBOX_SRC" ]]; then
	if [[ "$BUSYBOX_AUTO_FETCH" == "1" ]]; then
		fetch_busybox_source || exit 1
	else
		cat >&2 <<EOF
ERROR: busybox source not found at: $BUSYBOX_SRC

Options:
  1. Re-run with auto-fetch (make user sets BUSYBOX_AUTO_FETCH=1), or:
       BUSYBOX_AUTO_FETCH=1 ARCH=$ARCH script/rootfs/build_busybox.sh
  2. Download manually, e.g.:
       mkdir -p $ROOT_DIR/third_party
       curl -L -o /tmp/busybox.tar.bz2 https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2
       tar -C $ROOT_DIR/third_party -xjf /tmp/busybox.tar.bz2
       mv $ROOT_DIR/third_party/busybox-${BUSYBOX_VERSION} $BUSYBOX_SRC
  3. Or set BUSYBOX_SRC=/path/to/your/busybox

Then re-run this script.
EOF
		exit 1
	fi
fi

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

apply_busybox_config_tweaks() {
	local cfg="$1"

	# Static binary (defconfig defaults to dynamic on some versions).
	sed -i.bak \
		-e 's/^CONFIG_STATIC=.*/CONFIG_STATIC=y/' \
		-e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
		"$cfg" 2>/dev/null || \
	sed -i '' \
		-e 's/^CONFIG_STATIC=.*/CONFIG_STATIC=y/' \
		-e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
		"$cfg"

	# tc applet: CBQ constants removed in Linux 6.8+ UAPI (busybox #15934).
	# Networking Utilities -> tc must be off when using modern kernel headers.
	sed -i.bak \
		-e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' \
		-e 's/^CONFIG_TC=m/# CONFIG_TC is not set/' \
		"$cfg" 2>/dev/null || \
	sed -i '' \
		-e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' \
		-e 's/^CONFIG_TC=m/# CONFIG_TC is not set/' \
		"$cfg"

	if [[ -x "$BUSYBOX_SRC/scripts/config" ]]; then
		KCONFIG_CONFIG="$cfg" "$BUSYBOX_SRC/scripts/config" --disable TC
	fi
}

if ! command -v "${CC}" >/dev/null 2>&1; then
	echo "ERROR: cross compiler not found: $CC" >&2
	exit 1
fi

mkdir -p "$ROOTFS_DIR/bin"
BB_BUILD="$ROOT_DIR/build/busybox-$ARCH"
mkdir -p "$BB_BUILD"

echo "Configuring busybox (static, no tc) for $ARCH ..."
make -C "$BUSYBOX_SRC" O="$BB_BUILD" distclean 2>/dev/null || true
make -C "$BUSYBOX_SRC" O="$BB_BUILD" defconfig
apply_busybox_config_tweaks "$BB_BUILD/.config"
# busybox 1.36 kconfig has no olddefconfig; `make` below runs silentoldconfig.

make -C "$BUSYBOX_SRC" O="$BB_BUILD" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
	CROSS_COMPILE="$CROSS_PREFIX" CC="$CC"

install -m 755 "$BB_BUILD/busybox" "$ROOTFS_DIR/bin/busybox"
(
	cd "$ROOTFS_DIR/bin"
	# Drop stale symlinks from a prior full --install run.
	for f in *; do
		if [[ "$f" != "busybox" ]]; then
			rm -f "$f"
		fi
	done

	# Demo default: a small applet set (ls demo + shell). Set BUSYBOX_FULL=1 for all applets.
	if [[ "${BUSYBOX_FULL:-0}" == "1" ]]; then
		for applet in $(./busybox --list); do
			if [[ "$applet" == "busybox" ]]; then
				continue
			fi
			ln -sf busybox "$applet"
		done
	else
		for applet in ls sh ash cat echo pwd true false test mkdir mount umount \
			readlink stat ln cp mv rm clear uname env sleep; do
			ln -sf busybox "$applet"
		done
	fi
)

echo "Installed busybox to $ROOTFS_DIR/bin/"
echo "  $(find "$ROOTFS_DIR/bin" -maxdepth 1 | wc -l | tr -d ' ') entries (busybox + applets)"
