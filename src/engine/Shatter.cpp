#include "engine/Shatter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace reforce::engine::shatter {

// The shared fracture (ShatterShape.inc) compiled as C++: plain scalar math, no address spaces.
namespace shape {
using uint = uint32_t;
using std::abs;
using std::atan2;
using std::clamp;
using std::cos;
using std::exp;
using std::floor;
using std::log;
using std::max;
using std::min;
using std::sin;
using std::sqrt;
#define SH_CONST const
#define SHATTER_SHAPE_SOURCE(...) __VA_ARGS__
#include "engine/ShatterShape.inc"
#undef SHATTER_SHAPE_SOURCE
#undef SH_CONST
}  // namespace shape

static_assert(sizeof(shape::ShImpact) == sizeof(Impact) && sizeof(shape::ShPane) == sizeof(Pane));
static_assert(offsetof(shape::ShPane, count) == offsetof(Pane, count));
static_assert(offsetof(shape::ShImpact, seed) == offsetof(Impact, seed));

namespace {

constexpr double kPi = 3.141592653589793;

uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct Rng {
    uint64_t s;
    double next() { return double(mix64(s++) >> 11) * (1.0 / 9007199254740992.0); }
};

double finiteOr(double v, double fb) { return std::isfinite(v) ? v : fb; }

const shape::ShImpact& sh(const Impact& m) { return reinterpret_cast<const shape::ShImpact&>(m); }

// Spoke position q (0..4; 4 = base spoke k + 1) of base sector k at log radius u (relative to rot).
float spokeAngle(const Impact& im, int k, int q, float u) {
    const shape::ShImpact& m = sh(im);
    const float a0 = shape::sh_base(m, k, u);
    const float a4 = shape::sh_base(m, k + 1, u);
    if (q == 0) return a0;
    if (q == 4) return a4;
    const int k1 = k + 1 >= m.K ? 0 : k + 1;
    const int b2 = shape::sh_birth(m, k, 2);
    const float a2 = a0 + (a4 - a0) * shape::sh_forkFrac(m, k, 2, b2, k * 4, k1 * 4, u);
    if (q == 2) return a2;
    if (q == 1) return a0 + (a2 - a0) * shape::sh_forkFrac(m, k, 1, shape::sh_birth(m, k, 1), k * 4, k * 4 + 2, u);
    return a2 + (a4 - a2) * shape::sh_forkFrac(m, k, 3, shape::sh_birth(m, k, 3), k * 4 + 2, k1 * 4, u);
}

bool spokeExists(const Impact& im, int k, int q, int band) {
    if (q == 0 || q == 4) return true;
    return band >= shape::sh_birth(sh(im), k, q);
}

// The next spoke position after q that exists in band j (4: base spoke k + 1).
int nextSpoke(const Impact& im, int k, int q, int band) {
    for (int n = q + 1; n < 4; ++n)
        if (spokeExists(im, k, n, band)) return n;
    return 4;
}

struct P2 {
    double x = 0, y = 0;
};

// A polar point of impact m (radius, angle from rot) in pane units.
P2 polar(const Impact& m, double r, double phi) {
    return {m.cx + r * std::cos(double(m.rot) + phi), m.cy + r * std::sin(double(m.rot) + phi)};
}

constexpr int kPoly = 96;
struct Poly {
    std::array<P2, kPoly> p;
    int n = 0;
    void add(P2 q) {
        if (n < kPoly) p[std::size_t(n++)] = q;
    }
};

// Keeps the part of `in` where a x + b y <= c (Sutherland-Hodgman).
void clip(const Poly& in, double a, double b, double c, Poly& out) {
    out.n = 0;
    if (in.n == 0) return;
    for (int i = 0; i < in.n; ++i) {
        const P2 P = in.p[std::size_t(i)];
        const P2 Q = in.p[std::size_t((i + 1) % in.n)];
        const double fp = a * P.x + b * P.y - c, fq = a * Q.x + b * Q.y - c;
        if (fp <= 0) out.add(P);
        if ((fp <= 0) != (fq <= 0)) {
            const double t = fp / (fp - fq);
            out.add({P.x + (Q.x - P.x) * t, P.y + (Q.y - P.y) * t});
        }
    }
}

double polyArea(const Poly& g, P2& centroid) {
    double A = 0, cx = 0, cy = 0;
    for (int i = 0; i < g.n; ++i) {
        const P2 P = g.p[std::size_t(i)];
        const P2 Q = g.p[std::size_t((i + 1) % g.n)];
        const double c = P.x * Q.y - Q.x * P.y;
        A += c;
        cx += (P.x + Q.x) * c;
        cy += (P.y + Q.y) * c;
    }
    A *= 0.5;
    if (std::fabs(A) > 1e-12) {
        centroid = {cx / (6 * A), cy / (6 * A)};
    } else if (g.n > 0) {
        centroid = {};
        for (int i = 0; i < g.n; ++i) {
            centroid.x += g.p[std::size_t(i)].x / g.n;
            centroid.y += g.p[std::size_t(i)].y / g.n;
        }
    }
    return std::fabs(A);
}

// Ring jj of impact im at fraction t across the leaf (k, p..pR) of band jj - 1 or jj (whichever the leaf is in):
// its polar point. jj = 0: the center; jj = nb: far outside the frame.
void ringPoint(const Impact& im, int k, int p, int pR, int jj, double t, double farR, double& r, double& phi) {
    const shape::ShImpact& m = sh(im);
    t = std::clamp(t, 0.002, 0.998);
    if (jj <= 0) {
        r = 0;
        phi = 0;
        return;
    }
    if (jj >= m.nb) {
        r = farR;
        const float u = std::log(float(r));
        phi = spokeAngle(im, k, p, u) + t * (spokeAngle(im, k, pR, u) - spokeAngle(im, k, p, u));
        return;
    }
    r = double(m.r0) * std::exp(double(m.lg) * (jj - 1));
    for (int it = 0; it < 3; ++it) {
        const float u = std::log(std::max(float(r), m.r0 * 0.05f));
        const float a = spokeAngle(im, k, p, u), b = spokeAngle(im, k, pR, u);
        phi = a + t * (b - a);
        r = shape::sh_ring(m, k, jj, u, float(phi)).r;
    }
}

// The leaf's outline (inner ring, right spoke, outer ring, left spoke), sampled.
void leafOutline(const Impact& im, int j, int k, int p, int pR, double farR, Poly& out) {
    out.n = 0;
    constexpr int kRing = 6, kSpoke = 5;
    double ri[2], ro[2], dummy;
    ringPoint(im, k, p, pR, j, 0.0, farR, ri[0], dummy);
    ringPoint(im, k, p, pR, j, 1.0, farR, ri[1], dummy);
    ringPoint(im, k, p, pR, j + 1, 0.0, farR, ro[0], dummy);
    ringPoint(im, k, p, pR, j + 1, 1.0, farR, ro[1], dummy);
    const float floorR = im.r0 * 0.05f;
    // Spokes kink every third of a log-radius unit: a sample every .1 of one.
    auto spoke = [&](int q, double ra, double rb, bool forward) {
        const double la = std::log(std::max(ra, double(floorR))), lb = std::log(std::max(rb, double(floorR)));
        const int ns = std::clamp(int(std::ceil(std::fabs(lb - la) / 0.1)), kSpoke, 24);
        for (int s = 1; s < ns; ++s) {
            const double f = forward ? double(s) / ns : 1.0 - double(s) / ns;
            const double r = std::exp(la + (lb - la) * f);
            out.add(polar(im, r, spokeAngle(im, k, q, std::log(std::max(float(r), floorR)))));
        }
    };
    if (j == 0) {
        out.add({im.cx, im.cy});
    } else {
        for (int s = 0; s <= kRing; ++s) {
            double r, phi;
            ringPoint(im, k, p, pR, j, double(s) / kRing, farR, r, phi);
            out.add(polar(im, r, phi));
        }
    }
    spoke(pR, ri[1], ro[1], true);
    for (int s = kRing; s >= 0; --s) {
        double r, phi;
        ringPoint(im, k, p, pR, j + 1, double(s) / kRing, farR, r, phi);
        out.add(polar(im, r, phi));
    }
    spoke(p, ri[0], ro[0], false);
}

// The shard table of a pane: every leaf of every impact with a part on the pane. Returns the count, or more than
// kMaxShards (stops counting there) when the web is too dense.
int buildShards(Plan& pl) {
    pl.slotShard.fill(-1);
    pl.shardCount = 0;
    const Pane& pn = pl.pane;
    const double A = pl.aspect;
    constexpr double mg = 0.003;   // inclusion margin (the outline is sampled)
    constexpr double box = 0.012;  // the quad's margin around the sampled outline
    Poly a, b;
    for (int i = 0; i < pn.count; ++i) {
        const Impact& im = pn.imp[i];
        double farR = 0;  // beyond every corner of the frame
        for (const P2 q : {P2{0, 0}, P2{A, 0}, P2{0, 1}, P2{A, 1}}) farR = std::max(farR, std::hypot(q.x - im.cx, q.y - im.cy));
        farR = farR * 1.15 + 0.05;
        for (int j = 0; j < im.nb; ++j)
            for (int k = 0; k < im.K; ++k)
                for (int p = 0; p < 4; ++p) {
                    if (!spokeExists(im, k, p, j)) continue;
                    const int pR = nextSpoke(im, k, p, j);
                    leafOutline(im, j, k, p, pR, farR, a);
                    // Clip to the frame and the impact's Voronoi cell (a margin out).
                    clip(a, -1, 0, mg, b);
                    clip(b, 1, 0, A + mg, a);
                    clip(a, 0, -1, mg, b);
                    clip(b, 0, 1, 1 + mg, a);
                    for (int o = 0; o < pn.count && a.n > 0; ++o) {
                        if (o == i) continue;
                        const double ex = pn.imp[o].cx - im.cx, ey = pn.imp[o].cy - im.cy;
                        const double mx = 0.5 * (pn.imp[o].cx + im.cx), my = 0.5 * (pn.imp[o].cy + im.cy);
                        const double el = std::max(std::hypot(ex, ey), 1e-9);
                        clip(a, ex, ey, ex * mx + ey * my + (mg + 0.013) * el, b);  // (the bisector's wander)
                        a = b;
                    }
                    P2 c;
                    const double area = polyArea(a, c);
                    if (a.n < 3 || !(area > 1e-10)) continue;
                    if (pl.shardCount >= kMaxShards) return kMaxShards + 1;
                    Shard& s = pl.shards[std::size_t(pl.shardCount)];
                    s = Shard{};
                    s.slot = int16_t(slotOf(i, j, k, p));
                    s.impact = int8_t(i);
                    s.band = int8_t(j);
                    s.cx = float(c.x);
                    s.cy = float(c.y);
                    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
                    for (int v = 0; v < a.n; ++v) {
                        x0 = std::min(x0, a.p[std::size_t(v)].x);
                        x1 = std::max(x1, a.p[std::size_t(v)].x);
                        y0 = std::min(y0, a.p[std::size_t(v)].y);
                        y1 = std::max(y1, a.p[std::size_t(v)].y);
                    }
                    s.x0 = float(std::max(x0, -mg) - c.x - box);
                    s.x1 = float(std::min(x1, A + mg) - c.x + box);
                    s.y0 = float(std::max(y0, -mg) - c.y - box);
                    s.y1 = float(std::min(y1, 1 + mg) - c.y + box);
                    s.area = float(area);
                    Rng r{mix64(pl.seed ^ (uint64_t(uint16_t(s.slot)) * 0x2545F4914F6CDD1Dull))};
                    const double ang = 2 * kPi * r.next(), mag = 0.006 + 0.01 * r.next();
                    s.tiltX = float(mag * std::cos(ang));
                    s.tiltY = float(mag * std::sin(ang));
                    for (auto& v : s.rnd) v = float(r.next());
                    // Its boundary: left spoke, right spoke, inner ring, the outer ring's pieces.
                    const int k1 = k + 1 >= im.K ? 0 : k + 1;
                    s.nbound = 0;
                    auto add = [&](int e) {
                        if (s.nbound < kMaxBoundary) s.bound[std::size_t(s.nbound++)] = int16_t(e);
                    };
                    add(2 * slotOf(i, j, k, p));
                    add(2 * (pR < 4 ? slotOf(i, j, k, pR) : slotOf(i, j, k1, 0)));
                    if (j > 0) add(2 * slotOf(i, j, k, p) + 1);
                    if (j + 1 < im.nb)
                        for (int q = p; q < pR; ++q)
                            if (spokeExists(im, k, q, j + 1)) add(2 * slotOf(i, j + 1, k, q) + 1);
                    pl.slotShard[std::size_t(s.slot)] = int16_t(pl.shardCount);
                    ++pl.shardCount;
                }
    }
    return pl.shardCount;
}

// The spoke piece below this one (the same spoke in band j - 1, or for a fork born in band j the parent spoke it
// branches from); -1 for band 0.
int spokePred(const Impact& im, int i, int j, int k, int p) {
    if (j <= 0) return -1;
    if (spokeExists(im, k, p, j - 1)) return 2 * slotOf(i, j - 1, k, p);
    const bool left = shape::sh_rand(im.seed, uint32_t(k), uint32_t(p), 13u) < 0.5f;
    const int k1 = k + 1 >= im.K ? 0 : k + 1;
    int q;
    int kk = k;
    if (p == 2) {
        q = left ? 0 : 4;
    } else if (p == 1) {
        q = left ? 0 : 2;
    } else {
        q = left ? 2 : 4;
    }
    if (q == 4) {
        q = 0;
        kk = k1;
    }
    return 2 * slotOf(i, j - 1, kk, q);
}

}  // namespace

Hit owner(const Pane& p, float x, float y) {
    const shape::ShHit h = shape::sh_owner(reinterpret_cast<const shape::ShPane&>(p), x, y);
    return {h.slot, h.elem, h.param, h.dist, h.impact, h.band};
}

Config clampConfig(Config c) {
    c.impacts = std::clamp(c.impacts, 1, kMaxImpacts);
    c.cycle = std::clamp(finiteOr(std::round(c.cycle), 64.0), 16.0, 256.0);
    c.hit = std::clamp(finiteOr(std::round(c.hit), 4.0), kArrive + 2.0, c.cycle - 4.0);
    c.spokes = std::clamp(finiteOr(c.spokes, 9.0), 5.0, 12.0);
    c.rings = std::clamp(finiteOr(c.rings, 0.5), 0.0, 1.0);
    c.fork = std::clamp(finiteOr(c.fork, 0.6), 0.0, 1.0);
    c.cracked = std::clamp(finiteOr(c.cracked, 0.8), 0.0, 1.0);
    c.size = std::clamp(finiteOr(c.size, 1.0), 0.3, 3.0);
    c.arrive = std::clamp(c.arrive, 0, 1);
    return c;
}

void makePlan(Plan& pl, uint64_t seed, const Config& cfgIn, double aspect, double length, double first, double bar) {
    const Config c = clampConfig(cfgIn);
    pl.config = c;
    pl.seed = seed;
    pl.aspect = std::isfinite(aspect) ? std::clamp(aspect, 0.25, 4.0) : 16.0 / 9.0;
    pl.length = std::isfinite(length) ? std::clamp(length, 4.0, 512.0) : 62.0;
    first = std::isfinite(first) ? std::clamp(std::round(first), 0.0, pl.length - 2.0) : 2.0;
    bar = std::isfinite(bar) ? bar - 4.0 * std::floor(bar / 4.0) : 0.0;
    const double A = pl.aspect;
    Pane& pn = pl.pane;
    pn = Pane{};
    pn.aspect = float(A);
    pn.seed = uint32_t(mix64(seed));
    Rng r{mix64(seed ^ 0x5A17E5ull)};
    const int n = c.impacts;
    pn.count = n;
    // The impacts: one a little above the middle, several spread apart.
    double dmin = 0.62 * std::sqrt(A / n);
    for (int i = 0; i < n; ++i) {
        Impact& m = pn.imp[i];
        double x = 0, y = 0;
        if (n == 1) {
            x = A * (0.32 + 0.36 * r.next());
            y = 0.3 + 0.35 * r.next();
        } else {
            for (int tries = 0;; ++tries) {
                x = A * (0.08 + 0.84 * r.next());
                y = 0.1 + 0.8 * r.next();
                bool ok = true;
                for (int o = 0; o < i; ++o)
                    ok = ok && std::hypot(x - pn.imp[o].cx, y - pn.imp[o].cy) >= dmin;
                if (ok) break;
                if (tries % 24 == 23) dmin *= 0.9;
            }
        }
        m.cx = float(x);
        m.cy = float(y);
        m.rot = float(2 * kPi * r.next());
        m.seed = uint32_t(mix64(seed * 31 + uint64_t(i) + 1));
        m.K = int32_t(std::clamp(std::lround(c.spokes + (2 * r.next() - 1) * (n > 1 ? 1.5 : 2.0)), 5L, long(kMaxSpokes)));
        const double g = std::max(1.45, 1.95 - 0.5 * c.rings + 0.08 * (2 * r.next() - 1));
        m.lg = float(std::log(g));
        m.r0 = float(c.size * (n == 1 ? 0.04 : 0.026) * (0.85 + 0.3 * r.next()));
        double far = 0;
        for (const P2 q : {P2{0, 0}, P2{A, 0}, P2{0, 1}, P2{A, 1}}) far = std::max(far, std::hypot(q.x - x, q.y - y));
        m.nb = int32_t(std::clamp(int(std::ceil(std::log(far * 1.05 / m.r0) / m.lg)) + 1, 2, kMaxBands));
        m.f1 = std::max(2, int(m.nb / 3));
        m.f2 = m.f1 + 2;
        m.fork = float(c.fork);
    }
    // The shard table; a web too dense for kMaxShards is thinned (fewer forks, then fewer spokes), deterministically.
    for (int tries = 0; buildShards(pl) > kMaxShards && tries < 12; ++tries)
        for (int i = 0; i < n; ++i) {
            Impact& m = pn.imp[i];
            if (m.fork > 0.05f) {
                m.fork *= 0.6f;
            } else if (m.K > 5) {
                --m.K;
            } else {
                m.nb = std::max(2, m.nb - 1);
            }
        }
    if (pl.shardCount > kMaxShards) pl.shardCount = kMaxShards;  // (never reached with 6 impacts of 5 spokes)
    // The impacts' landings: the first on `first`, the others on bar lines spread over the pane's first half or so.
    for (int i = 0; i < n; ++i) {
        double L = first;
        if (i > 0) {
            const double span = std::max(4.0, (pl.length - 8.0 - first) * 0.75);
            L = first + span * double(i) / double(n);
            L = std::ceil((L - bar) / 4.0) * 4.0 + bar;  // the next bar line
            L = std::min(L, pl.length - 2.0);
        }
        pl.land[std::size_t(i)] = float(L);
    }
    // The crack elements on the pane, each impact's in its order: spokes outwards band by band, rings after the spokes
    // around them; some jitter.
    pl.elems.fill(Element{});
    pl.elemCount = 0;
    auto& sKey = pl.scratchKey;
    auto& sOrder = pl.scratchOrder;
    auto keyOf = [&](int e) -> float {
        const int slot = e / 2, band = (slot / 48) % 12;
        const float h = shape::sh_rand(pn.seed, uint32_t(e), 3u, 31u);
        return (e & 1) ? float(band) + 0.55f + 0.9f * h : float(band) + 0.35f * h;
    };
    for (int s = 0; s < pl.shardCount; ++s) {
        const Shard& sd = pl.shards[std::size_t(s)];
        pl.elemList[std::size_t(pl.elemCount++)] = int16_t(2 * sd.slot);
        if (sd.band > 0) pl.elemList[std::size_t(pl.elemCount++)] = int16_t(2 * sd.slot + 1);
    }
    for (int i = 0; i < n; ++i)
        for (int i2 = i + 1; i2 < n; ++i2) pl.elemList[std::size_t(pl.elemCount++)] = int16_t(pairElem(i, i2));
    pl.shown = 0;
    const double lastBeat = std::floor(pl.length - 1.0);  // cracks land up to the beat before the shatter
    for (int i = 0; i < n; ++i) {
        const Impact& im = pn.imp[i];
        int cnt = 0;
        for (int q = 0; q < pl.elemCount; ++q) {
            const int e = pl.elemList[std::size_t(q)];
            if (e >= 2 * kSlots) continue;
            if ((e / 2) / (12 * 12 * 4) != i) continue;
            sOrder[std::size_t(cnt)] = int16_t(e);
            sKey[std::size_t(e)] = keyOf(e);
            ++cnt;
        }
        std::sort(sOrder.begin(), sOrder.begin() + cnt,
                  [&](int16_t x, int16_t y) { return sKey[std::size_t(x)] < sKey[std::size_t(y)] || (sKey[std::size_t(x)] == sKey[std::size_t(y)] && x < y); });
        const double land = pl.land[std::size_t(i)];
        const int show = int(std::lround(c.cracked * cnt));
        int burst = 0;
        while (burst < show && sKey[std::size_t(sOrder[std::size_t(burst)])] < 1.5f) ++burst;
        const int rest = show - burst;
        const double beats = std::max(0.0, lastBeat - land);
        for (int q = 0; q < show; ++q) {
            const int e = sOrder[std::size_t(q)];
            double beat = land;
            if (q >= burst && beats >= 1.0) beat = land + 1.0 + std::floor(double(q - burst) * beats / double(std::max(rest, 1)));
            beat = std::min(beat, std::max(land, lastBeat));
            Element& el = pl.elems[std::size_t(e)];
            const int slot = e / 2;
            const int p = slot % 4, k = (slot / 4) % 12, j = (slot / 48) % 12;
            if ((e & 1) == 0) {
                // A spoke piece grows outwards, after the piece below it (chained within a beat).
                el.dur = 0.1f;
                el.dir = 1;
                double start = beat;
                const int pred = spokePred(im, i, j, k, p);
                if (pred >= 0 && pl.slotShard[std::size_t(pred / 2)] >= 0) {
                    const Element& pe = pl.elems[std::size_t(pred)];
                    if (!pe.shown()) continue;  // (hidden below: hidden too)
                    const double pend = double(pe.start) + 0.85 * pe.dur;
                    if (std::floor(pe.start) >= beat) start = std::max(start, pend);
                }
                el.start = float(start);
            } else {
                // A ring piece runs from the spoke that reached it first towards the other.
                el.dur = 0.16f;
                const int pR = nextSpoke(im, k, p, j);
                const int k1 = k + 1 >= im.K ? 0 : k + 1;
                const int left = spokePred(im, i, j, k, p);
                const int right = spokePred(im, i, j, pR < 4 ? k : k1, pR < 4 ? pR : 0);
                auto reached = [&](int pe) -> double {
                    if (pe < 0 || pl.slotShard[std::size_t(pe / 2)] < 0) return 1e30;
                    const Element& x = pl.elems[std::size_t(pe)];
                    return x.shown() ? double(x.start) + x.dur : 1e30;
                };
                const double tl = reached(left), tr = reached(right);
                if (tl >= 1e29 && tr >= 1e29) continue;
                el.dir = tl <= tr ? 1 : -1;
                double start = beat;
                const double t0 = std::min(tl, tr);
                if (std::floor(t0) >= beat) start = std::max(start, t0);
                el.start = float(start);
            }
            ++pl.shown;
        }
    }
    // Bisectors (the seams where two webs meet): well after the second of their impacts lands (60 % of the way from
    // it to the line), from the middle outwards over a beat and a half.
    for (int i = 0; i < n; ++i)
        for (int i2 = i + 1; i2 < n; ++i2) {
            Element& el = pl.elems[std::size_t(pairElem(i, i2))];
            const double later = std::max(pl.land[std::size_t(i)], pl.land[std::size_t(i2)]);
            const double t = std::ceil(later + 0.6 * (lastBeat - later));
            if (t <= lastBeat && c.cracked > 0.05) {
                el.start = float(t);
                el.dur = 1.5f;
                el.dir = 1;
                ++pl.shown;
            }
        }
    uint64_t key = mix64(seed);
    key = mix64(key ^ uint64_t(std::llround(pl.length * 16)));
    key = mix64(key ^ uint64_t(std::llround(first * 16)) ^ (uint64_t(std::llround(bar * 16)) << 20));
    key = mix64(key ^ uint64_t(std::llround(A * 4096)));
    key = mix64(key ^ uint64_t(c.impacts) ^ (uint64_t(std::llround(c.spokes * 64)) << 8) ^
                (uint64_t(std::llround(c.rings * 1024)) << 20) ^ (uint64_t(std::llround(c.fork * 1024)) << 32) ^
                (uint64_t(std::llround(c.cracked * 1024)) << 44) ^ (uint64_t(std::llround(c.size * 256)) << 54));
    pl.key = key | 1;
}

// ---------------------------------------------------------------------------------------------------------------- Player

Player::Player(uint64_t seed, const Config& c, double aspect, double startTime)
    : seed_(seed), config_(clampConfig(c)),
      aspect_(std::isfinite(aspect) ? std::clamp(aspect, 0.25, 4.0) : 16.0 / 9.0),
      start_(std::isfinite(startTime) ? startTime : 0.0),
      plans_(std::make_unique<std::array<Plan, 3>>()),
      creep_(std::make_unique<std::array<float, kElemIds>>()) {
    creep_->fill(0.0f);
    for (auto& p : *plans_) p.key = 0;
}

Player::PaneState Player::regular(int64_t k) const {
    PaneState p;
    p.valid = true;
    p.p0 = double(k) * config_.cycle + kArrive;
    p.p1 = double(k + 1) * config_.cycle;
    p.seed = mix64(seed_ * 0x9E3779B97F4A7C15ull ^ uint64_t(k));
    return p;
}

// A pane arriving on the beat line at or after `from`, shattering on the next line at least 12 beats later.
Player::PaneState Player::partial(double from, uint64_t salt) const {
    PaneState p;
    p.valid = true;
    p.p0 = std::ceil(from - 1e-9);
    const double C = config_.cycle;
    p.p1 = (std::floor(p.p0 / C) + 1.0) * C;
    if (p.p1 - p.p0 < 12.0) p.p1 += C;
    p.seed = mix64(seed_ * 0x9E3779B97F4A7C15ull ^ uint64_t(int64_t(p.p0)) ^ (salt << 40));
    return p;
}

void Player::rebase(double b) {
    const double C = config_.cycle;
    const auto k = int64_t(std::floor(b / C));
    cur_ = b - double(k) * C <= kArrive ? regular(k) : partial(b, 1);
    fall_ = FallState{};
    creepKey_ = 0;
    prevDrop_ = 1.0;  // a drop already under way does not shatter the first pane
}

int Player::plan(const PaneState& p) {
    const double C = config_.cycle;
    const double length = p.p1 - p.p0;
    // The first impact: Hit beats after the line for a regular pane, else two beats after the arrival.
    const double lineRel = std::floor(p.p0 / C) * C - p.p0;  // the line before, relative to the arrival
    double first = lineRel + config_.hit;
    if (first < kArriveBeats + 0.5) first = 2.0;
    const double bar = -p.p0;  // pane beat of absolute beat 0 (a bar line)
    // The key without building: compare the inputs that make a plan.
    auto& plans = *plans_;
    for (int i = 0; i < 3; ++i) {
        const Plan& q = plans[std::size_t(i)];
        if (q.key != 0 && q.seed == p.seed && q.length == std::clamp(length, 4.0, 512.0) && q.aspect == aspect_) {
            planUse_[std::size_t(i)] = ++uses_;
            return i;
        }
    }
    int slot = -1;
    for (int i = 0; i < 3; ++i) {
        if (i == curPlan_ || (fall_.valid && i == fall_.plan)) continue;
        if (slot < 0 || planUse_[std::size_t(i)] < planUse_[std::size_t(slot)]) slot = i;
    }
    if (slot < 0) slot = 0;
    makePlan(plans[std::size_t(slot)], p.seed, config_, aspect_, length, first, bar);
    ++planBuilds_;
    planUse_[std::size_t(slot)] = ++uses_;
    return slot;
}

void Player::shatter(double s, bool big) {
    fall_ = FallState{};
    fall_.valid = curPlan_ >= 0;
    fall_.plan = curPlan_;
    fall_.p0 = cur_.p0;
    fall_.s = s;
    fall_.big = big;
    ++shatters_;
    if (big) ++dropShatters_;
}

const Frame& Player::step(double t, const Clock& clockIn, const Controls& ctlIn) {
    // The beat: the phrase-aligned clock while it is on (junk: none), else 120 BPM from the start.
    const double age = std::isfinite(t) ? std::clamp(t - start_, 0.0, 1e8) : 0.0;
    const double cb = 4.0 * clockIn.bar;
    const bool on = clockIn.on && std::isfinite(cb) && std::fabs(cb) < 1e9;
    const double b = on ? cb : 2.0 * age;
    Clock clk = clockIn;
    clk.build = std::clamp(finiteOr(clockIn.build, 0.0), 0.0, 1.0);
    clk.drop = std::clamp(finiteOr(clockIn.drop, 0.0), 0.0, 1.0);
    Controls ctl;
    ctl.burst = std::clamp(finiteOr(ctlIn.burst, 0.3), 0.0, 1.0);
    ctl.gravity = std::clamp(finiteOr(ctlIn.gravity, 1.0), 0.0, 4.0);
    ctl.tilt = std::clamp(finiteOr(ctlIn.tilt, 1.0), 0.0, 4.0);
    const double C = config_.cycle;
    if (first_ || b < lastBeat_ - 0.25 || b > lastBeat_ + 16.0) {
        if (!first_) ++rebases_;
        first_ = false;
        rebase(b);
    }
    lastBeat_ = b;
    // The line: the pane shatters (a drop on it makes it explosive), the next one is on its way.
    for (int guard = 0; guard < 4 && cur_.valid && b >= cur_.p1; ++guard) {
        curPlan_ = plan(cur_);
        shatter(cur_.p1, ctl.burst >= 0.999 || clk.drop >= 0.5);
        const auto k = int64_t(std::llround(cur_.p1 / C));
        cur_ = regular(k);
        curPlan_ = -1;
    }
    // A drop: the pane (arrived) explodes on the next 16th line; a drop just after a gentle shatter blows it apart.
    if (clk.drop >= 0.5 && prevDrop_ < 0.5) {
        const double D = std::ceil(b * 4.0 - 1e-9) / 4.0;
        if (cur_.valid && b >= cur_.p0 + kArriveBeats && D < cur_.p1 - 0.5) {
            curPlan_ = plan(cur_);
            shatter(D, true);
            cur_ = partial(D + 3.0, 2);
            curPlan_ = -1;
        } else if (fall_.valid && !fall_.big && b - fall_.s < 2.0 && fall_.upgrade > 1e299) {
            fall_.upgrade = D;
        }
    }
    prevDrop_ = clk.drop;

    Frame& f = frame_;
    f.beat = b;
    f.build = float(clk.build);
    f.line16 = int64_t(std::floor(b * 4.0));
    // The pane: the one about to shatter on a pending drop line, else the current one once it starts arriving.
    f.pane = nullptr;
    if (fall_.valid && b < fall_.s && fall_.plan >= 0) {
        const Plan& pl = (*plans_)[std::size_t(fall_.plan)];
        paneFrame(pl, b - fall_.p0, b, clk, ctl, lastPaneKey_ != pl.key);
    } else if (cur_.valid && b >= cur_.p0) {
        curPlan_ = plan(cur_);
        const Plan& pl = (*plans_)[std::size_t(curPlan_)];
        paneFrame(pl, b - cur_.p0, b, clk, ctl, lastPaneKey_ != pl.key);
    }
    lastPaneKey_ = f.pane ? f.pane->key : 0;
    fallFrame(b, ctl);
    return f;
}

void Player::paneFrame(const Plan& pl, double rel, double b, const Clock& clk, const Controls& ctl, bool fresh) {
    Frame& f = frame_;
    f.pane = &pl;
    f.paneBeat = rel;
    const double x = std::clamp(rel / kArriveBeats, 0.0, 1.0);
    const double ease = 1.0 - std::pow(1.0 - x, 3.0);
    f.offset = config_.arrive == 0 ? float(ease - 1.0) : 0.0f;
    f.paneAlpha = config_.arrive == 1 ? float(x * x * (3 - 2 * x)) : 1.0f;
    f.sheen = float(1.0 + 1.5 * std::exp(-std::max(0.0, rel - kArriveBeats) / 1.2) * (rel >= 0 ? 1.0 : 0.0));
    if (fresh) {
        f.reveal.fill(0.0f);
        f.glow.fill(0.0f);
        f.tilt.fill(0.0f);
    }
    if (creepKey_ != pl.key) {
        creepKey_ = pl.key;
        creep_->fill(0.0f);
    }
    auto& creep = *creep_;
    // Creep: in a build, the cracks due in the next 4 beats run ahead on the 16th lines (up to 40 %, never back).
    const double q16 = std::floor(rel * 4.0) / 4.0;
    for (int q = 0; q < pl.elemCount; ++q) {
        const int e = pl.elemList[std::size_t(q)];
        const Element& el = pl.elems[std::size_t(e)];
        float rev = 0, glow = 0;
        if (el.shown()) {
            const double g = (rel - double(el.start)) / double(el.dur);
            if (g > 0) {
                const double gg = std::min(g, 1.0);
                rev = float(1.0 - (1.0 - gg) * (1.0 - gg));
                glow = float(std::exp(-std::max(0.0, rel - double(el.start) - el.dur) / 0.35));
            }
            if (clk.build > 0 && double(el.start) > rel) {
                const double ahead = double(el.start) - q16;
                if (ahead < 4.0) {
                    const float cv = float(clk.build * 0.4 * (1.0 - ahead / 4.0));
                    if (cv > creep[std::size_t(e)] + 1e-4f) {
                        creep[std::size_t(e)] = cv;
                        glow = std::max(glow, 0.35f);
                    }
                }
            }
            rev = std::max(rev, creep[std::size_t(e)]);
        }
        f.reveal[std::size_t(e)] = rev * float(el.dir);
        f.glow[std::size_t(e)] = glow;
    }
    // The pieces cracked loose tilt a hair (a build makes them creak on the 16ths).
    const double tiltGain = ctl.tilt;
    for (int s = 0; s < pl.shardCount; ++s) {
        const Shard& sd = pl.shards[std::size_t(s)];
        float sum = 0;
        for (int q = 0; q < sd.nbound; ++q) sum += std::fabs(f.reveal[std::size_t(sd.bound[std::size_t(q)])]);
        const double loose = double(sum) / double(std::max(1, int(sd.nbound)));
        const double w = std::clamp((loose - 0.45) / 0.45, 0.0, 1.0);
        double gain = tiltGain * w * w * (3 - 2 * w);
        if (clk.build > 0) {
            const float h = shape::sh_rand(pl.pane.seed, uint32_t(sd.slot), uint32_t(int64_t(std::floor(b * 4.0)) & 0xffffff), 41u);
            gain *= 1.0 + clk.build * 0.9 * (2.0 * h - 1.0);
        }
        f.tilt[std::size_t(2 * sd.slot)] = float(sd.tiltX * gain);
        f.tilt[std::size_t(2 * sd.slot + 1)] = float(sd.tiltY * gain);
    }
    for (int i = 0; i < kMaxImpacts; ++i)
        f.impactAge[std::size_t(i)] = i < pl.pane.count ? float(rel - double(pl.land[std::size_t(i)])) : -1.0f;
}

namespace {
// Row-major rotation about unit axis (x, y, z) by angle a.
void axisAngle(double x, double y, double z, double a, double* R) {
    const double c = std::cos(a), s = std::sin(a), t = 1 - c;
    R[0] = t * x * x + c;     R[1] = t * x * y - s * z; R[2] = t * x * z + s * y;
    R[3] = t * x * y + s * z; R[4] = t * y * y + c;     R[5] = t * y * z - s * x;
    R[6] = t * x * z - s * y; R[7] = t * y * z + s * x; R[8] = t * z * z + c;
}
void mul3(const double* A, const double* B, double* out) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = A[r * 3] * B[c] + A[r * 3 + 1] * B[3 + c] + A[r * 3 + 2] * B[6 + c];
}
}  // namespace

void Player::fallFrame(double b, const Controls& ctl) {
    Frame& f = frame_;
    f.loose = 0;
    f.fallen = nullptr;
    if (!fall_.valid || fall_.plan < 0 || b < fall_.s) return;
    const Plan& pl = (*plans_)[std::size_t(fall_.plan)];
    const double T = b - fall_.s;
    f.fallen = &pl;
    f.fallBeat = T;
    f.big = fall_.big;
    const double A = pl.aspect;
    const double g = 1.4 * ctl.gravity;
    const double burst = fall_.big ? 1.0 : ctl.burst;
    const double up = fall_.upgrade < 1e299 ? std::max(0.0, b - fall_.upgrade) : 0.0;
    std::array<float, kMaxShards> depth;
    int n = 0;
    for (int s = 0; s < pl.shardCount; ++s) {
        const Shard& sd = pl.shards[std::size_t(s)];
        const Impact& im = pl.pane.imp[std::size_t(sd.impact)];
        const auto& q = sd.rnd;
        double dx = sd.cx - im.cx, dy = sd.cy - im.cy;
        const double dist = std::max(std::hypot(dx, dy), 1e-4);
        dx /= dist;
        dy /= dist;
        // Gentle: the pane collapses (a little push out from the impact, pieces toppling forward as they fall), the
        // pieces near the impact first; big: thrown out from the impact and towards the camera, spinning.
        const double delayG = 0.12 * q[0] + 0.1 * dist;
        const double delayB = 0.04 * dist;
        const double tG = std::max(0.0, T - (fall_.big ? delayB : delayG));
        double vx, vy, vz, w;
        auto bigV = [&](double& ox, double& oy, double& oz, double& ow) {
            const double sp = (0.18 + 0.3 / (0.25 + dist)) * (0.6 + 0.8 * q[1]);
            ox = sp * dx + (q[2] - 0.5) * 0.2;
            oy = sp * dy + (q[3] - 0.5) * 0.2 - 0.15;
            oz = 0.3 + 0.6 * q[4];
            ow = 2.0 + 5.0 * q[5];
        };
        if (fall_.big) {
            bigV(vx, vy, vz, w);
        } else {
            const double sp = burst * (0.15 + 0.25 / (0.3 + dist)) * (0.6 + 0.8 * q[1]);
            vx = sp * dx + (q[2] - 0.5) * 0.06;
            vy = sp * dy + (q[3] - 0.5) * 0.05;
            vz = burst * 0.5 * q[4] - 0.04;
            w = (0.5 + 1.4 * q[5]) * (0.7 + burst);
        }
        double x = sd.cx + vx * tG;
        double y = sd.cy + vy * tG + 0.5 * g * tG * tG;
        double z = vz * tG;
        double ang = w * tG;
        if (up > 0) {
            double ux, uy, uz, uw;
            bigV(ux, uy, uz, uw);
            x += ux * up;
            y += uy * up;
            z += uz * up;
            ang += uw * up;
        }
        // Gone: below the frame, off a side, or past the camera.
        const double wz = std::max(0.05, (kFocal - z) / kFocal);
        const double ext = std::max(std::max(-sd.x0, sd.x1), std::max(-sd.y0, sd.y1)) / wz;
        const double sx = 0.5 * A + (x - 0.5 * A) / wz, sy = 0.5 + (y - 0.5) / wz;
        if (sy - ext > 1.02 || sx + ext < -0.02 || sx - ext > A + 0.02 || z > kFocal * 0.8 || T > 12.0) continue;
        if (n >= kMaxShards) break;
        ShardPose& p = f.poses[std::size_t(n)];
        p.shard = int16_t(s);
        p.x = float(x);
        p.y = float(y);
        p.z = float(z);
        // The axis: random, but a gentle fall mostly topples about x (the piece tips forward).
        double ax = q[2] - 0.5, ay = q[3] - 0.5, az = (q[4] - 0.5) * 0.6;
        if (!fall_.big && up <= 0) ax += 0.9 * (q[0] < 0.5 ? 1 : -1);
        const double al = std::max(std::sqrt(ax * ax + ay * ay + az * az), 1e-6);
        double R[9], Tl[9], out[9];
        axisAngle(ax / al, ay / al, az / al, ang, R);
        // The tilt it had in the cracked pane (about y by tiltX, about x by tiltY).
        const double tx = sd.tiltX * ctl.tilt, ty = sd.tiltY * ctl.tilt;
        double Ry[9], Rx[9];
        axisAngle(0, 1, 0, tx, Ry);
        axisAngle(1, 0, 0, ty, Rx);
        mul3(Ry, Rx, Tl);
        mul3(R, Tl, out);
        for (int k = 0; k < 9; ++k) p.rot[std::size_t(k)] = float(out[k]);
        p.flash = float(std::exp(-T / 0.3));
        p.alpha = float(1.0 - std::clamp((T - 9.0) / 3.0, 0.0, 1.0));
        depth[std::size_t(n)] = p.z;
        ++n;
    }
    // Back to front (farther from the camera first; equal depths in shard order).
    auto& idx = sortIdx_;
    for (int i = 0; i < n; ++i) idx[std::size_t(i)] = int16_t(i);
    std::sort(idx.begin(), idx.begin() + n, [&](int16_t a, int16_t c) {
        return depth[std::size_t(a)] < depth[std::size_t(c)] || (depth[std::size_t(a)] == depth[std::size_t(c)] && a < c);
    });
    for (int i = 0; i < n; ++i) sortTmp_[std::size_t(i)] = f.poses[std::size_t(idx[std::size_t(i)])];
    std::copy(sortTmp_.begin(), sortTmp_.begin() + n, f.poses.begin());
    f.loose = n;
    if (n == 0 && T > 1.0) {
        fall_.valid = false;
        f.fallen = nullptr;
    }
}

}  // namespace reforce::engine::shatter
