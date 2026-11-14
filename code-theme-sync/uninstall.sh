#!/usr/bin/env bash
# uninstall.sh — Disables and cleanly removes Code Theme Synchronizer modifications

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYNC_PY="$SCRIPT_DIR/bin/sync-theme.py"

echo "==> Uninstalling Code Theme Synchronizer..."

if [[ -f "$SYNC_PY" ]]; then
    python3 "$SYNC_PY" --restore
    python3 "$SYNC_PY" --disable
fi

echo "==> Restored original editor configurations."
