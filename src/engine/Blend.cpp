#include "engine/Blend.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace reforce::engine {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

constexpr double kPi = 3.14159265358979323846;

// ---- color spaces (palette values are display-encoded sRGB) ----
double toLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double toSrgb(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

struct Lab { double L, a, b; };

Lab rgbToOklab(const PaletteEntry& e) {
    const double r = toLinear(e.r), g = toLinear(e.g), b = toLinear(e.b);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

PaletteEntry oklabToRgb(const Lab& c) {
    const double l = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    const double m = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    const double s = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    const double l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    return {float(toSrgb(4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3)),
            float(toSrgb(-1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3)),
            float(toSrgb(-0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3)), 1.f};
}

void rgbToHsv(const PaletteEntry& e, double& h, double& s, double& v) {
    const double r = e.r, g = e.g, b = e.b;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 1e-12) { h = 0; return; }
    if (mx == r) h = (g - b) / d;
    else if (mx == g) h = 2.0 + (b - r) / d;
    else h = 4.0 + (r - g) / d;
    h /= 6.0;
    if (h < 0) h += 1.0;
}

double lerp(double a, double b, double w) { return a + (b - a) * w; }

bool inFrame(float x, float y) { return std::abs(x) < 1e6f && std::abs(y) < 1e6f; }

}  // namespace

// ---------------------------------------------------------------- registry

const std::vector<std::string>& blendModeNames(Layer layer) {
    static const std::vector<std::string> color{"gforce", "oklab", "hsv-short"};
    static const std::vector<std::string> field{"gforce", "polar", "crossfade"};
    static const std::vector<std::string> wave{"gforce", "aligned", "crossfade"};
    static const std::vector<std::string> none;
    switch (layer) {
        case Layer::ColorMap: return color;
        case Layer::FlowField: return field;
        case Layer::WaveShape: return wave;
        default: return none;
    }
}

std::optional<int> findBlendMode(Layer layer, const std::string& name) {
    const std::string n = lower(name);
    const auto& names = blendModeNames(layer);
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == n) return int(i);
    if (layer == Layer::ColorMap && (n == "rgb" || n == "hsv")) return n == "rgb" ? 0 : 2;
    if (layer == Layer::FlowField && n == "linear") return 0;
    if (!names.empty() && (n == "default" || n.empty())) return 0;
    return std::nullopt;
}

std::string blendModeName(Layer layer, int mode) {
    const auto& names = blendModeNames(layer);
    return mode >= 0 && std::size_t(mode) < names.size() ? names[std::size_t(mode)] : std::string("?");
}

// ---------------------------------------------------------------- ColorMap

PaletteEntry blendColor(int mode, const PaletteEntry& a, const PaletteEntry& b, double w) {
    w = std::clamp(w, 0.0, 1.0);
    switch (mode) {
        case blend::ColorOklab: {
            const Lab la = rgbToOklab(a), lb = rgbToOklab(b);
            return oklabToRgb({lerp(la.L, lb.L, w), lerp(la.a, lb.a, w), lerp(la.b, lb.b, w)});
        }
        case blend::ColorHsvShort: {
            double ha, sa, va, hb, sb, vb;
            rgbToHsv(a, ha, sa, va);
            rgbToHsv(b, hb, sb, vb);
            // A gray end has no hue: take the other end's.
            if (sa < 1e-4 || va < 1e-4) ha = hb;
            if (sb < 1e-4 || vb < 1e-4) hb = ha;
            double dh = hb - ha;
            dh -= std::floor(dh + 0.5);  // shortest arc, (-.5, .5]
            double h = ha + dh * w;
            h -= std::floor(h);
            const scene::Rgb c = scene::hsvToRgb(h, lerp(sa, sb, w), lerp(va, vb, w));
            return {std::clamp(c.r, 0.f, 1.f), std::clamp(c.g, 0.f, 1.f), std::clamp(c.b, 0.f, 1.f), 1.f};
        }
        default:  // gforce: linear RGB lerp (probe #9)
            return {float(lerp(a.r, b.r, w)), float(lerp(a.g, b.g, w)), float(lerp(a.b, b.b, w)), 1.f};
    }
}

void blendPalette(int mode, const Palette& a, const Palette& b, double w, Palette& out) {
    for (std::size_t k = 0; k < a.size(); ++k) out[k] = blendColor(mode, a[k], b[k], w);
}

// ---------------------------------------------------------------- FlowField

std::array<float, 2> blendFieldPoint(int mode, float px, float py, float ax, float ay, float bx, float by, double w,
                                     double aspect) {
    if (w <= 0) return {ax, ay};
    if (w >= 1) return {bx, by};
    if (!inFrame(ax, ay) || !inFrame(bx, by)) {
        // Out-of-frame sentinel on either side: stays out of frame mid-blend
        // (the lerp of a huge value is huge), except at the ends.
        return {float(lerp(ax, bx, w)), float(lerp(ay, by, w))};
    }
    if (mode == blend::FieldPolar) {
        const double Px = px * aspect, Py = py;
        const double Ax = ax * aspect, Ay = ay, Bx = bx * aspect, By = by;
        const double pp = Px * Px + Py * Py;
        const double ra = std::hypot(Ax, Ay), rb = std::hypot(Bx, By);
        constexpr double eps = 1e-4;
        if (pp > eps * eps && ra > eps && rb > eps) {
            // m = src / p (complex): scale + rotation about the center.
            const double rp = std::sqrt(pp);
            const double ta = std::atan2(Ay, Ax), tb = std::atan2(By, Bx), tp = std::atan2(Py, Px);
            double da = ta - tp, db = tb - tp;
            double d = db - da;
            d = std::remainder(d, 2.0 * kPi);  // shortest arc
            const double ang = tp + da + d * w;
            const double mag = rp * std::exp(lerp(std::log(ra / rp), std::log(rb / rp), w));
            return {float(mag * std::cos(ang) / aspect), float(mag * std::sin(ang))};
        }
    }
    // gforce (and the crossfade bake): linear lerp of source coordinates (probe #10).
    return {float(lerp(ax, bx, w)), float(lerp(ay, by, w))};
}

FlowFieldGrid bakeFieldBlend(int mode, const FlowFieldGrid& a, const FlowFieldGrid& b, const UnionMask* mask,
                             double w) {
    FlowFieldGrid g;
    g.width = a.width;
    g.height = a.height;
    g.srcXY.resize(a.srcXY.size());
    const bool useMask = mask && mask->width == a.width && mask->height == a.height;
    const double aspect = double(a.width) / double(std::max(a.height, 1));
    for (int r = 0; r < a.height; ++r) {
        // The FlowField pixel grid (FlowFieldInstance::rowXs/rowY): edge pixels on the frame edges.
        const float py = a.height > 1 ? float(1.0 - 2.0 * r / (a.height - 1.0)) : 0.f;
        for (int c = 0; c < a.width; ++c) {
            const std::size_t i = std::size_t(r) * std::size_t(a.width) + std::size_t(c);
            const float px = a.width > 1 ? float(-1.0 + 2.0 * c / (a.width - 1.0)) : 0.f;
            const double wp = useMask ? unionWeight(w, mask->alpha[i]) : w;
            const auto s = blendFieldPoint(mode, px, py, a.srcXY[2 * i], a.srcXY[2 * i + 1], b.srcXY[2 * i],
                                           b.srcXY[2 * i + 1], wp, aspect);
            const bool ok = std::isfinite(s[0]) && std::isfinite(s[1]) && inFrame(s[0], s[1]);
            g.srcXY[2 * i] = ok ? s[0] : kOutOfFrame;
            g.srcXY[2 * i + 1] = ok ? s[1] : kOutOfFrame;
        }
    }
    return g;
}

// ---------------------------------------------------------------- WaveShape

void toMorphFrame(const scene::WaveShapeFrame& frame, double xExtent, double yExtent, MorphFrame& out) {
    out.resize(frame.paths.size());
    const float ix = float(1.0 / (xExtent != 0 ? xExtent : 1.0)), iy = float(1.0 / (yExtent != 0 ? yExtent : 1.0));
    for (std::size_t k = 0; k < frame.paths.size(); ++k) {
        const scene::WavePath& p = frame.paths[k];
        MorphPath& m = out[k];
        m.connected = frame.connected;
        m.x.resize(p.x.size());
        m.y.resize(p.y.size());
        for (std::size_t j = 0; j < p.x.size(); ++j) {
            m.x[j] = p.x[j] * ix;
            m.y[j] = p.y[j] * iy;
        }
        m.pen = p.pen;
        m.width = p.width;
    }
}

double dilationFactor(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return 1.0 + 15.5 * std::pow(u, 4.16);
}

MorphPath resamplePath(const MorphPath& p, std::size_t n) {
    const std::size_t N = p.size();
    if (N == n || N == 0) return p;
    MorphPath r;
    r.connected = p.connected;
    r.x.resize(n);
    r.y.resize(n);
    r.pen.resize(n);
    r.width.resize(n);
    for (std::size_t j = 0; j < n; ++j) {
        const double pos = n > 1 ? double(j) * double(N - 1) / double(n - 1) : 0.0;
        const std::size_t i0 = std::min(std::size_t(pos), N - 1), i1 = std::min(i0 + 1, N - 1);
        const double f = pos - double(i0);
        auto at = [&](const std::vector<float>& v) { return float(lerp(v[i0], v[i1], f)); };
        r.x[j] = at(p.x);
        r.y[j] = at(p.y);
        r.pen[j] = at(p.pen);
        r.width[j] = at(p.width);
    }
    return r;
}

namespace {

bool closedLoop(const MorphPath& p) {
    const std::size_t n = p.size();
    if (n < 4 || !p.connected) return false;
    const double dx = p.x[n - 1] - p.x[0], dy = p.y[n - 1] - p.y[0];
    double extent = 0;
    for (std::size_t j = 0; j < n; ++j) extent = std::max({extent, double(std::abs(p.x[j])), double(std::abs(p.y[j]))});
    return std::hypot(dx, dy) <= std::max(1e-3, 0.02 * extent);
}

// Candidates of the shift search: (dir, s) for dir = +1 then -1, s = 0, stride, ...
std::size_t shiftStride(std::size_t m) { return m > 512 ? m / 512 : 1; }  // keep it O(512 * m)
std::size_t shiftCandidates(std::size_t m) { return 2 * ((m + shiftStride(m) - 1) / shiftStride(m)); }

// Best cyclic shift (and direction) of b's loop onto a's (both n points, last
// == first) among candidates [c0, c1): the first candidate with the smallest
// sum (sums never compare below `best` once they exceed it, so a candidate
// that cannot win is cut short). Over all candidates this is the serial
// search; split into ranges and reduced in order (smaller sum, earlier range
// on ties) it gives the same answer.
AlignCache::Candidates bestShiftRange(const MorphPath& a, const MorphPath& b, std::size_t c0, std::size_t c1) {
    const std::size_t m = a.size() - 1;  // distinct points
    const std::size_t stride = shiftStride(m);
    const std::size_t perDir = (m + stride - 1) / stride;
    AlignCache::Candidates r{std::numeric_limits<double>::infinity(), 0, 1};
    double best = r.sum;
    for (std::size_t c = c0; c < c1; ++c) {
        const int d = c < perDir ? 1 : -1;
        const std::size_t s = (c < perDir ? c : c - perDir) * stride;
        // k = (j + s) mod m (d > 0) or (s - j) mod m (d < 0), walked
        // without a division per point (Phase 5D).
        double sum = 0;
        std::size_t k = s;
        for (std::size_t j = 0; j < m && sum < best; ++j) {
            const double dx = a.x[j] - b.x[k], dy = a.y[j] - b.y[k];
            sum += dx * dx + dy * dy;
            if (d > 0) k = k + 1 == m ? 0 : k + 1;
            else k = k == 0 ? m - 1 : k - 1;
        }
        if (sum < best) {
            best = sum;
            r = {sum, int(s), static_cast<signed char>(d)};
        }
    }
    return r;
}

// Resamples `p` to n points into `dst` (resamplePath's arithmetic), reusing storage.
void resampleInto(const MorphPath& p, std::size_t n, MorphPath& dst) {
    const std::size_t N = p.size();
    dst.connected = p.connected;
    if (N == n || N == 0) {
        dst.x.assign(p.x.begin(), p.x.end());
        dst.y.assign(p.y.begin(), p.y.end());
        dst.pen.assign(p.pen.begin(), p.pen.end());
        dst.width.assign(p.width.begin(), p.width.end());
        return;
    }
    dst.x.resize(n);
    dst.y.resize(n);
    dst.pen.resize(n);
    dst.width.resize(n);
    for (std::size_t j = 0; j < n; ++j) {
        const double pos = n > 1 ? double(j) * double(N - 1) / double(n - 1) : 0.0;
        const std::size_t i0 = std::min(std::size_t(pos), N - 1), i1 = std::min(i0 + 1, N - 1);
        const double f = pos - double(i0);
        auto at = [&](const std::vector<float>& v) { return float(lerp(v[i0], v[i1], f)); };
        dst.x[j] = at(p.x);
        dst.y[j] = at(p.y);
        dst.pen[j] = at(p.pen);
        dst.width[j] = at(p.width);
    }
}

// Recomputes cache->shift/dir for the matched paths.
void computeShifts(const MorphFrame& from, const MorphFrame& to, std::size_t matched, AlignCache& c,
                   scene::WorkerPool* pool) {
    c.shift.assign(matched, 0);
    c.dir.assign(matched, 0);
    if (c.ra.size() < matched) c.ra.resize(matched);
    if (c.rb.size() < matched) c.rb.resize(matched);
    auto prepare = [&](std::size_t k) {
        const std::size_t n = std::max(from[k].size(), to[k].size());
        resampleInto(from[k], n, c.ra[k]);
        resampleInto(to[k], n, c.rb[k]);
    };
    if (pool && matched > 1) pool->run(int(matched), [&](int k, int) { prepare(std::size_t(k)); });
    else
        for (std::size_t k = 0; k < matched; ++k) prepare(k);
    // Chunks of candidates over all closed pairs: one dispatch for any mix of
    // many small loops and a few large ones.
    constexpr std::size_t kChunkWork = 1u << 15;  // point pairs per chunk (about 30 us)
    c.chunkBase.assign(matched + 1, 0);
    int chunks = 0;
    for (std::size_t k = 0; k < matched; ++k) {
        c.chunkBase[k] = chunks;
        if (closedLoop(c.ra[k]) && closedLoop(c.rb[k])) {
            const std::size_t m = c.ra[k].size() - 1;
            const std::size_t cand = shiftCandidates(m);
            const std::size_t perChunk = std::max<std::size_t>(1, kChunkWork / std::max<std::size_t>(m, 1));
            chunks += int(pool ? (cand + perChunk - 1) / perChunk : 1);
        }
    }
    c.chunkBase[matched] = chunks;
    c.chunkBest.resize(std::size_t(chunks));
    auto runChunk = [&](int ch) {
        const std::size_t k = std::size_t(std::upper_bound(c.chunkBase.begin(), c.chunkBase.end(), ch) -
                                          c.chunkBase.begin()) - 1;
        const std::size_t first = std::size_t(c.chunkBase[k]), count = std::size_t(c.chunkBase[k + 1]) - first;
        const std::size_t cand = shiftCandidates(c.ra[k].size() - 1);
        const std::size_t i = std::size_t(ch) - first;
        c.chunkBest[std::size_t(ch)] = bestShiftRange(c.ra[k], c.rb[k], cand * i / count, cand * (i + 1) / count);
    };
    if (pool && chunks > 1) pool->run(chunks, [&](int ch, int) { runChunk(ch); });
    else
        for (int ch = 0; ch < chunks; ++ch) runChunk(ch);
    for (std::size_t k = 0; k < matched; ++k) {
        if (c.chunkBase[k] == c.chunkBase[k + 1]) continue;  // not a closed pair
        AlignCache::Candidates best{std::numeric_limits<double>::infinity(), 0, 1};
        for (int ch = c.chunkBase[k]; ch < c.chunkBase[k + 1]; ++ch)
            if (c.chunkBest[std::size_t(ch)].sum < best.sum) best = c.chunkBest[std::size_t(ch)];
        c.shift[k] = best.shift;
        c.dir[k] = best.dir;
    }
}

void dilate(MorphPath& p, double f) {
    for (std::size_t j = 0; j < p.size(); ++j) {
        p.x[j] = float(p.x[j] * f);
        p.y[j] = float(p.y[j] * f);
    }
}

}  // namespace

namespace {

// Copies `p` into `dst`, reusing dst's storage.
void assignPath(MorphPath& dst, const MorphPath& p) {
    dst.connected = p.connected;
    dst.x.assign(p.x.begin(), p.x.end());
    dst.y.assign(p.y.begin(), p.y.end());
    dst.pen.assign(p.pen.begin(), p.pen.end());
    dst.width.assign(p.width.begin(), p.width.end());
}

// Index mapping of resamplePath (same arithmetic, so results are identical):
// point j of the path resampled from N to n points.
struct Tap {
    std::size_t i0 = 0, i1 = 0;
    double f = 0;
    bool exact = true;
    Tap(std::size_t N, std::size_t n, std::size_t j) {
        if (N == n) {
            i0 = i1 = j;
            return;
        }
        exact = false;
        const double pos = n > 1 ? double(j) * double(N - 1) / double(n - 1) : 0.0;
        i0 = std::min(std::size_t(pos), N - 1);
        i1 = std::min(i0 + 1, N - 1);
        f = pos - double(i0);
    }
    float operator()(const std::vector<float>& v) const { return exact ? v[i0] : float(lerp(v[i0], v[i1], f)); }
};

}  // namespace

void morphFrames(int mode, const MorphFrame& from, const MorphFrame& to, double w, MorphFrame& out,
                 AlignCache* cache, scene::WorkerPool* pool) {
    // Allocation-free in steady state: `out` keeps its paths' storage from the
    // previous frame, and resampling/alignment are index mappings, not copies.
    w = std::clamp(w, 0.0, 1.0);
    if (mode == blend::WaveCrossfade) {
        out.resize(from.size() + to.size());
        std::size_t o = 0;
        for (const MorphPath& p : from) {
            assignPath(out[o], p);
            for (float& v : out[o++].pen) v = float(v * (1.0 - w));
        }
        for (const MorphPath& p : to) {
            assignPath(out[o], p);
            for (float& v : out[o++].pen) v = float(v * w);
        }
        return;
    }
    const std::size_t matched = std::min(from.size(), to.size());
    if (cache && mode == blend::WaveAligned) {
        if (cache->shift.size() != matched || cache->age++ % 30 == 0) {
            cache->age = 1;
            computeShifts(from, to, matched, *cache, pool);
        }
    }
    out.resize(from.size() + to.size() - matched);
    for (std::size_t k = 0; k < matched; ++k) {
        const MorphPath& A = from[k];
        const MorphPath& B = to[k];
        const std::size_t n = std::max(A.size(), B.size());
        MorphPath& m = out[k];
        // An empty side (no points) takes the other side's points.
        const MorphPath& a = A.size() ? A : B;
        const MorphPath& b = B.size() ? B : A;
        // Aligned: b (resampled to n) read through its cyclic shift; last point = first.
        const bool shifted = cache && mode == blend::WaveAligned && k < cache->dir.size() && cache->dir[k] != 0 && n > 1;
        const std::size_t loopM = n - 1;
        const std::size_t shift = shifted ? std::size_t(cache->shift[k]) : 0;
        const int dir = shifted ? cache->dir[k] : 1;
        auto bIndex = [&](std::size_t j) -> std::size_t {
            if (!shifted) return j;
            const std::size_t jj = j < loopM ? j : 0;
            return dir > 0 ? (jj + shift) % loopM : (shift + loopM - jj % loopM) % loopM;
        };
        m.connected = w < 0.5 ? a.connected : b.connected;
        m.x.resize(n);
        m.y.resize(n);
        m.pen.resize(n);
        m.width.resize(n);
        if (a.size() == n && b.size() == n && !shifted) {
            // Common case: same point counts, no alignment.
            auto mix = [n, w](const float* pa, const float* pb, float* dst) {
                for (std::size_t j = 0; j < n; ++j) dst[j] = float(lerp(pa[j], pb[j], w));
            };
            mix(a.x.data(), b.x.data(), m.x.data());
            mix(a.y.data(), b.y.data(), m.y.data());
            mix(a.pen.data(), b.pen.data(), m.pen.data());
            mix(a.width.data(), b.width.data(), m.width.data());
            continue;
        }
        for (std::size_t j = 0; j < n; ++j) {
            const Tap ta(a.size(), n, j), tb(b.size(), n, bIndex(j));
            m.x[j] = float(lerp(ta(a.x), tb(b.x), w));
            m.y[j] = float(lerp(ta(a.y), tb(b.y), w));
            m.pen[j] = float(lerp(ta(a.pen), tb(b.pen), w));
            m.width[j] = float(lerp(ta(a.width), tb(b.width), w));
        }
    }
    // Unmatched paths keep their shape and full pen, dilated about the origin
    // (probe #12): outgoing by f(w), incoming by f(1 - w).
    std::size_t o = matched;
    for (std::size_t k = matched; k < from.size(); ++k) {
        assignPath(out[o], from[k]);
        dilate(out[o++], dilationFactor(w));
    }
    for (std::size_t k = matched; k < to.size(); ++k) {
        assignPath(out[o], to[k]);
        dilate(out[o++], dilationFactor(1.0 - w));
    }
}

}  // namespace reforce::engine
