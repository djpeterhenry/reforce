#pragma once
// Sprites (scene-format §5.6): images from the Sprite package drawn into the
// single-channel intensity buffer.
//
// Files: every image (.jpg/.jpeg/.png/.gif/.bmp/.tif/.tiff) and movie
// (isSpriteVideoFile; Phase 5F) in the package, except "-icon" thumbnails and
// names starting with '_'. A movie sprite's texture is the frame at the
// sprite's age (looping), as luminance like an image (app/Visualizer). A sibling .txt with
// the same stem REPLACES `_default_sprite_params.txt` (measured, probe #46: whole-file, not
// per-key: Astronaut/Moon set no Flag and place a small image at a random
// spot, which only makes sense without the default fit-to-frame Flag=7).
// A trailing '~' in the stem inverts the image; parenthesized names are
// script-only (never in the slideshow).
//
// Per frame (SpriteInstance): B vars, then X, Y (center, +-1 = frame edges),
// Pen, EPen. XPOS/YPOS: a point bouncing inside the frame at a speed set by
// Sped (GUESS: Sped pixels/second on a 1260 px tall frame; random start and
// direction from the instance RNG).
//
// Drawing (measured, probes #40-#42; reforce.SpriteLayering = gforce / trails):
//   - x = envelope * pen * L, L = Rec. 601 luminance of the straight colour (inverted per Invert / '~').
//     pen is Pen for the feedback draw, EPen for the display draw (see SpriteLayering below).
//   - Blend by flags, first match wins (F = the target):
//       AlphaBlend     F + x * (1 - F)              (a screen: luminance is the alpha, image alpha ignored)
//       AlphaMask      F * (1 - x)                  (darkens; image alpha ignored)
//       SelectiveBlend F + 0.5 * max(0, x - F)      (half-way lighten)
//       AdditiveBlend  min(1, F + x)
//       ThresholdCopy  replace with x where x >= kSpriteKeyThreshold, else keep F
//       (none)         like ThresholdCopy for Pen and EPen-with-Pen draws, like SelectiveBlend for EPen-only
//   - Several sprites: the oldest draws last (on top).
//   - melt layering keeps reforce's earlier guesses: AlphaBlend / AlphaMask use the image alpha,
//     SelectiveBlend = max(F, x), ThresholdCopy cuts at kSpriteThreshold, no flag = copy over by envelope.
//   - Size: ShrinkToFit / EnlargeToFit scale the native size (1 image px =
//     1 render px) down/up to the frame; MaintainAspect keeps the aspect,
//     covering the frame (cropped) unless DontCrop, which fits it inside.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/Particles.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

namespace sprite {
enum Flag : unsigned {
    ShrinkToFit = 1u << 0,
    EnlargeToFit = 1u << 1,
    MaintainAspect = 1u << 2,
    DontCrop = 1u << 3,
    Invert = 1u << 4,
    ThresholdCopy = 1u << 10,
    AdditiveBlend = 1u << 11,
    SelectiveBlend = 1u << 12,
    AlphaMask = 1u << 13,
    AlphaBlend = 1u << 14,
};
inline constexpr unsigned kDefaultFlags = ShrinkToFit | EnlargeToFit | MaintainAspect;  // Flag=7 [DS]
}  // namespace sprite

// "AlphaBlend|DontCrop|MaintainAspect|ShrinkToFit" (high bits first); unknown bits as numbers.
std::string describeSpriteFlags(unsigned flags);

enum class SpriteBlend : int { Copy = 0, Threshold = 1, Additive = 2, Selective = 3, AlphaMask = 4, AlphaBlend = 5 };
SpriteBlend spriteBlendMode(unsigned flags);
const char* spriteBlendName(SpriteBlend b);

inline constexpr float kSpriteThreshold = 0.5f;  // melt layering: ThresholdCopy / AlphaMask cut (GUESS)
// gforce / trails: keyed copies replace where envelope * pen * lum reaches this. G-Force truncates x to whole
// levels and copies from 10 up (probes #42 and run 32: every x <= 9.9 levels skipped, 10.0 copied, in fades too).
inline constexpr float kSpriteKeyThreshold = (10.0f - 1e-3f) / 255.0f;

// reforce.SpriteLayering: how sprites combine with the FlowField's feedback buffer.
//   gforce  G-Force (probes #40-#42): Pen draws into the feedback before the lines (default: a keyed
//           copy, so it flows and trails); EPen is display-only: without Pen a half-way lighten
//           (out = F + 0.5 * max(0, env * EPen * lum - F)), with Pen EPen drawn like Pen (EPen 0: nothing).
//           Default (EPen-only) sprites leave no trail and wipe nothing. The oldest sprite is on top.
//   melt    reforce before #40: Pen on the display only; EPen (x0.5 without Pen) copied into the
//           feedback after the lines, opaque black: sprites melt into the flow and wipe what's under them.
//   trails  gforce plus a soft feed: EPen-only sprites also lighten the feedback (max) at
//           EPen * reforce.SpriteTrails, so bright parts shed trails that flow away; nothing is wiped.
enum class SpriteLayering { GForce = 0, Melt, Trails };
std::optional<SpriteLayering> parseSpriteLayering(const std::string& s);  // case-insensitive
const char* spriteLayeringName(SpriteLayering m);

// melt draws sprites in start order (newest on top), gforce / trails oldest on top (probe #42).
bool spriteOldestOnTop(SpriteLayering m);

// One draw of a sprite. op Blend uses `blend` (the sprite's own flag mode, or Selective = max for
// the trails feed); KeyedCopy copies pen * lum where the image is non-black (black leaves the target
// alone); Fetch reads the target and applies the measured formula for `blend` (Copy and Selective: the
// half-way lighten; Additive, AlphaMask, AlphaBlend as above).
enum class SpriteOp : int { Blend = 0, KeyedCopy = 1, Fetch = 2 };
struct SpriteLayerDraw {
    SpriteOp op = SpriteOp::Blend;
    SpriteBlend blend = SpriteBlend::Copy;
    double pen = 0;
    double mix = 0;  // sprite Mix (reforce.Sprite.Mix / MovieSprite.Mix) applied to this draw; 0 = G-Force
};
struct SpriteLayerPlan {
    SpriteLayerDraw feedback[2];
    int feedbackCount = 0;
    SpriteLayerDraw display;
    bool hasDisplay = false;
    bool feedbackBeforeLines = true;  // gforce/trails: flow -> stamps -> lines; melt: after the lines
};
struct SpriteDrawSpec;
// Sprite Mix (PLAN 5K, reforce.Sprite.Mix / reforce.MovieSprite.Mix, 0..1, default 0 = G-Force). With p = mix,
// e = envelope and c = coverage (image alpha with SpriteAlpha = black, else 1), the draw that gets the mix first dims
// the target under the sprite, F' = F * (1 - p * e * c), then applies the sprite's own blend to F' with the half-way
// lighten's fraction raised from 0.5 to 0.5 + 0.5 p (a keyed copy keeps its key; render/ScenePipeline
// sprite_fetch_fs). p = 1 on the default (EPen-only) path: out = max(F (1 - e), x), the sprite itself at full
// envelope. Which draw: gforce / trails the display draw only (the feedback, so trails, stays G-Force); a Pen sprite
// with no display draw (EPen absent or 0) gets an extra display draw like its Pen stamp. melt: the feedback draw
// (its EPen-only 0.5 factor becomes 0.5 + 0.5 p), not the Pen display draw.
SpriteLayerPlan spriteLayerPlan(const SpriteDrawSpec& d, SpriteLayering mode, double trailsAmount, double mix = 0);

// Drawn size in render pixels for an image of imgW x imgH in a frame of frameW x frameH.
struct SpriteSize {
    double w = 0, h = 0;
};
SpriteSize fitSprite(double imgW, double imgH, double frameW, double frameH, unsigned flags);
// A sprite that covers the whole frame (auto-fit, [VH 3.0]).
bool coversFrame(const SpriteSize& s, double frameW, double frameH);
// "A new background sprite fades out the running one" ([VH 3.6]), measured (probe #44, runs 37-39): background =
// EPen-only (no Pen, "background-only" in the default params), whatever its size. A new EPen-only sprite fades out
// every running EPen-only sprite in a straight line from its current value to 0 over kSpriteReplaceFade seconds
// (independent of Sprite.Transition.Duration, .Quick and the frame rate); Pen sprites never replace or get replaced.
inline constexpr double kSpriteReplaceFade = 1.3;

// File stem conventions.
struct SpriteName {
    std::string name;        // stem as is (display, catalog, themes)
    bool invert = false;     // trailing '~' (after trimming spaces)
    bool scriptOnly = false; // parenthesized: never in the slideshow
};
SpriteName parseSpriteName(const std::string& stem);
bool isSpriteImageFile(const std::filesystem::path& p);
// Movie sprites (Phase 5F; G-Force plays video files in the Sprite package as
// sprites): .mov .mp4 .m4v .mpg .mpeg .avi .qt .3gp, same exclusions.
bool isSpriteVideoFile(const std::filesystem::path& p);
inline bool isSpriteFile(const std::filesystem::path& p) { return isSpriteImageFile(p) || isSpriteVideoFile(p); }

// Image -> 2 bytes per pixel: [0] straight luminance (Rec. 601 weights,
// inverted when `invert`), [1] alpha (the image's alpha channel, or the
// (inverted) luminance when it has none). Input is PREMULTIPLIED RGBA8 as
// render::loadImageRgba8 decodes it, rows top to bottom.
struct LumaImage {
    int width = 0, height = 0;
    bool hasAlpha = false;
    std::vector<std::uint8_t> la;  // width * height * 2
};
LumaImage toLumaAlpha(const std::uint8_t* rgba, int width, int height, bool invert, bool hasAlphaChannel);
// True when any pixel's alpha is below 255.
bool rgbaHasAlpha(const std::uint8_t* rgba, int width, int height);

// XPOS/YPOS: a point bouncing inside [-1, 1]^2 (frame coordinates), closed form
// so it is frame-rate independent.
struct Bouncer {
    double x0 = 0, y0 = 0;    // position at t0
    double vx = 0, vy = 0;    // frame units per second
    double t0 = 0;
    std::array<double, 2> at(double t) const;
};
// Speed |v| = sped pixels/s on a 1260 px tall frame (GUESS), in a random
// direction from `u1`, starting at (2*u2-1, 2*u3-1); u in [0, 1).
Bouncer makeBouncer(double sped, double frameW, double frameH, double t0, double u1, double u2, double u3);

// Our own minimal stand-in for _default_sprite_params.txt (used when the
// package has none): centered, fit to frame, EPen 1.
extern const char* const kBuiltinSpriteParams;

// Sprite params for an image: the sibling .txt if present, else the
// package's _default_sprite_params.txt (searched upward from the image's
// folder), else kBuiltinSpriteParams. Returns the text and its origin.
struct SpriteParamsText {
    std::string text;
    std::string origin;  // path, or "builtin"
};
SpriteParamsText spriteParamsFor(const std::filesystem::path& image);

// What the GPU sprite pass needs for one sprite this frame.
struct SpriteDrawSpec {
    double cx = 0, cy = 0;   // center, render pixels (origin top-left, +y down)
    double w = 0, h = 0;     // drawn size, render pixels
    double pen = 0, epen = 0;  // as evaluated (clamped to 0..1); layering rules: engine::spriteLayerDraws
    bool hasPen = false;       // Pen defined
    bool hasEPen = false;      // EPen defined
    double opacity = 0;      // fade envelope (FADER)
    SpriteBlend blend = SpriteBlend::Copy;
};

struct SpriteAudio {
    double bass = 0, level = 0, energy = 0;
    double mid = 0, treble = 0, beat = 0, tempo = 0;  // reforce extensions
    double linkOn = 0, linkBpm = 0, linkBeat = 0, linkPhase = 0;  // Ableton Link (LINK_*)
    scene::SyncInput sync;                                         // beat clock (SYNC_*)
};

// One live sprite: config evaluation, envelope, bouncing XPOS/YPOS.
class SpriteRun {
public:
    // `defaultHold`: seconds at full opacity when the config has no PDur.
    SpriteRun(std::shared_ptr<const scene::CompiledConfig> params, const scene::Environment& env, std::string name,
              bool nameInvert, double startTime, double fade, double defaultHold,
              std::shared_ptr<const TransitionFunction> fn);

    const std::string& name() const { return name_; }
    unsigned flags() const { return flags_; }
    bool invert() const { return invert_; }
    bool hasPen() const { return hasPen_; }
    double startTime() const { return start_; }
    double endTime() const { return start_ + env_.total(); }
    const FadeEnvelope& envelope() const { return env_; }
    bool expired(double t) const { return t - start_ >= env_.total(); }
    // Starts the fade-out now over `outFade` seconds (< 0: its own fade).
    void fadeOutFrom(double t, double outFade = -1.0, bool linear = false) { env_.fadeOutAt(t - start_, outFade, linear); }
    // "Loop until stopped" (control panel): full and held until a stop/kill fades it out.
    void holdForever() {
        env_.hold = 1e12;
        env_.earlyOut = -1.0;
        env_.earlyX = 0.0;
        env_.earlyFade = -1.0;
    }
    // Movie sprites that play once: the whole lifetime (fade in, hold, fade
    // out) becomes `seconds`, so the movie plays to its end and the fade-out
    // ends with it. The default hold (Sprite.Duration) gives way to the movie
    // in both directions; a PDur in the sprite's params only ever shortens.
    void fitLifetime(double seconds) {
        if (!(seconds > 0.0)) return;
        if (holdFromParams_ && env_.total() <= seconds) return;
        if (seconds >= 2.0 * env_.fade) {
            env_.hold = seconds - 2.0 * env_.fade;
        } else {
            env_.fade = seconds / 2.0;
            env_.hold = 0.0;
        }
    }

    // Image size (pixels) once known; sets the drawn size for the frame.
    void setImageSize(int w, int h, double frameW, double frameH);
    const SpriteSize& drawSize() const { return size_; }

    using Audio = SpriteAudio;
    SpriteDrawSpec evaluate(double t, double frameW, double frameH, const Audio& audio = {});

private:
    std::shared_ptr<const scene::CompiledConfig> cfg_;
    std::unique_ptr<scene::SpriteInstance> inst_;
    std::string name_;
    unsigned flags_ = sprite::kDefaultFlags;
    bool invert_ = false, hasPen_ = false;
    bool holdFromParams_ = false;  // the params set PDur (else the hold is the default)
    double start_ = 0;
    FadeEnvelope env_;
    Bouncer bounce_;
    SpriteSize size_;
};

}  // namespace reforce::engine
