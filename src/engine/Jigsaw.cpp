#include "engine/Jigsaw.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::engine::jigsaw {

// The shared cut (JigsawShape.inc) compiled as C++: a float2 and the few Metal functions it uses.
namespace shape {
using uint = uint32_t;
struct float2 {
    float x = 0, y = 0;
    float2() = default;
    float2(float x_, float y_) : x(x_), y(y_) {}
};
inline float2 operator+(float2 a, float2 b) { return {a.x + b.x, a.y + b.y}; }
inline float2 operator-(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
inline float2 operator/(float2 a, float k) { return {a.x / k, a.y / k}; }
inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
inline float length(float2 a) { return std::sqrt(dot(a, a)); }
inline float2 abs(float2 a) { return {std::fabs(a.x), std::fabs(a.y)}; }
inline float2 max(float2 a, float2 b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
using std::abs;
using std::clamp;
using std::floor;
using std::max;
using std::min;
using std::sin;
#define JIGSAW_SHAPE_SOURCE(...) __VA_ARGS__
#include "engine/JigsawShape.inc"
#undef JIGSAW_SHAPE_SOURCE
}  // namespace shape

namespace {

shape::JsGrid js(const Grid& g) { return {g.cols, g.rows, g.cw, g.seed}; }

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
double uniform(uint64_t& s) { return double(splitmix(s) >> 11) * (1.0 / 9007199254740992.0); }

double smooth(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * x * (x * (x * 6 - 15) + 10);
}

bool sameConfig(const Config& a, const Config& b) {
    return a.pieces == b.pieces && a.aspect == b.aspect && a.order == b.order && a.cycle == b.cycle && a.brk == b.brk;
}

// Pieces next to `p` (up to 4) into nb; returns how many.
int neighbors(const Grid& g, int p, int* nb) {
    const int c = p % g.cols, r = p / g.cols;
    int n = 0;
    if (c > 0) nb[n++] = p - 1;
    if (c < g.cols - 1) nb[n++] = p + 1;
    if (r > 0) nb[n++] = p - g.cols;
    if (r < g.rows - 1) nb[n++] = p + g.cols;
    return n;
}

bool isBorder(const Grid& g, int p) {
    const int c = p % g.cols, r = p / g.cols;
    return c == 0 || r == 0 || c == g.cols - 1 || r == g.rows - 1;
}

// The solve order: like a person (corners, the border, then inwards where most neighbors are placed), growing from a
// corner, row by row, or random.
void solveOrder(const Grid& g, Order order, uint64_t& rng, std::array<int16_t, kMaxPieces>& seq) {
    const int n = g.count();
    std::array<bool, kMaxPieces> placed{};
    std::array<float, kMaxPieces> tie{};
    for (int p = 0; p < n; ++p) tie[std::size_t(p)] = float(uniform(rng));
    int k = 0;
    auto place = [&](int p) {
        placed[std::size_t(p)] = true;
        seq[std::size_t(k++)] = int16_t(p);
    };
    auto placedNeighbors = [&](int p) {
        int nb[4];
        const int m = neighbors(g, p, nb);
        int c = 0;
        for (int i = 0; i < m; ++i) c += placed[std::size_t(nb[i])] ? 1 : 0;
        return c;
    };
    if (order == Order::Rows) {
        for (int p = 0; p < n; ++p) place(p);
        return;
    }
    if (order == Order::Random) {
        for (int p = 0; p < n; ++p) seq[std::size_t(p)] = int16_t(p);
        for (int i = n - 1; i > 0; --i)
            std::swap(seq[std::size_t(i)], seq[std::size_t(splitmix(rng) % uint64_t(i + 1))]);
        return;
    }
    const int corners[4] = {0, g.cols - 1, n - g.cols, n - 1};
    // Greedy: the unplaced piece (among those `allowed`) with the most placed neighbors, then the best score.
    auto greedy = [&](auto allowed, auto score) {
        while (true) {
            int best = -1;
            float bestScore = -1e30f;
            for (int p = 0; p < n; ++p) {
                if (placed[std::size_t(p)] || !allowed(p)) continue;
                const float s = float(placedNeighbors(p)) * 100.0f + score(p);
                if (s > bestScore) {
                    bestScore = s;
                    best = p;
                }
            }
            if (best < 0) return;
            place(best);
        }
    };
    if (order == Order::Corner) {
        const int start = corners[splitmix(rng) % 4];
        const int sc = start % g.cols, sr = start / g.cols;
        place(start);
        greedy([](int) { return true; }, [&](int p) {
            const float dc = float(p % g.cols - sc) * g.cw, dr = float(p / g.cols - sr);
            return -std::sqrt(dc * dc + dr * dr) * 3.0f + tie[std::size_t(p)] * 2.0f;
        });
        return;
    }
    // Edges: the four corners (random order), the border growing from them, then the inside.
    std::array<int, 4> cs{corners[0], corners[1], corners[2], corners[3]};
    for (int i = 3; i > 0; --i) std::swap(cs[std::size_t(i)], cs[std::size_t(splitmix(rng) % uint64_t(i + 1))]);
    for (int c : cs)
        if (!placed[std::size_t(c)]) place(c);
    greedy([&](int p) { return isBorder(g, p); }, [&](int p) { return tie[std::size_t(p)] * 50.0f; });
    greedy([](int) { return true; }, [&](int p) { return tie[std::size_t(p)] * 50.0f; });
}

}  // namespace

uint32_t cutSeed(uint64_t seed) {
    uint64_t s = seed ^ 0x6A09E667F3BCC909ull;
    return uint32_t(splitmix(s) >> 32);
}

Grid fitGrid(int pieces, double aspect, uint64_t seed) {
    if (!std::isfinite(aspect)) aspect = 16.0 / 9.0;
    aspect = std::clamp(aspect, 0.25, 4.0);
    const int n = std::clamp(pieces, 2, kMaxPieces);
    Grid g;
    g.rows = std::clamp(int(std::lround(std::sqrt(double(n) / aspect))), 1, kMaxRows);
    g.cols = std::clamp(int(std::lround(double(n) / double(g.rows))), 1, kMaxCols);
    while (g.cols * g.rows > kMaxPieces) --g.cols;
    if (g.cols * g.rows < 2) g.cols = 2;
    g.cw = float(aspect * double(g.rows) / double(g.cols));
    g.seed = cutSeed(seed);
    return g;
}

Point corner(const Grid& g, int i, int j) {
    const auto c = shape::js_corner(js(g), i, j);
    return {c.x, c.y};
}

float edgeField(const Grid& g, bool horizontal, int i, int j, Point p) {
    return shape::js_edge(js(g), horizontal ? 1 : 0, i, j, {p.x, p.y});
}

float pieceField(const Grid& g, int col, int row, Point p) { return shape::js_piece(js(g), col, row, {p.x, p.y}); }

Owner owner(const Grid& g, Point p) {
    const shape::JsHit h = shape::js_owner(js(g), {p.x, p.y});
    Owner o;
    if (h.index >= 0 && h.g < 0) {
        o.index = h.index;
        o.g = h.g;
        o.index2 = h.index2;
        o.g2 = h.g2;
    }
    return o;
}

Point homeCenter(const Grid& g, int index) {
    const int c = index % g.cols, r = index / g.cols;
    Point s;
    for (int k = 0; k < 4; ++k) {
        const Point q = corner(g, c + (k & 1), r + (k >> 1));
        s.x += 0.25f * q.x;
        s.y += 0.25f * q.y;
    }
    return s;
}

int knobSide(const Grid& g, bool horizontal, int i, int j) {
    if (horizontal ? (j <= 0 || j >= g.rows) : (i <= 0 || i >= g.cols)) return 0;
    const uint32_t salt = horizontal ? 16u : 48u;
    return shape::js_rand(g.seed, uint32_t(i), uint32_t(j), salt + 3u) < 0.5f ? -1 : 1;
}

Config clampConfig(Config c) {
    c.pieces = std::clamp(c.pieces, 2, kMaxPieces);
    if (!std::isfinite(c.aspect)) c.aspect = 16.0 / 9.0;
    c.aspect = std::clamp(c.aspect, 0.25, 4.0);
    if (!std::isfinite(c.cycle)) c.cycle = 32;
    c.cycle = std::clamp(std::round(c.cycle), 4.0, 256.0);
    if (!std::isfinite(c.brk)) c.brk = 4;
    c.brk = std::clamp(std::round(c.brk), 0.0, c.cycle - 2);
    const int o = int(c.order);
    if (o < 0 || o > 3) c.order = Order::Edges;
    return c;
}

double landingBeat(int k, int n, int first, int last) {
    if (last < first) last = first;
    const int m = last - first + 1;
    if (n <= 1) return last;
    k = std::clamp(k, 0, n - 1);
    int slot;
    if (n >= m) {
        const int base = n / m, extra = n % m;
        slot = k < extra * (base + 1) ? k / (base + 1) : extra + (k - extra * (base + 1)) / base;
    } else {
        slot = int(std::lround(double(k) * double(m - 1) / double(n - 1)));
    }
    return double(first + std::clamp(slot, 0, m - 1));
}

void makePlan(Plan& out, uint64_t seed, const Config& cin) {
    const Config c = clampConfig(cin);
    out.config = c;
    out.seed = seed;
    out.grid = fitGrid(c.pieces, c.aspect, seed);
    const Grid& g = out.grid;
    const int n = g.count();
    out.count = n;
    uint64_t rng = seed * 0xD1342543DE82EF95ull + 0x7A11ull;
    solveOrder(g, c.order, rng, out.sequence);
    const int first = int(c.brk) + 2, last = int(c.cycle);
    for (int k = 0; k < n; ++k)
        out.land[std::size_t(out.sequence[std::size_t(k)])] = float(landingBeat(k, n, first, last));
    for (int p = 0; p < n; ++p) out.rnd[std::size_t(p)] = float(uniform(rng));

    // The pile: each piece rests over another piece's slot, still empty, with as few others on it as possible and
    // preferably one that fills late; it hops aside when that slot is about to be filled.
    std::array<int, kMaxPieces> occ{}, rest{};
    auto choose = [&](int piece, double after) {
        int best = -1;
        double bestScore = 1e30;
        for (int s = 0; s < n; ++s) {
            if (s == piece || !(out.land[std::size_t(s)] > after)) continue;
            const double score =
                occ[std::size_t(s)] * 10.0 - (out.land[std::size_t(s)] - after) * 0.3 + uniform(rng) * 1.5;
            if (score < bestScore) {
                bestScore = score;
                best = s;
            }
        }
        return best;
    };
    auto spotOver = [&](int slot, float t0, float t1) {
        const Point h = homeCenter(g, slot);
        Spot s;
        s.t0 = t0;
        s.t1 = t1;
        s.x = h.x + float((uniform(rng) - 0.5) * 0.4) * g.cw;
        s.y = h.y + float((uniform(rng) - 0.5) * 0.4);
        const double a = 0.08 + 0.34 * uniform(rng);
        s.angle = float(uniform(rng) < 0.5 ? -a : a);
        s.z = float(0.03 + 0.07 * uniform(rng));
        return s;
    };
    std::array<int16_t, kMaxPieces> shuffled{};
    for (int p = 0; p < n; ++p) shuffled[std::size_t(p)] = int16_t(p);
    for (int i = n - 1; i > 0; --i)
        std::swap(shuffled[std::size_t(i)], shuffled[std::size_t(splitmix(rng) % uint64_t(i + 1))]);
    const float burstEnd = float(c.brk + kBurstBeats);
    for (int i = 0; i < n; ++i) {
        const int p = shuffled[std::size_t(i)];
        int s = choose(p, c.brk);
        if (s < 0) s = (p + 1) % n;  // (two pieces: the other one)
        out.spots[std::size_t(p)][0] = spotOver(s, float(c.brk), burstEnd);
        out.spotCount[std::size_t(p)] = 1;
        rest[std::size_t(p)] = s;
        ++occ[std::size_t(s)];
    }
    for (int k = 0; k < n; ++k) {
        const int p = out.sequence[std::size_t(k)];
        const double L = out.land[std::size_t(p)];
        if (rest[std::size_t(p)] >= 0) --occ[std::size_t(rest[std::size_t(p)])];
        rest[std::size_t(p)] = -1;
        for (int q = 0; q < n; ++q) {
            if (rest[std::size_t(q)] != p) continue;
            if (out.land[std::size_t(q)] <= L + 0.5) continue;  // it flies home itself on this beat
            if (out.spotCount[std::size_t(q)] >= kMaxSpots) continue;
            const int s = choose(q, L + 0.5);
            if (s < 0) continue;  // nowhere empty left: it stays (only near the end)
            auto& list = out.spots[std::size_t(q)];
            const Spot& prevSpot = list[std::size_t(out.spotCount[std::size_t(q)] - 1)];
            const float t1 = float(L - 0.1);
            const float t0 = std::max(prevSpot.t1, float(t1 - kHopBeats));
            list[std::size_t(out.spotCount[std::size_t(q)]++)] = spotOver(s, t0, std::max(t1, t0 + 0.05f));
            --occ[std::size_t(p)];
            ++occ[std::size_t(s)];
            rest[std::size_t(q)] = s;
        }
    }
}

void frameAt(const Plan& cur, const Plan& prev, const Input& in, Frame& out) {
    const Config& c = cur.config;
    const double C = c.cycle, B = c.brk;
    const double kick = std::isfinite(in.kick) ? std::clamp(in.kick, 0.0, 1.0) : 0.0;
    const double shake = std::isfinite(in.shake) ? std::clamp(in.shake, 0.0, 1.0) : 0.0;
    const bool absent = !std::isfinite(in.turn);
    const double tau = absent ? 0.0 : std::clamp(in.turn, 0.0, C);
    const bool wholePhase = absent || tau < B;
    const Plan& plan = wholePhase ? prev : cur;
    out.grid = plan.grid;
    const Grid& g = out.grid;
    const int n = plan.count;
    out.count = n;
    out.loose = 0;
    out.glow = 0;
    out.seam = 1;
    if (wholePhase) {
        // Completed on the line: the seams flash, then fade out until the break.
        const double fadeEnd = std::min(2.0, B), fadeStart = 0.25 * fadeEnd;
        out.seam = absent ? 0.0f : float(1.0 - smooth((tau - fadeStart) / std::max(1e-3, fadeEnd - fadeStart)));
        out.glow = absent ? 0.0f : float(std::exp(-2.5 * tau));
        for (int p = 0; p < n; ++p) {
            Piece& pc = out.pieces[std::size_t(p)];
            const Point h = homeCenter(g, p);
            pc = Piece{};
            pc.x = h.x;
            pc.y = h.y;
            const bool last = plan.land[std::size_t(p)] >= float(plan.config.cycle) - 0.01f;
            pc.flash = absent || !last ? 0.0f : float(std::exp(-3.5 * tau));
        }
    } else {
        if (tau >= C - 1e-9) out.glow = 1.0f;
        for (int p = 0; p < n; ++p) {
            Piece& pc = out.pieces[std::size_t(p)];
            pc = Piece{};
            const Point h = homeCenter(g, p);
            const double L = plan.land[std::size_t(p)];
            const double F = L - kFlightBeats;
            const float r = plan.rnd[std::size_t(p)];
            pc.x = h.x;
            pc.y = h.y;
            if (tau >= L) {
                pc.flash = float(std::exp(-3.5 * (tau - L)));
                continue;
            }
            pc.landed = false;
            const auto& list = plan.spots[std::size_t(p)];
            const int cnt = std::max(1, int(plan.spotCount[std::size_t(p)]));
            const double tq = std::min(tau, F);
            int s = 0;
            while (s + 1 < cnt && list[std::size_t(s + 1)].t0 <= tq) ++s;
            const Spot& sp = list[std::size_t(s)];
            float x = sp.x, y = sp.y, a = sp.angle, z = sp.z, tx = 0, ty = 0;
            bool resting = true;
            if (s == 0 && tq < sp.t1) {
                // Breaking apart: a burst out of the frame, fast at first, high in the middle so pieces pass over
                // each other, a little staggered.
                const double xb = std::clamp((tq - sp.t0 - 0.15 * r) / (kBurstBeats - 0.15), 0.0, 1.0);
                const double e = 1.0 - std::pow(1.0 - xb, 3.0);
                x = float(h.x + (sp.x - h.x) * e);
                y = float(h.y + (sp.y - h.y) * e);
                a = float(sp.angle * e + 0.5 * (r - 0.5) * std::sin(M_PI * xb));
                z = float(sp.z * e + (0.35 + 0.35 * r) * std::sin(M_PI * std::pow(xb, 0.6)));
                const float dx = sp.x - h.x, dy = sp.y - h.y, dl = std::max(1e-4f, std::hypot(dx, dy));
                const float amt = float(0.3 * std::sin(M_PI * xb));
                tx = -dy / dl * amt;
                ty = dx / dl * amt;
                resting = false;
            } else if (s > 0 && tq < sp.t1) {
                // Making room: a short hop to another empty slot.
                const Spot& from = list[std::size_t(s - 1)];
                const double xh = std::clamp((tq - sp.t0) / std::max(1e-3f, sp.t1 - sp.t0), 0.0, 1.0);
                const double e = smooth(xh);
                x = float(from.x + (sp.x - from.x) * e);
                y = float(from.y + (sp.y - from.y) * e);
                a = float(from.angle + (sp.angle - from.angle) * e);
                z = float(from.z + (sp.z - from.z) * e + 0.12 * std::sin(M_PI * xh));
                resting = false;
            }
            if (tau >= F) {
                // Home: lift, glide on an arc, turn upright, drop and snap in on the beat.
                const double xf = std::clamp((tau - F) / kFlightBeats, 0.0, 1.0);
                const double eh = smooth(xf / 0.8), ea = smooth(xf / 0.7);
                const double xp = std::pow(xf, 0.7);
                const float sx = x, sy = y;
                x = float(sx + (h.x - sx) * eh);
                y = float(sy + (h.y - sy) * eh);
                a = float(a * (1.0 - ea));
                z = float(z * (1.0 - xf) + 0.32 * 4.0 * xp * (1.0 - xp));
                const float dx = h.x - sx, dy = h.y - sy, dl = std::max(1e-4f, std::hypot(dx, dy));
                const float amt = float(0.18 * std::sin(M_PI * xf));
                tx = -dy / dl * amt;
                ty = dx / dl * amt;
                resting = false;
            }
            if (resting && shake > 0) {
                const double ph = r * 6.2831853, w = 6.2831853 * 2.0;
                a += float(shake * 0.1 * std::sin(w * tau + ph));
                x += float(shake * 0.025 * std::cos(1.3 * w * tau + ph));
                y += float(shake * 0.025 * std::sin(w * tau + 2.0 * ph));
                z += float(shake * 0.03);
            }
            pc.x = x;
            pc.y = y;
            pc.angle = a;
            pc.z = std::max(0.0f, z);
            pc.tiltX = tx;
            pc.tiltY = ty;
        }
    }
    // A drop: the picture pops apart, every piece out from the middle, lifted and turned, settling as Kick falls.
    if (kick > 1e-3) {
        const float cx = 0.5f * g.width(), cy = 0.5f * float(g.rows);
        for (int p = 0; p < n; ++p) {
            Piece& pc = out.pieces[std::size_t(p)];
            const float r = plan.rnd[std::size_t(p)];
            const float r2 = std::fmod(r * 7.13f + 0.31f, 1.0f), r3 = std::fmod(r * 13.7f + 0.57f, 1.0f);
            const Point h = homeCenter(g, p);
            float dx = h.x - cx + (r2 - 0.5f) * 0.6f, dy = h.y - cy + (r3 - 0.5f) * 0.6f;
            const float dl = std::max(1e-3f, std::hypot(dx, dy));
            dx /= dl;
            dy /= dl;
            const float k = float(kick);
            pc.x += dx * k * (0.25f + 0.35f * r);
            pc.y += dy * k * (0.25f + 0.35f * r);
            pc.z += k * (0.15f + 0.25f * r2);
            pc.angle += k * (r3 - 0.5f) * 0.8f;
            pc.landed = false;
        }
    }
    // The loose pieces, bottom to top.
    bool any = out.glow > 1e-3f || out.seam > 1e-3f;
    for (int p = 0; p < n; ++p) {
        const Piece& pc = out.pieces[std::size_t(p)];
        any = any || pc.flash > 1e-3f;
        if (pc.landed) continue;
        any = true;
        int i = out.loose++;
        while (i > 0 && out.pieces[std::size_t(out.order[std::size_t(i - 1)])].z > pc.z) {
            out.order[std::size_t(i)] = out.order[std::size_t(i - 1)];
            --i;
        }
        out.order[std::size_t(i)] = int16_t(p);
    }
    out.whole = !any;
}

const Frame& Solver::frame(uint64_t seed, const Config& cin, const Input& in) {
    const Config c = clampConfig(cin);
    if (!haveCur_ || cur_.seed != seed || !sameConfig(cur_.config, c)) {
        if (haveCur_ && cur_.seed + 1 == seed && sameConfig(cur_.config, c)) {
            prev_ = cur_;
            havePrev_ = true;
        }
        makePlan(cur_, seed, c);
        haveCur_ = true;
    }
    if (!havePrev_ || prev_.seed != seed - 1 || !sameConfig(prev_.config, c)) {
        makePlan(prev_, seed - 1, c);
        havePrev_ = true;
    }
    frameAt(cur_, prev_, in, frame_);
    return frame_;
}

}  // namespace reforce::engine::jigsaw
