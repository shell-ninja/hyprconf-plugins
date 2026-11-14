# Hyprconf Plugins (`hyprconf-plugins`)

The official community and verified plugin catalog for the **[Hyprconf](https://github.com/shell-ninja/hyprconf)** obsidian-themed desktop environment.

---

## 📦 Official Plugins

| Plugin | Category | Description |
|---|---|---|
| **[code-theme-sync](./code-theme-sync/)** | Theming & Integrations | Harmonizes VS Code, VSCodium, and Antigravity IDE themes and colors with active wallpaper obsidian palettes and accent tones. |

---

## 🎨 Installing & Managing Plugins

Plugins are managed directly from the **Hyprconf Settings Studio**:

1. Open Settings Studio (`Super + Shift + S` or via Dynamic Island / Top Bar).
2. Navigate to the **Plugins** section.
3. Browse the official catalog, preview plugin details, and install with a single click.
4. Toggle plugins on or off with instant zero-restart state switching.

For headless environments and automation, the native CLI helper is also available:

```bash
# List available and installed plugins
hyprconf-plugin list

# Install a plugin from the registry
hyprconf-plugin install <plugin-id>

# Toggle on or off
hyprconf-plugin enable <plugin-id>
hyprconf-plugin disable <plugin-id>

# Uninstall
hyprconf-plugin remove <plugin-id>
```

---

## 🛠️ Authoring a Plugin

Every plugin in this repository is a self-contained directory with a standard `plugin.json` manifest:

```json
{
  "id": "my-plugin",
  "name": "My Plugin",
  "version": "1.0.0",
  "description": "Short summary of what this plugin adds to Hyprconf...",
  "author": "your-github-username",
  "icon": "sparkles",
  "category": "Widgets",
  "entry": "bin/run.sh",
  "hooks": {
    "on_color_change": "bin/sync.py",
    "on_wallpaper_change": "bin/sync.py"
  },
  "settings": {
    "enabled": true
  }
}
```

### Contribution Workflow
1. Fork this repository.
2. Add your plugin folder under `./<your-plugin-id>/`.
3. Register your plugin in [`index.json`](./index.json).
4. Submit a Pull Request!
