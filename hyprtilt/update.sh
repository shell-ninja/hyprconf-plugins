#!/usr/bin/env bash
# ==============================================================================
# Hyprtilt - build / update helper for Hyprland 0.57+
#
# Safe by design:
#   * never edits Hyprland configs or keybinds
#   * never reloads or unloads anything in a running compositor
#   * default action is BUILD ONLY (nothing is installed)
#
# Usage:
#   ./update.sh                 build + verify only
#   ./update.sh --install       build + atomically install to the plugin dir
#   ./update.sh --install --load
#                               ...and load it into the running compositor
#   ./update.sh --help
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOME_DIR="${HOME:-$(getent passwd "$(whoami)" | cut -d: -f6)}"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME_DIR/.local/share}"

BIN_DIR="$XDG_DATA_HOME/hyprconf/plugins"
TARGET_SO="$BIN_DIR/hyprtilt.so"

BOLD='\033[1m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'
YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'

info() { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
err()  { echo -e "${RED}[ERROR]${NC} $*" >&2; }

DO_INSTALL=0
DO_LOAD=0

for arg in "$@"; do
    case "$arg" in
        --install) DO_INSTALL=1 ;;
        --load)    DO_LOAD=1 ;;
        -h|--help)
            sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            err "Unknown argument: $arg (try --help)"
            exit 2
            ;;
    esac
done

if [ "$DO_LOAD" = "1" ] && [ "$DO_INSTALL" != "1" ]; then
    err "--load requires --install"
    exit 2
fi

# ---- 1. Environment / version report -----------------------------------------
RUNNING_VER=""
if command -v hyprctl >/dev/null 2>&1; then
    RUNNING_VER="$(hyprctl version 2>/dev/null | head -1 | sed -E 's/^Hyprland ([^ ]+).*/\1/' || true)"
fi

BUILT_VER="$(pkg-config --modversion hyprland 2>/dev/null || true)"
if [ -z "$BUILT_VER" ]; then
    err "Hyprland development headers not found (pkg-config hyprland)."
    exit 1
fi

info "Building against Hyprland headers: $BUILT_VER"
if [ -n "$RUNNING_VER" ] && [ "$RUNNING_VER" != "$BUILT_VER" ]; then
    warn "Running Hyprland is $RUNNING_VER, headers are $BUILT_VER."
    warn "Hyprland rejects plugins built for a different ABI hash; restart Hyprland"
    warn "on $BUILT_VER before loading this build."
fi

# ---- 2. Build ----------------------------------------------------------------
info "Building hyprtilt..."
cd "$SCRIPT_DIR"
make -j"$(nproc)"
ok "Build complete: $SCRIPT_DIR/hyprtilt.so"

# ---- 3. Verify the shared object ---------------------------------------------
info "Verifying exports and symbol hygiene..."

for sym in pluginAPIVersion pluginExit pluginInit; do
    if ! nm -D --defined-only hyprtilt.so | awk -v s="$sym" '$NF == s { found = 1 } END { exit !found }'; then
        err "Missing required export: $sym"
        exit 1
    fi
done

# Inline globals must bind to the host compositor, so they have to stay weak.
if nm -D hyprtilt.so | awk '$2 == "u" { found = 1 } END { exit !found }'; then
    err "Found STB_GNU_UNIQUE symbols; build with -fno-gnu-unique."
    exit 1
fi

# No Hyprtilt symbol may be left unresolved.
if nm -D hyprtilt.so | awk '$2 == "U" && $3 ~ /Hyprtilt/ { found = 1 } END { exit !found }'; then
    err "Unresolved Hyprtilt symbols in hyprtilt.so:"
    nm -D hyprtilt.so | awk '$2 == "U" && $3 ~ /Hyprtilt/ { print "  " $3 }' >&2
    exit 1
fi

ok "Exports present, no unique or unresolved Hyprtilt symbols."

# ---- 4. Optional atomic install ----------------------------------------------
if [ "$DO_INSTALL" = "1" ]; then
    info "Installing to $TARGET_SO"
    mkdir -p "$BIN_DIR"

    # Atomic: copy to a temp inode, then rename over the target. Overwriting a
    # mapped .so in place can crash a compositor that still has it loaded.
    TMP_SO="$(mktemp "$BIN_DIR/.hyprtilt.so.XXXXXX")"
    install -m 755 hyprtilt.so "$TMP_SO"
    mv -f "$TMP_SO" "$TARGET_SO"

    # Drop the obsolete v3 compatibility symlink if it is still around.
    if [ -L "$BIN_DIR/hyprtilt-v3.so" ]; then
        rm -f "$BIN_DIR/hyprtilt-v3.so"
    fi

    ok "Installed: $TARGET_SO"
else
    info "Not installing (build-only mode). Re-run with --install to install."
fi

# ---- 5. Optional load into the running compositor ----------------------------
if [ "$DO_LOAD" = "1" ]; then
    if [ -z "$RUNNING_VER" ]; then
        warn "No running Hyprland session detected; skipping load."
    elif [ "$RUNNING_VER" != "$BUILT_VER" ]; then
        err "Refusing to load: running $RUNNING_VER, built for $BUILT_VER."
        err "Restart Hyprland first, then run: hyprctl plugin load $TARGET_SO"
        exit 1
    else
        info "Loading into the running compositor..."
        hyprctl plugin load "$TARGET_SO"
        ok "Loaded."
    fi
fi

# ---- 6. Summary --------------------------------------------------------------
echo ""
echo -e "${BOLD}${GREEN}Done.${NC}"
echo "  Plugin binary:  $SCRIPT_DIR/hyprtilt.so"
[ "$DO_INSTALL" = "1" ] && echo "  Installed at:   $TARGET_SO"
echo ""
echo "  Load:    hyprctl plugin load ${TARGET_SO}"
echo "  Toggle:  hyprctl hyprtilt toggle"
echo "  Status:  hyprctl hyprtilt status"
echo "  Debug:   hyprctl hyprtilt debug"
echo ""
echo "  Plugin options (Hyprland 0.57 Lua config):"
echo "    hl.config({ plugin = { hyprtilt = {"
echo "        enabled = true, max_tilt_deg = 3.0, spotlight = 0.5,"
echo "        edge_min = 0.0, opacity = 1.0 } } })"
echo ""
echo "  No Hyprland config files were modified by this script."
