#pragma once
// reforce extension (PLAN 5L.14, docs/3d-design.md "Fractal zoom"): an endless zoom into a 3D fractal, seamless by
// self-similarity. Pure C++ (the GPU side is render/FractalPipeline).
//
// The fractals are attractors of contractions f_i (x -> c_i + s (x - c_i)): the Sierpinski tetrahedron (4 maps,
// s = 1/2, vertices (1,1,1), (-1,-1,1), (1,-1,-1), (-1,1,-1)) and the Menger sponge (20 maps, s = 1/3, the cube
// [-1, 1]^3). A zoom step is a similarity g = f_w o sigma (a word of maps after a rotation sigma of the hull), so
// g(S) is one of S's pieces:
//
//     g(x) = p + lambda Q (x - p)        p = g's fixed point, lambda = s^|w|, Q = a rotation by `angle` about `axis`
//
// What is drawn is not S but S_inf = union over n >= 0 of g^-n(S): S grown outwards about p for ever. Because g(S) is
// a piece of S, S_inf is exactly invariant: g(S_inf) = S_inf. So a camera pose P and the pose g o P see the same
// picture, and the camera can run along g^u (u = 0..1: lambda^u, a turn by u angle about the axis through p) for
// ever: at u = 1 it resets to u = 0 with nothing visible changing and no precision lost. Shading, fog and detail are
// all measured relative to the camera (the distance to p, the ray length), so they are invariant too.
//
// The distance to S_inf at x (the shader, FractalPipeline): pick the integer m with lambda^m |x - p| in
// (lambda R, R] (R = rho / 2), map y = g^m(x), and take lambda^-m min(DE_S(y), rho - |y - p|), where rho is the radius
// about p within which S_inf equals S (rho = lambda^-1 x the distance from p to S's other pieces at g's level). The
// same y and level serve x and g(x), so the estimate is exactly g-equivariant.
//
// A camera path (FractalPath) is g plus a base pose B (eye, aim, up, fov); the pose at zoom u is G(u) o W o B, W the
// scene's sway (an orbit of the eye about the path's axis through p, and a look-around). W may be any continuous
// function of time: the picture only depends on the pose up to g.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace reforce::engine {

enum class FractalKind { Sierpinski, Menger };
const char* fractalKindName(FractalKind k);
std::optional<FractalKind> parseFractalKind(std::string_view name);  // case-insensitive

using FVec3 = std::array<double, 3>;
using FMat3 = std::array<std::array<double, 3>, 3>;  // [row][col]

// A zoom step g(x) = p + lambda Rot(axis, angle) (x - p), and the region where S_inf = S.
struct FractalMap {
    FVec3 p{0, 0, 0};
    double lambda = 0.5;
    FVec3 axis{0, 0, 1};
    double angle = 0;  // radians, (-pi, pi]
    double rho = 1;    // S_inf equals S within rho of p
    double shell() const { return 0.5 * rho; }  // R: the shader maps every point into |y - p| in (lambda R, R]
};

// g^u for any real u: lambda^u, rotation by u angle about the axis through p.
FVec3 fractalApply(const FractalMap& g, double u, const FVec3& x);
FVec3 fractalRotate(const FractalMap& g, double u, const FVec3& v);  // the rotation part only (directions)

// The pieces: the hull's circumradius-free description for tests.
int fractalPieceCount(FractalKind k);           // 4 | 20
double fractalPieceScale(FractalKind k);        // 1/2 | 1/3
FVec3 fractalPieceOffset(FractalKind k, int i); // f_i(x) = s x + offset
// Signed distance to the solid hull (tetrahedron | cube); a lower bound outside.
double fractalHullDistance(FractalKind k, const FVec3& x);
// Distance estimate to S itself at a prefractal level (a fraction blends the two neighboring levels). The CPU twin
// of render/FractalPipeline's shader, for the tests.
double fractalDistanceS(FractalKind k, const FVec3& x, double level);
constexpr double kFractalPiece = 2.0;  // the size of the level-0 piece in the level formula
double fractalMaxLevel(FractalKind k);  // 22 | 14
// Distance estimate to S_inf (the shell mapping above) at the level that makes the smallest pieces `detailAngle`
// radians wide seen from `eye` (eyeScale = the camera's distance to p): the shader's deInf, in double.
double fractalDistanceInf(FractalKind k, const FractalMap& g, const FVec3& x, const FVec3& eye, double detailAngle,
                          double eyeScale);

// The step g = f_w o sigma for a word (piece indices, applied last first: f_w0 o f_w1 o ...) and a hull rotation
// sigma (a signed permutation matrix with det 1 that maps the hull to itself).
FractalMap fractalMapFor(FractalKind k, const int* word, int wordLength, const FMat3& sigma);

// A camera path: a zoom step and the base camera (at u = 0, no sway).
struct FractalPath {
    const char* name;
    FractalKind kind;
    const char* about;
    FractalMap g;
    FVec3 eye, aim, up;  // world (fractal) units; |eye - p| <= R
    double fov = 60;     // degrees, vertical
    double fog = 3;      // fog distance in units of the camera's distance to p
    FVec3 orbitAxis;     // the eye's orbit (Orbt) turns about this axis through p
    double orbit = 0;    // default orbit sway amplitude (radians) and turn per beat (radians, apex-like paths)
    double spin = 0;
    double look = 0;     // default look-around amplitude (radians)
};
int fractalPathCount();
const FractalPath& fractalPath(int i);  // i wraps
// The named path of a fractal ("" = its first). nullptr when unknown.
const FractalPath* findFractalPath(FractalKind k, std::string_view name);
std::string fractalPathNames(FractalKind k);  // "apex, dive, vortex" (logs)

// The camera at zoom u (only frac(u) is used by callers; any real works) with the scene's sway.
struct FractalSway {
    double orbit = 0;  // radians: the eye (and its aim and up) turned about the path's orbit axis through p
    double lookX = 0;  // radians: the view turned right (yaw) ...
    double lookY = 0;  // ... and up (pitch), about the eye
    double roll = 0;   // radians, about the view axis
    double fov = 0;    // degrees; 0 = the path's
};
struct FractalCamera {
    FVec3 eye{0, 0, 0}, right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, -1};
    double fov = 60;
    double scale = 1;  // |eye - p|: fog, detail and steps are measured in it
};
FractalCamera fractalCamera(const FractalPath& path, double u, const FractalSway& sway = {});

// ---- music (docs/3d-design.md "Fractal zoom"): where the zoom is, from the beat clock.
struct FractalTiming {
    double barsPerStep = 2;  // ZBar: bars per self-similar step (the loop); the steps land on these bar lines
    double pulse = 0.5;      // Puls 0..1: 0 steady; 1 all of each bar's motion surging right after its bar line
    double drop = 2;         // Drop: extra steps rushed through at a drop (SYNC_DROP), over DrpB beats
    double dropBeats = 4;    // DrpB
    double hover = 0.75;     // Hovr 0..1: how far a build (SYNC_BUILD) slows the zoom (1 = to a standstill)
    double beatStep = 0.6;   // Stpb 0..1: the orbit / sway moves in a step on each beat (0 = smoothly)
};
struct FractalBeat {
    double zoom = 0;   // the zoom position in steps, frac in [0, 1)
    double pulse = 0;  // 1 on a beat line, decaying over the beat (0 without a confident clock)
    double beats = 0;  // stepped beat count: whole beats, eased between (the sway's clock)
};
class FractalClock {
public:
    FractalClock() = default;
    explicit FractalClock(const FractalTiming& t) : t_(t) {}
    const FractalTiming& timing() const { return t_; }
    void setHover(double h) { t_.hover = std::isfinite(h) ? std::clamp(h, 0.0, 1.0) : 0.75; }
    // beat, bar: the clock (bar lines on whole numbers); clockOn: SYNC_ON; age: seconds since the start (no clock:
    // 120 BPM from the start); build, drop: SYNC_BUILD, SYNC_DROP; dt: seconds since the last step.
    FractalBeat step(double beat, double bar, bool clockOn, double age, double build, double drop, double dt);
    // The zoom from the clock alone (no build lag, no drop): integer on every ZBar-th bar line.
    static double baseZoom(const FractalTiming& t, double bar);

private:
    FractalTiming t_;
    double lag_ = 0;        // build: steps behind the base zoom (only its fraction matters)
    double off_ = 0;        // drops: steps ahead (finished bursts, mod 1)
    double dropFrom_ = 0;   // the running burst's start beat
    bool bursting_ = false;
    double dropPrev_ = 0;
    double prevBase_ = 0;
    bool started_ = false;
};

}  // namespace reforce::engine
