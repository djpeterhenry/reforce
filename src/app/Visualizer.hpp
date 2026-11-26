#pragma once
// The layered G-Force frame on the GPU pipeline: FlowField + WaveShape +
// ColorMap, each with its own independent transition (architecture §5a).
// Owned and driven by one thread (the render/sim thread; the main thread in
// the windowed app).
//
// step(t) runs one simulation tick (1/TargetFrameRate):
//   - FlowField: adopt a finished build (worker threads) and start its
//     transition; advance/finish the running transition
//   - WaveShape: evaluate the current config (and, while morphing, the
//     incoming one: both stay audio-reactive) -> morph -> line segments
//   - ColorMap: re-evaluate every CCal seconds; blend palettes while morphing
//   - encode flow (+ field blend) + lines into the intensity buffer
// colorize() maps the latest intensity through the palette (display rate).
//
// Interruptions (a switch while the layer is morphing) start from the current
// blend: FlowField bakes the blended field, ColorMap bakes the palette,
// WaveShape keeps the composite (both shapes at the frozen weight) as "from",
// at most two deep (a third change snaps the oldest morph to its target).

#include <array>
#include <map>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/ConfigCache.hpp"
#include "engine/SceneLibrary.hpp"
#include "engine/Blend.hpp"
#include "engine/Commands.hpp"
#include "engine/FlowFieldBuilder.hpp"
#include "engine/LineGeometry.hpp"
#include "engine/MovieBeatSync.hpp"
#include "engine/Palette.hpp"
#include "engine/Particles.hpp"
#include "engine/Prefs.hpp"
#include "engine/Sprites.hpp"
#include "engine/Transition.hpp"
#include "render/ScenePipeline.hpp"
#include "engine/Dancers.hpp"
#include "engine/Organics.hpp"
#include "engine/Solids.hpp"
#include "engine/Knight.hpp"
#include "engine/PieceColors.hpp"
#include "engine/StickerColors.hpp"
#include "render/DancerPipeline.hpp"
#include "render/FieldPipeline.hpp"
#include "render/SolidPipeline.hpp"
#include "render/JigsawPipeline.hpp"
#include "render/ModelPipeline.hpp"
#include "render/RainPipeline.hpp"
#include "render/HanoiPipeline.hpp"
#include "render/BubblePipeline.hpp"
#include "render/ShatterPipeline.hpp"
#include "render/FractalPipeline.hpp"
#include "app/ModelLoader.hpp"
#include "render/VideoMips.hpp"
#include "scene/Evaluate.hpp"
#include "sync/BeatClock.hpp"
#include "sync/DeckFeed.hpp"
#include "sync/NowPlaying.hpp"
#include "sync/Lyrics.hpp"
#include "render/TextOverlay.hpp"
#include "web/GuestBook.hpp"
#include "web/Guests.hpp"
#include "sync/DeckStarts.hpp"
#include "sync/DropDetector.hpp"
#include "sync/PhraseClock.hpp"
#include "video/BodyTracker.hpp"
#include "video/VideoSource.hpp"
#include "engine/Body.hpp"

namespace MTL { class Device; class Texture; class CommandBuffer; }
namespace reforce::audio { struct AudioFrame; }
namespace reforce::lights { class LightsOutput; }
namespace reforce::render { class LightsCapture; }

namespace reforce::app {

struct VisualizerOptions {
    uint32_t width = 1920, height = 1080;  // render size (supersample x the output size)
    int supersample = 1;                    // reforce.Supersample, for the Look panel's state
    std::uint64_t seed = 1;
    render::ScenePipelineOptions pipeline;
    engine::Settings prefs;
    int fieldThreads = 0;    // 0 = all cores
    int evalThreads = -1;    // WaveShape evaluation workers besides the render thread (-1 = auto, 0 = serial)
    bool syncBuilds = false; // deterministic: FlowField builds complete inside the switch call
                             // (and video sources deliver the exact frame for the sim time)
    // TEST HOOK (PLAN 5L.2, --solid-test): hard-coded 3D solids that live from the first step on, each holding a
    // slot of reforce.Object.MaxLive + 1. Solid scene files (startSolid) are the real way in (PLAN 5L.4).
    std::vector<render::SolidTestSpec> solidTests;
    // TEST HOOK (PLAN 5P, --dancer-test STYLE[,STYLE...]): built-in characters dancing from the first step on.
    std::vector<std::string> dancerTests;
    // Stream guests (web/Guests.hpp) may poll the guest service. Off for offline renders and --background instances.
    bool network = false;
    // Room lights (docs/lights.md): the paired Nanoleaf (nanoleaf.json next to settings.json). Empty: no lights (offline
    // renders, and --background instances unless --lights).
    std::filesystem::path lightsConfig;
};

struct FrameTimings {
    double waveEvalMs = 0;   // WaveShape evaluation (+ morph)
    double geometryMs = 0;   // segment building
    double colorMs = 0;      // ColorMap + LUT
    double encodeMs = 0;     // Metal encoding
    std::size_t segments = 0;
    double totalCpuMs() const { return waveEvalMs + geometryMs + colorMs + encodeMs; }
};

struct TransitionSpec {
    double duration = 0;  // seconds; <= 0 = cut
    std::shared_ptr<const engine::TransitionFunction> fn;
};

// Particle / sprite lifetime (probe #19): fade-in and fade-out of `fade`
// seconds with `fn`, `defaultHold` seconds at full pen unless the config sets PDur.
struct FadeSpec {
    double fade = 6.0;
    double defaultHold = 25.0;
    bool forever = false;  // Dancers with Duration 0 (∞): stay until cleared or made way for (wins over PDur)
    std::shared_ptr<const engine::TransitionFunction> fn;
};

struct LayerStatus {
    std::string current, target, unionName;
    bool pending = false, transitioning = false, crossfaderHeld = false;
    double progress = 0, weight = 0;
    int depth = 0;
};

class Visualizer {
public:
    Visualizer(MTL::Device* device, const VisualizerOptions& opts);
    ~Visualizer();

    // ---- configs
    // Compiles a scene file/text for `type` (logs errors). nullptr if unreadable.
    // Files go through the prepared-config cache (ConfigCache): compiled on a
    // worker ahead of time when prefetched, else here.
    std::shared_ptr<const scene::CompiledConfig> compileFile(const std::filesystem::path& path, scene::SceneType type);
    // Queues a background compile (and, for a .png ColorMap, the palette decode;
    // for a Sprite image, its params). `front`: before the bulk warm-up.
    void prefetchConfig(const std::filesystem::path& path, scene::SceneType type, bool front = false);
    std::shared_ptr<const scene::CompiledConfig> compileText(const std::string& text, const std::string& label,
                                                             scene::SceneType type) const;

    // ---- switches. Without a current config (startup) or with duration <= 0: cut.
    void switchWaveShape(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                         const TransitionSpec& spec);
    void switchColorMap(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                        const TransitionSpec& spec);
    void switchColorMapPalette(const engine::Palette& palette, const std::string& label, double t,
                               const TransitionSpec& spec);
    // .txt or .png ColorMap.
    bool switchColorMapFile(const std::filesystem::path& path, const std::string& label, double t,
                            const TransitionSpec& spec);
    // Starts the field build (or takes the preloaded one) and, with `unionCfg`,
    // the Union mask build; the transition starts when both are ready
    // (G-Force's ConfigPrepTime idea). A newer switch replaces a pending one.
    void switchFlowField(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                         const TransitionSpec& spec, std::shared_ptr<const scene::CompiledConfig> unionCfg = nullptr,
                         const std::string& unionLabel = {});
    // Builds a field ahead of its switch (slideshow preload). One slot.
    void preloadFlowField(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t);
    const std::string& preloadLabel() const { return preloadLabel_; }

    // ---- convenience loaders (cuts; used at startup and by tests)
    bool loadFlowField(const std::filesystem::path& path, double t = 0);
    bool loadWaveShape(const std::filesystem::path& path, double t = 0);
    bool loadColorMap(const std::filesystem::path& path, double t = 0);
    bool loadFlowFieldText(const std::string& text, const std::string& label, double t = 0);
    bool loadWaveShapeText(const std::string& text, const std::string& label, double t = 0);
    bool loadColorMapText(const std::string& text, const std::string& label, double t = 0);

    // Blocks until a pending FlowField switch is ready and starts it at `t`.
    void waitForField(double t = 0);
    bool fieldPending() const { return ffPending_.has_value(); }

    // ---- per-layer controls
    // Blend mode index (engine::blend::*); takes effect on the next transition.
    void setBlendMode(engine::Layer layer, int mode);
    int blendMode(engine::Layer layer) const;
    // Manual crossfader: holds the running transition at weight w; nullopt
    // releases it (the timer resumes from there). Returns false when the layer
    // has no transition to hold.
    bool setCrossfader(engine::Layer layer, std::optional<double> w, double t);

    // ---- particles and sprites (M6). Several may be alive at once.
    void startParticle(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                       const FadeSpec& fade);
    // Fades every live particle out over `fade` seconds (0 = remove now).
    void killParticles(double t, double fade = 0);
    // Image sprite; params from the sibling .txt or _default_sprite_params.txt.
    // The image is decoded on a worker thread (or taken from prefetchSprite);
    // the sprite starts at the first step after it is ready (at once with
    // syncBuilds). False when the params do not compile. A sprite that covers
    // the frame fades out the running ones that do ([VH 3.6]).
    // (Superseded by probe #44: a new EPen-only sprite fades out the running
    // EPen-only ones; with reforce.SpriteReplace = own only those of its kind.)
    bool startSprite(const std::filesystem::path& image, const std::string& label, double t, const FadeSpec& fade);
    // Starts decoding an image ahead of its startSprite (slideshow). One slot.
    void prefetchSprite(const std::filesystem::path& image);
    // Image sprites vs movie / live video sprites (control panel cards; reforce.SpriteReplace).
    enum class SpriteKind { All, Images, Movies };
    // Fades every live sprite (of `kind`) out over `fade` seconds (0 = remove
    // now) and drops the ones still being decoded.
    void killSprites(double t, double fade = 0, SpriteKind kind = SpriteKind::All);
    // Fades out the live sprites named in `names` (their labels, the file stems) over `fade`
    // seconds and drops the ones still being decoded. Returns how many.
    std::size_t fadeOutSprites(double t, double fade, const std::vector<std::string>& names);
    std::size_t spritesPending() const { return spritePending_.size(); }
    // Sprite Mix (PLAN 5K; reforce.Sprite.Mix / reforce.MovieSprite.Mix, 0..1): `movie` = movie and live
    // video sprites, else image sprites. Applies to every live sprite of that kind from the next step.
    void setSpriteMix(bool movie, double mix);
    double spriteMix(bool movie) const { return movie ? movieSpriteMix_ : spriteMix_; }
    // Luminance levels of a movie / live sprite (PLAN 5K): the movie's Black / White / Gamma (.reforce.txt, in its
    // spec) with the Gamma multiplied by reforce.MovieSprite.Gamma.
    render::VideoLevels videoLevels(const video::SourceSpec& spec) const;
    std::vector<std::string> particleNames() const { return particles_.names(); }

    // ---- 3D solids (PLAN 5L.4): reforce.Solid.package scene files. Up to reforce.Object.MaxLive (solids and organics together) at full strength:
    // a new one fades the oldest out under it (each holds one of MaxLive + 1 render targets; with none free the
    // oldest already fading goes at once). PBar sets the hold in bars. False when solids are unavailable or the Mesh
    // name is unknown.
    bool startSolid(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                    const FadeSpec& fade);
    // Fades every live solid out over `fade` seconds (0 = remove now).
    void killSolids(double t, double fade = 0);
    std::vector<std::string> solidNames() const;
    // ---- Organics (PLAN 5N): reforce.Organic.package scene files, drawn as line segments after the particles.
    // Up to reforce.Object.MaxLive (with the solids) at full strength (a new one fades the oldest out under it). PBar sets the hold in
    // bars. False for an unknown Kind.
    bool startOrganic(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                      const FadeSpec& fade);
    // Fades every live organic out over `fade` seconds (trees with Wthr ungrow; 0 = remove now).
    void killOrganics(double t, double fade = 0);
    std::vector<std::string> organicNames() const;
    // ---- Objects (solids and organics as one layer, 2026-11-22): reforce.Object.MaxLive counts both.
    void killObjects(double t, double fade = 0);
    std::vector<std::string> objectNames() const;
    // Room for a slideshow start on `layer` (Object: fewer than MaxLive at full strength; true for the others): the
    // slideshow waits until the live one starts fading out, so long pieces (PBar) are never cut.
    bool spawnRoom(engine::Layer layer, double t) const;
    // The same for movie / live video sprites (reforce.MovieSprite.MaxLive; prerolling ones count).
    bool movieRoom(double t) const;
    // A solid, an organic or a movie / live video sprite at full strength (or a movie prerolling): the slideshow
    // starts none of the three meanwhile (reforce.Slideshow.OneFeature).
    bool featureActive(double t) const;
    // ---- Dancers (PLAN 5P): reforce.Dancer.package characters, drawn in full color over the colorized frame.
    // Up to reforce.Dancer.MaxLive at once (a new one sends the oldest off). False when dancers are unavailable.
    bool startDancer(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                     const FadeSpec& fade);
    bool startDancerSpec(engine::DancerSpec spec, double t, double hold);
    // Sends every dancer off (a wave goodbye, then they shrink away over `fade` seconds; 0 = remove now).
    void killDancers(double t, double fade = 0);
    std::vector<std::string> dancerNames() const;
    std::vector<std::string> spriteNames(SpriteKind kind = SpriteKind::All) const;

    // ---- video (Phase 5F; architecture §5c)
    // Movie sprites go through startSprite/prefetchSprite (video files in the
    // Sprite package or an explicit path): same params, flags, fades. A live
    // (camera / Syphon) or explicit-source sprite: params from `paramsFor`
    // (spriteParamsFor: its sibling .txt or the package defaults) and a hold
    // of fade.defaultHold seconds (live: until stopped).
    bool startVideoSprite(const video::SourceSpec& spec, const std::string& label, double t, const FadeSpec& fade,
                          const std::filesystem::path& paramsFor);
    // Fades every movie/live sprite out over `fade` seconds.
    void stopVideoSprites(double t, double fade);
    // Control panel "Loop until stopped": every movie sprite running or about to
    // start loops and stays (no ageing out, not replaced by new background
    // sprites) until a stop or kill. Session only. Returns how many.
    int holdVideoSprites();
    bool videoSpritesHeld() const;
    // Color video layer: opens `spec` (preroll on its own thread) and
    // crossfades to it over params().crossfade once its first frame is in.
    void playVideoLayer(const video::SourceSpec& spec, double t);
    // Fades the color layer out over params().crossfade (0: at once).
    void stopVideoLayer(double t);
    // One `videoset` key (engine::applyVideoSetting); "" or an error.
    std::string setVideoParam(const std::string& key, const std::string& value);
    // Rhythm (PLAN 5O): phrase clock and kick zoom keys of engine::CommandType::BeatSet (the quantize keys are the
    // Director's). "" or an error.
    std::string setBeatParam(const std::string& key, const std::string& value);
    // Whether a dancer scene file may be picked as the next dancer: named characters always; a generated one
    // ("Random STYLE") only in a style ticked in reforce.Dancer.RandomStyles.
    bool dancerFileAllowed(const std::filesystem::path& path);
    void rhythmState(engine::RhythmState& out) const;
    const sync::PhraseState& phraseState() const { return phrase_.state(); }
    // Deck starts (PLAN 5O.8): the live rkbx_link / MIDI feed (owned by the App; null offline and in tests). The first
    // feed also starts watching the lyrics folder (sync/Lyrics).
    void setDeckFeed(sync::DeckFeed* feed);
    // True once per drop (PLAN 5O.5): the Director cuts the layers when reforce.Drop.Switch is on.
    bool takeDrop() {
        const bool d = dropPending_;
        dropPending_ = false;
        return d;
    }
    const engine::VideoLayerParams& videoParams() const { return videoParams_; }
    // A source is on the color layer (shown or fading).
    bool videoLayerActive() const { return vlA_.src || vlB_.src; }
    bool videoToSyphon() const { return videoParams_.toSyphon; }
    void videoState(engine::VideoState& out, double t) const;
    // Applies loop / rate / offset / feedback settings to a spec.
    video::SourceSpec withVideoDefaults(video::SourceSpec spec) const;

    // ---- Look (engine/Look.hpp): live rendering-quality A/B (thin lines,
    // intensity format, flow rounding, AA; FlowPrecision from the next field build).
    // Applies a CommandType::Look; returns "" or ": note" for the engine log.
    std::string handleLook(const engine::Command& c);
    void lookState(engine::LookState& out) const;

    LayerStatus status(engine::Layer layer, double t) const;
    bool busy(engine::Layer layer) const;  // pending or transitioning

    // Seeds the intensity buffer with value noise (static M3 demo).
    void seedNoise(MTL::CommandBuffer* cmd);

    // One simulation step at time t.
    void step(MTL::CommandBuffer* cmd, double t, const audio::AudioFrame* audio);
    // Colorizes the latest simulation frame into `output` (display rate), with
    // the color video layer composited unless `withVideo` is false. `trackAlpha`: the output's alpha holds how
    // much must stay opaque in the Syphon alpha (encodeSyphonAlpha), else it is 1.
    // `backdrop`: the camera backdrop is mixed in after the visuals and before the dancers and text (encodeBackdrop);
    // the output is then opaque.
    // `visualsCopy` (same size, BGRA8): also gets a copy of the visuals alone (movie layer and sprites included),
    // taken before the backdrop, dancers and text. `overlays` false: stop there (no backdrop, dancers or text).
    void colorize(MTL::CommandBuffer* cmd, MTL::Texture* output, double t, bool withVideo = true,
                  bool trackAlpha = false, bool backdrop = false, MTL::Texture* visualsCopy = nullptr,
                  bool overlays = true);
    // The window shows the visuals only (reforce.Window.VisualsOnly): no backdrop, dancers or text there, so screen
    // mirroring for room lights (Nanoleaf) follows the visuals; Syphon still gets everything.
    bool windowVisualsOnly() const { return windowVisualsOnly_; }
    // Room lights (reforce.Lights.Enabled, docs/lights.md): the Nanoleaf panels follow the visuals.
    bool lightsOn() const { return lights_.enabled; }
    // Puts the panels back and stops their thread (App::onShutdown, after the GPU is idle).
    void shutdownLights();
    // The Syphon output with transparency (reforce.Syphon.Alpha, docs/syphon-alpha.md): `frame` colorized with
    // trackAlpha, into `output` (same size, BGRA8) with the color unchanged and the alpha keyed.
    bool syphonAlpha() const { return syphonAlpha_; }
    void encodeSyphonAlpha(MTL::CommandBuffer* cmd, MTL::Texture* frame, MTL::Texture* output);
    // Camera backdrop (docs/backdrop.md): `spec` under everything (nullopt = off; fades in / out over 0.4 s).
    void setBackdrop(const std::optional<video::SourceSpec>& spec);
    // On (a source set, or one still fading out): colorize with `backdrop` (mixed in before the dancers), and the
    // Syphon output is the opaque result.
    bool backdropActive() const { return bdWanted_.has_value() || bdSrc_ != nullptr; }
    // In place on `frame` (visuals colorized with trackAlpha, no dancers yet): the backdrop where the Syphon alpha
    // would be transparent.
    void encodeBackdrop(MTL::CommandBuffer* cmd, MTL::Texture* frame);

    const FrameTimings& lastTimings() const { return timings_; }
    const std::string& flowFieldName() const { return ffName_; }
    const std::string& waveShapeName() const { return wsName_; }
    const std::string& colorMapName() const { return cmName_; }
    double lastFieldBuildSeconds() const { return lastFieldBuild_; }
    const engine::Palette& palette() const { return palette_; }

private:
    struct WaveNode;
    // A FlowField on the GPU: built and uploaded on a worker (Phase 5D: the
    // render thread never touches the CPU grid).
    struct GpuField {
        render::TextureHandle texture;
        double buildSeconds = 0;
        bool polar = false;
        bool float32 = false;  // built with reforce.FlowPrecision = float32
    };
    struct FieldBuild {
        std::future<GpuField> future;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<const GpuField> ready;
        std::string label;
    };
    struct PendingField {
        FieldBuild field;
        std::future<render::TextureHandle> mask;
        std::shared_ptr<std::atomic<bool>> maskCancel;
        bool hasMask = false;
        std::string unionLabel;
        TransitionSpec spec;
    };
    struct FieldTransition {
        std::shared_ptr<const GpuField> to;
        render::TextureHandle mask;
        engine::TransitionClock clock;
        int mode = 0;
        std::string toName, unionName;
    };

    scene::Environment environment(double t) const;
    engine::FlowFieldBuildOptions buildOptions() const;
    FieldBuild startFieldBuild(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t);
    // Cancels a build without waiting for it (Phase 5D): its future is parked
    // in retired*_ and dropped once ready (a std::async future's destructor
    // would block the render thread until the worker notices the cancel).
    void cancelBuild(FieldBuild& b);
    void cancelPending();
    void reapRetired();
    std::vector<std::future<GpuField>> retiredFields_;
    std::vector<std::future<render::TextureHandle>> retiredMasks_;
    bool pendingReady(bool block);
    void beginFieldTransition(double t);
    void switchColorMapSource(std::unique_ptr<engine::ColorMapSource> src, const std::string& label, double t,
                              const TransitionSpec& spec);
    void stepFlowField(double t);
    void stepWaveShape(double t, const audio::AudioFrame* audio);
    void stepColorMap(double t);
    void stepParticles(double t, const audio::AudioFrame* audio);
    void stepSprites(double t, const audio::AudioFrame* audio);

    VisualizerOptions opts_;
    std::unique_ptr<render::ScenePipeline> pipeline_;
    // 3D solids (PLAN 5L): the solid pass; its targets are sprite textures of pipeline_ (declared after it, so
    // destroyed first). Null when it could not be created.
    std::unique_ptr<render::SolidPipeline> solids_;
    // 3D models (PLAN 5R, docs/model-design.md): the model pass (shares solids_' slots; declared after it, so
    // destroyed first) and the worker that loads models (declared after the pass it uploads with).
    std::unique_ptr<render::ModelPipeline> models_;
    std::unique_ptr<ModelLoader> modelLoader_;
    // Rain on glass (Mesh="rain", docs/3d-design.md "Rain on glass"): stepped with the solids, laid over the
    // colorized frame after the glass (encodeComposite). One rain at a time (a newer one replaces an older).
    std::unique_ptr<render::RainPipeline> rain_;
    struct RainDraw {
        std::shared_ptr<const engine::rain::Sim> sim;
        render::RainLook look;
    };
    std::vector<RainDraw> rainDraws_;
    // The glass Tower of Hanoi (Mesh="hanoi", docs/3d-design.md "Tower of Hanoi"): laid over the colorized frame after
    // the glass puzzle, before the rain.
    std::unique_ptr<render::HanoiPipeline> hanoi_;
    std::vector<render::HanoiDraw> hanoiDraws_;
    // Soap bubbles (Mesh="bubbles", docs/3d-design.md "Bubbles"; not the dancers' speech bubbles_): stepped with the
    // solids, laid over the colorized frame after the rain (encodeComposite). The pass keeps no state between frames,
    // so several may run at once.
    std::unique_ptr<render::BubblePipeline> soap_;
    struct BubbleDraw {
        std::shared_ptr<const engine::bubbles::Sim> sim;
        render::BubbleLook look;
    };
    std::vector<BubbleDraw> soapDraws_;
    // Shatter (Mesh="shatter", docs/3d-design.md "Shatter"): the glass pane over the colorized frame, after the rain.
    std::unique_ptr<render::ShatterPipeline> shatter_;
    struct ShatterDraw {
        std::shared_ptr<const engine::shatter::Player> player;
        engine::shatter::Look look;
    };
    std::vector<ShatterDraw> shatterDraws_;
    std::vector<render::ModelDraw> modelColorDraws_;  // this step's look-B models (encodeColor after colorize)
    // The jigsaw (docs/3d-design.md "Jigsaw"): its pass over the colorized frame and this step's draws.
    std::unique_ptr<render::JigsawPipeline> jigsaws_;
    std::vector<render::JigsawDraw> jigsawDraws_;
    void preloadModels();
    // Fractal zooms (PLAN 5L.14, docs/3d-design.md "Fractal zoom"): the raymarching pass (shares solids_' slots;
    // declared after it, so destroyed first). Null when unavailable.
    std::unique_ptr<render::FractalPipeline> fractals_;
    std::filesystem::path contentDir_;  // reforce.ContentDir: camera presets (content/Camera)
public:
    void setContentDir(const std::filesystem::path& dir) { contentDir_ = dir; }
private:
    // TEST HOOK (--solid-test): live test solids, each on its own target slot.
    struct LiveSolidTest {
        render::SolidTestSpec spec;
        int slot = -1;
        double start = -1;
        bool logged = false;
    };
    std::vector<LiveSolidTest> solidTests_;
    // Solids from scene files (start order), each on its own target slot.
    struct LiveSolid {
        std::unique_ptr<engine::SolidRun> run;
        int slot = -1;
        // Model solids: the loader key, the model once resident, and what it takes to start the run again when
        // the model arrives (it waits, invisible, while loading: `waiting`).
        std::string modelKey;
        std::shared_ptr<const render::ModelGpu> model;
        bool waiting = false;
        std::shared_ptr<const scene::CompiledConfig> cfg;
        FadeSpec fade;
        std::unique_ptr<engine::jigsaw::Solver> jigsaw;  // Mesh="jigsaw": its plans (made at start)
    };
    std::vector<LiveSolid> liveSolids_;
    void stepModel(MTL::CommandBuffer* cmd, LiveSolid& s, double t, const engine::SpriteAudio& a);
    void stepFractal(MTL::CommandBuffer* cmd, LiveSolid& s, double t, const engine::SpriteAudio& a);
    std::vector<render::SpriteQuad> opaqueSolids_;  // this step's ColorKey = 0 solids (encodeColorize)
    std::vector<render::SpriteQuad> colorSolids_;  // this step's rubik solids, drawn in color (encodeColorize)
    std::vector<render::GlassDraw> glassDraws_;     // this step's glass puzzles (encodeGlass after colorize)
    engine::StickerPicker stickerPicker_;           // rubik sticker intensities for the current ColorMap
    int stickerLogged_ = -1;
    engine::TilePicker tilePicker_;                 // the 15 puzzle's row colors and numbers for the current ColorMap
    engine::knight::TourCache knightTours_;         // the knight solids' tours (searched once per Seed)
    // The ColorMap's colors for the organics (engine::OrganicColors), recomputed when the palette or MapFunction LUT
    // changes (the copies say what they were made from).
    engine::OrganicColors organicColors_{};
    engine::Palette organicPal_{};
    engine::MapLut organicLut_{};
    bool organicColorsHave_ = false;
    int tileLogged_ = -1;
    engine::TrioPicker trioPicker_;                 // the labyrinth's board, walls and ball colors
    int trioLogged_ = -1;
    engine::PiecePicker piecePicker_;               // the soma's seven piece colors for the current ColorMap
    int pieceLogged_ = -1;
    std::vector<std::unique_ptr<engine::OrganicRun>> organics_;  // start order
    void stepOrganics(double t, const audio::AudioFrame* audio);
    // Dancers (PLAN 5P).
    std::unique_ptr<render::DancerPipeline> dancers_;
    std::vector<std::unique_ptr<engine::DancerRun>> liveDancers_;  // start order
    render::DancerStage stage_;
    std::vector<std::string> dancerTestsPending_;
    std::uint32_t dancerSeed_ = 1;
    double danceBass_ = 0, lastDanceT_ = -1;
    bool danceFloor_ = true;
    std::string danceRandom_ = "all";  // reforce.Dancer.RandomStyles
    std::map<std::string, int> dancerFileStyle_;  // scene file -> its generated style, or -1 (a named character)
    engine::SocialMode socialMode_ = engine::SocialMode::Show;
    engine::Mingle mingle_;
    double danceLighting_ = 0.75, danceOutline_ = 2, danceHeight_ = 0.24, danceHuman_ = 1;
    double danceMocap_ = 1.0;  // reforce.Dancer.Mocap: share of routines that are mocap moves
    int danceTry_ = -1;        // beatset dancetry: the mocap move every dancer loops (a moveLibrary index), -1 none
    double danceReflection_ = 0.35;  // reforce.Dancer.Reflection
    int danceMaxLive_ = 5;
    // Name tags (stream guests'): a name over each guest's head, following them about. The anchor is the
    // dancer's floor spot at a steadied head height (a low-passed head, so the tag doesn't bob with every move); the
    // tag pops in and out with the dancer. Text blocks are laid out once per name (render/TextOverlay slots).
    static constexpr int kMaxNameTags = 32;   // = the most dancers on stage (reforce.Dancer.MaxLive)
    std::unique_ptr<render::TextOverlay> tags_;
    struct NameTag {
        const engine::DancerRun* run = nullptr;
        std::string text;
        bool placed = false;
        float x = 0, y = 0, z = 0;   // stage point the tag's bottom center sits on
        float headY = 0;             // low-passed head-top height
        float scale = 0;             // the dancer's pop
        bool seen = false;
        std::string bubble;          // a guest's message showing over them ("" = none)
        double bubbleAge = 0;        // seconds since it arrived
    };
    // Guest messages (web/Guests.hpp GuestMessage, Peter 2026-11-20): a speech bubble over the guest's dancer, above
    // the name tag, for kBubbleSeconds (a new message replaces it). Drawn by bubbles_ in full color (emoji).
    static constexpr double kBubbleSeconds = 30;
    struct GuestBubble {
        const engine::DancerRun* run = nullptr;
        std::string text;
        double start = 0;  // lastDanceT_ when it arrived
    };
    std::vector<GuestBubble> guestBubbles_;
    std::unique_ptr<render::TextOverlay> bubbles_;
    std::array<std::string, kMaxNameTags> bubbleSlotText_;
    std::vector<NameTag> nameTags_;
    double tagsLastT_ = -1;
    std::array<std::string, kMaxNameTags> tagSlotText_;  // what each text slot holds (render thread)
    void stepNameTags(double dt);
    // Stream guests (web/Guests.hpp, reforce.Guests.*): new check-ins become dancers with their name tag.
    std::unique_ptr<web::GuestFeed> guestFeed_;   // null when opts.network is off
    std::unique_ptr<web::GuestBook> guestBook_;   // past guests (Guests/ next to settings.json)
    bool callGuest(const std::string& idOrLabel, double t);  // beatset guestcall: a past guest on stage, until cleared
    bool guestsOn_ = false;
    std::string guestsUrl_;
    std::string guestsKey_;  // reforce.Guests.Key
    double guestsMinutes_ = 30;
    // Hide dancers (beatset dancehide on|off, not saved): the stage fades out over half a second but keeps going
    // (dancers keep their time, guests still arrive), so showing it again brings back whoever is still due.
    bool danceHidden_ = false;
    float danceShow_ = 1;   // 0 hidden .. 1 shown (eased)
    // A crowd pulls the camera back (Peter, 2026-11-20: up to 32 dancers): dancer size x sqrt(14 / n) above 14 on
    // stage (32: 0.66), eased over a couple of seconds so arrivals don't jump the view.
    float danceZoom_ = 1;
    void startGuests(double t);
    std::vector<std::string> guestsBlocked_;        // reforce.Guests.Blocked
    std::map<std::string, std::string> guestNames_; // guest id -> name, as seen this session (the panel)
    void setGuestsBlocked(const std::string& list);
    void encodeNameTags(MTL::CommandBuffer* cmd, MTL::Texture* output);
    engine::dance::MusicState danceMusic(double t, const audio::AudioFrame* audio);
    void stepDancers(double t, const audio::AudioFrame* audio);
    // GPU organics (PLAN 5O.6: julia, mandel): drawn into fields_ slots, composited like solids. Null when it could
    // not be created (those kinds then do not start).
    std::unique_ptr<render::FieldPipeline> fields_;
    std::vector<render::SpriteQuad> fieldFeedback_, fieldDisplay_;
    void stepFields(MTL::CommandBuffer* cmd);
    int freeSolidSlot() const;
    void stepSolids(MTL::CommandBuffer* cmd, double t, const audio::AudioFrame* audio);
    // One solid's composite through the sprite pass (Pen / EPen / Flag layering plan).
    void addSolidQuads(const render::SolidQuad& sq, const engine::SpriteDrawSpec& ds,
                       std::vector<render::SpriteQuad>& feedback, std::vector<render::SpriteQuad>& display,
                       bool over = false) const;
    std::vector<render::SpriteQuad> solidFeedback_, solidDisplay_;  // per-step scratch
    // Text=0 face source: wsOut_ mapped into the WaveShape tile (PLAN 5L.3); reused, no per-step allocation once
    // grown.
    std::vector<engine::LineSegment> tileSegments_;
    void appendTileSegments(const engine::MorphFrame& frame, std::vector<engine::LineSegment>& out) const;
    engine::LineStyle lineStyle_;
    engine::LookControl look_;
    void applyLook(unsigned changed, std::string& note);
    std::atomic<int> modes_[3] = {0, 0, 0};  // WaveShape, ColorMap, FlowField (read by the main thread's hotkeys)
    std::uint64_t loads_ = 0;   // per-load seed offset

    // FlowField
    std::shared_ptr<const GpuField> ffCur_;  // current, or "from" while morphing
    std::optional<FieldTransition> ffTrans_;
    std::optional<PendingField> ffPending_;
    std::optional<double> ffPendingHold_;
    FieldBuild preload_;
    std::string preloadLabel_;
    std::string ffName_;
    std::string ffHeldUnion_;  // Union whose blend is held after its transition ended (probe #45)
    double lastFieldBuild_ = 0;

    // WaveShape
    std::unique_ptr<WaveNode> wsCur_, wsTo_;
    engine::TransitionClock wsClock_;
    int wsTransMode_ = 0;
    engine::AlignCache wsCache_;
    engine::MorphFrame wsFrom_, wsToFrame_, wsOut_;
    bool wsOutValid_ = false;                         // wsOut_ is this step's output
    std::pair<double, double> wsExtents_{1.0, 1.0};   // its config extents (X_EXTENTS, Y_EXTENTS), morph-blended
    std::string wsName_;
    std::vector<engine::LineSegment> segments_;

    // ColorMap
    std::unique_ptr<engine::ColorMapSource> cmCur_, cmTo_;
    engine::Palette palCur_{}, palTo_{};
    engine::TransitionClock cmClock_;
    int cmTransMode_ = 0;
    std::string cmName_, cmToName_;
    engine::Palette palette_{};
    engine::MapFunction mapFn_;
    engine::MapLut lut_{};
    bool lutValid_ = false;

    // Particles and sprites
    engine::ParticleSystem particles_;
    struct LiveSprite {
        std::unique_ptr<engine::SpriteRun> run;
        int texture = -1;
        std::unique_ptr<video::VideoSource> video;  // movie / live sprite
        bool videoFailed = false;                   // source failed after it started: fading out
        bool held = false;                          // "loop until stopped": loops, never ages out
        std::unique_ptr<engine::MovieBeatSync> beatSync;  // movie with a BPM: playhead follows the beat clock
    };
    std::vector<LiveSprite> sprites_;
    // Beat-synced movies (PLAN 5J.4): one clock per sim step, Link or the audio PLL.
    sync::BeatClock beatClock_;
    double beatClockT_ = -1, beatClockDt_ = 0;
    sync::PhraseClock phrase_;  // PLAN 5O.1, configured from settings in the constructor
    double kickZoom_ = 0, kickRotateDeg_ = 0, kickDecayMs_ = 120;  // PLAN 5O.3 (settings, panel)
    double kick_ = 0;           // kick pulse this step, 0..1
    sync::DropDetector drop_;   // PLAN 5O.5
    sync::DeckFeed* deckFeed_ = nullptr;  // PLAN 5O.8
    sync::DeckStarts deckStarts_;
    bool deckStartsOn_ = true;            // reforce.Phrase.DeckStarts
    // Now playing (sync/NowPlaying, render/TextOverlay): picked each step, drawn over the finished frame.
    sync::NowPlaying nowPlaying_;
    std::unique_ptr<render::TextOverlay> text_;
    bool nowPlayingOn_ = false;           // reforce.NowPlaying.Show
    double nowPlayingSize_ = 100;         // reforce.NowPlaying.Size (%)
    std::string nowPlayingCorner_ = "bl"; // reforce.NowPlaying.Corner
    render::TextOverlay::Shadow npShadow_;  // reforce.NowPlaying.Shadow / ShadowFrom
    struct TextFade {
        bool on = false;                  // the block should be visible
        double alpha = 0;
        render::TextBlockSpec spec;       // what it shows (kept while it fades out)
    };
    TextFade npMain_, npVocal_;
    double npLastT_ = -1;
    void encodeNowPlaying(MTL::CommandBuffer* cmd, MTL::Texture* output);
    // Lyrics (sync/Lyrics): karaoke for the deck heard most that has lyrics, over the finished frame. Line i of the
    // song is laid out in slot i % kLyricSlots (at most three lines show at once).
    static constexpr int kLyricSlots = 4;
    std::unique_ptr<sync::LyricsLibrary> lyricsLib_;
    std::array<sync::DeckClock, sync::DeckStarts::kDecks> deckClocks_;
    sync::KaraokePicker karaokePick_;
    std::unique_ptr<render::TextOverlay> lyricsText_;
    bool lyricsOn_ = false;                   // reforce.Lyrics.Show
    double lyricsSize_ = 100;                 // reforce.Lyrics.Size (%)
    std::string lyricsPosition_ = "bottom";   // reforce.Lyrics.Position
    double lyricsOffsetMs_ = 0;               // reforce.Lyrics.OffsetMs
    bool lyricsReviewedOnly_ = false;         // reforce.Lyrics.ReviewedOnly
    struct DeckLyrics {
        sync::DeckTrack track;                // what the song was looked up for
        std::uint64_t version = ~std::uint64_t(0);
        std::shared_ptr<const sync::LyricsSong> song;
    };
    std::array<DeckLyrics, sync::DeckStarts::kDecks> deckLyrics_;
    int lyricsDeck_ = -1;                     // picked this step
    std::shared_ptr<const sync::LyricsSong> lyricsSong_;  // drawn (kept while it fades out)
    int lyricsSongDeck_ = -1;
    double lyricsAlpha_ = 0;
    double lyricsLastT_ = -1;
    int lyricsLineNow_ = -1;                  // the line being sung (or next), for the panel
    void stepLyrics(double now);
    void encodeLyrics(MTL::CommandBuffer* cmd, MTL::Texture* output);
    double lyricsSongTime(int deck, const sync::LyricsSong& song, double now) const;
    std::vector<std::pair<double, sync::DeckCommit>> deckPending_;  // (apply at, commit): waits for Link to settle
    void stepDeckStarts(const sync::BeatClockState& s);
    bool dropPending_ = false;  // a drop the Director has not taken yet
    void stepKick(const audio::AudioFrame* audio);
    double lastAudioTempo_ = 0, loggedAudioTempo_ = 0;  // TEMPO seen / last logged (changes > 3 % are logged)
    bool lastKick_ = true;
    std::optional<bool> beatSyncOverride_;  // panel toggle (videoset beatsync); else reforce.Video.BeatSync
    bool movieBeatSyncOn() const { return beatSyncOverride_.value_or(opts_.prefs.movieBeatSync); }
    std::optional<double> beatOffsetOverride_;  // panel nudge (videoset beatoffset); else reforce.BeatClock.OffsetMs

public:
    // Beat clock latency nudge, ms (+ = beats earlier). The app samples Link this much further ahead.
    double beatOffsetMs() const { return beatOffsetOverride_.value_or(opts_.prefs.beatClockOffsetMs); }
    // For the periodic log line: "audio 124.0 BPM locked (lock 0.98), kick, TEMPO 123.9, offset +45 ms".
    std::string beatSummary() const;

private:
    void stepBeatClock(double t, const audio::AudioFrame* audio);
    engine::BeatSample beatSample() const;
    // The beat clock as SYNC_* scene variables (PLAN 5L.4); all 0 without a source.
    scene::SyncInput syncInput() const;
    // Analyzer scalars + LINK_* + SYNC_* for sprite and solid configs.
    engine::SpriteAudio spriteAudio(const audio::AudioFrame* audio) const;
    // A beat sync for a movie whose options have a BPM (else null); `who` for the log.
    std::unique_ptr<engine::MovieBeatSync> makeBeatSync(const video::VideoSource& src, const std::string& who) const;
    struct DecodedSprite {
        bool ok = false;
        MTL::Texture* texture = nullptr;  // created and filled on the decode worker (owned)
        bool hasAlpha = false;            // of the luminance/alpha texture
        int width = 0, height = 0;
        bool alpha = false;
        double decodeMs = 0;
        ~DecodedSprite();
    };
    using SpriteImage = std::shared_future<std::shared_ptr<const DecodedSprite>>;
    struct SpriteParams {
        std::shared_ptr<const scene::CompiledConfig> cfg;
        std::string origin;
        bool nameInvert = false, invert = false;
    };
    struct PendingSprite {
        SpriteImage image;
        std::unique_ptr<video::VideoSource> video;  // movie / live sprite (instead of `image`)
        SpriteParams params;
        std::filesystem::path path;
        std::string label;
        FadeSpec fade;
        double requested = 0;
        std::uint64_t seed = 0;
        bool held = false;
    };
    std::optional<SpriteParams> spriteParams(const std::filesystem::path& image);  // cached
    std::optional<SpriteParams> makeSpriteParams(const std::filesystem::path& image) const;
    SpriteImage decodeSprite(const std::filesystem::path& image, bool invert);
    void adoptSprites(double t);
    // Solids and organics at full strength (not fading out); makes room for a new object (fades the oldest out).
    int activeObjects(double t) const;
    void makeRoomForObject(double t, double fade, const std::string& label);
    std::vector<PendingSprite> spritePending_;  // request order
    std::string prefetchKey_;
    SpriteImage prefetch_;
    std::unique_ptr<video::VideoSource> prefetchVideo_;  // next slideshow movie sprite, prerolling
    std::vector<render::SpriteQuad> feedbackQuads_, displayQuads_;
    engine::SpriteLayering spriteLayering_ = engine::SpriteLayering::GForce;  // reforce.SpriteLayering (Look: sprites)
    double spriteTrails_ = 0.35;                                               // reforce.SpriteTrails
    bool spriteIgnoreAlpha_ = false;                                           // reforce.SpriteAlpha = ignore
    double spriteMix_ = 0, movieSpriteMix_ = 0;                                // reforce.Sprite.Mix / MovieSprite.Mix
    // reforce.MovieSprite.Color = over (PLAN 5K): movie / live sprites skip the intensity buffer; their frames are
    // drawn in color over the colorized frame (colorQuads_, alpha = envelope x (0.5 + 0.5 movie Mix)).
    bool movieColorOver_ = false;
    // Syphon alpha (reforce.Syphon.Alpha; videoset syphonalpha / keysimilarity / keysmoothness, OBS's units).
    bool syphonAlpha_ = true, syphonPremultiplied_ = false;
    bool windowVisualsOnly_ = false;
    // Room lights: settings, the connection (its own thread) and the GPU readback (made at the first capture).
    engine::LightsSettings lights_;
    std::shared_ptr<lights::LightsOutput> lightsOut_;
    std::unique_ptr<render::LightsCapture> lightsCap_;
    double lightsLastCapture_ = -1;  // wall seconds
    void applyLightsSettings();
    void captureLights(MTL::CommandBuffer* cmd, MTL::Texture* frame);
    double keySimilarity_ = 80, keySmoothness_ = 50;
    std::vector<render::SpriteQuad> colorQuads_;  // this step's color sprites, drawn by every colorize until the next
    std::vector<render::SpriteQuad> opaqueMovies_;  // this step's ColorKey = 0 movie sprites in the visuals (wholeRect)
    std::vector<render::SpriteQuad> opaqueScratch_; // opaqueSolids_ + opaqueMovies_ for encodeColorize
    std::vector<video::FramePtr> colorFrames_;     // their frames, kept alive for those colorize passes
    std::vector<video::FramePtr> colorHold_;       // scratch for holdFrames
    // Camera backdrop (reforce.Backdrop.*; videoset backdrop / backdropmirror / backdropfit / backdropopacity).
    std::optional<video::SourceSpec> bdWanted_;          // what should show (nullopt = off)
    std::unique_ptr<video::VideoSource> bdSrc_, bdPending_;  // shown (or fading out) / opening
    double bdStart_ = 0, bdLevel_ = 0, bdLastT_ = -1;    // source start, fade 0..1, last step time
    bool bdFailReported_ = false;
    bool bdMirror_ = false;
    engine::VideoFit bdFit_ = engine::VideoFit::Cover;
    double bdOpacity_ = 1;
    std::vector<video::FramePtr> bdHold_;
    // The frame the backdrop shows: the newest, or with Body.Sync the one the current silhouette is from.
    video::FramePtr backdropFrame();
    void stepBackdrop(double t);
    // The backdrop's fit to the frame (false while it has no size yet).
    bool backdropUv(engine::UvTransform& uv) const;

    // Body tracking on the backdrop source (reforce.Body.*, docs/body.md; videoset body<key>).
    engine::BodySettings body_;
    // While tracking is on and a backdrop source is set. Shared: a live source's frame listener holds it weakly.
    std::shared_ptr<video::BodyTracker> tracker_;
    bool trackerListens_ = false;     // the source hands it frames itself (live); else stepBody offers them
    video::FramePtr bdSyncFrame_;     // the frame the current mask is from (reforce.Body.Sync)
    double bdCamFps_ = 0;             // backdrop frames per second (measured)
    std::uint64_t bdCamCount_ = 0;
    double bdCamT_ = -1;
    const void* trackedSrc_ = nullptr;
    std::uint64_t bodySeq_ = 0;
    double bodyLastT_ = -1;
    engine::PoseSample poseSample_;
    engine::PoseSmoother pose_;
    engine::HandTrails trails_;
    std::array<float, 4 * render::BackdropBody::kJoints> bodyJoints_{};  // screen uv x, y, weight, 0
    // The person mask: a ring of R8 textures (uploads at the tracker's rate while earlier frames may still read).
    std::array<MTL::Texture*, 4> maskTex_{};
    int maskW_ = 0, maskH_ = 0, maskCur_ = -1, maskPrev_ = -1;
    void stepBody(double t);
    void resetBody();
    void uploadBodyMask(const std::vector<std::uint8_t>& mask, int w, int h);
    void releaseBodyMasks();

    // Video (Phase 5F)
    std::unique_ptr<video::VideoSource> openVideo(const video::SourceSpec& spec);
    void stepVideoLayer(double t);
    // Keeps video frames alive until the GPU has finished `cmd`.
    void holdFrames(MTL::CommandBuffer* cmd, std::vector<video::FramePtr>& frames);
    void videoError(const std::string& msg);
    struct LayerSlot {
        std::unique_ptr<video::VideoSource> src;
        double start = 0;
        video::FramePtr frame;
        bool failReported = false;
        bool endHandled = false;  // a play-once movie started its fade-out
        std::unique_ptr<engine::MovieBeatSync> beatSync;  // movie with a BPM: playhead follows the beat clock
        double lastT = -1;
    };
    engine::VideoLayerParams videoParams_;
    std::vector<engine::ExcludeRule> videoLoopRules_;  // reforce.Video.Loop
    // True when `path` loops: `fallback` (the layer/sprite Loop setting) or listed in reforce.Video.Loop.
    bool movieLoops(const std::string& path, bool fallback) const;
    // Files: loop from movieLoops, then the movie's own "<stem>.reforce.txt" (Loop, Rate, Start) wins.
    void applyMovieOptions(video::SourceSpec& spec, bool loopDefault) const;
    LayerSlot vlA_, vlB_;           // shown (or outgoing) / incoming; a fade with no B fades out
    std::unique_ptr<video::VideoSource> vlPending_;  // waiting for its first frame
    bool vlFading_ = false;
    double vlFadeStart_ = 0, vlFadeDur_ = 0, vlWeight_ = 1;
    std::vector<video::FramePtr> stepFrames_;
    std::string videoError_;
    // Minified video frames get a prefiltered, mipmapped copy (render/VideoMips).
    std::unique_ptr<render::VideoMips> videoMips_;
    struct VideoSpriteDraw {
        int texture = -1;
        const void* slot = nullptr;
        video::FramePtr frame;
        double scale = 1;  // drawn size / frame size (more minified axis)
    };
    std::vector<VideoSpriteDraw> videoSpriteDraws_;  // this step's video sprite frames
    MTL::Texture* minified(MTL::CommandBuffer* cmd, const void* slot, const video::FramePtr& f, double scale);

    FrameTimings timings_;

    std::shared_ptr<const engine::Palette> pngPalette(const std::filesystem::path& path);
    // Last members: destroyed first (join their workers, which use the above).
    std::unique_ptr<ConfigCache> cache_;
    // WaveShape step / morph evaluation on the render thread's behalf (Phase 5D).
    std::unique_ptr<scene::WorkerPool> evalPool_;
};

// Built-in scenes (our own text, not G-Force content): used when no scene
// directory is available.
extern const char* const kBuiltinFlowField;
extern const char* const kBuiltinWaveShape;
extern const char* const kBuiltinColorMap;

}  // namespace reforce::app
