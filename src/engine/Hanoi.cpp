#include "engine/Hanoi.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::engine::hanoi {
namespace {

constexpr double kPi = 3.141592653589793;

double smooth(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

double discRadius(int size, int discs) {
    discs = std::clamp(discs, kMinDiscs, kMaxDiscs);
    size = std::clamp(size, 0, discs - 1);
    return kMinR + (kMaxR - kMinR) * double(size) / double(discs - 1);
}

double pegHeight(int discs) { return std::clamp(discs, kMinDiscs, kMaxDiscs) * kThick + 0.45; }

int moveCount(int discs) { return (1 << std::clamp(discs, kMinDiscs, kMaxDiscs)) - 1; }

Settings clampSettings(Settings s) {
    s.discs = std::clamp(s.discs, kMinDiscs, kMaxDiscs);
    s.each = std::isfinite(s.each) ? std::clamp(s.each, 0.25, 16.0) : 1.0;
    const double need = moveCount(s.discs) * s.each;
    s.beats = std::isfinite(s.beats) ? std::clamp(s.beats, need + 0.25, need + 512.0) : need + s.each;
    s.reset = s.reset == 1 ? 1 : 0;
    return s;
}

Style makeStyle(double lift, double flip) {
    Style st;
    st.lift = std::isfinite(lift) ? std::clamp(lift, 0.0, 3.0) : 1.0;
    st.flips = std::isfinite(flip) ? int(std::lround(2.0 * std::clamp(flip, 0.0, 1.0))) : 0;
    return st;
}

Plan::Plan(const Settings& settings) : s_(clampSettings(settings)) {
    const int n = s_.discs;
    // The stacks (bottom first) while the canonical solve (peg 0 -> 2) is played.
    std::array<std::array<int8_t, kMaxDiscs>, kPegs> stack{};
    std::array<int, kPegs> height{};
    for (int d = n - 1; d >= 0; --d) stack[0][std::size_t(height[0]++)] = int8_t(d);
    auto snapshot = [&](int k) {
        for (int p = 0; p < kPegs; ++p)
            for (int l = 0; l < height[std::size_t(p)]; ++l) {
                const int d = stack[std::size_t(p)][std::size_t(l)];
                peg_[std::size_t(k)][std::size_t(d)] = int8_t(p);
                level_[std::size_t(k)][std::size_t(d)] = int8_t(l);
            }
    };
    snapshot(0);
    // The minimal solve, iteratively (an explicit stack of sub-towers): move n-1 discs aside, the largest across, the
    // n-1 back on top.
    struct Task { int8_t discs, from, to, via; bool single; };
    std::array<Task, 3 * kMaxDiscs + 4> todo{};
    int top = 0;
    todo[std::size_t(top++)] = {int8_t(n), 0, 2, 1, false};
    while (top > 0) {
        const Task t = todo[std::size_t(--top)];
        if (t.single || t.discs == 1) {
            const int from = t.from, to = t.to;
            const int d = stack[std::size_t(from)][std::size_t(height[std::size_t(from)] - 1)];
            Move m;
            m.disc = int8_t(d);
            m.from = int8_t(from);
            m.to = int8_t(to);
            m.fromLevel = int8_t(height[std::size_t(from)] - 1);
            m.toLevel = int8_t(height[std::size_t(to)]);
            --height[std::size_t(from)];
            stack[std::size_t(to)][std::size_t(height[std::size_t(to)]++)] = int8_t(d);
            moves_[std::size_t(count_++)] = m;
            snapshot(count_);
            continue;
        }
        // Pushed in reverse: the last sub-tower first.
        todo[std::size_t(top++)] = {int8_t(t.discs - 1), t.via, t.to, t.from, false};
        todo[std::size_t(top++)] = {1, t.from, t.to, t.via, true};
        todo[std::size_t(top++)] = {int8_t(t.discs - 1), t.from, t.via, t.to, false};
    }
}

double Plan::resetBeats() const {
    const double lead = this->lead();
    return lead > 0 ? std::min(lead, std::max(1.0, s_.each)) : 0.0;
}

int64_t Plan::runOf(double seed) {
    if (!std::isfinite(seed)) return 0;
    return int64_t(std::floor(std::clamp(seed, -1e15, 1e15)));
}

int Plan::startPeg(int64_t run) const {
    if (s_.reset == 1) return 0;
    return int(((2 * (run % 3)) % 3 + 3) % 3);
}

int Plan::targetPeg(int64_t run) const { return s_.reset == 1 ? 2 : (startPeg(run) + 2) % 3; }

int Plan::moveAt(double turn) const {
    if (!std::isfinite(turn)) return -2;
    turn = std::clamp(turn, 0.0, s_.beats);
    const double lead = this->lead();
    if (turn < lead) return turn < resetBeats() ? -1 : -2;
    const double m = (turn - lead) / s_.each;
    const int k = int(std::min(std::floor(m), double(count_)));
    return k < count_ ? k : -2;
}

PathPoint carry(double xa, double ya, double xb, double yb, double yc, double arc, double u) {
    PathPoint p;
    if (!(u > 0)) {
        p.x = xa;
        p.y = ya;
        return p;
    }
    if (u >= 1) {
        p.x = xb;
        p.y = yb;
        p.theta = kPi;
        return p;
    }
    const double rise = std::max(0.0, yc - ya), drop = std::max(0.0, yc - yb);
    arc = std::max(0.0, arc);
    // The arc's length by its angle (a half ellipse: vertical at both ends, so the path is smooth).
    constexpr int kN = 32;
    std::array<double, kN + 1> cum{};
    auto at = [&](double th) {
        return std::array<double, 2>{xa + (xb - xa) * 0.5 * (1.0 - std::cos(th)), yc + arc * std::sin(th)};
    };
    auto prev = at(0.0);
    for (int i = 1; i <= kN; ++i) {
        const auto q = at(kPi * i / kN);
        cum[std::size_t(i)] = cum[std::size_t(i - 1)] + std::hypot(q[0] - prev[0], q[1] - prev[1]);
        prev = q;
    }
    const double la = cum[kN];
    const double total = rise + la + drop;
    if (!(total > 1e-12)) {
        p.x = xb;
        p.y = yb;
        p.theta = kPi;
        return p;
    }
    // Time along the path: eased in (lifted from rest), arriving with speed (.9 x the mean): it lands, it doesn't
    // stop short.
    constexpr double w = 0.45;
    const double f = (1.0 - w) * u * u * (3.0 - 2.0 * u) + w * u * u;
    const double s = total * f;
    if (s <= rise) {
        p.x = xa;
        p.y = ya + s;
        return p;
    }
    if (s <= rise + la) {
        const double r = s - rise;
        const auto it = std::upper_bound(cum.begin(), cum.end(), r);
        const int i = std::clamp(int(it - cum.begin()), 1, kN);
        const double seg = cum[std::size_t(i)] - cum[std::size_t(i - 1)];
        const double fr = seg > 0 ? (r - cum[std::size_t(i - 1)]) / seg : 0.0;
        p.theta = kPi * (i - 1 + std::clamp(fr, 0.0, 1.0)) / kN;
        const auto q = at(p.theta);
        p.x = q[0];
        p.y = q[1];
        return p;
    }
    p.x = xb;
    p.y = std::max(yb, yc - (s - rise - la));
    p.theta = kPi;
    return p;
}

Frame Plan::frameAt(double seed, double turn, const Controls& c) const {
    Frame f;
    const int n = s_.discs;
    f.discs = n;
    const int64_t run = runOf(seed);
    f.start = startPeg(run);
    f.target = targetPeg(run);
    if (!std::isfinite(turn)) turn = s_.beats;  // (NaN: solved)
    turn = std::clamp(turn, 0.0, s_.beats);
    f.turn = turn;
    const double lead = this->lead();
    f.lead = lead;
    // Canonical pegs (0 -> 2) to this run's.
    const int perm[3] = {f.start, 3 - f.start - f.target, f.target};
    const double hover = std::isfinite(c.hover) ? std::clamp(c.hover, 0.0, 1.0) : 0.0;
    const Style st = makeStyle(c.style.lift, double(std::clamp(c.style.flips, 0, 2)) / 2.0);
    auto restY = [&](int level) {
        double y = kThick * (level + 0.5) + hover * kHover * level;
        // A build: the open stack bobs a little, more towards the top.
        if (hover > 0)
            y += hover * 0.025 * double(level) / double(n - 1) * std::sin(2.0 * kPi * (turn / 2.0 + 0.37 * level));
        return y;
    };
    const double pegTop = pegHeight(n);
    const double yc = pegTop + kClear + kThick / 2;  // a carried disc's center once its bottom clears the pegs
    auto landed = [](Disc& d, double tau) {
        if (!(tau >= 0) || tau > 1.0) return;
        d.squash = 1.0 - 0.08 * std::sin(2.0 * kPi * tau / 0.2) * std::exp(-tau / 0.08);
        d.glint = std::max(d.glint, std::exp(-tau / 0.15));
    };

    int done = 0;
    if (turn < lead) {
        f.resetting = true;
    } else if (turn >= s_.beats) {
        done = count_;
        f.solved = true;
    } else {
        done = int(std::min(std::floor((turn - lead) / s_.each), double(count_)));
    }
    f.done = done;
    for (int d = 0; d < n; ++d) {
        Disc& o = f.disc[std::size_t(d)];
        o.size = d;
        o.peg = perm[pegAfter(done, d)];
        o.x = pegX(o.peg);
        o.y = restY(levelAfter(done, d));
    }
    if (f.resetting) {
        const double rb = resetBeats();
        const double u = rb > 0 ? turn / rb : 1.0;
        if (u < 1) {
            if (s_.reset == 1) {
                // The whole stack off the right peg, back over to the left one, landing on the reset's end.
                const PathPoint p = carry(pegX(2), 0.0, pegX(0), 0.0, pegTop + kClear, (0.35 + 0.2) * st.lift, u);
                for (int d = 0; d < n; ++d) {
                    Disc& o = f.disc[std::size_t(d)];
                    o.peg = 0;
                    o.x = p.x;
                    o.y += p.y;
                    o.moving = true;
                }
            } else {
                // The solved tower springs open (the top the most) and shuts again on the line.
                const double open = 0.14 * std::max(0.25, st.lift) * 4.0 * u * (1.0 - u);
                for (int d = 0; d < n; ++d) {
                    Disc& o = f.disc[std::size_t(d)];
                    o.y += open * levelAfter(0, d);
                    o.moving = levelAfter(0, d) > 0;
                }
            }
        }
        // The last run's final disc landed on the line (turn 0); every disc lands at the reset's end.
        if (count_ > 0) landed(f.disc[std::size_t(moves_[std::size_t(count_ - 1)].disc)], turn);
        if (u >= 1)
            for (int d = 0; d < n; ++d) landed(f.disc[std::size_t(d)], turn - rb);
    } else {
        if (done < count_) {
            // Move `done` in flight.
            const double u = (turn - lead) / s_.each - done;
            const Move& m = moves_[std::size_t(done)];
            Disc& o = f.disc[std::size_t(m.disc)];
            f.move = done;
            const int from = perm[m.from], to = perm[m.to];
            const double xa = pegX(from), xb = pegX(to);
            const double r = discRadius(m.disc, n);
            // A flip needs the disc's radius of room at the arc's middle (the flip turns over between .3 pi and .7 pi,
            // where the arc is at least .81 of its height).
            const double arc = (0.3 + 0.1 * std::abs(to - from)) * st.lift + (st.flips > 0 ? 1.25 * r : 0.0);
            const PathPoint p = carry(xa, restY(m.fromLevel), xb, restY(m.toLevel), yc, arc, u);
            o.peg = u > 0 ? to : from;
            o.x = p.x;
            o.y = p.y;
            o.moving = u > 0;
            const double dir = xb > xa ? 1.0 : -1.0;
            // Swung over (the leading edge up, then down; no more than the arc leaves room for: its edge dips
            // r sin(swing) where the arc has risen arc sin(theta)), and tumbling forward on a flip.
            const double swing = std::min(0.12, 0.4 * arc / r);
            o.tilt = dir * swing * std::sin(2.0 * p.theta) -
                     dir * kPi * st.flips * smooth(0.3 * kPi, 0.7 * kPi, p.theta);
        }
        if (done >= 1) landed(f.disc[std::size_t(moves_[std::size_t(done - 1)].disc)], turn - landing(done));
        if (done == 0) {
            const double rb = resetBeats();
            for (int d = 0; d < n; ++d) landed(f.disc[std::size_t(d)], turn - rb);
        }
    }
    // Squashed about its bottom.
    for (int d = 0; d < n; ++d) {
        Disc& o = f.disc[std::size_t(d)];
        o.y -= (1.0 - o.squash) * kThick / 2;
    }
    return f;
}

}  // namespace reforce::engine::hanoi
