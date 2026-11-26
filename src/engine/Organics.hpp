#pragma once
// Organics (PLAN 5N, docs/organics-design.md): one live organic from a scene file in reforce.Organic.package.
//
// Like a solid (engine/Solids.hpp): the config is evaluated per frame (scene::OrganicInstance: B vars, then the
// frame keys) and the lifetime is a fade-in / hold / fade-out envelope (FADER; PBar or PDur, else Object.Duration). The
// kinds (all drawn as line segments, engine::LineSegment, through the WaveShape line pass, one path per organic):
//   tree       engine/TreeGrowth: grows in simulation time, tapered branches (PLAN 5N)
//   pendulums  a pendulum wave (PLAN 5O.2): Num pendulums, pendulum k swinging Osc + k times per cycle of Cyc bars,
//              so they drift into waves and line up again on every cycle line (with the beat clock: the phrase
//              clock's bars, so a 16-bar cycle lines up on the phrase lines)
//
// Scene keys (docs/organics-design.md "Keys"):
//   Kind   literal: tree (default) | pendulums | phyllotaxis | harmonograph | automaton | life | lsystem | attractor |
//          dragon | double | julia | mandel
//   PDur   seconds at full strength (else Object.Duration)
//   Pts, PX, PY (per point, with s), Rts, RtX, RtY, RtA (per root), Seg, Infl, Kill, Jitr, Wdth, Tip, BudW, Wthr,
//   Strk, SGap, SAln, SwyB, VibF, PlsB, PlsE, PlsL, PlsP, Rwir, RwrB, LeafW, LeafD, LeafA, LeafU
//   per frame (s = beats since the current growth began): Grow, TrpX, TrpY, Sway, X, Y, Scal, Rot, Pen, Bud, Vib,
//   Puls, PAll, Leaf, LFlt
//   pendulums: Num, Osc, Cyc, Sprd, Len, View, BobW, Wdth (load); Amp, Strg, Phas, X, Y, Scal, Rot, Pen (frame)
//   ripples: Rate, Rad, Spd, Decy, Edge, Flag (load); DX, DY, Amp, Rain, Gain, Pen (frame)
//   julia: Iter, Flag (load); CRe, CIm, X, Y, Scal, Rot, Band, Phas, Gain, Pen (frame)
//   mandel: Dive, Dbl, Deep, Out, Kick, Drop, Iter, Itk, MaxI, Prec, Res, Mini, Aff, Flag (load); Tgt, X, Y, Scal, Rot, Band,
//           Phas, Gain, Pen (frame)

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/LineGeometry.hpp"
#include "engine/MandelDive.hpp"
#include "engine/OrganicKinds.hpp"
#include "engine/Particles.hpp"
#include "engine/Sprites.hpp"
#include "engine/TreeGrowth.hpp"
#include "expr/Rng.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

// Body: engine/OrganicKinds; Field: the GPU kinds below (kindName() says which).
enum class OrganicKind { Tree, Pendulums, Body, Field };

// GPU kinds (PLAN 5O.6): not line segments but a field drawn by render/FieldPipeline into the organic's own texture,
// composited through the sprite pass like a solid (Pen, Flag, the fade envelope as opacity, trails).
//   julia    the Julia set of z^2 + c, c = CRe + i CIm per frame; Iter iterations, smooth escape-time shading
//   mandel   a Mandelbrot dive on the beat clock (engine/MandelDive); deep views by perturbation (or double-float)
struct OrganicField {
    enum class Kind { Julia, Mandel };
    Kind kind = Kind::Julia;
    double pen = 1, opacity = 1;   // Pen; FADER
    unsigned flags = 0;            // Flag (sprite blend flags)
    double gain = 1;               // display: luminance scale
    // julia
    double cre = -0.8, cim = 0.156;
    double x = 0, y = 0, scale = 1, rot = 0;  // tree units (y = +-1 at the bottom / top): center, size, turn
    int iter = 120;
    double bands = 0, phase = 0;   // Band > 0: bands of the escape count, Band per Iter, moved by Phas (radians)
    // mandel (also x, y, scale, rot, iter, gain, phase; bands = escape-count iterations per band, 0 = none)
    double cx = kMandelHomeRe, cy = 0;  // view center (the plane)
    double half = kMandelHomeHalf;      // half the frame height in plane units at Scal 1
    double depth = 0;                   // log2 magnification
    long long dive = 0;
    int target = 0;
    int precision = 0;                  // 0 float32, 1 double-float, 2 perturbation (render/FieldPipeline)
    double glow = 1;                    // Glow: pixels from the edge lit fully (0 = none)
    // A mini filling the frame (MandelView::mini): its nucleus and size, for the shader's affine interior test.
    bool mini = false;
    double nre = 0, nim = 0, sre = 0, sim = 0;
    double res = 1;                     // Res: render scale (.25..1; the quad samples the rendered corner)
};

const char* organicFieldKindName(OrganicField::Kind k);

const char* organicKindName(OrganicKind k);
// Kind of a compiled organic config: its Kind key, default tree; nullopt for an unknown name.
std::optional<OrganicKind> organicKind(const scene::CompiledConfig& cfg, std::string* written = nullptr);

// Frame mapping for tree units (y = +-1 at the bottom / top, x in the same units) at a render size.
FrameMapping organicMapping(double width, double height);

class OrganicRun {
public:
    // `defaultHold`: seconds at full strength when the config has no PDur. `maxNodes`: reforce.Organic.MaxNodes.
    OrganicRun(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env, std::string name,
               double startTime, double fade, double defaultHold, std::shared_ptr<const TransitionFunction> fn,
               int maxNodes = 6000);

    const std::string& name() const { return name_; }
    OrganicKind kind() const { return kind_; }
    const std::string& kindName() const { return kindName_; }  // "tree", "pendulums", "phyllotaxis", ...
    double startTime() const { return start_; }
    double endTime() const { return start_ + env_.total(); }
    const FadeEnvelope& envelope() const { return env_; }
    bool expired(double t) const { return t - start_ >= env_.total(); }
    void fadeOutFrom(double t, double outFade = -1.0) { env_.fadeOutAt(t - start_, outFade); }
    // Past full strength (fading out or over) at `t`: no longer counts against MaxLive.
    bool fadingOut(double t) const { return env_.fadingOut(t - start_); }
    // PBar (NaN when absent) and an explicit hold in seconds (PBar through engine::holdForBars).
    double bars() const { return inst_->bars(); }
    void setHold(double seconds) {
        if (std::isfinite(seconds) && seconds >= 0) env_.hold = std::min(seconds, 24.0 * 3600.0);
    }
    // After setHold(holdForBars(...)): full strength keeps ending on a line of the live clock (Peter 2026-11-23, the
    // dragons "always phrase aligned"). The time left is counted in bars as the clock runs (so tempo changes move the
    // end with it), a jump of the clock is not counted, and every step the end snaps to the nearest line, `unitBars`
    // apart (1: bar lines; the phrase length when PBar is whole phrases), of the clock as it is now. Without the beat
    // clock, the bodies' fallback: 2 beats a second of age, 4 beats a bar.
    void followBars(double unitBars) {
        barHold_.unit = std::isfinite(unitBars) && unitBars > 0 ? std::min(unitBars, 4096.0) : 0.0;
    }
    const TreeGrowth& tree() const { return *tree_; }  // trees only
    int strike() const { return strike_; }
    bool rewiring() const { return dying_; }  // trees with Rwir: branches withering back              // trees: current growth (Strk), -1 before the first
    std::size_t livePulses() const { return live_.size(); }  // trees with Puls: pulses running
    struct Pulse {
        int root = 0;
        double beat = 0;           // clock beat it fired
        double strength = 1;       // brightness and thickness of the front (bar-line leads, drops: more)
    };
    const std::vector<Pulse>& pulses() const { return live_; }
    // GPU kinds: this step's field (after step()), null for the other kinds or when nothing is to be drawn.
    const OrganicField* field() const { return kind_ == OrganicKind::Field && fieldOn_ ? &field_ : nullptr; }
    // The FieldPipeline slot the Visualizer gave it (-1 = none).
    int fieldSlot() const { return fieldSlot_; }
    void setFieldSlot(int s) { fieldSlot_ = s; }
    std::string summary() const;                        // for the log

    // Evaluates the frame at `t`, grows the tree by the time since the last step, and appends its segments
    // (one path) to `out`.
    // `colors`: the current ColorMap's colors (optional; see OrganicColors).
    void step(double t, const SpriteAudio& audio, const FrameMapping& map, const LineStyle& style,
              std::vector<LineSegment>& out, const OrganicColors* colors = nullptr);

private:
    std::shared_ptr<const scene::CompiledConfig> cfg_;
    std::unique_ptr<scene::OrganicInstance> inst_;
    std::unique_ptr<TreeGrowth> tree_;  // Tree
    OrganicKind kind_ = OrganicKind::Tree;
    std::string kindName_ = "tree";
    std::unique_ptr<OrganicBody> body_;  // Body (after inst_: it holds a reference to it)
    BarHold barHold_;  // followBars
    // Pendulums
    int pendulums_ = 15;
    double osc_ = 16, cycleBars_ = 16, spread_ = 3.0, length_ = 1.4, bobWidth_ = 10, stringWidth_ = 1.5;
    int view_ = 0;  // 0 from above (a row), 1 from the side (strings), 2 a ring (radial swings)
    void stepPendulums(double t, const scene::OrganicFrame& f, double penScale, const SpriteAudio& audio,
                       const FrameMapping& map, const LineStyle& style, std::vector<LineSegment>& out);
    // GPU kinds
    OrganicField field_;
    bool fieldOn_ = false;
    int fieldSlot_ = -1;
    MandelDive mandel_;
    int mandelPrec_ = 0;  // Prec: 0 auto (float32, perturbation past 2^12), 1 float32, 2 double-float, 3 perturbation
    void stepField(double t, double dtSim, const scene::OrganicFrame& f, double fader, const SpriteAudio& audio,
                   const FrameMapping& map);
    std::string name_;
    double start_ = 0;
    FadeEnvelope env_;
    double width_ = 8, tip_ = 1, budWidth_ = 4;
    bool wither_ = true;
    double lastT_ = -1, acc_ = 0;
    std::vector<float> px_, py_, ang_;  // displayed (swayed) positions, per node
    // Trees (docs/organics-design.md "Strikes", "Beat sway", "Vibration", "Pulses").
    std::uint64_t seed_ = 0;
    int maxNodes_ = 6000;
    void buildTree(int strike);
    // Strk: the tree grows Strk times, each from fresh points and roots, on beat lines SGap beats apart.
    int strikes_ = 1;
    double strikeGap_ = 4;
    double strikeAlign_ = 1;       // SAln: strike lines every this many beats (1 beats, 4 bars)
    int strike_ = 0;               // current growth (-1: waiting for the first beat line)
    bool clockStarted_ = false;
    double growBeat_ = 0;          // clock beat when the current growth began
    double nextStrike_ = 0;        // clock beat of the next strike's line
    double swayBeats_ = 0;         // SwyB: beats per sway cycle on the beat clock (0 = the free 0.3 Hz wave)
    double vibRate_ = 3;           // VibF: vibration cycles per beat at the tips
    // Pulses (Puls): one root fires on every PlsE beats, the pulse reaching its farthest tip after PlsB beats.
    bool pulses_ = false;
    double pulseBeats_ = 1, pulseEvery_ = 1;
    std::vector<Pulse> live_;
    long long fireLine_ = 0;
    bool fireStarted_ = false;
    int lastFired_ = -1, beforeFired_ = -1;
    double prevAll_ = 0;
    std::size_t contactNodes_ = 0;
    std::vector<std::uint64_t> contacts_;  // per root: the roots whose branches come within reach of its own
    expr::Pcg32 pulseRng_;
    void updateContacts();
    // Leaves (Leaf > 0): on the outer twigs (nodes feeding at most 4 tips, once the crown has 8), every LeafD-th node
    // and every tip; LeafW long, LeafA off the twig, alternating sides, unfurling over LeafU beats.
    bool leaves_ = false;
    double leafW_ = 0.05, leafD_ = 2, leafA_ = 0.9, leafU_ = 4;
    std::vector<double> leafBorn_;  // per node: clock beat its leaf began to unfurl (NaN: none yet; then it stays)
    void fire(double clock, bool all, double strength = 1.0);
    // Musical structure (PlsL, PlsP): a lead root on every bar line restarts the cascade with a stronger pulse; every
    // root fires on the phrase lines. Lines of the phrase-aligned bar clock (SYNC_BAR, SYNC_PHRASE; 120 BPM without).
    double leadStrength_ = 0, phraseStrength_ = 0;
    long long barLine_ = 0, phraseLine_ = 0;
    bool rewireStarted_ = false;   // the bar / phrase lines have been read once
    double structBeat_ = -1e9;     // clock beat of the last bar / phrase fire (a beat fire this close is skipped)
    std::vector<int> leadOrder_;   // seeded order of the lead roots, one per bar
    // Rewiring (Rwir): on each phrase line a seeded share of the side branches withers back over RwrB bars, is
    // removed (TreeGrowth::prune), and new attraction points (PX / PY again) let new branches grow elsewhere.
    double rewire_ = 0, rewireBars_ = 4;
    bool dying_ = false;
    double dieStart_ = 0;
    std::vector<int> dieRel_, dieMax_;  // per node: depth below its cut (-1: not dying); its cut's deepest
    expr::Pcg32 rewireRng_;
    int contactRevision_ = -1;
    void startRewire(double clock);
    void finishRewire();
};

}  // namespace reforce::engine
