#include "engine/Prefs.hpp"
#include "engine/Dancers.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace reforce::engine {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// G-Force quoted token at s[pos] == '"'; advances past the closing quote.
std::string readQuoted(std::string_view s, std::size_t& pos) {
    std::string out;
    ++pos;
    while (pos < s.size() && s[pos] != '"') {
        char c = s[pos++];
        if (c == '\\' && pos < s.size()) {
            const char e = s[pos++];
            switch (e) {
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'n': c = '\n'; break;
                default: c = e; break;
            }
        }
        out.push_back(c);
    }
    if (pos < s.size()) ++pos;
    return out;
}

bool isNumber(const std::string& v) {
    if (v.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return end && *end == '\0' && std::isfinite(d);
}

std::string jsonEscape(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(char(c));
                }
        }
    }
    return out + "\"";
}

// Minimal JSON reader for a flat object.
struct JsonReader {
    std::string_view s;
    std::size_t i = 0;
    std::string err;

    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i);
        return false;
    }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\') {
                if (i >= s.size()) return fail("bad escape");
                const char e = s[i++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (i + 4 > s.size()) return fail("bad \\u escape");
                        const unsigned cp = unsigned(std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16));
                        i += 4;
                        if (cp < 0x80) out.push_back(char(cp));
                        else if (cp < 0x800) {
                            out.push_back(char(0xC0 | (cp >> 6)));
                            out.push_back(char(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(char(0xE0 | (cp >> 12)));
                            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(char(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i;
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------- PrefsTable

void PrefsTable::set(const std::string& key, const std::string& value, bool quoted) {
    values_[lower(key)] = Entry{key, value, quoted};
}

PrefsTable PrefsTable::parse(std::string_view text) {
    PrefsTable t;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#' || line.substr(0, 2) == "//") continue;
        std::string key;
        std::size_t pos = 0;
        if (line[0] == '"') {
            key = readQuoted(line, pos);
        } else {
            pos = line.find('=');
            if (pos == std::string_view::npos) continue;
            key = std::string(trim(line.substr(0, pos)));
        }
        pos = line.find('=', pos);
        if (pos == std::string_view::npos || key.empty()) continue;
        std::string_view rest = trim(line.substr(pos + 1));
        if (!rest.empty() && rest[0] == '"') {
            std::size_t q = 0;
            t.set(std::string(trim(key)), readQuoted(rest, q), true);
        } else {
            t.set(std::string(trim(key)), std::string(rest), false);
        }
    }
    return t;
}

std::optional<PrefsTable> PrefsTable::parseJson(std::string_view json, std::string* error) {
    JsonReader r{json, 0, {}};
    PrefsTable t;
    auto bad = [&]() -> std::optional<PrefsTable> {
        if (error) *error = r.err.empty() ? "malformed JSON" : r.err;
        return std::nullopt;
    };
    r.ws();
    if (r.i >= json.size() || json[r.i] != '{') return r.fail("expected '{'"), bad();
    ++r.i;
    r.ws();
    if (r.i < json.size() && json[r.i] == '}') return t;
    for (;;) {
        r.ws();
        std::string key;
        if (!r.str(key)) return bad();
        r.ws();
        if (r.i >= json.size() || json[r.i] != ':') return r.fail("expected ':'"), bad();
        ++r.i;
        r.ws();
        if (r.i >= json.size()) return r.fail("unexpected end"), bad();
        const char c = json[r.i];
        if (c == '"') {
            std::string v;
            if (!r.str(v)) return bad();
            t.set(key, v, true);
        } else if (json.compare(r.i, 4, "true") == 0) {
            t.set(key, "1", false);
            r.i += 4;
        } else if (json.compare(r.i, 5, "false") == 0) {
            t.set(key, "0", false);
            r.i += 5;
        } else if (json.compare(r.i, 4, "null") == 0) {
            r.i += 4;
        } else if (c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t b = r.i;
            while (r.i < json.size() && (std::isdigit(static_cast<unsigned char>(json[r.i])) ||
                                         std::strchr("+-.eE", json[r.i])))
                ++r.i;
            const std::string num(json.substr(b, r.i - b));
            if (!isNumber(num)) return r.fail("bad number"), bad();
            t.set(key, num, false);
        } else {
            return r.fail("unsupported value (settings.json is a flat object)"), bad();
        }
        r.ws();
        if (r.i < json.size() && json[r.i] == ',') {
            ++r.i;
            continue;
        }
        if (r.i < json.size() && json[r.i] == '}') return t;
        return r.fail("expected ',' or '}'"), bad();
    }
}

std::string PrefsTable::toJson() const {
    std::string out = "{\n";
    std::size_t n = 0;
    for (const auto& [lk, e] : values_) {
        out += "  " + jsonEscape(e.key) + ": ";
        out += (!e.quoted && isNumber(e.value)) ? e.value : jsonEscape(e.value);
        out += ++n < values_.size() ? ",\n" : "\n";
    }
    return out + "}\n";
}

const PrefsTable::Entry* PrefsTable::entry(std::string_view key) const {
    auto it = values_.find(lower(key));
    return it == values_.end() ? nullptr : &it->second;
}

std::optional<std::string> PrefsTable::get(std::string_view key) const {
    const Entry* e = entry(key);
    if (!e) return std::nullopt;
    return e->value;
}

std::string PrefsTable::string(std::string_view key, std::string fallback) const {
    auto v = get(key);
    return v ? *v : std::move(fallback);
}

double PrefsTable::number(std::string_view key, double fallback) const {
    auto v = get(key);
    if (!v || v->empty()) return fallback;
    char* endp = nullptr;
    const double d = std::strtod(v->c_str(), &endp);
    if (endp == v->c_str() || !std::isfinite(d)) return fallback;
    return d;
}

// ---------------------------------------------------------------- Settings

namespace {

const char* const kLayerPrefixes[] = {"WaveShape", "FlowField", "ColorMap", "Particle", "Sprite", "FlowField Union"};

void applyLayer(LayerPrefs& p, const PrefsTable& t, const std::string& prefix) {
    p.transitionDuration = t.number(prefix + ".Transition.Duration", p.transitionDuration);
    p.transitionDeviation = t.number(prefix + ".Transition.Duration.Deviation", p.transitionDeviation);
    p.transitionQuick = t.number(prefix + ".Transition.Duration.Quick", p.transitionQuick);
    p.transitionFunction = t.string(prefix + ".Transition.Function", p.transitionFunction);
    p.slideshowInterval = t.number(prefix + ".Slideshow.Interval.Duration", p.slideshowInterval);
    p.slideshowDeviation = t.number(prefix + ".Slideshow.Interval.Duration.Deviation", p.slideshowDeviation);
    p.duration = t.number(prefix + ".Duration", p.duration);
    p.durationDeviation = t.number(prefix + ".Duration.Deviation", p.durationDeviation);
    p.slideshowOnStartup = t.flag(prefix + ".Slideshow.EnableOnStartup", p.slideshowOnStartup);
}

int positiveInt(double v, int fallback) { return v >= 1.0 && v < 1e6 ? int(v) : fallback; }

}  // namespace

void Settings::apply(const PrefsTable& t) {
    mapFunction = t.string("ColorMap.MapFunction", mapFunction);
    inputSource = t.string("Audio.InputSource", inputSource);
    pcmNumBins = positiveInt(t.number("Audio.PCM.NumBins", pcmNumBins), pcmNumBins);
    fftNumBins = positiveInt(t.number("Audio.FFT.NumBins.vis", fftNumBins), fftNumBins);
    fftNumBinsIn = positiveInt(t.number("Audio.FFT.NumBins.in", fftNumBinsIn), fftNumBinsIn);
    fftParams = t.string("Audio.FFT.Params", fftParams);
    pcmSmooth = t.number("Audio.PCM.Smooth", pcmSmooth);
    fftSmooth = t.number("Audio.FFT.Smooth", fftSmooth);
    fftFreqLo = t.number("Audio.FFT.FreqLo", fftFreqLo);
    fftFreqHi = t.number("Audio.FFT.FreqHi", fftFreqHi);
    responseScale = t.number("Audio.Response.Scale", responseScale);
    lineWidthScale = t.number("WaveShape.LineWidth.Scale", lineWidthScale);
    lineWidthOffset = t.number("WaveShape.LineWidth.Offset", lineWidthOffset);
    autoLineScale = t.flag("WaveShape.AutoLineScale", autoLineScale);
    lineScale = t.string("reforce.LineScale", lineScale);
    antiAlias = t.flag("WaveShape.AntiAlias", antiAlias);
    intensity = t.string("reforce.Intensity", intensity);
    flowRounding = t.string("reforce.FlowRounding", flowRounding);
    flowPrecision = t.string("reforce.FlowPrecision", flowPrecision);
    thinLines = t.string("reforce.ThinLines", thinLines);
    spriteLayering = t.string("reforce.SpriteLayering", spriteLayering);
    spriteTrails = t.number("reforce.SpriteTrails", spriteTrails);
    spriteAlpha = t.string("reforce.SpriteAlpha", spriteAlpha);
    targetFrameRate = t.number("Graphics.TargetFrameRate", targetFrameRate);
    LayerPrefs* layers[] = {&waveShape, &flowField, &colorMap, &particle, &sprite, &union_};
    for (int i = 0; i < 6; ++i) applyLayer(*layers[i], t, kLayerPrefixes[i]);
    applyLayer(object, t, "Object");    // reforce-only: solids and organics
    applyLayer(dancer, t, "Dancer");    // reforce-only
    startupTheme = t.string("Theme.StartupTheme", startupTheme);
    blendWaveShape = t.string("reforce.Blend.WaveShape", blendWaveShape);
    blendColorMap = t.string("reforce.Blend.ColorMap", blendColorMap);
    blendFlowField = t.string("reforce.Blend.FlowField", blendFlowField);
    scenesDir = t.string("reforce.ScenesDir", scenesDir);
    startFullScreen = t.flag("reforce.Window.StartFullScreen", startFullScreen);
    externalSpritesDir = t.string("reforce.ExternalSpritesDir", externalSpritesDir);
    videoDir = t.string("reforce.VideoDir", videoDir);
    videoStartupSource = t.string("reforce.Video.Layer.Source", videoStartupSource);
    videoSpriteLoop = t.flag("reforce.Video.Sprite.Loop", videoSpriteLoop);
    movieBeatSync = t.flag("reforce.Video.BeatSync", movieBeatSync);
    beatClockSource = t.string("reforce.BeatClock.Source", beatClockSource);
    beatClockAudioOffsetMs = t.number("reforce.BeatClock.AudioOffsetMs", beatClockAudioOffsetMs);
    beatClockOffsetMs = std::clamp(t.number("reforce.BeatClock.OffsetMs", beatClockOffsetMs), -200.0, 500.0);
    beatClockSteadyTempo = t.flag("reforce.BeatClock.SteadyTempo", beatClockSteadyTempo);
    spriteReplace = t.string("reforce.SpriteReplace", spriteReplace);
    movieSpriteInterval = t.number("reforce.MovieSprite.Slideshow.Interval.Duration", movieSpriteInterval);
    spriteMix = t.number("reforce.Sprite.Mix", spriteMix);
    movieSpriteMix = t.number("reforce.MovieSprite.Mix", movieSpriteMix);
    movieSpriteColour = t.string("reforce.MovieSprite.Colour", movieSpriteColour);
    movieSpriteGamma = t.number("reforce.MovieSprite.Gamma", movieSpriteGamma);
    movieSpriteMaxLive = std::clamp(int(t.number("reforce.MovieSprite.MaxLive", movieSpriteMaxLive)), 0, 64);
    syphonAllowFeedback = t.flag("reforce.Syphon.AllowFeedback", syphonAllowFeedback);
    syphonAlpha = t.flag("reforce.Syphon.Alpha", syphonAlpha);
    syphonAlphaPremultiplied = t.flag("reforce.Syphon.AlphaPremultiplied", syphonAlphaPremultiplied);
    windowVisualsOnly = t.flag("reforce.Window.VisualsOnly", windowVisualsOnly);
    syphonKeySimilarity = std::clamp(t.number("reforce.Syphon.KeySimilarity", syphonKeySimilarity), 1.0, 1000.0);
    syphonKeySmoothness = std::clamp(t.number("reforce.Syphon.KeySmoothness", syphonKeySmoothness), 1.0, 1000.0);
    backdropSource = t.string("reforce.Backdrop.Source", backdropSource);
    backdropMirror = t.flag("reforce.Backdrop.Mirror", backdropMirror);
    backdropFit = t.string("reforce.Backdrop.Fit", backdropFit);
    backdropOpacity = std::clamp(t.number("reforce.Backdrop.Opacity", backdropOpacity), 0.0, 1.0);
    objectMaxLive = std::clamp(int(t.number("reforce.Object.MaxLive", objectMaxLive)), 1, 15);
    solidMsaa = int(t.number("reforce.Solid.Msaa", solidMsaa));
    solidTileSize = int(t.number("reforce.Solid.TileSize", solidTileSize));
    solidDir = t.string("reforce.Solid.Dir", solidDir);
    organicMaxNodes = std::clamp(int(t.number("reforce.Organic.MaxNodes", organicMaxNodes)), 100, 50000);
    dancerMaxLive = std::clamp(int(t.number("reforce.Dancer.MaxLive", dancerMaxLive)), 1, 32);
    dancerFloor = t.flag("reforce.Dancer.Floor", dancerFloor);
    dancerRandomStyles = normalizeRandomStyles(t.string("reforce.Dancer.RandomStyles", dancerRandomStyles)).value_or("all");
    dancerLighting = std::clamp(t.number("reforce.Dancer.Lighting", dancerLighting), 0.0, 1.0);
    dancerReflection = std::clamp(t.number("reforce.Dancer.Reflection", dancerReflection), 0.0, 1.0);
    dancerOutline = std::clamp(t.number("reforce.Dancer.Outline", dancerOutline), 0.0, 8.0);
    dancerHeight = std::clamp(t.number("reforce.Dancer.Height", dancerHeight), 0.15, 0.9);
    dancerHuman = std::clamp(t.number("reforce.Dancer.Human", dancerHuman), 0.0, 2.0);
    dancerMocap = std::clamp(t.number("reforce.Dancer.Mocap", dancerMocap), 0.0, 1.0);
    guestsEnabled = t.flag("reforce.Guests.Enabled", guestsEnabled);
    guestsUrl = t.string("reforce.Guests.Url", guestsUrl);
    guestsKey = t.string("reforce.Guests.Key", guestsKey);
    guestsBlocked = t.string("reforce.Guests.Blocked", guestsBlocked);
    guestsMinutes = std::clamp(t.number("reforce.Guests.Minutes", guestsMinutes), 1.0, 240.0);
    dancerMsaa = int(t.number("reforce.Dancer.Msaa", dancerMsaa));
    contentDir = t.string("reforce.ContentDir", contentDir);
    linkEnabled = t.flag("reforce.Link.Enabled", linkEnabled);
    linkFollow = t.flag("reforce.Link.Follow", linkFollow);
    exclude = t.string("reforce.Exclude", exclude);
    spriteHidden = t.string("reforce.Sprite.Hidden", spriteHidden);
    favorites = t.string("reforce.Favorites", favorites);
    videoLoopList = t.string("reforce.Video.Loop", videoLoopList);
    linkOffsetMs = t.number("reforce.Link.OffsetMs", linkOffsetMs);
    linkQuantum = std::max(1.0, t.number("reforce.Link.Quantum", linkQuantum));
    linkDriveBeat = t.flag("reforce.Link.DriveBeat", linkDriveBeat);
    linkQuantize = t.string("reforce.Link.Quantize", linkQuantize);
    linkQuantizeManual = t.string("reforce.Link.QuantizeManual", linkQuantizeManual);
    spawnQuantize = t.string("reforce.Quantize.Spawn", spawnQuantize);
    slideshowOneFeature = t.number("reforce.Slideshow.OneFeature", slideshowOneFeature ? 1 : 0) != 0;
    phraseBars = std::clamp(int(t.number("reforce.Phrase.Bars", phraseBars)), 1, 64);
    phraseAutoAlign = t.flag("reforce.Phrase.AutoAlign", phraseAutoAlign);
    phraseDeckStarts = t.flag("reforce.Phrase.DeckStarts", phraseDeckStarts);
    phraseFaderUp = std::clamp(t.number("reforce.Phrase.FaderUp", phraseFaderUp), 0.0, 1.0);
    phraseRkbxPort = std::clamp(int(t.number("reforce.Phrase.RkbxPort", phraseRkbxPort)), 1, 65535);
    phraseMidiDevice = t.string("reforce.Phrase.MidiDevice", phraseMidiDevice);
    kickZoom = std::clamp(t.number("reforce.Kick.Zoom", kickZoom), -0.1, 0.1);
    kickRotate = std::clamp(t.number("reforce.Kick.Rotate", kickRotate), -10.0, 10.0);
    kickDecayMs = std::clamp(t.number("reforce.Kick.DecayMs", kickDecayMs), 20.0, 2000.0);
    nowPlaying = t.flag("reforce.NowPlaying.Show", nowPlaying);
    nowPlayingSize = std::clamp(t.number("reforce.NowPlaying.Size", nowPlayingSize), 25.0, 300.0);
    nowPlayingCorner = t.string("reforce.NowPlaying.Corner", nowPlayingCorner);
    if (nowPlayingCorner != "bl" && nowPlayingCorner != "br" && nowPlayingCorner != "tl" && nowPlayingCorner != "tr")
        nowPlayingCorner = "bl";
    nowPlayingSwitch = std::clamp(t.number("reforce.NowPlaying.SwitchSeconds", nowPlayingSwitch), 0.0, 30.0);
    nowPlayingShadow = std::clamp(t.number("reforce.NowPlaying.Shadow", nowPlayingShadow), 0.0, 1.0);
    nowPlayingShadowFrom = std::clamp(t.number("reforce.NowPlaying.ShadowFrom", nowPlayingShadowFrom), 0.0, 1.0);
    dropSwitch = t.flag("reforce.Drop.Switch", dropSwitch);
    linkTransitionBeats = std::max(0.0, t.number("reforce.Link.TransitionBeats", linkTransitionBeats));
    for (const char* const* k = videoSettingKeys(); *k; ++k) {
        std::string key = *k;
        // Settings spelling: reforce.Video.Layer.KeyColor etc. (case-insensitive lookup).
        if (auto v = t.get("reforce.Video.Layer." + key)) {
            const std::string err = applyVideoSetting(videoLayer, key, *v);
            if (!err.empty()) settingErrors.push_back("reforce.Video.Layer." + key + ": " + err);
        }
    }
    for (const char* key : {"track", "quality", "front", "burn", "burnamount", "trails", "trailwidth", "show", "sync"}) {
        const std::string name = bodySettingKey(key);
        if (auto v = t.get(name)) {
            const std::string err = applyBodySetting(body, key, *v);
            if (!err.empty()) settingErrors.push_back(name + ": " + err);
        }
    }
    const std::pair<const char*, const char*> lightKeys[] = {{"", "reforce.Lights.Enabled"},
                                                             {"brightness", "reforce.Lights.Brightness"},
                                                             {"saturation", "reforce.Lights.Saturation"},
                                                             {"floor", "reforce.Lights.Floor"},
                                                             {"smoothing", "reforce.Lights.Smoothing"},
                                                             {"pulse", "reforce.Lights.Pulse"},
                                                             {"pulsetiming", "reforce.Lights.PulseTimingMs"}};
    for (const auto& [key, name] : lightKeys)
        if (auto v = t.get(name)) {
            const std::string err = applyLightsSetting(lights, key, *v);
            if (!err.empty()) settingErrors.push_back(std::string(name) + ": " + err);
        }
}

std::vector<std::string> importedGForceKeys() {
    std::vector<std::string> keys;
    for (const char* p : kLayerPrefixes) {
        const std::string pre = p;
        for (const char* k : {".Slideshow.EnableOnStartup", ".Slideshow.Interval.Duration",
                              ".Slideshow.Interval.Duration.Deviation",
                              ".Transition.Duration", ".Transition.Duration.Quick",
                              ".Transition.Duration.Deviation", ".Transition.Function"})
            keys.push_back(pre + k);
    }
    for (const char* k : {"ColorMap.MapFunction", "Audio.InputSource", "Audio.PCM.NumBins", "Audio.PCM.Smooth",
                          "Audio.FFT.NumBins.vis", "Audio.FFT.NumBins.in", "Audio.FFT.Smooth", "Audio.FFT.Params",
                          "Audio.FFT.FreqLo", "Audio.FFT.FreqHi", "WaveShape.LineWidth.Scale",
                          "WaveShape.LineWidth.Offset", "WaveShape.AutoLineScale", "WaveShape.AntiAlias",
                          "Graphics.TargetFrameRate", "Theme.StartupTheme", "Particle.Duration",
                          "Particle.Duration.Deviation", "Sprite.Duration", "Sprite.Duration.Deviation"})
        keys.push_back(k);
    return keys;
}

std::string defaultGForcePrefsPath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") +
           "/Library/Preferences/SoundSpectrum/G-Force/Preferences (Standalone).txt";
}

std::string defaultSettingsPath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/Library/Application Support/reforce/settings.json";
}

namespace {

bool readText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::in | std::ios::binary);  // read-only
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

Settings loadSettings(const std::string& path, std::string* error) {
    Settings s;
    s.path = path;
    std::string text;
    if (!readText(path, text)) return s;
    std::string err;
    auto table = PrefsTable::parseJson(text, &err);
    if (!table) {
        if (error) *error = path + ": " + err;
        return s;
    }
    s.apply(*table);
    s.loaded = true;
    return s;
}

bool mergeSettingsFile(const std::string& settingsPath, const PrefsTable& entries, std::string* error) {
    namespace fs = std::filesystem;
    auto fail = [&](std::string m) {
        if (error) *error = std::move(m);
        return false;
    };
    std::error_code ec;
    if (lower(fs::weakly_canonical(fs::absolute(settingsPath), ec).string()).find("/preferences/soundspectrum") !=
        std::string::npos)
        return fail("refusing to write settings under G-Force's preferences folder: " + settingsPath);
    PrefsTable merged;
    std::string existing;
    if (readText(settingsPath, existing)) {
        std::string err;
        auto t = PrefsTable::parseJson(existing, &err);
        if (!t) return fail(settingsPath + " exists but is not valid JSON (" + err + "); not overwriting it");
        merged = *t;
    }
    for (const auto& [lk, e] : entries.entries()) merged.set(e.key, e.value, e.quoted);

    fs::create_directories(fs::path(settingsPath).parent_path(), ec);
    if (ec) return fail("cannot create " + fs::path(settingsPath).parent_path().string() + ": " + ec.message());
    const std::string tmp = settingsPath + ".tmp";
    {
        std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        out << merged.toJson();
        if (!out) return fail("cannot write " + tmp);
    }
    fs::rename(tmp, settingsPath, ec);
    if (ec) return fail("cannot rename " + tmp + ": " + ec.message());
    return true;
}

bool mergeSettingsFile(const std::string& settingsPath,
                       const std::vector<std::pair<std::string, std::string>>& entries, std::string* error) {
    PrefsTable t;
    for (const auto& [k, v] : entries) t.set(k, v, true);
    return mergeSettingsFile(settingsPath, t, error);
}

ImportResult importGForcePrefs(const std::string& gforcePath, const std::string& settingsPath) {
    namespace fs = std::filesystem;
    ImportResult r;
    // Never write into G-Force's prefs folder (or the G-Force file itself).
    std::error_code ec;
    const std::string target = fs::weakly_canonical(fs::absolute(settingsPath), ec).string();
    const std::string guarded = lower(fs::weakly_canonical(fs::absolute(defaultGForcePrefsPath()).parent_path()
                                                               .parent_path(), ec).string());
    if (lower(target).rfind(guarded, 0) == 0 || lower(target).find("/preferences/soundspectrum") != std::string::npos ||
        fs::equivalent(settingsPath, gforcePath, ec)) {
        r.error = "refusing to write settings under G-Force's preferences folder: " + target;
        return r;
    }

    std::string text;
    if (!readText(gforcePath, text)) {
        r.error = "cannot read " + gforcePath;
        return r;
    }
    const PrefsTable gforce = PrefsTable::parse(text);

    PrefsTable wanted;
    for (const std::string& k : importedGForceKeys()) {
        const PrefsTable::Entry* e = gforce.entry(k);
        if (!e) {
            r.missing.push_back(k);
            continue;
        }
        wanted.set(k, e->value, e->quoted);
        r.imported.emplace_back(k, e->value);
    }
    if (!mergeSettingsFile(settingsPath, wanted, &r.error)) return r;
    r.ok = true;
    return r;
}

}  // namespace reforce::engine
