#include "engine/LightsSettings.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace reforce::engine {

namespace {
std::string lower(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    return o;
}

struct Range {
    const char* key;
    const char* setting;
    double LightsSettings::*field;
    double lo, hi;
};
constexpr Range kRanges[] = {
    {"brightness", "reforce.Lights.Brightness", &LightsSettings::brightness, 0, 2},
    {"saturation", "reforce.Lights.Saturation", &LightsSettings::saturation, 0, 3},
    {"floor", "reforce.Lights.Floor", &LightsSettings::floor, 0, 0.5},
    {"smoothing", "reforce.Lights.Smoothing", &LightsSettings::smoothing, 0, 2},
    {"pulse", "reforce.Lights.Pulse", &LightsSettings::pulse, 0, 1},
    {"pulsetiming", "reforce.Lights.PulseTimingMs", &LightsSettings::pulseTimingMs, -100, 300},
};
}  // namespace

std::string applyLightsSetting(LightsSettings& s, const std::string& key, const std::string& value) {
    const std::string k = lower(key), v = lower(value);
    if (k.empty()) {
        if (v == "1" || v == "on" || v == "true" || v == "yes") s.enabled = true;
        else if (v == "0" || v == "off" || v == "false" || v == "no") s.enabled = false;
        else return "lights: on | off";
        return {};
    }
    for (const Range& r : kRanges) {
        if (k != r.key) continue;
        char* end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (v.empty() || *end || !std::isfinite(x) || x < r.lo || x > r.hi) {
            char b[64];
            std::snprintf(b, sizeof b, "lights%s: %g..%g", r.key, r.lo, r.hi);
            return b;
        }
        s.*(r.field) = x;
        return {};
    }
    return "lights: unknown setting \"" + key + "\"";
}

std::optional<std::pair<std::string, std::string>> lightsSettingEntry(const std::string& key, const std::string& value) {
    LightsSettings s;
    if (!applyLightsSetting(s, key, value).empty()) return std::nullopt;
    const std::string k = lower(key);
    if (k.empty()) return std::make_pair(std::string("reforce.Lights.Enabled"), std::string(s.enabled ? "1" : "0"));
    for (const Range& r : kRanges)
        if (k == r.key) {
            char b[32];
            std::snprintf(b, sizeof b, "%g", s.*(r.field));
            return std::make_pair(std::string(r.setting), std::string(b));
        }
    return std::nullopt;
}

}  // namespace reforce::engine
