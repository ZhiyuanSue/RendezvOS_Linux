#!/usr/bin/env bash
# Install one user test ELF into rootfs/tests/ for cpio pilot / smoke.
#
# Usage (after user ELFs exist):
#   ARCH=x86_64 script/rootfs/install_test_elf.sh test_echo
#
# Full suite: make user ARCH=x86_64  (packs all tests via pack_user_rootfs.py)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
TEST_NAME="${1:-test_echo}"

ELF_SRC="$ROOT_DIR/user_payload/user/build/${ARCH}/${TEST_NAME}"
ROOTFS_TESTS="$ROOT_DIR/rootfs/tests"
MANIFEST="$ROOTFS_TESTS/manifest"

if [[ ! -f "$ELF_SRC" ]]; then
	echo "ERROR: ELF not found: $ELF_SRC" >&2
	echo "Run: make user ARCH=${ARCH}" >&2
	exit 1
fi

mkdir -p "$ROOTFS_TESTS"
install -m 755 "$ELF_SRC" "$ROOTFS_TESTS/${TEST_NAME}"

cat >"$MANIFEST" <<EOF
# Pilot manifest — single test
/tests/${TEST_NAME}
EOF

echo "Installed $ROOTFS_TESTS/${TEST_NAME}"
echo "Wrote $MANIFEST"
echo "Next: make rootfs ARCH=${ARCH} && make build ARCH=${ARCH} run"
