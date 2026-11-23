#pragma once
// Arcade organic `hopper` (PLAN 5Q.2, docs/arcade-design.md): the game model, apart from the drawing so the tests can
// check the rules (landings on the beat lines, the board done exactly on the cycle line, balls never meeting the
// player). Pure state: no canvas, no audio.
//
// Cube units: a cube is 1 wide; the apex's top-face center is the origin, y up. Cube (row r, column c), c = 0..r, has
// its top-face center at (c - r / 2, -.75 r): a top diamond 1 x .5 and two side faces .5 tall below it, so the cube
// down-left of it (r + 1, c) and down-right (r + 1, c + 1) sit .5 left / right and .75 lower.

#include <cstdint>
#include <vector>

namespace reforce::engine::arcade::hopper {

constexpr int kMinRows = 3, kMaxRows = 10;  // 2 rows: the apex is the only even-row cube, and every cycle (an even
                                             // number of hops) would have to end on it while landing on it before

inline int cubeCount(int rows) { return rows * (rows + 1) / 2; }
inline int cubeIndex(int r, int c) { return r * (r + 1) / 2 + c; }
int rowOf(int cube);
int colOf(int cube);
// Diagonal neighbors (one row up or down, half a cube left or right).
bool adjacent(int a, int b);
// Top-face center in cube units.
double topX(int cube);
double topY(int cube);

// The smallest cycle (Cyc bars, doubled as needed, at most 64) whose 4 x bars hops can land on every cube levels - 1
// times (dark to bright): the pyramid's graph is two-colored by row parity, so each color needs enough landings of
// its own.
int cycleBarsFor(int rows, int cycBars, int levels = 2);

struct Ball {
    int t0 = 0;                  // the cycle beat it lands on the apex (+ .5 on the off-beat)
    int col[kMaxRows + 1] = {};  // its column on each row; col[rows] = the side it falls off
    double popAt = 1e300;        // the cycle beat it burst at (a drop)
    bool alive = true;           // false: no route around the player (after a replan)
};

struct Plan {
    long long cycle = 0;
    int hops = 32;            // landings per cycle (4 x the cycle's bars)
    std::vector<int> pos;     // landing k = 0..hops: the cube the player lands on at cycle beat k; -1 = on the disc
    // Every cycle starts all dark (state 0); a cube's first levels - 1 landings each step its top one state brighter.
    std::vector<int> stepAt;  // cube x (levels - 1): the landings that step it (hops + 1: never)
    std::vector<int> doneAt;  // per cube: the landing that makes it bright (the last of them is exactly `hops`)
    std::vector<Ball> balls;
    // A drop's disc ride: from cycle beat rideFrom (where the player was: rideX, rideY) out, up and round, landing at
    // rideTo on pos[rideTo] (the apex when a route from there is left, else back on the route).
    double rideFrom = -1;
    int rideTo = -1;
    double rideX = 0, rideY = 0;
    // A drop without a ride (no route left): the hop from landing spinHop to spinHop + 1 is a high spinning jump.
    int spinHop = -1;
    double dropAt = -1e300;  // cycle beat of the last drop
    bool searched = true;    // false: the route search ran out of budget (never seen in the tests)
};

struct Config {
    int rows = 7;
    int cycBars = 8;
    int levels = 2;
    double ballsPerBar = 1;
    bool offbeat = false;
    std::uint64_t seed = 0;
};

// The player's pose in cube units (x, y = its feet).
struct Pose {
    double x = 0, y = 0;
    double ground = 0;      // the height of the top face under it (its shadow), or y on the disc
    double air = 0;         // height above that
    int facing = 1;         // +1 right, -1 left
    bool back = false;      // hopping up (seen from behind)
    double squash = 1;      // height scale (landing / crouching < 1, stretched in the air > 1)
    double spin = 0;        // radians (a spin jump)
    bool onDisc = false;    // shadowless, riding
    double discX = 0, discY = 0, discScale = 0, discSpin = 0;
};

struct BallPose {
    bool visible = false;
    double x = 0, y = 0, squash = 1;
    double burst = -1;  // 0..1 while bursting (a drop), else -1
};

class Game {
public:
    explicit Game(const Config& c);
    // h: the hop clock in beats; landing k of cycle n is at h = n x hops + k. `drop`: a drop starts now.
    void advance(double h, bool drop);

    int rows() const { return rows_; }
    int cubes() const { return cubes_; }
    int hops() const { return hops_; }
    int cycleBars() const { return hops_ / 4; }
    int levels() const { return levels_; }
    const Plan& plan() const { return plan_; }
    double cycleBeat() const { return hc_; }
    // The last landing (0..hops - 1).
    int landing() const;
    // The top's state 0 (dark) .. levels - 1 (bright) after the last landing.
    int state(int cube) const;
    bool bright(int cube) const;
    // hop: the hop height in cube units (the drawing's `Hop` x breakdown lift).
    Pose pose(double hop) const;
    BallPose ballPose(const Ball& b, double hop) const;
    // Plans made so far (cycle starts and drop replans), for the tests.
    int plansMade() const { return plansMade_; }

    // A walk of landings pos[from + 1 .. hops] from pos[from], each a neighbor of the last, landing at least need[i]
    // (0..2) times on cube i, the last such landing exactly at `hops`. False when the search gives up.
    bool walk(std::vector<int>& pos, int from, const std::vector<int>& need, std::uint64_t rngSeed) const;

private:
    void newCycle(long long cycle);
    void onDrop(double hc);
    void computeFlips();
    bool completes(const std::vector<int>& pos) const;
    void placeBalls(std::uint64_t rngSeed, double after);
    bool placeBall(Ball& b, std::uint64_t rngSeed) const;
    std::uint64_t seedFor(long long cycle, std::uint64_t salt) const;

    int rows_, cubes_, hops_, levels_;
    double ballsPerBar_;
    bool offbeat_;
    std::uint64_t seed_;
    std::vector<unsigned char> dist_;  // cubes x cubes hop distances
    Plan plan_;
    bool havePlan_ = false;
    double hc_ = 0;
    int plansMade_ = 0;
};

}  // namespace reforce::engine::arcade::hopper
