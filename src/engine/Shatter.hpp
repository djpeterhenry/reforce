#pragma once
// reforce extension (docs/3d-design.md "Shatter", Peter 2026-11-26): a pane of glass in front of the visuals that
// cracks on the kicks and shatters on the phrase line. The CPU side: the fracture plan, the beat schedule, the shards'
// flight. The GPU side is render/ShatterPipeline (the crack map, the pane and the shards over the colorized frame).
//
// The fracture (engine/ShatterShape.inc, shared with the shaders): impacts, each a spider web of kinked radial cracks
// that fork outwards and concentric ring cracks between them; every shard is one sector of one band of the web,
// clipped to the impact's Voronoi cell (the bisector between two impacts is a crack too). So the shards tile the
// pane exactly, and the cracks are their boundaries, revealed piece by piece.
//
// The cycle (beats on the phrase-aligned clock, 4 x SYNC_BAR, or 120 BPM from the start without it), `Cycl` beats:
//   - the line: the pane shatters along all its cracks (and the hidden cuts between them); its shards fall (a gentle
//     collapse, Brst) or fly apart (a drop: towards the camera, spinning), leaving the clean visuals;
//   - 2 beats later a fresh pane slides down (or fades in, Arrv) over 1.5 beats;
//   - `Hit` beats after the line the first impact lands (others on later bar lines, Imp): its inner cracks shoot out
//     on that beat; on every later beat more cracks grow (outwards along the spokes, then the rings between them),
//     each starting exactly on its beat line and running out in about a tenth of a beat with a flash; by the line
//     `Crk` of the web shows;
//   - a build (SYNC_BUILD) makes the cracks due in the next 4 beats creep ahead on the 16th lines, and the cracked
//     pieces creak (their tilts tick);
//   - a drop (SYNC_DROP rising past .5) shatters the pane explosively on the next 16th line, whatever its state; a
//     fresh pane follows 3 beats later and cracks over the beats left until the next line (at least 12; else the one
//     after). A drop within 2 beats after a gentle shatter blows its shards apart.
// Deterministic: plans are pure functions of (seed, config, aspect, the pane's beats); the frame is a function of the
// clock's beat and those (the shards' flight is closed-form, not integrated), so it is the same at any frame rate. A
// clock that jumps (back, or more than 16 beats ahead) re-bases: the pane due at the new beat. No allocations after
// construction: the plans are cached (three).

#include <array>
#include <cstdint>
#include <memory>

namespace reforce::engine::shatter {

inline constexpr int kMaxImpacts = 6;
inline constexpr int kMaxBands = 12;
inline constexpr int kMaxSpokes = 12;
inline constexpr int kSlots = kMaxImpacts * kMaxBands * kMaxSpokes * 4;  // shard slots (ShatterShape.inc sh_slot)
inline constexpr int kElemIds = 2 * kSlots + 36;                         // crack element ids (+ the bisectors)
inline constexpr int kMaxShards = 640;   // shards in a pane at most (the plan thins the web to stay under)
inline constexpr int kMaxBoundary = 8;   // crack elements around one shard
inline constexpr double kArrive = 2.0;   // beats after the line the fresh pane starts to arrive
inline constexpr double kArriveBeats = 1.5;
inline constexpr double kFocal = 2.2;    // the camera's height above the pane (frame heights), as the jigsaw's

// The fracture as the shaders see it (layout of ShatterShape.inc's ShImpact / ShPane).
struct Impact {
    float cx = 0, cy = 0, rot = 0, r0 = 0.04f, lg = 0.5f, fork = 0;
    int32_t K = 9, nb = 8, f1 = 3, f2 = 5;
    uint32_t seed = 0;
    float pad = 0;
};
struct Pane {
    Impact imp[kMaxImpacts];
    int32_t count = 0;
    float aspect = 16.0f / 9.0f;
    uint32_t seed = 0;
    float pad = 0;
};
static_assert(sizeof(Impact) == 48 && sizeof(Pane) == 304, "Impact / Pane must match ShatterShape.inc");

struct Hit {
    int slot = 0;      // the shard (sh_slot)
    int elem = 0;      // the nearest crack element
    float param = 0;   // position along it, 0..1
    float dist = 0;    // distance to it (pane units)
    int impact = 0, band = 0;
};
// The shard at pane point (x, y) (ShatterShape.inc sh_owner, the same code the crack map runs).
Hit owner(const Pane& p, float x, float y);
inline int slotOf(int impact, int band, int spoke, int leaf) { return ((impact * 12 + band) * 12 + spoke) * 4 + leaf; }
inline int pairElem(int i, int i2) { return 2 * kSlots + (i < i2 ? i : i2) * 6 + (i < i2 ? i2 : i); }

// Load-time settings (scene keys).
struct Config {
    int impacts = 1;       // Imp: impacts per pane, 1..6 (the first on Hit, the others on later bar lines)
    double cycle = 64;     // Cycl: beats from one shatter to the next (a phrase line to the next), 16..256
    double hit = 4;        // Hit: the first impact, beats after the line (>= 4)
    double spokes = 9;     // Spk: radial cracks per impact (each impact varies it by up to 2), 5..12
    double rings = 0.5;    // Rng: ring cracks, 0 sparse (ratio 1.95) .. 1 dense (1.45)
    double fork = 0.6;     // Fork: how often a radial crack forks, 0..1
    double cracked = 0.8;  // Crk: the share of the web showing by the line, 0..1 (the rest are cuts the shatter shows)
    double size = 1.0;     // Size: the web's inner ring, x .04 frame heights (.025 with several impacts)
    int arrive = 0;        // Arrv: 0 the fresh pane slides down, 1 it fades in
};
Config clampConfig(Config c);

struct Element {
    float start = 1e30f;  // pane beat the crack starts to grow (>= 1e29: hidden until the shatter)
    float dur = 0.1f;     // beats to grow over its whole length
    int8_t dir = 1;       // +1 grows from param 0, -1 from 1
    bool shown() const { return start < 1e29f; }
};

struct Shard {
    int16_t slot = 0;
    int8_t impact = 0, band = 0;
    float cx = 0, cy = 0;               // centroid (pane units)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounds around the centroid (a margin included): the quad drawn
    float area = 0;
    float tiltX = 0, tiltY = 0;         // its hair of tilt once cracked loose (radians)
    std::array<float, 6> rnd{};
    std::array<int16_t, kMaxBoundary> bound{};
    int8_t nbound = 0;
};

struct Plan {
    uint64_t key = 0;
    uint64_t seed = 0;
    Config config;
    Pane pane;
    double aspect = 16.0 / 9.0;
    double length = 62;  // beats from the arrival (pane beat 0) to the shatter
    std::array<float, kMaxImpacts> land{};  // pane beat each impact lands on
    int shardCount = 0;
    std::array<Shard, kMaxShards> shards{};
    std::array<int16_t, kSlots> slotShard{};  // the shard of a slot, -1 none (off the pane)
    std::array<Element, kElemIds> elems{};
    int elemCount = 0;                        // elements on the pane
    std::array<int16_t, kElemIds> elemList{};
    int shown = 0;                            // of them planned to show before the shatter
    // (makePlan's scratch, so planning allocates nothing)
    std::array<float, kElemIds> scratchKey{};
    std::array<int16_t, kElemIds> scratchOrder{};
};
// `length`: the pane's beats from its arrival to the shatter; `first`: the pane beat of the first impact; `bar`: the
// pane beat of a bar line (impacts after the first land on bar lines).
void makePlan(Plan& out, uint64_t seed, const Config& c, double aspect, double length, double first, double bar);

// The beat clock (scene::SyncInput's SYNC_* values).
struct Clock {
    bool on = false;
    double bar = 0, build = 0, drop = 0;
};

// Per-frame controls (scene keys).
struct Controls {
    double burst = 0.3;    // Brst: the phrase-line shatter, 0 a collapse .. 1 an explosion (a drop is always 1)
    double gravity = 1.0;  // Grav
    double tilt = 1.0;     // Tilt: how much the cracked pieces tilt (the refraction offset across a crack)
};

// The look (render/ShatterPipeline), per frame.
struct Look {
    float depth = 0.35f;   // Dpth: the visuals' distance behind the pane, frame heights
    float spec = 1.0f;     // Spec: highlights
    float line = 1.0f;     // Line: the cracks' visibility
    float sparkle = 1.0f;  // Sprk: glints along the cracks
    float sheen = 1.0f;    // Gls: the intact glass's faint reflection
    float thick = 1.0f;    // Thck: the shards' thickness (their bevel and slab shift)
    float opacity = 1.0f;  // fade envelope
    bool opaque = false;   // ColorKey = 0: cracks and shards raise the tracked alpha
};

struct ShardPose {
    int16_t shard = 0;            // index into the plan's shards
    float x = 0, y = 0, z = 0;    // where its centroid is (pane units; z towards the camera)
    std::array<float, 9> rot{};   // row-major 3 x 3 (x right, y down, z towards the camera)
    float flash = 0;              // the shatter's flash on its edges
    float alpha = 1;
};

struct Frame {
    double beat = 0;                 // the clock's beat (phrase-aligned)
    // The pane (null: none): its plan, beat (0 = it starts arriving), slide (frame heights, -1 above .. 0 in place),
    // opacity (Arrv = 1 fades), sheen boost as it settles.
    const Plan* pane = nullptr;
    double paneBeat = 0;
    float offset = 0, paneAlpha = 1, sheen = 1;
    // Per crack element of the pane: how much shows (signed: + from param 0, - from 1; 0..1), the flash as it lands.
    std::array<float, kElemIds> reveal{};
    std::array<float, kElemIds> glow{};
    // Per slot: the shard's tilt (x, y radians), once cracked loose.
    std::array<float, 2 * kSlots> tilt{};
    std::array<float, kMaxImpacts> impactAge{};  // beats since each impact landed (< 0: not yet)
    float build = 0;
    int64_t line16 = 0;              // the 16th line (creaks tick on it)
    // The shards in flight (null plan: none), back to front.
    const Plan* fallen = nullptr;
    double fallBeat = 0;             // beats since the shatter
    bool big = false;                // an explosive shatter (a drop)
    int loose = 0;
    std::array<ShardPose, kMaxShards> poses{};
    bool empty() const { return !pane && loose == 0; }
};

class Player {
public:
    Player(uint64_t seed, const Config& c, double aspect, double startTime);
    // Advances to scene time t with the clock; returns this frame.
    const Frame& step(double t, const Clock& clock, const Controls& ctl);
    const Frame& frame() const { return frame_; }
    const Config& config() const { return config_; }
    double aspect() const { return aspect_; }
    // The pane (absolute beats: its arrival and its shatter), for the tests and the log.
    double paneStart() const { return cur_.p0; }
    double paneEnd() const { return cur_.p1; }
    int shatters() const { return shatters_; }
    int dropShatters() const { return dropShatters_; }
    int rebases() const { return rebases_; }
    int planBuilds() const { return planBuilds_; }

private:
    struct PaneState {
        bool valid = false;
        uint64_t seed = 0;
        double p0 = 0, p1 = 0;
    };
    struct FallState {
        bool valid = false;
        int plan = -1;
        double p0 = 0;            // the fallen pane's arrival (its cracks before the shatter)
        double s = 0;             // the shatter's beat
        bool big = false;
        double upgrade = 1e300;   // a drop just after a gentle shatter: blown apart from here
    };
    void rebase(double b);
    PaneState regular(int64_t k) const;
    PaneState partial(double from, uint64_t salt) const;
    int plan(const PaneState& p);
    void shatter(double s, bool big);
    void paneFrame(const Plan& pl, double rel, double b, const Clock& clk, const Controls& ctl, bool fresh);
    void fallFrame(double b, const Controls& ctl);

    uint64_t seed_;
    Config config_;
    double aspect_;
    double start_;
    std::unique_ptr<std::array<Plan, 3>> plans_;
    std::array<uint64_t, 3> planUse_{};
    uint64_t uses_ = 0;
    PaneState cur_;
    int curPlan_ = -1;
    FallState fall_;
    bool first_ = true;
    double lastBeat_ = 0;
    double prevDrop_ = 0;
    uint64_t creepKey_ = 0;
    std::unique_ptr<std::array<float, kElemIds>> creep_;
    int shatters_ = 0, dropShatters_ = 0, rebases_ = 0, planBuilds_ = 0;
    uint64_t lastPaneKey_ = 0;
    std::array<int16_t, kMaxShards> sortIdx_{};
    std::array<ShardPose, kMaxShards> sortTmp_{};
    Frame frame_;
};

}  // namespace reforce::engine::shatter
