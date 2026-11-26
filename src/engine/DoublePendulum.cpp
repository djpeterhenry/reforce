// Organic kind `double` (PLAN 5O.10): chaotic double pendulums released together on the phrase line. See
// DoublePendulum.hpp.
#include "engine/DoublePendulum.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>

#include "engine/arcade/Arcade.hpp"

namespace reforce::engine::pendulum2 {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kClock = 1e12;      // beats / bars are clamped to +-this (whole cycle numbers stay exact in int64)
constexpr double kStep = 1.0 / kStepsPerBeat;
constexpr double kKickWidth = 0.125;  // beats a kick's push lasts (a sin^2 pulse from the beat line)
constexpr double kVisible = 0.03;     // tip spread (share of the reach) at which the fan counts as open
constexpr double kHold = 4.0;         // how firmly the energy is held at its level (kicks fade over about a beat)

double smoother(double a) {
    a = std::clamp(a, 0.0, 1.0);
    return a * a * a * (a * (a * 6 - 15) + 10);
}

}  // namespace

Params paramsFrom(const scene::OrganicInstance& in) {
    using arcade::keyCount;
    using arcade::keyNum;
    Params p;
    p.num = keyCount(in, "NUM", p.num, 1, kMaxNum);
    p.view = keyCount(in, "VIEW", p.view, 0, 1);
    p.cycBars = keyNum(in, "CYC", p.cycBars, 1, 256);
    p.back = keyNum(in, "BACK", p.back, 0.25, 64);
    p.back = std::min(p.back, p.cycBars);  // at most a quarter of the cycle (8 beats in a build at most half)
    p.buildGather = keyNum(in, "BLDG", p.buildGather, 0, 1);
    p.period = keyNum(in, "PRD", p.period, 0.25, 64);
    p.th1 = keyNum(in, "TH1", p.th1, -180, 180);
    p.th2 = keyNum(in, "TH2", p.th2, -180, 180);
    p.div = keyNum(in, "DIV", p.div, 0.25, 256);
    p.lenRatio = keyNum(in, "LRAT", p.lenRatio, 0.2, 5);
    p.massRatio = keyNum(in, "MRAT", p.massRatio, 0.05, 20);
    p.kick = keyNum(in, "KICK", p.kick, 0, 4);
    p.drop = keyNum(in, "DROP", p.drop, 0, 12);
    p.len = keyNum(in, "LEN", p.len, 0.01, 4);
    p.spread = keyNum(in, "SPRD", p.spread, 0, 8);
    p.trail = keyNum(in, "TRL", p.trail, 0, 16);
    p.width = keyNum(in, "WDTH", p.width, 0, 50);
    p.bobW = keyNum(in, "BOBW", p.bobW, 0, 100);
    p.jointW = keyNum(in, "JNTW", p.jointW, 0, 100);
    p.traceW = keyNum(in, "TRCW", p.traceW, 0, 50);
    p.shade = keyNum(in, "SHD", p.shade, 0, 1);
    return p;
}

// ------------------------------------------------------------------ physics

State deriv(const State& s, double g, double r, double mu, double tau) {
    // Upper arm 1 long with a bob of mass 1, lower arm r with mu, angles from straight down.
    const double s1 = std::sin(s.t1), c1 = std::cos(s.t1), s2 = std::sin(s.t2), c2 = std::cos(s.t2);
    const double sd = s1 * c2 - c1 * s2, cd = c1 * c2 + s1 * s2;  // sin, cos of t1 - t2
    const double den = 2 + mu - mu * (cd * cd - sd * sd);          // 2 m1 + m2 - m2 cos 2(t1 - t2)
    const double s12 = sd * c2 - cd * s2;                           // sin(t1 - 2 t2)
    const double a1 = (-g * (2 + mu) * s1 - mu * g * s12 - 2 * sd * mu * (s.w2 * s.w2 * r + s.w1 * s.w1 * cd)) / den;
    const double a2 = 2 * sd * (s.w1 * s.w1 * (1 + mu) + g * (1 + mu) * c1 + s.w2 * s.w2 * r * mu * cd) / (r * den);
    return {s.w1, s.w2, a1 + tau, a2};
}

double energy(const State& s, double g, double r, double mu) {
    const double kin = 0.5 * (1 + mu) * s.w1 * s.w1 + 0.5 * mu * r * r * s.w2 * s.w2 +
                       mu * r * s.w1 * s.w2 * std::cos(s.t1 - s.t2);
    const double pot = -(1 + mu) * g * std::cos(s.t1) - mu * g * r * std::cos(s.t2);
    return kin + pot;
}

double firstSwing(double th1, double th2, double r, double mu) {
    // Unit g: integrate until the upper arm's velocity has changed sign twice (out and back), interpolating the
    // second crossing within its step.
    const double h = 0.005;
    State s{th1, th2, 0, 0};
    double prev = 0;
    int crossings = 0;
    auto none = [](double, const State&) { return 0.0; };
    for (int n = 1; n <= 40000; ++n) {
        const State q = rk4(s, h, 1.0, r, mu, none);
        if (!std::isfinite(q.w1)) return std::nan("");
        if (prev != 0 && ((prev < 0) != (q.w1 < 0)) && q.w1 != 0) {
            if (++crossings == 2) return (n - 1) * h + h * prev / (prev - q.w1);
        }
        if (q.w1 != 0) prev = q.w1;
        s = q;
    }
    return std::nan("");
}

// ------------------------------------------------------------------ the cycle

Cycle::Cycle(const Params& p) : p_(p) {
    r_ = p_.lenRatio;
    mu_ = p_.massRatio;
    cycBeats_ = 4.0 * p_.cycBars;
    const double a1 = p_.th1 * kPi / 180, a2 = p_.th2 * kPi / 180;
    const double t1 = firstSwing(a1, a2, r_, mu_);
    // Time scales as 1 / sqrt(g): the first swing takes Prd beats with g = (T1 / Prd)^2. A pose that never swings
    // back (hanging straight down) takes the simple pendulum's g; a pose balanced near the top (a very slow first
    // swing) is held to a simple-pendulum period of Prd / 2 at the quickest.
    const double rate = std::isfinite(t1) && t1 > 0 ? t1 / p_.period : 2 * kPi / p_.period;
    g_ = std::clamp(rate * rate, 1e-4, (4 * kPi / p_.period) * (4 * kPi / p_.period));
    range_ = 2 * g_ * (1 + mu_ + mu_ * r_);
    // Substeps per step: more when the motion is quick for the 1/48 beat step (a stiff g, a light short lower arm).
    const double quick = std::sqrt(g_ * std::max(1.0, 1.0 / r_) * (1 + mu_) / 2);
    substeps_ = std::clamp(int(std::ceil(quick / 1.4 - 1e-9)), 1, 8);

    // The spread at the release: follow a tiny difference along the reference motion (the kicks included), renormalized
    // so it stays small, and find its largest growth in tip distance up to Div bars; then choose the offset between
    // neighbors so the outermost pendulums are kVisible apart from the middle one by then.
    eps_ = 0;
    if (p_.num > 1) {
        const double d0 = 1e-9;
        const std::vector<State> v = {State{a1, a2, 0, 0}, State{a1 + d0, a2, 0, 0}};
        double logScale = 0, growth = 0;
        const double wScale = p_.period / (2 * kPi);  // rad / beat -> rad per swing
        const long until =
            long(std::lround(std::min(4.0 * p_.div, cycBeats_ - std::min(p_.back, 0.5 * cycBeats_)) * kStepsPerBeat));
        reset(p_.back);
        st_ = v;
        eRef_ = {energy(v[0], g_, r_, mu_), energy(v[1], g_, r_, mu_)};
        calibrating_ = true;
        for (long n = 0; n < until; ++n) {
            stepOnce();
            State& a = st_[0];
            State& b = st_[1];
            double ax, ay, bx, by;
            tip(a, ax, ay);
            tip(b, bx, by);
            growth = std::max(growth, std::hypot(bx - ax, by - ay) * std::exp(logScale) / d0);
            const double d1 = b.t1 - a.t1, d2 = b.t2 - a.t2, d3 = wScale * (b.w1 - a.w1), d4 = wScale * (b.w2 - a.w2);
            const double dd = std::sqrt(d1 * d1 + d2 * d2 + d3 * d3 + d4 * d4);
            if (dd > 1e-6 && std::isfinite(dd)) {
                const double f = d0 / dd;
                b = {a.t1 + d1 * f, a.t2 + d2 * f, a.w1 + (b.w1 - a.w1) * f, a.w2 + (b.w2 - a.w2) * f};
                eRef_[1] = eRef_[0] + (eRef_[1] - eRef_[0]) * f;  // its level, scaled with it
                logScale += std::log(dd / d0);
            }
        }
        calibrating_ = false;
        const double half = 0.5 * (p_.num - 1);
        eps_ = growth > 0 && std::isfinite(growth) ? kVisible / (growth * half) : 1e-3;
        eps_ = std::clamp(eps_, 1e-14, 0.02);
    }
    reset(p_.back);
}

State Cycle::pose(int k) const {
    const double off = (k - 0.5 * (p_.num - 1)) * eps_;
    return {p_.th1 * kPi / 180 + off, p_.th2 * kPi / 180, 0, 0};
}

void Cycle::reset(double gather) {
    gather_ = std::clamp(gather, 0.25, 0.5 * cycBeats_);
    st_.resize(std::size_t(p_.num));
    eRef_.resize(std::size_t(p_.num));
    for (int k = 0; k < p_.num; ++k) {
        st_[std::size_t(k)] = pose(k);
        eRef_[std::size_t(k)] = energy(st_[std::size_t(k)], g_, r_, mu_);
    }
    m1_.assign(std::size_t(p_.num), 0.0);
    m2_.assign(std::size_t(p_.num), 0.0);
    step_ = 0;
    dropStep_ = -1;
    holding_ = p_.kick > 0;
    targets_ = false;
}

double Cycle::kickPulse(double u) const {
    if (p_.kick <= 0 || u < 1) return 0;
    const double b = std::floor(u), f = u - b;
    if (f >= kKickWidth || b + kKickWidth > cycBeats_ - gather_) return 0;  // none at the release, none in the gather
    const double s = std::sin(kPi * f / kKickWidth);
    return 2.0 * p_.kick / kKickWidth * s * s;  // integrates to Kick over the pulse
}

double Cycle::push(const State& s) const {
    // Along the upper arm's momentum (smoothly: nearly equal pendulums get nearly equal pushes, so the synchronized
    // ones stay together); standing still, the way gravity pulls it.
    const double p1 = (1 + mu_) * s.w1 + mu_ * r_ * s.w2 * std::cos(s.t1 - s.t2);
    const double along = std::tanh(p1 / (0.2 * std::sqrt(range_)));
    return along - (1 - std::abs(along)) * std::tanh(std::sin(s.t1) / 0.1);
}

double Cycle::torque(const State& s, double u, int k) const {
    double tau = 0;
    const double kick = kickPulse(u);
    if (kick > 0) tau += kick * push(s);
    if (holding_) {
        // Hold the pendulum's energy at its level (the release's, a drop's): a pull along or against the upper arm's
        // momentum in proportion to the difference, so a kick's energy fades over about a beat.
        const double p1 = (1 + mu_) * s.w1 + mu_ * r_ * s.w2 * std::cos(s.t1 - s.t2);
        const double d = (energy(s, g_, r_, mu_) - eRef_[std::size_t(k)]) / range_;
        tau -= kHold * std::clamp(d, -1.0, 1.0) * p1;
    }
    return tau;
}

bool Cycle::setGather(double gather) {
    const double g = std::clamp(gather, 0.25, 0.5 * cycBeats_);
    if (targets_ || double(step_) >= std::floor((cycBeats_ - g) * kStepsPerBeat)) return false;
    gather_ = g;
    return true;
}

double Cycle::gatherWeight(double u) const { return smoother((u - (cycBeats_ - gather_)) / gather_); }

State Cycle::integrate(State s, int k, double u0, double h) const {
    const int n = substeps_;
    const double hs = h / n;
    for (int i = 0; i < n; ++i) {
        const double ui = u0 + i * hs;
        const State q = rk4(s, hs, g_, r_, mu_, [&](double x, const State& at) { return torque(at, ui + x, k); });
        if (!(std::isfinite(q.t1) && std::isfinite(q.t2) && std::isfinite(q.w1) && std::isfinite(q.w2))) break;
        s = q;
    }
    return s;
}

void Cycle::stepOnce() {
    const long n = step_;
    const double u0 = double(n) * kStep;
    const double uG = cycBeats_ - gather_;
    // A drop: a strong push on every upper arm, along its motion, and the energy held at the new level from then on.
    if (n == dropStep_ && p_.drop > 0 && u0 < uG) {
        for (std::size_t k = 0; k < st_.size(); ++k) {
            State& s = st_[k];
            s.w1 += p_.drop * push(s);
            eRef_[k] = energy(s, g_, r_, mu_);
        }
        holding_ = true;
    }
    for (std::size_t k = 0; k < st_.size(); ++k) st_[k] = integrate(st_[k], int(k), u0, kStep);
    ++step_;
    // The gather's targets: the release pose plus the whole turns nearest to where each arm is as the gather starts.
    if (!targets_ && !calibrating_ && double(step_) >= std::floor(uG * kStepsPerBeat)) {
        for (int k = 0; k < p_.num; ++k) {
            const State p = pose(k);
            const State& s = st_[std::size_t(k)];
            m1_[std::size_t(k)] = 2 * kPi * std::round((s.t1 - p.t1) / (2 * kPi));
            m2_[std::size_t(k)] = 2 * kPi * std::round((s.t2 - p.t2) / (2 * kPi));
        }
        targets_ = true;
    }
}

long Cycle::advance(long step, long budget) {
    return advanceWith(step, budget, [] {});
}

State Cycle::shown(int k, double frac) const {
    State s = st_[std::size_t(k)];
    const double u0 = double(step_) * kStep;
    frac = std::clamp(frac, 0.0, 1.0);
    if (frac > 0) s = integrate(s, k, u0, frac * kStep);
    const double u = u0 + frac * kStep;
    if (targets_ && u >= cycBeats_ - gather_) {
        const double e = gatherWeight(u);
        const State p = pose(k);
        s.t1 = (1 - e) * s.t1 + e * (p.t1 + m1_[std::size_t(k)]);
        s.t2 = (1 - e) * s.t2 + e * (p.t2 + m2_[std::size_t(k)]);
        s.w1 *= 1 - e;
        s.w2 *= 1 - e;
    }
    return s;
}

void Cycle::tip(const State& s, double& x, double& y) const {
    const double l1 = 1.0 / (1.0 + r_), l2 = r_ / (1.0 + r_);
    x = l1 * std::sin(s.t1) + l2 * std::sin(s.t2);
    y = -l1 * std::cos(s.t1) - l2 * std::cos(s.t2);
}

// ------------------------------------------------------------------ the body

namespace {

struct P2 {
    double x = 0, y = 0;
};

class DoublePendulum : public OrganicBody {
public:
    DoublePendulum(scene::OrganicInstance& in, std::uint64_t seed) : p_(paramsFrom(in)), cyc_(p_), inst_(in) {
        (void)seed;  // the motion is the physics' own: the same every run
        trailSteps_ = std::size_t(std::lround(p_.trail * kStepsPerBeat));
        trails_.assign(std::size_t(p_.num), {});
        cycSteps_ = long(std::lround(cyc_.cycleBeats() * kStepsPerBeat));
    }

    void step(const OrganicStep& s, OrganicCanvas& c) override {
        using arcade::finiteOr;
        const double beat = std::clamp(finiteOr(s.beat, 0), -kClock, kClock);
        const double bar = std::clamp(finiteOr(s.bar, beat / 4), -kClock, kClock);
        const double drop = arcade::dropOf(s), build = arcade::buildOf(s);

        // The cycle: whole cycles of Cyc bars on the (phrase-aligned) bar clock.
        const double cb = p_.cycBars;
        const double cf = std::floor(bar / cb);
        const std::int64_t cycle = std::int64_t(cf);
        const double u = std::clamp((bar - cf * cb) * 4.0, 0.0, cyc_.cycleBeats());

        // Fading out from a cycle line (the end of its PBar, which follows the live clock): the pendulums stay
        // gathered at the start pose, never released into the next cycle during the fade. A fade-out that began
        // mid-cycle (another object took over) plays on.
        bool hold = false;
        if (s.ending) {
            if (!ending_) {
                ending_ = true;
                endCycle_ = cycle;
                endU_ = u;
            }
            const bool fromLine = endU_ <= 1.0 && cycle == endCycle_;
            const bool beforeLine = endU_ >= cyc_.cycleBeats() - 1.0 && cycle > endCycle_;
            hold = fromLine || beforeLine;
        } else {
            ending_ = false;
        }

        if (!hold) {
            follow(cycle, u, drop, build);
        }

        // Draw.
        const double pen = s.pen;
        const double arm = std::clamp(arcade::frameNum(inst_, "ARM", 0.55), 0.0, 2.0);
        const double trc = std::clamp(arcade::frameNum(inst_, "TRC", 0.8), 0.0, 2.0);
        const double flash = 0.6 * drop;
        const int n = p_.num;
        const double l1 = p_.len / (1.0 + p_.lenRatio), l2 = p_.len - l1;
        joints_.resize(std::size_t(n));
        for (int k = 0; k < n; ++k) {
            const State q = hold ? cyc_.pose(k) : cyc_.shown(k, frac_);
            const P2 o = pivot(k);
            Joints& j = joints_[std::size_t(k)];
            j.o = o;
            j.e = {o.x + l1 * std::sin(q.t1), o.y - l1 * std::cos(q.t1)};
            j.t = {j.e.x + l2 * std::sin(q.t2), j.e.y - l2 * std::cos(q.t2)};
        }
        // In layers, traces under arms under bobs; in each the dimmest pendulum first, so where they overlap (all of
        // them while they swing as one) the brightest shows.
        auto level = [&](int k) { return n > 1 ? 1.0 - p_.shade * double(k) / double(n - 1) : 1.0; };
        if (p_.trail > 0 && trc > 0 && p_.traceW > 0) {
            // The tip's path over the last Trl beats, fading towards its tail.
            const double tl = std::max(1.0, double(trailSteps_));
            for (int k = n; k-- > 0;) {
                const auto& tr = trails_[std::size_t(k)];
                const P2 o = joints_[std::size_t(k)].o;
                double px = joints_[std::size_t(k)].t.x, py = joints_[std::size_t(k)].t.y;
                double prevPen = std::clamp((trc * level(k) + flash) * pen, 0.0, 1.0);
                for (std::size_t i = tr.size(); i-- > 0;) {
                    const double age = double(tr.size() - 1 - i) + frac_;
                    const double a = std::clamp(1.0 - age / tl, 0.0, 1.0);
                    const double pi = std::clamp((trc * level(k) * a * a + flash * a) * pen, 0.0, 1.0);
                    const double qx = o.x + p_.len * tr[i].x, qy = o.y + p_.len * tr[i].y;
                    if (pi > 0.002 || prevPen > 0.002) c.line(px, py, qx, qy, p_.traceW, prevPen, pi);
                    px = qx;
                    py = qy;
                    prevPen = pi;
                }
            }
        }
        for (int k = n; k-- > 0;) {
            const Joints& j = joints_[std::size_t(k)];
            const double ap = std::clamp((arm * level(k) + 0.5 * flash) * pen, 0.0, 1.0);
            if (ap <= 0) continue;
            if (p_.width > 0) {
                c.line(j.o.x, j.o.y, j.e.x, j.e.y, p_.width, ap);
                c.line(j.e.x, j.e.y, j.t.x, j.t.y, p_.width, ap);
            }
            if (p_.jointW > 0) c.dot(j.e.x, j.e.y, p_.jointW, ap);
        }
        for (int k = n; k-- > 0;) {
            const double bp = std::clamp((level(k) + flash) * pen, 0.0, 1.0);
            if (p_.bobW > 0 && bp > 0) c.dot(joints_[std::size_t(k)].t.x, joints_[std::size_t(k)].t.y, p_.bobW, bp);
        }
    }

    std::string summary() const override {
        char b[160];
        std::snprintf(b, sizeof b,
                      "%d double pendulum(s), cycles of %g bars, a swing per %g beats, spread %.2g rad, g %.3g",
                      p_.num, p_.cycBars, p_.period, cyc_.offset(), cyc_.gravity());
        return b;
    }

private:
    P2 pivot(int k) const {
        if (p_.view == 1 && p_.num > 1) return {-0.5 * p_.spread + p_.spread * double(k) / double(p_.num - 1), 0.0};
        return {0, 0};
    }

    // Brings the simulation to beat u of cycle `cycle`: on along the cycle, into the next one when the last one ran
    // to its end, else again from the release (a jump). At most kMaxStepsPerFrame pendulum-steps a frame.
    void follow(std::int64_t cycle, double u, double drop, double build) {
        const long target = std::min(cycSteps_, long(std::floor(u * kStepsPerBeat)));
        long budget = kMaxStepsPerFrame;
        const bool started = haveCycle_;
        if (!started || cycle != cycle_ || target < cyc_.step()) {
            const bool next = started && cycle == cycle_ + 1 && cyc_.step() >= cycSteps_ - 2 * kStepsPerBeat;
            if (next) {  // finish the last cycle (its last steps), keeping the traces: the motion is continuous
                budget -= (cycSteps_ - cyc_.step()) * p_.num;
                cyc_.advanceWith(cycSteps_, -1, [&] { record(); });
            } else {
                for (auto& t : trails_) t.clear();
            }
            cycle_ = cycle;
            haveCycle_ = true;
            gatherSet_ = false;
            cyc_.reset(p_.back);
        }
        // A build may lengthen the gather (BldG: up to (1 + BldG) x Back), decided once per cycle as it could first begin.
        const double early = cyc_.cycleBeats() - std::min(2.0 * p_.back, 0.5 * cyc_.cycleBeats());
        if (!gatherSet_ && u >= early) {
            gatherSet_ = true;
            const double g = p_.back * (1.0 + p_.buildGather * build);
            if (std::abs(g - cyc_.gatherBeats()) > 1e-12) setGather(g);
        }
        // A drop: SYNC_DROP rising past .5, once per cycle, before the gather.
        if (drop >= 0.5 && lastDrop_ < 0.5 && cyc_.dropStep() < 0 && u < cyc_.cycleBeats() - cyc_.gatherBeats())
            cyc_.setDrop(std::max(cyc_.step(), target));
        lastDrop_ = drop;
        cyc_.advanceWith(target, std::max<long>(budget, p_.num), [&] { record(); });
        frac_ = cyc_.step() == target && target < cycSteps_ ? std::clamp(u * kStepsPerBeat - double(target), 0.0, 1.0)
                                                            : 0.0;
    }

    // A cycle's gather is set before its steps reach the gather (the decision comes at Cyc - 2 Back at the latest);
    // otherwise the cycle is played again with it (same steps, same drop).
    void setGather(double g) {
        if (cyc_.setGather(g)) return;
        const long drop = cyc_.dropStep();
        const long at = cyc_.step();
        cyc_.reset(g);
        cyc_.setDrop(drop);
        cyc_.advance(at);
    }

    void record() {
        if (trailSteps_ == 0) return;
        for (int k = 0; k < p_.num; ++k) {
            const State q = cyc_.shown(k, 0);
            P2 t;
            cyc_.tip(q, t.x, t.y);
            auto& tr = trails_[std::size_t(k)];
            tr.push_back(t);
            while (tr.size() > trailSteps_) tr.pop_front();
        }
    }

    Params p_;
    Cycle cyc_;
    scene::OrganicInstance& inst_;
    std::size_t trailSteps_ = 0;
    long cycSteps_ = 0;
    struct Joints {
        P2 o, e, t;  // pivot, elbow, tip (tree units)
    };
    std::vector<Joints> joints_;
    std::vector<std::deque<P2>> trails_;  // per pendulum: tip positions (units of the reach) at the last steps
    std::int64_t cycle_ = 0;
    bool haveCycle_ = false;
    bool gatherSet_ = false;
    double frac_ = 0;
    double lastDrop_ = 0;
    bool ending_ = false;
    std::int64_t endCycle_ = 0;
    double endU_ = 0;
};

}  // namespace

std::unique_ptr<OrganicBody> makeDoublePendulum(scene::OrganicInstance& inst, std::uint64_t seed) {
    return std::make_unique<DoublePendulum>(inst, seed);
}

}  // namespace reforce::engine::pendulum2
