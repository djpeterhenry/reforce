#pragma once
// Organic kind `dragon` (PLAN 5O.9, docs/rhythm-design.md "Dragon"): the Heighway (paper-folding) dragon unfolding on
// the beat. Generation n + 1 is generation n plus a copy of it turned about its end point; the copy starts folded
// back onto the curve (like a folded strip of paper) and swings open to `Angl` (90 degrees) over the last part of a
// fold interval, landing exactly on the fold line. The view fits the growing curve to the frame.
//
// The schedule (Schedule) is a pure function of the beat clock, so a step that jumps any number of beats costs the
// same as any other, and runs repeat (--deterministic). The curve's points are made once for the last generation:
// with a fixed fold angle generation g is the first 2^g + 1 points of every later one, so a frame only turns the
// moving half (2^g points) and fits the view.

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/OrganicKinds.hpp"

namespace reforce::engine::dragon {

constexpr int kMaxGens = 14;         // 16384 segments per dragon at most
constexpr int kMaxSegments = 16384;  // all dragons together (Num x 2^Gens), before rounded corners

// The regular paper-folding sequence: the turn at joint j (j >= 1, between segments j - 1 and j) of the dragon,
// +1 = left (counterclockwise), -1 = right, for a curve unfolded counterclockwise (Unfold below with angle > 0).
int turn(std::uint64_t j);

struct P2 {
    double x = 0, y = 0;
};

// The points of generation `gens` (0..kMaxGens): 2^gens + 1 points from (0, 0), the first segment to (1, 0), each
// generation the last plus a copy turned by `angle` (radians, + = counterclockwise) about its end point.
std::vector<P2> unfold(int gens, double angle);

// The generation a segment (0-based index along the curve) was born in: 0 for the first, then 1 + floor(log2 i).
inline int bornIn(std::uint64_t i) {
    int g = 0;
    while (i) {
        ++g;
        i >>= 1;
    }
    return g;
}

// Load-time keys (docs/scene-format.md 5.8), clamped.
struct Params {
    int gens = 12;          // generations at full growth (0..14; Num x 2^Gens <= 16384)
    int num = 1;            // dragons tail to tail about one point: 1, 2 (twindragon), 3, 4 (they tile the plane)
    double fold = 1;        // beats per fold
    double cycBars = 4;     // bars per cycle (unfold, hold, refold), on the phrase-aligned bar clock
    double back = 2;        // beats of the refold at the end of each cycle (0 = cut back to one segment)
    double swing = 0.5;     // share of a fold interval the copy swings over (ending on the fold line)
    double stagger = 0;     // with Num > 1: dragon k folds k x Stag fold intervals after dragon 0
    double angle = 90;      // fold angle, degrees (90 = the Heighway dragon)
    double round = 0;       // corner radius as a share of a segment (0 = sharp corners; at most .45)
    double width = 3;       // line width (LWdt) on long segments
    double thin = 0.12;     // LWdt per pixel of segment length: lines thin as the curve gets finer
    double shade = 0.6;     // intensity spread by birth generation (0 = flat, 1 = the first segment black)
    double flash = 0.5;     // extra intensity of the newest copy as it lands, fading over a beat
    // The newest copy swings out at 1 - rise of its brightness and, once landed, brightens smoothly to full just as
    // the next fold starts to swing (Peter 2026-11-23, Dragon Fold); 0 = off.
    double rise = 0;
    // Settle (Peter 2026-11-23, Dragon Fold): the rise goes on past the dragon's steady level to a glow of 1 + settle
    // times it, reached as the next fold starts to swing, then settles back to the level over the next fold interval.
    // The level is 1 / (1 + settle), so the glow peaks at full. A segment's brightness then only depends on how long
    // ago it landed: the latest fold is marked, everything older is even. 0 = off.
    double settle = 0;
    double run = 0;         // a bright band running along the curve once per bar (intensity)
    double spread = 0.45;   // with Num > 1: the dragons' intensities spread over 1 - Sprd .. 1, neighbors apart
    int spin = 0;           // 0: the view turns with the curve (start-to-end chord fixed); 1: the curve turns
    double burst = 1;       // beats a drop's burst unfolds every generation over (SYNC_DROP)
    double fill = 0.9;      // share of the frame the curve fills
    bool vary = true;       // a seeded mirror and turn per cycle
};
Params paramsFrom(const scene::OrganicInstance& in);

// When the folds happen: continuous generations (g + the swing of the next copy, 0..1) at a time.
class Schedule {
public:
    explicit Schedule(const Params& p);
    // Fold count reached in a cycle (Gens, fewer when the cycle is too short for them all).
    int top() const { return top_; }
    double cycleBeats() const { return cycBeats_; }
    // Generations of dragon `k` at `u` beats into its cycle (0..cycleBeats), without a drop.
    double gensAt(double u, int k = 0) const;
    // The same with a drop's burst: `sinceDrop` beats after a drop in this cycle (< 0: none). `uDrop` = u at the drop.
    double gensWithDrop(double u, double sinceDrop, double uDrop, int k = 0) const;
    // Swing ease: share of the fold angle reached at share s of the swing (0 at 0, 1 at 1, fastest at the end).
    static double ease(double s);

private:
    Params p_;
    int top_ = 0;
    double cycBeats_ = 16;
};

std::unique_ptr<OrganicBody> makeDragon(scene::OrganicInstance& inst, std::uint64_t seed);

}  // namespace reforce::engine::dragon
