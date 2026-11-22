#include "engine/StickerColors.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace reforce::engine {
namespace {

constexpr int kFirst = 16;  // lowest sticker intensity index: clear of the plastic and of sprite keying
constexpr int kStep = 2;    // candidate spacing
constexpr int kWobble = 3;  // +- levels for the wobble

float lin(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
float labF(float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f; }

struct Ctx {
    const LabTable& lab;
    std::array<float, kPaletteSize> wob{};
    int body;  // < 0: no plastic to keep clear of
    explicit Ctx(const LabTable& l, int b) : lab(l), body(b < 0 ? -1 : std::min(b, kPaletteSize - 1)) {
        for (int k = 0; k < kPaletteSize; ++k) {
            float w = 0;
            for (int d = -kWobble; d <= kWobble; ++d) {
                const int j = std::clamp(k + d, 0, kPaletteSize - 1);
                w = std::max(w, deltaE(lab[std::size_t(k)], lab[std::size_t(j)]));
            }
            wob[std::size_t(k)] = w;
        }
    }
    float pair(int a, int b) const {
        return deltaE(lab[std::size_t(a)], lab[std::size_t(b)]) - 0.5f * (wob[std::size_t(a)] + wob[std::size_t(b)]);
    }
    float toBody(int a) const {
        if (body < 0) return 1e9f;
        return deltaE(lab[std::size_t(a)], lab[std::size_t(body)]) - 0.5f * wob[std::size_t(a)];
    }
    // Smallest net distance of `a` to set[0..n) and to the body (a duplicate level scores -inf).
    float near(int a, const int* set, int n) const {
        float m = toBody(a);
        for (int i = 0; i < n; ++i) m = std::min(m, set[i] == a ? -1e9f : pair(a, set[i]));
        return m;
    }
    // Every pair once (each level against the ones before it) and every level against the body.
    float score(const int* set, int n) const {
        float m = 1e9f;
        for (int i = 0; i < n; ++i) m = std::min(m, near(set[i], set, i));
        return m;
    }
};

// The 15 ways to split six into three opposite pairs; index -> (p0, p1), (p2, p3), (p4, p5).
struct Matching {
    std::array<int, 6> order;
};
std::array<Matching, 15> matchings() {
    std::array<Matching, 15> out{};
    int n = 0;
    for (int b = 1; b < 6; ++b) {
        int rest[4], r = 0;
        for (int k = 1; k < 6; ++k)
            if (k != b) rest[r++] = k;
        for (int c = 1; c < 4; ++c) {
            int rr[2], q = 0;
            for (int k = 1; k < 4; ++k)
                if (k != c) rr[q++] = rest[k];
            out[std::size_t(n++)].order = {0, b, rest[0], rest[c], rr[0], rr[1]};
        }
    }
    return out;
}

}  // namespace

Lab labFromSrgb(float r, float g, float b) {
    const float R = lin(r), G = lin(g), B = lin(b);
    const float X = (0.4124f * R + 0.3576f * G + 0.1805f * B) / 0.95047f;
    const float Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
    const float Z = (0.0193f * R + 0.1192f * G + 0.9505f * B) / 1.08883f;
    const float fx = labF(X), fy = labF(Y), fz = labF(Z);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

float deltaE(const Lab& p, const Lab& q) {
    const float dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
    return std::sqrt(dl * dl + da * da + db * db);
}

void effectiveColours(const Palette& pal, const MapLut& lut, LabTable& out) {
    for (int k = 0; k < kPaletteSize; ++k) {
        // As colorize_at: m = lut at i (k is exact here), then the palette interpolated at m.
        const float y = std::clamp(lut[std::size_t(k)], 0.0f, 1.0f) * 255.0f;
        const int j0 = std::min(int(y), 254);
        const float f = y - float(j0);
        const PaletteEntry& a = pal[std::size_t(j0)];
        const PaletteEntry& b = pal[std::size_t(j0 + 1)];
        out[std::size_t(k)] = labFromSrgb(a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f);
    }
}

std::array<float, 3> colourAt(const Palette& pal, const MapLut& lut, float i) {
    const float x = std::clamp(i, 0.0f, 1.0f) * 255.0f;
    const int k0 = std::min(int(x), 254);
    const float m = std::clamp(lut[std::size_t(k0)] + (lut[std::size_t(k0 + 1)] - lut[std::size_t(k0)]) * (x - float(k0)), 0.0f, 1.0f);
    const float y = m * 255.0f;
    const int j0 = std::min(int(y), 254);
    const float f = y - float(j0);
    const PaletteEntry& a = pal[std::size_t(j0)];
    const PaletteEntry& b = pal[std::size_t(j0 + 1)];
    return {std::clamp(a.r + (b.r - a.r) * f, 0.0f, 1.0f), std::clamp(a.g + (b.g - a.g) * f, 0.0f, 1.0f),
            std::clamp(a.b + (b.b - a.b) * f, 0.0f, 1.0f)};
}

float stickerScore(const LabTable& lab, const std::array<int, 6>& level, int body) {
    const Ctx c(lab, body);
    int set[6];
    for (int i = 0; i < 6; ++i) set[i] = std::clamp(level[std::size_t(i)], 0, kPaletteSize - 1);
    return c.score(set, 6);
}

StickerPick pickStickers(const LabTable& lab, int body, const std::array<int, 6>* prev) {
    const Ctx c(lab, body);
    int best[6] = {0};
    float bestScore = -1e9f;
    // Greedy farthest-point from several starts.
    for (int start = kFirst; start < kPaletteSize; start += 8 * kStep) {
        int set[6];
        set[0] = start;
        for (int n = 1; n < 6; ++n) {
            int pick = -1;
            float pv = -1e9f;
            for (int k = kFirst; k < kPaletteSize; k += kStep) {
                const float v = c.near(k, set, n);
                if (v > pv) {
                    pv = v;
                    pick = k;
                }
            }
            set[n] = pick;
        }
        const float s = c.score(set, 6);
        if (s > bestScore) {
            bestScore = s;
            std::copy(set, set + 6, best);
        }
    }
    // Move one level at a time while it helps.
    for (int round = 0; round < 3; ++round) {
        bool moved = false;
        for (int i = 0; i < 6; ++i) {
            int keep = best[i];
            for (int k = kFirst; k < kPaletteSize; k += kStep) {
                best[i] = k;
                const float s = c.score(best, 6);
                if (s > bestScore + 1e-4f) {
                    bestScore = s;
                    keep = k;
                    moved = true;
                }
            }
            best[i] = keep;
        }
        if (!moved) break;
    }
    // Pair onto opposite faces: the matching whose adjacent (non-opposite) pairs are farthest apart.
    static const std::array<Matching, 15> kMatch = matchings();
    std::array<int, 6> paired{};
    float bestAdj = -1e9f;
    for (const Matching& m : kMatch) {
        float adj = 1e9f;
        for (int p = 0; p < 6; ++p)
            for (int q = p + 1; q < 6; ++q)
                if (p / 2 != q / 2)
                    adj = std::min(adj, c.pair(best[m.order[std::size_t(p)]], best[m.order[std::size_t(q)]]));
        if (adj > bestAdj) {
            bestAdj = adj;
            for (int p = 0; p < 6; ++p) paired[std::size_t(p)] = best[m.order[std::size_t(p)]];
        }
    }
    // Assign the three pairs to the three axes and each pair's order (48 ways), closest to `prev`.
    StickerPick out;
    out.score = bestScore;
    out.level = paired;
    if (prev) {
        static constexpr int kPerm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        float bestCost = 1e30f;
        for (const auto& perm : kPerm)
            for (int flip = 0; flip < 8; ++flip) {
                std::array<int, 6> cand{};
                for (int axis = 0; axis < 3; ++axis) {
                    const int pr = perm[axis];
                    const bool f = (flip >> axis) & 1;
                    cand[std::size_t(axis * 2)] = paired[std::size_t(pr * 2 + (f ? 1 : 0))];
                    cand[std::size_t(axis * 2 + 1)] = paired[std::size_t(pr * 2 + (f ? 0 : 1))];
                }
                float cost = 0;
                for (int i = 0; i < 6; ++i) {
                    const int p = std::clamp((*prev)[std::size_t(i)], 0, kPaletteSize - 1);
                    cost += deltaE(lab[std::size_t(cand[std::size_t(i)])], lab[std::size_t(p)]);
                }
                if (cost < bestCost) {
                    bestCost = cost;
                    out.level = cand;
                }
            }
    }
    return out;
}

// ---------------------------------------------------------------- 15 puzzle tiles

namespace {
// Measured over the 212 PNG maps: light tiles from L* 45 (dark numbers at .15 x the colour sit near L* 5), with dark
// tiles only when the light set scores under 8, gives light tiles on 176 maps, dark on 36; 15 maps stay under 10
// either way. Dark tiles stay above L* 15 so they never sink into black.
constexpr float kLightMin = 45.0f;  // light tiles (dark numbers): L* at least this
constexpr float kDarkMin = 15.0f, kDarkMax = 40.0f;  // dark tiles (light numbers)
bool allowedFor(const Lab& c, bool light) { return light ? c.l >= kLightMin : c.l >= kDarkMin && c.l <= kDarkMax; }
}  // namespace

TilePick pickTiles(const LabTable& lab, bool light, const std::array<int, 4>* prev) {
    const Ctx c(lab, -1);
    TilePick out;
    out.light = light;
    int cand[kPaletteSize / kStep + 1], nc = 0;
    for (int k = kFirst; k < kPaletteSize; k += kStep)
        if (allowedFor(lab[std::size_t(k)], light)) cand[nc++] = k;
    if (nc < 4) {
        out.score = -1e9f;
        for (int i = 0; i < 4; ++i) out.level[std::size_t(i)] = std::clamp(kFirst + 60 * i, 0, 255);
        return out;
    }
    int best[4] = {0};
    float bestScore = -1e9f;
    for (int si = 0; si < nc; si += 4) {
        int set[4];
        set[0] = cand[si];
        for (int n = 1; n < 4; ++n) {
            int pick = cand[0];
            float pv = -1e9f;
            for (int j = 0; j < nc; ++j) {
                const float v = c.near(cand[j], set, n);
                if (v > pv) pv = v, pick = cand[j];
            }
            set[n] = pick;
        }
        const float s = c.score(set, 4);
        if (s > bestScore) bestScore = s, std::copy(set, set + 4, best);
    }
    for (int round = 0; round < 3; ++round) {
        bool moved = false;
        for (int i = 0; i < 4; ++i) {
            int keep = best[i];
            for (int j = 0; j < nc; ++j) {
                best[i] = cand[j];
                const float s = c.score(best, 4);
                if (s > bestScore + 1e-4f) bestScore = s, keep = cand[j], moved = true;
            }
            best[i] = keep;
        }
        if (!moved) break;
    }
    out.score = bestScore;
    for (int i = 0; i < 4; ++i) out.level[std::size_t(i)] = best[i];
    if (prev) {  // the 24 orders: rows keep their colours as far as possible
        int p[4] = {0, 1, 2, 3};
        float bestCost = 1e30f;
        do {
            float cost = 0;
            for (int i = 0; i < 4; ++i)
                cost += deltaE(lab[std::size_t(best[p[i]])], lab[std::size_t(std::clamp((*prev)[std::size_t(i)], 0, 255))]);
            if (cost < bestCost) {
                bestCost = cost;
                for (int i = 0; i < 4; ++i) out.level[std::size_t(i)] = best[p[i]];
            }
        } while (std::next_permutation(p, p + 4));
    }
    return out;
}

float tileScore(const LabTable& lab, const std::array<int, 4>& level, bool light) {
    const Ctx c(lab, -1);
    int set[4];
    for (int i = 0; i < 4; ++i) {
        set[i] = std::clamp(level[std::size_t(i)], 0, kPaletteSize - 1);
        if (!allowedFor(lab[std::size_t(set[i])], light)) return -1e9f;  // no longer light (dark) enough
    }
    return c.score(set, 4);
}

void TilePicker::update(const Palette& palette, const MapLut& lut, double t, double minInterval) {
    if (have_ && score_ >= 2.0f && t - lastT_ < minInterval && t >= lastT_) return;
    if (have_ && std::memcmp(&palette, &lastPal_, sizeof(Palette)) == 0 &&
        std::memcmp(&lut, &lastLut_, sizeof(MapLut)) == 0)
        return;
    lastT_ = t;
    lastPal_ = palette;
    lastLut_ = lut;
    effectiveColours(palette, lut, lab_);
    const auto cur = levels(t);
    std::array<int, 4> now{};
    for (int i = 0; i < 4; ++i) now[std::size_t(i)] = std::clamp(int(std::lround(cur[std::size_t(i)] * 255.0f)), 0, 255);
    const TilePick lightPick = pickTiles(lab_, true, have_ ? &now : nullptr);
    const TilePick darkPick = pickTiles(lab_, false, have_ ? &now : nullptr);
    // Light tiles with dark numbers unless the map has no good light colours; switch only on a clear difference.
    bool light;
    if (!have_ || score_ < 2.0f) light = lightPick.score >= 8.0f || lightPick.score >= darkPick.score;
    else if (light_) light = !(lightPick.score < 6.0f && darkPick.score > lightPick.score + 4.0f);
    else light = lightPick.score >= 10.0f || lightPick.score > darkPick.score + 4.0f;
    const TilePick& p = light ? lightPick : darkPick;
    const float curScore = have_ ? tileScore(lab_, to_, light) : -1e9f;
    const bool useless = !have_ || score_ < 2.0f;
    if (light != light_ || useless || (p.score > curScore * 1.1f + 1.5f && p.level != to_)) {
        from_ = useless ? p.level : now;
        to_ = p.level;
        numFrom_ = useless ? (light ? 0.0f : 1.0f) : numeral(t);
        light_ = light;
        switchT_ = t;
        score_ = p.score;
        ++switches_;
    } else {
        score_ = curScore;
    }
    have_ = true;
}

std::array<float, 4> TilePicker::levels(double t) const {
    std::array<float, 4> out{};
    if (!have_) {
        for (int i = 0; i < 4; ++i) out[std::size_t(i)] = 0.4f + 0.2f * float(i);
        return out;
    }
    const float w = eased(t);
    for (int i = 0; i < 4; ++i)
        out[std::size_t(i)] = float((from_[std::size_t(i)] + (to_[std::size_t(i)] - from_[std::size_t(i)]) * w) / 255.0);
    return out;
}

float TilePicker::numeral(double t) const {
    if (!have_) return 0.0f;
    const float target = light_ ? 0.0f : 1.0f;
    return numFrom_ + (target - numFrom_) * eased(t);
}

float TilePicker::eased(double t) const {
    double w = ease_ > 0 ? (t - switchT_) / ease_ : 1.0;
    w = std::clamp(w, 0.0, 1.0);
    return float(w * w * (3 - 2 * w));
}

void StickerPicker::update(const Palette& palette, const MapLut& lut, double t, double minInterval) {
    if (have_ && t - lastT_ < minInterval && t >= lastT_) return;
    if (have_ && std::memcmp(&palette, &lastPal_, sizeof(Palette)) == 0 &&
        std::memcmp(&lut, &lastLut_, sizeof(MapLut)) == 0)
        return;
    lastT_ = t;
    lastPal_ = palette;
    lastLut_ = lut;
    effectiveColours(palette, lut, lab_);
    // Where the faces are now (mid-ease counts as the eased value).
    std::array<int, 6> now{};
    const std::array<float, 6> cur = levels(t);
    for (int i = 0; i < 6; ++i) now[std::size_t(i)] = std::clamp(int(std::lround(cur[std::size_t(i)] * 255.0f)), 0, 255);
    const StickerPick p = pickStickers(lab_, 0, have_ ? &now : nullptr);
    const float curScore = have_ ? stickerScore(lab_, to_) : -1e9f;
    if (!have_) {
        from_ = to_ = p.level;
        score_ = p.score;
        have_ = true;
        switchT_ = -1e30;
        return;
    }
    if (p.score > curScore * 1.1f + 1.5f && p.level != to_) {
        from_ = curScore < 2.0f ? p.level : now;  // nothing worth easing from: switch at once
        to_ = p.level;
        switchT_ = t;
        score_ = p.score;
        ++switches_;
    } else {
        score_ = curScore;
    }
}

std::array<float, 6> StickerPicker::levels(double t) const {
    std::array<float, 6> out{};
    if (!have_) {
        for (int i = 0; i < 6; ++i) out[std::size_t(i)] = 0.3f + 0.14f * float(i);
        return out;
    }
    double w = ease_ > 0 ? (t - switchT_) / ease_ : 1.0;
    w = std::clamp(w, 0.0, 1.0);
    w = w * w * (3 - 2 * w);
    for (int i = 0; i < 6; ++i)
        out[std::size_t(i)] = float((from_[std::size_t(i)] + (to_[std::size_t(i)] - from_[std::size_t(i)]) * w) / 255.0);
    return out;
}

}  // namespace reforce::engine
