#pragma once
// reforce settings, and the one-time G-Force prefs importer.
//
// reforce reads ONLY its own settings file at startup:
//   ~/Library/Application Support/reforce/settings.json
// a flat JSON object keyed with G-Force's pref names, e.g.
//   { "ColorMap.MapFunction": "i^2.1", "Audio.PCM.NumBins": 255, ... }
// Missing file or keys -> built-in defaults = G-Force 5.9.7's measured
// factory prefs (docs/gforce-defaults.md).
//
// `reforce --import-gforce-prefs [PATH]` snapshots selected keys from G-Force's
//   ~/Library/Preferences/SoundSpectrum/G-Force/Preferences (Standalone).txt
// (`Key = value` lines, quoted strings with \t \r \n \" \\ escapes, keys may be
// quoted) into settings.json once. The G-Force file is only ever read; nothing
// in reforce writes under ~/Library/Preferences/SoundSpectrum.

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/Body.hpp"
#include "engine/LightsSettings.hpp"
#include "engine/VideoComposite.hpp"

namespace reforce::engine {

// Key/value table (case-insensitive keys). Values are kept as text; `quoted`
// records whether the source wrote them as strings.
class PrefsTable {
public:
    struct Entry {
        std::string key;    // original spelling
        std::string value;
        bool quoted = false;
    };

    // G-Force `Key = value` text.
    static PrefsTable parse(std::string_view text);
    // Flat JSON object (strings, numbers, booleans; null entries ignored).
    // Returns nullopt on malformed JSON (error text in *error).
    static std::optional<PrefsTable> parseJson(std::string_view json, std::string* error = nullptr);
    // Flat JSON object, keys sorted case-insensitively. Quoted entries become
    // JSON strings, others numbers (or strings if not numeric).
    std::string toJson() const;

    void set(const std::string& key, const std::string& value, bool quoted);
    std::optional<std::string> get(std::string_view key) const;
    const Entry* entry(std::string_view key) const;
    std::string string(std::string_view key, std::string fallback) const;
    double number(std::string_view key, double fallback) const;
    bool flag(std::string_view key, bool fallback) const { return number(key, fallback ? 1.0 : 0.0) != 0.0; }
    std::size_t size() const { return values_.size(); }
    const std::map<std::string, Entry>& entries() const { return values_; }  // lower-case key -> entry

private:
    std::map<std::string, Entry> values_;
};

// Per-layer slideshow/transition settings (stored now, used by the M5 scheduler).
struct LayerPrefs {
    double transitionDuration = 10.0;   // <Type>.Transition.Duration
    double transitionDeviation = 0.02;  // <Type>.Transition.Duration.Deviation
    double transitionQuick = 0.9;       // <Type>.Transition.Duration.Quick
    std::string transitionFunction = "1-(1-t)^1.5";  // <Type>.Transition.Function
    double slideshowInterval = 20.0;    // <Type>.Slideshow.Interval.Duration
    double slideshowDeviation = 0.1;    // <Type>.Slideshow.Interval.Duration.Deviation
    double duration = 0.0;              // Particle/Sprite .Duration (0 = n/a; Dancer.Duration 0 = forever)
    bool slideshowOnStartup = true;     // <Type>.Slideshow.EnableOnStartup
    double durationDeviation = 0.2;     // Particle/Sprite .Duration.Deviation (relative std. dev.)
};

// Built-in defaults = G-Force 5.9.7's measured factory prefs (docs/gforce-defaults.md).
// The user's own values arrive through settings.json (--import-gforce-prefs).
struct Settings {
    bool loaded = false;  // a settings file was found and parsed
    std::string path;

    // ColorMap
    std::string mapFunction = "i^2.1";  // ColorMap.MapFunction

    // Audio
    std::string inputSource;            // Audio.InputSource ("" = system default; G-Force: first input found)
    int pcmNumBins = 255;               // Audio.PCM.NumBins     -> NUM_SAMPLE_BINS
    int fftNumBins = 255;               // Audio.FFT.NumBins.vis -> NUM_FFT_BINS
    int fftNumBinsIn = 128;             // Audio.FFT.NumBins.in (stored)
    std::string fftParams =             // Audio.FFT.Params
        "FloorRaiseMax=.7 Envelopes=16 InitialVolume=1.0     ShortImpulse=0.60    ShortWindow=0.3    "
        "ShortRamp=44.0     LongImpulse=0.59   LongWindow=0.043  LongRamp=185.0  ExciterImpulse=1.2   "
        "PersistCoef=1.0   PersistLowThresh=.035";
    double pcmSmooth = 0.008;           // Audio.PCM.Smooth
    double fftSmooth = 1.0;             // Audio.FFT.Smooth (frames @ 60 Hz)
    double fftFreqLo = 0.0;             // Audio.FFT.FreqLo (0 = auto)
    double fftFreqHi = 0.0;             // Audio.FFT.FreqHi (0 = auto)
    double responseScale = 1.0;         // Audio.Response.Scale

    // WaveShape lines
    double lineWidthScale = 1.0;        // WaveShape.LineWidth.Scale
    double lineWidthOffset = 0.0;       // WaveShape.LineWidth.Offset
    bool autoLineScale = true;          // WaveShape.AutoLineScale
    std::string lineScale = "gforce";   // reforce.LineScale: gforce (width law on G-Force's frame, which is half size
                                        // above ~5.2 MP, e.g. Retina full screen) | frame (law on the render size)
    bool antiAlias = true;              // WaveShape.AntiAlias
    std::string intensity = "gforce";     // reforce.Intensity: gforce (8-bit trail buffer, like G-Force) | hq (16-bit
                                          // float: no banding/sticking; FlowRounding then doesn't apply). --intensity wins.
    std::string flowRounding = "fixed16"; // reforce.FlowRounding: fixed16 (G-Force, measured by the corpus sweep) |
                                          // floor | point | fixed256 | nearest
    std::string flowPrecision = "double"; // reforce.FlowPrecision: double (default) | float32 (FlowField expressions
                                          // in single precision, a G-Force hypothesis; docs/sweep.md notes)
    std::string thinLines = "smooth";     // reforce.ThinLines: smooth (antialiased round dots/lines, energy matched
                                          // to G-Force) | gforce (G-Force's rasterization, probe #32)
    std::string spriteLayering = "gforce"; // reforce.SpriteLayering: gforce (probe #40: EPen display-only, Pen feeds
                                           // back) | melt (sprites stamped into the feedback) | trails (gforce + soft feed)
    double spriteTrails = 0.35;            // reforce.SpriteTrails: feed strength of `trails` (0..1)
    std::string spriteAlpha = "black";     // reforce.SpriteAlpha: black (transparent image pixels count as black)
                                           // | ignore (G-Force, probe #42: the alpha channel is ignored, the
                                           // colour under transparent pixels shows)

    // Graphics
    double targetFrameRate = 60.0;      // Graphics.TargetFrameRate

    // Slideshow / transitions (M5)
    LayerPrefs flowField{10, 0.02, 0.9, "1-(1-t)^1.35", 20, 0.1, 0};
    LayerPrefs waveShape{13, 0.02, 0.9, "1-(1-t)^1.6", 25, 0.1, 0};
    LayerPrefs colorMap{10, 0.02, 0.9, "1-(1-t)^1.5", 15, 0.1, 0};
    LayerPrefs particle{6, 0.02, 0.9, "1-(1-t)^1.5", 120, 0.1, 25};
    LayerPrefs sprite{6, 0.02, 0.9, "1-(1-t)^1.5", 240, 0.1, 45};
    LayerPrefs union_{6, 0.02, 0.9, "1-(1-t)^1.5", 210, 0.1, 0};
    // reforce-only: Objects (solids and organics, one layer since 2026-11-22): Object.* keys like Sprite.*; interval
    // 0 = none from the slideshow (manual + Themes).
    LayerPrefs object{4, 0.02, 0.9, "1-(1-t)^1.5", 0, 0.1, 40};
    // reforce-only (PLAN 5P): Dancer.* keys like Solid.* (Duration = seconds on stage; fade = the exit).
    LayerPrefs dancer{2, 0.02, 0.9, "1-(1-t)^1.5", 0, 0.1, 90};

    // Themes (scene-format §8): "(Standard)" = no filtering.
    std::string startupTheme = "(Standard)";  // Theme.StartupTheme

    // reforce-only: blend mode per layer (architecture §5a; "gforce" = measured G-Force behavior).
    std::string blendWaveShape = "gforce";    // reforce.Blend.WaveShape  (gforce | aligned | crossfade)
    std::string blendColorMap = "gforce";     // reforce.Blend.ColorMap   (gforce | oklab | hsv-short)
    std::string blendFlowField = "gforce";    // reforce.Blend.FlowField  (gforce | polar | crossfade)

    // reforce-only: app packaging (Phase 5A).
    std::string scenesDir;                    // reforce.ScenesDir (scene root holding Packages/; set by
                                              //   scripts/install-app.sh; --scenes overrides)
    bool startFullScreen = false;             // reforce.Window.StartFullScreen

    // reforce-only: video (Phase 5F).
    std::string externalSpritesDir;           // reforce.ExternalSpritesDir: extra sprite folder (images and
                                              //   movies; like G-Force's ExternalSprites.Path), joins the slideshow
    std::string videoDir;                     // reforce.VideoDir: movie folder for the video source picker
    std::string videoStartupSource;           // reforce.Video.Layer.Source: played on the colour layer at startup
    std::string spriteReplace = "own";        // reforce.SpriteReplace: which running EPen-only sprites a new EPen-only
                                              //   sprite fades out: own = only its own kind (images replace images,
                                              //   movies/live video replace movies/live video); any = G-Force (#44)
    double movieSpriteInterval = -1;          // reforce.MovieSprite.Slideshow.Interval.Duration: seconds between
                                              //   slideshow movie sprites (own timer and shuffle; 0 = none;
                                              //   < 0 = Sprite.Slideshow.Interval.Duration)
    double spriteMix = 0.0;                   // reforce.Sprite.Mix: image sprites' Mix, 0..1 (0 = G-Force; 1 = the
                                              //   sprite takes over its rectangle; engine::spriteLayerPlan)
    double movieSpriteMix = 0.0;              // reforce.MovieSprite.Mix: the same for movie and live video sprites
    std::string movieSpriteColour = "off";    // reforce.MovieSprite.Colour: off (G-Force: movie sprites are luminance in
                                              //   the intensity buffer) | over (their full-colour frames are drawn
                                              //   over the colorized frame, alpha = envelope x (0.5 + 0.5 Mix))
    int movieSpriteMaxLive = 1;               // reforce.MovieSprite.MaxLive: movie / live video sprites at full strength at
                                              // once (0 = no limit); like Object.MaxLive
    double movieSpriteGamma = 1.0;            // reforce.MovieSprite.Gamma: luminance gamma of movie and live sprites
                                              //   (> 0, below 1 brightens; times a movie's own Gamma in .reforce.txt)
    bool videoSpriteLoop = false;             // reforce.Video.Sprite.Loop: movie sprites loop for their lifetime
    bool movieBeatSync = true;                // reforce.Video.BeatSync: movies with a BPM (.reforce.txt) play
                                              //   locked to the beat clock, as sprites and on the colour layer
                                              //   (engine/MovieBeatSync.hpp)
    std::string beatClockSource = "auto";     // reforce.BeatClock.Source: auto (Link, else audio) | link | audio | off
    double beatClockAudioOffsetMs = 10.0;     // reforce.BeatClock.AudioOffsetMs: the onset detector's own delay (+ =
                                              //   the audio clock's beats earlier; measured 10 ms, tools/beat_eval)
    double beatClockOffsetMs = 0.0;           // reforce.BeatClock.OffsetMs: the beat clock runs this much early, to make
                                              //   up for audio / display latency (Link: sampled ahead; audio: the
                                              //   count shifted). Control panel nudge; -200..500
    bool beatClockSteadyTempo = true;         // reforce.BeatClock.SteadyTempo: the audio tempo and beat lock assume a
                                              //   steady tempo (house; breakdowns hold it). 0 = react quickly.
                                              //   Read at startup (audio/BeatTracker.hpp, sync/BeatClock.hpp)
                                              //   (off: play once; the sprite fades out as the movie ends)
    std::string videoLoopList;                // reforce.Video.Loop: movies that always loop, sprite or colour layer
                                              //   ("name; name", matched like reforce.Exclude)
    bool syphonAllowFeedback = false;         // reforce.Syphon.AllowFeedback: allow our own Syphon server as input
    bool syphonAlpha = true;                  // reforce.Syphon.Alpha: send transparency in the Syphon output's alpha
                                              //   (the window is unchanged): the visuals keyed like OBS's Color Key
                                              //   on black, dancers and ColorKey = 0 movies opaque (docs/syphon-alpha.md)
    bool windowVisualsOnly = false;           // reforce.Window.VisualsOnly: the window shows the visuals alone (movie
                                              //   layer and sprites included; no backdrop, dancers or text), for
                                              //   screen-mirrored room lights; Syphon still gets everything
    double syphonKeySimilarity = 80;          // reforce.Syphon.KeySimilarity: OBS Color Key similarity, 1..1000
    double syphonKeySmoothness = 50;          // reforce.Syphon.KeySmoothness: OBS Color Key smoothness, 1..1000
    bool syphonAlphaPremultiplied = false;    // reforce.Syphon.AlphaPremultiplied: colour multiplied by the alpha
                                              //   (for a client that expects premultiplied alpha; OBS: 0)
    // Camera backdrop (docs/backdrop.md): a camera / Syphon / movie under everything, seen where the Syphon alpha would
    // be transparent (same key); while it is on, the Syphon output is opaque.
    std::string backdropSource;               // reforce.Backdrop.Source: "camera:BRIO", "syphon:...", a movie; "" = off
    bool backdropMirror = false;              // reforce.Backdrop.Mirror: flipped left-right
    std::string backdropFit = "cover";        // reforce.Backdrop.Fit: cover | contain | stretch
    double backdropOpacity = 1;               // reforce.Backdrop.Opacity: 0..1 (fades the backdrop to black)
    BodySettings body;                        // reforce.Body.*: body tracking on the backdrop camera (docs/body.md)
    LightsSettings lights;                    // reforce.Lights.*: Nanoleaf room lights following the visuals (docs/lights.md)
    // reforce-only: 3D solids (PLAN 5L, docs/3d-design.md).
    int objectMaxLive = 1;                    // reforce.Object.MaxLive: objects (solids and organics) at full
                                              // strength at once (a new one fades the oldest out under it; one render
                                              // target more for solids and GPU organics, for that crossfade)
    int solidMsaa = 4;                        // reforce.Solid.Msaa: 4 (4x MSAA edges) | 1
    int solidTileSize = 512;                  // reforce.Solid.TileSize: face texture tiles (WaveShape / frame), px
    std::string solidDir;                     // reforce.Solid.Dir: extra folder of solid scene files (any layout)
    // reforce-only: Organics (PLAN 5N, docs/organics-design.md).
    int organicMaxNodes = 6000;               // reforce.Organic.MaxNodes: a tree stops growing here
    // reforce-only: Dancers (PLAN 5P, docs/dancers-design.md). The panel's Dancers card saves Floor .. Height.
    int dancerMaxLive = 5;                    // reforce.Dancer.MaxLive: dancers on stage (a new one replaces the oldest)
    bool dancerFloor = true;                  // reforce.Dancer.Floor: the disco floor under them (only while one is live)
    std::string dancerRandomStyles = "all";   // reforce.Dancer.RandomStyles: styles the generated characters may come in
                                              //   when the next dancer is picked: all | none | disco,rave,...
    double dancerLighting = 0.75;             // reforce.Dancer.Lighting: 0 studio light .. 1 lit only by the visuals
    double dancerReflection = 0.35;           // reforce.Dancer.Reflection: the dancers mirrored in the floor, 0..1
    double dancerOutline = 2.0;               // reforce.Dancer.Outline: outline width, px at 1080p (0 = none)
    double dancerHeight = 0.24;               // reforce.Dancer.Height: dancer height, fraction of the frame height
    double dancerMocap = 1.0;                 // reforce.Dancer.Mocap: share of routines that are mocap moves (0..1)
    double dancerHuman = 1.0;                 // reforce.Dancer.Human: 0 machine-exact .. 1 as designed .. 2 sloppy
    bool guestsEnabled = false;  // reforce.Guests.Enabled: poll the guest service for stream guests (docs/guests.md)
    std::string guestsUrl;       // reforce.Guests.Url: the guest service (no built-in one: nothing polls until set)
    std::string guestsKey;       // reforce.Guests.Key: sent as "Authorization: Bearer KEY" when set (private)
    double guestsMinutes = 30;                // reforce.Guests.Minutes
    std::string guestsBlocked;                // reforce.Guests.Blocked: guest ids never put on stage, comma separated: a guest dances this long per "I'm here"
    int dancerMsaa = 4;                       // reforce.Dancer.Msaa: 4 | 1
    std::string contentDir;                   // reforce.ContentDir: reforce's own content root (Packages/, Themes/,
                                              //   Categories/; default: the source tree's content/)
    VideoLayerParams videoLayer;              // reforce.Video.Layer.<Key>: Opacity, Blend, Fit, Order, Key, KeyColor,
                                              //   Tolerance, Softness, Threshold, Crossfade, Loop, Rate, Offset,
                                              //   Syphon (engine::applyVideoSetting keys)
    // reforce-only: Ableton Link (Phase 5H; sync/LinkClock.hpp).
    bool linkEnabled = true;                  // reforce.Link.Enabled: join a Link session (rekordbox LINK etc.)
    bool linkFollow = true;                   // reforce.Link.Follow: only join once another Link app runs, so reforce
                                              //   adopts its tempo (0 = announce at launch; reforce's 120 BPM can win)
    double linkOffsetMs = 0.0;                // reforce.Link.OffsetMs: sample the session this much ahead (display
                                              //   latency; + = visuals earlier)
    double linkQuantum = 4.0;                 // reforce.Link.Quantum: beats per bar (LINK_PHASE range)
    bool linkDriveBeat = true;                // reforce.Link.DriveBeat: with a session, BEAT/TEMPO follow Link
    std::string linkQuantize = "bar";         // reforce.Link.Quantize: slideshow changes start on a bar | beat | phrase
                                              //   | off (beat / bar: Link only; phrase: the phrase clock, PLAN 5O.1)
    std::string linkQuantizeManual = "beat";  // reforce.Link.QuantizeManual: manual switches wait for bar | beat | phrase
                                              //   | off
    bool slideshowOneFeature = true;          // reforce.Slideshow.OneFeature: the slideshow starts a solid, an organic or a
                                              // movie sprite only while none of the three is at full strength (Peter
                                              // 2026-11-22: "too busy"); manual starts are not held back
    std::string spawnQuantize = "off";        // reforce.Quantize.Spawn: slideshow particles / sprites / solids /
                                              //   organics wait for the next beat | bar | phrase line, or off
    // Phrases (PLAN 5O.1, sync/PhraseClock.hpp).
    int phraseBars = 16;                      // reforce.Phrase.Bars: bars per phrase
    bool phraseAutoAlign = true;              // reforce.Phrase.AutoAlign: find phrase lines from loudness changes
    // Deck starts (PLAN 5O.8, sync/DeckStarts.hpp): a song brought in sets the phrase lines (rkbx_link + DJ MIDI).
    bool phraseDeckStarts = true;             // reforce.Phrase.DeckStarts
    double phraseFaderUp = 1.0 / 3.0;         // reforce.Phrase.FaderUp: channel fader position that counts as "in"
    int phraseRkbxPort = 4470;                // reforce.Phrase.RkbxPort: UDP port of rkbx_link's reforce output
    std::string phraseMidiDevice;  // reforce.Phrase.MidiDevice: only MIDI sources whose name contains this ("" = any source a controller map matches, "none" = no MIDI)
    // Kick zoom (PLAN 5O.3): the FlowField pushes out (and turns) for a moment on every beat.
    double kickZoom = 0.0;                    // reforce.Kick.Zoom: peak zoom per frame (0 = off; .01 = 1 %)
    double kickRotate = 0.0;                  // reforce.Kick.Rotate: peak turn per frame, degrees (+ = anticlockwise)
    double kickDecayMs = 120.0;               // reforce.Kick.DecayMs: how fast the push dies away
    // Now playing (sync/NowPlaying): artist / title / year over the output, from rkbx_link and the faders.
    bool nowPlaying = false;                  // reforce.NowPlaying.Show
    double nowPlayingSize = 100;              // reforce.NowPlaying.Size: % of history_server's size (48 px at 1080)
    std::string nowPlayingCorner = "bl";      // reforce.NowPlaying.Corner: bl | br | tl | tr
    double nowPlayingSwitch = 3;              // reforce.NowPlaying.SwitchSeconds: a louder deck leads this long first
    double nowPlayingShadow = 0.8;            // reforce.NowPlaying.Shadow: adaptive shadow strength 0..1 (0 = off)
    double nowPlayingShadowFrom = 0.55;       // reforce.NowPlaying.ShadowFrom: background brightness where it starts
    // Drops (PLAN 5O.5, sync/DropDetector.hpp).
    bool dropSwitch = false;                  // reforce.Drop.Switch: on a drop, cut FlowField, WaveShape and ColorMap
                                              //   to their next configs
    double linkTransitionBeats = 0.0;         // reforce.Link.TransitionBeats: >0 = slideshow transitions last this
                                              //   many beats with a session (not quick ones; 0 = seconds)
    std::string favorites;                    // reforce.Favorites: control panel favourites, "layer:id; ..." (web/Api.hpp)
    std::string exclude;                      // reforce.Exclude: skip list, "Type:name; ..." (SceneLibrary.hpp)
    std::string spriteHidden;                 // reforce.Sprite.Hidden: sprites hidden from the control panel's
                                              //   Review images: never started, by the slideshow or by hand.
                                              //   "id; id" (SceneLibrary.hpp parseIdList); changed live
    std::vector<std::string> settingErrors;   // values that were ignored (bad video settings)

    // Applies every known key present in `table` over the current values.
    void apply(const PrefsTable& table);
};

// The keys --import-gforce-prefs copies (exact G-Force spelling).
std::vector<std::string> importedGForceKeys();

// ~/Library/Preferences/SoundSpectrum/G-Force/Preferences (Standalone).txt
std::string defaultGForcePrefsPath();
// ~/Library/Application Support/reforce/settings.json
std::string defaultSettingsPath();

// Reads settings.json. Missing file: defaults with loaded == false. Malformed
// file: defaults plus a message in *error.
Settings loadSettings(const std::string& path = defaultSettingsPath(), std::string* error = nullptr);

// Merges `entries` (key, value; written as JSON strings) into the settings
// file, keeping every other key; creates the directory; writes atomically.
// Refuses a malformed existing file (it is not overwritten). Returns false
// with a message in *error on failure.
bool mergeSettingsFile(const std::string& settingsPath,
                       const std::vector<std::pair<std::string, std::string>>& entries,
                       std::string* error = nullptr);
// Same, from a table (keeps each entry's quoted flag).
bool mergeSettingsFile(const std::string& settingsPath, const PrefsTable& entries, std::string* error = nullptr);

struct ImportResult {
    bool ok = false;
    std::string error;
    std::vector<std::pair<std::string, std::string>> imported;  // key, value (as written)
    std::vector<std::string> missing;                           // wanted keys absent in the G-Force file
};

// Reads the G-Force prefs (read-only) and merges the imported keys into the
// settings file (creating its directory). Other keys in settings.json are kept.
// Refuses to write anywhere under ~/Library/Preferences/SoundSpectrum.
ImportResult importGForcePrefs(const std::string& gforcePath, const std::string& settingsPath);

}  // namespace reforce::engine
