#pragma once
// Engine command API: the one path by which hotkeys now (and the web UI in
// M5b, architecture §5b) change what plays. Producers on any thread push
// Commands; the render/sim thread drains the queue at the start of each
// simulation step and owns all engine state. Readers get an EngineState
// snapshot copied under a mutex.

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "engine/Look.hpp"
#include "engine/Transition.hpp"

namespace reforce::engine {

enum class CommandType {
    Switch,                 // layer, target (name/next/prev), quick
    Hold,                   // layer or all layers: flag = hold (true) / unhold
    ToggleHold,             // layer or all layers
    SetSlideshowEnabled,    // layer or all layers: flag
    SetSlideshowInterval,   // layer: value seconds (Particle/Sprite/Solid: 0 = the slideshow starts none)
    SetSpawnDuration,       // Particle / Sprite / Solid: value = seconds at full strength (<Type>.Duration)
    SetMovieInterval,       // Sprite: value = seconds between slideshow movie sprites (0 = none)
    SetSpriteMix,           // Sprite: text = "sprite" (image sprites) | "movie" (movie and live sprites),
                            // value = Mix 0..1 (flag: value is a nudge added to the current Mix)
    SetTransitionDuration,  // layer: value seconds (slideshow transitions)
    SetBlendMode,           // layer: text = mode name
    SetTheme,               // text = theme name ("(Standard)" = all)
    SetCrossfader,          // layer: value = weight 0..1 (holds the transition at that weight)
    ReleaseCrossfader,      // layer
    Kill,                   // Particle / Sprite / Solid layer: fade every live one out over Transition.Duration.Quick;
                            // Union: clear (Director::clearUnion)
    // Video (Phase 5F). `videoTarget` = the color layer or a sprite.
    VideoStart,             // text = source ("file:PATH", "camera[:ID]", "syphon:NAME", or a video library name)
    VideoStop,              // color layer: fade out over its crossfade; sprites: fade every video sprite out
    VideoSet,               // text = key (engine::applyVideoSetting), arg = value
    VideoHold,              // movie sprites: loop until stopped (session only)
    // Look (engine/Look.hpp): text = look key ("thinlines", ...) with arg = value, or an
    // operation: "ab" (A/B toggle), "preset" (arg gforce|right), "reset" (arg = key or "" for all),
    // "save" (arg = key or ""; handled by the web server, which writes settings.json),
    // "saved" (arg = KEY=VALUE: the value now in settings.json).
    Look,
    // Rhythm (PLAN 5O): text = key, arg = value. Keys (engine::validateBeatSetting):
    //   phrasenow (a phrase starts now), phrasenudge N (bars, +-), phrasebars N, phraseauto on|off,
    //   quantize / quantizemanual / quantizespawn off|beat|bar|phrase, kickzoom X, kickrotate DEG, kickdecay MS,
    //   dropswitch on|off.
    BeatSet,
    // Sprite: text = the hidden sprites (reforce.Sprite.Hidden, engine::parseIdList), the whole list.
    // Queued by the web server after the control panel's Review images saved it; not a text command.
    SetSpriteHidden,
};

enum class VideoTarget { Layer, Sprite };

enum class SwitchTarget { Name, Next, Previous };

struct Command {
    CommandType type = CommandType::Switch;
    Layer layer = Layer::WaveShape;
    bool allLayers = false;
    SwitchTarget target = SwitchTarget::Next;
    std::string text;   // config name / blend mode / theme
    double value = 0;
    bool flag = false;
    bool quick = true;  // Switch: Transition.Duration.Quick (manual) vs the slideshow duration
    double seconds = -1;  // Switch: explicit morph duration (>= 0; 0 = cut) overriding `quick`; < 0 = unset
    VideoTarget videoTarget = VideoTarget::Layer;  // VideoStart / VideoStop
    std::string arg;      // VideoSet: value

    static Command switchTo(Layer l, std::string name, bool quick = true);
    // Switch with an explicit morph duration (0 = cut), like G-Force's scripted
    // StartConfig(type, name, morphSecs) / SetFlowField(name, union, morphSecs).
    static Command morphTo(Layer l, std::string name, double seconds);
    static Command next(Layer l, bool quick = true);
    static Command previous(Layer l, bool quick = true);
    static Command hold(Layer l, bool on);
    static Command holdAll(bool on);
    static Command toggleHold(Layer l);
    static Command toggleHoldAll();
    static Command slideshow(Layer l, bool on);
    static Command slideshowAll(bool on);
    static Command interval(Layer l, double seconds);
    static Command transitionDuration(Layer l, double seconds);
    static Command spawnDuration(Layer l, double seconds);  // Particle / Sprite / Solid
    static Command movieInterval(double seconds);           // movie sprites
    static Command spriteMix(bool movie, double value, bool relative = false);  // PLAN 5K
    static Command blendMode(Layer l, std::string mode);
    static Command theme(std::string name);
    static Command crossfader(Layer l, double weight);
    static Command releaseCrossfader(Layer l);
    static Command kill(Layer l);  // Particle / Sprite / Solid
    static Command videoStart(VideoTarget target, std::string source);
    static Command videoStop(VideoTarget target);
    static Command videoSet(std::string key, std::string value);
    static Command beatSet(std::string key, std::string value);
    static Command videoHold();
    static Command look(std::string keyOrOp, std::string value = {});
    static Command spriteHidden(std::string idList);
};

const char* videoTargetName(VideoTarget t);  // "layer" / "sprite"

std::string describe(const Command& c);
// settings.json entries that make a slideshow setting change permanent (interval,
// transition, Particle/Sprite duration, movie interval); empty for other
// commands and for values the engine would ignore. The web server saves these.
PrefsTable slideshowSaveEntries(const Command& c);
// Rhythm settings (CommandType::BeatSet): "" when `key` = `value` is valid, else why not.
std::string validateBeatSetting(const std::string& key, const std::string& value);
// "" when a Look command's key/value/operation is valid, else the error.
std::string validateLook(const Command& c);

// Text form (CLI scripting / debugging; the web UI will post JSON):
//   next:LAYER  prev:LAYER  switch:LAYER:NAME  slide:LAYER:NAME (slideshow duration)
//   morph:LAYER:SECS:NAME (explicit morph duration, 0 = cut; switch:union:NAME first to give the
//   next FlowField morph a Union)
//   hold:LAYER|all  unhold:LAYER|all  togglehold:LAYER|all  slideshow:LAYER|all:on|off
//   interval:LAYER:SEC  duration:LAYER:SEC  blend:LAYER:MODE  theme:NAME
//   kill:union (clear: no pending Union; a Union blend morphs to the plain field)
//   kill:sprite:images|movies (one kind only)  nextmovie:sprite (next from the movie shuffle)
//   life:particle|sprite|solid:SEC (seconds at full strength)  movieinterval:sprite:SEC (0 = no movie sprites)
//   spritemix:sprite|movie:W (sprite Mix 0..1 of image / movie+live sprites; +W / -W nudges the current one)
//   fader:LAYER:W  release:LAYER
//   Particles/Sprites: switch:particle:NAME starts one (manual: fade-in over
//   Transition.Duration.Quick), start:particle:NAME is a scripted start (like
//   G-Force's StartParticle: the regular Transition.Duration), next:particle
//   starts the next of the playlist (quick), kill:particle|sprite fades all out
//   over Transition.Duration.Quick. Solids (PLAN 5L.4) work like particles:
//   switch:object:NAME, next:object, kill:object (Objects: solids and organics).
//   Video (Phase 5F): video:layer:SOURCE plays SOURCE on the color video layer
//   (crossfade), video:sprite:SOURCE starts it as a movie/live sprite;
//   SOURCE = file:PATH | camera[:ID] | syphon:NAME | a name from the video
//   library. videostop:layer|sprite. videohold (movie sprites loop until stopped). videoset:KEY:VALUE (opacity, blend, fit,
//   order, key, keycolor, tolerance, softness, threshold, crossfade, loop,
//   rate, offset, syphon; engine/VideoComposite.hpp).
//   Look (engine/Look.hpp): look:KEY:VALUE (thinlines gforce|smooth, intensity gforce|hq,
//   flowrounding fixed16|nearest|floor|point|fixed256, flowprecision double|float32, aa on|off;
//   settings keys work too), look:ab, look:preset:gforce|right, look:reset[:KEY], look:save[:KEY].
std::optional<Command> parseCommand(const std::string& text, std::string* error = nullptr);

class CommandQueue {
public:
    void push(Command c) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(c));
    }
    std::vector<Command> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Command> out;
        out.swap(queue_);
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<Command> queue_;
};

struct LayerState {
    Layer layer = Layer::WaveShape;
    std::string current;      // config shown (the "from" side during a transition)
    std::string target;       // incoming config ("" when none)
    bool pending = false;     // incoming config still being prepared (FlowField build)
    bool transitioning = false;
    double progress = 0;      // 0..1 of the running transition
    double weight = 0;        // Transition.Function(progress), or the crossfader value
    bool crossfaderHeld = false;
    int morphDepth = 0;       // WaveShape: 1 = morph, 2 = morph from an interrupted morph
    bool slideshowEnabled = true;
    bool held = false;
    double interval = 0;            // Slideshow.Interval.Duration (s)
    double transitionDuration = 0;  // Transition.Duration (s)
    double nextSwitchIn = -1;       // seconds until the slideshow switches (-1: not scheduled)
    std::string blendMode;
    std::vector<std::string> blendModes;
    std::size_t playlistSize = 0;
    std::string unionName;    // FlowField: Union shaping the current transition
    bool unionDue = false;    // FlowField: next transition takes the next Union
    std::vector<std::string> alive;  // Particle / Sprite: live instances (spawn order)
    double spawnDuration = 0;        // Particle / Sprite: <Type>.Duration (s at full strength)
    double movieInterval = -1;       // Sprite: movie sprite interval (0 = none; -1 = n/a)
    double spriteMix = 0;            // Sprite: image sprites' Mix (reforce.Sprite.Mix)
    double movieSpriteMix = 0;       // Sprite: movie / live sprites' Mix (reforce.MovieSprite.Mix)
    double nextMovieIn = -1;         // Sprite: seconds until the next slideshow movie sprite (-1: none due)
    std::size_t moviePlaylistSize = 0;  // Sprite: movies in the movie shuffle (not in playlistSize)
};

// Frame-time stats for the web UI (Phase 5D; filled by the app, empty offline).
struct PerfState {
    bool valid = false;
    std::uint64_t frames = 0, hitches = 0, renderAllocs = 0;
    double workP50 = 0, workP99 = 0, workMax = 0;  // max(cpu, gpu) per frame, ms
    double gpuP99 = 0, budgetMs = 0, displayHz = 0;
    std::vector<double> histEdges;    // ms
    std::vector<std::uint32_t> hist;  // histEdges.size() + 1 counts
    std::string lastHitch;
};

// Audio meters for the web UI (Phase 5C.5): the analyzer's scalars at the last
// published step; valid = audio is being analyzed (false with --no-audio).
struct AudioState {
    bool valid = false;
    double level = 0, bass = 0, energy = 0;  // G-Force scene variables
    double mid = 0, treble = 0, beat = 0;    // reforce extensions (scene variables)
    double tempo = 0;                        // TEMPO, BPM (0 = unsure)
    // Ableton Link (Phase 5H).
    bool linkEnabled = false;                // Link running (reforce.Link.Enabled)
    bool linkListening = false, linkJoining = false;  // follow mode (sync::LinkSnapshot)
    int linkPeers = 0;
    bool linkOn = false;                     // >= 1 peer: LINK_* valid, BEAT/TEMPO may follow Link
    double linkBpm = 0, linkBeat = 0, linkPhase = 0, linkQuantum = 4;
};

// Video (Phase 5F) for the web UI: the color layer and the video sprites.
struct VideoState {
    bool layerActive = false;         // a source is shown (or fading in/out)
    std::string layerSource;          // label of the current (or outgoing) source
    std::string layerIncoming;        // label of the source fading in ("" = none / fading out)
    std::string layerPending;         // opened, waiting for its first frame (or a Syphon server)
    std::string layerWaiting;         // what a source that shows nothing waits for (camera permission, Syphon server, ...)
    std::string layerSpec;            // source spec of the shown source ("file:...", "camera:...", ...)
    double crossfade = -1;            // 0..1 while a crossfade runs, else -1
    int width = 0, height = 0;        // shown source
    double fps = 0, duration = 0;     // files
    double decodeMsAvg = 0, decodeMsMax = 0;
    std::uint64_t decoded = 0, dropped = 0;
    std::string blend = "normal", fit = "cover", order = "over", key = "none", keyColor = "#00ff00";
    double opacity = 1, threshold = 0.1, tolerance = 0.15, softness = 0.1, crossfadeSeconds = 1, rate = 1,
           offset = 0;
    bool loop = false, toSyphon = true;
    std::vector<std::string> sprites;  // video sprites alive (movie + live)
    bool spritesHeld = false;          // "loop until stopped" active
    bool beatSync = true;              // reforce.Video.BeatSync (videoset beatsync)
    std::string spriteColor = "off";  // reforce.MovieSprite.Color: off | over (videoset spritecolor)
    bool syphonAlpha = true;           // reforce.Syphon.Alpha (videoset syphonalpha)
    bool windowVisuals = false;        // reforce.Window.VisualsOnly (videoset windowvisuals)
    // Room lights (reforce.Lights.*, docs/lights.md; videoset lights / lightsbrightness / ...).
    bool lightsAvailable = false;      // this instance may drive them (not offline / a test instance without --lights)
    bool lightsOn = false;
    double lightsBrightness = 1, lightsSaturation = 1.3, lightsFloor = 0, lightsSmoothing = 0.3;
    std::string lightsState = "off";   // live: off | not paired | connecting | streaming | error
    std::string lightsMessage, lightsDevice;  // live
    int lightsPanels = 0;              // live
    double lightsHz = 0;               // live: packets a second
    double lightsPulse = 0, lightsPulseTiming = 40;
    bool lightsPulsing = false;        // live: the pulse runs (on, beat clock confident)
    double lightsSide = 150;           // the panels' side length (layout units)
    std::vector<double> lightsLayout;  // per panel x, y, o (layout units, y up; degrees)
    std::string lightsColors;         // live: per panel "rrggbb", as last sent
    double keySimilarity = 80, keySmoothness = 50;  // reforce.Syphon.KeySimilarity / KeySmoothness (OBS units)
    // Camera backdrop (reforce.Backdrop.*, docs/backdrop.md; videoset backdrop / backdropmirror / backdropfit /
    // backdropopacity).
    bool backdropOn = false;           // a source is set (the Syphon output is opaque)
    std::string backdropSpec;          // its source spec
    std::string backdropSource, backdropPending, backdropWaiting;  // shown / opening / waiting for (labels)
    double backdropLevel = 0;          // fade 0..1 (live)
    bool backdropMirror = false;
    std::string backdropFit = "cover";
    double backdropOpacity = 1;
    int backdropWidth = 0, backdropHeight = 0;
    // Body tracking (reforce.Body.*, docs/body.md; videoset body<key>).
    bool bodyTrack = false, bodyShow = false, bodySync = true;
    std::string bodyQuality = "balanced", bodyBurn = "off";
    double bodyFront = 0, bodyBurnAmount = 0.5, bodyTrails = 0, bodyTrailWidth = 8;
    bool bodyRunning = false, bodyMask = false;  // live: the tracker runs (tracking on and a backdrop) / a mask is in
    double bodyPresence = 0, bodyFps = 0, bodyMs = 0;  // live
    double bodyMaskMs = 0, bodyPoseMs = 0, bodyCamFps = 0;  // live: Vision time each, backdrop frames per second
    int bodyJoints = 0;                                // live: joints seen (smoothed weight > 0.5)
    std::string bodyError;
    std::string beatSource = "none";   // beat clock: link | audio | none (sync/BeatClock.hpp)
    bool beatConfident = false;
    double beatBpm = 0, beatLock = 0;
    double beatOffsetMs = 0;           // reforce.BeatClock.OffsetMs (videoset beatoffset): beats this much earlier
    std::vector<std::string> beatSprites;  // per synced movie: "name: rate 1.10x, phase +0.01 beat"
    std::string lastError;
};

// Rhythm (PLAN 5O) for the web UI: phrases (sync/PhraseClock.hpp), quantize choices, kick zoom.
struct RhythmState {
    bool phraseOn = false;              // the beat clock has a source
    int bars = 16;                      // bars per phrase (reforce.Phrase.Bars)
    double quantum = 4;
    double phrase = 0;                  // running phrase count (live)
    int barInPhrase = 0;                // 0 .. bars-1 (live)
    double bpm = 0;                     // beat clock tempo (live; the panel runs the bar on between reports)
    std::string how = "none";           // where the phrase origin comes from: none | auto | manual | deck
    int deck = -1;                      // how == deck: rekordbox deck 0..3
    bool deckStarts = true;             // reforce.Phrase.DeckStarts
    std::string decks;                  // deck starts status line (live; "" = no rkbx_link data)
    std::string deckInputs;             // "rkbx_link ✓ · MIDI DDJ-1000" ("" = deck starts off)
    bool nowPlaying = false;            // reforce.NowPlaying.Show
    double nowPlayingSize = 100;        // reforce.NowPlaying.Size (%)
    std::string nowPlayingCorner = "bl";
    double nowPlayingSwitch = 3;        // reforce.NowPlaying.SwitchSeconds
    double nowPlayingShadow = 0.8, nowPlayingShadowFrom = 0.55;  // reforce.NowPlaying.Shadow / ShadowFrom
    std::string nowPlayingLine;         // what it shows (live): "deck 2 · ARTIST – TITLE · 2003 + vocal ..."
    bool lyrics = false;                // reforce.Lyrics.Show
    double lyricsSize = 100;            // reforce.Lyrics.Size (%)
    std::string lyricsPosition = "bottom";
    double lyricsOffsetMs = 0;          // reforce.Lyrics.OffsetMs
    bool lyricsReviewedOnly = false;    // reforce.Lyrics.ReviewedOnly
    std::string lyricsLine;             // live: "deck 2 · ARTIST – TITLE · line 12 of 38" / which decks have lyrics
    std::string lyricsLibrary;          // "12 songs in ..." / what went wrong
    double confidence = 0;              // auto: how clear the peak is (live)
    bool autoAlign = true;              // reforce.Phrase.AutoAlign
    std::string quantize = "bar", quantizeManual = "beat", quantizeSpawn = "off";
    double kickZoom = 0, kickRotate = 0, kickDecayMs = 120;
    double kick = 0;                    // the kick pulse now, 0..1 (live)
    bool breakdown = false;             // drops (PLAN 5O.5, live)
    double build = 0, drop = 0;
    int drops = 0;
    bool dropSwitch = false;            // reforce.Drop.Switch
    // Dancers (PLAN 5P): the Dancers card's settings and who is dancing what (live).
    bool danceFloor = true;
    std::string danceMode = "mingle";
    std::string danceRandom = "all";  // reforce.Dancer.RandomStyles (beatset dancerandom)
    bool guests = false;              // reforce.Guests.Enabled (beatset guests)
    double guestMinutes = 30;         // reforce.Guests.Minutes (beatset guestminutes)
    bool danceHidden = false;         // beatset dancehide (session only)
    // Who is on stage, for the panel's Send off / Block buttons: id (beatset dancerleave ID), name, guest id.
    struct DanceRun {
        std::uint32_t id = 0;
        std::string name, guest;
        bool leaving = false;
    };
    std::vector<DanceRun> danceRuns;
    // Past stream guests (web/GuestBook) for the Dancers card, newest first, blocked ones left out.
    struct PastGuest {
        std::string id, label, style;
        double seen = 0;  // ms since 1970
    };
    std::vector<PastGuest> guestBook;
    // reforce.Guests.Blocked (beatset guestsblocked ID,ID,...): guests never put on stage; names where known.
    std::vector<std::pair<std::string, std::string>> guestsBlocked;
    std::string guestStatus;          // "" (off / not polled yet), "ok", or the last error
    int guestArrivals = 0;            // check-ins picked up since reforce started
    double danceLighting = 0.75, danceOutline = 2, danceHeight = 0.24, danceHuman = 1;
    double danceMocap = 1.0;   // reforce.Dancer.Mocap (beatset dancemocap)
    std::string danceTry;      // the mocap move being tried (beatset dancetry), its id; "" = none
    double danceReflection = 0.35;  // reforce.Dancer.Reflection (beatset dancereflect)
    int danceMax = 5;  // reforce.Dancer.MaxLive
    std::vector<std::string> dancing;   // "Name (style): routine"
};

struct EngineState {
    double simTime = 0;
    std::uint64_t simSteps = 0;
    double simRate = 60;
    double displayFps = 0;   // measured presented frames/s (filled by the app for the web UI; 0 = unknown)
    std::string theme = "(Standard)";
    std::vector<std::string> themes;
    std::vector<LayerState> layers;  // WaveShape, ColorMap, FlowField, Union
    std::vector<std::string> recentLog;  // last switches / messages
    PerfState perf;
    AudioState audio;
    VideoState video;
    LookState look;
    RhythmState rhythm;
};

// Latest-snapshot holder shared between the sim thread and readers.
class StateBox {
public:
    void publish(const EngineState& s) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = s;
    }
    // Render thread (Phase 5D): never waits. Swaps `s` in when no reader holds
    // the lock (s then holds the previous state, to be refilled in place);
    // returns false (nothing published) when a reader is copying right now.
    bool tryPublish(EngineState& s) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        std::swap(state_, s);
        return true;
    }
    EngineState snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

private:
    mutable std::mutex mutex_;
    EngineState state_;
};

}  // namespace reforce::engine
