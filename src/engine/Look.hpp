#pragma once
// "Look": the rendering-quality choices that can be A/B'd live on the running
// scene (PLAN "Fidelity choices" F1/F2/F4/F5 + line AA). Pure state and
// validation; the Visualizer applies the values to the GPU pipeline, the web
// UI shows them (GET /api/state "look") and the command queue changes them
// (engine::parseCommand "look:...").
//
//   key            settings key           values (G-Force value first)
//   thinlines      reforce.ThinLines      gforce | smooth
//   intensity      reforce.Intensity      gforce | hq
//   flowrounding   reforce.FlowRounding   fixed16 | nearest | floor | point | fixed256
//   flowprecision  reforce.FlowPrecision  double | float32   (applies to the next FlowField build)
//   aa             WaveShape.AntiAlias    on | off
//   linescale      reforce.LineScale      gforce | frame   (line-width law on G-Force's half-size frame or on ours)
//   sprites        reforce.SpriteLayering gforce | melt | trails   (how sprites meet the feedback, probe #40)
//
// Changes are session-only until saved (settings.json, mergeSettingsFile).
// A/B: two slots. "G-Force exact" (A) and "Done right" (B) to start with;
// toggling switches slots, edits while a slot is active change that slot.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "engine/Prefs.hpp"

namespace reforce::engine {

enum class LookKey : int { ThinLines = 0, Intensity, FlowRounding, FlowPrecision, Antialias, LineScale, SpriteLayering };
constexpr int kLookKeys = 7;

struct LookKeyInfo {
    LookKey key;
    const char* id;            // short id ("thinlines")
    const char* settingsKey;   // settings.json key
    std::vector<const char*> values;
    const char* gforce;        // value that matches G-Force
    const char* right;         // "done right" / precise value
    bool live;                 // false: applies to the next FlowField build
    bool numericSetting;       // saved as a number (WaveShape.AntiAlias = 1/0)
};

const std::array<LookKeyInfo, kLookKeys>& lookKeys();
const LookKeyInfo& lookInfo(LookKey k);
// Short id or settings key, case-insensitive.
std::optional<LookKey> parseLookKey(const std::string& s);
// Canonical value or nullopt ("1"/"true" -> "on" for aa; case-insensitive).
std::optional<std::string> normalizeLookValue(LookKey k, const std::string& v);

struct LookValues {
    std::array<std::string, kLookKeys> v;
    const std::string& operator[](LookKey k) const { return v[std::size_t(k)]; }
    std::string& operator[](LookKey k) { return v[std::size_t(k)]; }
    bool operator==(const LookValues& o) const { return v == o.v; }
    bool operator!=(const LookValues& o) const { return v != o.v; }
};

// "gforce" = G-Force exact (thinlines gforce, intensity gforce, fixed16, double, aa on, linescale gforce);
// "right"  = done right (smooth, hq, nearest, double, aa on, linescale frame). Sprite layering is scene
// semantics, so it is gforce in both presets (melt/trails are set per key). FlowPrecision stays
// double in both (float32 is a G-Force guess that measured as a wash, PLAN F5),
// so flipping never forces a field rebuild.
std::optional<LookValues> lookPreset(const std::string& name);
// From settings (unknown values fall back to the defaults).
LookValues lookFromSettings(const Settings& s);

struct LookState {
    bool valid = false;           // false offline / before the visualizer publishes
    LookValues current, saved, slotA, slotB;
    char slot = 0;                // 'A', 'B' or 0 (no slot active yet)
    std::string fieldPrecision;   // precision the shown FlowField was built with
};

// Entries for engine::mergeSettingsFile: one key, or every key when `key` is empty.
PrefsTable lookSaveEntries(const LookValues& values, std::optional<LookKey> key = std::nullopt);

class LookControl {
public:
    LookControl() : LookControl(lookFromSettings(Settings{}), lookFromSettings(Settings{})) {}
    LookControl(const LookValues& current, const LookValues& saved);
    explicit LookControl(const LookState& s) : state_(s) { state_.valid = true; }

    // Each returns a bit mask (1 << int(LookKey)) of the current values that changed.
    // set: "" + mask on success, else an error (nothing changes).
    std::string set(LookKey k, const std::string& value, unsigned* changed);
    unsigned preset(const LookValues& p);
    unsigned toggleAB();
    unsigned resetToSaved(std::optional<LookKey> k = std::nullopt);
    void markSaved(LookKey k, const std::string& value);

    const LookValues& current() const { return state_.current; }
    const LookState& state() const { return state_; }

private:
    unsigned assign(const LookValues& next);
    void syncSlot();
    LookState state_;
};

}  // namespace reforce::engine
