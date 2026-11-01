# hyprtilt — 3D Perspective Card Hover Effect for Hyprland

A C++23 Hyprland plugin that turns ordinary 2D windows into responsive 3D
surfaces. Every eligible window keeps a subtle static lean while the plugin is
enabled, and hovering a window makes the quadrant under the pointer recede into
the screen with a damped spring, subtle perspective foreshortening, and an
optional uniform shrink. When the pointer leaves, the window springs back to
its static lean rather than to flat.

The plugin targets the Hyprland 0.57 API and OpenGL ES 3.2. It is built against
Hyprland 0.57.0 and refuses to load into a compositor with a different API hash.

---

## How it works

`hyprtilt` is implemented as a native `Render::IWindowTransformer`. Hyprland
renders a transformed window into a monitor-sized work buffer, passes that
buffer to the transformer, and draws the returned texture back over the
monitor. This avoids trampolines and private renderer patches.

The transformer:

1. Reads the current spring state for the window.
2. Computes the window's position on the monitor, including workspace slide,
   floating offset, and monitor scale.
3. Warps the monitor-sized work buffer with a small perspective shader.
4. Normalises the projected quadrilateral's bounding box back onto the
   window's original box, so the card stays centred and does not drift or
   change apparent size as it tilts.
5. Feathers the transformed window edge by one pixel to keep it antialiased.
6. Returns a monitor-sized ARGB8888 texture that Hyprland composites normally.

Output canvases are cached by pixel size and double-buffered, so the normal
window pass and the blur-matte pass cannot overwrite each other. The cache is
bounded to two sizes (LRU-evicted) and released when the plugin unloads.

---

## Features

- **Reverse tilt:** the hovered quadrant moves away from the viewer rather
  than toward it.
- **Persistent 3D lean:** while the plugin is enabled every eligible window
  keeps a static tilt (`idle_tilt`), even when the pointer is nowhere near it.
  Hover adds the dynamic cursor-following lean on top.
- **Size-aware deflection:** large windows use a smaller maximum angle:
  `effectiveTilt = max_tilt_deg * clamp(800 / max(W, H), 0.35, 1.0)`.
- **Spring physics:** semi-implicit Euler integration with 2 ms substeps, so
  motion is frame-rate independent and stable.
- **Weak window references:** per-window state is keyed by `PHLWINDOWREF` and
  removed when the window is destroyed.
- **Targeted damage:** while a spring is moving, the plugin damages only the
  affected windows. Once every spring is at rest, no damage requests are made.
  The compositor therefore does not redraw continuously because of this plugin.
- **Automatic bypass:** tilt eases back to flat when the window is fullscreen
  or maximized, the user is dragging or resizing, a popup belonging to the
  window is open, the window class is excluded, or its tiled/floating category
  is disabled in the config.
- **Native Lua API:** `hl.plugin.hyprtilt.toggle()`,
  `hl.plugin.hyprtilt.set_enabled(bool)`, and
  `hl.plugin.hyprtilt.is_enabled()`.
- **Shell command:** `hyprctl hyprtilt [toggle|on|off|status]`.
- **Border spotlight:** a soft additive glow along the window's outer edge,
  controlled by `plugin:hyprtilt:spotlight`. It does not change the window's
  opacity and never lets the wallpaper bleed through.

---

## Mathematical model

### 1. Normalized cursor offset

For a window with center `(cx, cy)` and size `(W, H)`:

```text
nx = clamp((mouseX - cx) / (W / 2), -1, 1)
ny = clamp((mouseY - cy) / (H / 2), -1, 1)
```

### 2. Target angles

```text
effectiveTilt = max_tilt_deg * clamp(800 / max(W, H), 0.35, 1.0)

targetRotY = -nx * effectiveTilt
targetRotX =  ny * effectiveTilt
```

Hovering the top-right corner (`nx > 0`, `ny < 0`) gives negative rotations on
both axes. The shader uses those signs so the top-right area recedes while the
opposite edges move toward the viewer.

### 3. Perspective coefficients

With the configured camera distance `d = perspective`, and physical window
half-size `(hw, hh)` in monitor pixels:

```text
Dx = max(d / hw, 0.5)
Dy = max(d / hh, 0.5)

kx = -sin(rotY) / Dx
ky =  sin(rotX) / Dy
```

### 4. Shader warp

For each monitor pixel, `screenUV` is the normalized monitor coordinate. The
shader converts it to window-relative coordinates, maps that aligned position
back into raw perspective space, inverts the projection, and samples the
source texture:

```text
u_dst = (screenUV - center) / halfSize
raw   = normCenter + u_dst * normHalf / scale
u_src = raw / (1 - dot(k, raw))
srcUV = center + u_src * halfSize
```

`normCenter` and `normHalf` are computed on the CPU from the four raw projected
corners. They map the perspective quadrilateral's bounding box exactly onto the
original window box, so the card stays centred and its outer extents do not
drift or change size while tilting. `scale` is an optional uniform shrink; it
defaults to `1.0`, which leaves the window aligned. Values below `1.0` make the
whole card recede slightly, and values above `1.0` make it grow.

The shader then computes a one-pixel edge feather from the source footprint:

```text
edgeDist   = (1 - abs(u_src)) * (hw, hh)
edgeFactor = clamp(min(edgeDist.x, edgeDist.y), 0, 1)
```

and writes a premultiplied, edge-feathered result:

```text
rgb = unpremultiply(texture(srcUV).rgb) * brightness_boost
out = vec4(rgb, 1) * edgeFactor
```

This is a lightweight, first-order perspective approximation designed for the
small angles used by the plugin. It is not a full projective homography, but it
produces the expected card-like recession without a matrix decomposition on
every frame.

### 5. Spring integration

Each axis is integrated with:

```text
force = -k * (value - target) - c * velocity
velocity += force * h
value    += velocity * h
```

where `h` is a 2 ms substep. The simulation uses the same flat thresholds as
the damage scheduler:

- rotation: `0.02°`
- scale: `0.001`
- rotation velocity: `0.05°/s`
- scale velocity: `0.005/s`

---

## Building

### Prerequisites

- Hyprland development headers (`pkg-config hyprland`)
- GCC 13+ or Clang 17+ with C++23 support
- `pkg-config`, `make`, OpenGL ES 3.2 (`libGLESv2`), and EGL (`libEGL`)

The Hyprland `pkg-config` file pulls in the remaining compile dependencies
(`libdrm`, Cairo, Aquamarine, and related libraries).

### Make

```bash
cd ~/hyprtilt
make -j"$(nproc)"
make install
```

`make install` copies `hyprtilt.so` to
`~/.local/share/hyprconf/plugins/hyprtilt.so` and creates the compatibility
symlink `hyprtilt-v3.so`.

Use `PREFIX=/some/path make install` to install somewhere else, and
`make uninstall` to remove the installed files.

### CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake --install build
```

With the default install prefix, CMake installs to
`~/.local/share/hyprconf/plugins`. An explicit prefix installs to
`<prefix>/lib/hyprland`. Override the destination directly with:

```bash
cmake -S . -B build -DHYPRTILT_INSTALL_DIR="$HOME/.local/share/hyprconf/plugins"
```

A `compile_commands.json` is included at the repository root for clangd and
other language servers.

---

## Configuration

The plugin works with its built-in defaults even if no config file exists.
To customize it, add a `hyprtilt` table to any Lua config that Hyprland loads.
For example, create `~/.config/hypr/hyprtilt.lua` and load it from
`hyprland.lua`:

```lua
pcall(require, "hyprtilt")
```

```lua
hl.config({
    plugin = {
        hyprtilt = {
            enabled          = true,            -- master switch
            max_tilt_deg     = 3.0,             -- 0.0 .. 30.0
            hover_scale      = 1.0,             -- 1.0 = aligned, <1.0 shrinks the card
            perspective      = 1000.0,          -- camera distance, pixels
            stiffness        = 580.0,           -- spring constant k
            damping          = 34.0,            -- spring damping c
            brightness_boost = 1.0,             -- 1.0 = unchanged
            idle_tilt        = 0.7,             -- always-on lean, 0.0 .. 1.0
            spotlight        = 0.5,             -- border glow, 0.0 .. 1.0
            edge_min         = 0.0,             -- 0 = feathered edge, 1 = hard edge
            opacity          = 1.0,             -- keep 1.0 to preserve Hyprland opacity
            tiled_windows    = true,
            floating_windows = true,
            exclude_classes  = "",              -- comma-separated substrings
        },
    },
})
```

`exclude_classes` is a comma-separated list of case-insensitive substrings
matched against the window class. For example, `"steam,gamescope"` excludes
both classes.

`brightness_boost` values above `1.0` brighten the tilted window but can clip
highlights; the default `1.0` leaves colors unchanged. Damping is clamped to a
minimum of `1.0` so an undamped spring cannot keep the damage loop alive
forever.

`idle_tilt` keeps the 3D effect alive without hover. It is a fraction of
`max_tilt_deg` applied to every eligible window while the plugin is enabled.
The lean direction follows the window's position on its monitor: windows on the
left lean one way and windows on the right lean the other, so a desktop reads
as a 3D scene instead of every card tilting identically. A window near the
centre of its monitor still keeps at least 60% of the lean. `0.0` restores the
old hover-only behaviour; `0.7` is the default.

`spotlight` adds a soft glow along the window's outer edge only. It never
touches the interior or the alpha channel, so the window stays fully opaque and
the wallpaper cannot bleed through. `0.0` disables it; `0.5` is the default.
`edge_min` controls the one-pixel antialiasing feather: `0.0` keeps the soft
edge, `1.0` forces a hard edge. `opacity` multiplies the output alpha and
defaults to `1.0`, which preserves Hyprland's own window opacity; lowering it
makes the whole window translucent.

To change an option at runtime, use the Lua config API. In Hyprland 0.57
`hyprctl keyword` does not understand plugin options, but `hyprctl eval` does:

```bash
hyprctl eval 'hl.config({ plugin = { hyprtilt = { spotlight = 1.0 } } })'
hyprctl getoption plugin:hyprtilt:spotlight
```

### Runtime API

```lua
hl.plugin.hyprtilt.toggle()          -- returns the effective enabled state
hl.plugin.hyprtilt.set_enabled(true) -- returns the effective enabled state
hl.plugin.hyprtilt.is_enabled()      -- returns the effective enabled state
```

The shell command works from `hyprland.conf` binds or any script:

```bash
hyprctl hyprtilt toggle   # returns "enabled" or "disabled"
hyprctl hyprtilt on
hyprctl hyprtilt off
hyprctl hyprtilt status
```

Hyprland 0.57 removed plugin dispatchers (`addDispatcher`/`addDispatcherV2`
are no-ops), so `hyprctl dispatch hyprtilt:toggle` does not work. Use the
socket command above instead.

The config `enabled` value is the master switch. The runtime override cannot
force the plugin on while the master switch is off.

---

## Installation

The scripts never edit Hyprland configs or keybinds, and they never reload or
unload anything in a running compositor unless you explicitly ask.

### Build only (safe default)

```bash
cd ~/hyprtilt
./update.sh
```

This builds `hyprtilt.so`, verifies the required exports, and checks that the
shared object has no unresolved Hyprtilt symbols or `STB_GNU_UNIQUE` entries.
Nothing is installed.

### Build and install

```bash
./update.sh --install          # atomic install to the plugin directory
./update.sh --install --load   # ...and load it into the running compositor
```

`install.sh` is the same installer with a friendlier summary:

```bash
./install.sh                  # build + install
./install.sh --no-install     # build only
./install.sh --install-dir DIR
```

The install is atomic: the new binary is written to a temporary inode and
renamed over the target, so a compositor that still has the old `.so` mapped
never sees a truncated file.

### Manual load

```bash
hyprctl plugin load "$HOME/.local/share/hyprconf/plugins/hyprtilt.so"
hyprctl plugin list
hyprctl eval 'return hl.plugin.hyprtilt.is_enabled()'
```

### Keybind

Add it yourself; the scripts will not touch your config:

```lua
hl.bind("SUPER + T", function()
    if hl.plugin and hl.plugin.hyprtilt then
        hl.plugin.hyprtilt.toggle()
    end
end)
```

### Uninstall

```bash
./uninstall.sh
```

The uninstaller unloads the plugin from the running session and removes the
installed binaries. It leaves your config and keybinds untouched.

---

## Known limitations

- **XRGB work buffer:** Hyprland's transformer input format is XRGB8888 with no
  usable alpha channel. The plugin forces its output to ARGB8888, reconstructs
  premultiplied colour, and keeps the source alpha so Hyprland's own window
  opacity survives. The one-pixel edge feather is antialiasing, not a black
  border; `edge_min` can harden it if a thin dark line is visible. The desktop
  backdrop behind a genuinely transparent window still cannot be reconstructed
  from the work buffer.
- **Border spotlight:** `spotlight` is an additive rim glow drawn inside the
  window's outer edge. It is not a Hyprland decoration border, and it is
  invisible on pure-white content because the glow is additive.
- **Monitor rotation:** window coordinates currently assume an unrotated
  output. On rotated monitors the tilt region may be offset from the window.
- **Popups:** Hyprland's transformer API only affects the main window pass, so
  popups and context menus are not transformed.
- **Brightness clipping:** `brightness_boost > 1.0` can clip bright content.
- **ABI matching:** the plugin checks Hyprland's API hash at load time. A
  plugin built against a different Hyprland commit will refuse to load.

---

## Troubleshooting

Check that the plugin is loaded:

```bash
hyprctl plugin list
```

Check the effective state:

```bash
hyprctl eval 'return hl.plugin.hyprtilt.is_enabled()'
```

Read Hyprland's log for load or shader errors:

```bash
cat "$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/hyprland.log"
```

Reload after rebuilding:

```bash
hyprctl plugin unload "$HOME/.local/share/hyprconf/plugins/hyprtilt.so"
hyprctl plugin load "$HOME/.local/share/hyprconf/plugins/hyprtilt.so"
```

If the keybind does nothing, confirm that the bind you added actually calls
`hl.plugin.hyprtilt.toggle()`, that the plugin is loaded (`hyprctl plugin list`),
and that the ABI hash matches the running Hyprland version.

---

## Repository layout

```text
~/hyprtilt/
├── update.sh              # safe build / optional install helper (no config edits)
├── install.sh             # build + atomic install (no config edits)
├── uninstall.sh           # unload and remove binaries (no config edits)
├── Makefile               # standalone Make build with dependency tracking
├── CMakeLists.txt         # CMake build configuration
├── .clangd                # clangd C++23 configuration
├── .gitignore             # build and editor artifacts
├── compile_commands.json  # compilation database for language servers
├── README.md
└── src/
    ├── compat.hpp         # isolated Hyprland and GLES headers
    ├── globals.hpp        # shared handles, config values, helpers
    ├── Spring.hpp         # spring integration and settle thresholds
    ├── TiltTransformer.hpp# transformer declaration and per-monitor FBO cache
    ├── TiltTransformer.cpp# GLES perspective shader and framebuffer pipeline
    └── main.cpp           # lifecycle, hooks, hover detection, config and Lua API
```
