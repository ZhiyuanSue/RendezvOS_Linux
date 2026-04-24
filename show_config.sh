#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 1) Always show core-local summary (core writes into core/build/).
"$SCRIPT_DIR/core/show_config.sh"

echo ""
echo "root_config_file	=	$SCRIPT_DIR/script/config/root_${ARCH:-unknown}.json"
echo "root_env_file	=	$SCRIPT_DIR/Makefile.root.env"
echo "config_summary	=	$SCRIPT_DIR/build/config_summary.txt"
echo ""

# 2) Show integrated summary at repo root (writes into build/).
python3 "$SCRIPT_DIR/core/script/config/config_summary.py" "$SCRIPT_DIR/core" "$SCRIPT_DIR/build" "$SCRIPT_DIR/Makefile.root.env" >/dev/null
cat "$SCRIPT_DIR/build/config_summary.txt"

