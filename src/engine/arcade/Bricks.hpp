#pragma once
// Arcade organic `bricks` (PLAN 5Q.6, docs/arcade-design.md): the game itself, apart from its OrganicBody so the
// tests can drive it on a beat clock and look at the plan (every bounce time, every brick's break time).
//
// The ball flies at a constant speed; its flight through a cycle (Cyc bars) is planned volley by volley a few beats
// ahead: every paddle and brick hit on a tick line (Rate per beat), the paths' angles (and side-wall bounces) chosen
// so their lengths fit, how many bricks each hit breaks chosen so the last ones break exactly on the cycle line.
// Drawing is a pure function of the plan and the time. A drop (multiball) or a breakdown (finer ticks) re-plans from
// the next bounce on.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/OrganicKinds.hpp"

namespace reforce::engine::arcade::bricks {

// Load-time keys (docs/arcade-design.md), clamped.
struct Params {
    int cols = 10, rows = 3;  // the wall
    int cycBars = 16;         // a wall is cleared every Cyc bars, on the line
    double rate = 1;          // ticks (bounce lines) per beat
    int buildMul = 2;         // ticks x this through a breakdown (SYNC_BUILD)
    double speed = 1.5;       // ball speed, tree units per beat (constant; within 3 % to land on a brick)
    double ball = 0.06;       // ball diameter
    double paddle = 0.46;     // paddle width
    double gap = 0.018;       // between bricks
    double brickH = 0.075;    // brick height (smaller when the rows would not fit)
    double halfW = 1.6, halfH = 0.92;  // the playfield (walls at +-halfW, ceiling at halfH)
    int multi = 2;            // extra balls on a drop (SYNC_DROP)
    int frags = 6;            // pieces per broken brick
    double fragLife = 2;      // beats a piece flies
    int layout = -1;          // wall pattern 0..7; -1 = a seeded one per cycle
    double wallPen = 0.35;    // the walls' intensity (relative to Pen)
};
Params paramsFrom(const scene::OrganicInstance& in);

enum class Hit : std::uint8_t { Paddle, Shield, Brick, Wall, Ceiling };

// One straight flight of a ball, from (x0, y0) at beat t0 to the bounce (x1, y1) at t1. A leg (paddle to target, or
// back down) is one segment, or more when it goes off the side walls; `legEnd` marks a leg's last segment (a paddle or
// brick hit, on a tick line).
struct Seg {
    double t0, t1, x0, y0, x1, y1;
    int ball;
    Hit end;
    bool legEnd;
    int bricks;  // bricks this bounce breaks (Hit::Brick)
    int brick;   // the brick it hits (Hit::Brick), -1 otherwise
};

class Game {
public:
    Game(const Params& p, std::uint64_t seed);

    // Moves to `bar` (the phrase-aligned bar count; beats = 4 x bars) with SYNC_DROP / SYNC_BUILD (0..1).
    void update(double bar, double drop, double build);
    // The board at the current time, through the canvas (tree units), intensities x `pen`.
    void draw(OrganicCanvas& c, double pen) const;

    // ---- inspection (tests, summary)
    const Params& params() const { return p_; }
    double now() const { return u_; }  // beats (4 x bar)
    double cycleStart() const { return ts_; }
    double cycleEnd() const { return te_; }
    double tick() const { return 1.0 / (p_.rate * (fast_ ? p_.buildMul : 1)); }  // beats between bounce lines
    bool fast() const { return fast_; }
    const std::vector<Seg>& segs() const { return segs_; }
    int balls() const { return balls_; }
    int plans() const { return plans_; }  // plans started so far (each cycle, each re-plan)
    // Volleys planned at each relaxation level (0 = within 3 % of Spd on clear paths .. 5 = any speed; 6 = nothing
    // fitted in time).
    int relaxed(int level) const { return level >= 0 && level < 7 ? relaxed_[level] : 0; }
    // Bricks broken on the line without a hit (the last resort when ball 0 found nothing to aim at; never expected).
    int orphans() const { return orphans_; }
    int brickCount() const { return int(bricks_.size()); }
    struct Brick {
        double x, y, w, h;  // centre and size (wall in place)
        float pen;
        double broken;  // beat the brick breaks (planned)
    };
    const std::vector<Brick>& bricks() const { return bricks_; }
    double paddleX(double u) const;  // paddle centre
    double ballContactY() const { return padContact_; }  // ball centre when it touches the paddle
    double ballRadius() const { return 0.5 * p_.ball; }
    // Ball `i`'s centre at beat `u` (false while it is not in play then).
    bool ballAt(int i, double u, double& x, double& y) const;
    std::string summary() const;

    // How far the planner may bend its rules to fit a leg (tests: the first level is the normal one).
    struct Relax {
        double tol, flat;
        bool clear;
    };
    static double speedOf(const Seg& s);

private:
    static constexpr int kMaxPts = 24;
    struct BallState {
        double t, x, y;
        bool atPaddle, done;
        double v;  // speed (units per beat), set at the paddle
    };
    struct UpPick {
        bool ok = false;
        int target = -1;
        double X = 0, ty = 0, v = 0;
        int n = 0;
        double score = -1e300;
    };
    struct Frag {
        double t, x, y, w, h;
        float pen;
        std::uint32_t id;
    };
    void newCycle(long k, bool carry, double build);
    void replan(double u);
    void plan(double until);
    void decideUp(int i);
    void decideVolley(int i);
    void commitUp(int i, double px, double tp, const UpPick& up, int n, bool final, int R);
    void bestUp(double px, double tp, int nMin, int nMax, const Relax& rx, const std::vector<int>& low, bool wantFree,
                bool final, std::vector<UpPick>& out, int want);
    struct Volley {
        double score, X;  // X: the paddle point, unfolded
        int d;            // ticks down
        UpPick up;
        bool last = false;  // an extra ball's last volley: down into the shield, which takes it (no up leg)
        bool final = false;  // ends on the line
    };
    void volleys(int i, const Relax& rx, int n, std::vector<Volley>& out, int perDown, bool forceFinal = false);
    void commitVolley(int i, const Volley& vl, int n);
    bool fitsAhead(int i, int depth);
    bool downOnly(int i, int dTop);
    bool sky(int i, const Relax& rx, int nTop);
    struct Snapshot {
        std::vector<double> broken;
        std::vector<int> claim;
        std::size_t segs = 0, keys = 0;
        std::vector<BallState> st;
        double legBeats = 0;
        int legs = 0;
        std::uint64_t rng = 0;
    };
    Snapshot snapshot() const;
    void restore(const Snapshot& sn);
    int share(int i, bool final);
    int claim(int i, int target, double t1, int n, bool final);
    int legMin() const;
    double fastVolley() const;
    void paddleKey(double t, double px, double nx, double ny);
    double fold(double x) const;
    int trace(double ax, double ay, double X, double by, double* xs, double* ys, int maxPts) const;
    bool pathClear(const double* xs, const double* ys, int n, const std::vector<int>& low) const;
    void pushPath(int ball, double t0, double t1, const double* xs, const double* ys, int n, Hit end, int bricks,
                  int brick);
    void layoutWall(long k);
    double snapUp(double t) const;  // the first tick line at or after t
    void lowest(double t, std::vector<int>& low) const;
    double fromFinale(int b) const;  // distance from the finale brick (field widths)
    bool clear(double ax, double ay, double bx, double by, const std::vector<int>& low) const;
    double rnd();
    void drawFrags(OrganicCanvas& c, double pen, double x, double y, double w, double h, float bpen, double tb,
                   std::uint32_t id) const;

    Params p_;
    std::uint64_t seed_;
    std::uint64_t rng_ = 0;
    double u_ = 0;
    bool started_ = false;
    long cyc_ = 0;
    double ts_ = 0, te_ = 0;
    bool fast_ = false;
    bool dropArmed_ = true;
    double multiUntil_ = -1e300;
    int balls_ = 1;
    int plans_ = 0;
    int relaxed_[7] = {0, 0, 0, 0, 0, 0, 0};
    int orphans_ = 0;
    double typical_ = 1;   // a typical up leg's height
    double legBeats_ = 0;  // legs planned this cycle and their beats
    double pace_ = 0;      // beats per volley wanted (share(); 0 = any)
    int legs_ = 0;
    // Geometry.
    double bw_ = 0, bh_ = 0, top_ = 0, padY_ = 0, padH_ = 0, padContact_ = 0;
    std::vector<Brick> bricks_;
    std::vector<int> col_, row_;     // per brick
    std::vector<int> claimBall_;     // per brick: the ball whose hit breaks it
    int finale_ = 0;                 // the brick the last ones gather around
    std::vector<Seg> segs_;
    std::vector<BallState> st_;
    std::vector<std::pair<double, double>> padKeys_;  // (beat, paddle centre) at ball 0's paddle hits
    std::vector<Frag> prevFrags_;                      // pieces of earlier walls still flying
};

}  // namespace reforce::engine::arcade::bricks
