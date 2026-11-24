#pragma once
// reforce extension (docs/3d-design.md "Labyrinth"): the tilt maze behind the `labyrinth` solid. Pure C++; the
// player's buffers are made once (constructor), so frames on the render thread allocate nothing.
//
// A wooden tilt maze: a square board of Grid x Grid cells with raised walls, a steel ball and a goal hole. One run
// lasts `beats` beats (content: 64 = a 16-bar phrase) and ends with the ball dropping into the goal exactly on the
// run's last beat line; the next run (the next seed) starts there with a new maze.
//
//   - Maze (generate): a perfect maze (a spanning tree of the grid) by a recursive backtracker that prefers to keep
//     going straight (longer runs, fewer turns), from `seed`. The start is a dead end at one end of the tree's
//     longest path; the goal is the cell whose route from the start fills the run best (Timing, below), preferring
//     dead ends and long routes. Of a few candidate mazes per seed the best is kept. Trap holes go into dead ends off
//     the route (decoration: the ball never comes near them).
//   - Route: the solution path cut into straight segments. Each ends in one of three ways: against a wall (the
//     cell beyond is walled: the ball clacks into it), braked (the way goes on straight past the turn: the board
//     tilts back and the ball stops on the cell), or in the goal hole (the last).
//   - Timing (makeRoute): every segment starts on a whole beat (the board tilts) and ends on a whole beat (the clack,
//     the stop, or the drop); between them a segment may rest some beats (pause). Each segment gets at least the
//     beats its distance needs at the steepest planned tilt (through the board's response), then up to its "natural"
//     length (at a comfortable tilt); beats left over become pauses, spread evenly. The first two beats of a run are
//     the interlude (old walls sink, new ones rise, the ball pops up from the start).
//   - Physics (Player): a rolling ball on a tilted plane (a = 5/7 g sin(tilt) per axis, rolling resistance), in
//     beat time: all constants are per beat, so the motion is a pure function of the beat position and a tempo
//     change needs no re-plan (at 120 BPM a cell is about 6 cm). Fixed step 1/240 beat; collisions with the walls
//     (boxes) are found with sub-steps of at most kSubStep cells and a bisected time of impact, so the ball can never
//     pass through a wall; bounce with restitution kRestitution and Coulomb friction along the wall (kWallFriction),
//     so glancing hits slide and clacks tick to rest. The goal hole: the ball rolls over its rim, sinks against it and
//     drops. The board follows its command like a wrist: second order (kBoardOmega, kBoardDamping), rate-limited
//     (kTiltRate).
//   - Control: a human-like "player" tilts the board. Along a segment it commits to one tilt and plays a planned move
//     with it (tiltShape: into a wall, tilting back on a long roll, leaning in again before the clack; a braked stop,
//     leveling the board as the ball stops; the goal, easing to level), starting kLead before the beat. Across it is
//     loose: late (it sees the ball a moment ago), a dead band, imperfect gains, a tremor (seeded per segment), so the
//     ball wanders and grazes the side walls. The committed tilt is found by a root search (secant, then bisection)
//     so the segment's event (first contact with the end wall, the drop into the goal, a braked ball at rest on its
//     cell) lands on its beat line, within kTimingTolerance.
//   - Planning is lazy and cached: a segment is simulated (and its trajectory stored, one sample per step) when the
//     run first needs it; one at() plans at most two segments (a jump catches up over a few frames, Frame::catchingUp).
//     The plan depends only on (seed, grid, beats, traps), so any frame is a lookup and the whole thing is
//     deterministic.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace reforce::engine::labyrinth {

inline constexpr int kMinGrid = 4, kMaxGrid = 12, kMaxCells = kMaxGrid * kMaxGrid;
inline constexpr int kMinBeats = 8, kMaxBeats = 128;
inline constexpr int kMaxTraps = 6;
inline constexpr int kStepsPerBeat = 240;
inline constexpr int kMaxHits = 512;

// Geometry, board units (a cell is 1; walls sit on the cell borders).
inline constexpr double kWall = 0.16;        // wall thickness
inline constexpr double kWallHeight = 0.5;
inline constexpr double kBall = 0.28;        // ball radius
inline constexpr double kHole = 0.36;        // goal / trap hole radius
inline constexpr double kFree = 0.5 - kWall / 2 - kBall;  // how far the ball's center can leave its cell's center line
// Physics, beat time (cells, beats).
inline constexpr double kGravity = 40.0;                    // cells / beat^2
inline constexpr double kRollGain = 5.0 / 7.0 * kGravity;   // rolling sphere: a = 5/7 g sin(tilt)
inline constexpr double kMaxTilt = 0.21;                    // sin of the steepest tilt (about 12 degrees)
inline constexpr double kRestitution = 0.42;               // steel on wood, head-on
inline constexpr double kRestingSpeed = 0.12;              // slower into a wall: no bounce (resting contact)
inline constexpr double kWallFriction = 0.12;              // along the wall, x the normal impulse
inline constexpr double kRolling = 0.006;                   // rolling resistance (x g)
// The board follows its command like a wrist: second order (natural frequency, damping), rate-limited.
inline constexpr double kBoardOmega = 7.5;                  // rad / beat: time constant ~.13 beats
inline constexpr double kBoardDamping = 0.9;
inline constexpr double kTiltRate = 1.5;                    // sin of the tilt per beat at most
inline constexpr double kSubStep = 0.04;                    // cells per collision sub-step at most
inline constexpr double kMaxSpeed = 60.0;                   // cells / beat
inline constexpr double kTimingTolerance = 2e-4;            // beats
inline constexpr double kClackSpeed = 1.7;                  // into a wall, about (cells / beat)
inline constexpr double kLead = 0.15;                       // the player starts a tilt this early (beats)
inline constexpr double kRise = 0.1;                        // ... reaching it this long after the beat
inline constexpr double kLean = 0.35;                       // leaning into the wall at the clack (x the tilt)

enum Dir { East = 0, North = 1, West = 2, South = 3 };
enum class Stop : uint8_t { Wall, Brake, Goal };

struct Maze {
    int n = 0;                                  // cells per side
    std::array<uint8_t, kMaxCells> open{};      // per cell, bit d: open towards Dir d
    int start = 0, goal = 0;
    std::array<int16_t, kMaxCells> path{};      // cells from start to goal
    int pathLength = 0;
    std::array<int16_t, kMaxTraps> traps{};
    int trapCount = 0;
    bool wall(int cell, int d) const { return !((open[std::size_t(cell)] >> d) & 1); }
    int neighbor(int cell, int d) const;  // -1 off the board
    // Cell center in board units: x right, y up, the board centered on the origin.
    double cellX(int cell) const { return double(cell % n) - 0.5 * (n - 1); }
    double cellY(int cell) const { return 0.5 * (n - 1) - double(cell / n); }
    bool isPerfect() const;  // every cell reachable, exactly n^2 - 1 openings, symmetric
};

struct Segment {
    int from = 0, to = 0, dir = 0, cells = 0;
    Stop stop = Stop::Wall;
    int start = 0, end = 0, next = 0;  // beats in the run: tilt starts, event (clack / stop / drop), next segment
    double event = -1;                 // planned: when the event happened (beats), -1 until planned
    double param = 0;                  // the reference duration the secant search settled on
    double endError = 0;               // brake: distance from the cell center at `end` (cells)
};

struct Route {
    Maze maze;
    std::array<Segment, kMaxCells> segments{};
    int count = 0;
    int beats = 32, interlude = 2;
};

// The maze and its timed route for one run. `traps`: 0..kMaxTraps.
Route makeRoute(uint64_t seed, int grid, int beats, int traps);
// The player's tilt along a segment of D cells and d beats, in units of the tilt it commits to (which the search
// tunes per segment), at tau beats from the segment's beat (from -kLead: it starts ahead of the beat):
//   wall   tilt towards the wall; a long roll is braked (tilted back by brakeShare) mid-way, so the clack is not too
//          hard, then leaned into the wall again (kLean) just before it;
//   brake  tilt, then the same tilt back: the ball comes to rest on its cell;
//   goal   tilt, then easing to level: the ball rolls into the hole.
// The ramp in is smooth; the board's own inertia smooths the rest.
double tiltShape(Stop stop, double D, double d, double tau);
double brakeShare(Stop stop, double D, double d);
// A first guess of the tilt for a segment (the search starts from it; the durations are planned with it).
double tiltGuess(Stop stop, double D, double d);

struct Hit {
    double time = 0;   // beats into the run
    float speed = 0;   // normal speed into the wall (cells / beat)
    float nx = 0, ny = 0;  // the wall's normal (away from it)
};

// One frame of the labyrinth.
struct Frame {
    Maze maze;          // this run's
    Maze previous;      // the last run's (seed - 1): its walls sink during the interlude
    float rise = 1;     // this maze's walls, 0 (flat) .. 1 (full height)
    float sink = 0;     // the previous maze's walls, 1 .. 0
    float holes = 1;    // this maze's holes and start ring, 0 .. 1 (size)
    float oldHoles = 0; // the previous maze's holes
    float ballX = 0, ballY = 0, ballZ = 0;  // board units; z = 0 resting on the floor, < 0 sunk into a hole
    float tiltX = 0, tiltY = 0;  // sin of the board's slope: tiltX > 0 = its +x side lower (the ball rolls to +x)
    float flash = 0;    // 0..1, just after a wall hit
    int segment = -1;   // the route segment in progress (-1: interlude / before the first)
    bool catchingUp = false;  // the plan has not reached this beat yet (after a jump): board level, ball resting
    double turn = 0;    // beats into the run (clamped)
};

class Player {
public:
    explicit Player(int maxBeats = kMaxBeats);
    // The frame at `turn` beats into run `seed` (junk turn: clamped / 0). Replans only when an argument but turn
    // changes; plans lazily up to `turn`.
    const Frame& at(uint64_t seed, int grid, int beats, double turn, int traps = 3);
    // How many segments one at() may plan (default 2; <= 0: as many as it needs, as tests that jump may want).
    void setSegmentsPerFrame(int n) { segmentsPerFrame_ = n; }
    // Tests: plan the whole run of the last at() and inspect it.
    void planAll();
    const Route& route() const { return route_; }
    const Hit* hits() const { return hits_.data(); }
    int hitCount() const { return hitCount_; }
    double maxPenetration() const { return maxPen_; }  // deepest the ball ever got into a wall (cells)
    int illegalCrossings() const { return illegal_; }   // cell changes through a wall (must stay 0)
    // The player's looseness (planned segments so far): the ball's largest side drift from a corridor's center line
    // while rolling (cells), its mean, the samples off the route's cells (must stay 0), the board's largest tilt change
    // per beat.
    double maxWander() const { return maxWander_; }
    double meanWander() const { return wanderN_ > 0 ? wanderSum_ / wanderN_ : 0.0; }
    int offRoute() const { return offRoute_; }
    double maxTiltRate() const { return maxTiltRate_; }
    int plannedSegments() const { return planned_; }

    // The physics step, exposed for tests: a ball in `maze` with the board tilted (tx, ty) (sin of the slope).
    struct Ball {
        double x = 0, y = 0, vx = 0, vy = 0;
        double z = 0, vz = 0;     // height above resting (goal hole only)
        double tx = 0, ty = 0;    // the board's actual tilt
        double rx = 0, ry = 0;    // and its rate (per beat)
        bool falling = false;
    };
    static void step(const Maze& maze, Ball& b, double cx, double cy, double dt, double t, Hit* hit = nullptr,
                     double* penetration = nullptr);

private:
    void reset(uint64_t seed, int grid, int beats, int traps);
    void planSegment(int k);
    double simulate(int k, double param, bool record);
    void interludeFrame(double turn);

    int maxBeats_;
    int segmentsPerFrame_ = 2;
    uint64_t seed_ = 0;
    int grid_ = -1, beats_ = -1, traps_ = -1;
    bool have_ = false;
    Route route_;
    Maze previous_;
    uint64_t previousSeed_ = 0;
    bool havePrevious_ = false;
    int planned_ = 0;   // segments planned (their samples stored)
    Ball state_;        // at the start of segment planned_
    std::vector<float> samples_;  // 5 per step: x, y, z, tx, ty (from the first segment's start)
    std::array<Hit, kMaxHits> hits_{};
    int hitCount_ = 0;
    double maxPen_ = 0;
    int illegal_ = 0;
    double maxWander_ = 0, wanderSum_ = 0, maxTiltRate_ = 0;
    int wanderN_ = 0, offRoute_ = 0;
    std::array<uint8_t, kMaxCells> onRoute_{};
    Frame frame_;
};

}  // namespace reforce::engine::labyrinth
