#!/usr/bin/env bash
# install.sh — Installs and enables the Code Theme Synchronizer plugin for Hyprconf

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYNC_PY="$SCRIPT_DIR/bin/sync-theme.py"
PLUGIN_STATE_DIR="$HOME/.local/share/hyprconf/plugins/code-theme-sync"

echo "==> Installing Code Theme Synchronizer..."

mkdir -p "$PLUGIN_STATE_DIR/backup"

chmod +x "$SYNC_PY"

# Run initial sync
python3 "$SYNC_PY" --enable
python3 "$SYNC_PY" --sync

echo "==> Code Theme Synchronizer installed and active!"
