#pragma once

#include "compat.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <atomic>

namespace Hyprtilt {

    // Recursive: render/damage callbacks re-enter transformedExtents() while a
    // spring update already holds this lock (damageWindow -> transformBoxForDamage).
    inline std::recursive_mutex s_stateMutex;

    // Log through Hyprland's own logger. Keeping a separate log file caused
    // stray files in the user's home directory and a filesystem hit per call.
    inline void logTilt(const std::string& msg) {
        if (Log::logger)
            LOG(Log::INFO, "{}", msg);
    }

    // Plugin handle for Hyprland API
    inline HANDLE g_pluginHandle = nullptr;

    // Runtime toggle override state (for Lua hl.plugin.hyprtilt.toggle() &
    // dispatchers). Lua/dispatcher callbacks and the render thread may access
    // this concurrently, so keep it atomic.
    inline std::atomic<bool> g_enabledOverride{true};

    // Hyprland native config values (plugin:hyprtilt:*)
    inline SP<Config::Values::CBoolValue>   conf_enabled;
    inline SP<Config::Values::CFloatValue>  conf_max_tilt_deg;
    inline SP<Config::Values::CFloatValue>  conf_hover_scale;
    inline SP<Config::Values::CFloatValue>  conf_perspective;
    inline SP<Config::Values::CFloatValue>  conf_stiffness;
    inline SP<Config::Values::CFloatValue>  conf_damping;
    inline SP<Config::Values::CFloatValue>  conf_brightness_boost;
    inline SP<Config::Values::CFloatValue>  conf_idle_tilt;
    inline SP<Config::Values::CFloatValue>  conf_spotlight;
    inline SP<Config::Values::CFloatValue>  conf_opacity;
    inline SP<Config::Values::CFloatValue>  conf_edge_min;
    inline SP<Config::Values::CBoolValue>   conf_tiled_windows;
    inline SP<Config::Values::CBoolValue>   conf_floating_windows;
    inline SP<Config::Values::CStringValue> conf_exclude_classes;

    // Read a float config value, falling back to a sane default and clamping
    // pathological values so a bad config cannot produce NaNs or divisions by 0.
    inline float confFloat(const SP<Config::Values::CFloatValue>& value, float fallback, float lo, float hi) {
        const float raw = value ? static_cast<float>(value->value()) : fallback;
        if (!std::isfinite(raw))
            return fallback;
        return std::clamp(raw, lo, hi);
    }

    // Check overall plugin active state
    // The config master switch is primary; runtime override (hyprctl commands)
    // can modify the effective state but cannot force the plugin on while the
    // master switch is off.
    inline bool isPluginActive() {
        if (conf_enabled && !conf_enabled->value())
            return false;        // config master switch off → always inactive
        return g_enabledOverride.load(std::memory_order_relaxed);
    }

    // Helper to check if a window class matches comma-separated exclude_classes
    inline bool isClassExcluded(const std::string& className) {
        if (!conf_exclude_classes)
            return false;

        const std::string raw = conf_exclude_classes->value();
        if (raw.empty())
            return false;

        std::stringstream ss(raw);
        std::string token;
        while (std::getline(ss, token, ',')) {
            // Trim whitespace (tolerate an all-whitespace token without UB)
            const auto first = token.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                continue;
            const auto last = token.find_last_not_of(" \t\r\n");
            token = token.substr(first, last - first + 1);
            if (token.empty())
                continue;

            // Case-insensitive substring match. Cast to unsigned char before
            // tolower() because plain char may be signed.
            const auto it = std::search(className.begin(), className.end(), token.begin(), token.end(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });

            if (it != className.end())
                return true;
        }

        return false;
    }

} // namespace Hyprtilt
