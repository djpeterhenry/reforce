// reforce extension (PLAN 5L.14, docs/3d-design.md "Fractal zoom"): the self-similar zoom. See FractalZoom.hpp.
#include "engine/FractalZoom.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace reforce::engine {
namespace {

constexpr double kPi = 3.14159265358979323846;

double fin(double v, double fb) { return std::isfinite(v) ? v : fb; }

FVec3 add(const FVec3& a, const FVec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
FVec3 sub(const FVec3& a, const FVec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
FVec3 mul(const FVec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const FVec3& a, const FVec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
FVec3 cross(const FVec3& a, const FVec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double len(const FVec3& a) { return std::sqrt(dot(a, a)); }
FVec3 norm(const FVec3& a, const FVec3& fb) {
    const double l = len(a);
    return l > 1e-300 && std::isfinite(l) ? mul(a, 1.0 / l) : fb;
}
// Rodrigues: v turned by `a` radians about the unit axis k.
FVec3 rotate(const FVec3& v, const FVec3& k, double a) {
    const double c = std::cos(a), s = std::sin(a);
    return add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1.0 - c)));
}
FVec3 matvec(const FMat3& m, const FVec3& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}
FMat3 matmul(const FMat3& a, const FMat3& b) {
    FMat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[std::size_t(i)][std::size_t(j)] += a[std::size_t(i)][std::size_t(k)] * b[std::size_t(k)][std::size_t(j)];
    return r;
}

constexpr FVec3 kTetra[4] = {{1, 1, 1}, {-1, -1, 1}, {1, -1, -1}, {-1, 1, -1}};

// The Menger sponge's 20 sub-cube centers, in the order (i, j, k) over (-1, 0, 1) with at most one 0.
const std::vector<FVec3>& mengerCenters() {
    static const std::vector<FVec3> c = [] {
        std::vector<FVec3> v;
        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j)
                for (int k = -1; k <= 1; ++k)
                    if ((i == 0) + (j == 0) + (k == 0) <= 1) v.push_back({i * 2.0 / 3.0, j * 2.0 / 3.0, k * 2.0 / 3.0});
        return v;
    }();
    return c;
}

double tetraSdf(const FVec3& z) {
    const double x = z[0], y = z[1], w = z[2];
    const double m = std::max(std::max(-x - y - w, x + y - w), std::max(x - y + w, -x + y + w));
    return (m - 1.0) / std::sqrt(3.0);
}

double boxSdf(const FVec3& p) {
    const FVec3 q{std::fabs(p[0]) - 1.0, std::fabs(p[1]) - 1.0, std::fabs(p[2]) - 1.0};
    const FVec3 o{std::max(q[0], 0.0), std::max(q[1], 0.0), std::max(q[2], 0.0)};
    return len(o) + std::min(std::max(q[0], std::max(q[1], q[2])), 0.0);
}

}  // namespace

const char* fractalKindName(FractalKind k) {
    switch (k) {
        case FractalKind::Sierpinski: return "sierpinski";
        case FractalKind::Menger: return "menger";
    }
    return "?";
}

std::optional<FractalKind> parseFractalKind(std::string_view name) {
    std::string s;
    for (char c : name)
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (s.empty() || s == "sierpinski" || s == "tetrahedron") return FractalKind::Sierpinski;
    if (s == "menger" || s == "sponge") return FractalKind::Menger;
    return std::nullopt;
}

FVec3 fractalRotate(const FractalMap& g, double u, const FVec3& v) {
    return g.angle != 0 ? rotate(v, g.axis, u * g.angle) : v;
}

FVec3 fractalApply(const FractalMap& g, double u, const FVec3& x) {
    return add(g.p, mul(fractalRotate(g, u, sub(x, g.p)), std::pow(g.lambda, u)));
}

int fractalPieceCount(FractalKind k) { return k == FractalKind::Sierpinski ? 4 : 20; }
double fractalPieceScale(FractalKind k) { return k == FractalKind::Sierpinski ? 0.5 : 1.0 / 3.0; }
FVec3 fractalPieceOffset(FractalKind k, int i) {
    if (k == FractalKind::Sierpinski) return mul(kTetra[((i % 4) + 4) % 4], 0.5);  // v + (x - v) / 2
    const auto& c = mengerCenters();
    return c[std::size_t(((i % 20) + 20) % 20)];  // c + x / 3
}

double fractalHullDistance(FractalKind k, const FVec3& x) {
    return k == FractalKind::Sierpinski ? tetraSdf(x) : boxSdf(x);
}

namespace {
double sierpinskiAt(FVec3 z, int n, double* next) {
    double sc = 1.0, d0 = 0.0;
    for (int i = 0; i <= n; ++i) {
        if (i == n) d0 = tetraSdf(z) * sc;
        if (z[0] + z[1] < 0) { const double t = -z[1]; z[1] = -z[0]; z[0] = t; }
        if (z[0] + z[2] < 0) { const double t = -z[2]; z[2] = -z[0]; z[0] = t; }
        if (z[1] + z[2] < 0) { const double t = -z[2]; z[2] = -z[1]; z[1] = t; }
        z = sub(mul(z, 2.0), {1, 1, 1});
        sc *= 0.5;
    }
    *next = tetraSdf(z) * sc;
    return d0;
}
// The Menger sponge in local cell coordinates (w = the point's place in its cell at each level, -1..1), as the shader.
double mengerAt(const FVec3& p, int n, double* next) {
    double d = boxSdf(p), sc = 1.0, d0 = d;
    FVec3 w;
    for (int j = 0; j < 3; ++j) w[std::size_t(j)] = p[std::size_t(j)] - 2.0 * std::floor(p[std::size_t(j)] * 0.5) - 1.0;
    for (int i = 0; i <= n; ++i) {
        if (i == n) d0 = d;
        sc *= 3.0;
        FVec3 r;
        for (int j = 0; j < 3; ++j) r[std::size_t(j)] = std::fabs(1.0 - 3.0 * std::fabs(w[std::size_t(j)]));
        const double da = std::max(r[0], r[1]), db = std::max(r[1], r[2]), dc = std::max(r[2], r[0]);
        d = std::max(d, (std::min(da, std::min(db, dc)) - 1.0) / sc);
        for (int j = 0; j < 3; ++j) {
            const double v = 3.0 * w[std::size_t(j)] + 1.0;
            w[std::size_t(j)] = v - 2.0 * std::floor(v * 0.5) - 1.0;
        }
    }
    *next = d;
    return d0;
}
}  // namespace

double fractalDistanceS(FractalKind k, const FVec3& x, double level) {
    level = std::clamp(fin(level, 0.0), 0.0, 40.0);
    const int n = int(level);
    const double f = level - n;
    double next = 0;
    const double d0 = k == FractalKind::Sierpinski ? sierpinskiAt(x, n, &next) : mengerAt(x, n, &next);
    return d0 + (next - d0) * f;
}

// Deep enough for any view at 4K (the levels a point needs grow by the step's levels across its shell), shallow
// enough for float: the Menger cell coordinates lose a factor 3 of precision per level, the Sierpinski folds 2.
double fractalMaxLevel(FractalKind k) { return k == FractalKind::Sierpinski ? 22.0 : 12.0; }

double fractalDistanceInf(FractalKind k, const FractalMap& g, const FVec3& x, const FVec3& eye, double detailAngle,
                          double eyeScale) {
    const FVec3 v = sub(x, g.p);
    const double r = len(v);
    const double m = std::clamp(std::ceil(std::log(std::max(r, 1e-30) / g.shell()) / std::log(1.0 / g.lambda)), -30.0, 30.0);
    const double s = std::pow(g.lambda, m);
    const FVec3 y = fractalApply(g, m, x);
    const double delta = s * detailAngle * std::max(len(sub(x, eye)), 1e-3 * eyeScale);
    const double level = std::clamp(std::log(kFractalPiece / delta) / std::log(1.0 / fractalPieceScale(k)), 0.0,
                                    fractalMaxLevel(k));
    return std::min(fractalDistanceS(k, y, level) / s, r * (g.rho / g.shell() - 1.0));
}

FractalMap fractalMapFor(FractalKind k, const int* word, int wordLength, const FMat3& sigma) {
    const double s = fractalPieceScale(k);
    FMat3 A{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    FVec3 b{0, 0, 0};
    for (int i = 0; i < wordLength; ++i) {  // (A, b) o (s I, off): x -> A (s x + off) + b
        b = add(matvec(A, fractalPieceOffset(k, word[i])), b);
        for (auto& row : A)
            for (double& v : row) v *= s;
    }
    A = matmul(A, sigma);
    FractalMap g;
    g.lambda = std::pow(s, double(wordLength));
    // p = (I - A)^-1 b
    FMat3 M{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) M[std::size_t(i)][std::size_t(j)] = (i == j ? 1.0 : 0.0) - A[std::size_t(i)][std::size_t(j)];
    const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                       M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    FMat3 inv{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            inv[std::size_t(c)][std::size_t(r)] = (M[std::size_t(r1)][std::size_t(c1)] * M[std::size_t(r2)][std::size_t(c2)] -
                                                   M[std::size_t(r1)][std::size_t(c2)] * M[std::size_t(r2)][std::size_t(c1)]) / det;
        }
    g.p = matvec(inv, b);
    // The rotation Q = A / lambda: axis and angle.
    FMat3 Q = A;
    for (auto& row : Q)
        for (double& v : row) v /= g.lambda;
    const double tr = Q[0][0] + Q[1][1] + Q[2][2];
    const double ang = std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0));
    if (ang < 1e-9) {
        g.angle = 0;
        g.axis = {0, 0, 1};
    } else if (kPi - ang < 1e-9) {  // a half turn: the axis from the diagonal of (Q + I) / 2
        g.angle = kPi;
        int i = 0;
        for (int j = 1; j < 3; ++j)
            if (Q[std::size_t(j)][std::size_t(j)] > Q[std::size_t(i)][std::size_t(i)]) i = j;
        FVec3 a{};
        for (int j = 0; j < 3; ++j) a[std::size_t(j)] = (Q[std::size_t(j)][std::size_t(i)] + (i == j ? 1.0 : 0.0)) / 2.0;
        g.axis = norm(a, {0, 0, 1});
    } else {
        g.angle = ang;
        g.axis = norm({Q[2][1] - Q[1][2], Q[0][2] - Q[2][0], Q[1][0] - Q[0][1]}, {0, 0, 1});
    }
    // rho: lambda^-1 x the distance from p to S's other pieces at this level (a lower bound through their hulls).
    const int n = fractalPieceCount(k);
    double nearest = 1e30;
    std::vector<int> w2(std::size_t(std::max(1, wordLength)), 0);
    const int total = int(std::lround(std::pow(double(n), double(wordLength))));
    for (int idx = 0; idx < total; ++idx) {
        int v = idx;
        bool same = true;
        for (int i = wordLength - 1; i >= 0; --i) {
            w2[std::size_t(i)] = v % n;
            v /= n;
            same = same && w2[std::size_t(i)] == word[i];
        }
        if (same) continue;
        FVec3 bb{0, 0, 0};
        double sc = 1.0;
        for (int i = 0; i < wordLength; ++i) {
            bb = add(mul(fractalPieceOffset(k, w2[std::size_t(i)]), sc), bb);
            sc *= s;
        }
        nearest = std::min(nearest, fractalHullDistance(k, mul(sub(g.p, bb), 1.0 / sc)) * sc);
    }
    g.rho = std::max(1e-6, nearest) / g.lambda;
    return g;
}

namespace {

constexpr FMat3 kIdentity3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
constexpr FMat3 kTurn120{{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}};    // (x, y, z) -> (z, x, y): a third of a turn about (1,1,1)
constexpr FMat3 kSierVortex{{{0, 0, 1}, {-1, 0, 0}, {0, -1, 0}}};  // a third of a turn about (-1, 1, -1)
constexpr FMat3 kQuarterZ{{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}}};  // a quarter turn about -z

FVec3 unit(const FVec3& v) { return norm(v, {0, 0, 1}); }

std::vector<FractalPath> makePaths() {
    std::vector<FractalPath> v;
    const double r3 = 1.0 / std::sqrt(3.0);
    const FVec3 diag{r3, r3, r3};
    {  // Sierpinski apex: outside the tip of the endless pyramid, a third of a turn per halving.
        const int w[] = {0};
        FractalPath p{"apex", FractalKind::Sierpinski,
                      "the tip of an endless Sierpinski pyramid from outside, turning a third about its axis per step",
                      fractalMapFor(FractalKind::Sierpinski, w, 1, kTurn120), {}, {}, {}, 45, 3.0, diag, 0.25, 0, 0.05};
        const double r = 0.9 * p.g.shell();
        const FVec3 side = unit({1, -1, 0});
        const FVec3 dir = add(mul(diag, std::cos(55 * kPi / 180)), mul(side, std::sin(55 * kPi / 180)));
        p.eye = add(p.g.p, mul(dir, r));
        p.aim = add(p.g.p, mul(diag, -0.18 * r));
        p.up = diag;
        v.push_back(p);
    }
    {  // Sierpinski dive: inside the pyramid down its axis, through the triangular windows, rolling a third per step.
        const int w[] = {0};
        FractalPath p{"dive", FractalKind::Sierpinski,
                      "down the axis of the endless pyramid through its triangular windows, rolling a third per step",
                      fractalMapFor(FractalKind::Sierpinski, w, 1, kTurn120), {}, {}, {}, 70, 4.0, diag, 0, 0, 0.12};
        const double r = 0.9 * p.g.shell();
        p.eye = add(p.g.p, mul(diag, -r));
        p.aim = p.g.p;
        p.up = unit({1, -1, 0});
        v.push_back(p);
    }
    {  // Sierpinski vortex: spiralling in on a point deep inside the gaps, a quarter size and a third of a turn per step.
        const int w[] = {2, 3};
        FractalPath p{"vortex", FractalKind::Sierpinski,
                      "spiralling in on a point inside the gaps, a quarter the size and a third of a turn per step",
                      fractalMapFor(FractalKind::Sierpinski, w, 2, kSierVortex), {}, {}, {}, 65, 3.0, {}, 0.12, 0, 0.1};
        const double r = 0.9 * p.g.shell();
        p.eye = add(p.g.p, mul(unit({-0.807, 0.029, 0.59}), r));
        p.aim = p.g.p;
        p.up = p.g.axis;
        p.orbitAxis = p.g.axis;
        v.push_back(p);
    }
    {  // Menger corner: outside the corner of an endless sponge, a third of a turn about the diagonal per step.
        const int w[] = {0};
        FractalPath p{"corner", FractalKind::Menger,
                      "the corner of an endless Menger sponge from outside, turning a third about the diagonal per step",
                      fractalMapFor(FractalKind::Menger, w, 1, kTurn120), {}, {}, {}, 45, 3.0, diag, 0.25, 0, 0.05};
        const double r = 0.9 * p.g.shell();
        const FVec3 side = unit({1, -1, 0});
        const double polar = 125 * kPi / 180;  // from the inward diagonal
        const FVec3 dir = add(mul(diag, std::cos(polar)), mul(side, std::sin(polar)));
        p.eye = add(p.g.p, mul(dir, r));
        p.aim = add(p.g.p, mul(diag, 0.35 * r));
        p.up = mul(diag, -1);
        v.push_back(p);
    }
    {  // Menger vortex: through the sponge's tunnels, spiralling in on a point a third the size and a quarter turn per step.
        const int w[] = {1};
        FractalPath p{"vortex", FractalKind::Menger,
                      "through the sponge's tunnels, spiralling in on a point, a third the size and a quarter turn per step",
                      fractalMapFor(FractalKind::Menger, w, 1, kQuarterZ), {}, {}, {}, 65, 3.0, {}, 0.12, 0, 0.1};
        const double r = 0.9 * p.g.shell();
        p.eye = add(p.g.p, mul(unit({-0.371, 0.928, 0.008}), r));
        p.aim = p.g.p;
        p.up = p.g.axis;
        p.orbitAxis = p.g.axis;
        v.push_back(p);
    }
    {  // Menger tunnel: straight down the sponge's tunnels toward a point between two edge cubes, a ninth per step.
        const int w[] = {9, 10};
        FractalPath p{"tunnel", FractalKind::Menger,
                      "straight down the sponge's tunnels toward a point deep inside, a ninth the size per step",
                      fractalMapFor(FractalKind::Menger, w, 2, kIdentity3), {}, {}, {}, 65, 3.0, {}, 0.1, 0, 0.1};
        const double r = 0.9 * p.g.shell();
        const FVec3 dir = unit({0.016, 0.708, 0.706});
        p.eye = add(p.g.p, mul(dir, r));
        p.aim = p.g.p;
        p.up = {1, 0, 0};
        p.orbitAxis = dir;
        v.push_back(p);
    }
    return v;
}

const std::vector<FractalPath>& paths() {
    static const std::vector<FractalPath> v = makePaths();
    return v;
}

}  // namespace

int fractalPathCount() { return int(paths().size()); }
const FractalPath& fractalPath(int i) {
    const int n = fractalPathCount();
    return paths()[std::size_t(((i % n) + n) % n)];
}

const FractalPath* findFractalPath(FractalKind k, std::string_view name) {
    std::string s;
    for (char c : name)
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    for (const auto& p : paths())
        if (p.kind == k && (s.empty() || s == p.name)) return &p;
    return nullptr;
}

std::string fractalPathNames(FractalKind k) {
    std::string out;
    for (const auto& p : paths())
        if (p.kind == k) out += (out.empty() ? "" : ", ") + std::string(p.name);
    return out;
}

FractalCamera fractalCamera(const FractalPath& path, double u, const FractalSway& sway) {
    const FractalMap& g = path.g;
    u = std::clamp(fin(u, 0.0), -64.0, 64.0);
    // W: the eye, its aim and up turned about the orbit axis through p.
    const double orbit = std::clamp(fin(sway.orbit, 0.0), -1e6, 1e6);
    const FVec3 oa = norm(path.orbitAxis, g.axis);
    const FVec3 e = add(g.p, rotate(sub(path.eye, g.p), oa, orbit));
    const FVec3 a = add(g.p, rotate(sub(path.aim, g.p), oa, orbit));
    const FVec3 upHint = rotate(path.up, oa, orbit);
    FVec3 f = norm(sub(a, e), {0, 0, -1});
    FVec3 r = norm(cross(f, upHint), norm(cross(f, {0, 1, 0}), {1, 0, 0}));
    FVec3 up = cross(r, f);
    // Look around (yaw about up, pitch about right), then roll about the view axis.
    const double lx = std::clamp(fin(sway.lookX, 0.0), -3.0, 3.0), ly = std::clamp(fin(sway.lookY, 0.0), -1.5, 1.5);
    f = rotate(f, up, -lx);
    r = rotate(r, up, -lx);
    f = rotate(f, r, ly);
    up = rotate(up, r, ly);
    const double roll = std::clamp(fin(sway.roll, 0.0), -1e6, 1e6);
    r = rotate(r, f, roll);
    up = rotate(up, f, roll);
    FractalCamera c;
    c.eye = fractalApply(g, u, e);
    c.forward = fractalRotate(g, u, f);
    c.right = fractalRotate(g, u, r);
    c.up = fractalRotate(g, u, up);
    c.fov = sway.fov > 0 && std::isfinite(sway.fov) ? std::clamp(sway.fov, 10.0, 150.0) : path.fov;
    c.scale = std::pow(g.lambda, u) * len(sub(e, g.p));
    return c;
}

// ---- music

double FractalClock::baseZoom(const FractalTiming& t, double bar) {
    bar = std::clamp(fin(bar, 0.0), -1e12, 1e12);
    const double per = std::clamp(fin(t.barsPerStep, 2.0), 0.25, 64.0);
    const double pulse = std::clamp(fin(t.pulse, 0.5), 0.0, 1.0);
    // Each bar's share (or each step's, for a step that isn't a whole number of bars) eased so it surges right
    // after its line: speed 1 + 2 pulse there, 1 - pulse at its end.
    const double unit = per >= 1.0 && per == std::floor(per) ? 1.0 : per;
    const double x = bar / unit;
    const double whole = std::floor(x), f = x - whole;
    const double e = (1.0 - pulse) * f + pulse * (1.0 - (1.0 - f) * (1.0 - f) * (1.0 - f));
    // Whole units first reduced by the units per step, so the result stays precise for any clock.
    const double n = std::max(1.0, std::round(per / unit));
    const double rest = whole - std::floor(whole / n) * n;  // units into the step, 0 .. n - 1
    const double z = (rest + e) / n;
    return z - std::floor(z);
}

FractalBeat FractalClock::step(double beat, double bar, bool clockOn, double age, double build, double drop,
                               double dt) {
    age = std::clamp(fin(age, 0.0), 0.0, 1e9);
    if (!clockOn) {  // 120 BPM from the start
        beat = age * 2.0;
        bar = beat / 4.0;
        build = 0;
        drop = 0;
    }
    beat = std::clamp(fin(beat, 0.0), -1e12, 1e12);
    bar = std::clamp(fin(bar, 0.0), -1e12, 1e12);
    build = std::clamp(fin(build, 0.0), 0.0, 1.0);
    drop = std::clamp(fin(drop, 0.0), 0.0, 1.0);
    dt = std::clamp(fin(dt, 0.0), 0.0, 0.25);
    FractalBeat out;
    const double base = baseZoom(t_, bar);
    // The base zoom's advance this step (its wrap at each step's end taken out; a jump of the clock counts as none).
    double adv = base - prevBase_;
    if (adv < -0.5) adv += 1.0;
    if (!started_ || adv < 0 || adv > 0.5) adv = 0;
    started_ = true;
    prevBase_ = base;
    // Build: fall behind (the zoom slows to 1 - Hovr at the top of the build); afterwards catch up to the next
    // whole step within about a beat, so the steps land on bar lines again.
    const double hover = std::clamp(fin(t_.hover, 0.75), 0.0, 1.0);
    if (build > 0) {
        lag_ += hover * build * adv;
    } else if (lag_ > 0) {
        const double target = std::floor(lag_);
        lag_ = target + (lag_ - target) * std::exp(-dt / 0.35);
        if (lag_ - target < 1e-4) lag_ = target;
    }
    lag_ -= std::floor(lag_);
    // Drop: a burst of Drop steps over DrpB beats (eased in and out), starting on the rise of SYNC_DROP.
    const double dropSteps = std::clamp(fin(t_.drop, 2.0), 0.0, 16.0);
    const double dropBeats = std::clamp(fin(t_.dropBeats, 4.0), 0.25, 64.0);
    double burst = 0;
    if (drop > 0.6 && dropPrev_ <= 0.6 && dropSteps > 0) {
        if (bursting_) off_ += dropSteps;  // a burst still running: finish it
        bursting_ = true;
        dropFrom_ = beat;
    }
    dropPrev_ = drop;
    if (bursting_) {
        const double x = (beat - dropFrom_) / dropBeats;
        if (x >= 1.0 || x < 0.0) {  // done (or the clock jumped back): the whole burst taken
            off_ += dropSteps;
            bursting_ = false;
        } else {
            burst = dropSteps * x * x * (3.0 - 2.0 * x);
        }
    }
    off_ -= std::floor(off_);
    const double z = base - lag_ + off_ + burst;
    out.zoom = z - std::floor(z);
    if (!(out.zoom >= 0.0 && out.zoom < 1.0)) out.zoom = 0.0;
    const double fb = beat - std::floor(beat);
    out.pulse = clockOn ? std::exp(-6.0 * fb) : 0.0;
    const double stepE = std::clamp(fin(t_.beatStep, 0.6), 0.0, 1.0);
    const double eased = 1.0 - (1.0 - fb) * (1.0 - fb) * (1.0 - fb);
    out.beats = std::fmod(std::floor(beat), 65536.0) + (1.0 - stepE) * fb + stepE * eased;
    return out;
}

}  // namespace reforce::engine
