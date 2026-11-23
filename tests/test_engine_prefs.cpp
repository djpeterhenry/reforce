// Settings: G-Force prefs text parser, settings.json round trip, importer.
// Inline samples written for these tests.
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "engine/Prefs.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;

namespace {
const char* kSample =
    "Audio.FFT.NumBins.vis = 128\n"
    "Audio.FFT.Params = \"Envelopes=8     ShortImpulse=0.5\"\r\n"
    "Audio.InputSource = \"My Loopback\"\n"
    "Audio.PCM.NumBins = 300\n"
    "ColorMap.MapFunction = \"(1-i)^.7\"\n"
    "\"FlowField Union.Transition.Duration\" = 4.5\n"
    "UI.ActivateOnChars = \"\\t\\r \"\n"
    "WaveShape.AutoLineScale = 0\n"
    "WaveShape.LineWidth.Scale = 1.5\n"
    "Graphics.TargetFrameRate = 30\n"
    "FlowField.Transition.Function = \"1-(1-t)^2\"\n"
    "Window.left = 738\n"
    "garbage line without equals\n";

fs::path scratch(const char* name) {
    fs::path d = fs::temp_directory_path() / "reforce_prefs_test";
    fs::create_directories(d);
    return d / name;
}
}  // namespace

TEST_CASE("prefs: G-Force text parsing") {
    const auto t = PrefsTable::parse(kSample);
    CHECK(t.string("audio.inputsource", "") == "My Loopback");
    CHECK(t.number("Audio.PCM.NumBins", 0) == 300);
    CHECK(t.number("FlowField Union.Transition.Duration", 0) == 4.5);  // quoted key
    CHECK(t.string("UI.ActivateOnChars", "") == "\t\r ");             // escapes
    CHECK(t.string("Audio.FFT.Params", "") == "Envelopes=8     ShortImpulse=0.5");
    CHECK(t.entry("Audio.InputSource")->quoted);
    CHECK(!t.entry("Audio.PCM.NumBins")->quoted);
    CHECK(!t.get("missing").has_value());
    CHECK(t.number("missing", 7) == 7);
}

TEST_CASE("prefs: built-in defaults are G-Force's factory values; apply overrides") {
    Settings p;
    CHECK(p.pcmNumBins == 255);
    CHECK(p.mapFunction == "i^2.1");
    CHECK(p.fftSmooth == 1.0);
    CHECK(p.waveShape.slideshowInterval == 25);
    CHECK(p.flowField.transitionFunction == "1-(1-t)^1.35");
    CHECK(p.inputSource.empty());
    p.apply(PrefsTable::parse(kSample));
    CHECK(p.pcmNumBins == 300);
    CHECK(p.fftNumBins == 128);
    CHECK(p.inputSource == "My Loopback");
    CHECK(p.mapFunction == "(1-i)^.7");
    CHECK(!p.autoLineScale);
    CHECK(p.lineWidthScale == 1.5);
    CHECK(p.targetFrameRate == 30);
    CHECK(p.union_.transitionDuration == 4.5);
    CHECK(p.flowField.transitionFunction == "1-(1-t)^2");
    CHECK(p.waveShape.transitionFunction == "1-(1-t)^1.6");  // untouched default
}

TEST_CASE("prefs: reforce.Intensity selects the trail buffer (default gforce)") {
    Settings p;
    CHECK(p.intensity == "gforce");
    PrefsTable t;
    t.set("reforce.Intensity", "hq", false);
    p.apply(t);
    CHECK(p.intensity == "hq");
}

TEST_CASE("prefs: JSON round trip") {
    PrefsTable t;
    t.set("ColorMap.MapFunction", "i^\"2\"\\", true);
    t.set("Audio.PCM.NumBins", "300", false);
    t.set("WaveShape.AntiAlias", "0", false);
    const std::string json = t.toJson();
    std::string err;
    auto back = PrefsTable::parseJson(json, &err);
    REQUIRE(back.has_value());
    CHECK(back->string("ColorMap.MapFunction", "") == "i^\"2\"\\");
    CHECK(back->number("audio.pcm.numbins", 0) == 300);
    CHECK(!back->flag("WaveShape.AntiAlias", true));
    CHECK(PrefsTable::parseJson("{\"a\": true, \"b\": null, \"c\": -1.5e2}")->number("c", 0) == -150);
    CHECK(!PrefsTable::parseJson("{\"a\": {\"nested\": 1}}", &err).has_value());
    CHECK(!PrefsTable::parseJson("[1,2]").has_value());
}

TEST_CASE("prefs: importer copies only the supported keys and merges") {
    const fs::path gf = scratch("gforce.txt"), js = scratch("settings.json");
    fs::remove(js);
    { std::ofstream(gf) << kSample; }
    { std::ofstream(js) << "{ \"Custom.Key\": \"keep me\", \"Audio.PCM.NumBins\": 1 }"; }
    const ImportResult r = importGForcePrefs(gf.string(), js.string());
    REQUIRE(r.ok);
    CHECK(r.imported.size() == 10);
    std::string err;
    const Settings s = loadSettings(js.string(), &err);
    CHECK(err.empty());
    CHECK(s.loaded);
    CHECK(s.pcmNumBins == 300);  // overwritten by the import
    CHECK(s.inputSource == "My Loopback");
    std::stringstream ss;
    ss << std::ifstream(js).rdbuf();
    CHECK(ss.str().find("keep me") != std::string::npos);         // merged, not replaced
    CHECK(ss.str().find("Window.left") == std::string::npos);     // not a supported key
    CHECK(ss.str().find("UI.ActivateOnChars") == std::string::npos);
}

TEST_CASE("prefs: importer refuses to write into G-Force's prefs folder") {
    const fs::path gf = scratch("gforce.txt");
    { std::ofstream(gf) << kSample; }
    const auto r = importGForcePrefs(gf.string(), "/tmp/x/Library/Preferences/SoundSpectrum/G-Force/settings.json");
    CHECK(!r.ok);
    CHECK(!fs::exists("/tmp/x/Library/Preferences/SoundSpectrum"));
}

TEST_CASE("prefs: a missing settings file gives defaults") {
    const auto p = loadSettings("/nonexistent/reforce/settings.json");
    CHECK(!p.loaded);
    CHECK(p.pcmNumBins == 255);
}

TEST_CASE("prefs: reforce.BeatClock.SteadyTempo (default on)") {
    CHECK(loadSettings("/nonexistent/reforce/settings.json").beatClockSteadyTempo);
    const fs::path js = scratch("steady.json");
    { std::ofstream(js) << "{ \"reforce.BeatClock.SteadyTempo\": 0 }"; }
    CHECK(!loadSettings(js.string()).beatClockSteadyTempo);
}

// ---- unknown settings and Look values are reported, not ignored silently (Peter 2026-11-22) ----
#include <regex>

#include "engine/Look.hpp"

TEST_CASE("settings: keys reforce does not read are listed; known ones are not") {
    const fs::path p = fs::temp_directory_path() / "reforce_unknown_settings.json";
    {
        std::ofstream out(p);
        out << "{\n  \"reforce.Object.MaxLive\": 1,\n  \"reforce.MovieSprite.Colour\": \"off\",\n"
               "  \"reforce.LineEdges\": \"colour\",\n  \"Reforce.ThinLines\": \"smooth\"\n}\n";
    }
    const Settings s = loadSettings(p.string());
    fs::remove(p);
    REQUIRE(s.loaded);
    CHECK(s.unknownKeys == std::vector<std::string>{"reforce.MovieSprite.Colour"});  // keys match case-insensitively
    const auto problems = lookSettingProblems(s);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0] == "reforce.LineEdges: \"colour\" is not gforce | color (using gforce)");
}

TEST_CASE("settings: every key reforce writes is one it reads (no false 'unknown setting' warnings)") {
    // Keys written by literal name anywhere in src/ (PrefsTable::set and the Look panel's save), each loaded back.
    std::set<std::string> keys;
    const std::regex setCall(R"re(\.set\("((?:reforce|WaveShape|ColorMap|FlowField|Particle|Sprite|Object|Dancer|Graphics)\.[A-Za-z0-9.]+)")re");
    for (const auto& e : fs::recursive_directory_iterator(fs::path(REFORCE_SOURCE_DIR) / "src")) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".mm") continue;
        std::ifstream in(e.path());
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        for (std::sregex_iterator it(text.begin(), text.end(), setCall), end; it != end; ++it) keys.insert((*it)[1]);
    }
    for (const auto& k : lookKeys()) keys.insert(k.settingsKey);
    REQUIRE(keys.size() > 20);
    const fs::path p = fs::temp_directory_path() / "reforce_written_settings.json";
    {
        std::ofstream out(p);
        out << "{\n";
        std::size_t n = 0;
        for (const auto& k : keys) out << "  \"" << k << "\": \"1\"" << (++n < keys.size() ? ",\n" : "\n");
        out << "}\n";
    }
    const Settings s = loadSettings(p.string());
    fs::remove(p);
    for (const auto& k : s.unknownKeys) CHECK_MESSAGE(false, "written but never read: " << k);
}
