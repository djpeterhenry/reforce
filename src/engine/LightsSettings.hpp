#pragma once
// Room lights (docs/lights.md): the reforce.Lights.* settings and their panel keys (videoset lights...), shared by
// the Visualizer, the web API's validation and the command saver.
#include <optional>
#include <string>

namespace reforce::engine {

struct LightsSettings {
    bool enabled = false;     // reforce.Lights.Enabled: drive the paired Nanoleaf panels from the visuals
    double brightness = 1.0;  // reforce.Lights.Brightness 0..2
    double saturation = 1.3;  // reforce.Lights.Saturation 0..3 (1 = as on screen)
    double floor = 0.0;       // reforce.Lights.Floor 0..0.5: never darker than this
    double smoothing = 0.3;   // reforce.Lights.Smoothing 0..2 s: how slowly the panels fall back
    double pulse = 0.0;       // reforce.Lights.Pulse 0..1: flash on each beat, fading until the next (0 = off; only
                              //   while the beat clock is confident)
    double pulseTimingMs = 40;   // reforce.Lights.PulseTimingMs -100..300: the flash this much early (network and
                                 //   the panels' own delay; the flash itself is instant)
};

// A panel key without the "lights" prefix ("" = on/off, "brightness", "saturation", "floor", "smoothing", "pulse",
// "pulsetiming") and its value applied to `s`. "" on success, else the error ("lights: on | off",
// "lightsbrightness: 0..2", ...).
std::string applyLightsSetting(LightsSettings& s, const std::string& key, const std::string& value);
// The settings.json key for a panel key, and the value to save; nullopt when the key or value is not valid.
std::optional<std::pair<std::string, std::string>> lightsSettingEntry(const std::string& key, const std::string& value);

}  // namespace reforce::engine
