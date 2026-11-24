#include "engine/Knight.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace reforce::engine::knight {
namespace {

constexpr int kMoves[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};

struct Neighbors {
    std::array<std::array<int8_t, 8>, kSquares> to{};
    std::array<int8_t, kSquares> count{};
    Neighbors() {
        for (int s = 0; s < kSquares; ++s) {
            const int r = s / kSide, c = s % kSide;
            for (const auto& m : kMoves) {
                const int rr = r + m[0], cc = c + m[1];
                if (rr >= 0 && rr < kSide && cc >= 0 && cc < kSide)
                    to[std::size_t(s)][std::size_t(count[std::size_t(s)]++)] = int8_t(rr * kSide + cc);
            }
        }
    }
};
const Neighbors& neighbors() {
    static const Neighbors n;
    return n;
}

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int below(int n) { return int(next() % uint64_t(n)); }
};

// Fallbacks (from home g1), used only if the bounded searches find nothing (never seen in the tests).
constexpr Tour kFallbackSymmetric = {6,  12, 2,  8,  25, 10, 0,  17, 32, 49, 59, 42, 48, 58, 52, 62,
                                     47, 30, 36, 26, 43, 60, 54, 44, 34, 24, 18, 35, 41, 56, 50, 40,
                                     57, 51, 61, 55, 38, 53, 63, 46, 31, 14, 4,  21, 15, 5,  11, 1,
                                     16, 33, 27, 37, 20, 3,  9,  19, 29, 39, 45, 28, 22, 7,  13, 23};

Tour rotatedToStart(const Tour& t, int home) {
    int at = 0;
    for (int i = 0; i < kSquares; ++i)
        if (t[std::size_t(i)] == home) at = i;
    Tour r{};
    for (int i = 0; i < kSquares; ++i) r[std::size_t(i)] = t[std::size_t((at + i) % kSquares)];
    return r;
}

// The board's symmetry `k` (0..7: quarter turns, then mirrored) applied to every square; knight moves stay knight moves.
Tour transformed(const Tour& t, int k) {
    Tour r{};
    for (int i = 0; i < kSquares; ++i) {
        int row = t[std::size_t(i)] / kSide, col = t[std::size_t(i)] % kSide;
        if (k & 4) col = kSide - 1 - col;
        for (int q = 0; q < (k & 3); ++q) {
            const int nr = col, nc = kSide - 1 - row;
            row = nr;
            col = nc;
        }
        r[std::size_t(i)] = int8_t(row * kSide + col);
    }
    return r;
}

// Randomized Warnsdorff from `home`: always to an unvisited neighbor with the fewest onward moves, ties at random.
bool warnsdorff(Rng& rng, int home, Tour& out) {
    const Neighbors& nb = neighbors();
    std::array<bool, kSquares> used{};
    std::array<int8_t, kSquares> deg = nb.count;
    int cur = home;
    used[std::size_t(cur)] = true;
    out[0] = int8_t(cur);
    for (int k = 0; k < nb.count[std::size_t(cur)]; ++k) --deg[std::size_t(nb.to[std::size_t(cur)][std::size_t(k)])];
    for (int step = 1; step < kSquares; ++step) {
        // Early on, now and then a square one onward move worse than the best is allowed too: pure Warnsdorff
        // finds only a few dozen closed tours from one square.
        int cand[8], cdeg[8], nc = 0, bestDeg = 99;
        for (int k = 0; k < nb.count[std::size_t(cur)]; ++k) {
            const int n = nb.to[std::size_t(cur)][std::size_t(k)];
            if (used[std::size_t(n)]) continue;
            cand[nc] = n;
            cdeg[nc] = deg[std::size_t(n)];
            if (cdeg[nc] < bestDeg && (cdeg[nc] > 0 || step == kSquares - 1)) bestDeg = cdeg[nc];
            ++nc;
        }
        const int slack = step < 32 && rng.below(4) == 0 ? 1 : 0;
        int best[8], nbest = 0;
        for (int i = 0; i < nc; ++i)
            if (cdeg[i] >= bestDeg && cdeg[i] <= bestDeg + slack) best[nbest++] = cand[i];
        if (nbest == 0) return false;
        cur = best[rng.below(nbest)];
        used[std::size_t(cur)] = true;
        out[std::size_t(step)] = int8_t(cur);
        for (int k = 0; k < nb.count[std::size_t(cur)]; ++k)
            --deg[std::size_t(nb.to[std::size_t(cur)][std::size_t(k)])];
    }
    return isKnightMove(out[kSquares - 1], out[0]);
}

// Half-turn symmetric tours: a path of 32 squares holding one square of each pair {s, 63 - s}, whose last square is a
// knight move from the first one's partner; the tour is the path followed by its half turn. Depth first, neighbors
// in Warnsdorff order (ties shuffled), at most `budget` nodes.
struct HalfSearch {
    Rng& rng;
    int budget;
    int nodes = 0;
    std::array<bool, kSquares> used{};
    std::array<int8_t, kSquares / 2> path{};

    int degree(int s) const {
        const Neighbors& nb = neighbors();
        int d = 0;
        for (int k = 0; k < nb.count[std::size_t(s)]; ++k)
            if (!used[std::size_t(nb.to[std::size_t(s)][std::size_t(k)])]) ++d;
        return d;
    }
    bool dfs(int depth) {
        if (++nodes > budget) return false;
        const int cur = path[std::size_t(depth - 1)];
        if (depth == kSquares / 2) return isKnightMove(cur, kSquares - 1 - path[0]);
        const Neighbors& nb = neighbors();
        int cand[8], key[8], n = 0;
        for (int k = 0; k < nb.count[std::size_t(cur)]; ++k) {
            const int s = nb.to[std::size_t(cur)][std::size_t(k)];
            if (used[std::size_t(s)]) continue;
            cand[n] = s;
            // Warnsdorff order, ties shuffled, now and then a square taken as if one move worse (more variety).
            key[n] = (degree(s) + (rng.below(4) == 0 ? 1 : 0)) * 16 + rng.below(16);
            ++n;
        }
        for (int i = 1; i < n; ++i)  // insertion sort by key (deterministic)
            for (int j = i; j > 0 && key[j] < key[j - 1]; --j) {
                std::swap(key[j], key[j - 1]);
                std::swap(cand[j], cand[j - 1]);
            }
        for (int i = 0; i < n; ++i) {
            const int s = cand[i];
            used[std::size_t(s)] = used[std::size_t(kSquares - 1 - s)] = true;
            path[std::size_t(depth)] = int8_t(s);
            if (dfs(depth + 1)) return true;
            used[std::size_t(s)] = used[std::size_t(kSquares - 1 - s)] = false;
            if (nodes > budget) return false;
        }
        return false;
    }
};

}  // namespace

bool isKnightMove(int a, int b) {
    if (a < 0 || a >= kSquares || b < 0 || b >= kSquares) return false;
    const int dr = std::abs(a / kSide - b / kSide), dc = std::abs(a % kSide - b % kSide);
    return (dr == 1 && dc == 2) || (dr == 2 && dc == 1);
}

bool isClosedTour(const Tour& t) {
    std::array<bool, kSquares> seen{};
    for (int i = 0; i < kSquares; ++i) {
        const int s = t[std::size_t(i)];
        if (s < 0 || s >= kSquares || seen[std::size_t(s)]) return false;
        seen[std::size_t(s)] = true;
        if (!isKnightMove(s, t[std::size_t((i + 1) % kSquares)])) return false;
    }
    return true;
}

bool isSymmetric(const Tour& t) {
    for (int i = 0; i < kSquares / 2; ++i)
        if (t[std::size_t(i + kSquares / 2)] != kSquares - 1 - t[std::size_t(i)]) return false;
    return true;
}

Tour makeTour(uint64_t seed, bool symmetric, int home) {
    home = std::clamp(home, 0, kSquares - 1);
    Rng rng{seed * 0xD1342543DE82EF95ull + (symmetric ? 0x5Bull : 0x17ull)};
    Tour t{};
    if (symmetric) {
        for (int attempt = 0; attempt < 48; ++attempt) {
            HalfSearch h{rng, 3000};
            const int s0 = rng.below(kSquares);
            h.used[std::size_t(s0)] = h.used[std::size_t(kSquares - 1 - s0)] = true;
            h.path[0] = int8_t(s0);
            if (h.dfs(1)) {
                for (int i = 0; i < kSquares / 2; ++i) {
                    t[std::size_t(i)] = h.path[std::size_t(i)];
                    t[std::size_t(i + kSquares / 2)] = int8_t(kSquares - 1 - h.path[std::size_t(i)]);
                }
                return rotatedToStart(transformed(t, rng.below(8)), home);
            }
        }
        return rotatedToStart(kFallbackSymmetric, home);
    }
    for (int attempt = 0; attempt < 3000; ++attempt)
        if (warnsdorff(rng, home, t)) {
            // One of the board's eight symmetries for more variety (a closed tour visits home anyway).
            return rotatedToStart(transformed(t, rng.below(8)), home);
        }
    return rotatedToStart(kFallbackSymmetric, home);
}

const Tour& TourCache::get(uint64_t seed, bool symmetric, int home) {
    const int flags = (symmetric ? 1 : 0) | (std::clamp(home, 0, kSquares - 1) << 1);
    for (const Entry& e : entries_)
        if (e.flags == flags && e.seed == seed) return e.tour;
    Entry& e = entries_[std::size_t(next_)];
    next_ = (next_ + 1) % int(entries_.size());
    e.seed = seed;
    e.flags = flags;
    e.tour = makeTour(seed, symmetric, home);
    return e.tour;
}

float hopHeading(const Tour& t, int k) {
    k = std::clamp(k, 1, kSquares);
    const int a = t[std::size_t(k - 1)], b = t[std::size_t(k % kSquares)];
    return std::atan2(squareY(b) - squareY(a), squareX(b) - squareX(a));
}

namespace {
float smooth01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
float angleTo(float from, float to) {  // shortest signed turn
    const float pi = 3.14159265f;
    float d = std::fmod(to - from + pi, 2.0f * pi);
    if (d < 0) d += 2.0f * pi;
    return d - pi;
}
}  // namespace

Frame frameAt(const Tour& tour, const Tour& prev, double turn, double hover) {
    Frame f;
    if (!std::isfinite(turn)) turn = 0.0;
    turn = std::clamp(turn, 0.0, double(kSquares));
    if (!std::isfinite(hover)) hover = 0.0;
    const float hv = float(std::clamp(hover, 0.0, 1.0));
    int k = int(std::floor(turn));  // hops landed
    float p = float(turn - k);      // phase within the next hop
    if (k >= kSquares) {
        k = kSquares;
        p = 0.0f;
    }
    f.landed = k;
    f.since = p;
    f.square = tour[std::size_t(k % kSquares)];
    f.next = k < kSquares ? tour[std::size_t((k + 1) % kSquares)] : f.square;
    const float restHeading = k == 0 ? hopHeading(prev, kSquares) : hopHeading(tour, k);
    float x = squareX(f.square), y = squareY(f.square), z = 0.0f, squash = 1.0f, pitch = 0.0f;
    float heading = restHeading;
    if (k < kSquares) {
        // Turn towards the next square (.3 .. .75), crouch (.36 .. .5), fly (.5 .. 1), land on the whole turn.
        const float nextHeading = hopHeading(tour, k + 1);
        heading = restHeading + angleTo(restHeading, nextHeading) * smooth01((p - 0.3f) / 0.45f);
        if (p < 0.22f) squash = 1.0f - 0.13f * std::sin(3.14159265f * p / 0.22f);  // the landing settles
        if (p >= 0.36f && p < 0.5f) squash = 1.0f - 0.09f * std::sin(3.14159265f * (p - 0.36f) / 0.14f);
        if (p >= 0.5f) {
            const float u = (p - 0.5f) / 0.5f;
            const float e = 0.5f - 0.5f * std::cos(3.14159265f * u);  // ease in / out along the ground
            f.grow = e;
            x += (squareX(f.next) - x) * e;
            y += (squareY(f.next) - y) * e;
            const float h = 0.85f;
            z = 4.0f * h * u * (1.0f - u);
            squash = 1.0f + 0.06f * std::sin(3.14159265f * u);
            // Nose up on the way up, down on the way down (the arc's slope, softened).
            const float slope = 4.0f * h * (1.0f - 2.0f * u) / (2.236f * 3.14159265f * 0.5f * std::sin(3.14159265f * u) + 0.6f);
            pitch = 0.35f * std::atan(slope);
        }
    }
    // A build: the knight floats and bobs (it doesn't touch down while it hovers).
    z += hv * (0.28f + 0.06f * std::sin(6.2831853f * p));
    f.x = x;
    f.y = y;
    f.z = z;
    f.heading = heading;
    f.squash = squash;
    f.pitch = pitch;
    return f;
}

// ---------------------------------------------------------------------------------------------- the piece
namespace {

struct V2 {
    double x, y;
};

std::vector<V2> catmull(const std::vector<V2>& pts, int n) {
    std::vector<V2> out;
    const int m = int(pts.size());
    for (int i = 0; i < m; ++i) {
        const V2 p0 = pts[std::size_t((i + m - 1) % m)], p1 = pts[std::size_t(i)], p2 = pts[std::size_t((i + 1) % m)],
                 p3 = pts[std::size_t((i + 2) % m)];
        for (int k = 0; k < n; ++k) {
            const double t = double(k) / n, t2 = t * t, t3 = t2 * t;
            auto c = [&](double a0, double a1, double a2, double a3) {
                return 0.5 * (2 * a1 + (-a0 + a2) * t + (2 * a0 - 5 * a1 + 4 * a2 - a3) * t2 +
                              (-a0 + 3 * a1 - 3 * a2 + a3) * t3);
            };
            out.push_back({c(p0.x, p1.x, p2.x, p3.x), c(p0.y, p1.y, p2.y, p3.y)});
        }
    }
    return out;
}

// Signed distance to a closed polygon (negative inside).
double sdPoly(double px, double py, const std::vector<V2>& v) {
    double d = 1e18, s = 1.0;
    const std::size_t n = v.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i, ++i) {
        const double ex = v[j].x - v[i].x, ey = v[j].y - v[i].y;
        const double wx = px - v[i].x, wy = py - v[i].y;
        const double h = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0, 1.0);
        const double qx = wx - ex * h, qy = wy - ey * h;
        d = std::min(d, qx * qx + qy * qy);
        const bool c1 = py >= v[i].y, c2 = py < v[j].y, c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    return s * std::sqrt(d);
}

// A 2D distance table with bilinear lookups.
struct Table {
    double x0 = 0, y0 = 0, h = 1;
    int nx = 0, ny = 0;
    std::vector<float> v;
    void build(const std::vector<V2>& poly, double ax, double ay, double bx, double by, double step) {
        x0 = ax;
        y0 = ay;
        h = step;
        nx = int(std::ceil((bx - ax) / step)) + 1;
        ny = int(std::ceil((by - ay) / step)) + 1;
        v.resize(std::size_t(nx) * std::size_t(ny));
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) v[std::size_t(j * nx + i)] = float(sdPoly(x0 + i * h, y0 + j * h, poly));
    }
    double at(double x, double y) const {
        const double fx = std::clamp((x - x0) / h, 0.0, double(nx - 1) - 1e-9);
        const double fy = std::clamp((y - y0) / h, 0.0, double(ny - 1) - 1e-9);
        const int i = int(fx), j = int(fy);
        const double ux = fx - i, uy = fy - j;
        auto g = [&](int a, int b) { return double(v[std::size_t(b * nx + a)]); };
        const double d = (g(i, j) * (1 - ux) + g(i + 1, j) * ux) * (1 - uy) + (g(i, j + 1) * (1 - ux) + g(i + 1, j + 1) * ux) * uy;
        // Outside the table: add the distance beyond it (keeps far points outside).
        const double ox = std::max({x0 - x, x - (x0 + (nx - 1) * h), 0.0});
        const double oy = std::max({y0 - y, y - (y0 + (ny - 1) * h), 0.0});
        return d + std::sqrt(ox * ox + oy * oy);
    }
};

double smin(double a, double b, double k) {
    const double h = std::clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return b * (1 - h) + a * h - k * h * (1 - h);
}
double smax(double a, double b, double k) { return -smin(-a, -b, k); }

// Rounded extrusion of a 2D distance d2 to half width w along y, edges rounded with radius r.
double extrude(double d2, double y, double w, double r) {
    const double ex = d2 + r, ey = std::fabs(y) - (w - r);
    return std::min(std::max(ex, ey), 0.0) + std::hypot(std::max(ex, 0.0), std::max(ey, 0.0)) - r;
}

double smooth(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// Distance to the segment a-b in the (x, z) plane.
double segment2(double x, double z, double ax, double az, double bx, double bz) {
    const double pax = x - ax, paz = z - az, bax = bx - ax, baz = bz - az;
    const double h = std::clamp((pax * bax + paz * baz) / (bax * bax + baz * baz), 0.0, 1.0);
    return std::hypot(pax - bax * h, paz - baz * h);
}

struct PieceField {
    Table head, ear, mane, base;

    PieceField() {
        // Side profile of the head and neck (x forward, z up; Catmull-Rom through these points): the chest swells
        // forward from the collar, the throat cuts in under the jaw, the head reaches forward and down to a rounded
        // muzzle, the forehead rises to the poll, and the neck sweeps down the back.
        const std::vector<V2> headPts = {
            {0.13, 0.265}, {0.20, 0.335}, {0.245, 0.43}, {0.235, 0.50}, {0.195, 0.552}, {0.25, 0.578},
            {0.36, 0.572}, {0.45, 0.578}, {0.505, 0.605}, {0.522, 0.655}, {0.505, 0.703}, {0.43, 0.745},
            {0.33, 0.805}, {0.225, 0.875}, {0.13, 0.92},  {0.03, 0.93},  {-0.06, 0.895}, {-0.125, 0.80},
            {-0.175, 0.68}, {-0.21, 0.54}, {-0.222, 0.42}, {-0.20, 0.32}, {-0.15, 0.265}, {0.0, 0.245}};
        // An ear (pointing up and a little back) and the mane (a band along the back of the neck).
        const std::vector<V2> earPts = {{0.0, 0.88}, {0.13, 0.885}, {0.115, 0.965}, {0.06, 1.045}, {0.02, 0.975}};
        const std::vector<V2> manePts = {{0.02, 0.945}, {-0.075, 0.915}, {-0.15, 0.815}, {-0.205, 0.69},
                                         {-0.243, 0.545}, {-0.255, 0.42}, {-0.235, 0.32}, {-0.17, 0.33},
                                         {-0.17, 0.45},  {-0.15, 0.58},  {-0.11, 0.71},  {-0.05, 0.82}, {0.02, 0.86}};
        // Base: (radius, z), lathed: a foot, a bead, a cove and a collar.
        const std::vector<V2> basePts = {{0.0, 0.0},     {0.36, 0.0},    {0.375, 0.015}, {0.375, 0.045}, {0.35, 0.065},
                                         {0.325, 0.075}, {0.33, 0.095},  {0.345, 0.11},  {0.33, 0.13},   {0.27, 0.17},
                                         {0.245, 0.20},  {0.275, 0.215}, {0.28, 0.235},  {0.24, 0.255},  {0.0, 0.26}};
        const double step = 0.005;
        head.build(catmull(headPts, 10), -0.4, 0.1, 0.65, 1.2, step);
        ear.build(catmull(earPts, 6), -0.1, 0.75, 0.3, 1.2, step);
        mane.build(catmull(manePts, 6), -0.4, 0.2, 0.15, 1.1, step);
        std::vector<V2> b = catmull(basePts, 8);
        for (V2& p : b) p.x = std::max(p.x, 0.0);
        base.build(b, 0.0, -0.1, 0.6, 0.4, step);
    }

    // Half width of the head and neck: a full neck, narrowing to the muzzle, with the cheeks swelling.
    static double width(double x, double z) {
        const double cheek = std::exp(-((x - 0.21) * (x - 0.21) + (z - 0.67) * (z - 0.67)) / (2 * 0.07 * 0.07));
        return 0.148 - 0.05 * smooth(0.12, 0.52, x) + 0.012 * smooth(0.6, 0.3, z) + 0.016 * cheek;
    }

    double operator()(double x, double y, double z) const {
        const double r = std::hypot(x, y);
        double d = smax(base.at(r, z), -z, 0.03);  // flat underneath, the edge rounded
        const double w = width(x, z);
        d = smin(d, extrude(head.at(x, z), y, w, 0.06), 0.045);
        // Ears, a little apart and leaning out.
        const double de = ear.at(x, z);
        for (int sy = -1; sy <= 1; sy += 2) {
            const double ey = y - sy * (0.06 + 0.25 * std::max(0.0, z - 0.9));
            d = smin(d, extrude(de, ey, 0.036, 0.022), 0.02);
        }
        // Mane: a narrower ridge along the back, carved with slanting grooves.
        {
            double m = extrude(mane.at(x, z), y, 0.062, 0.03);
            const double g = std::sin((z * 0.82 + x * 0.57) * 120.0);
            m += 0.007 * std::max(0.0, g) * std::max(0.0, g);
            d = smin(d, m, 0.012);
        }
        for (int sy = -1; sy <= 1; sy += 2) {
            const double ex = x - 0.235, ez = z - 0.80;
            const double side = w + 0.0;
            // Eye: a socket with a round eyeball in it, under a little brow.
            const double socket = std::sqrt(ex * ex + (y - sy * side) * (y - sy * side) + ez * ez) - 0.046;
            d = smax(d, -socket, 0.016);
            const double ball = std::sqrt(ex * ex + (y - sy * (side - 0.04)) * (y - sy * (side - 0.04)) + ez * ez) - 0.034;
            d = smin(d, ball, 0.004);
            // Nostril.
            const double nx = x - 0.485, nz = z - 0.668, ny = y - sy * (w + 0.01);
            d = smax(d, -(std::sqrt(nx * nx + ny * ny + nz * nz) - 0.024), 0.01);
        }
        // The mouth: a groove from the front of the muzzle back along the side.
        {  // carved into the sides and the front only, so it reads as a groove, not a slit
            const double outer = std::max(std::fabs(y) - (w - 0.04), x - 0.5);
            const double groove = std::max(segment2(x, z, 0.54, 0.618, 0.44, 0.608) - 0.016, -outer);
            d = smax(d, -groove, 0.012);
        }
        return d;
    }
};


Mesh buildPiece(double cell) {
    cell = std::clamp(cell, 0.005, 0.05);
    const PieceField f;
    Mesh m;
    m.kind = MeshKind::Knight;
    m.convex = false;
    const double bx0 = -0.45, by0 = -0.45, bz0 = -0.0437, bx1 = 0.62, by1 = 0.45, bz1 = 1.15;
    const int nx = int(std::ceil((bx1 - bx0) / cell)) + 1, ny = int(std::ceil((by1 - by0) / cell)) + 1,
              nz = int(std::ceil((bz1 - bz0) / cell)) + 1;
    auto node = [&](int i, int j, int k) { return (std::size_t(k) * std::size_t(ny) + std::size_t(j)) * std::size_t(nx) + std::size_t(i); };
    std::vector<float> val(std::size_t(nx) * std::size_t(ny) * std::size_t(nz));
    // A narrow band: the field on a grid 4x coarser first; a fine node farther from the surface than its coarse
    // node's distance allows (|d| > the coarse half diagonal + a margin; the field is about 1-Lipschitz) keeps the coarse
    // value (only its sign matters there). About a fifth of the nodes are evaluated.
    const int c4 = 4;
    const int cx = (nx + c4 - 1) / c4 + 1, cy = (ny + c4 - 1) / c4 + 1, cz = (nz + c4 - 1) / c4 + 1;
    std::vector<float> coarse(std::size_t(cx) * std::size_t(cy) * std::size_t(cz));
    for (int k = 0; k < cz; ++k)
        for (int j = 0; j < cy; ++j)
            for (int i = 0; i < cx; ++i)
                coarse[(std::size_t(k) * std::size_t(cy) + std::size_t(j)) * std::size_t(cx) + std::size_t(i)] =
                    float(f(bx0 + i * c4 * cell, by0 + j * c4 * cell, bz0 + k * c4 * cell));
    const double band = c4 * cell * 0.5 * std::sqrt(3.0) * 1.25;
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const int ci = (i + c4 / 2) / c4, cj = (j + c4 / 2) / c4, ck = (k + c4 / 2) / c4;
                const float dc = coarse[(std::size_t(ck) * std::size_t(cy) + std::size_t(cj)) * std::size_t(cx) + std::size_t(ci)];
                val[node(i, j, k)] =
                    std::fabs(dc) > band ? dc : float(f(bx0 + i * cell, by0 + j * cell, bz0 + k * cell));
            }
    // Surface nets: one vertex per cell the surface crosses, at the mean of its edge crossings, pulled onto the surface.
    std::vector<int> cellVert(val.size(), -1);
    std::vector<std::array<float, 3>> pos, nrm;
    const double eps = cell * 0.25;
    auto grad = [&](double x, double y, double z) {
        std::array<double, 3> g{f(x + eps, y, z) - f(x - eps, y, z), f(x, y + eps, z) - f(x, y - eps, z),
                                f(x, y, z + eps) - f(x, y, z - eps)};
        const double l = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (l > 1e-12)
            for (double& c : g) c /= l;
        return g;
    };
    static const int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k + 1 < nz; ++k)
        for (int j = 0; j + 1 < ny; ++j)
            for (int i = 0; i + 1 < nx; ++i) {
                float v[8];
                int inside = 0;
                for (int c = 0; c < 8; ++c) {
                    v[c] = val[node(i + kCorner[c][0], j + kCorner[c][1], k + kCorner[c][2])];
                    inside += v[c] < 0 ? 1 : 0;
                }
                if (inside == 0 || inside == 8) continue;
                double sx = 0, sy = 0, sz = 0;
                int n = 0;
                for (const auto& e : kEdge) {
                    const float a = v[e[0]], b = v[e[1]];
                    if ((a < 0) == (b < 0)) continue;
                    const double t = a / double(a - b);
                    sx += kCorner[e[0]][0] + (kCorner[e[1]][0] - kCorner[e[0]][0]) * t;
                    sy += kCorner[e[0]][1] + (kCorner[e[1]][1] - kCorner[e[0]][1]) * t;
                    sz += kCorner[e[0]][2] + (kCorner[e[1]][2] - kCorner[e[0]][2]) * t;
                    ++n;
                }
                double px = bx0 + (i + sx / n) * cell, py = by0 + (j + sy / n) * cell, pz = bz0 + (k + sz / n) * cell;
                for (int it = 0; it < 1; ++it) {  // onto the surface, staying in the cell
                    const double d = f(px, py, pz);
                    const auto g = grad(px, py, pz);
                    px = std::clamp(px - d * g[0], bx0 + i * cell, bx0 + (i + 1) * cell);
                    py = std::clamp(py - d * g[1], by0 + j * cell, by0 + (j + 1) * cell);
                    pz = std::clamp(pz - d * g[2], bz0 + k * cell, bz0 + (k + 1) * cell);
                }
                const auto g = grad(px, py, pz);
                cellVert[node(i, j, k)] = int(pos.size());
                pos.push_back({float(px), float(py), float(pz)});
                nrm.push_back({float(g[0]), float(g[1]), float(g[2])});
            }
    auto emit = [&](int a) {
        MeshVertex mv{};
        for (int c = 0; c < 3; ++c) {
            mv.pos[c] = pos[std::size_t(a)][std::size_t(c)];
            mv.normal[c] = nrm[std::size_t(a)][std::size_t(c)];
        }
        mv.edge = 1.0f;
        m.vertices.push_back(mv);
    };
    auto dist2 = [&](int a, int b) {
        double s = 0;
        for (int c = 0; c < 3; ++c) {
            const double d = pos[std::size_t(a)][std::size_t(c)] - pos[std::size_t(b)][std::size_t(c)];
            s += d * d;
        }
        return s;
    };
    const int dims[3] = {nx, ny, nz};
    for (int k = 1; k + 1 < nz; ++k)
        for (int j = 1; j + 1 < ny; ++j)
            for (int i = 1; i + 1 < nx; ++i) {
                const int at[3] = {i, j, k};
                const float v0 = val[node(i, j, k)];
                for (int a = 0; a < 3; ++a) {
                    int o[3] = {i, j, k};
                    o[a] += 1;
                    if (o[a] >= dims[a]) continue;
                    const float v1 = val[node(o[0], o[1], o[2])];
                    if ((v0 < 0) == (v1 < 0)) continue;
                    const int b = (a + 1) % 3, c = (a + 2) % 3;
                    int q[4];
                    const int offs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                    bool ok = true;
                    for (int r = 0; r < 4; ++r) {
                        int p[3] = {at[0], at[1], at[2]};
                        p[b] -= offs[r][0];
                        p[c] -= offs[r][1];
                        q[r] = cellVert[node(p[0], p[1], p[2])];
                        ok = ok && q[r] >= 0;
                    }
                    if (!ok) continue;
                    // (0,0) -> (-1,0) -> (-1,-1) -> (0,-1) in the (b, c) plane turns counter-clockwise: normal +a,
                    // outward when the low end is inside.
                    if (!(v0 < 0)) std::swap(q[1], q[3]);
                    if (dist2(q[0], q[2]) <= dist2(q[1], q[3])) {
                        emit(q[0]), emit(q[1]), emit(q[2]);
                        emit(q[0]), emit(q[2]), emit(q[3]);
                    } else {
                        emit(q[0]), emit(q[1]), emit(q[3]);
                        emit(q[1]), emit(q[2]), emit(q[3]);
                    }
                }
            }
    return m;
}

}  // namespace

Mesh makePiece(double cell) {
    // The default piece is built once per process (~50 ms) and copied.
    if (cell == 0.0125) {
        static const Mesh piece = buildPiece(cell);
        return piece;
    }
    return buildPiece(cell);
}

namespace {
MeshVertex flatVertex(double x, double y, double u, double v, double edge) {
    MeshVertex mv{};
    mv.pos[0] = float(x);
    mv.pos[1] = float(y);
    mv.normal[2] = 1.0f;
    mv.uv[0] = float(u);
    mv.uv[1] = float(v);
    mv.edge = float(edge);
    return mv;
}
}  // namespace

Mesh makeBoard() {
    Mesh m;
    m.kind = MeshKind::Knight;
    m.convex = false;
    for (int s = 0; s < kSquares; ++s) {
        const double cx = squareX(s), cy = squareY(s);
        const double light = ((s / kSide + s % kSide) % 2 == 1) ? 1.0 : 0.0;  // a1 (0, 0) is dark
        const double xs[4] = {-0.5, 0.5, 0.5, -0.5}, ys[4] = {-0.5, -0.5, 0.5, 0.5};
        for (int q : {0, 1, 2, 0, 2, 3})
            m.vertices.push_back(flatVertex(cx + xs[q], cy + ys[q], xs[q] + 0.5, ys[q] + 0.5, light));
    }
    return m;
}

Mesh makeSegmentQuad() {
    Mesh m;
    m.kind = MeshKind::Knight;
    m.convex = false;
    const double xs[4] = {0, 1, 1, 0}, ys[4] = {-0.5, -0.5, 0.5, 0.5};
    for (int q : {0, 1, 2, 0, 2, 3}) m.vertices.push_back(flatVertex(xs[q], ys[q], xs[q], ys[q] + 0.5, 1.0));
    return m;
}

Mesh makeDisc(int segments, double inner) {
    Mesh m;
    m.kind = MeshKind::Knight;
    m.convex = false;
    segments = std::clamp(segments, 3, 256);
    inner = std::clamp(inner, 0.0, 0.99);
    for (int i = 0; i < segments; ++i) {
        const double a0 = 6.283185307179586 * i / segments, a1 = 6.283185307179586 * (i + 1) / segments;
        const double c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        if (inner <= 0.0) {
            m.vertices.push_back(flatVertex(0, 0, 0, 0, 1));
            m.vertices.push_back(flatVertex(c0, s0, 1, 0, 1));
            m.vertices.push_back(flatVertex(c1, s1, 1, 0, 1));
        } else {
            const MeshVertex a = flatVertex(inner * c0, inner * s0, inner, 0, 1), b = flatVertex(c0, s0, 1, 0, 1),
                             c = flatVertex(c1, s1, 1, 0, 1), d = flatVertex(inner * c1, inner * s1, inner, 0, 1);
            for (const MeshVertex& v : {a, b, c, a, c, d}) m.vertices.push_back(v);
        }
    }
    return m;
}

}  // namespace reforce::engine::knight
