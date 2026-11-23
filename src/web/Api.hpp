#pragma once
// Web control API (architecture §5b): the JSON layer between HTTP and the
// engine command API. Pure functions, no sockets, so they are unit-testable
// without an engine or a server.
//
//   GET  /api/library   libraryToJson(buildLibrary(scenesDir))  (built once)
//   GET  /api/state     stateToJson(EngineState)
//   POST /api/command   parseCommandJson(body) -> engine::Commands
//
// Command JSON (one object, or an array of up to 16):
//   {"action":"switch","layer":"flowfield","config":"HAL/Crop Spiral"}   quick transition
//        (optional "quick": false = slideshow transition duration)
//   {"action":"next"|"prev","layer":L}
//   {"action":"hold"|"unhold"|"togglehold","layer":L|"all"}   ("hold" takes optional "on": bool)
//   {"action":"slideshow","layer":L|"all","on":bool}
//   {"action":"interval","layer":L,"seconds":S}        slideshow interval, 0.5..3600 s
//   {"action":"duration","layer":L,"seconds":S}        slideshow transition duration, 0..600 s
//   {"action":"blend","layer":L,"mode":M}              one of the layer's blend modes
//   {"action":"theme","theme":NAME}                    "(Standard)" = all
//   {"action":"fader","layer":L,"weight":W}            crossfader takes over the weight, 0..1
//   {"action":"release","layer":L}                     crossfader released, timer resumes
//   {"action":"video","target":"layer"|"sprite","source":S}   play a video (Phase 5F); S = "file:PATH",
//        "camera[:ID]", "syphon:NAME" or a video library name (GET /api/video lists them)
//   {"action":"videostop","target":"layer"|"sprite"}
//   {"action":"videohold"}                             movie sprites loop until stopped (session only)
//   {"action":"videoset","key":K,"value":V}           colour layer setting (engine::applyVideoSetting)
//   {"action":"beatset","key":K,"value":V}            rhythm: phrases, quantize, kick zoom (engine::validateBeatSetting)
//   {"action":"look","key":K,"value":V}                rendering-quality choice (engine/Look.hpp): K = thinlines,
//        intensity, flowrounding, flowprecision, aa, linescale, sprites (or the settings key); session-only
//   {"action":"look","op":"ab"}                       A/B toggle between the two Look slots
//   {"action":"look","op":"preset","value":"gforce"|"right"}
//   {"action":"look","op":"reset"|"save","key":K}     back to settings.json / write it (key omitted = all)
//   {"command":"next:flowfield"}                       text form (engine::parseCommand)
// L = waveshape | colormap | flowfield | union | particle | sprite (aliases as parseLayer).

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/Commands.hpp"
#include "engine/Transition.hpp"

namespace reforce::web {

// Stable lower-case key for a layer ("flowfield", "union", ...).
const char* layerKey(engine::Layer l);

struct LibraryEntry {
    std::string id;     // relative path inside the package, no extension (what "switch" sends)
    std::string name;   // file stem
    bool parenthesized = false;  // "(Name)": never picked by the slideshow
    std::vector<std::string> categories;  // titles of the categories that admit it
    bool movie = false;                   // Sprite: a movie file (Movie sprites card)
    double bpm = 0;                       // movie: BPM from its .reforce.txt (beat sync, tools/dance_sync); 0 = none
    bool noColorKey = false;              // movie: ColorKey = 0 in its .reforce.txt (opaque in the Syphon alpha)
    bool beat = false;                    // Solid: its expressions read the beat or tempo (scene::usesBeat)
    std::filesystem::path path;           // the file (not sent: GET /api/sprites/image serves image sprites)
};

struct LibraryLayer {
    engine::Layer layer = engine::Layer::WaveShape;
    std::vector<std::string> categories;  // category titles defined for this type
    std::vector<LibraryEntry> configs;
    // Objects: the groups the Objects card browses (each category, in file order) and the theme that plays just
    // that group (the theme whose Object.CategoryExpr is "+<title>"; "" when there is none).
    struct Group {
        std::string name, theme;
    };
    std::vector<Group> groups;
};

struct Library {
    std::vector<LibraryLayer> layers;  // FlowField, WaveShape, ColorMap, Union, Particle, Sprite
    std::vector<std::string> themes;   // theme names (without "(Standard)")

    const LibraryLayer* find(engine::Layer l) const;
};

// Scans <scenesDir> (Packages, Themes, Categories). Empty dir -> empty library.
// `exclude`: reforce.Exclude (skipped scenes are left out).
// `externalSpritesDir`: reforce.ExternalSpritesDir joins the Sprite list ("External/..." ids, like the Director).
// `contentDir` (reforce.ContentDir) adds reforce's own solids, themes and categories; `solidDir`
// (reforce.Solid.Dir) more solids ("Extra/..." ids), in the Director's order.
Library buildLibrary(const std::filesystem::path& scenesDir, const std::string& exclude = {},
                     const std::filesystem::path& externalSpritesDir = {},
                     const std::filesystem::path& contentDir = {}, const std::filesystem::path& solidDir = {});

// Favourites (reforce.Favorites): "layer:id; layer:id", layer = the UI key
// (flowfield, waveshape, colormap, union, particle, sprite). Unknown layers and
// duplicates are dropped.
struct Favorite {
    std::string layer, id;
    bool operator==(const Favorite& o) const { return layer == o.layer && id == o.id; }
};
std::vector<Favorite> parseFavorites(const std::string& text);
std::string joinFavorites(const std::vector<Favorite>& favs);
std::string favoritesToJson(const std::vector<Favorite>& favs, bool canSave);

std::string libraryToJson(const Library& lib);

// `stable` = true leaves out the fields that change every tick (sim time,
// steps, countdowns, fps); used to detect state changes for SSE pushes.
// `stable` also leaves out transition progress/weight (continuous during a morph).
std::string stateToJson(const engine::EngineState& s, bool stable = false);
// The small, fast-changing part (audio meters, Link beat, per-layer progress and
// countdowns, video crossfade): the SSE `live` event, sent between full states.
std::string liveToJson(const engine::EngineState& s);

struct CommandParseResult {
    std::vector<engine::Command> commands;
    int status = 200;   // 400 malformed/invalid, 404 unknown config/theme
    std::string error;  // empty on success
    bool ok() const { return error.empty(); }
};

// Parses and validates a POST /api/command body. `lib` (may be null) lets it
// reject unknown configs and themes up front.
CommandParseResult parseCommandJson(std::string_view body, const Library* lib = nullptr);

// GET /api/video (Phase 5F): what the video source picker offers. Each entry
// carries the "source" text the video command takes.
struct VideoSourceList {
    struct File { std::string id, name, path; };
    struct Camera { std::string id, name; };
    struct Syphon { std::string name, app, display; bool self = false; };
    std::vector<File> files;
    std::vector<Camera> cameras;
    std::vector<Syphon> syphon;
    int cameraPermission = 0;  // 0 not asked, 1 granted, 2 denied, 3 prompt up (waiting), 4 unavailable (no usage key)
};
std::string videoSourcesToJson(const VideoSourceList& v);

// Validates one command (ranges, blend modes, names against `lib`).
// Returns "" when valid; sets *status (400/404) otherwise.
std::string validateCommand(const engine::Command& c, const Library* lib, int* status = nullptr);

}  // namespace reforce::web
