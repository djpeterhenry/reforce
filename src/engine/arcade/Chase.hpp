#pragma once
// Chase (PLAN 5Q.8, docs/arcade-design.md "Chase"): the light-cycle game (CyclesSim, Cycles.hpp) seen in 3D from a
// camera chasing one cycle. Projected on the CPU: the grid is the floor, each wall a vertical ribbon along a trail,
// the cycles small line-art machines of our own; drawn far to near (painter's algorithm, one path per occluder), with
// fog towards the horizon. The camera is a director stepped on the beat clock: springy chase shots that swing wide
// around turns, cuts / glides to another shot on `Cut` lines (another cycle only after `Hold` bars on one, or a
// crash), a crane up over the arena on phrase lines (the walls
// bursting), a low fast angle with speed lines on a drop, an orbit round the explosion when its cycle crashes.
//
// World coordinates are cell units: X along the grid's columns, Z along its rows (row 0 nearest Z = 0), Y up; a cell
// center (gx, gy) of the game is (gx, 0, gy).
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/arcade/Cycles.hpp"

namespace reforce::engine {
class OrganicCanvas;
}

namespace reforce::engine::arcade {

struct V3 {
    double x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 lerp(V3 a, V3 b, double t) { return a + (b - a) * t; }

// A perspective camera: world points to tree units (y = +-1 at the frame's top / bottom for the vertical field of
// view), the roll applied on the screen.
struct ChaseCamera {
    V3 eye, right{1, 0, 0}, up{0, 1, 0}, fwd{0, 0, 1};
    double focal = 1.0 / std::tan(0.5 * 62.0 * 3.141592653589793 / 180.0);  // 1 / tan(fov / 2)
    double roll = 0;
    double nearZ = 0.05;

    static ChaseCamera lookAt(V3 eye, V3 target, double fovDeg, double roll);
    // World -> view (x right, y up, z depth along the view direction).
    V3 toView(V3 p) const { return {dot(p - eye, right), dot(p - eye, up), dot(p - eye, fwd)}; }
    // View (z >= nearZ) -> screen (tree units, roll applied).
    void toScreen(V3 v, double& sx, double& sy) const;
    // World -> screen; false (nothing written) behind the near plane.
    bool project(V3 p, double& sx, double& sy) const;
    // Clips a view-space segment to z >= nearZ: false if wholly behind; else the ends moved, ta / tb their
    // parameters along the original segment.
    bool clipNear(V3& a, V3& b, double& ta, double& tb) const;
    // Clips a view-space polygon (n <= 16) to z >= nearZ (Sutherland-Hodgman); returns the vertex count (<= n + 1).
    int clipNearPoly(const V3* in, int n, V3* out) const;
};

// Clips a screen segment to |x| <= hx, |y| <= hy (Liang-Barsky): false if outside; t0 / t1 the kept part.
bool clipRect2(double& x0, double& y0, double& x1, double& y1, double hx, double hy, double& t0, double& t1);
// Clips a screen polygon (n <= 24) to |x| <= hx, |y| <= hy; returns the vertex count (<= n + 4).
int clipRectPoly(const double* xs, const double* ys, int n, double hx, double hy, double* ox, double* oy);

struct ChaseConfig {
    CyclesConfig game;
    double cam = 4.2;    // camera distance behind the chased cycle, cells
    double camH = 1.5;   // camera height, cells
    double fov = 62;     // vertical field of view, degrees
    double walH = 0.8;   // wall height, cells
    double fog = 42;     // distance (cells) at which lines have faded to about a tenth
    double grid = 0.45;  // floor grid intensity
    double cut = 2;      // bars between camera changes (the shot)
    double hold = 8;     // bars at least on one cycle (unless it crashes); then it changes on a cut line
    double wdth = 2.4;   // line width (LWdt units) at the chase distance
    int shot = 0;        // 0: the director mixes shots; 1..5: always that shot (chase, low, side, ahead, high)
    int bike = 0;        // the machine's design: 0 arc, 1 blade, 2 orb
};

class ChaseView {
public:
    // The camera's shots.
    enum Shot { kChase = 0, kLow, kSide, kFront, kHigh, kShots };
    // Why the chased cycle last changed.
    enum Reason { kStart = 0, kCutLine, kCrashed };
    static constexpr int kFillBudget = 4200;  // fill strokes per frame (walls, cycles); further walls as outlines

    ChaseView(const ChaseConfig& cfg, std::uint64_t seed);

    // Steps the game and the camera to `beat` (bar: the bar position at that beat; drop / build: SYNC_DROP /
    // SYNC_BUILD; onset: BEAT) without drawing.
    void advance(double beat, double bar, double drop, double build, double onset);
    // advance() then draws the frame (pen: Pen x FADER).
    void step(double beat, double bar, double drop, double build, double onset, double pen, OrganicCanvas& c);

    const ChaseConfig& config() const { return cfg_; }
    const CyclesSim& sim() const { return sim_; }
    const ChaseCamera& camera() const { return cam_; }
    int target() const { return target_; }
    int shot() const { return shot_; }
    Reason reason() const { return reason_; }
    double crane() const { return crane_; }       // 0..1: the phrase-line crane shot's weight this frame
    double dropShot() const { return dropW_; }    // 0..1: the drop shot's weight
    bool orbiting() const { return orbiting_; }   // orbiting the chased cycle's explosion
    // Occluders of the last frame (walls, cycles) in drawing order: their distance from the eye.
    const std::vector<double>& drawOrder() const { return order_; }
    long fillStrokes() const { return fillUsed_; }
    long bikeSegments() const { return bikeSegs_; }  // the machines' segments last frame
    // The wheels' rolling angle (radians) at `beat`: the cells ridden (Spd per beat) over the radius (cells).
    static double wheelTurn(double beat, int spd, double radius);

private:
    struct Item {
        double depth;
        int kind, idx;
    };
    struct Chunk {
        V3 a, b;
        double h, penTop, penBot, penFill;
        double fade;  // 1, falling towards 0 right at the camera (see-through: outlines only)
        bool ghost, fill;
    };
    // stay: how much the cycle chased counts for (negative: a little away from it).
    void chooseTarget(std::uint64_t key, Reason why, double stay = -0.35);
    int pickShot(std::uint64_t key) const;
    void desired(double beat, V3& eye, V3& look, double& fov) const;
    void snapSprings(double beat);
    void compose(double beat, double bar, double onset);
    void headAt(int i, double beat, V3& p, double& angle) const;

    // Drawing (into the canvas, with cam_).
    void draw(double beat, double build, double pen, OrganicCanvas& c);
    void seg3(OrganicCanvas& c, V3 a, V3 b, double w, double pa, double pb);
    void line3(OrganicCanvas& c, V3 a, V3 b, double w, double pa, double pb);
    void dot3(OrganicCanvas& c, V3 p, double diam, double pen, double maxDiam = 0.05);
    int screenPoly(const V3* pts, int n, double* xs, double* ys) const;
    double fog(double d) const;
    double widthAt(double z) const;
    void drawCycle(OrganicCanvas& c, int i, double beat, double pen, bool fast);
    double bikeRear() const;  // cells from the head back to the rear wheel (where the wall starts)

    ChaseConfig cfg_;
    std::uint64_t seed_;
    CyclesSim sim_;
    ChaseCamera cam_;
    bool started_ = false;
    double last_ = 0, lastBar_ = 0;
    long cutIdx_ = 0;
    double targetBar_ = 0;  // the bar the chased cycle was picked on
    int target_ = 0, shot_ = kChase;
    double side_ = 1;  // which side of its cycle the shot keeps to (+1 right)
    Reason reason_ = kStart;
    bool snap_ = true;
    bool orbiting_ = false;
    V3 orbitAt_;
    double orbitT_ = 0, orbitA0_ = 0;
    V3 lastHead_;
    // Springs (beats as time).
    double yaw_ = 0, yawV_ = 0;
    V3 eye_, eyeV_, look_, lookV_;
    double fov_ = 62, fovV_ = 0;
    double crane_ = 0, dropW_ = 0, roll_ = 0, fogDist_ = 42;
    double hx_ = 1.78, hy_ = 1.0;  // clip rectangle (tree units)
    double ppu_ = 540;             // pixels per tree unit
    std::vector<Item> items_;
    std::vector<Chunk> chunks_;
    std::vector<double> order_;
    long fillUsed_ = 0, bikeSegs_ = 0, emitted_ = 0;  // emitted_: lines and dots drawn (counting)
};

}  // namespace reforce::engine::arcade
