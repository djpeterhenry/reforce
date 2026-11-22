#pragma once
// Organics (PLAN 5N, docs/organics-design.md): one live organic from a scene file in reforce.Organic.package.
//
// Like a solid (engine/Solids.hpp): the config is evaluated per frame (scene::OrganicInstance: B vars, then the
// frame keys) and the lifetime is a fade-in / hold / fade-out envelope (FADER; PDur, else Organic.Duration). The
// kinds (all drawn as line segments, engine::LineSegment, through the WaveShape line pass, one path per organic):
//   tree       engine/TreeGrowth: grows in simulation time, tapered branches (PLAN 5N)
//   pendulums  a pendulum wave (PLAN 5O.2): Num pendulums, pendulum k swinging Osc + k times per cycle of Cyc bars,
//              so they drift into waves and line up again on every cycle line (with the beat clock: the phrase
//              clock's bars, so a 16-bar cycle lines up on the phrase lines)
//
// Scene keys (docs/organics-design.md "Keys"):
//   Kind   literal: tree (default) | pendulums | phyllotaxis | harmonograph | automaton | life | lsystem | attractor |
//          ripples | julia
//   PDur   seconds at full strength (else Organic.Duration)
//   Pts, PX, PY (per point, with s), Rts, RtX, RtY, RtA (per root), Seg, Infl, Kill, Jitr, Wdth, Tip, BudW, Wthr
//   per frame: Grow, TrpX, TrpY, Sway, X, Y, Scal, Rot, Pen, Bud
//   pendulums: Num, Osc, Cyc, Sprd, Len, View, BobW, Wdth (load); Amp, Strg, Phas, X, Y, Scal, Rot, Pen (frame)
//   ripples: Rate, Rad, Spd, Decy, Edge, Flag (load); DX, DY, Amp, Rain, Gain, Pen (frame)
//   julia: Iter, Flag (load); CRe, CIm, X, Y, Scal, Rot, Band, Phas, Gain, Pen (frame)

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/LineGeometry.hpp"
#include "engine/OrganicKinds.hpp"
#include "engine/Particles.hpp"
#include "engine/Sprites.hpp"
#include "engine/TreeGrowth.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

// Body: engine/OrganicKinds; Field: the GPU kinds below (kindName() says which).
enum class OrganicKind { Tree, Pendulums, Body, Field };

// GPU kinds (PLAN 5O.6): not line segments but a field drawn by render/FieldPipeline into the organic's own texture,
// composited through the sprite pass like a solid (Pen, Flag, the fade envelope as opacity, trails).
//   ripples  the wave equation on a grid (reforce.Organic.RippleRows rows): a drop on every 1 / Rate beats at DX, DY
//            (random when absent), Rain more small drops per second; the crests are drawn
//   julia    the Julia set of z^2 + c, c = CRe + i CIm per frame; Iter iterations, smooth escape-time shading
struct OrganicField {
    enum class Kind { Ripples, Julia };
    Kind kind = Kind::Ripples;
    bool reset = false;            // first step: start from still water
    double pen = 1, opacity = 1;   // Pen; FADER
    unsigned flags = 0;            // Flag (sprite blend flags)
    // ripples
    struct Drop {
        double x = 0.5, y = 0.5;   // frame UV (0..1, origin top-left)
        double radius = 0.03;      // frame heights
        double amp = 1;
    };
    std::vector<Drop> drops;       // this step's new drops
    double dt = 0;                 // seconds simulated this step
    double speed = 0.4;            // wave speed, frame heights per second
    double decay = 2.5;            // seconds to 1/e
    double gain = 3;               // display: luminance per unit of height
    bool reflect = false;          // Edge = 1: the frame's edges reflect; 0: they absorb
    // julia
    double cre = -0.8, cim = 0.156;
    double x = 0, y = 0, scale = 1, rot = 0;  // tree units (y = +-1 at the bottom / top): centre, size, turn
    int iter = 120;
    double bands = 0, phase = 0;   // Band > 0: bands of the escape count, Band per Iter, moved by Phas (radians)
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
    const TreeGrowth& tree() const { return *tree_; }  // trees only
    // GPU kinds: this step's field (after step()), null for the other kinds or when nothing is to be drawn.
    const OrganicField* field() const { return kind_ == OrganicKind::Field && fieldOn_ ? &field_ : nullptr; }
    // The FieldPipeline slot the Visualizer gave it (-1 = none).
    int fieldSlot() const { return fieldSlot_; }
    void setFieldSlot(int s) { fieldSlot_ = s; }
    std::string summary() const;                        // for the log

    // Evaluates the frame at `t`, grows the tree by the time since the last step, and appends its segments
    // (one path) to `out`.
    void step(double t, const SpriteAudio& audio, const FrameMapping& map, const LineStyle& style,
              std::vector<LineSegment>& out);

private:
    std::shared_ptr<const scene::CompiledConfig> cfg_;
    std::unique_ptr<scene::OrganicInstance> inst_;
    std::unique_ptr<TreeGrowth> tree_;  // Tree
    OrganicKind kind_ = OrganicKind::Tree;
    std::string kindName_ = "tree";
    std::unique_ptr<OrganicBody> body_;  // Body (after inst_: it holds a reference to it)
    // Pendulums
    int pendulums_ = 15;
    double osc_ = 16, cycleBars_ = 16, spread_ = 3.0, length_ = 1.4, bobWidth_ = 10, stringWidth_ = 1.5;
    int view_ = 0;  // 0 from above (a row), 1 from the side (strings), 2 a ring (radial swings)
    void stepPendulums(double t, const scene::OrganicFrame& f, double penScale, const SpriteAudio& audio,
                       const FrameMapping& map, const LineStyle& style, std::vector<LineSegment>& out);
    // GPU kinds
    OrganicField field_;
    bool fieldOn_ = false, fieldStarted_ = false;
    int fieldSlot_ = -1;
    double dropRate_ = 1, dropRadius_ = 0.03, rainAcc_ = 0;
    long long lastDrop_ = 0;
    bool haveDrop_ = false;
    std::uint64_t rng_ = 0;
    void stepField(double t, double dtSim, const scene::OrganicFrame& f, double fader, const SpriteAudio& audio,
                   const FrameMapping& map);
    std::string name_;
    double start_ = 0;
    FadeEnvelope env_;
    double width_ = 8, tip_ = 1, budWidth_ = 4;
    bool wither_ = true;
    double lastT_ = -1, acc_ = 0;
    std::vector<float> px_, py_, ang_;  // displayed (swayed) positions, per node
};

}  // namespace reforce::engine
