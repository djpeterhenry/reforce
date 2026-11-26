#pragma once
// reforce extension (docs/3d-design.md "Tower of Hanoi", Peter 2026-11-26): the Tower of Hanoi behind the `hanoi`
// solid, solved on the beat. Pure C++, no allocations per frame (the move list is made once per Plan), so it can run
// on the render thread every frame.
//
// A run of `beats` beats: the first `lead` beats (beats - moves x each) are the reset, then one disc move every
// `each` beats, move k landing exactly on the line lead + k x each, so the last lands on the run's end (6 discs, 63
// moves, each 1, 64 beats: a 16-bar phrase with one beat for the reset). Each run is the minimal solve from its
// start peg to its target peg (2^n - 1 moves). Reset 0 (`Rset`): the tower carries on from the peg it was solved on
// (run r goes from peg 2r mod 3 to the next peg round to the left: 0 -> 2, 2 -> 1, 1 -> 0, ...), and the reset is the
// solved tower springing open and shut, landing on the line; reset 1: every run goes from the left peg to the right
// one, and the reset lifts the whole stack off the right peg, carries it back and drops it onto the left one.
//
// Everything is a pure function of (settings, run, turn) plus two per-move style values the caller latches when a
// move starts (Style: a drop's higher arc and flips), so a clock jump or a tempo change is a lookup; there is nothing
// to plan ahead (the state after k moves is a table made once).
//
// Tower space: x along the pegs (left to right), y up (the base's top at y = 0), z towards the viewer. Discs are glass
// washers: an annulus from the hole (kHole) to their outer radius, kThick high, with half-round inner and outer rims.

#include <array>
#include <cstdint>

namespace reforce::engine::hanoi {

inline constexpr int kMinDiscs = 3, kMaxDiscs = 8, kPegs = 3;
inline constexpr int kMaxMoves = (1 << kMaxDiscs) - 1;

// Geometry (tower units).
inline constexpr double kThick = 0.26;    // disc height
inline constexpr double kHole = 0.17;     // disc hole radius
inline constexpr double kPegR = 0.085;    // peg radius (rounded top)
inline constexpr double kMinR = 0.5, kMaxR = 1.0;  // smallest / largest disc's outer radius
inline constexpr double kSpacing = 2.3;   // between pegs
inline constexpr double kBaseR = 1.14;    // the base: a stadium, the pegs' line +- kSpacing, round ends of this radius
inline constexpr double kBaseT = 0.22;    // ... this thick
inline constexpr double kBaseBevel = 0.07;
inline constexpr double kRadius = kSpacing + kBaseR;  // half the base's length: Scal (the solid's circumradius)
inline constexpr double kHover = 0.06;    // the stacks open up this much per level at Hovr 1
inline constexpr double kClear = 0.08;    // a carried disc's bottom clears the peg tops by this

inline double pegX(int peg) { return (peg - 1) * kSpacing; }
double discRadius(int size, int discs);  // outer radius of disc `size` (0 = the smallest)
double pegHeight(int discs);             // above the base's top (rounded top included)
int moveCount(int discs);                // 2^discs - 1

struct Settings {
    int discs = 6;      // Disc, 3..8
    double beats = 64;  // Beats: the run's length, at least moves x each + .25 (raised to fit)
    double each = 1;    // Each: beats per move, .25..16
    int reset = 0;      // Rset: 0 = carry on from the solved peg, 1 = slide back to the left peg
};
Settings clampSettings(Settings s);  // junk-safe (NaN: the defaults)

// One move of the canonical solve (peg 0 -> 2): disc `disc` (0 = smallest) from `from` (where it is level
// `fromLevel`, 0 = on the base) to `to` (landing on level `toLevel`).
struct Move {
    int8_t disc = 0, from = 0, to = 0, fromLevel = 0, toLevel = 0;
};

// Per-move style, latched by the caller when the move starts (so a value changing mid-flight never jumps the disc).
struct Style {
    double lift = 1;  // Lift: the arc's height x this (0..3; a drop: higher)
    int flips = 0;    // half turns the disc tumbles over in flight (0..2; a drop: 2 = a full flip)
};
Style makeStyle(double lift, double flip);  // flip 0..1 -> round(2 x flip) half turns; junk-safe

struct Controls {
    double hover = 0;  // Hovr 0..1: the stacks open up and bob (a build)
    Style style;       // the move in flight (or the reset's carry)
};

struct Disc {
    int size = 0;
    int peg = 0;                 // the peg it is on (or headed for)
    double x = 0, y = 0, z = 0;  // its center (tower units)
    double tilt = 0;             // radians about tower z (the swing in flight, flips)
    double squash = 1;           // height x this (the landing), widths x (1 + (1 - squash) / 2)
    double glint = 0;            // 0..1: just landed (a flash on the rim)
    bool moving = false;
};

struct Frame {
    int discs = 0;
    std::array<Disc, kMaxDiscs> disc{};  // by size, smallest first
    int done = 0;          // moves landed this run, 0..moveCount
    int move = -1;         // the move in flight (0-based), -1 when none
    bool resetting = false;  // in the reset (before the first move)
    bool solved = false;     // every disc landed on the target (the run's end)
    int start = 0, target = 2;  // this run's pegs
    double turn = 0;       // beats into the run (clamped)
    double lead = 1;       // beats before the first move
};

class Plan {
public:
    explicit Plan(const Settings& s);
    const Settings& settings() const { return s_; }
    int moves() const { return count_; }
    const Move& move(int k) const { return moves_[std::size_t(k)]; }
    // Canonical peg and level of disc d after k moves (0..moves).
    int pegAfter(int k, int d) const { return peg_[std::size_t(k)][std::size_t(d)]; }
    int levelAfter(int k, int d) const { return level_[std::size_t(k)][std::size_t(d)]; }
    double lead() const { return s_.beats - count_ * s_.each; }
    double landing(int k) const { return lead() + k * s_.each; }  // move k (1-based) lands on this beat
    double resetBeats() const;  // the reset's length (the first `lead` beats or less)

    // The run's pegs: start and target (and where the last run left the tower).
    static int64_t runOf(double seed);
    int startPeg(int64_t run) const;
    int targetPeg(int64_t run) const;
    // The move in flight at `turn` (0-based), -1 in the reset (the carry), -2 when nothing moves.
    int moveAt(double turn) const;

    Frame frameAt(double seed, double turn, const Controls& c = {}) const;

private:
    Settings s_;
    int count_ = 0;
    std::array<Move, kMaxMoves> moves_{};
    std::array<std::array<int8_t, kMaxDiscs>, kMaxMoves + 1> peg_{}, level_{};
};

// A carried path: up from (xa, ya), over an arc of height `arc` above `yc`, down to (xb, yb); `u` 0..1 along it in
// time (eased in, landing with speed). Returns the position and the arc's angle (0..pi; 0 before it, pi after it).
struct PathPoint {
    double x = 0, y = 0, theta = 0;
};
PathPoint carry(double xa, double ya, double xb, double yb, double yc, double arc, double u);

}  // namespace reforce::engine::hanoi
