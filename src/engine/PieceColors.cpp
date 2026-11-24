#include "engine/PieceColors.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace reforce::engine {
namespace {

constexpr int kN = kPieceColors;
constexpr int kFirst = 16;  // lowest intensity index: clear of sprite keying
constexpr int kStep = 2;
constexpr int kWobble = 3;

struct Ctx {
    const LabTable& lab;
    std::array<float, kPaletteSize> wob{};
    explicit Ctx(const LabTable& l) : lab(l) {
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
        if (a == b) return -1e9f;
        return deltaE(lab[std::size_t(a)], lab[std::size_t(b)]) - 0.5f * (wob[std::size_t(a)] + wob[std::size_t(b)]);
    }
    // Against the background (intensity 0: what the visuals mostly show around the figure), a little more lenient
    // than between pieces (the pieces are shaded and outlined there).
    float back(int a) const { return 1.25f * (deltaE(lab[std::size_t(a)], lab[0]) - 0.5f * wob[std::size_t(a)]); }
    float near(int a, const int* set, int n) const {
        float m = back(a);
        for (int i = 0; i < n; ++i) m = std::min(m, pair(a, set[i]));
        return m;
    }
    float score(const int* set, int n) const {
        float m = 1e9f;
        for (int i = 0; i < n; ++i) m = std::min(m, near(set[i], set, i));
        return m;
    }
};

}  // namespace

float pieceScore(const LabTable& lab, const std::array<int, kPieceColors>& level) {
    const Ctx c(lab);
    int set[kN];
    for (int i = 0; i < kN; ++i) set[i] = std::clamp(level[std::size_t(i)], 0, kPaletteSize - 1);
    return c.score(set, kN);
}

PiecePick pickPieces(const LabTable& lab, const std::array<int, kPieceColors>* prev) {
    const Ctx c(lab);
    PiecePick out;
    // Candidates light enough to shade; the floor comes down until there are enough of them.
    int cand[kPaletteSize / kStep + 1], nc = 0;
    for (float minL : {40.0f, 30.0f, 20.0f, -1.0f}) {
        nc = 0;
        for (int k = kFirst; k < kPaletteSize; k += kStep)
            if (lab[std::size_t(k)].l >= minL) cand[nc++] = k;
        out.minL = std::max(0.0f, minL);
        // Enough distinct-looking ones: at least 3x the pieces, so the set can spread out.
        if (nc >= 3 * kN) break;
    }
    if (nc < kN) {  // (a map can't have fewer than kN candidates at minL -1; guard anyway)
        for (int i = 0; i < kN; ++i) out.level[std::size_t(i)] = kFirst + 30 * i;
        out.score = -1e9f;
        return out;
    }
    int best[kN] = {0};
    float bestScore = -1e9f;
    // Greedy farthest-point picks from several starts, then moving one level at a time while it helps.
    for (int si = 0; si < nc; si += std::max(1, nc / 12)) {
        int set[kN];
        set[0] = cand[si];
        for (int n = 1; n < kN; ++n) {
            int pick = cand[0];
            float pv = -1e9f;
            for (int j = 0; j < nc; ++j) {
                const float v = c.near(cand[j], set, n);
                if (v > pv) pv = v, pick = cand[j];
            }
            set[n] = pick;
        }
        const float s = c.score(set, kN);
        if (s > bestScore) bestScore = s, std::copy(set, set + kN, best);
    }
    for (int round = 0; round < 3; ++round) {
        bool moved = false;
        for (int i = 0; i < kN; ++i) {
            int keep = best[i];
            for (int j = 0; j < nc; ++j) {
                best[i] = cand[j];
                const float s = c.score(best, kN);
                if (s > bestScore + 1e-4f) bestScore = s, keep = cand[j], moved = true;
            }
            best[i] = keep;
        }
        if (!moved) break;
    }
    out.score = bestScore;
    // Sorted by intensity, unless `prev` asks to keep the pieces' colors: then the assignment (of 5040) closest to it.
    std::sort(best, best + kN);
    for (int i = 0; i < kN; ++i) out.level[std::size_t(i)] = best[i];
    if (prev) {
        int p[kN];
        for (int i = 0; i < kN; ++i) p[i] = i;
        float bestCost = 1e30f;
        do {
            float cost = 0;
            for (int i = 0; i < kN && cost < bestCost; ++i)
                cost += deltaE(lab[std::size_t(best[p[i]])], lab[std::size_t(std::clamp((*prev)[std::size_t(i)], 0, 255))]);
            if (cost < bestCost) {
                bestCost = cost;
                for (int i = 0; i < kN; ++i) out.level[std::size_t(i)] = best[p[i]];
            }
        } while (std::next_permutation(p, p + kN));
    }
    return out;
}

void PiecePicker::update(const Palette& palette, const MapLut& lut, double t, double minInterval) {
    if (have_ && t - lastT_ < minInterval && t >= lastT_) return;
    if (have_ && std::memcmp(&palette, &lastPal_, sizeof(Palette)) == 0 &&
        std::memcmp(&lut, &lastLut_, sizeof(MapLut)) == 0)
        return;
    lastT_ = t;
    lastPal_ = palette;
    lastLut_ = lut;
    effectiveColors(palette, lut, lab_);
    std::array<int, kN> now{};
    const auto cur = levels(t);
    for (int i = 0; i < kN; ++i) now[std::size_t(i)] = std::clamp(int(std::lround(cur[std::size_t(i)] * 255.0f)), 0, 255);
    const PiecePick p = pickPieces(lab_, have_ ? &now : nullptr);
    if (!have_) {
        from_ = to_ = p.level;
        score_ = p.score;
        have_ = true;
        switchT_ = -1e30;
        return;
    }
    const float score = pieceScore(lab_, to_);
    float curScore = score;
    // A map that changes over time can turn a piece's color dark (it would shade to black): switch then.
    for (int k : to_)
        if (lab_[std::size_t(std::clamp(k, 0, 255))].l < p.minL - 8.0f) curScore = -1e9f;
    if (p.score > curScore * 1.1f + 1.5f && p.level != to_) {
        from_ = score < 2.0f ? p.level : now;  // nothing worth easing from: switch at once
        to_ = p.level;
        switchT_ = t;
        score_ = p.score;
        ++switches_;
    } else {
        score_ = curScore;
    }
}

std::array<float, kPieceColors> PiecePicker::levels(double t) const {
    std::array<float, kN> out{};
    if (!have_) {
        for (int i = 0; i < kN; ++i) out[std::size_t(i)] = 0.25f + 0.12f * float(i);
        return out;
    }
    double w = ease_ > 0 ? (t - switchT_) / ease_ : 1.0;
    w = std::clamp(w, 0.0, 1.0);
    w = w * w * (3 - 2 * w);
    for (int i = 0; i < kN; ++i)
        out[std::size_t(i)] = float((from_[std::size_t(i)] + (to_[std::size_t(i)] - from_[std::size_t(i)]) * w) / 255.0);
    return out;
}

}  // namespace reforce::engine
