// reforce entry point.
//
//   reforce [scene options] [audio options] [render options] [debug options]
//
// Scenes (NAME = file stem, or a relative path inside the package, or a file path):
//   --scenes DIR          scene root holding Packages/ (default: settings reforce.ScenesDir, then ./scenes,
//                         then the source tree's scenes/, then an installed G-Force's
//                         /Library/Application Support/SoundSpectrum/G-Force; none: built-in scenes only)
//   --content DIR|none    reforce's own content root (reforce.Solid.package, reforce.Organic.package, Themes/,
//                         Categories/; default:
//                         settings reforce.ContentDir, then the source tree's content/, then the app's
//                         Resources/content)
//   --flowfield NAME      --waveshape NAME      --colormap NAME (.txt or .png)
//   --seed N              RNG seed (default: 1 with --deterministic, else random)
//                         The named configs are the starting ones; the slideshow takes over from there.
// Slideshow:
//   --slideshow on|off    override <Type>.Slideshow.EnableOnStartup for all layers
//   --theme NAME          Theme filter (default: settings Theme.StartupTheme, "(Standard)" = all)
//   --blend LAYER=MODE    blend mode (waveshape=gforce|aligned|crossfade, colormap=gforce|oklab|hsv-short,
//                         flowfield=gforce|polar|crossfade); also settings reforce.Blend.<Type>
//   --set KEY=VALUE       override one setting (G-Force pref name), e.g. --set FlowField.Transition.Duration=4
// Audio:
//   --audio-device NAME   input device name or UID, or "system" for System Audio (everything the Mac plays;
//                         macOS 14.2+) (default: settings Audio.InputSource, else system default)
//   --audio-file PATH     analyze a file instead (never played out loud)
//   --no-audio            silence
// Rendering:
//   --render-size WxH     fixed render size (default 1920x1080; --frame-size is an alias)
//   --intensity gforce|hq 8-bit intensity buffer (default) or 16-bit float
//   --no-aa               no line antialiasing (default: settings WaveShape.AntiAlias)
//   --line-aa path|analytic|msaa  line AA method: per-path coverage (default), legacy per-segment
//                         analytic, or 4x MSAA (comparison only; Phase 5E)
//   --noise               start from a seeded noise image (static FlowField demo)
//   --settings PATH       settings file (default ~/Library/Application Support/reforce/settings.json);
//                         --no-settings: built-in defaults (G-Force 5.9.7 factory prefs)
//   --write-setting KEY=VALUE  merge KEY into the settings file (other keys kept) and exit; repeatable
//   --import-guests FILE  save past stream guests (a JSON list of {id, name, dancer, at}) to Guests/ next to the
//                         settings file (web/GuestBook) and exit
//                         (used by scripts/install-app.sh for reforce.ScenesDir)
//   --describe-config PATH...  print one JSON line per scene file (type, rnd/time/audio use per tier,
//                         builtins) and exit; used by tools/sweep (Phase 5B.4)
// Settings import (one-time snapshot, then exit):
//   --import-gforce-prefs [PATH]  read G-Force's prefs (read-only; default
//                         ~/Library/Preferences/SoundSpectrum/G-Force/Preferences (Standalone).txt)
//                         and merge the supported keys into the settings file
// Deterministic / offline:
//   --deterministic       fixed time step (1/fps), seeded RNG, deterministic file audio
//   --fps F               frame rate for --deterministic (default: settings Graphics.TargetFrameRate)
//   --render-frames N     render N frames offscreen (no window) and exit
//   --out DIR             write PNGs there (with --render-frames)
//   --save-frames A,B,..  0-based frames to write (default: the last one)
//   --save-every K        also write every Kth frame
//   --command T=CMD       (offline) push an engine command at sim time T, e.g. 2.5=next:flowfield,
//                         3=switch:waveshape:NAME, 4=blend:flowfield:polar, 5=fader:colormap:.3 (see Commands.hpp)
// Video (Phase 5F; architecture §5c):
//   --video SOURCE        play SOURCE on the color video layer at startup (file path, file:PATH,
//                         camera[:ID], syphon:NAME, or a video library name); settings
//                         reforce.Video.Layer.Source. Movies in the Sprite package (and in
//                         reforce.ExternalSpritesDir) are movie sprites; reforce.VideoDir adds a movie
//                         folder to the source picker. Commands: video:layer|sprite:SOURCE,
//                         videostop:layer|sprite, videoset:KEY:VALUE (see engine/Commands.hpp)
// 3D solids (PLAN 5L): scene files in reforce.Solid.package (content/), started with the command
//   switch:object:NAME (or --command T=switch:object:NAME), next:object, kill:object, or a Theme. Test hook:
//   --solid-test SPEC     a hard-coded solid from the first frame on (repeatable, up to reforce.Object.MaxLive + 1),
//                         SPEC = MESH[,key=value...], e.g. cube,rotx=.5,roty=.7,scale=.6,pen=1,flag=0,text=0
//                         (keys: render/SolidPipeline.hpp parseSolidTest; text=0|1|2, default 2 = flat)
// Dancers (PLAN 5P): scene files in reforce.Dancer.package (content/), started with switch:dancer:NAME, next:dancer,
//   kill:dancer, or a Theme. Test hook:
//   --dancer-test STYLES  built-in characters from the first frame on, one per comma-separated dance style
//                         (disco, hiphop, house, pop, robot, rave, retro, chill)
//   --syphon-name NAME    our Syphon server name (default "reforce"; never subscribed to by our own
//                         video inputs unless reforce.Syphon.AllowFeedback = 1)
// Web control UI (architecture §5b):
//   --ui-port N           HTTP port (default 8411); the UI is at http://127.0.0.1:N/
//   --ui-lan              listen on all interfaces (phone/tablet); a random token printed to the log and
//                         shown in the window title is then required (?token=...)
//   --no-ui               no web server
//   --no-link             do not join an Ableton Link session (reforce.Link.Enabled)
// App (Phase 5A; windowed runs only):
//   --allow-multiple      skip the single-instance check (default: a second launch activates the first and exits)
//   --background          a development / test instance that never disturbs a live one: never activates, its window
//                         opens behind the others and its position is not saved, no log file, its own Syphon server
//                         (reforce-dev-PID unless --syphon-name), and no Link, audio input, deck feed (rkbx_link,
//                         MIDI) or control panel unless asked for (--audio-device / --audio-file, --ui-port).
//                         Implies --allow-multiple.
//   --lights              a --background instance drives the room lights (docs/lights.md) too (quit the live one's
//                         lights first: both would fight over the panels)
//   --log-file PATH       log file (default ~/Library/Logs/reforce/reforce.log, rotated at 4 MB, 3 kept);
//                         stderr is copied there, and still printed when it is a terminal. --no-log-file: off
// Debug: --quit-after SEC  --minimize-after SEC  --fullscreen-after SEC
// Performance (Phase 5D):
//   --stress              switch all layers every ~2 s (random blend modes, interrupted transitions),
//                         spawn particles/sprites/solids; --stress-interval SEC changes the period
//   --perf-report SEC     run SEC seconds after a 3 s warm-up, quit, print the frame-time report on stdout
//                         (p50/p99/max, histogram, hitches by kind and cause)
//   --eval-threads N      WaveShape step/morph evaluation workers besides the render thread
//                         (default: performance cores - 2; 0 = serial, as before Phase 5D)
//
// Keys: W / C / F next WaveShape / ColorMap / FlowField (Shift: previous), U next FlowField Union
// (used by the next FlowField transition), H hold/unhold the slideshow on all layers, 1 / 2 / 3 cycle the
// WaveShape / ColorMap / FlowField blend mode, P / S start the next particle / sprite (Shift: kill them all).
// Cmd+Ctrl+F toggles fullscreen, Esc leaves it, Cmd+Q quits.

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <random>
#include <string>

#include "app/App.hpp"
#include "app/Describe.hpp"
#include "app/AudioDriver.hpp"
#include "app/Director.hpp"
#include "app/LogFile.hpp"
#include "app/Offline.hpp"
#include "app/Visualizer.hpp"
#include "render/SolidPipeline.hpp"
#include "audio/MicPermission.hpp"
#include "audio/SystemAudioTap.hpp"
#include "engine/ModelScenes.hpp"
#include "engine/Commands.hpp"
#include "engine/DanceClips.hpp"
#include "engine/Prefs.hpp"
#include "engine/SceneLibrary.hpp"
#include "platform/Platform.hpp"
#include "video/VideoSource.hpp"
#include "web/GuestBook.hpp"
#include "web/Guests.hpp"
#include "web/WebServer.hpp"

#ifndef REFORCE_SOURCE_DIR
#define REFORCE_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using reforce::scene::SceneType;

void usage() {
    std::fprintf(stderr,
                 "usage: reforce [--scenes DIR] [--content DIR|none] [--flowfield NAME] [--waveshape NAME] [--colormap NAME] [--seed N]\n"
                 "               [--slideshow on|off] [--theme NAME] [--blend LAYER=MODE] [--set KEY=VALUE]\n"
                 "               [--audio-device NAME | --audio-file PATH | --no-audio]\n"
                 "               [--render-size WxH] [--intensity gforce|hq] [--no-aa]\n"
                 "               [--line-aa path|analytic|msaa] [--noise]\n"
                 "               [--settings PATH | --no-settings] [--import-gforce-prefs [PATH]]\n"
                 "               [--deterministic] [--fps F] [--render-frames N] [--out DIR]\n"
                 "               [--save-frames A,B,...] [--save-every K]\n"
                 "               [--video SOURCE] [--syphon-name NAME] [--solid-test SPEC] [--dancer-test STYLES]\n"
                 "               [--ui-port N] [--ui-lan] [--no-ui] [--no-link]\n"
                 "               [--write-setting KEY=VALUE] [--allow-multiple] [--background] [--lights]\n"
                 "               [--log-file PATH | --no-log-file]\n"
                 "               [--quit-after SEC] [--minimize-after SEC] [--fullscreen-after SEC]\n"
                 "               [--stress] [--stress-interval SEC] [--perf-report SEC] [--eval-threads N]\n"
                 "       reforce --describe-config PATH...\n");
}

// reforce's own content root: --content (none = off), else settings reforce.ContentDir, else the source tree's
// content/ (a development build: scene edits show without a rebuild), else the copy bundled in the app's
// Resources/content (an app built elsewhere, or a source tree that has moved).
fs::path resolveContentDir(const std::string& cli, const std::string& fromSettings) {
    if (cli == "none") return {};
    if (!cli.empty() || !fromSettings.empty()) {
        const fs::path p = !cli.empty() ? fs::path(cli) : fs::path(fromSettings);
        if (reforce::engine::packagesRoot(p)) return p;
        std::fprintf(stderr, "[reforce] content %s has no Packages/; no reforce content\n", p.string().c_str());
        return {};
    }
    if (const fs::path p = fs::path(REFORCE_SOURCE_DIR) / "content"; reforce::engine::packagesRoot(p)) return p;
    if (const std::string res = reforce::platform::bundleResourcesDir(); !res.empty())
        if (const fs::path p = fs::path(res) / "content"; reforce::engine::packagesRoot(p)) return p;
    return {};
}

// Where a G-Force install keeps its scenes (read only: reforce never writes under a scene root).
constexpr const char* kGForceInstallDir = "/Library/Application Support/SoundSpectrum/G-Force";

fs::path defaultScenesDir() {
    std::error_code ec;
    for (const fs::path& p : {fs::current_path(ec) / "scenes", fs::path(REFORCE_SOURCE_DIR) / "scenes",
                              fs::path(kGForceInstallDir)})
        if (reforce::engine::packagesRoot(p)) {
            std::fprintf(stderr, "[reforce] scenes: %s\n", p.string().c_str());
            return p;
        }
    return fs::path("scenes");
}

// Scene root: --scenes, else settings reforce.ScenesDir (if it holds packages), else the dev-path search.
fs::path resolveScenesDir(const std::string& cli, const std::string& fromSettings) {
    if (!cli.empty()) return fs::path(cli);
    if (!fromSettings.empty()) {
        if (reforce::engine::packagesRoot(fromSettings)) {
            std::fprintf(stderr, "[reforce] scenes: %s (settings reforce.ScenesDir)\n", fromSettings.c_str());
            return fs::path(fromSettings);
        }
        std::fprintf(stderr, "[reforce] settings reforce.ScenesDir=%s has no scene packages; searching defaults\n",
                     fromSettings.c_str());
    }
    return defaultScenesDir();
}

}  // namespace

int main(int argc, char** argv) {
    using namespace reforce;

    platform::DebugOptions debug;
    app::AppOptions options;
    app::OfflineOptions offline;
    std::string contentDirArg;  // --content
    std::string importGuestsFile;  // --import-guests
    std::string scenesDir, ffName, wsName, cmName, audioDevice, audioFile, prefsPath, intensity;  // empty: settings reforce.Intensity
    bool haveSeed = false, deterministic = false, noAudio = false, noPrefs = false, noAa = false, noise = false,
         haveDevice = false, haveFps = false;
    std::uint64_t seed = 1;
    render::LineAa lineAa = render::LineAa::PerPath;
    int renderFrames = 0;
    int evalThreads = -1;
    bool importPrefs = false;
    std::string importPath;
    double fps = 60.0;
    std::optional<bool> slideshow;
    bool uiEnabled = true, uiLan = false, noLink = false;
    int uiPort = 8411;
    std::string theme, videoSource, syphonName = "reforce";
    std::vector<render::SolidTestSpec> solidTests;  // --solid-test (phase-2 test hook)
    std::vector<std::string> dancerTests;            // --dancer-test (PLAN 5P test hook)
    std::vector<std::pair<std::string, std::string>> blends, sets, writeSettings;
    bool lightsFlag = false;  // --lights: a --background instance drives the room lights too
    bool allowMultiple = false, logToFile = true, background = false, haveSyphonName = false, haveUiPort = false;
    std::string logPath;
    auto splitKv = [](const std::string& s, std::pair<std::string, std::string>& out) {
        const auto eq = s.find('=');
        if (eq == std::string::npos || eq == 0) return false;
        out = {s.substr(0, eq), s.substr(eq + 1)};
        return true;
    };

    if (argc > 1 && std::strcmp(argv[1], "--describe-config") == 0) {
        if (argc < 3) { usage(); return 2; }
        return app::describeConfigs(std::vector<std::string>(argv + 2, argv + argc));
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        auto value = [&]() -> std::string { return argv[++i]; };
        if (arg == "--quit-after" && hasValue) debug.quitAfter = std::atof(argv[++i]);
        else if (arg == "--minimize-after" && hasValue) debug.minimizeAfter = std::atof(argv[++i]);
        else if (arg == "--fullscreen-after" && hasValue) debug.fullscreenAfter = std::atof(argv[++i]);
        else if (arg == "--stress") options.stressInterval = options.stressInterval > 0 ? options.stressInterval : 2.0;
        else if (arg == "--stress-interval" && hasValue) options.stressInterval = std::atof(argv[++i]);
        else if (arg == "--eval-threads" && hasValue) evalThreads = std::atoi(argv[++i]);
        else if (arg == "--perf-report" && hasValue) {
            options.perfReport = std::atof(argv[++i]);
            if (options.perfReport <= 0) { usage(); return 2; }
        }
        else if ((arg == "--frame-size" || arg == "--render-size") && hasValue) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h || w > 16384 || h > 16384) {
                usage();
                return 2;
            }
            options.frameWidth = w;
            options.frameHeight = h;
        } else if (arg == "--scenes" && hasValue) scenesDir = value();
        else if (arg == "--content" && hasValue) contentDirArg = value();
        else if (arg == "--flowfield" && hasValue) ffName = value();
        else if (arg == "--waveshape" && hasValue) wsName = value();
        else if (arg == "--colormap" && hasValue) cmName = value();
        else if (arg == "--slideshow" && hasValue) {
            const std::string v = value();
            if (v != "on" && v != "off") { usage(); return 2; }
            slideshow = v == "on";
        } else if (arg == "--theme" && hasValue) theme = value();
        else if (arg == "--import-guests" && hasValue) importGuestsFile = value();
        else if ((arg == "--blend" || arg == "--set" || arg == "--write-setting") && hasValue) {
            std::pair<std::string, std::string> kv;
            if (!splitKv(value(), kv)) { usage(); return 2; }
            (arg == "--blend" ? blends : arg == "--set" ? sets : writeSettings).push_back(kv);
        }
        else if (arg == "--allow-multiple") allowMultiple = true;
        else if (arg == "--background") background = true;
        else if (arg == "--lights") lightsFlag = true;
        else if (arg == "--video" && hasValue) videoSource = value();
        else if (arg == "--dancer-test" && hasValue) {  // test hook (PLAN 5P)
            std::string v = value(), cur;
            for (char c : v + ",") {
                if (c == ',') {
                    if (!cur.empty()) dancerTests.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
        }
        else if (arg == "--solid-test" && hasValue) {  // test hook (PLAN 5L.2)
            render::SolidTestSpec st;
            std::string err;
            const std::string v = value();
            if (!render::parseSolidTest(v, st, &err)) {
                std::fprintf(stderr, "[reforce] --solid-test %s: %s\n", v.c_str(), err.c_str());
                return 2;
            }
            solidTests.push_back(st);
        }
        else if (arg == "--syphon-name" && hasValue) { syphonName = value(); haveSyphonName = true; }
        else if (arg == "--log-file" && hasValue) logPath = value();
        else if (arg == "--no-log-file") logToFile = false;
        else if (arg == "--command" && hasValue) {
            const std::string v = value();
            const auto eq = v.find('=');
            std::string err;
            std::optional<engine::Command> c;
            if (eq != std::string::npos) c = engine::parseCommand(v.substr(eq + 1), &err);
            if (!c) {
                std::fprintf(stderr, "[reforce] --command %s: %s\n", v.c_str(), err.empty() ? "expected T=CMD" : err.c_str());
                return 2;
            }
            offline.commands.push_back({std::atof(v.substr(0, eq).c_str()), *c});
        }
        else if (arg == "--seed" && hasValue) { seed = std::strtoull(argv[++i], nullptr, 10); haveSeed = true; }
        else if (arg == "--audio-device" && hasValue) { audioDevice = value(); haveDevice = true; }
        else if (arg == "--audio-file" && hasValue) audioFile = value();
        else if (arg == "--no-audio") noAudio = true;
        else if (arg == "--no-ui") uiEnabled = false;
        else if (arg == "--no-link") noLink = true;
        else if (arg == "--ui-lan") uiLan = true;
        else if (arg == "--ui-port" && hasValue) {
            uiPort = std::atoi(argv[++i]);
            if (uiPort <= 0 || uiPort > 65535) { usage(); return 2; }
            haveUiPort = true;
        }
        else if (arg == "--intensity" && hasValue) intensity = value();
        else if (arg == "--no-aa") noAa = true;
        else if (arg == "--line-aa" && hasValue) {
            if (!render::parseLineAa(value(), lineAa)) { usage(); return 2; }
        }
        else if (arg == "--noise") noise = true;
        else if (arg == "--settings" && hasValue) prefsPath = value();
        else if (arg == "--no-settings") noPrefs = true;
        else if (arg == "--import-gforce-prefs") {
            importPrefs = true;
            if (hasValue && std::strncmp(argv[i + 1], "--", 2) != 0) importPath = value();
        }
        else if (arg == "--deterministic") deterministic = true;
        else if (arg == "--fps" && hasValue) { fps = std::atof(argv[++i]); haveFps = true; }
        else if (arg == "--render-frames" && hasValue) renderFrames = std::atoi(argv[++i]);
        else if (arg == "--out" && hasValue) offline.outDir = value();
        else if (arg == "--save-every" && hasValue) offline.saveEvery = std::atoi(argv[++i]);
        else if (arg == "--save-frames" && hasValue) {
            std::string s = value();
            for (char* p = s.data(); *p;) {
                offline.saveFrames.insert(int(std::strtol(p, &p, 10)));
                if (*p) ++p;
            }
        } else if (arg.rfind("-psn_", 0) == 0) {
            // Finder/LaunchServices process serial number; ignore.
        } else {
            usage();
            return 2;
        }
    }
    if (!intensity.empty() && intensity != "gforce" && intensity != "hq") { usage(); return 2; }

    const std::string settingsPath = prefsPath.empty() ? engine::defaultSettingsPath() : prefsPath;
    if (!importGuestsFile.empty()) {
        std::FILE* f = std::fopen(importGuestsFile.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "[reforce] --import-guests: can't open %s\n", importGuestsFile.c_str());
            return 1;
        }
        std::string text;
        char buf[65536];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
        web::GuestBook book(fs::path(settingsPath).parent_path() / "Guests");
        std::string err;
        const int n = web::importGuests(text, book, &err);
        if (n < 0) {
            std::fprintf(stderr, "[reforce] --import-guests: %s\n", err.c_str());
            return 1;
        }
        std::printf("%d guest(s) saved in %s\n", n, book.dir().string().c_str());
        for (const auto& g : book.guests()) std::printf("  %s (%s)\n", g.label.c_str(), g.style.c_str());
        return 0;
    }
    if (!writeSettings.empty()) {
        std::string err;
        if (!engine::mergeSettingsFile(settingsPath, writeSettings, &err)) {
            std::fprintf(stderr, "[reforce] --write-setting: %s\n", err.c_str());
            return 1;
        }
        for (const auto& [k, v] : writeSettings) std::printf("%s: %s = %s\n", settingsPath.c_str(), k.c_str(), v.c_str());
        return 0;
    }
    if (background) {  // a dev / test instance: leave a live reforce (and its OBS output) alone
        allowMultiple = true;
        logToFile = false;
        noLink = true;
        if (!haveDevice && audioFile.empty()) noAudio = true;
        if (!haveUiPort) uiEnabled = false;
        if (!haveSyphonName) syphonName = "reforce-dev-" + std::to_string(int(getpid()));
    }
    const bool windowed = renderFrames <= 0 && !importPrefs;
    // Log file: a Dock launch has no terminal, so stderr goes to ~/Library/Logs/reforce/.
    std::unique_ptr<app::LogTee> logTee;
    if (windowed && logToFile) {
        logTee = app::LogTee::start(logPath.empty() ? app::defaultLogPath() : fs::path(logPath), isatty(STDERR_FILENO));
        if (logTee) {
            std::fprintf(stderr, "[reforce] ---- launch (pid %d); log %s\n", int(getpid()), logTee->path().c_str());
        } else {
            std::fprintf(stderr, "[reforce] cannot open the log file; logging to stderr only\n");
        }
    }
    // Single instance (Dock/Finder launches, or the binary run again): hand over to the running one.
    if (windowed && platform::activateRunningInstance(allowMultiple)) return 0;
    if (importPrefs) {
        // One-time snapshot of G-Force's prefs (read-only) into our settings file.
        const std::string src = importPath.empty() ? engine::defaultGForcePrefsPath() : importPath;
        const engine::ImportResult r = engine::importGForcePrefs(src, settingsPath);
        if (!r.ok) {
            std::fprintf(stderr, "[reforce] import failed: %s\n", r.error.c_str());
            return 1;
        }
        std::printf("Imported %zu G-Force pref(s) from\n  %s\ninto\n  %s\n", r.imported.size(), src.c_str(),
                    settingsPath.c_str());
        for (const auto& [k, v] : r.imported) std::printf("  %-50s = %s\n", k.c_str(), v.c_str());
        if (!r.missing.empty()) {
            std::printf("Not present in the G-Force file (built-in defaults apply):\n");
            for (const auto& k : r.missing) std::printf("  %s\n", k.c_str());
        }
        return 0;
    }

    // reforce settings (never G-Force's files at runtime).
    engine::Settings prefs;
    // A settings-free run (tests, --no-settings) reads no user model folder unless --set gives one.
    if (noPrefs) prefs.modelDir = "none";
    if (!noPrefs) {
        std::string err;
        prefs = engine::loadSettings(settingsPath, &err);
        if (!err.empty()) std::fprintf(stderr, "[reforce] settings: %s (using defaults)\n", err.c_str());
        std::fprintf(stderr, "[reforce] settings: %s%s\n", prefs.path.c_str(),
                     prefs.loaded ? "" : " (not found; built-in G-Force defaults)");
    }
    if (!sets.empty()) {
        engine::PrefsTable overrides;
        for (const auto& [k, v] : sets) overrides.set(k, v, false);
        prefs.apply(overrides);
    }
    for (const auto& [layerName, mode] : blends) {
        const auto layer = engine::parseLayer(layerName);
        if (!layer || !engine::findBlendMode(*layer, mode)) {
            std::fprintf(stderr, "[reforce] --blend %s=%s: unknown layer or mode\n", layerName.c_str(), mode.c_str());
            return 2;
        }
        if (*layer == engine::Layer::WaveShape) prefs.blendWaveShape = mode;
        else if (*layer == engine::Layer::ColorMap) prefs.blendColorMap = mode;
        else if (*layer == engine::Layer::FlowField) prefs.blendFlowField = mode;
    }
    if (!theme.empty()) prefs.startupTheme = theme;
    if (!videoSource.empty()) prefs.videoStartupSource = videoSource;
    if (intensity.empty()) {
        intensity = prefs.intensity;
        if (intensity != "gforce" && intensity != "hq") {
            std::fprintf(stderr, "[reforce] settings reforce.Intensity = \"%s\": expected gforce or hq; using gforce\n",
                         intensity.c_str());
            intensity = "gforce";
        }
    }
    if (!haveFps && prefs.targetFrameRate > 0) fps = prefs.targetFrameRate;
    if (fps <= 0) fps = 60;
    const bool offscreen = renderFrames > 0;
    if (offscreen) deterministic = true;
    if (!haveSeed) seed = deterministic ? 1 : std::random_device{}();

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (!device) {
        std::fprintf(stderr, "[reforce] no Metal device\n");
        return 1;
    }

    int rc = 0;
    try {
        app::VisualizerOptions vo;
        // reforce.Supersample (live runs only): everything renders at 2x the output size; the App averages it down.
        const int supersample = !offscreen && (prefs.supersample == "2x" || prefs.supersample == "2") ? 2 : 1;
        vo.width = options.frameWidth * uint32_t(supersample);
        vo.height = options.frameHeight * uint32_t(supersample);
        vo.supersample = supersample;
        options.supersample = supersample;
        vo.seed = seed;
        vo.prefs = prefs;
        vo.pipeline.format = intensity == "hq" ? render::IntensityFormat::Half16 : render::IntensityFormat::GForce8;
        vo.pipeline.antialias = prefs.antiAlias && !noAa;
        vo.pipeline.colorEdges = prefs.lineEdges == "color";
        vo.pipeline.lineAa = lineAa;
        if (prefs.thinLines == "gforce") vo.pipeline.thinLines = render::ThinLines::GForce;
        // (other values: reported with the other Look settings, engine::lookSettingProblems)
        {
            const std::string& r = prefs.flowRounding;
            vo.pipeline.flowRounding = r == "nearest"  ? render::FlowRounding::Nearest
                                     : r == "point"    ? render::FlowRounding::PointFloor
                                     : r == "floor"    ? render::FlowRounding::Floor
                                     : r == "fixed256" ? render::FlowRounding::Fixed256Floor
                                                       : render::FlowRounding::Fixed16Floor;
        }
        vo.syncBuilds = deterministic;  // FlowField builds finish inside the switch: reproducible frames
        vo.network = !deterministic && !background;  // stream guests (web/Guests): never offline or in a test instance
        // Room lights (docs/lights.md): the live instance's; a test instance only with --lights (both would fight).
        if (windowed && !deterministic && (!background || lightsFlag))
            vo.lightsConfig = fs::path(settingsPath).parent_path() / "nanoleaf.json";
        vo.evalThreads = evalThreads;
        vo.solidTests = solidTests;
        vo.dancerTests = dancerTests;
        auto vis = std::make_unique<app::Visualizer>(device, vo);

        // Scenes: the CLI names are the starting configs; otherwise the slideshow
        // picks (or the first reference scene of each type), else built-in.
        const fs::path root = resolveScenesDir(scenesDir, prefs.scenesDir);
        const bool haveRoot = engine::packagesRoot(root).has_value();
        if (!haveRoot) std::fprintf(stderr, "[reforce] no scene packages under %s; using built-in scenes\n",
                                    root.string().c_str());
        app::DirectorOptions dopts;
        if (haveRoot) dopts.scenesDir = root;
        dopts.contentDir = resolveContentDir(contentDirArg, prefs.contentDir);
        // Mocap dance moves (tools/aist_import): before any dancer starts and before the panel's library is built.
        if (!dopts.contentDir.empty()) {
            const fs::path moves = dopts.contentDir / "Moves" / "aist.moves";
            std::error_code ec;
            if (fs::exists(moves, ec)) {
                const std::string err = engine::dance::loadClips(moves.string());
                if (err.empty()) std::fprintf(stderr, "[reforce] %d mocap dance moves from %s\n", engine::dance::clipCount(), moves.string().c_str());
                else std::fprintf(stderr, "[reforce] mocap dance moves not loaded: %s\n", err.c_str());
            }
        }
        dopts.slideshow = slideshow;
        dopts.seed = seed;
        dopts.simRate = fps;
        dopts.flowField = ffName;
        dopts.waveShape = wsName;
        dopts.colorMap = cmName;
        auto director = std::make_unique<app::Director>(std::move(vis), prefs, dopts);
        if (!director->start()) rc = 2;

        // Audio.
        std::unique_ptr<app::AudioDriver> audio;
        std::shared_ptr<std::atomic<int>> micAnswer;  // set while the mic permission prompt is up
        std::string deferredDevice;
        // The Audio menu switches inputs live, so a window run always has a driver
        // (silent until a device is chosen). Settings Audio.InputSource "none" = silence.
        const std::string dev = haveDevice ? audioDevice : prefs.inputSource;
        if (rc == 0 && (noAudio || dev == "none") && !offscreen && audioFile.empty())
            audio = std::make_unique<app::AudioDriver>(app::analyzerConfigFromPrefs(prefs));
        else if (rc == 0 && !noAudio) {
            audio = std::make_unique<app::AudioDriver>(app::analyzerConfigFromPrefs(prefs));
            if (!audioFile.empty()) {
                if (!audio->openFile(audioFile, deterministic, !offscreen)) rc = 1;
            } else if (offscreen) {
                // Offline without a file: silence (deterministic).
            } else if (dev == audio::kSystemAudioId) {
                // System Audio: no microphone permission involved (macOS asks for its own,
                // "System Audio Recording Only"). Opened from the frame loop on the audio control
                // thread, so a permission prompt never holds up the window.
                micAnswer = std::make_shared<std::atomic<int>>(1);
                deferredDevice = dev;
            } else {
                // Live input: ask for microphone permission first (TCC).
                const audio::MicPermission perm = audio::microphonePermission();
                std::fprintf(stderr, "[reforce] microphone permission: %s\n", audio::micPermissionName(perm));
                if (perm == audio::MicPermission::NotDetermined) {
                    // First launch: CoreAudio would block in AudioComponentInstanceNew until the user
                    // answers the system prompt, so start silent with the window up and open the
                    // device from the frame loop once the answer is in (App::deferAudioInput).
                    micAnswer = std::make_shared<std::atomic<int>>(0);
                    audio::requestMicrophonePermission([answer = micAnswer](bool granted) {
                        std::fprintf(stderr, "[reforce] microphone permission %s\n", granted ? "granted" : "denied");
                        answer->store(granted ? 1 : 2);
                    });
                    deferredDevice = dev.empty() ? std::string("default") : dev;
                } else if (!audio->openDevice(dev == "default" ? std::string() : dev)) {
                    std::fprintf(stderr, "[reforce] no audio input; running silent\n");
                }
            }
            if (audio) std::fprintf(stderr, "[reforce] audio: %s\n", audio->description().c_str());
        }

        if (rc == 0) {
            if (offscreen) {
                offline.frames = renderFrames;
                offline.fps = fps;
                offline.seedNoise = noise;
                rc = app::runOffline(device, options.frameWidth, options.frameHeight, offline, *director, audio.get());
            } else {
                options.syphonName = syphonName.c_str();
                options.fixedStep = deterministic;
                options.link = prefs.linkEnabled && !noLink && !deterministic;
                options.linkOffset = prefs.linkOffsetMs / 1000.0;
                options.linkQuantum = prefs.linkQuantum;
                options.linkFollow = prefs.linkFollow;
                options.linkDriveBeat = prefs.linkDriveBeat;
                options.deckFeed = !deterministic && !background;
                options.deckFeedPort = prefs.phraseRkbxPort;
                options.deckFeedMidi = prefs.phraseMidiDevice;
                // Controller maps: reforce's own (content/Controllers), then the user's next to settings.json.
                if (!dopts.contentDir.empty()) options.controllerDirs.push_back(dopts.contentDir / "Controllers");
                options.controllerDirs.push_back(fs::path(settingsPath).parent_path() / "Controllers");
                options.fps = fps;
                options.seedNoise = noise;
                if (options.perfReport > 0) debug.quitAfter = options.perfWarmup + options.perfReport;
                app::Director* dir = director.get();  // owned by the App from here on
                app::App application(device, options, std::move(director), std::move(audio));
                if (micAnswer) application.deferAudioInput(deferredDevice, micAnswer);
                if (!noPrefs) {
                    application.setAudioInputChanged([settingsPath](const std::string& id) {
                        std::string err;
                        if (!engine::mergeSettingsFile(settingsPath, {{"Audio.InputSource", id}}, &err))
                            std::fprintf(stderr, "[reforce] saving Audio.InputSource: %s\n", err.c_str());
                    });
                }
                platform::WindowConfig window;
                window.startFullScreen = prefs.startFullScreen && !background;
                window.screen = background ? std::string() : prefs.windowScreen;
                window.openPanelOnLaunch = prefs.openPanelOnLaunch && !background;
                if (!noPrefs && !background)
                    window.saveStartSetup = [settingsPath](const platform::WindowConfig::StartSetup& s,
                                                           std::string* err) {
                        engine::PrefsTable t;
                        t.set("reforce.Window.Screen", s.screen, true);
                        t.set("reforce.Window.StartFullScreen", s.fullScreen ? "1" : "0", false);
                        t.set("reforce.UI.OpenOnLaunch", s.openPanel ? "1" : "0", false);
                        return engine::mergeSettingsFile(settingsPath, t, err);
                    };
                window.background = background;
                window.targetFrameRate = deterministic ? 0.0 : fps;
                // Web control UI: its threads only read snapshots and push commands.
                std::unique_ptr<web::WebServer> ui;
                if (uiEnabled) {
                    web::WebServerOptions wo;
                    wo.port = uiPort;
                    if (uiLan) {
                        wo.host = "0.0.0.0";
                        wo.token = web::generateToken();
                    }
                    auto library = std::make_shared<const web::Library>(
                        web::buildLibrary(haveRoot ? root : fs::path(), prefs.exclude,
                                          prefs.externalSpritesDir.empty() ? fs::path() : fs::path(prefs.externalSpritesDir),
                                          dopts.contentDir,
                                          prefs.solidDir.empty() ? fs::path() : fs::path(prefs.solidDir),
                                          engine::modelFolder(prefs.modelDir)));
                    // Video source picker: the Director's video library is read-only
                    // after construction; cameras and Syphon servers are listed live.
                    auto videoSources = [dir] {
                        web::VideoSourceList v;
                        for (const auto& f : dir->videoFiles()) v.files.push_back({f.id, f.name, f.path.string()});
                        for (const auto& c : video::listCameras()) v.cameras.push_back({c.id, c.name});
                        for (const auto& srv : video::listSyphonServers())
                            v.syphon.push_back({srv.name, srv.app, video::syphonDisplayName(srv), srv.self});
                        v.cameraPermission = video::cameraPermission();
                        return v;
                    };
                    ui = std::make_unique<web::WebServer>(
                        wo, dir->commands(),
                        [dir, &application] {
                            engine::EngineState s = dir->snapshot();
                            s.displayFps = application.displayFps();
                            const perf::PerfSummary ps = application.perfSummary();
                            if (ps.frames > 0) {
                                s.perf.valid = true;
                                s.perf.frames = ps.frames;
                                s.perf.hitches = ps.hitches;
                                s.perf.renderAllocs = ps.renderAllocs;
                                s.perf.workP50 = ps.workP50;
                                s.perf.workP99 = ps.workP99;
                                s.perf.workMax = ps.workMax;
                                s.perf.gpuP99 = ps.gpuP99;
                                s.perf.budgetMs = ps.budgetMs;
                                s.perf.displayHz = ps.displayHz;
                                s.perf.histEdges = ps.histEdges;
                                s.perf.hist = ps.hist;
                                s.perf.lastHitch = ps.lastHitch;
                            }
                            return s;
                        },
                        std::move(library));
                    ui->setVideoSources(videoSources);
                    // Favorites (control panel top bar): saved to settings.json unless --no-settings.
                    ui->setFavorites(prefs.favorites,
                                     noPrefs ? std::function<std::string(const std::string&)>()
                                             : [settingsPath](const std::string& text) {
                                                   std::string err;
                                                   if (!engine::mergeSettingsFile(settingsPath,
                                                                                  {{"reforce.Favorites", text}}, &err))
                                                       return err;
                                                   return std::string();
                                               });
                    // Hidden sprites (Image sprites card, Review images): saved like the favorites.
                    ui->setSpriteHidden(prefs.spriteHidden,
                                        noPrefs ? std::function<std::string(const std::string&)>()
                                                : [settingsPath](const std::string& text) {
                                                      std::string err;
                                                      if (!engine::mergeSettingsFile(
                                                              settingsPath, {{"reforce.Sprite.Hidden", text}}, &err))
                                                          return err;
                                                      return std::string();
                                                  });
                    if (!noPrefs)  // Look panel "Save as default" (engine/Look.hpp)
                        ui->setLookSaver([settingsPath](const engine::PrefsTable& t) {
                            std::string err;
                            return engine::mergeSettingsFile(settingsPath, t, &err) ? std::string() : err;
                        });
                    std::string err;
                    if (ui->start(&err)) {
                        const std::string q = wo.token.empty() ? std::string() : "/?token=" + wo.token;
                        std::fprintf(stderr, "[reforce] web UI: http://127.0.0.1:%d%s\n", ui->port(), q.c_str());
                        window.controlPanelUrl =
                            "http://127.0.0.1:" + std::to_string(ui->port()) + (q.empty() ? "/" : q);
                        if (uiLan) {
                            for (const auto& ip : web::lanAddresses())
                                std::fprintf(stderr, "[reforce] web UI (LAN): http://%s:%d%s\n", ip.c_str(),
                                             ui->port(), q.c_str());
                            window.title += " — UI token " + wo.token;
                        }
                    } else {
                        std::fprintf(stderr, "[reforce] web UI disabled: %s\n", err.c_str());
                        ui.reset();
                    }
                }
                // Syphon's server directory listens for announcements on the main run loop.
                video::initSyphonDirectory();
                rc = platform::runApplication(window, device, application, debug);
                ui.reset();  // before the App (and its Director) go away
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[reforce] fatal: %s\n", e.what());
        rc = 1;
    }
    device->release();
    pool->release();
    return rc;
}
