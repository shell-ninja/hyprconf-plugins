# Hyprconf Plugins

The official plugin repository for the [Hyprconf](https://github.com/shell-ninja/hyprconf) desktop environment.

---

## Plugins

| Plugin | Category | Description |
|---|---|---|
| [code-theme-sync](./code-theme-sync/) | Theming | Harmonizes VS Code, VSCodium, and Antigravity IDE themes and colors with the active wallpaper palette. |

---

## Installation

Plugins are browsed, installed, and managed directly from the **Hyprconf Settings Studio** (`Super + Shift + S` -> Plugins).

To install manually, download or drag and drop a plugin directory into the Settings Studio window.

---

## Plugin Development

Each plugin resides in its own folder with a `plugin.json` manifest:

```json
{
  "id": "my-plugin",
  "name": "My Plugin",
  "version": "1.0.0",
  "description": "Description of what this plugin does.",
  "author": "your-name",
  "icon": "code",
  "category": "Theming",
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

To contribute a plugin, fork this repository, add your plugin folder, add an entry to `index.json`, and open a pull request.
