#pragma once
// Built-in HTTP control server (architecture §5b), cpp-httplib on its own
// threads. The HTTP side only reads EngineState snapshots (through the state
// provider) and pushes validated Commands onto the engine's CommandQueue; it
// never touches engine state.
//
//   GET  /                 embedded UI (src/web/ui, compiled in)
//   GET  /api/library      configs per type, categories, themes (built once)
//   GET  /api/state        EngineState snapshot as JSON
//   GET  /api/events       Server-Sent Events: "state" events on change (<= 10/s),
//                          a refresh every 2 s and a keepalive comment
//   POST /api/command      JSON command(s), see web/Api.hpp
//   GET  /api/video        video sources: files, cameras, Syphon servers (Phase 5F)
//   GET  /api/sprites/hidden   hidden sprites (reforce.Sprite.Hidden); POST changes them
//   GET  /api/sprites/image?id=ID   an image sprite's file (ids from the library only)
//   POST /api/movies/colorkey  {"id":ID,"colorKey":bool}: ColorKey in a movie sprite's .reforce.txt
//                          (engine::setMediaColorKey; ids from the library only), used from its next start
//   (POST /api/command with a Look "save" writes settings.json: setLookSaver)
//
// Security: binds 127.0.0.1 by default and then only accepts loopback Host
// headers (DNS rebinding). LAN mode binds 0.0.0.0 and requires the token on
// every /api route (?token=, X-Reforce-Token or Authorization: Bearer).
// POST bodies must be application/json (a cross-site form post cannot be).

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/Commands.hpp"
#include "web/Api.hpp"

namespace reforce::web {

struct WebServerOptions {
    std::string host = "127.0.0.1";
    int port = 8411;            // 0 = ephemeral (tests)
    std::string token;          // non-empty: required on /api routes
    double ssePollSeconds = 0.1;     // full-state change check, and the live event period unless /api/events?live=HZ
    double sseRefreshSeconds = 2.0;  // full state even without changes (countdowns, fps)
    double sseKeepaliveSeconds = 15.0;
};

using StateProvider = std::function<engine::EngineState()>;

class WebServer {
public:
    WebServer(WebServerOptions options, engine::CommandQueue& commands, StateProvider state,
              std::shared_ptr<const Library> library);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // GET /api/video (Phase 5F): the video source list, built on the HTTP
    // thread (files from a read-only library, live camera / Syphon lists).
    // Set before start(); without it the route returns empty lists.
    void setVideoSources(std::function<VideoSourceList()> provider);
    // Look "save" commands (engine/Look.hpp, {"action":"look","op":"save"}):
    // the server writes the current values (from the state, plus the Look
    // commands earlier in the same request) through `saver` (settings.json via
    // engine::mergeSettingsFile; returns "" or an error) and then queues
    // "look:saved:KEY=VALUE" so the engine knows the new saved values.
    // Without a saver, save answers 501. Set before start().
    // The same saver also makes slideshow settings from the panel permanent:
    // interval, transition, Particle/Sprite duration and the movie sprite
    // interval are written as they are posted (engine::slideshowSaveEntries);
    // without a saver they last for this run.
    void setLookSaver(std::function<std::string(const engine::PrefsTable&)> saver);
    // Favorites (control panel top bar, star in the lists). GET /api/favorites;
    // POST /api/favorites {"layer":L,"id":ID,"on":bool} changes the list and
    // saves it through `saver` (reforce.Favorites text; returns "" or an
    // error). Without a saver changes last for this run. Set before start().
    void setFavorites(const std::string& initial, std::function<std::string(const std::string&)> saver);

    // Hidden sprites (Image sprites card, Review images): never started, by the slideshow or by hand.
    // GET /api/sprites/hidden -> {"hidden":[ids],"canSave":bool}; POST /api/sprites/hidden
    // {"ids":[ids],"on":bool} hides (on) or shows them again, saves the list through `saver`
    // (reforce.Sprite.Hidden text; returns "" or an error) and queues Command::spriteHidden.
    // Without a saver changes last for this run. Set before start().
    void setSpriteHidden(const std::string& initial, std::function<std::string(const std::string&)> saver);

    // Binds synchronously (false + *error on failure), then serves on a thread.
    bool start(std::string* error = nullptr);
    void stop();
    bool running() const;
    int port() const;  // the bound port (after start)
    const WebServerOptions& options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 32 hex characters from the system CSPRNG.
std::string generateToken();

// Non-loopback IPv4 addresses of this machine (for printing LAN URLs).
std::vector<std::string> lanAddresses();

// Embedded UI file by name ("index.html"); nullptr when absent.
struct UiAsset {
    const char* name;
    const unsigned char* data;
    std::size_t size;
};
const UiAsset* findUiAsset(const std::string& name);
std::vector<std::string> uiAssetNames();

}  // namespace reforce::web
