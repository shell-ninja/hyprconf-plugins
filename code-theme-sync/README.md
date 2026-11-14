# Code Theme Synchronizer (`code-theme-sync`)

A native plugin for **Hyprconf** that synchronizes **VS Code**, **VSCodium**, and **Antigravity IDE** themes and colors with active wallpaper obsidian palettes and accent tones.

---

## Features

- **Zero-Restart Live Injection**: Injects theme customizations into `settings.json` via `workbench.colorCustomizations` and `editor.tokenColorCustomizations`. Changes appear instantly in open editor windows.
- **Obsidian Glass Surfaces**: Replaces flat gray chrome with deep obsidian glass surfaces (`#0a0a0d`, `#111116`, `#16161e`) and subtle glass rims (`accent` with 25% alpha).
- **Wallpaper Harmony**: Automatically applies your active wallpaper's primary accent, secondary tone, and terminal ANSI colors.
- **Safe & Non-Destructive**: Backs up your original `settings.json` before any modifications. You can revert back to your original setup anytime with `--restore`.
- **Broad Target Support**:
  - Antigravity IDE (`~/.config/Antigravity IDE/User/settings.json`)
  - VSCodium (`~/.config/VSCodium/User/settings.json`)
  - VS Code (`~/.config/Code/User/settings.json`)
  - Code - OSS, Cursor, OpenCode, and Windsurf

---

## CLI Usage

```bash
# Sync active colors to all detected editors
python3 ~/.hyprconf/plugins/code-theme-sync/bin/sync-theme.py --sync

# Check status and detected installations
python3 ~/.hyprconf/plugins/code-theme-sync/bin/sync-theme.py --status

# Restore original themes
python3 ~/.hyprconf/plugins/code-theme-sync/bin/sync-theme.py --restore

# Enable / Disable plugin
python3 ~/.hyprconf/plugins/code-theme-sync/bin/sync-theme.py --enable
python3 ~/.hyprconf/plugins/code-theme-sync/bin/sync-theme.py --disable
```

---

## Configuration

Stored in `~/.local/share/hyprconf/plugins/code-theme-sync/config.json`:

```json
{
  "enabled": true,
  "mode": "hybrid",
  "targets": ["antigravity", "vscodium", "vscode"],
  "sync_ui": true,
  "sync_syntax": true,
  "sync_terminal": true,
  "obsidian_depth": "deep"
}
```
