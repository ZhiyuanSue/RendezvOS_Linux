#!/usr/bin/env bash
# Pack rootfs/ into newc cpio for Phase 4 initramfs.
#
# Usage (from repo root):
#   script/rootfs/build_cpio.sh
#   ARCH=x86_64 script/rootfs/build_cpio.sh
#
# Optional: run script/rootfs/build_busybox.sh first to populate rootfs/bin/.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS_DIR="${ROOTFS_DIR:-$ROOT_DIR/rootfs}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/build}"
OUT_CPIO="${OUT_CPIO:-$OUT_DIR/rootfs.cpio}"

if [[ ! -d "$ROOTFS_DIR" ]]; then
	echo "ERROR: rootfs directory missing: $ROOTFS_DIR" >&2
	exit 1
fi

mkdir -p "$OUT_DIR"

TMP_CPIO="${OUT_CPIO}.tmp.$$"

# newc cpio (same family as Linux initramfs). Write via temp file so a failed
# pack never leaves a zero-byte rootfs.cpio that Make will treat as up to date.
(
	cd "$ROOTFS_DIR"
	find . -print0 | LC_ALL=C sort -z | cpio -o -H newc --null
) >"$TMP_CPIO"

if [[ ! -s "$TMP_CPIO" ]]; then
	rm -f "$TMP_CPIO"
	echo "ERROR: cpio pack produced empty output (is 'cpio' installed?)" >&2
	exit 1
fi

mv -f "$TMP_CPIO" "$OUT_CPIO"

echo "Wrote $OUT_CPIO ($(wc -c <"$OUT_CPIO" | tr -d ' ') bytes)"
echo "Contents (first 32 paths):"
cpio -t <"$OUT_CPIO" 2>/dev/null | head -32
if [[ $(cpio -t <"$OUT_CPIO" 2>/dev/null | wc -l | tr -d ' ') -gt 32 ]]; then
	echo "... (truncated)"
fi
