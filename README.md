# hyprconf-plugins

A collection of plugins for my hyprconf setup.

## Plugins

| Plugin | Description |
| --- | --- |
| [hyprtilt](hyprtilt/README.md) | 3D perspective card tilt for Hyprland 0.57+ windows. Every window keeps a persistent idle lean; hovering adds a cursor-following tilt, with a border spotlight and spring physics. |

## Building

Each plugin is self-contained and has its own build instructions in its
`README.md`. The scripts never edit Hyprland configs or keybinds.

```bash
cd hyprtilt
./install.sh          # build and install
./update.sh --install # rebuild and install atomically
```
