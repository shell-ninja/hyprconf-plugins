#include "compat.hpp"
#include "globals.hpp"
#include "Spring.hpp"
#include "TiltTransformer.hpp"

#include <chrono>
#include <cmath>
#include <format>
#include <string_view>
#include <vector>

using namespace Hyprtilt;

static CHyprSignalListener s_preRenderListener;
static CHyprSignalListener s_mouseMoveListener;
static CHyprSignalListener s_windowDestroyListener;
static CHyprSignalListener s_configReloadListener;

static std::chrono::steady_clock::time_point s_lastFrameTime = std::chrono::steady_clock::now();

static CTiltTransformer* getTransformer(const PHLWINDOW& window) {
    if (!window)
        return nullptr;

    return window->effects().transformers()->get<CTiltTransformer>();
}

static void attachTransformer(const PHLWINDOW& window) {
    if (!window)
        return;

    // Reuse the existing transformer if one is already attached but inactive.
    // Hyprland only drops transformers through removeInactive(), so blindly
    // emplacing a new one would leak the old instance for every hover.
    if (auto* existing = getTransformer(window)) {
        existing->setActive(true);
        return;
    }

    window->effects().transformers()->emplace<CTiltTransformer>(window->m_self);
}

// Deactivate rather than destroy: CWindowTransformerList skips inactive
// transformers and Hyprland removes them on its next removeInactive() sweep.
static void detachTransformer(const PHLWINDOW& window) {
    if (auto* transformer = getTransformer(window))
        transformer->setActive(false);
}

static bool hasTransformer(const PHLWINDOW& window) {
    return getTransformer(window) != nullptr;
}

// Immediately detach every transformer. Used when the plugin is unloaded.
// removeInactive() is important here: the transformer objects must be destroyed
// before dlclose(), otherwise the window list keeps vtables pointing into the
// unloaded plugin and the next frame crashes.
static void resetAllTilt() {
    std::scoped_lock lock(s_stateMutex);

    if (Desktop::windowState()) {
        for (const auto& window : Desktop::windowState()->windows()) {
            if (!window || !hasTransformer(window))
                continue;

            detachTransformer(window);
            window->effects().transformers()->removeInactive();

            if (g_pHyprRenderer)
                g_pHyprRenderer->damageWindow(window, true);
        }
    }

    CTiltTransformer::s_springStates.clear();
}

// Schedule a frame for every tracked window so the springs can ease back to
// flat after the plugin is disabled. Unlike resetAllTilt(), this keeps the
// transformers attached and lets the animation finish smoothly.
static void easeOutAll() {
    std::scoped_lock lock(s_stateMutex);
    if (!g_pHyprRenderer)
        return;

    for (auto& [windowRef, spring] : CTiltTransformer::s_springStates) {
        if (auto window = windowRef.lock()) {
            if (auto mon = window->m_monitor.lock())
                mon->scheduleFrame();
            g_pHyprRenderer->damageWindow(window, true);
        }
    }
}

// Damage every mapped window so the idle tilt animates in immediately when the
// plugin is enabled, even if the cursor never moves afterwards.
static void damageAllWindows() {
    if (!g_pHyprRenderer || !Desktop::windowState())
        return;

    for (const auto& window : Desktop::windowState()->windows()) {
        if (window && window->mapped()) {
            if (auto mon = window->m_monitor.lock())
                mon->scheduleFrame();
            g_pHyprRenderer->damageWindow(window, true);
        }
    }
}

static bool isWindowBypassed(const PHLWINDOW& window) {
    if (!window)
        return true;

    // 1. Actively dragging or resizing a window
    if (g_layoutManager && g_layoutManager->dragController() && g_layoutManager->dragController()->mode() != MBIND_INVALID) {
        return true;
    }
    // 2. Fullscreen or maximized. FSMODE_MAXIMIZED is distinct from
    //    FSMODE_FULLSCREEN, so inspect the full mode struct.
    if (Fullscreen::controller() && Fullscreen::controller()->getFullscreenModes(window).internal != Fullscreen::FSMODE_NONE) {
        return true;
    }
    // 3. XDG Popup / context menu open on window
    if (window->popupMappedTreeSize() > 0) {
        return true;
    }
    // 4. Tiled vs Floating window type toggle
    if (window->isFloating() && conf_floating_windows && !conf_floating_windows->value()) {
        return true;
    }
    if (!window->isFloating() && conf_tiled_windows && !conf_tiled_windows->value()) {
        return true;
    }
    // 5. Excluded classes pattern match
    if (isClassExcluded(window->metadata().appID())) {
        return true;
    }

    return false;
}

static PHLWINDOW findHoveredWindow(const Vector2D& mousePos) {
    if (!Desktop::viewState())
        return nullptr;

    // 1. Hit-test the topmost window under the cursor. Do not pass
    //    FOCUS_PRIORITY: an unfocused window under the pointer should still
    //    tilt (FOCUS_PRIORITY would skip anything that isn't focus-priority).
    constexpr uint16_t props = Desktop::View::ALLOW_FLOATING | Desktop::View::INPUT_EXTENTS | Desktop::View::RESERVED_EXTENTS;
    PHLWINDOW          w     = Desktop::viewState()->hitTest().windowAt(mousePos, props);
    if (w && w->mapped())
        return w;

    // 2. Fallback: Hyprland's last mouse focus is the window the pointer is
    //    actually over, even when it isn't focused. Ignore stale focus refs
    //    whose workspace is no longer visible.
    if (g_pInputManager) {
        auto lastFocus = g_pInputManager->m_lastMouseFocus.lock();
        if (lastFocus && lastFocus->mapped() && (!lastFocus->m_workspace || lastFocus->m_workspace->visible()) &&
            lastFocus->getFullWindowBoundingBox().containsPoint(mousePos))
            return lastFocus;
    }

    // 3. Last resort: scan the window list. It is ordered bottom-to-top, so
    //    iterate in reverse to find the topmost containing window.
    if (Desktop::windowState()) {
        const auto& windows = Desktop::windowState()->windows();
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            const auto& candidate = *it;
            if (!candidate || !candidate->mapped())
                continue;
            if (candidate->m_workspace && !candidate->m_workspace->visible())
                continue;
            if (candidate->getFullWindowBoundingBox().containsPoint(mousePos))
                return candidate;
        }
    }

    return nullptr;
}

// Static lean used while the cursor is not over a window. The direction follows
// the window's position on its monitor, so a grid of windows reads as a 3D
// scene instead of every card leaning the same way. A minimum magnitude keeps
// the effect visible even for a perfectly centred window.
static void idleTiltTarget(const PHLWINDOW& window, float maxTilt, float strength, float& outRotX, float& outRotY) {
    const CBox  box = window->getFullWindowBoundingBox();
    const float W   = (float)box.w;
    const float H   = (float)box.h;
    if (W <= 1.0f || H <= 1.0f)
        return;

    const float sizeFactor = std::clamp(800.0f / std::max(W, H), 0.35f, 1.0f);
    const float base       = maxTilt * strength * sizeFactor;

    // Default lean: upper-right recedes, matching the hover sign convention.
    float nx = 0.6f;
    float ny = -0.6f;

    if (auto mon = window->m_monitor.lock(); mon && mon->m_size.x > 1.0 && mon->m_size.y > 1.0) {
        const float    scale   = std::max(mon->m_scale, 0.01f);
        const Vector2D monSize = mon->m_size / scale;
        const Vector2D monCtr  = mon->m_position + monSize * 0.5;
        const Vector2D winCtr{box.x + box.w * 0.5, box.y + box.h * 0.5};

        nx = std::clamp((float)((winCtr.x - monCtr.x) / (monSize.x * 0.5)), -1.0f, 1.0f);
        ny = std::clamp((float)((winCtr.y - monCtr.y) / (monSize.y * 0.5)), -1.0f, 1.0f);

        // Keep at least 60% of the lean so centred windows still look 3D.
        nx = (nx < 0.0f ? -1.0f : 1.0f) * std::max(std::abs(nx), 0.6f);
        ny = (ny < 0.0f ? -1.0f : 1.0f) * std::max(std::abs(ny), 0.6f);
    }

    outRotY = -nx * base;
    outRotX = ny * base;
}

static void onPreRender([[maybe_unused]] PHLMONITOR pMonitor) {
    const auto  now   = std::chrono::steady_clock::now();
    const float rawDt = std::chrono::duration<float>(now - s_lastFrameTime).count();

    // render.pre fires once per monitor. Only the first monitor in a frame
    // should advance the shared spring state; otherwise a dual-monitor setup
    // would integrate every spring twice per frame. A large dt means the
    // compositor was idle; cap the jump instead of teleporting the spring.
    constexpr float MIN_FRAME_DT = 0.0005f;
    if (rawDt <= MIN_FRAME_DT)
        return;

    s_lastFrameTime = now;
    const float dt  = rawDt > 0.05f ? 0.016f : rawDt;

    const Vector2D mousePos      = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : Vector2D{0, 0};
    PHLWINDOW      hoveredWindow = nullptr;
    const bool     active        = isPluginActive();

    if (active) {
        hoveredWindow = findHoveredWindow(mousePos);
        if (hoveredWindow && isWindowBypassed(hoveredWindow))
            hoveredWindow = nullptr;
    }

    const float idleTilt = confFloat(conf_idle_tilt, 0.7f, 0.0f, 1.0f);

    // Recursive mutex: damageWindow() re-enters transformedExtents(), which
    // reads the spring state under the same lock.
    std::scoped_lock lock(s_stateMutex);

    if (active && idleTilt > 0.0f && Desktop::windowState()) {
        // Track every eligible window so the 3D lean persists without hover.
        for (const auto& window : Desktop::windowState()->windows()) {
            if (!window || !window->mapped() || isWindowBypassed(window))
                continue;
            CTiltTransformer::s_springStates.try_emplace(window);
        }
    } else if (hoveredWindow) {
        CTiltTransformer::s_springStates.try_emplace(hoveredWindow);
    }

    const float stiffness  = confFloat(conf_stiffness, 580.0f, 1.0f, 5000.0f);
    // Keep at least a little damping. An undamped spring never satisfies the
    // settle threshold, which would keep the damage loop running forever.
    const float damping    = confFloat(conf_damping, 34.0f, 1.0f, 500.0f);
    const float maxTilt    = confFloat(conf_max_tilt_deg, 3.0f, 0.0f, 30.0f);
    const float hoverScale = confFloat(conf_hover_scale, 1.0f, 0.5f, 1.05f);

    std::vector<PHLWINDOWREF> toErase;

    for (auto& [windowRef, spring] : CTiltTransformer::s_springStates) {
        auto window = windowRef.lock();
        if (!window || !window->mapped()) {
            // A window can be unmapped while it is still tilted. Detach the
            // transformer now; otherwise it would stay attached after a
            // remap and keep warping with no spring state.
            if (window && spring.wasAttached) {
                detachTransformer(window);
                spring.wasAttached = false;
            }
            toErase.push_back(windowRef);
            continue;
        }

        float targetRotX  = 0.0f;
        float targetRotY  = 0.0f;
        float targetScale = 1.0f;

        if (window == hoveredWindow) {
            const CBox  box = window->getFullWindowBoundingBox();
            const float W   = (float)box.w;
            const float H   = (float)box.h;
            if (W > 1.0f && H > 1.0f) {
                const float cx = (float)box.x + W * 0.5f;
                const float cy = (float)box.y + H * 0.5f;

                // 1. Normalized cursor offsets in [-1.0, 1.0]
                const float nx = std::clamp(((float)mousePos.x - cx) / (W * 0.5f), -1.0f, 1.0f);
                const float ny = std::clamp(((float)mousePos.y - cy) / (H * 0.5f), -1.0f, 1.0f);

                // Inverse scaling for large windows
                const float maxDim        = std::max(W, H);
                const float sizeFactor    = std::clamp(800.0f / maxDim, 0.35f, 1.0f);
                const float effectiveTilt = maxTilt * sizeFactor;

                // 2. Reverse-tilt sign convention: the hovered quadrant moves
                //    backward into negative Z (away from the viewer).
                const float hoverRotY = -nx * effectiveTilt;
                const float hoverRotX = ny * effectiveTilt;

                // 3. Keep the idle lean underneath the hover offset. Without
                //    this, hovering the exact centre would flatten the window
                //    and the persistent 3D look would disappear. Blend towards
                //    the pure hover target as the cursor moves outwards.
                float idleRotX = 0.0f;
                float idleRotY = 0.0f;
                float blend    = 0.0f;
                if (active && idleTilt > 0.0f && !isWindowBypassed(window)) {
                    idleTiltTarget(window, maxTilt, idleTilt, idleRotX, idleRotY);
                    const float hoverMag = std::hypot(hoverRotX, hoverRotY);
                    blend = std::clamp(hoverMag / std::max(effectiveTilt, 1e-4f), 0.0f, 1.0f);
                }

                targetRotY  = hoverRotY + idleRotY * (1.0f - blend);
                targetRotX  = hoverRotX + idleRotX * (1.0f - blend);
                targetScale = hoverScale;
            }
        } else if (active && idleTilt > 0.0f && !isWindowBypassed(window)) {
            idleTiltTarget(window, maxTilt, idleTilt, targetRotX, targetRotY);
        }

        // 3. Integrate spring physics
        spring.step(targetRotX, targetRotY, targetScale, stiffness, damping, dt);

        const bool settled = spring.isSettled(targetRotX, targetRotY, targetScale);
        const bool flat    = spring.isFlat();

        // 4. Attach while tilted, detach once flat.
        if (!flat) {
            attachTransformer(window);
            spring.wasAttached = true;
        } else if (spring.wasAttached) {
            detachTransformer(window);
            spring.snapFlat();
            spring.wasAttached = false;
            if (g_pHyprRenderer)
                g_pHyprRenderer->damageWindow(window, true); // Final pass without transformer
        }

        // 5. Keep scheduling frames until the spring reaches its target.
        //    This is intentionally independent of `flat`: the first few
        //    sub-steps of a new animation can still be below the flat
        //    threshold, and without a damage the animation would stall until
        //    the next mouse event. Schedule the monitor explicitly because a
        //    damage request alone does not always wake an idle compositor.
        if (!settled) {
            if (auto mon = window->m_monitor.lock())
                mon->scheduleFrame();
            if (g_pHyprRenderer)
                g_pHyprRenderer->damageWindow(window, true);
        }

        // 6. Forget windows that have returned to flat and are no longer hovered.
        if (window != hoveredWindow && flat)
            toErase.push_back(windowRef);
    }

    for (const auto& ref : toErase)
        CTiltTransformer::s_springStates.erase(ref);
}

// ----------------------------------------------------------------------------
// Lua C Callbacks (hl.plugin.hyprtilt.*)
// ----------------------------------------------------------------------------

static void kickHoveredWindow() {
    if (!g_pHyprRenderer || !g_pInputManager)
        return;

    const auto mousePos = g_pInputManager->getMouseCoordsInternal();
    if (auto hovered = findHoveredWindow(mousePos)) {
        std::scoped_lock lock(s_stateMutex);
        if (auto mon = hovered->m_monitor.lock())
            mon->scheduleFrame();
        g_pHyprRenderer->damageWindow(hovered, true);
    }
}

static bool setPluginEnabled(bool enabled) {
    g_enabledOverride.store(enabled, std::memory_order_relaxed);

    // The config master switch can still keep the plugin off; report the
    // effective state, not the requested one.
    const bool effective = isPluginActive();
    if (effective) {
        kickHoveredWindow();
        damageAllWindows();
    } else {
        easeOutAll();
    }

    logTilt(std::string("[hyprtilt] set_enabled: requested=") + (enabled ? "true" : "false") + " effective=" + (effective ? "true" : "false"));
    return effective;
}

static bool togglePlugin() {
    // Toggle relative to the effective state so the keybind is meaningful even
    // if the config master switch is off (in which case it cannot force-enable).
    return setPluginEnabled(!isPluginActive());
}

static int lua_toggle(lua_State* L) {
    lua_pushboolean(L, togglePlugin());
    return 1;
}

static int lua_set_enabled(lua_State* L) {
    if (lua_isboolean(L, 1))
        setPluginEnabled(lua_toboolean(L, 1) != 0);

    lua_pushboolean(L, isPluginActive());
    return 1;
}

static int lua_is_enabled(lua_State* L) {
    lua_pushboolean(L, isPluginActive());
    return 1;
}

// ----------------------------------------------------------------------------
// Plugin API Exports
// ----------------------------------------------------------------------------

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;

    // 1. Mandatory commit hash mismatch check
    const std::string serverHash = __hyprland_api_get_hash();
    const std::string clientHash = __hyprland_api_get_client_hash();

    if (serverHash != clientHash) {
        const std::string err = std::format("[hyprtilt] Commit hash mismatch! Server: {}, Client: {}", serverHash, clientHash);
        LOG(Log::CRIT, "{}", err);
        throw std::runtime_error(err);
    }

    // 2. Register native config options under plugin:hyprtilt:*
    conf_enabled = makeShared<Config::Values::CBoolValue>("plugin:hyprtilt:enabled", "Enable 3D card tilt effect", true);
    HyprlandAPI::addConfigValueV2(handle, conf_enabled);

    conf_max_tilt_deg = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:max_tilt_deg", "Maximum tilt angle in degrees", 3.0f,
                                                                Config::Values::SFloatValueOptions{.min = 0.0f, .max = 30.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_max_tilt_deg);

    conf_hover_scale = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:hover_scale", "Uniform hover shrink (1.0 = keep the window aligned)", 1.0f,
                                                               Config::Values::SFloatValueOptions{.min = 0.5f, .max = 1.05f});
    HyprlandAPI::addConfigValueV2(handle, conf_hover_scale);

    conf_perspective = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:perspective", "Virtual camera perspective distance in pixels", 1000.0f,
                                                               Config::Values::SFloatValueOptions{.min = 50.0f, .max = 100000.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_perspective);

    conf_stiffness = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:stiffness", "Spring physics stiffness constant", 580.0f,
                                                             Config::Values::SFloatValueOptions{.min = 1.0f, .max = 5000.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_stiffness);

    conf_damping = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:damping", "Spring physics damping constant", 34.0f,
                                                           Config::Values::SFloatValueOptions{.min = 1.0f, .max = 500.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_damping);

    conf_brightness_boost = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:brightness_boost", "Luminance multiplier (1.0 = resting luminance)", 1.0f,
                                                                    Config::Values::SFloatValueOptions{.min = 0.25f, .max = 4.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_brightness_boost);

    conf_idle_tilt = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:idle_tilt", "Always-on tilt strength as a fraction of max_tilt_deg (0 = hover only)", 0.7f,
                                                                    Config::Values::SFloatValueOptions{.min = 0.0f, .max = 1.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_idle_tilt);

    conf_spotlight = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:spotlight", "Border spotlight intensity (0 = off, 1 = strong)", 0.5f,
                                                                    Config::Values::SFloatValueOptions{.min = 0.0f, .max = 1.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_spotlight);
    conf_opacity = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:opacity", "Window opacity multiplier (1.0 = full opacity)", 1.0f,
                                                                    Config::Values::SFloatValueOptions{.min = 0.0f, .max = 1.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_opacity);
    conf_edge_min = makeShared<Config::Values::CFloatValue>("plugin:hyprtilt:edge_min", "Minimum edge feather factor (0.0 = soft edge, 1.0 = hard edge)", 0.0f,
                                                                    Config::Values::SFloatValueOptions{.min = 0.0f, .max = 1.0f});
    HyprlandAPI::addConfigValueV2(handle, conf_edge_min);
    conf_tiled_windows = makeShared<Config::Values::CBoolValue>("plugin:hyprtilt:tiled_windows", "Apply tilt to tiled windows", true);
    HyprlandAPI::addConfigValueV2(handle, conf_tiled_windows);

    conf_floating_windows = makeShared<Config::Values::CBoolValue>("plugin:hyprtilt:floating_windows", "Apply tilt to floating windows", true);
    HyprlandAPI::addConfigValueV2(handle, conf_floating_windows);

    conf_exclude_classes = makeShared<Config::Values::CStringValue>("plugin:hyprtilt:exclude_classes", "Comma-separated window classes excluded from tilt", "");
    HyprlandAPI::addConfigValueV2(handle, conf_exclude_classes);

    // 3. Register Lua functions under hl.plugin.hyprtilt.*
    HyprlandAPI::addLuaFunction(handle, "hyprtilt", "toggle", lua_toggle);
    HyprlandAPI::addLuaFunction(handle, "hyprtilt", "set_enabled", lua_set_enabled);
    HyprlandAPI::addLuaFunction(handle, "hyprtilt", "is_enabled", lua_is_enabled);

    // 4. Register `hyprctl hyprtilt [toggle|on|off|status]`.
    //    Hyprland 0.57 removed plugin dispatchers (addDispatcherV2 is a no-op),
    //    so a socket command is the supported shell entry point.
    HyprlandAPI::registerHyprCtlCommand(handle, IPC::Socket1::SCommand{
        .name    = "hyprtilt",
        .match   = IPC::Socket1::COMMAND_MATCH_PREFIX,
        .handler = [](const IPC::Socket1::SRequest& request) -> IPC::Socket1::SResponse {
            constexpr std::string_view NAME = "hyprtilt";
            std::string                args = request.command;

            if (args.starts_with(NAME))
                args.erase(0, NAME.size());
            while (!args.empty() && args.front() == ' ')
                args.erase(args.begin());

            if (args.empty() || args == "status")
                return isPluginActive() ? "enabled" : "disabled";

            if (args == "debug") {
                const auto pos = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : Vector2D{-1, -1};

                std::string springs;
                {
                    std::scoped_lock lock(s_stateMutex);
                    for (const auto& [ref, st] : CTiltTransformer::s_springStates) {
                        if (!springs.empty())
                            springs += ';';
                        springs += std::format("rotX={:.3f} rotY={:.3f} scale={:.4f}", st.rotX.val, st.rotY.val, st.scale.val);
                    }
                }

                return std::format("inMgr={} renderer={} mouse={:.0f},{:.0f} springs={} [{}] calls={} warps={} last(rotX={:.3f} rotY={:.3f} scale={:.4f})",
                                   g_pInputManager ? 1 : 0, g_pHyprRenderer ? 1 : 0, pos.x, pos.y, CTiltTransformer::s_springStates.size(), springs,
                                   CTiltTransformer::s_transformCalls.load(), CTiltTransformer::s_transformWarps.load(),
                                   CTiltTransformer::s_lastRotX.load(), CTiltTransformer::s_lastRotY.load(), CTiltTransformer::s_lastScale.load());
            }

            if (args == "toggle")
                return togglePlugin() ? "enabled" : "disabled";

            if (args == "on" || args == "enable")
                return setPluginEnabled(true) ? "enabled" : "disabled";

            if (args == "off" || args == "disable")
                return setPluginEnabled(false) ? "enabled" : "disabled";

            return "usage: hyprctl hyprtilt [toggle|on|off|status]";
        }});

    // 5. Connect Event Listeners
    s_preRenderListener = Event::bus()->m_events.render.pre.listen([](PHLMONITOR pMonitor) {
        onPreRender(pMonitor);
    });

    s_mouseMoveListener = Event::bus()->m_events.input.mouse.move.listen([](Vector2D coord, Event::SCallbackInfo&) {
        if (!isPluginActive() || !g_pHyprRenderer)
            return;

        const Vector2D mousePos = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : coord;
        PHLWINDOW      hovered  = findHoveredWindow(mousePos);

        if (hovered && isWindowBypassed(hovered))
            hovered = nullptr;

        std::scoped_lock lock(s_stateMutex);

        // Damage the hovered window so the spring starts without waiting for
        // the compositor to schedule a frame for another reason.
        if (hovered)
            g_pHyprRenderer->damageWindow(hovered, true);

        // Damage any window that is still easing out, otherwise leaving a
        // window would stop mid-animation when no other damage is generated.
        for (auto& [windowRef, spring] : CTiltTransformer::s_springStates) {
            if (!spring.isFlat()) {
                if (auto window = windowRef.lock())
                    g_pHyprRenderer->damageWindow(window, true);
            }
        }
    });

    s_windowDestroyListener = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF pWindowRef) {
        std::scoped_lock lock(s_stateMutex);
        CTiltTransformer::s_springStates.erase(pWindowRef);
    });

    s_configReloadListener = Event::bus()->m_events.config.reloaded.listen([]() {
        s_lastFrameTime = std::chrono::steady_clock::now();
        if (!Desktop::windowState() || !g_pHyprRenderer)
            return;
        for (const auto& window : Desktop::windowState()->windows()) {
            if (window)
                g_pHyprRenderer->damageWindow(window);
        }
    });

    logTilt("[hyprtilt] hyprtilt v1.0.0 initialized (Hyprland 0.57+)");

    return {
        .name        = "hyprtilt",
        .description = "Web-style 3D Perspective Card Hover Effect for Hyprland windows",
        .author      = "Shell-Ninja & DeepMind",
        .version     = "1.0.0"
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    logTilt("[hyprtilt] Unloading hyprtilt...");

    s_preRenderListener.reset();
    s_mouseMoveListener.reset();
    s_windowDestroyListener.reset();
    s_configReloadListener.reset();

    resetAllTilt();
    CTiltTransformer::releaseGL();

    logTilt("[hyprtilt] Plugin unloaded cleanly");
}
