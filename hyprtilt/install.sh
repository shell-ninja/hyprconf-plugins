#!/usr/bin/env bash
# ==============================================================================
# Hyprtilt - 3D perspective card tilt plugin for Hyprland 0.57+
#
# Safe by design:
#   * never edits Hyprland configs or keybinds
#   * never reloads a running compositor
#   * installs the .so atomically
#
# Usage:
#   ./install.sh                 build and install
#   ./install.sh --no-install    build only
#   ./install.sh --install-dir DIR
#   ./install.sh --help
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOME_DIR="${HOME:-$(getent passwd "$(whoami)" | cut -d: -f6)}"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME_DIR/.local/share}"

INSTALL_BIN_DIR="$XDG_DATA_HOME/hyprconf/plugins"
TARGET_SO=""
DO_INSTALL=1

BOLD='\033[1m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'
YELLOW='\033[1;33m'; RED='\033[0;31m'; CYAN='\033[0;36m'; NC='\033[0m'

info() { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
err()  { echo -e "${RED}[ERROR]${NC} $*" >&2; }

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-install)   DO_INSTALL=0; shift ;;
        --install-dir)
            [ -n "${2:-}" ] || { err "--install-dir needs a path"; exit 2; }
            INSTALL_BIN_DIR="$2"; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *)              err "Unknown argument: $1 (try --help)"; exit 2 ;;
    esac
done

TARGET_SO="$INSTALL_BIN_DIR/hyprtilt.so"

echo -e "${BOLD}${BLUE}Hyprtilt installer (Hyprland 0.57+)${NC}"
echo ""

# ---- 1. Build environment ----------------------------------------------------
info "Checking build environment..."
for tool in pkg-config make; do
    command -v "$tool" >/dev/null 2>&1 || { err "$tool is required."; exit 1; }
done
pkg-config --exists hyprland || { err "Hyprland development headers not found."; exit 1; }

CXX_BIN="${CXX:-}"
if [ -z "$CXX_BIN" ]; then
    if command -v g++ >/dev/null 2>&1; then CXX_BIN=g++
    elif command -v clang++ >/dev/null 2>&1; then CXX_BIN=clang++
    else err "A C++23 compiler (g++ or clang++) is required."; exit 1
    fi
fi
ok "Hyprland headers $(pkg-config --modversion hyprland); compiler $CXX_BIN."

# ---- 2. Build ----------------------------------------------------------------
info "Compiling hyprtilt..."
cd "$SCRIPT_DIR"
make clean >/dev/null 2>&1 || true
make CXX="$CXX_BIN" -j"$(nproc)"
ok "Built $SCRIPT_DIR/hyprtilt.so"

# ---- 3. Install --------------------------------------------------------------
if [ "$DO_INSTALL" = "1" ]; then
    info "Installing to $TARGET_SO"
    mkdir -p "$INSTALL_BIN_DIR"

    # Atomic install: write a new inode and rename over the target so a running
    # compositor never sees a truncated, half-written shared object.
    TMP_SO="$(mktemp "$INSTALL_BIN_DIR/.hyprtilt.so.XXXXXX")"
    install -m 755 hyprtilt.so "$TMP_SO"
    mv -f "$TMP_SO" "$TARGET_SO"

    if [ -L "$INSTALL_BIN_DIR/hyprtilt-v3.so" ]; then
        rm -f "$INSTALL_BIN_DIR/hyprtilt-v3.so"
    fi

    ok "Installed $TARGET_SO"
else
    info "Build-only mode; nothing installed."
fi

# ---- 4. Version check --------------------------------------------------------
BUILT_VER="$(pkg-config --modversion hyprland)"
RUNNING_VER="$(hyprctl version 2>/dev/null | head -1 | sed -E 's/^Hyprland ([^ ]+).*/\1/' || true)"
if [ -n "$RUNNING_VER" ] && [ "$RUNNING_VER" != "$BUILT_VER" ]; then
    warn "Running Hyprland is $RUNNING_VER but this build targets $BUILT_VER."
    warn "Hyprland enforces an ABI hash; restart Hyprland on $BUILT_VER before loading."
fi

# ---- 5. Next steps -----------------------------------------------------------
echo ""
echo -e "${BOLD}${GREEN}Finished. No Hyprland config files were modified.${NC}"
echo ""
echo "  Load:    hyprctl plugin load $TARGET_SO"
echo "  Toggle:  hyprctl hyprtilt toggle"
echo "  Status:  hyprctl hyprtilt status"
echo "  Debug:   hyprctl hyprtilt debug"
echo ""
echo "  Optional keybind (add it yourself, the installer will not edit configs):"
echo "    hl.bind(\"SUPER + T\", function()"
echo "        if hl.plugin and hl.plugin.hyprtilt then"
echo "            hl.plugin.hyprtilt.toggle()"
echo "        end"
echo "    end)"
echo ""
echo "  Plugin options (Hyprland 0.57 Lua config):"
echo "    hl.config({ plugin = { hyprtilt = {"
echo "        enabled = true, max_tilt_deg = 3.0, spotlight = 0.5,"
echo "        edge_min = 0.0, opacity = 1.0 } } })"
