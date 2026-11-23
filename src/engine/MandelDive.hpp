#pragma once
// The Mandelbrot dive (Kind="mandel", PLAN 5O.6, docs/rhythm-design.md "GPU kinds"): an endless zoom that follows
// the beat clock. Pure function of the clock (plus a smoothed drop), so it is deterministic and seekable.
//
//   - Dive n runs over bars [o + n Dive, o + (n + 1) Dive) of the phrase-aligned bar clock, o = the Algn (16) line at
//     or before the first step (Dive 16: one per phrase; Dive 64: one per four phrases, from the start). Its
//     target is one of a curated list (mandelTarget(i)) of four kinds (MandelKind: spirals, lace, mini-Mandelbrots,
//     the decorations around a mini); the kinds take turns (never the same kind twice in a row), each kind's
//     targets in a seeded order.
//   - Depth (log2 magnification) grows by one every Dbl beats (Dbl <= 0: whatever reaches Deep just before the
//     rush out), easing in over the first beat and softly capped at Deep; over the last
//     Out beats it eases back to 0 (a rush back out to the whole set), so the next dive starts from the same
//     overview: no cut on the phrase line.
//   - The view center slides from the overview's center to the target as it zooms (C = T + (H - T) 2^(-1.5 depth)),
//     so the target drifts into the middle of the frame.
//   - Started mid-dive (by hand: the slideshow waits for a dive line with SBar), it starts from the overview and
//     catches up at Ent doublings a beat to where the dive is, then follows it.
//   - Kicks: Kick doublings surge in on every beat line and relax over the beat. Drops: SYNC_DROP pulls out by Drop
//     doublings (a fast attack, then it follows the drop's decay over the bar and the dive resumes).
//   - Iterations grow with depth as the target needs (MandelTarget::itA + itB depth) x Itk, at least Iter, capped
//     at MaxI.
//   - Mini-Mandelbrots: a dive toward one stops at its depth (the mini filling the frame like the whole set did at
//     the start), turning it upright over the last 10 doublings; its iterations ramp up to 45 x its period (the
//     99th percentile escape count around a mini) over the last 6; the shader skips most of its inside by the
//     affine interior test (MandelView::mini). Decorations: a dive toward a deep mini that stops at `land`, about
//     half way down, in the ring of Julia-like decorations around it (10 x period iterations there).

#include <cstdint>

namespace reforce::engine {

// What a dive ends on.
enum class MandelKind : int {
    Spiral,  // a seahorse-valley Misiurewicz point: spirals of 2..6 arms all the way down
    Lace,    // an elephant-valley Misiurewicz point: lace of elephants' trunks
    Mini,    // a mini-Mandelbrot filling the frame, upright, like the whole set at the start
    Decor,   // the ring of Julia-like decorations around a mini, the mini small in the middle
};
constexpr int kMandelKinds = 4;
const char* mandelKindName(MandelKind k);

struct MandelTarget {
    const char* name;
    MandelKind kind;
    double re, im;
    double itA, itB;  // escape counts a frame at depth d needs on the way (99.5th percentile): itA + itB d
    // Mini / Decor: (re, im) is the mini's nucleus, (sre, sim) its size: the complex factor by which it copies the
    // whole set (c ~ nucleus + size w, w on the set), and its period.
    int period = 0;
    double sre = 0, sim = 0;
    double land = 0;  // Decor: the depth the dive ends at
    constexpr bool hasMini() const { return period > 0; }
    double miniDeep() const;  // log2(1 / |size|): the mini fills the frame like the set at the start
    double deep() const;      // where the dive ends: Mini miniDeep, Decor land, else 0 (the Deep key)
    double angle() const;     // arg(size): the turn that stands the mini upright
    double frameRe() const;   // where the dive heads: Mini nucleus - .65 size (framed like the set), else re
    double frameIm() const;
};

int mandelTargetCount();
const MandelTarget& mandelTarget(int i);  // i wraps

// The overview: the whole set, centered on (kHomeRe, 0), +-kHomeHalf high at Scal 1.
constexpr double kMandelHomeRe = -0.65;
constexpr double kMandelHomeHalf = 1.25;

struct MandelDiveParams {
    double diveBars = 16;   // Dive
    double dbl = 2;         // Dbl: beats per doubling (<= 0: auto)
    double deep = 34;       // Deep: max log2 depth (df64 stays clean to about 2^36)
    double out = 4;         // Out: beats of the rush back out at the end of a dive (0 = a cut on the line)
    double kick = 0.3;      // Kick: doublings on every beat line
    double drop = 6;        // Drop: doublings pulled out at a drop
    int iter = 200;         // Iter: at least this many iterations
    double itk = 1.5;       // Itk: x the iterations the target needs at the depth
    int maxIter = 4000;     // MaxI
    double entry = 6;       // Ent: doublings per beat while catching up after a start mid-dive (0 = jump straight in)
    double align = 16;      // Algn: dives count from the phrase line (every Algn bars) at or before the start; 0 = bar 0
    int minis = 0;          // Mini: 0 = every kind, 1 = minis and decorations only, 2 = no minis
    bool affine = true;     // Aff: the affine interior test on a mini (0: off, to measure what it saves)
    // Shared (the organics): one sequence of dives for the whole process, so a new Mandelbrot object carries on
    // where the last one stopped (another kind of ending, every target in turn), in the first object's seeded order.
    bool shared = false;
};

struct MandelView {
    long long dive = 0;     // dive index (bar / Dive)
    int target = 0;         // index into mandelTargets
    double pos = 0;         // 0..1 through the dive
    double depth = 0;       // log2 magnification (after kicks and drops)
    double cx = kMandelHomeRe, cy = 0;  // view center
    double half = kMandelHomeHalf;      // half the frame height in plane units (before Scal)
    double turn = 0;                    // radians added to Rot: a mini turns upright as the dive lands on it
    bool mini = false;                  // a mini fills a good part of the frame: its affine interior test is safe
    int iter = 120;
};

class MandelDive {
public:
    MandelDive() = default;
    MandelDive(const MandelDiveParams& p, std::uint64_t seed);
    const MandelDiveParams& params() const { return p_; }
    // Target of dive n: `fixed` >= 0 picks that entry; else a seeded shuffle of the table, reshuffled each round.
    int targetFor(long long dive, int fixed) const;
    // beat, bar: the clock (bar lines on whole numbers, aligned to the phrase); drop: SYNC_DROP; dt: seconds since
    // the last step (smooths the drop's attack).
    MandelView step(double beat, double bar, double drop, double dt, int fixedTarget);

private:
    MandelDiveParams p_;
    std::uint64_t seed_ = 0;
    double dropS_ = 0;
    double startBeat_ = 0;  // beat of the first step: started mid-dive, the zoom catches up from the overview
    double originBar_ = 0;  // bar the dives count from (the Algn line at or before the first step)
    bool started_ = false;
    long long base_ = 0;  // shared: the process-wide dive number of this object's first dive
};

}  // namespace reforce::engine
