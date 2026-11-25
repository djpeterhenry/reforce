#pragma once
// reforce extension (docs/3d-design.md "Rain on glass", Peter 2026-11-23): the CPU side of the rain object
// (Mesh="rain"). The GPU side (height fields, optics) is render/RainPipeline.
//
// Two modes (scene key Rain):
//   - window: the frame is a vertical pane with rain on it. Drops land on beat lines, sit as beads (spherical caps),
//     merge when they touch (volume conserved, a wobble along the merge axis), and once a drop grows past its slide
//     threshold it runs down with stick-slip motion (pinned for a moment, then off again, wandering sideways and
//     following wet trails), sweeping up what it meets, shedding residual droplets and a wet streak that wipes the
//     condensation fog. Drops leave only at the bottom.
//   - floor: the frame is a horizontal glass floor seen from above, water standing in puddles on it. A drop striking a
//     puddle pushes the surface down (an impulse for the GPU wave equation) and throws a crown of droplets that fly
//     and land as smaller impulses; on the dry glass between puddles drops stay as beads and merge.
//
// Units: frame heights. x runs 0 .. aspect (left to right), y 0 .. 1 (top to bottom). Radii are contact radii;
// a drop's volume is volumeOf(r) (a spherical cap of the contact angle, Bead), so merging conserves sum r^3.
//
// Time: a fixed substep (kSubstep = 1/120 s) from the start, so the state depends on the beat clock and the seed, not
// on the frame rate. A step that jumps more than kMaxCatchUp substeps simulates only the last ones, and at most
// kMaxLines 16th lines are spawned for it; a clock jumping back re-bases. Junk inputs (NaN, inf, 1e300) are
// clamped or treated as no clock.
//
// Music: everything spawns on the 16th-note grid of the beat clock (the phrase-aligned SYNC_* while it is on, else
// 120 BPM from the start):
//   - every beat line: Dens drops (sizes around Size);
//   - every bar line: Clus clusters (one big drop with a ring of satellites);
//   - the other 16ths: Drz drizzle drops (small);
//   - a build (SYNC_BUILD) thins the beat drops and thickens the drizzle (the rain building), a drop (SYNC_DROP) is
//     a downpour (up to 6x the drops, a cluster on every beat);
//   - window with Wipe: on phrase lines the slide threshold drops for a beat, so a cascade runs down the pane.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace reforce::engine::rain {

enum class Mode { Window, Floor };
std::optional<Mode> parseMode(std::string_view s);  // "window" | "floor" | "0" | "1" (case-insensitive)
const char* modeName(Mode m);

inline constexpr double kSubstep = 1.0 / 120.0;
inline constexpr int kMaxCatchUp = 30;    // substeps simulated per step at most (a quarter second)
inline constexpr int kMaxLines = 16;      // 16th lines spawned per step at most
inline constexpr int kMaxPerLine = 48;    // drops spawned per 16th line at most
inline constexpr int kMaxImpulses = 48;   // floor: impulses handed to the GPU per step
inline constexpr int kMaxWipes = 96;      // window: wiped capsules handed to the GPU per step

// Load-time settings.
struct Settings {
    Mode mode = Mode::Window;
    double slide = 17.0;    // Slid: radius (px at 1080p) past which a window drop runs down
    double contact = 62.0;  // Bead: contact angle in degrees (20..85): flatter or rounder beads
    bool wipe = true;       // Wipe: window: a cascade on every phrase line
    int maxDrops = 700;     // Max: drops alive at most (the smallest evaporate first)
    std::uint64_t seed = 0;
};

// Per-frame controls.
struct Controls {
    double density = 4.0;  // Dens: drops per beat line
    double size = 11.0;    // Size: mean drop radius, px at 1080p
    double drizzle = 1.0;  // Drz: small drops per 16th line
    double cluster = 1.0;  // Clus: clusters per bar line
    double wet = 0.65;     // Wet: floor: the share of the glass under puddles (0 dry .. 1 one sheet of water)
    double flow = 1.0;     // Flow: floor: how much the puddles' edges move (creep, swell with the rain; 0 = still)
};

// The look (render/RainPipeline), per frame.
struct Look {
    Mode mode = Mode::Window;
    float depth = 0.5f;    // Dpth: distance from the glass to the visuals, frame heights
    float fog = 0.8f;      // Fog: condensation, 0..1 (window)
    float refog = 25.0f;   // Refg: seconds for wiped glass to fog up again
    float frost = 16.0f;   // Fros: blur of the fog, px at 1080p
    float spec = 1.0f;     // Spec: highlight strength
    float rim = 1.0f;      // Rim: dark contact line and halo
    float wet = 0.65f;     // Wet: floor puddle cover
    float pudl = 1.0f;     // Pudl: how visible the floor's puddles are (rim, contact line, deeper inside; 0 = none)
    float speed = 0.3f;    // Spd: floor ripple speed, frame heights / s
    float decay = 1.6f;    // Dcay: floor ripple decay, s
    float opacity = 1.0f;  // fade envelope
    bool opaque = false;   // ColorKey = 0: the water raises the tracked alpha
};

// The beat clock (scene::SyncInput's SYNC_* values).
struct Clock {
    bool on = false;
    double beat = 0, bar = 0, phrase = 0, build = 0, drop = 0;
};

struct Drop {
    double x = 0, y = 0;  // center (frame heights)
    double r = 0;         // contact radius at rest (frame heights)
    double vx = 0, vy = 0;
    double age = 0;               // seconds since it landed (the impact squash)
    double wobble = 0;            // merge wobble amplitude (0..~.4) along wobbleAxis, decaying
    double wobbleAxis = 0;        // radians (0 = along x)
    double wobbleT = 0;           // seconds since the last merge
    double slideAt = 0;           // its own slide threshold (frame heights)
    double pinned = 0;            // seconds still pinned (stick-slip)
    double trailLeft = 0;         // distance to the next residual droplet
    double stretch = 1;           // elongation along the motion (smoothed)
    double flight = 0;            // floor: seconds of flight left (a crown droplet in the air), 0 = on the glass
    double wx = 0, wy = 0;        // where it was at the start of this step (its wipe this step)
    std::uint32_t id = 0;
    std::int32_t pinCell = 0;     // the last stick-slip cell rolled
    bool sliding = false;
};

// Floor: a push on the water surface for this step (the GPU wave equation adds amp x a Gaussian).
struct Impulse {
    double x = 0, y = 0, radius = 0, amp = 0;
};

// Window: a capsule a running drop wiped this step (fog cleared, a wet film left).
struct Wipe {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, radius = 0;
};

// Totals for the tests (volumes in frame heights^3 of volumeOf).
struct Tally {
    double spawned = 0;     // landed on the glass (or in a puddle)
    double exited = 0;      // ran off the bottom
    double evaporated = 0;  // removed small (Max, tiny residuals)
    double film = 0;        // left behind as the wet streak
    double pooled = 0;      // floor: fell into a puddle
    std::int64_t merges = 0, slides = 0, residuals = 0, lines = 0, drops = 0, cascades = 0;
    double exitMinY = 1e9;  // the smallest y (center) a drop left the pane at
};

// The spherical-cap volume of contact radius r at contact angle `degrees`.
double volumeOf(double r, double degrees);
double radiusOf(double volume, double degrees);

// Floor puddles. The water's edge is where puddleField(x, y, t) crosses puddleThreshold(wet): two fixed octaves of
// seeded value noise (the puddles' shapes) plus two that slowly change over the creep time t (Sim::puddleTime()), so
// the edges creep a little, each of them a crossfade between noise layers (9 s and 4 s apart). puddle() is the cover
// at (x, y) frame heights, 0 dry .. 1 in a puddle, for cover `wet` (0..1). The GPU computes the same functions
// (render/RainPipeline's puddleN / puddleThr / puddle).
double puddleField(double x, double y, double t, std::uint32_t seed);
double puddleThreshold(double wet);
double puddle(double x, double y, double wet, std::uint32_t seed, double t = 0.0);
// An upper bound of |grad puddleField| (per frame height): a point this far below the threshold, over the slope, is
// at least that far from any water.
inline constexpr double kPuddleSlope = 9.0;

// One floor wave-equation step of `dt` seconds on a grid `rows` cells high (render/RainPipeline, rain_wave_fs):
//   h' = (2 h - h_prev + c2 L(h) - b L(L(h)) + nu L(h - h_prev)) * damp - impulses
// An impulse is a crater with its rim, amp (1 - q^2) exp(-q^2), q = distance / radius: no net push, so the mean level
// never drifts.
// L = the 5-point Laplacian. Stable (no growth) while 8 c2 + 64 b + 16 nu <= 4 (stable() below).
struct WaveStep {
    int substeps = 1;
    double c2 = 0, b = 0, nu = 0, damp = 1;
    bool stable() const { return 8.0 * c2 + 64.0 * b + 16.0 * nu <= 4.0 + 1e-9 && nu >= 0 && nu <= 0.25; }
};
inline constexpr int kMaxWaveSubsteps = 12;
// speed: frame heights per second; decay: seconds to 1/e. dt is clamped to 0..0.25 s.
WaveStep waveStep(double dt, double speed, double decay, int rows);

// A CPU copy of the GPU wave step (tests: stability over long runs, the GPU kernel against it).
class WaveGrid {
public:
    WaveGrid(int cols, int rows);
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    // One substep; dry (0..1 per cell, may be empty) damps the dry glass.
    void substep(const WaveStep& w, const std::vector<Impulse>& impulses, const std::vector<float>* wetMask = nullptr);
    double maxAbs() const;
    double at(int x, int y) const { return cur_[std::size_t(y * cols_ + x)]; }

private:
    int cols_, rows_;
    std::vector<double> cur_, prev_, next_, lap_;
};

class Sim {
public:
    // aspect = width / height of the frame; startTime = the object's start (seconds).
    Sim(const Settings& s, double aspect, double startTime);

    // Advances to scene time t. Clears and refills this step's impulses() and wipes().
    void step(double t, const Controls& c, const Clock& clock);

    // Puts a drop on the glass now (tests; counted as spawned).
    void place(double x, double y, double r);

    const Settings& settings() const { return settings_; }
    double aspect() const { return aspect_; }
    const std::vector<Drop>& drops() const { return drops_; }
    const std::vector<Impulse>& impulses() const { return impulses_; }
    const std::vector<Wipe>& wipes() const { return wipes_; }
    const Tally& tally() const { return tally_; }
    // Total volume on the glass now.
    double volume() const;
    // The 16th lines spawned so far (the line index k: beat = k / 4), for the tests; the last 256 kept.
    const std::vector<std::int64_t>& spawnLines() const { return lines_; }
    double cascade() const { return cascade_; }  // seconds of the phrase-line cascade left
    double simTime() const { return double(done_) * kSubstep; }
    // Seconds simulated in the last step (the GPU steps the fog / waves by the same time).
    double lastDt() const { return lastDt_; }
    // Floor: the puddles' creep time (advances at Flow) and their cover now: Wet plus a swell that rises while rain
    // pours into them and drains over ~20 s (none when Wet is 0 or 1). The GPU draws puddle(x, y, puddleWet(),
    // seed, puddleTime()).
    double puddleTime() const { return creep_; }
    double puddleWet() const;
    double swell() const { return swell_; }
    // Floor: whether a drop of contact radius r at (x, y) touches a puddle (its center or any point of its contact
    // line in the water); (cx, cy) is where it touches (the center when that is wet).
    bool touchesPuddle(double x, double y, double r, double& cx, double& cy) const;

private:
    void spawnLine(std::int64_t k, double beatAt, const Clock& clock, const Controls& c);
    void addDrop(double x, double y, double r, bool impact, std::uint64_t h);
    void substep(const Controls& c);
    void slide(Drop& d, double h, Drop& residual);
    void mergeAll();
    void bound();
    void pool(double volume);  // floor: volume taken into a puddle (counted, swells the puddles)
    double rand01(std::uint64_t& h) const;

    Settings settings_;
    double aspect_ = 16.0 / 9.0;
    double start_ = 0;
    double contact_ = 62;
    std::int64_t done_ = 0;         // substeps simulated
    bool first_ = true;
    double lastBeat_ = 0;
    std::int64_t nextLine_ = 0;     // the next 16th line to spawn
    double lastDt_ = 0;
    double cascade_ = 0;
    double creep_ = 0;   // floor: the puddles' creep time
    double swell_ = 0;   // floor: the puddles' swell (added to Wet)
    std::uint32_t seed32_ = 0;
    std::uint32_t nextId_ = 1;
    std::vector<Drop> drops_;
    std::vector<Impulse> impulses_;
    std::vector<Wipe> wipes_;
    std::vector<std::uint32_t> order_;              // sweep order for the merges
    std::vector<std::int64_t> lines_;
    // A coarse wetness grid of the pane (window): when each cell was last run over (sim seconds, -1 never); running
    // drops prefer wet paths.
    int wetCols_ = 1, wetRows_ = 1;
    std::vector<float> wet_;
    Tally tally_;
    Controls controls_;
};

}  // namespace reforce::engine::rain
