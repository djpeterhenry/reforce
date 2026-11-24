// Phase 5A packaging: settings merge (reforce.ScenesDir, written by
// scripts/install-app.sh), the single-instance decision, log rotation and the
// stderr log tee.
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app/LogFile.hpp"
#include "engine/Prefs.hpp"
#include "platform/Platform.hpp"

using namespace reforce;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const char* name) {
    fs::path d = fs::temp_directory_path() / "reforce_packaging_test" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

TEST_CASE("packaging: mergeSettingsFile adds reforce.ScenesDir and keeps other keys") {
    const fs::path dir = freshDir("merge");
    const fs::path path = dir / "sub" / "settings.json";  // directory created on demand

    // New file.
    std::string err;
    REQUIRE(engine::mergeSettingsFile(path.string(), {{"reforce.ScenesDir", "/repo/scenes"}}, &err));
    engine::Settings s = engine::loadSettings(path.string());
    CHECK(s.loaded);
    CHECK(s.scenesDir == "/repo/scenes");

    // Existing keys survive; the key is replaced (case-insensitively), not duplicated.
    writeFile(path, "{\n  \"Audio.InputSource\": \"DDJ-1000 Loopback\",\n  \"Graphics.TargetFrameRate\": 30,\n"
                    "  \"reforce.scenesdir\": \"/old\"\n}\n");
    REQUIRE(engine::mergeSettingsFile(path.string(), {{"reforce.ScenesDir", "/repo/scenes"}}, &err));
    s = engine::loadSettings(path.string());
    CHECK(s.inputSource == "DDJ-1000 Loopback");
    CHECK(s.targetFrameRate == doctest::Approx(30));
    CHECK(s.scenesDir == "/repo/scenes");
    const auto t = engine::PrefsTable::parseJson(slurp(path));
    REQUIRE(t);
    CHECK(t->size() == 3);
    CHECK(slurp(path).find("\"Graphics.TargetFrameRate\": 30") != std::string::npos);  // number stays a number

    // Idempotent: the same merge again changes nothing.
    const std::string before = slurp(path);
    REQUIRE(engine::mergeSettingsFile(path.string(), {{"reforce.ScenesDir", "/repo/scenes"}}, &err));
    CHECK(slurp(path) == before);

    // A malformed file is left alone.
    writeFile(path, "{ not json");
    CHECK_FALSE(engine::mergeSettingsFile(path.string(), {{"reforce.ScenesDir", "/x"}}, &err));
    CHECK(err.find("not valid JSON") != std::string::npos);
    CHECK(slurp(path) == "{ not json");
}

TEST_CASE("packaging: mergeSettingsFile refuses G-Force's preferences folder") {
    std::string err;
    CHECK_FALSE(engine::mergeSettingsFile("/tmp/x/Library/Preferences/SoundSpectrum/G-Force/settings.json",
                                          {{"a", "b"}}, &err));
    CHECK(err.find("refusing") != std::string::npos);
}

TEST_CASE("packaging: window settings") {
    engine::PrefsTable t;
    t.set("reforce.Window.StartFullScreen", "1", false);
    engine::Settings s;
    CHECK_FALSE(s.startFullScreen);
    s.apply(t);
    CHECK(s.startFullScreen);
}

TEST_CASE("packaging: Start Like This round-trips through settings.json, read and not reported as unknown") {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / "reforce_start_like_this.json";
    fs::remove(p);
    {
        std::ofstream out(p);
        out << "{\n  \"reforce.Object.MaxLive\": 1\n}\n";  // other keys are kept
    }
    // What View > Start Like This writes (main.cpp's saveStartSetup).
    engine::PrefsTable t;
    t.set("reforce.Window.Screen", "DELL U2720Q", true);
    t.set("reforce.Window.StartFullScreen", "1", false);
    t.set("reforce.UI.OpenOnLaunch", "1", false);
    std::string err;
    REQUIRE(engine::mergeSettingsFile(p.string(), t, &err));
    const engine::Settings s = engine::loadSettings(p.string());
    REQUIRE(s.loaded);
    CHECK(s.windowScreen == "DELL U2720Q");
    CHECK(s.startFullScreen);
    CHECK(s.openPanelOnLaunch);
    CHECK(s.objectMaxLive == 1);
    CHECK(s.unknownKeys.empty());
    // Don't Start Like This: all off again.
    engine::PrefsTable off;
    off.set("reforce.Window.Screen", "", true);
    off.set("reforce.Window.StartFullScreen", "0", false);
    off.set("reforce.UI.OpenOnLaunch", "0", false);
    REQUIRE(engine::mergeSettingsFile(p.string(), off, &err));
    const engine::Settings u = engine::loadSettings(p.string());
    fs::remove(p);
    CHECK(u.windowScreen.empty());
    CHECK_FALSE(u.startFullScreen);
    CHECK_FALSE(u.openPanelOnLaunch);
}

TEST_CASE("packaging: single-instance decision") {
    using platform::instanceToActivate;
    using platform::RunningInstance;
    CHECK(instanceToActivate(100, {}, false) == 0);
    CHECK(instanceToActivate(100, {{100, false}}, false) == 0);           // only ourselves
    CHECK(instanceToActivate(100, {{100, false}, {42, false}}, false) == 42);
    CHECK(instanceToActivate(100, {{100, false}, {42, true}}, false) == 0);  // terminated: ignore
    CHECK(instanceToActivate(100, {{100, false}, {42, false}}, true) == 0);  // --allow-multiple
    CHECK(instanceToActivate(100, {{77, false}, {42, false}, {100, false}}, false) == 42);  // oldest
    CHECK(instanceToActivate(100, {{0, false}}, false) == 0);
}

TEST_CASE("packaging: log rotation") {
    const fs::path dir = freshDir("rotate");
    const fs::path log = dir / "reforce.log";

    CHECK_FALSE(app::rotateLogIfNeeded(log, 10, 2));  // missing file
    writeFile(log, "small");
    CHECK_FALSE(app::rotateLogIfNeeded(log, 10, 2));
    CHECK(fs::exists(log));

    writeFile(log, "first-generation");
    CHECK(app::rotateLogIfNeeded(log, 10, 2));
    CHECK_FALSE(fs::exists(log));
    CHECK(slurp(dir / "reforce.1.log") == "first-generation");

    writeFile(log, "second-generation");
    CHECK(app::rotateLogIfNeeded(log, 10, 2));
    CHECK(slurp(dir / "reforce.1.log") == "second-generation");
    CHECK(slurp(dir / "reforce.2.log") == "first-generation");

    writeFile(log, "third-generation");
    CHECK(app::rotateLogIfNeeded(log, 10, 2));  // keep 2: the oldest is dropped
    CHECK(slurp(dir / "reforce.1.log") == "third-generation");
    CHECK(slurp(dir / "reforce.2.log") == "second-generation");
    CHECK_FALSE(fs::exists(dir / "reforce.3.log"));
}

TEST_CASE("packaging: the log tee copies stderr into the file with timestamps") {
    const fs::path dir = freshDir("tee");
    const fs::path log = dir / "logs" / "reforce.log";
    {
        auto tee = app::LogTee::start(log, /*echo=*/false, /*maxBytes=*/1 << 20, 2);
        REQUIRE(tee);
        std::fprintf(stderr, "[reforce] hello from the test\n");
        std::fprintf(stderr, "partial line without newline");
    }  // destructor restores stderr and drains the pipe
    const std::string text = slurp(log);
    CHECK(text.find("[reforce] hello from the test\n") != std::string::npos);
    CHECK(text.find("partial line without newline\n") != std::string::npos);
    CHECK(text.rfind("20", 0) == 0);  // "2026-..." timestamp prefix
}

TEST_CASE("packaging: the log tee rotates while running") {
    const fs::path dir = freshDir("tee_rotate");
    const fs::path log = dir / "reforce.log";
    {
        auto tee = app::LogTee::start(log, false, /*maxBytes=*/200, 2);
        REQUIRE(tee);
        for (int i = 0; i < 20; ++i) std::fprintf(stderr, "line %02d: some padding text to fill the log\n", i);
    }
    CHECK(fs::exists(dir / "reforce.1.log"));
    CHECK_FALSE(fs::exists(dir / "reforce.3.log"));
    const std::string newest = slurp(log) + slurp(dir / "reforce.1.log");
    CHECK(newest.find("line 19") != std::string::npos);
    std::error_code ec;
    CHECK(fs::file_size(dir / "reforce.1.log", ec) < 400);
}
