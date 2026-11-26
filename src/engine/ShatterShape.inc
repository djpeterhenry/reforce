// reforce extension (docs/3d-design.md "Shatter"): the fracture of the glass pane, one source for C++ (engine/Shatter,
// the tests) and Metal (render/ShatterPipeline, the crack map), so both break the pane into exactly the same shards.
//
// Included with SHATTER_SHAPE_SOURCE(...) defined either as the code itself (C++, inside a namespace that provides
// uint, sqrt, log, exp, sin, cos, atan2, floor, min, max, clamp, abs; SH_CONST `const`) or as #__VA_ARGS__ (a string for the
// shader compiler, whose header defines SH_CONST as `constant`). So: no preprocessor lines and no string literals in
// here, only the subset of C++ that is also Metal.
//
// Pane units: frame heights, x 0..aspect, y 0..1 (down). Every impact owns its Voronoi cell of the pane (the nearest
// impact; the bisector between two impacts, with a gentle wander, is a crack too). Around an impact, a spider web in polar
// coordinates:
//   - K radial cracks (spokes), jittered in angle, each kinked by a piecewise-linear wander in log radius;
//   - forks: from ring f1 (or one later) a spoke may branch into its sector (level 1, at about the sector's middle),
//     from ring f2 each half may branch again (level 2): the branch leaves its parent spoke where that spoke meets
//     the ring and curves out to its share of the sector over about a band, so it never crosses a neighbor;
//   - concentric cracks: ring j (1 .. nb - 1) runs from spoke to spoke at radius r0 g^(j - 1), its end on each spoke
//     jittered by 10 % (so rings kink at the spokes), nearly straight between them (from the arc towards the chord as
//     far as the margin to the next ring allows) with a little bow. Ring j is drawn over the sectors of band j - 1
//     (the spokes that exist inside it).
// A shard is one sector (leaf) of one band: slot ((impact * 12 + band) * 12 + spoke) * 4 + leaf (leaf 0..3: the
// positions of the base spoke, the level-2 fork in the left half, the level-1 fork, the level-2 fork in the right half).
// The rings' jitter, bow and straightening keep ring j + 1 outside ring j everywhere (ratio >= .77 g (1 - .95 (1 -
// 1 / .77 g)) > 1), and the
// spokes' jitter (.28 of a sector) and wander (.11) keep them apart, so every point has exactly one shard.
// A crack element is a boundary piece: 2 slot (the shard's left spoke in its band) or 2 slot + 1 (its inner ring,
// the part of ring j over its angle), or 2 * 3456 + i * 6 + i2 (the bisector of impacts i < i2). `param` runs along
// it: 0 at the inner end of a spoke piece .. 1 at the outer, 0 .. 1 left to right along a ring (over the whole
// sector of the band inside it), 0 at a bisector's middle .. 1 1.2 frame heights out.
SHATTER_SHAPE_SOURCE(
struct ShImpact {
    float cx; float cy;   // center (pane units)
    float rot;            // the first spoke's angle
    float r0;             // ring 1's radius
    float lg;             // log of the ring ratio g (>= log 1.45)
    float fork;           // fork probability
    int K;                // base spokes, 3..12
    int nb;               // bands, 1..12 (the last one is open)
    int f1; int f2;       // the rings where level-1 / level-2 forks start (or one later)
    uint seed;
    float pad;
};
struct ShPane { ShImpact imp[6]; int count; float aspect; uint seed; float pad; };
struct ShHit { int slot; int elem; float param; float dist; int impact; int band; };
struct ShLeaf { int p; int pR; float a; float b; int idL; int idR; };
struct ShRing { float r; float s; int p; };

inline uint sh_hash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

inline float sh_rand(uint seed, uint a, uint b, uint salt) {
    const uint h = sh_hash(seed ^ (a * 0x9E3779B1u) ^ (b * 0x85EBCA77u) ^ (salt * 0xC2B2AE3Du));
    return float(h >> 8u) * (1.0f / 16777216.0f);
}

inline float sh_twopi() { return 6.28318531f; }
inline int sh_slot(int i, int j, int k, int p) { return ((i * 12 + j) * 12 + k) * 4 + p; }
inline int sh_slots() { return 3456; }
inline int sh_pair(int i, int i2) { return 2 * 3456 + min(i, i2) * 6 + max(i, i2); }

// A piecewise-linear wander in -1..1 with a kink every third of a log-radius unit.
inline float sh_wig(uint seed, uint id, float u) {
    const float x = clamp(u * 3.0f + 64.0f, 0.0f, 1000.0f);
    const float f = floor(x);
    const float t = x - f;
    const uint i = uint(f);
    const float a = sh_rand(seed, id, i, 7u) * 2.0f - 1.0f;
    const float b = sh_rand(seed, id, i + 1u, 7u) * 2.0f - 1.0f;
    return a + (b - a) * t;
}

// Base spoke k (0..K; K is spoke 0 a turn later): its angle from rot at log radius u.
inline float sh_base(ShImpact m, int k, float u) {
    const int kk = k >= m.K ? k - m.K : k;
    const float gap = sh_twopi() / float(m.K);
    const float jit = (sh_rand(m.seed, uint(kk), 0u, 1u) * 2.0f - 1.0f) * 0.28f;
    const float w = sh_wig(m.seed, uint(kk) * 4u, u) * 0.11f;
    return gap * (float(kk) + jit + w) + (k >= m.K ? sh_twopi() : 0.0f);
}

// Where ring j (1..nb - 1) meets spoke `id` (base k: 4 k; forks 4 k + p).
inline float sh_ringAt(ShImpact m, int j, int id) {
    return m.r0 * exp(m.lg * float(j - 1)) * (1.0f + 0.1f * (sh_rand(m.seed, uint(id), uint(j), 21u) * 2.0f - 1.0f));
}

// The band fork p (1, 2, 3) of base sector k starts at (99: never). Level 2 starts two rings after level 1 at least.
inline int sh_birth(ShImpact m, int k, int p) {
    if (sh_rand(m.seed, uint(k), 2u, 11u) >= m.fork) return 99;
    const int b2 = m.f1 + (sh_rand(m.seed, uint(k), 2u, 14u) < 0.5f ? 0 : 1);
    if (p == 2) return b2;
    if (sh_rand(m.seed, uint(k), uint(p), 11u) >= m.fork) return 99;
    return max(m.f2 + (sh_rand(m.seed, uint(k), uint(p), 14u) < 0.5f ? 0 : 1), b2 + 2);
}

// A fork's share of its parent interval at log radius u: 0 (on the left parent) .. 1 (on the right parent). It
// leaves the parent it branches from where that parent meets ring `birth`, and curves out to its share over a band.
inline float sh_forkFrac(ShImpact m, int k, int p, int birth, int idParentL, int idParentR, float u) {
    const bool left = sh_rand(m.seed, uint(k), uint(p), 13u) < 0.5f;
    const float s = p == 2 ? 0.38f + 0.24f * sh_rand(m.seed, uint(k), uint(p), 12u)
                           : 0.34f + 0.32f * sh_rand(m.seed, uint(k), uint(p), 12u);
    const float rb = sh_ringAt(m, birth, left ? idParentL : idParentR);
    const float x = clamp((u - log(rb)) / (m.lg * 1.1f), 0.0f, 1.0f);
    const float ramp = x * x * (3.0f - 2.0f * x);
    const float w = 0.07f * sh_wig(m.seed, uint(k) * 4u + uint(p), u) * ramp;
    const float f = left ? s * ramp : 1.0f - (1.0f - s) * ramp;
    return clamp(f + w, 0.0f, 1.0f);
}

// The spokes of base sector k at log radius u: the base spokes' angles (a0, a4), the forks' (a2 level 1, a1 / a3
// level 2: where they would be, even before they are born) and the bands they are born in (99: never).
struct ShSpokes { float a0; float a1; float a2; float a3; float a4; int b1; int b2; int b3; int k; int k1; };
inline ShSpokes sh_spokes(ShImpact m, int k, float u) {
    ShSpokes S;
    S.k = k;
    S.k1 = k + 1 >= m.K ? 0 : k + 1;
    S.a0 = sh_base(m, k, u);
    S.a4 = sh_base(m, k + 1, u);
    S.b1 = sh_birth(m, k, 1);
    S.b2 = sh_birth(m, k, 2);
    S.b3 = sh_birth(m, k, 3);
    S.a2 = S.b2 < 99 ? S.a0 + (S.a4 - S.a0) * sh_forkFrac(m, k, 2, S.b2, k * 4, S.k1 * 4, u) : S.a4;
    S.a1 = S.b1 < 99 ? S.a0 + (S.a2 - S.a0) * sh_forkFrac(m, k, 1, S.b1, k * 4, k * 4 + 2, u) : S.a2;
    S.a3 = S.b3 < 99 ? S.a2 + (S.a4 - S.a2) * sh_forkFrac(m, k, 3, S.b3, k * 4 + 2, S.k1 * 4, u) : S.a4;
    return S;
}

// The leaf of the sector in band j holding angle phi (from rot, unwrapped into the sector): its position p, the next
// spoke's position pR (4: base spoke k + 1), its bounding angles and spoke ids.
inline ShLeaf sh_leafS(ShSpokes S, int j, float phi) {
    ShLeaf L;
    L.p = 0;
    L.pR = 4;
    L.a = S.a0;
    L.b = S.a4;
    L.idL = S.k * 4;
    L.idR = S.k1 * 4;
    if (j < S.b2) return L;
    if (phi < S.a2) {
        L.b = S.a2;
        L.pR = 2;
        L.idR = S.k * 4 + 2;
        if (j >= S.b1) {
            if (phi < S.a1) {
                L.b = S.a1;
                L.pR = 1;
                L.idR = S.k * 4 + 1;
            } else {
                L.p = 1;
                L.a = S.a1;
                L.idL = S.k * 4 + 1;
            }
        }
    } else {
        L.p = 2;
        L.a = S.a2;
        L.idL = S.k * 4 + 2;
        if (j >= S.b3) {
            if (phi < S.a3) {
                L.b = S.a3;
                L.pR = 3;
                L.idR = S.k * 4 + 3;
            } else {
                L.p = 3;
                L.a = S.a3;
                L.idL = S.k * 4 + 3;
            }
        }
    }
    return L;
}
inline ShLeaf sh_leaf(ShImpact m, int k, int j, float u, float phi) { return sh_leafS(sh_spokes(m, k, u), j, phi); }

// Ring j (1..nb - 1) at angle phi: its radius, the fraction s across the sector of band j - 1 it spans there, and
// that sector's leaf position.
inline ShRing sh_ringS(ShImpact m, ShSpokes S, int j, float phi) {
    const ShLeaf L = sh_leafS(S, j - 1, phi);
    ShRing o;
    const float d = max(L.b - L.a, 1e-6f);
    o.s = clamp((phi - L.a) / d, 0.0f, 1.0f);
    const float ra = sh_ringAt(m, j, L.idL);
    const float rb = sh_ringAt(m, j, L.idR);
    // From the arc (linear in angle) towards the straight chord between the spokes, as far as the next ring's margin
    // allows (the chord sags by 1 - cos(d / 2) in the middle).
    const float arc = ra + (rb - ra) * o.s;
    const float den = ra * sin(o.s * d) + rb * sin((1.0f - o.s) * d);
    const float chord = den > 1e-9f ? ra * rb * sin(d) / den : arc;
    const float sig = clamp(0.95f * (1.0f - 1.0f / (0.77f * exp(m.lg))) / max(1.0f - cos(0.5f * d), 1e-6f), 0.0f, 0.9f);
    const float bow = sh_rand(m.seed, uint(L.idL), uint(j), 22u) * 0.06f - 0.03f;
    o.r = (arc + (chord - arc) * sig) * (1.0f + bow * 4.0f * o.s * (1.0f - o.s));
    o.p = L.p;
    return o;
}
inline ShRing sh_ring(ShImpact m, int k, int j, float u, float phi) { return sh_ringS(m, sh_spokes(m, k, u), j, phi); }

// The shard at pane point (px, py): its slot, the nearest crack element, the position along it and the distance to
// it (pane units), the impact and band.
inline ShHit sh_owner(SH_CONST ShPane& P, float px, float py) {
    int i1 = -1;
    int i2 = -1;
    float d1 = 1e30f;
    float d2 = 1e30f;
    for (int i = 0; i < P.count; ++i) {
        const float dx = px - P.imp[i].cx;
        const float dy = py - P.imp[i].cy;
        const float d = dx * dx + dy * dy;
        if (d < d1) {
            i2 = i1;
            d2 = d1;
            i1 = i;
            d1 = d;
        } else if (d < d2) {
            i2 = i;
            d2 = d;
        }
    }
    ShHit h;
    h.slot = 0;
    h.elem = 0;
    h.param = 0.0f;
    h.dist = 1e9f;
    h.impact = 0;
    h.band = 0;
    if (i1 < 0) return h;
    // Between the two nearest impacts the boundary is their bisector with a gentle wander (up to .012 either side),
    // the same function of the position along it from both sides.
    float db = 1e9f;
    float bp = 0.0f;
    if (i2 >= 0) {
        const int lo = min(i1, i2);
        const int hi = max(i1, i2);
        const float ex = P.imp[hi].cx - P.imp[lo].cx;
        const float ey = P.imp[hi].cy - P.imp[lo].cy;
        const float el = max(sqrt(ex * ex + ey * ey), 1e-6f);
        const float dlo = i1 == lo ? d1 : d2;
        const float dhi = i1 == lo ? d2 : d1;
        const float sd = (dlo - dhi) / (2.0f * el);
        const float mx = 0.5f * (P.imp[lo].cx + P.imp[hi].cx);
        const float my = 0.5f * (P.imp[lo].cy + P.imp[hi].cy);
        const float t = ((px - mx) * (-ey) + (py - my) * ex) / el;
        const float w = 0.012f * sh_wig(P.seed, uint(sh_pair(lo, hi)), t * 4.0f);
        const int own = sd > w ? hi : lo;
        i2 = own == i1 ? i2 : i1;
        i1 = own;
        db = abs(sd - w);
        bp = min(abs(t) / 1.2f, 1.0f);
    }
    const ShImpact m = P.imp[i1];
    const float vx = px - m.cx;
    const float vy = py - m.cy;
    const float r = max(sqrt(vx * vx + vy * vy), 1e-6f);
    const float u = log(max(r, m.r0 * 0.05f));
    float phi = atan2(vy, vx) - m.rot;
    phi = phi - sh_twopi() * floor(phi / sh_twopi());
    // The base sector (the jitter and wander move a spoke by less than half a sector: one of three).
    const float gap = sh_twopi() / float(m.K);
    const int k0 = clamp(int(phi / gap), 0, m.K - 1);
    int k = k0;
    float ph = phi;
    for (int d = -1; d <= 1; ++d) {
        int kk = k0 + d;
        float pp = phi;
        if (kk < 0) {
            kk += m.K;
            pp += sh_twopi();
        } else if (kk >= m.K) {
            kk -= m.K;
            pp -= sh_twopi();
        }
        if (pp >= sh_base(m, kk, u) && pp < sh_base(m, kk + 1, u)) {
            k = kk;
            ph = pp;
        }
    }
    const int k1 = k + 1 >= m.K ? 0 : k + 1;
    const ShSpokes S = sh_spokes(m, k, u);
    // The band: from the nominal one, stepping in or out until it lies between its rings.
    int j = clamp(int(floor((u - log(m.r0)) / m.lg)) + 1, 0, m.nb - 1);
    ShRing ri;
    ri.r = 0.0f;
    ri.s = 0.0f;
    ri.p = 0;
    float ro = 1e9f;
    for (int it = 0; it < 6; ++it) {
        ri.r = 0.0f;
        ri.s = 0.0f;
        if (j > 0) ri = sh_ringS(m, S, j, ph);
        if (j > 0 && r < ri.r) {
            j -= 1;
            continue;
        }
        ro = j < m.nb - 1 ? sh_ringS(m, S, j + 1, ph).r : ri.r + 4.0f;
        if (j < m.nb - 1 && r >= ro) {
            j += 1;
            continue;
        }
        break;
    }
    const ShLeaf L = sh_leafS(S, j, ph);
    const int slot = sh_slot(i1, j, k, L.p);
    h.slot = slot;
    h.impact = i1;
    h.band = j;
    // The nearest boundary: the shard's left / right spoke (arcs), its inner / outer ring, a bisector.
    const float dl = r * (ph - L.a);
    const float dr = r * (L.b - ph);
    const float di = j > 0 ? r - ri.r : 1e9f;
    const float dout = j < m.nb - 1 ? ro - r : 1e9f;
    const float sp = clamp((r - ri.r) / max(ro - ri.r, 1e-6f), 0.0f, 1.0f);
    float best = dl;
    h.elem = 2 * slot;
    h.param = sp;
    if (dr < best) {
        best = dr;
        h.elem = 2 * (L.pR < 4 ? sh_slot(i1, j, k, L.pR) : sh_slot(i1, j, k1, 0));
    }
    if (di < best) {
        best = di;
        h.elem = 2 * slot + 1;
        h.param = ri.s;
    }
    if (dout < best) {
        best = dout;
        const ShLeaf Lo = sh_leafS(S, j + 1, ph);
        h.elem = 2 * sh_slot(i1, j + 1, k, Lo.p) + 1;
        h.param = clamp((ph - L.a) / max(L.b - L.a, 1e-6f), 0.0f, 1.0f);
    }
    if (i2 >= 0 && db < best) {
        best = db;
        h.elem = sh_pair(i1, i2);
        h.param = bp;
    }
    h.dist = max(best, 0.0f);
    return h;
}
)
