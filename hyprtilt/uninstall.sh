#!/usr/bin/env bash
# ==============================================================================
# Hyprtilt - uninstaller
#
# Removes the plugin binary and unloads it from the running compositor.
# It does NOT edit Hyprland configs or keybinds; any bind you added stays put
# and simply becomes a no-op until you remove it yourself.
# ==============================================================================

set -euo pipefail

HOME_DIR="${HOME:-$(getent passwd "$(whoami)" | cut -d: -f6)}"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME_DIR/.local/share}"
INSTALL_BIN_DIR="$XDG_DATA_HOME/hyprconf/plugins"
TARGET_SO="$INSTALL_BIN_DIR/hyprtilt.so"

GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; NC='\033[0m'
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

# Resolve the running session signature without guessing if it is exported.
HYPR_SIG="${HYPRLAND_INSTANCE_SIGNATURE:-}"
if [ -z "$HYPR_SIG" ]; then
    HYPR_SIG="$(ls -t /run/user/"$(id -u)"/hypr/ 2>/dev/null | head -1 || true)"
fi

if command -v hyprctl >/dev/null 2>&1 && [ -n "$HYPR_SIG" ]; then
    info "Unloading hyprtilt from session $HYPR_SIG..."
    HYPRLAND_INSTANCE_SIGNATURE="$HYPR_SIG" hyprctl plugin unload "$TARGET_SO" >/dev/null 2>&1 || true
fi

info "Removing installed binaries..."
rm -f "$TARGET_SO" \
      "$INSTALL_BIN_DIR/hyprtilt-v3.so" \
      "$INSTALL_BIN_DIR/hyprtilt-v2.so" \
      "$INSTALL_BIN_DIR/hyprtilt-v1.so"

ok "Plugin removed from $INSTALL_BIN_DIR"

echo ""
warn "Your Hyprland config was not modified."
echo "  If you added a hyprtilt keybind or a plugin:hyprtilt config table,"
echo "  remove it yourself. A leftover keybind only calls a missing function"
echo "  and is harmless."
echo ""
echo "  Removed: $TARGET_SO"
