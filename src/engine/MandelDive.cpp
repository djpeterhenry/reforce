#include "engine/MandelDive.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace reforce::engine {

namespace {

// Where the dives end, four kinds (MandelKind), taken in turn.
//   Spiral, Lace: Misiurewicz points M(k, p), c whose critical orbit lands on a repelling p-cycle after k steps,
//     f^(k+p)(0) = f^k(0). The boundary passes through each one and looks the same (turned, scaled) at every depth,
//     so a dive toward one never runs out of detail. The period sets the number of arms: seahorse-valley spirals with
//     2..5 arms, spirals of the period-4 and period-5 bulbs' valleys, elephant-valley lace. Found by Newton's method
//     in 50 digits (mpmath) from random seeds in each valley, checked to be exactly (k, p); branch points (filaments
//     crossing in a Y or a star: the top branch point M(4,1), the west antenna M(3,1), the period-3 bulb's M(10,2),
//     the south dendrite M(7,1)) dropped (Peter 2026-11-22). Iterations: a linear fit (12 <= depth <= 32) of the
//     99.5th percentile escape count over the dive's frames (numpy, double).
//   Mini: a mini-Mandelbrot, nucleus (f^p(0) = 0 by Newton from the period a small box near a Misiurewicz point first
//     surrounds 0) and size (the estimate 1 / (b l^2), l = prod 2 z_k, b = sum 1 / partial products). Kept: clean
//     copies (the shader's affine interior test finds no false insides against numpy with 300 x period iterations)
//     that fit the GPU budget. Approach iterations: the nearby Misiurewicz point's fit; at the landing 45 x period.
//   Decor: the ring of Julia-like decorations a dive passes on its way to a deep mini, the dive ending about half
//     way down in log (land), centred on the nucleus; about 10 x period iterations there.
// Listed to double precision (the centre is a double: clean to ~2^40).
using K = MandelKind;
constexpr MandelTarget kTargets[] = {
    {"seahorse double spiral M(28,2)", K::Spiral, -0.74515806380161955527, 0.11257491620541652118, 584, 11.0},
    {"seahorse three-arm spiral M(20,3)", K::Spiral, -0.74716476082597510388, 0.15032843344881328706, 207, 2.0},
    {"seahorse four-arm spiral M(20,4)", K::Spiral, -0.74547192882489467855, 0.13880796958401817906, 374, 2.7},
    {"seahorse five-arm spiral M(20,5)", K::Spiral, -0.74798894294387559519, 0.14707862952477302287, 250, 0.5},
    {"period-5 bulb valley swirl M(12,4)", K::Spiral, 0.38508992360614463799, 0.30386756513077908039, 161, 0.9},
    {"elephant valley lace M(13,3)", K::Lace, 0.29161981157542174295, 0.01303370118013765602, 148, 0.9},
    {"elephant valley lace M(12,2)", K::Lace, 0.30593911351735693992, 0.020515540918573715452, 122, 0.5},
    {"antenna mini p13", K::Mini, -1.5364873165110883288, 0.0033042282317407624943, 23, 1.3, 13, -1.1527023778e-6, -2.77264135948e-7},
    {"elephant mini p19", K::Mini, 0.29176508743430503773, 0.013197401941790544639, 148, 0.9, 19, -1.87813893016e-6, -8.10513041938e-7},
    {"snowflake mini p20", K::Mini, -0.1000861746032708448, 0.95679766940193796642, 35, 2.4, 20, -1.58971825493e-6, 9.30679837962e-7},
    {"south mini p49", K::Mini, 0.0016552775327385077886, -0.82246235218741677781, 49, 3.6, 49, 1.4476834355e-10, -2.88893161795e-10},
    {"swirl ring around a p112 mini", K::Decor, -0.7450806558883047818, 0.11259933712280159442, 584, 11.0, 112, 7.98170192838e-9, 5.5403230805e-9, 23},
    {"spiral ring around a p49 mini", K::Decor, 0.0016552775327385077886, -0.82246235218741677781, 49, 3.6, 49, 1.4476834355e-10, -2.88893161795e-10, 27},
    {"star around a p32 mini", K::Decor, -0.10106405613316007691, 0.95630627630197365666, 35, 2.4, 32, -1.93577935064e-9, 7.84640836321e-10, 24},
};
constexpr int kCount = int(sizeof(kTargets) / sizeof(kTargets[0]));

std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

double smoothstep(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3 - 2 * u);
}

double fin(double v, double fb) { return std::isfinite(v) ? v : fb; }

}  // namespace

int mandelTargetCount() { return kCount; }

const char* mandelKindName(MandelKind k) {
    switch (k) {
        case MandelKind::Spiral: return "spiral";
        case MandelKind::Lace: return "lace";
        case MandelKind::Mini: return "mini";
        case MandelKind::Decor: return "decoration";
    }
    return "?";
}

double MandelTarget::miniDeep() const { return hasMini() ? -std::log2(std::hypot(sre, sim)) : 0.0; }
double MandelTarget::deep() const { return kind == MandelKind::Mini ? miniDeep() : kind == MandelKind::Decor ? land : 0.0; }
double MandelTarget::angle() const { return hasMini() ? std::atan2(sim, sre) : 0.0; }
double MandelTarget::frameRe() const { return kind == MandelKind::Mini ? re - 0.65 * sre : re; }
double MandelTarget::frameIm() const { return kind == MandelKind::Mini ? im - 0.65 * sim : im; }

const MandelTarget& mandelTarget(int i) { return kTargets[((i % kCount) + kCount) % kCount]; }

namespace {

// A seeded shuffle of 0..n-1 for `round`; the first two swapped if the first would repeat the previous round's last
// (n >= 3: that swap never changes a round's last entry, so the rule is consistent).
template <std::size_t N>
std::array<int, N> roundOrder(std::uint64_t seed, long long round, int n) {
    auto raw = [&](long long r) {
        std::array<int, N> o{};
        for (int i = 0; i < n; ++i) o[std::size_t(i)] = i;
        std::uint64_t s = mix(seed ^ mix(std::uint64_t(r)));
        for (int i = n - 1; i > 0; --i) {
            s = mix(s);
            std::swap(o[std::size_t(i)], o[std::size_t(s % std::uint64_t(i + 1))]);
        }
        return o;
    };
    auto o = raw(round);
    if (n >= 3 && o[0] == raw(round - 1)[std::size_t(n - 1)]) std::swap(o[0], o[1]);
    return o;
}

long long floorDiv(long long a, long long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

}  // namespace

namespace {
struct SharedDives {
    long long next = 0;
    std::uint64_t seed = 0;
    bool seeded = false;
};
SharedDives& sharedDives() {
    static SharedDives s;  // the render thread's (organics are stepped there)
    return s;
}
}  // namespace

MandelDive::MandelDive(const MandelDiveParams& p, std::uint64_t seed) : p_(p), seed_(seed) {
    if (!p_.shared) return;
    SharedDives& g = sharedDives();
    if (!g.seeded) {
        g.seed = seed;
        g.seeded = true;
    }
    seed_ = g.seed;
    base_ = g.next;
}

int MandelDive::targetFor(long long dive, int fixed) const {
    if (fixed >= 0) return fixed % kCount;
    // The pool (Mini: every kind, minis and decorations only, or no minis), grouped by kind. Dives take the kinds in
    // turn, a seeded order of the kinds per block of dives (never the same kind twice in a row); within a kind its
    // targets in a seeded order per round (each once per round, no repeat across a round's seam).
    std::array<std::array<int, kCount>, kMandelKinds> byKind{};
    std::array<int, kMandelKinds> count{};
    for (int i = 0; i < kCount; ++i) {
        const bool mini = kTargets[i].hasMini();
        if (p_.minis == 1 && !mini) continue;
        if (p_.minis == 2 && mini) continue;
        const int k = int(kTargets[i].kind);
        byKind[std::size_t(k)][std::size_t(count[std::size_t(k)]++)] = i;
    }
    std::array<int, kMandelKinds> kinds{};
    int nk = 0;
    for (int k = 0; k < kMandelKinds; ++k)
        if (count[std::size_t(k)] > 0) kinds[std::size_t(nk++)] = k;
    if (nk == 0) return 0;
    const long long block = floorDiv(dive, nk);
    const int slot = int(dive - block * nk);
    // nk <= 2: a fixed order (it alternates); else a seeded one per block.
    const int kind = nk <= 2 ? kinds[std::size_t(slot)]
                             : kinds[std::size_t(roundOrder<kMandelKinds>(seed_ ^ 0x6b696e64ULL, block, nk)[std::size_t(slot)])];
    // The kind comes up once per block: its block number counts its appearances.
    const int m = count[std::size_t(kind)];
    const long long round = floorDiv(block, m);
    const int at = int(block - round * m);
    const auto o = roundOrder<kCount>(seed_ ^ mix(std::uint64_t(kind) + 1), round, m);
    return byKind[std::size_t(kind)][std::size_t(o[std::size_t(at)])];
}

MandelView MandelDive::step(double beat, double bar, double drop, double dt, int fixedTarget) {
    MandelView v;
    beat = fin(beat, 0.0);
    bar = fin(bar, 0.0);
    const double diveBars = std::clamp(fin(p_.diveBars, 16.0), 0.25, 1024.0);
    // Dives count from the phrase line at or before the first step (Algn bars on the phrase-aligned bar clock), so a
    // long dive (several phrases) starts with the object, not on a far multiple of its own length.
    const double align = std::clamp(fin(p_.align, 16.0), 0.0, 1024.0);
    if (!started_ || beat < startBeat_) originBar_ = align > 0 ? std::floor(bar / align) * align : 0.0;
    bar -= originBar_;
    const double d0 = std::floor(bar / diveBars);
    v.dive = (long long)std::clamp(d0, -1e15, 1e15);
    const double total = diveBars * 4.0;  // beats in a dive
    const double b = std::clamp((bar - d0 * diveBars) * 4.0, 0.0, total);
    v.pos = b / total;
    v.target = targetFor(base_ + v.dive, fixedTarget);
    if (p_.shared && v.dive >= 0 && v.dive < 1000000) {
        SharedDives& g = sharedDives();
        g.next = std::max(g.next, base_ + v.dive + 1);
    }
    const MandelTarget& t = kTargets[v.target];

    const double deepMax = std::clamp(fin(p_.deep, 34.0), 0.0, 60.0);
    const double cap = t.deep() > 0 ? std::min(deepMax, t.deep()) : deepMax;
    const double out = std::clamp(fin(p_.out, 4.0), 0.0, total * 0.5);
    double dbl = fin(p_.dbl, 2.0);
    if (dbl <= 0) dbl = cap > 0 ? std::max(0.05, (total - out) / cap) : 1.0;
    dbl = std::max(0.05, dbl);
    // Ease in over the first beat: depth runs (b - (1 - e^-b)) / dbl.
    const double lin = (b - (1.0 - std::exp(-b))) / dbl;
    // Soft cap: -k ln(e^(-lin/k) + e^(-cap/k)).
    constexpr double k = 1.0;
    double depth = std::max(0.0, -k * std::log(std::exp(-lin / k) + std::exp(-cap / k)));
    if (out > 0 && b > total - out) depth *= 1.0 - smoothstep((b - (total - out)) / out);
    // Catch-up after a start mid-dive: from the overview at Ent doublings a beat (eased in) until it meets the dive.
    if (!started_ || beat < startBeat_) {
        started_ = true;
        startBeat_ = beat;
    }
    const double ent = std::clamp(fin(p_.entry, 6.0), 0.0, 64.0);
    if (ent > 0) {
        const double e = beat - startBeat_;
        depth = std::min(depth, ent * (e - (1.0 - std::exp(-e))));
        depth = std::max(depth, 0.0);
    }
    // Kick: a quick surge on the beat line, relaxing over the beat.
    const double f = beat - std::floor(beat);
    const double kick = std::clamp(fin(p_.kick, 0.0), -8.0, 8.0);
    depth += kick * std::min(1.0, f / 0.06) * std::exp(-5.0 * f);
    // Drop: a fast attack (~80 ms), then follows SYNC_DROP's decay.
    drop = std::clamp(fin(drop, 0.0), 0.0, 1.0);
    dt = std::clamp(fin(dt, 0.0), 0.0, 1.0);
    if (drop > dropS_) dropS_ += (drop - dropS_) * (1.0 - std::exp(-dt / 0.08));
    else dropS_ = drop;
    depth -= std::clamp(fin(p_.drop, 0.0), 0.0, 60.0) * dropS_;
    v.depth = std::clamp(depth, 0.0, deepMax + 8.0);

    const double s = std::exp2(-1.5 * v.depth);
    const double tx = t.frameRe(), ty = t.frameIm();
    v.cx = tx + (kMandelHomeRe - tx) * s;
    v.cy = ty + (0.0 - ty) * s;
    v.half = kMandelHomeHalf * std::exp2(-v.depth);
    double need = t.itA + t.itB * v.depth;
    if (t.hasMini()) {
        // Landing (the soft cap reaches cap - ln 2): the iterations ramp up over the last 6 doublings to 45 x the
        // period (the 99th percentile escape count around a mini filling the frame; 10 x for a decoration's ring),
        // a mini turns upright over the last 10.
        const double ramp = v.depth - (cap - 1.0);
        need = std::max(need, (t.kind == MandelKind::Mini ? 45.0 : 10.0) * t.period * smoothstep((ramp + 6.0) / 6.0));
        if (t.kind == MandelKind::Mini) v.turn = t.angle() * smoothstep((ramp + 10.0) / 10.0);
        // The affine interior test needs the mini at least an eighth of the frame (float offsets of a few sizes).
        v.mini = p_.affine && v.depth > t.miniDeep() - 3.0;
    }
    const double it = std::max(double(std::max(1, p_.iter)), std::clamp(fin(p_.itk, 1.5), 0.0, 100.0) * need);
    v.iter = int(std::clamp(it, 8.0, double(std::clamp(p_.maxIter, 8, 20000))));
    return v;
}

}  // namespace reforce::engine
