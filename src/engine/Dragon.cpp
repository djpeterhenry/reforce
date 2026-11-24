// Organic kind `dragon` (PLAN 5O.9): the paper-folding dragon unfolding on the beat. See Dragon.hpp.
#include "engine/Dragon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "engine/arcade/Arcade.hpp"

namespace reforce::engine::dragon {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kClock = 1e12;  // beats / bars are clamped to +-this (whole cycle numbers stay exact in int64)

std::uint64_t mix64(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
double smooth(double a) {
    a = std::clamp(a, 0.0, 1.0);
    return a * a * (3 - 2 * a);
}

}  // namespace

int turn(std::uint64_t j) {
    if (j == 0) return 0;
    int tz = 0;
    while (!((j >> tz) & 1)) ++tz;
    return ((j >> (tz + 1)) & 1) ? +1 : -1;
}

std::vector<P2> unfold(int gens, double angle) {
    gens = std::clamp(gens, 0, kMaxGens);
    std::vector<P2> p;
    p.reserve((std::size_t(1) << gens) + 1);
    p.push_back({0, 0});
    p.push_back({1, 0});
    const double c = std::cos(angle), s = std::sin(angle);
    for (int g = 0; g < gens; ++g) {
        const std::size_t n = p.size() - 1;  // the end point's index
        const P2 e = p[n];
        for (std::size_t i = n; i-- > 0;) {
            const double dx = p[i].x - e.x, dy = p[i].y - e.y;
            p.push_back({e.x + c * dx - s * dy, e.y + s * dx + c * dy});
        }
    }
    return p;
}

Params paramsFrom(const scene::OrganicInstance& in) {
    using arcade::keyCount;
    using arcade::keyNum;
    Params p;
    p.num = keyCount(in, "NUM", p.num, 1, 4);
    p.gens = keyCount(in, "GENS", p.gens, 0, kMaxGens);
    while (p.gens > 0 && (std::int64_t(p.num) << p.gens) > kMaxSegments) --p.gens;
    p.fold = keyNum(in, "FOLD", p.fold, 0.125, 64);
    p.cycBars = keyNum(in, "CYC", p.cycBars, 0.25, 256);
    p.back = keyNum(in, "BACK", p.back, 0, 64);
    p.swing = keyNum(in, "SWNG", p.swing, 0.05, 1);
    p.stagger = keyNum(in, "STAG", p.stagger, 0, 1);
    p.angle = keyNum(in, "ANGL", p.angle, -179, 179);
    p.round = keyNum(in, "RND", p.round, 0, 0.45);
    p.width = keyNum(in, "WDTH", p.width, 0, 100);
    p.thin = keyNum(in, "THIN", p.thin, 0, 10);
    p.shade = keyNum(in, "SHD", p.shade, 0, 1);
    p.flash = keyNum(in, "FLSH", p.flash, 0, 2);
    p.rise = keyNum(in, "RISE", p.rise, 0, 1);
    p.settle = keyNum(in, "SETL", p.settle, 0, 2);
    p.run = keyNum(in, "RUN", p.run, 0, 2);
    p.spread = keyNum(in, "SPRD", p.spread, 0, 1);
    p.spin = keyCount(in, "SPIN", p.spin, 0, 1);
    p.burst = keyNum(in, "BRST", p.burst, 0, 16);
    p.fill = keyNum(in, "FILL", p.fill, 0.1, 2);
    p.vary = keyNum(in, "VARY", 1, 0, 1) >= 0.5;
    return p;
}

// ------------------------------------------------------------------ schedule

Schedule::Schedule(const Params& p) : p_(p) {
    cycBeats_ = 4.0 * p_.cycBars;
    // The last dragon's last fold lands at (top + (num - 1) stagger) x fold, before the refold starts.
    const double lag = (p_.num - 1) * p_.stagger;
    const double room = (cycBeats_ - p_.back) / p_.fold - lag;
    top_ = std::clamp(int(std::floor(room + 1e-9)), 0, p_.gens);
}

double Schedule::ease(double s) {
    s = std::clamp(s, 0.0, 1.0);
    return s * s;  // ease in: the flap speeds up and snaps flat on the line
}

double Schedule::gensAt(double u, int k) const {
    if (!std::isfinite(u)) return 0;
    u = std::clamp(u, 0.0, cycBeats_);
    const double refold = cycBeats_ - p_.back;
    if (u >= refold) {  // all dragons fold back together, landing on the cycle line as one segment
        if (p_.back <= 0) return 0;
        return top_ * (1 - smooth((u - refold) / p_.back));
    }
    const double f = u / p_.fold - k * p_.stagger;  // fold intervals since the cycle line, for dragon k
    if (f <= 0) return 0;
    const double whole = std::floor(f);
    if (whole >= top_) return top_;
    // Fold whole + 1 swings over the last Swng of its interval and lands on the line.
    const double s = (f - whole - (1 - p_.swing)) / p_.swing;
    return whole + ease(s);
}

double Schedule::gensWithDrop(double u, double sinceDrop, double uDrop, int k) const {
    const double normal = gensAt(u, k);
    if (!(sinceDrop >= 0) || top_ == 0) return normal;
    const double refold = cycBeats_ - p_.back;
    if (sinceDrop < p_.burst) {
        // Back to one segment on the drop, then every generation at once, fastest at the start.
        const double x = sinceDrop / p_.burst;
        const double b = top_ * (1 - (1 - x) * (1 - x));
        return u >= refold ? std::min(b, normal) : b;
    }
    if (uDrop < refold && u < refold) return top_;  // stays fully open until the refold
    return normal;
}

// ------------------------------------------------------------------ the body

namespace {

class Dragon : public OrganicBody {
public:
    Dragon(scene::OrganicInstance& in, std::uint64_t seed)
        : p_(paramsFrom(in)), sched_(p_), seed_(seed) {
        base_ = unfold(p_.gens, p_.angle * kPi / 180.0);
        // Generation g's chord (start to end) direction, for the view that turns with the curve.
        for (int g = 0; g <= p_.gens; ++g) {
            const P2& e = base_[std::size_t(1) << g];
            chord_[g] = std::atan2(e.y, e.x);
        }
        for (int g = 1; g <= p_.gens; ++g)  // unwrapped, so the view turns the short way between generations
            while (chord_[g] - chord_[g - 1] > kPi) chord_[g] -= 2 * kPi;
        for (int g = 1; g <= p_.gens; ++g)
            while (chord_[g] - chord_[g - 1] < -kPi) chord_[g] += 2 * kPi;
        offset_ = bestOffset();
        // Each generation's half extents in its own view (all dragons at that generation, at rest).
        for (int g = 0; g <= p_.gens; ++g) {
            const double v = offset_ - (p_.spin == 0 ? chord_[g] : chord_[p_.gens]);
            double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
            for (int k = 0; k < p_.num; ++k) {
                const double ra = 2 * kPi * k / p_.num + v, cr = std::cos(ra), sr = std::sin(ra);
                for (std::size_t i = 0; i <= (std::size_t(1) << g); ++i) {
                    const double x = cr * base_[i].x - sr * base_[i].y, y = sr * base_[i].x + cr * base_[i].y;
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
            }
            halfW_[g] = std::max(1e-9, 0.5 * (x1 - x0));
            halfH_[g] = std::max(1e-9, 0.5 * (y1 - y0));
        }
    }

    void step(const OrganicStep& s, OrganicCanvas& c) override {
        using arcade::finiteOr;
        const double beat = std::clamp(finiteOr(s.beat, 0), -kClock, kClock);
        const double bar = std::clamp(finiteOr(s.bar, beat / 4), -kClock, kClock);
        const double drop = arcade::dropOf(s), build = arcade::buildOf(s);

        // The cycle: whole cycles of Cyc bars on the (phrase-aligned) bar clock.
        const double cyc = p_.cycBars;
        const double cf = std::floor(bar / cyc);
        const std::int64_t cycle = std::int64_t(cf);
        double u = std::clamp((bar - cf * cyc) * 4.0, 0.0, sched_.cycleBeats());

        // Fading out from a cycle line (the end of its PBar, which follows the live clock): one still segment from
        // the line on, so the fade never shows the next cycle beginning. A fade-out that began mid-cycle (another
        // object took over) plays on as it is.
        if (s.ending) {
            if (!ending_) {
                ending_ = true;
                endCycle_ = cycle;
                endU_ = u;
            }
            const bool fromLine = endU_ <= 1.0 && cycle == endCycle_;
            const bool beforeLine = endU_ >= sched_.cycleBeats() - 1.0 && cycle > endCycle_;
            if (fromLine || beforeLine) {
                u = 0;
                haveDrop_ = false;
            }
        } else {
            ending_ = false;
        }

        // A drop: SYNC_DROP rising past .5. Forgotten when the clock runs back before it or in another cycle.
        if (drop >= 0.5 && lastDrop_ < 0.5) {
            dropBeat_ = beat;
            dropU_ = u;
            dropCycle_ = cycle;
            haveDrop_ = true;
        }
        lastDrop_ = drop;
        if (haveDrop_ && (beat < dropBeat_ || cycle != dropCycle_ || (ending_ && u == 0))) haveDrop_ = false;
        const double since = haveDrop_ ? beat - dropBeat_ : -1;

        // Each dragon's continuous generations; the view follows the most grown.
        const int num = p_.num;
        double gk[4] = {0, 0, 0, 0};
        double gv = 0;
        for (int k = 0; k < num; ++k) {
            gk[k] = std::clamp(sched_.gensWithDrop(u, since, dropU_, k), 0.0, double(p_.gens));
            gv = std::max(gv, gk[k]);
        }

        // A seeded mirror and half turn per cycle (Vary); both keep the framing.
        bool mirror = false, flip = false;
        if (p_.vary) {
            const std::uint64_t h = mix64(seed_ ^ mix64(std::uint64_t(cycle) * 0x9e3779b97f4a7c15ULL + 17));
            mirror = (h & 1) != 0;
            flip = (h & 2) != 0;
        }

        // Points of every dragon this frame, in curve units (before the view).
        pts_.clear();
        starts_.clear();
        for (int k = 0; k < num; ++k) {
            const double G = gk[k];
            int g = std::min(int(std::floor(G)), p_.gens);
            double e = G - g;
            if (g >= p_.gens) {
                g = p_.gens;
                e = 0;
            }
            // A breakdown makes the flap hesitate: it trembles mid-swing (still landing exactly).
            double ang = e * p_.angle;
            if (build > 0 && e > 0) ang += build * 9.0 * std::sin(2 * kPi * 6 * beat) * 4 * e * (1 - e);
            const double a = ang * kPi / 180.0, ca = std::cos(a), sa = std::sin(a);
            const double ra = 2 * kPi * k / num, cr = std::cos(ra), sr = std::sin(ra);
            const std::size_t n = std::size_t(1) << g;
            starts_.push_back(pts_.size());
            auto put = [&](double x, double y) {
                pts_.push_back({cr * x - sr * y, sr * x + cr * y});
            };
            for (std::size_t i = 0; i <= n; ++i) put(base_[i].x, base_[i].y);
            if (e > 0) {  // the moving copy, from the end point outwards, turned by `a` about it
                const P2 en = base_[n];
                for (std::size_t i = n; i-- > 0;) {
                    const double dx = base_[i].x - en.x, dy = base_[i].y - en.y;
                    put(en.x + ca * dx - sa * dy, en.y + sa * dx + ca * dy);
                }
            }
        }
        starts_.push_back(pts_.size());

        // The view: turn so generation gv's chord keeps its direction (Spin 0; the last generation's with Spin 1, so
        // only the full curve sits still), at the angle that fills the frame best; then fit the box to the frame.
        double ch = chord_[p_.gens];
        if (p_.spin == 0) {
            const int g0 = std::min(int(std::floor(gv)), p_.gens);
            const double e0 = gv - g0;
            ch = g0 >= p_.gens ? chord_[p_.gens] : chord_[g0] + e0 * (chord_[g0 + 1] - chord_[g0]);
        }
        const double view = offset_ - ch;
        const double cv = std::cos(view), sv = std::sin(view);
        const double fx = flip ? -1.0 : 1.0, fy = (flip ? -1.0 : 1.0) * (mirror ? -1.0 : 1.0);
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        for (P2& q : pts_) {
            const double x = fx * (cv * q.x - sv * q.y), y = fy * (sv * q.x + cv * q.y);
            q = {x, y};
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
        const double hw = std::clamp(c.halfWidth(), 0.25, 8.0) * p_.fill, hh = p_.fill;
        const double bw = std::max(1e-9, 0.5 * (x1 - x0)), bh = std::max(1e-9, 0.5 * (y1 - y0));
        // The scale: from generation g's fit to g + 1's (geometric), so the zoom runs steadily through a fold; but
        // never so large that the swinging copy (whose box is bigger mid-swing) leaves the frame itself (fill 1).
        const int gi = std::min(int(std::floor(gv)), p_.gens);
        const int gj = std::min(gi + 1, p_.gens);
        const double ei = gv - gi;
        const double ki = std::min(hw / halfW_[gi], hh / halfH_[gi]);
        const double kj = std::min(hw / halfW_[gj], hh / halfH_[gj]);
        const double k = std::min(ki * std::pow(kj / ki, ei), std::min(hw / bw, hh / bh) / p_.fill);
        const double mx = 0.5 * (x0 + x1), my = 0.5 * (y0 + y1);
        for (P2& q : pts_) q = {(q.x - mx) * k, (q.y - my) * k};

        // Width: thinner as segments get shorter on screen, so the folds stay apart.
        const double segPx = k * c.pixelsPerUnit();
        const double w = std::min(p_.width, std::max(0.4, p_.thin * segPx));

        // Intensities: by the generation a segment was born in (the newest brightest), the newest copy flashing as
        // it lands, a band running along the curve once per bar, each dragon its own level.
        const double denom = std::max(1.0, gv);
        const double runAt = bar - std::floor(bar);
        const double dropFlash = 0.6 * drop;
        for (int d = 0; d < num; ++d) {
            const std::size_t b = starts_[std::size_t(d)], e = starts_[std::size_t(d) + 1];
            if (e - b < 2) continue;
            const double G = gk[d];
            const int landed = std::min(int(std::floor(G)), p_.gens);
            // Beats since generation `landed` landed: the fold line it landed on, in this dragon's own fold lines.
            const double sinceLand =
                landed > 0 ? std::max(0.0, (u / p_.fold - d * p_.stagger) - landed) * p_.fold : 1e9;
            const double fl = p_.flash * std::exp(-4.0 * sinceLand);
            // Neighbors around the center differ by at least a third of Sprd (with four: 0, 2/3, 1/3, 1).
            static constexpr double kOrder[4][4] = {
                {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 0.5, 1, 0}, {0, 2.0 / 3, 1.0 / 3, 1}};
            const double level = 1.0 - p_.spread * kOrder[num - 1][d];
            // Per birth generation (room for the running band: the rest dims by up to half).
            double byBorn[kMaxGens + 2];
            for (int g = 0; g <= kMaxGens + 1; ++g) {
                const double r = std::min(1.0, g / denom);
                byBorn[g] = level * (1.0 - p_.shade * (1.0 - std::pow(r, 1.5))) * (1.0 - 0.5 * std::min(1.0, p_.run));
                if (g == landed && landed > 0) byBorn[g] += fl;
            }
            if (p_.rise > 0 || p_.settle > 0) {
                // By the beats since each generation landed: a dim ghost while it swings, rising to the glow just as
                // the next fold starts to swing, settling back to the level over the next fold interval. Generation g
                // landed on this dragon's fold line g; one born but not landed by the schedule (a drop's burst) is
                // simply at the level.
                const double level = 1.0 / (1.0 + p_.settle), peak = 1.0, ghost = (1.0 - p_.rise) * level;
                const double riseBeats = std::max(1e-6, p_.fold * (1.0 - p_.swing));
                const double settleBeats = std::max(1e-6, p_.fold);
                const double fd = u / p_.fold - d * p_.stagger;  // fold lines since the cycle line
                auto life = [&](double age) {
                    if (!(age >= 0)) return level;
                    if (age < riseBeats) return ghost + (peak - ghost) * smooth(age / riseBeats);
                    const double a = age - riseBeats;
                    if (a < settleBeats) return level + (peak - level) * (1.0 - smooth(a / settleBeats));
                    return level;
                };
                byBorn[0] *= level;
                for (int g = 1; g <= landed; ++g) byBorn[g] *= life((fd - g) * p_.fold);
                byBorn[std::min(landed + 1, kMaxGens + 1)] *= ghost;
            }
            // The running band (Peter 2026-11-23): measured along a length that grows smoothly with the unfolding
            // (2^G segments, G this dragon's continuous generations), so the band never jumps when a swinging copy
            // appears or a refold takes one away, and slides from the old curve onto the opening copy. Its center
            // travels from three widths before the curve's start (just after the bar line) to three widths past its
            // end (just before the next): one band a bar, entering at the start and leaving at the end, never split.
            constexpr double kSigma = 0.03, kOff = 3 * kSigma;  // width and run-in / run-out, shares of the length
            const double grown = std::exp2(std::clamp(G, 0.0, double(kMaxGens)));
            const double center = (-kOff + runAt * (1.0 + 2.0 * kOff)) * grown;  // in segments from the start
            const double inv = 1.0 / (2.0 * kSigma * kSigma * grown * grown);
            auto penOf = [&](std::size_t i) {
                double v = byBorn[std::min(bornIn(i), kMaxGens + 1)];
                if (p_.run > 0) {
                    const double dd = double(i) + 0.5 - center;
                    v += p_.run * std::exp(-dd * dd * inv);
                }
                return std::clamp((v + dropFlash) * s.pen, 0.0, 1.0);
            };
            drawPath(c, b, e, w, penOf);
        }
    }

    std::string summary() const override {
        char b[96];
        std::snprintf(b, sizeof b, "%d dragon(s) to %d generations, a fold per %g beat(s), cycles of %g bars", p_.num,
                      sched_.top(), p_.fold, p_.cycBars);
        return b;
    }

private:
    // The angle of the chord on screen that fits all the dragons of a well-grown generation (up to 10) into the frame
    // largest (16:9), in 1-degree steps: about 11 degrees for one dragon (it is wider than tall then).
    double bestOffset() const {
        const int g = std::min(p_.gens, 10);
        const std::size_t n = std::size_t(1) << g;
        std::vector<P2> all;
        for (int k = 0; k < p_.num; ++k) {
            const double ra = 2 * kPi * k / p_.num, cr = std::cos(ra), sr = std::sin(ra);
            for (std::size_t i = 0; i <= n; ++i)
                all.push_back({cr * base_[i].x - sr * base_[i].y, sr * base_[i].x + cr * base_[i].y});
        }
        double best = 0, bestK = -1;
        for (int d = 0; d < 180; ++d) {
            const double v = d * kPi / 180 - chord_[g], cv = std::cos(v), sv = std::sin(v);
            double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
            for (const P2& q : all) {
                const double x = cv * q.x - sv * q.y, y = sv * q.x + cv * q.y;
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
            const double k = std::min(2 * (16.0 / 9.0) / std::max(1e-9, x1 - x0), 2 / std::max(1e-9, y1 - y0));
            if (k > bestK + 1e-12) {
                bestK = k;
                best = d * kPi / 180;
            }
        }
        return best;
    }

    template <class PenOf>
    void drawPath(OrganicCanvas& c, std::size_t b, std::size_t e, double w, PenOf&& penOf) {
        const std::size_t n = e - b - 1;  // segments
        // Rounded corners: each segment drawn short of its ends, a curve (a quadratic Bezier through the corner,
        // nearly a quarter circle at 90 degrees) joining it to the next. Fewer pieces when segments are small.
        const double r = p_.round;
        const double segPx = std::hypot(pts_[b + 1].x - pts_[b].x, pts_[b + 1].y - pts_[b].y) * c.pixelsPerUnit();
        const int pieces = r <= 0 || segPx < 3 ? 0 : std::clamp(int(segPx * r / 6), 1, 8);
        for (std::size_t i = 0; i < n; ++i) {
            const P2 a = pts_[b + i], q = pts_[b + i + 1];
            const double pen = penOf(i);
            if (pieces == 0) {
                c.line(a.x, a.y, q.x, q.y, w, pen);
                continue;
            }
            const double sx = a.x + r * (q.x - a.x), sy = a.y + r * (q.y - a.y);
            const double ex = q.x - r * (q.x - a.x), ey = q.y - r * (q.y - a.y);
            c.line(i == 0 ? a.x : sx, i == 0 ? a.y : sy, i + 1 == n ? q.x : ex, i + 1 == n ? q.y : ey, w, pen);
            if (i + 1 == n) break;
            const P2 z = pts_[b + i + 2];
            const double tx = q.x + r * (z.x - q.x), ty = q.y + r * (z.y - q.y);
            const double pen2 = 0.5 * (pen + penOf(i + 1));
            double px = ex, py = ey;
            for (int j = 1; j <= pieces; ++j) {
                const double t = double(j) / pieces, m = 1 - t;
                const double bx = m * m * ex + 2 * t * m * q.x + t * t * tx;
                const double by = m * m * ey + 2 * t * m * q.y + t * t * ty;
                c.line(px, py, bx, by, w, pen2);
                px = bx;
                py = by;
            }
        }
    }

    Params p_;
    Schedule sched_;
    std::uint64_t seed_;
    std::vector<P2> base_;       // the last generation's points (every generation is a prefix)
    double chord_[kMaxGens + 1] = {};
    double offset_ = 0;  // the chord's angle on screen
    double halfW_[kMaxGens + 1] = {}, halfH_[kMaxGens + 1] = {};  // generation g's box at rest, in its view
    std::vector<P2> pts_;        // this frame's points, all dragons
    std::vector<std::size_t> starts_;
    double lastDrop_ = 0, dropBeat_ = 0, dropU_ = 0;
    std::int64_t dropCycle_ = 0;
    bool ending_ = false;            // fading out (OrganicStep::ending) since endCycle_ / endU_
    std::int64_t endCycle_ = 0;
    double endU_ = 0;
    bool haveDrop_ = false;
};

}  // namespace

std::unique_ptr<OrganicBody> makeDragon(scene::OrganicInstance& inst, std::uint64_t seed) {
    return std::make_unique<Dragon>(inst, seed);
}

}  // namespace reforce::engine::dragon
