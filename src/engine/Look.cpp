#include "engine/Look.hpp"

#include <cctype>

namespace reforce::engine {

namespace {

std::string lowered(const std::string& s) {
    std::string out;
    for (char c : s) out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

}  // namespace

const std::array<LookKeyInfo, kLookKeys>& lookKeys() {
    static const std::array<LookKeyInfo, kLookKeys> keys = {{
        {LookKey::ThinLines, "thinlines", "reforce.ThinLines", {"gforce", "smooth"}, "gforce", "smooth", true, false},
        {LookKey::Intensity, "intensity", "reforce.Intensity", {"gforce", "hq"}, "gforce", "hq", true, false},
        {LookKey::FlowRounding, "flowrounding", "reforce.FlowRounding",
         {"fixed16", "nearest", "floor", "point", "fixed256"}, "fixed16", "nearest", true, false},
        {LookKey::FlowPrecision, "flowprecision", "reforce.FlowPrecision", {"double", "float32"}, "double", "double",
         false, false},
        {LookKey::Antialias, "aa", "WaveShape.AntiAlias", {"on", "off"}, "on", "on", true, true},
        {LookKey::LineScale, "linescale", "reforce.LineScale", {"gforce", "frame"}, "gforce", "frame", true, false},
        {LookKey::SpriteLayering, "sprites", "reforce.SpriteLayering", {"gforce", "melt", "trails"}, "gforce", "gforce",
         true, false},
    }};
    return keys;
}

const LookKeyInfo& lookInfo(LookKey k) { return lookKeys()[std::size_t(k)]; }

std::optional<LookKey> parseLookKey(const std::string& s) {
    const std::string l = lowered(s);
    for (const auto& k : lookKeys())
        if (l == k.id || l == lowered(k.settingsKey)) return k.key;
    if (l == "antialias" || l == "lineaa") return LookKey::Antialias;
    if (l == "spritelayering" || l == "sprite") return LookKey::SpriteLayering;
    return std::nullopt;
}

std::optional<std::string> normalizeLookValue(LookKey k, const std::string& v) {
    std::string l = lowered(v);
    if (k == LookKey::Antialias) {
        if (l == "1" || l == "true" || l == "yes") l = "on";
        else if (l == "0" || l == "false" || l == "no") l = "off";
    }
    for (const char* x : lookInfo(k).values)
        if (l == x) return l;
    return std::nullopt;
}

std::optional<LookValues> lookPreset(const std::string& name) {
    const std::string l = lowered(name);
    const bool right = l == "right" || l == "done-right" || l == "precise" || l == "b";
    if (!right && l != "gforce" && l != "g-force" && l != "a") return std::nullopt;
    LookValues p;
    for (const auto& k : lookKeys()) p[k.key] = right ? k.right : k.gforce;
    return p;
}

LookValues lookFromSettings(const Settings& s) {
    LookValues out;
    auto put = [&](LookKey k, const std::string& v, const char* fallback) {
        out[k] = normalizeLookValue(k, v).value_or(fallback);
    };
    put(LookKey::ThinLines, s.thinLines, "smooth");
    put(LookKey::Intensity, s.intensity, "gforce");
    put(LookKey::FlowRounding, s.flowRounding, "fixed16");
    put(LookKey::FlowPrecision, s.flowPrecision, "double");
    out[LookKey::Antialias] = s.antiAlias ? "on" : "off";
    put(LookKey::LineScale, s.lineScale, "gforce");
    put(LookKey::SpriteLayering, s.spriteLayering, "gforce");
    return out;
}

PrefsTable lookSaveEntries(const LookValues& values, std::optional<LookKey> key) {
    PrefsTable t;
    for (const auto& k : lookKeys()) {
        if (key && *key != k.key) continue;
        if (k.numericSetting) t.set(k.settingsKey, values[k.key] == "on" ? "1" : "0", false);
        else t.set(k.settingsKey, values[k.key], true);
    }
    return t;
}

LookControl::LookControl(const LookValues& current, const LookValues& saved) {
    state_.valid = true;
    state_.current = current;
    state_.saved = saved;
    state_.slotA = *lookPreset("gforce");
    state_.slotB = *lookPreset("right");
}

unsigned LookControl::assign(const LookValues& next) {
    unsigned mask = 0;
    for (int i = 0; i < kLookKeys; ++i)
        if (state_.current.v[std::size_t(i)] != next.v[std::size_t(i)]) mask |= 1u << i;
    state_.current = next;
    return mask;
}

void LookControl::syncSlot() {
    if (state_.slot == 'A') state_.slotA = state_.current;
    else if (state_.slot == 'B') state_.slotB = state_.current;
}

std::string LookControl::set(LookKey k, const std::string& value, unsigned* changed) {
    const auto v = normalizeLookValue(k, value);
    if (!v) {
        std::string allowed;
        for (const char* x : lookInfo(k).values) allowed += (allowed.empty() ? "" : " | ") + std::string(x);
        return std::string(lookInfo(k).id) + ": \"" + value + "\" is not one of " + allowed;
    }
    LookValues next = state_.current;
    next[k] = *v;
    const unsigned m = assign(next);
    syncSlot();
    if (changed) *changed = m;
    return {};
}

unsigned LookControl::preset(const LookValues& p) {
    const unsigned m = assign(p);
    syncSlot();
    return m;
}

unsigned LookControl::toggleAB() {
    char to;
    if (state_.slot == 'A') to = 'B';
    else if (state_.slot == 'B') to = 'A';
    else to = state_.current == state_.slotA ? 'B' : 'A';
    state_.slot = to;
    return assign(to == 'A' ? state_.slotA : state_.slotB);
}

unsigned LookControl::resetToSaved(std::optional<LookKey> k) {
    LookValues next = state_.current;
    if (k) next[*k] = state_.saved[*k];
    else next = state_.saved;
    const unsigned m = assign(next);
    syncSlot();
    return m;
}

void LookControl::markSaved(LookKey k, const std::string& value) {
    if (auto v = normalizeLookValue(k, value)) state_.saved[k] = *v;
}

}  // namespace reforce::engine
