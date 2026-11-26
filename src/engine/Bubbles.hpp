#pragma once
// reforce extension (docs/3d-design.md "Bubbles", Peter 2026-11-26): the CPU side of the bubbles object
// (Mesh="bubbles"). The GPU side (the thin-film optics) is render/BubblePipeline.
//
// Soap bubbles drifting over the visuals. Each bubble is a thin spherical shell: it drifts on smooth air currents
// (the curl of seeded value noise, so the flow has no sources or sinks) and rises gently, wobbles in its low-order shape
// modes (n = 2 and 3, damped oscillators kicked by the air, by contacts and by a build's tremble), bounces softly off
// the others, or sticks to one in a cluster with a flat shared wall (two touching bubbles settle where their films meet
// at 120 degrees: centers sqrt(r1^2 + r2^2 - r1 r2) apart). New bubbles are blown in from a seeded source (Src: a wand,
// the bottom edge, or anywhere in the air), inflating as they come.
//
// Pops land exactly on beat lines and are planned on the beat clock a beat ahead: on beat line k the victims for line
// k + 4 are picked (the oldest first, seeded), their film drains towards black from the top over that beat ("doom"),
// and on line k + 4 the film tears from one point; the hole's rim runs round the shell in kPopSeconds and throws a
// faint spray of droplets. A drop (SYNC_DROP rising past .5) pops many at once on the next beat line and blows a
// burst in; a build (SYNC_BUILD) blows more in and makes them tremble; early in a breakdown they turn slow and big.
//
// Units: frame heights. x runs 0 .. aspect (left to right), y 0 .. 1 (top to bottom). Radii are the bubbles' rest
// radii; a bubble being blown in is drawn at r x its inflation.
//
// Time: fixed substeps (kSubstep = 1/120 s) from the start, so the state depends on the beat clock and the seed, not
// on the frame rate. A step that jumps more than kMaxCatchUp substeps simulates only the last ones, and at most
// kMaxLines 16th lines are visited for it; a clock jumping back re-bases. Junk inputs (NaN, inf, 1e300) are clamped
// or treated as no clock.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace reforce::engine::bubbles {

enum class Source { Wand, Edge, Air };
std::optional<Source> parseSource(std::string_view s);  // "wand" | "edge" | "air" (case-insensitive; "" = wand)
const char* sourceName(Source s);

inline constexpr double kSubstep = 1.0 / 120.0;
inline constexpr int kMaxCatchUp = 30;      // substeps simulated per step at most (a quarter second)
inline constexpr int kMaxLines = 16;        // 16th lines visited per step at most
inline constexpr int kMaxBubbles = 96;      // alive at most, whatever Max says
inline constexpr int kMaxDroplets = 640;    // spray droplets alive at most
inline constexpr int kMaxPerLine = 24;      // bubbles blown in on one 16th line at most
inline constexpr int kMaxBonds = 3;         // cluster walls per bubble
inline constexpr double kPopSeconds = 0.045;  // the hole's rim runs round the shell in this long (~3 frames)
inline constexpr double kInflateSeconds = 0.3;

// Load-time settings.
struct Settings {
    Source source = Source::Wand;
    int maxBubbles = 40;   // Max: bubbles alive at most (the extras are planned to pop on the next beat lines)
    double cluster = 0.5;  // Clst: the chance that two bubbles meeting gently stick together (0..1)
    std::uint64_t seed = 0;
};

// Per-frame controls.
struct Controls {
    double blow = 6.0;   // Blow: bubbles blown in per bar
    double size = 55.0;  // Size: mean radius, px at 1080p
    double pop = 1.0;    // Pop: bubbles popped per beat line (expected; planned a beat ahead)
    double life = 32.0;  // Life: beats a bubble lives at most before its pop is planned
    double air = 1.0;    // Air: how strongly the air currents carry them
    double rise = 1.0;   // Rise: how fast they rise
    double wind = 0.0;   // Wind: a breeze whose heading and strength wander slowly (0 none .. 4)
};

// The look (render/BubblePipeline), per frame.
struct Look {
    float film = 520.0f;  // Film: mean film thickness, nm (the interference colors' order)
    float iris = 1.0f;    // Iris: strength of the interference colors (0 gray reflections .. 3)
    float swirl = 1.0f;   // Swrl: how much the film swirls (0 smooth bands .. 3)
    float drain = 1.0f;   // Drn: how fast the bands flow down and the top thins (0 still .. 4)
    float bend = 1.0f;    // Bend: how much the visuals shift through the shell (strongest at the rim; 0 none .. 4)
    float spec = 1.0f;    // Spec: the window highlight
    float rim = 1.0f;     // Rim: the Fresnel reflection at the edge (0 physical .. 3)
    float depth = 0.3f;   // Dpth: the visuals' distance behind the bubbles, frame heights (what the rim reflects)
    float tint = 0.2f;    // Tint: how much the ColorMap colors the light the bubbles reflect (0..1)
    float tintColor[3] = {1.0f, 1.0f, 1.0f};  // that color (sRGB 0..1; the Visualizer's ColorMap at .8)
    float opacity = 1.0f; // fade envelope
    bool opaque = false;  // ColorKey = 0: the bubbles raise the tracked alpha
};

// The beat clock (scene::SyncInput's SYNC_* values).
struct Clock {
    bool on = false;
    double beat = 0, bar = 0, phrase = 0, build = 0, drop = 0;
};

struct Bubble {
    double x = 0, y = 0;    // center (frame heights)
    double vx = 0, vy = 0;
    double r = 0;           // rest radius (frame heights)
    double grow = 1;        // inflation 0..1 (drawn at r x an ease of it)
    double age = 0;         // seconds since it was blown in
    std::array<double, 4> mode{};  // shape modes: n = 2 (cos, sin), n = 3 (cos, sin), relative to r
    std::array<double, 4> modeV{};
    double pop = -1;        // seconds since the film tore (< 0: intact)
    double tear[3] = {0, 0, 1};  // where it tore (unit, x right, y down, z towards the viewer)
    double film = 0;        // a seeded offset into the film's swirl
    double doom = 0;        // 0..1: how far its planned pop is (the film drains black from the top)
    std::int64_t bornLine = 0;  // the 16th line it was blown in on
    std::int64_t popLine = 0;   // planned: the beat line it pops on (0 = none)
    std::int64_t planLine = 0;  // the line its pop was planned on
    std::array<std::uint32_t, kMaxBonds> bond{};  // ids of the bubbles it shares a wall with (0 = none)
    std::uint32_t id = 0;
    int bonds() const {
        int n = 0;
        for (auto b : bond) n += b != 0;
        return n;
    }
};

struct Droplet {
    double x = 0, y = 0, vx = 0, vy = 0;
    double r = 0;     // radius (frame heights)
    double age = 0, life = 0.5;
};

// Totals for the tests.
struct Tally {
    std::int64_t blown = 0, popped = 0, exited = 0, lines = 0, bonds = 0, unbonds = 0, dropPops = 0, bursts = 0;
    std::int64_t offBeatPops = 0;  // pops that started anywhere but on a beat line (never, the tests check)
};

// A pop, as it started (the tests check they land on beat lines).
struct PopEvent {
    std::int64_t line = 0;  // the 16th line (a beat line: line % 4 == 0)
    double beat = 0;        // the clock's beat at the substep the tear started
    std::uint32_t id = 0;
    bool planned = true;    // false: a drop's (or the count bound's) pop without warning
};

class Sim {
public:
    // aspect = width / height of the frame; startTime = the object's start (seconds).
    Sim(const Settings& s, double aspect, double startTime);

    // Advances to scene time t.
    void step(double t, const Controls& c, const Clock& clock);

    // Puts a bubble (fully inflated, at rest) at (x, y) with radius r now (tests). Returns its id (0: refused).
    std::uint32_t place(double x, double y, double r);
    // Plans the bubble `id` to pop on the next beat line (tests).
    void planPop(std::uint32_t id);

    const Settings& settings() const { return settings_; }
    double aspect() const { return aspect_; }
    const std::vector<Bubble>& bubbles() const { return bubbles_; }
    const std::vector<Droplet>& droplets() const { return droplets_; }
    const Tally& tally() const { return tally_; }
    // The last pops (at most 256 kept) and the 16th lines visited (the last 256).
    const std::vector<PopEvent>& pops() const { return pops_; }
    const std::vector<std::int64_t>& lines() const { return lines_; }
    double simTime() const { return double(done_) * kSubstep; }
    double lastDt() const { return lastDt_; }
    // Tremble (the build) and calm (a breakdown's start), 0..1, as of the last step.
    double tremble() const { return tremble_; }
    double calm() const { return calm_; }
    // The air's velocity at (x, y) and sim time t (frame heights / s; without Air / Rise scaling): the curl of a
    // seeded noise potential, so it neither converges nor diverges.
    static void airAt(double x, double y, double t, std::uint32_t seed, double& vx, double& vy);
    // The bubble's drawn radius now (r x its inflation).
    static double drawnRadius(const Bubble& b);
    // The wand (Src wand): where bubbles are blown from and the aim (radians, 0 = +x, y down), for the line k.
    void wand(std::int64_t k, double& x, double& y, double& aim) const;

private:
    void visitLine(std::int64_t k, double beatAt, const Clock& clock, const Controls& c, double frac);
    void blow(std::int64_t k, int n, bool burst, const Controls& c, std::uint64_t& h);
    void startPop(Bubble& b, std::int64_t line, double beat, bool planned, std::uint64_t h);
    void substep(const Controls& c);
    void unbond(Bubble& a, std::uint32_t other);
    Bubble* find(std::uint32_t id);
    double rand01(std::uint64_t& h) const;

    Settings settings_;
    double aspect_ = 16.0 / 9.0;
    double start_ = 0;
    std::int64_t done_ = 0;
    bool first_ = true;
    double lastBeat_ = 0;
    std::int64_t nextLine_ = 0;
    double lastDt_ = 0;
    double lastDrop_ = 0;     // the clock's drop at the last beat line (a drop is a rise past .5)
    bool dropArmed_ = false;  // a drop seen since the last beat line, not yet popped
    double tremble_ = 0, calm_ = 0;
    int queue_ = 0;           // bubbles still to blow in (a bar's puff, released over its 16ths)
    std::uint32_t seed32_ = 0;
    std::uint32_t nextId_ = 1;
    std::vector<Bubble> bubbles_;
    std::vector<Droplet> droplets_;
    std::vector<PopEvent> pops_;
    std::vector<std::int64_t> lines_;
    std::vector<std::uint32_t> pick_;  // scratch: candidates for the pops
    Tally tally_;
    Controls controls_;
    double curBeat_ = 0;      // the beat at the substep being simulated (pop events)
};

}  // namespace reforce::engine::bubbles
