#pragma once
// Director: the slideshow and the command API on top of the Visualizer.
// Runs on the render/sim thread; one step() per simulation tick.
//
//   - scene catalog per layer (WaveShape, ColorMap, FlowField, FlowField Union)
//     from the Packages tree; Theme/Category filtering; parenthesized names
//     never enter the shuffle (scene-format §5.2, §8)
//   - per-layer slideshow timers (interval from the end of one change to the
//     start of the next, relative-deviation sampling), shuffled playlists
//   - Unions: every `FlowField Union.Slideshow.Interval.Duration` seconds the
//     next FlowField transition uses the next Union (scene-format §7)
//   - FlowField preload: the next slideshow field is built while the current
//     one plays, so its transition can start on time (ConfigPrepTime idea)
//   - command queue (hotkeys now, web UI in M5b) and an EngineState snapshot
//   - Particles/Sprites (M6): own playlists (Theme-filtered like the rest);
//     the slideshow spawns one every <Type>.Slideshow.Interval.Duration
//     (+- Deviation, counted from spawn to spawn); several may be alive.
//     Lifetime = fade-in, Duration/PDur, fade-out (probe #19).
//     Movie sprites have their own shuffle and timer (reforce.MovieSprite.
//     Slideshow.Interval.Duration); the Sprite timer starts images only.
//   - Solids (PLAN 5L.4): a spawn layer like Particles, from reforce.Solid.package
//     (scenesDir and contentDir) and reforce.Solid.Dir. Not in the default
//     slideshow (Object.Slideshow.Interval.Duration 0); a Theme may set
//     Object.Slideshow.Interval.Duration to start them while it is on.

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/Visualizer.hpp"
#include "engine/Commands.hpp"
#include "engine/Prefs.hpp"
#include "engine/SceneLibrary.hpp"
#include "engine/Scheduler.hpp"
#include "engine/Themes.hpp"
#include "engine/Transition.hpp"

namespace MTL { class CommandBuffer; class Texture; }
namespace reforce::audio { struct AudioFrame; }

namespace reforce::app {

struct DirectorOptions {
    std::filesystem::path scenesDir;    // scene root (Packages/, Themes/, Categories/); may be empty
    // reforce's own content root, same layout (reforce.Solid.package, Themes/, Categories/; reforce.ContentDir,
    // default the source tree's content/): its solids, themes and categories join scenesDir's. May be empty.
    std::filesystem::path contentDir;
    std::optional<bool> slideshow;      // --slideshow on|off overrides <Type>.Slideshow.EnableOnStartup
    std::uint64_t seed = 1;
    double simRate = 60.0;              // Graphics.TargetFrameRate
    // Starting configs (name, relative path or file path; "" = slideshow pick
    // or the reference scene; "builtin" = our built-in scene).
    std::string flowField, waveShape, colorMap;
};

class Director {
public:
    Director(std::unique_ptr<Visualizer> vis, const engine::Settings& prefs, const DirectorOptions& opts);

    // Loads the starting configs (cuts). Returns false if an explicitly named
    // config was not found.
    bool start();

    engine::CommandQueue& commands() { return commands_; }
    // Thread-safe snapshot (updated every step).
    engine::EngineState snapshot() const { return state_.snapshot(); }

    // One simulation tick at sim time t: commands, slideshow, visualizer step.
    void step(MTL::CommandBuffer* cmd, double t, const audio::AudioFrame* audio);
    void colorize(MTL::CommandBuffer* cmd, MTL::Texture* output, bool withVideo = true, bool trackAlpha = false,
                  bool backdrop = false, MTL::Texture* visualsCopy = nullptr, bool overlays = true);

    Visualizer& visualizer() { return *vis_; }
    // Video library (Phase 5F; built once, read-only afterwards, so any thread
    // may read it): movies in the Sprite package (ids "Sprites/..."), in
    // reforce.ExternalSpritesDir ("External/...") and reforce.VideoDir ("Videos/...").
    const std::vector<engine::SceneEntry>& videoFiles() const { return videoFiles_; }
    // A video source from command text: "file:PATH", "camera[:ID]",
    // "syphon:NAME", a video library name/id, or an existing file path.
    std::optional<video::SourceSpec> resolveVideo(const std::string& text, std::string* error = nullptr) const;
    std::size_t catalogSize(engine::Layer layer) const;
    std::size_t playlistSize(engine::Layer layer) const;

private:
    struct LayerData {
        engine::Layer layer = engine::Layer::WaveShape;
        scene::SceneType type = scene::SceneType::WaveShape;
        std::vector<engine::SceneEntry> catalog;
        engine::Playlist playlist;
        engine::LayerSlideshow slideshow;
        std::shared_ptr<const engine::TransitionFunction> fn;
        std::string current;  // id of the current config
    };

    LayerData* data(engine::Layer layer);
    const LayerData* data(engine::Layer layer) const;
    const engine::SceneEntry* resolve(const LayerData& d, const std::string& name) const;
    // The playlist's next item; for Dancers, skipping generated characters in a style switched off
    // (reforce.Dancer.RandomStyles). "" when nothing may come next.
    std::string nextId(LayerData& d, engine::SchedRng& rng);
    // An entry's scene type: the layer's, or Solid / Organic for an Object.
    scene::SceneType entryType(const LayerData& d, const engine::SceneEntry& e) const;
    // An object's `SBar` key: from the slideshow it starts only on a line every SBar bars of the phrase clock
    // (Lightning: 8); a manual start ignores it. 0 when unset.
    double startBars(const LayerData& d, const engine::SceneEntry& e);
    void applyTheme(const std::string& name);
    void handle(const engine::Command& c, double t);
    bool switchTo(LayerData& d, const engine::SceneEntry& e, bool quick, double t, const char* why,
                  double seconds = -1);
    bool switchToTarget(LayerData& d, engine::SwitchTarget target, const std::string& name, bool quick, double t,
                        double seconds = -1);
    void runSlideshow(double t);
    void preloadNextField(double t);
    void prefetchNext(LayerData& d, engine::SchedRng& rng);
    void prefetchNextSprite();
    void publish(double t);
    bool spawn(LayerData& d, const engine::SceneEntry& e, double t, const char* why, bool quick);
    void log(const std::string& msg);
    void handleVideo(const engine::Command& c, double t);
    FadeSpec spawnFade(LayerData& d, bool quick);
    std::vector<engine::SceneEntry> videoFiles_;

    std::unique_ptr<Visualizer> vis_;
    engine::Settings prefs_;
    std::vector<std::string> spriteHidden_;  // reforce.Sprite.Hidden ids (SetSpriteHidden replaces it)
    bool hiddenSprite(const LayerData& d, const engine::SceneEntry& e) const;
    DirectorOptions opts_;
    engine::SchedRng rng_;
    engine::ThemeLibrary themes_;
    std::string theme_ = "(Standard)";
    std::array<LayerData, 8> layers_;  // WaveShape, ColorMap, FlowField, Union, Particle, Sprite, Object, Dancer
    double objectInterval_ = 0;        // Object interval outside themes (setting, or the last interval command)
    std::set<std::string> objectSolids_;  // lower-case ids of the Object catalog's solids (the rest are organics)
    double dancerInterval_ = 0;        // the same for Dancers
    engine::SchedRng spawnRng_;        // Particle/Sprite decisions (keeps the other layers' sequence)
    engine::Playlist moviePlaylist_;   // slideshow movie sprites (kept out of layers_[5].playlist)
    engine::LayerSlideshow movieTimer_;  // their spawn timer (follows the Sprite layer's on/held)
    bool unionDue_ = false;
    const engine::SceneEntry* forcedUnion_ = nullptr;  // the Union the next FlowField transition uses
    std::vector<std::string> unionBack_;                // earlier picks of Union Next, for Union Prev
    std::string preloadId_;
    bool spritePrefetchDue_ = true;
    engine::CommandQueue commands_;
    // FlowField / Union switches wait one step: G-Force runs script commands
    // between a frame's flow pass and its WaveShape draw, so a new field first
    // moves the *next* frame (parity suite: a FlowField + WaveShape cut in the
    // same call flows the old WaveShape's last frame once more with the old field).
    std::vector<engine::Command> deferred_;
    // Ableton Link (Phase 5H): beat-aligned switching. Read from the step's
    // AudioFrame (App fills AudioFrame::link*).
    void updateLink(const audio::AudioFrame* audio);
    // Control panel Union "Clear": cancels a pending Union and, while one shapes
    // the field, morphs to the plain current FlowField (quick duration).
    void clearUnion(double t);
    // True when a change may start now under `mode` (reforce.Link.Quantize*, reforce.Quantize.Spawn): always with
    // "off"; "beat" / "bar": without a Link session always, else only on the step that crosses a beat or bar line;
    // "phrase": without a beat clock always, else only on the step that crosses a phrase line (PLAN 5O.1).
    bool linkAllows(const std::string& mode) const;
    void route(const engine::Command& c, double t);  // handle now, or defer FlowField/Union switches a step
    bool linkOn_ = false, linkPrevValid_ = false;
    double linkBeat_ = 0, linkPrevBeat_ = 0, linkBpm_ = 0, linkQuantum_ = 4;
    std::vector<engine::Command> linkHeld_;  // manual switches waiting for the next beat/bar (QuantizeManual)
    bool phraseOn_ = false;                  // phrase clock (PLAN 5O.1) as of the last step
    double phrasePrev_ = 0, phraseCur_ = 0;
    double barPrev_ = 0, barCur_ = 0;        // the phrase clock's running bar count, last step and this one
    std::map<std::string, double> startBars_;  // object id -> its SBar (slideshow start lines, bars; 0 = any time)
    engine::StateBox state_;
    engine::AudioState audioState_;  // latest analyzer scalars (published with the state)
    engine::EngineState scratch_;    // filled in place, swapped into state_ (no reader wait)
    bool publishDue_ = true;
    std::uint64_t lastPublish_ = 0;
    std::deque<std::string> recent_;
    double lastT_ = 0;
    std::uint64_t steps_ = 0;
};

}  // namespace reforce::app
