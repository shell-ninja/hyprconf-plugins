#pragma once

// ==============================================================================
// hyprtilt: Compat header isolating internal Hyprland & Wayland headers
// Targeting Hyprland 0.57+ (native hyprland.lua & aquamarine architecture)
// ==============================================================================

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>

// Desktop view & window state
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>

// Rendering, framebuffers & transformers
#include <hyprland/src/render/transformer/Transformer.hpp>
#include <hyprland/src/render/transformer/TransformerList.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>

// Outputs (monitor scale), input, layouts & fullscreen
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

// Event bus & logging
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

// Lua config value types (Hyprland 0.55+)
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>

// OpenGL ES 3.20 & EGL
#include <GLES3/gl32.h>
#include <EGL/egl.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// Ensure math constants
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
