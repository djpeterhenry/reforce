// Organic kind `double` (PLAN 5O.10): chaotic double pendulums released together on the phrase line. The equations
// of motion, energy, the swing on the tempo, determinism and frame-rate independence, the phrase cycle (released on
// the line, back at the start pose by the next), the fan opening, kicks, drops and builds, clock jumps, junk and cost.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "PerfBudget.hpp"
#include "arcade_harness.hpp"
#include "engine/DoublePendulum.hpp"

using namespace reforce::engine;
using namespace reforce::engine::pendulum2;

namespace {

constexpr double kPi = 3.141592653589793;

Params paramsOf(const std::string& keys) {
    reforce::scene::Environment env;
    const auto cfg = arcade_test::compile("Kind=\"double\"\n" + keys + "\n");
    reforce::scene::OrganicInstance inst(*cfg, env);
    return paramsFrom(inst);
}

// The largest distance of a pendulum's tip from the middle one's (units of the reach).
double tipSpread(const Cycle& c, double frac = 0) {
    const int n = c.params().num;
    double mx, my;
    c.tip(c.shown(n / 2, frac), mx, my);
    double most = 0;
    for (int k = 0; k < n; ++k) {
        double x, y;
        c.tip(c.shown(k, frac), x, y);
        most = std::max(most, std::hypot(x - mx, y - my));
    }
    return most;
}

double meanEnergy(const Cycle& c) {
    double e = 0;
    for (int k = 0; k < c.params().num; ++k)
        e += energy(c.free(k), c.gravity(), c.params().lenRatio, c.params().massRatio);
    return e / c.params().num;
}

// Angle difference modulo whole turns.
double turnDiff(double a, double b) {
    const double d = std::remainder(a - b, 2 * kPi);
    return std::abs(d);
}

bool same(const std::vector<LineSegment>& a, const std::vector<LineSegment>& b, float tol = 0) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i].ax - b[i].ax) > tol || std::abs(a[i].ay - b[i].ay) > tol ||
            std::abs(a[i].bx - b[i].bx) > tol || std::abs(a[i].by - b[i].by) > tol ||
            std::abs(a[i].penA - b[i].penA) > tol + 1e-6f || std::abs(a[i].penB - b[i].penB) > tol + 1e-6f ||
            std::abs(a[i].radius - b[i].radius) > tol + 1e-6f)
            return false;
    return true;
}

constexpr long kSpb = kStepsPerBeat;

}  // namespace

TEST_CASE("double pendulum: the equations of motion (normal mode, energy without kicks)") {
    // Small swings of equal arms and bobs: the in-phase normal mode (lower arm sqrt 2 x the upper) has
    // omega^2 = (2 - sqrt 2) g, so the first swing out and back takes 2 pi / sqrt(2 - sqrt 2).
    const double a = 0.01;
    const double slow = 2 * kPi / std::sqrt(2 - std::sqrt(2.0)), fast = 2 * kPi / std::sqrt(2 + std::sqrt(2.0));
    CHECK(firstSwing(a, std::sqrt(2.0) * a, 1, 1) == doctest::Approx(slow).epsilon(2e-3));
    // The out-of-phase mode (lower arm -sqrt 2 x): omega^2 = (2 + sqrt 2) g.
    CHECK(firstSwing(a, -std::sqrt(2.0) * a, 1, 1) == doctest::Approx(fast).epsilon(2e-3));
    // Hanging still never swings.
    CHECK(std::isnan(firstSwing(0, 0, 1, 1)));

    // Energy is conserved by the free motion (RK4, 1/48 beat), through the chaos, for a whole 16-bar cycle; also
    // with other arm and bob ratios.
    for (const std::string keys :
         {"Kick=\"0\"", "Kick=\"0\" LRat=\".6\" MRat=\"2.5\"", "Kick=\"0\" Th1=\"170\" Th2=\"-170\""}) {
        CAPTURE(keys);
        Cycle c(paramsOf("Num=\"4\" " + keys));
        const double e0 = meanEnergy(c);
        // About the energy's range.
        const double scale = c.gravity() * (1 + c.params().massRatio) * (1 + c.params().lenRatio);
        double worst = 0;
        for (long s = kSpb; s <= long(c.cycleBeats() - c.gatherBeats()) * kSpb; s += kSpb) {
            c.advance(s);
            worst = std::max(worst, std::abs(meanEnergy(c) - e0) / scale);
        }
        CHECK(worst < 1e-3);
        MESSAGE(keys << ": energy drift " << worst << " of its range over " << c.cycleBeats() - c.gatherBeats()
                     << " beats");
    }
}

TEST_CASE("double pendulum: the first swing out and back takes Prd beats (the tempo sets the swing)") {
    for (double prd : {1.0, 2.0, 4.0}) {
        CAPTURE(prd);
        Cycle c(paramsOf("Num=\"1\" Kick=\"0\" Prd=\"" + std::to_string(prd) + "\""));
        // Find the upper arm's second change of direction.
        double prev = 0;
        int crossings = 0;
        double at = -1;
        for (long s = 1; s <= long(4 * prd * kSpb) && at < 0; ++s) {
            c.advance(s);
            const double w = c.free(0).w1;
            if (prev != 0 && (prev < 0) != (w < 0) && ++crossings == 2) at = double(s) / kSpb;
            if (w != 0) prev = w;
        }
        CHECK(at == doctest::Approx(prd).epsilon(0.02));
    }
}

TEST_CASE("double pendulum: released on the cycle line, swinging as one, fanning out by Div, gathered by the next") {
    const Params p = paramsOf("Num=\"16\" Cyc=\"16\" Back=\"4\" Div=\"6\"");
    Cycle c(p);
    CHECK(c.offset() > 0);
    CHECK(c.offset() < 1e-3);
    // The release: every pendulum at its start pose, at rest.
    for (int k = 0; k < p.num; ++k) {
        const State s = c.shown(k, 0);
        CHECK(s.t1 == doctest::Approx(120 * kPi / 180).epsilon(1e-3));
        CHECK(s.t2 == doctest::Approx(150 * kPi / 180).epsilon(1e-9));
        CHECK(s.w1 == 0);
    }
    double opened = -1;
    for (long s = kSpb / 4; s <= long(c.cycleBeats() - c.gatherBeats()) * kSpb; s += kSpb / 4) {
        c.advance(s);
        const double spread = tipSpread(c);
        if (opened < 0 && spread > 0.03) opened = double(s) / kSpb / 4;  // bars
        if (s < 3 * 4 * kSpb) REQUIRE(spread < 0.003);                   // the first three bars: as one
    }
    MESSAGE("the fan opens (tips 3 % of the reach apart) at bar " << opened);
    CHECK(opened > 4.0);
    CHECK(opened < 8.5);
    // Well open in the chaos: the tips spread over the reach.
    double most = 0;
    {
        Cycle d(p);
        for (long s = 9 * 4 * kSpb; s <= 15 * 4 * kSpb; s += kSpb / 2) {
            d.advance(s);
            most = std::max(most, tipSpread(d));
        }
    }
    CHECK(most > 0.8);
    // The gather: by the cycle line every pendulum is back at its start pose (whole turns aside), at rest.
    const long end = long(c.cycleBeats()) * kSpb;
    c.advance(end - 1);
    for (int k = 0; k < p.num; ++k) {
        const State at = c.shown(k, 1.0);
        const State pose = c.pose(k);
        CHECK(turnDiff(at.t1, pose.t1) < 1e-9);
        CHECK(turnDiff(at.t2, pose.t2) < 1e-9);
        CHECK(std::abs(at.w1) < 1e-9);
    }
    // A bar before the line they are still apart; it gathers smoothly (no jumps between steps).
    {
        Cycle d(p);
        d.advance(end - 4 * kSpb);
        CHECK(tipSpread(d) > 0.05);
        double lx = 0, ly = 0;
        bool first = true;
        double worst = 0;
        for (long s = end - 4 * kSpb; s < end; ++s) {
            d.advance(s);
            double x, y;
            d.tip(d.shown(0, 0), x, y);
            if (!first) worst = std::max(worst, std::hypot(x - lx, y - ly));
            lx = x, ly = y, first = false;
        }
        CHECK(worst < 0.25);  // per 1/48 beat: the fastest arms slow down into the pose
    }
}

TEST_CASE("double pendulum: kicks push all together on the beat and hold the release energy; drops add energy") {
    // Without the gather's influence: a 32-bar cycle, compare free energies.
    const Params p = paramsOf("Num=\"8\" Cyc=\"32\" Kick=\".4\"");
    Cycle c(p);
    const double e0 = meanEnergy(c);
    const double scale = c.gravity() * 4;
    // Just before each beat line (a beat after the last kick) every pendulum is back near its release energy: the
    // kicks never pump it up, however long it runs.
    double worst = 0, kicked = 0;
    for (long b = 2; b < 100; ++b) {
        c.advance(b * kSpb - 1);
        for (int k = 0; k < p.num; ++k)
            worst = std::max(worst, std::abs(energy(c.free(k), c.gravity(), 1, 1) - e0) / scale);
        c.advance(b * kSpb + kSpb / 8);
        kicked = std::max(kicked, std::abs(meanEnergy(c) - e0) / scale);
    }
    MESSAGE("kicks: right after a kick the energy is up to " << kicked
                                                             << " of its range off the release's, a beat later " << worst);
    CHECK(worst < 0.15);
    // A kick changes the motion within its eighth of a beat, the same for every pendulum.
    Cycle a(paramsOf("Num=\"8\" Kick=\".4\"")), b(paramsOf("Num=\"8\" Kick=\"0\""));
    a.advance(kSpb);
    b.advance(kSpb);
    CHECK(a.free(0).w1 == doctest::Approx(b.free(0).w1).epsilon(1e-6));  // the same up to the first beat line
    a.advance(kSpb + kSpb / 8);
    b.advance(kSpb + kSpb / 8);
    CHECK(std::abs(a.free(0).w1 - b.free(0).w1) == doctest::Approx(0.4).epsilon(0.25));
    CHECK(std::abs(a.free(0).w1 - a.free(7).w1) < 1e-4);  // still together

    // A drop: a push of Drop on every upper arm, the same way; the kicks then hold the higher energy.
    Cycle d(p), n(p);
    d.setDrop(10 * kSpb);
    d.advance(10 * kSpb + 1);
    n.advance(10 * kSpb + 1);
    CHECK(std::abs(d.free(3).w1 - n.free(3).w1) == doctest::Approx(p.drop).epsilon(0.05));
    const double hot = meanEnergy(d);
    CHECK(hot > meanEnergy(n));
    double drift = 0;
    for (long s = 11 * kSpb; s < 60 * kSpb; s += kSpb) {
        d.advance(s);
        drift = std::max(drift, std::abs(meanEnergy(d) - hot) / scale);
    }
    CHECK(drift < 0.2);
}

TEST_CASE("double pendulum: deterministic and frame-rate independent; tempo changes and clock jumps") {
    const std::string cfg = "Kind=\"double\"\nNum=\"12\"\nTrl=\"1.5\"\n";
    // The same beat reached at 24, 60 and 144 frames a second draws exactly the same (traces included).
    auto playTo = [&](double fps, double bpm, double to) {
        arcade_test::Run r(cfg);
        const double perFrame = bpm / 60.0 / fps;
        for (double b = 0; b < to; b += perFrame) r.at(b, bpm, 0, 0, 1.0 / fps);
        return r.at(to, bpm, 0, 0, 1.0 / fps);
    };
    const double when = 4 * 9 + 2.37;  // in the chaos
    const auto a = playTo(60, 120, when);
    CHECK(a.size() > 12 * 4);
    CHECK(arcade_test::allFinite(a));
    CHECK(same(a, playTo(144, 120, when)));
    CHECK(same(a, playTo(24, 120, when)));
    // The swing is in beats: a faster tempo plays the same motion faster.
    CHECK(same(a, playTo(60, 128, when)));

    // A jump into the middle of a cycle catches up over a few frames, then draws what playing through it drew.
    {
        arcade_test::Run r(cfg);
        int frames = 1;
        std::vector<LineSegment> got = r.at(when);
        double worst = 0;
        while (!same(got, a) && frames < 60) {
            const auto t0 = std::chrono::steady_clock::now();
            got = r.at(when);
            worst = std::max(worst,
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            ++frames;
        }
        CHECK(same(got, a));
        MESSAGE("a jump to beat " << when << " caught up in " << frames << " frames (worst " << worst << " ms)");
        CHECK(frames <= 8);
        PERF_BUDGET(worst < 4.0);
    }
    // Backwards (a phrase re-alignment): the cycle is played again from its line.
    {
        arcade_test::Run r(cfg);
        for (double b = 0; b < 52; b += 0.5) r.at(b);
        std::vector<LineSegment> got;
        for (int i = 0; i < 30; ++i) got = r.at(when);
        CHECK(same(got, a));
    }
    // Into the next cycle and on: the same as a run started there (traces aside: Trl 0).
    {
        const std::string plain = "Kind=\"double\"\nNum=\"6\"\nTrl=\"0\"\n";
        arcade_test::Run r(plain), s(plain);
        for (double b = 0; b < 64 + 5.5; b += 1.0 / 30) r.at(b);
        std::vector<LineSegment> got;
        for (int i = 0; i < 10; ++i) got = s.at(64 + 5.5);
        CHECK(same(r.at(64 + 5.5), got));
        // On the cycle line itself: the start pose, the same as at the very start.
        arcade_test::Run t(plain), u(plain);
        for (double b = 0; b < 64; b += 1.0 / 30) t.at(b);
        CHECK(same(t.at(64), u.at(0), 1e-3f));
    }
    // A run without the beat clock: 120 BPM from the start, so the release is the start.
    {
        arcade_test::Run r(cfg);
        reforce::engine::SpriteAudio au;  // sync off
        std::vector<LineSegment> segs;
        r.run->step(0.5, au, r.map, LineStyle{}, segs);
        CHECK(!segs.empty());
        CHECK(arcade_test::allFinite(segs));
    }
}

TEST_CASE("double pendulum: drops and builds on the clock, junk keys and huge clocks stay finite and bounded") {
    const std::string cfg = "Kind=\"double\"\nNum=\"10\"\n";
    const std::string plain = "Kind=\"double\"\nNum=\"10\"\nTrl=\"0\"\n";
    // A drop changes the motion from then on; without it the same frames.
    {
        arcade_test::Run d(cfg), n(cfg);
        for (double b = 0; b < 20; b += 0.05) {
            d.at(b, 120, b >= 10 && b < 13 ? 1.0 : 0.0);
            n.at(b);
        }
        CHECK(!same(d.at(20), n.at(20), 0.5f));
        // The next cycle releases as usual.
        std::vector<LineSegment> x, y;
        for (double b = 20; b <= 66; b += 0.05) {
            x = d.at(b);
            y = n.at(b);
        }
        CHECK(same(x, y, 1e-4f));  // two beats into the next cycle: released from the same pose
        CHECK(arcade_test::allFinite(x));
    }
    // By default a build leaves the gather to the last bar: the same frames as without it.
    {
        arcade_test::Run b(plain), n(plain);
        std::vector<LineSegment> x, y;
        for (double t = 0; t <= 64 - 6; t += 0.05) {
            x = b.at(t, 120, 0, t > 40 ? 1.0 : 0.0);
            y = n.at(t);
        }
        CHECK(same(x, y, 1e-4f));
    }
    // With BldG 1 a build lengthens the gather: two bars before the line the built-up run is already gathering.
    {
        const std::string bplain = plain + "BldG=\"1\"\n";
        arcade_test::Run b(bplain), n(bplain);
        std::vector<LineSegment> x, y;
        for (double t = 0; t <= 64 - 6; t += 0.05) {
            x = b.at(t, 120, 0, t > 40 ? 1.0 : 0.0);
            y = n.at(t);
        }
        CHECK(!same(x, y, 0.5f));
        // Both are at the pose on the line.
        for (double t = 64 - 6; t < 64; t += 0.05) {
            b.at(t, 120, 0, 1.0);
            n.at(t);
        }
        CHECK(same(b.at(64), n.at(64), 1e-3f));
    }
    // Variants, junk, huge or broken clocks.
    for (const std::string keys : {"View=\"1\" Num=\"12\" Len=\".45\"", "Num=\"4\" Trl=\"8\" Arm=\".2\"",
                             "Num=\"32\" Trl=\"16\"", "Num=\"1\"", "LRat=\".4\" MRat=\"5\" Th1=\"179\" Th2=\"0\""}) {
        CAPTURE(keys);
        arcade_test::Run r("Kind=\"double\"\n" + keys + "\n");
        std::size_t most = 0;
        for (double b = 0; b < 80; b += 0.37) {
            const auto segs = r.at(b, 120, b > 20 && b < 21 ? 1.0 : 0.0, b > 50 ? 0.8 : 0.0);
            REQUIRE(arcade_test::allFinite(segs));
            most = std::max(most, segs.size());
        }
        CHECK(most >= 3);
        CHECK(most < 40000);
    }
    for (const std::string junk : {"Num=\"1e9\" Cyc=\"-4\" Back=\"1e300\" Prd=\"0\" Div=\"-1\"",
                             "Th1=\"0 / 0\" Th2=\"1 / 0\" LRat=\"0\" MRat=\"-3\" Kick=\"1e9\" Drop=\"-1\"",
                             "Trl=\"1e9\" Wdth=\"-1\" BobW=\"1e9\" Len=\"0\" Shd=\"9\" View=\"7\" Sprd=\"1e300\"",
                             "Th1=\"0\" Th2=\"0\""}) {
        CAPTURE(junk);
        arcade_test::Run r("Kind=\"double\"\n" + junk + "\n");
        const double inf = std::numeric_limits<double>::infinity();
        for (double b : {0.0, 1.0, 7.5, -3.0, 1e300, -1e300, std::nan(""), inf, -inf, 1e15, 2.0, 63.9, 64.0})
            for (int i = 0; i < 3; ++i) REQUIRE(arcade_test::allFinite(r.at(b, 120, b > 1 ? 1.0 : 0.0, 0.5)));
    }
    arcade_test::Run r("Kind=\"double\"\nNum=\"8\"\n");
    reforce::engine::SpriteAudio a;
    a.sync.on = 1;
    for (double v : {1e300, -1e300, 1e18, -7.0, 4e12}) {
        a.sync.beat = v;
        a.sync.bar = -v * 3;
        a.sync.drop = 1;
        std::vector<LineSegment> segs;
        r.t += 1.0 / 60;
        r.run->step(r.t, a, r.map, LineStyle{}, segs);
        REQUIRE(arcade_test::allFinite(segs));
    }
}

namespace {
// A run with a real fade (the harness has none), stepped on a clock whose bar can jump (as in the dragon's tests).
struct LiveRun {
    reforce::scene::Environment env;
    std::shared_ptr<const reforce::scene::CompiledConfig> cfg;
    std::unique_ptr<OrganicRun> run;
    FrameMapping map = organicMapping(1920, 1080);
    LineStyle style{};
    double t = 0, bar = 0;
    explicit LiveRun(const std::string& keys, double fade = 1.0) : cfg(arcade_test::compile(keys)) {
        run = std::make_unique<OrganicRun>(cfg, env, "double", 0.0, fade, 600.0, nullptr);
        reforce::scene::SyncInput s;
        s.on = 1, s.bpm = 125, s.bar = 0, s.beat = 0;
        run->setHold(holdForBars(run->bars(), fade, s, 16, 4.0));
        run->followBars(holdLineBars(run->bars(), 16));
    }
    std::vector<LineSegment> frame(double bpm) {
        SpriteAudio a;
        a.sync.on = 1, a.sync.bpm = bpm, a.sync.bar = bar, a.sync.beat = 4 * bar, a.sync.phrase = bar / 16;
        std::vector<LineSegment> segs;
        run->step(t, a, map, style, segs);
        t += 1.0 / 60, bar += bpm / 240.0 / 60;
        return segs;
    }
};
}  // namespace

TEST_CASE("double pendulum: fading out from the phrase line, they stay gathered at the start pose") {
    const std::string keys = "Kind=\"double\"\nPBar=\"16\"\nNum=\"8\"\nTrl=\"0\"\n";
    LiveRun r(keys, 2.0);
    std::vector<LineSegment> first, last;
    while (r.bar < 16.0 && !r.run->fadingOut(r.t)) r.frame(125);
    CHECK(r.bar == doctest::Approx(16).epsilon(0.01));
    first = r.frame(125);
    while (r.bar < 17.0) last = r.frame(125);  // a bar into what would be the next cycle's swing
    REQUIRE(!first.empty());
    REQUIRE(first.size() == last.size());
    for (std::size_t i = 0; i < last.size(); ++i) {
        REQUIRE(std::abs(first[i].ax - last[i].ax) < 0.01f);
        REQUIRE(std::abs(first[i].by - last[i].by) < 0.01f);
    }
}

TEST_CASE("double pendulum: cost per frame at the busiest") {
    for (const std::string keys : {"Num=\"16\" Trl=\"1.5\"", "Num=\"12\" View=\"1\" Len=\".45\" Trl=\"1\"",
                             "Num=\"5\" Trl=\"8\"", "Num=\"32\" Trl=\"16\""}) {
        CAPTURE(keys);
        arcade_test::Run r("Kind=\"double\"\n" + keys + "\n");
        for (double b = 0; b < 40; b += 2.0 / 60) r.at(b);
        double worst = 0, sum = 0;
        std::vector<double> times;
        std::size_t most = 0;
        int n = 0;
        for (double b = 40; b < 56; b += 2.0 / 60, ++n) {  // in the chaos: the traces longest and most spread
            const auto t0 = std::chrono::steady_clock::now();
            const auto segs = r.at(b);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst = std::max(worst, ms);
            times.push_back(ms);
            sum += ms;
            most = std::max(most, segs.size());
        }
        // The budget is on the 99th percentile: a single worst frame measures the scheduler when ctest runs the
        // suites in parallel (one frame took 5.6 ms at a .27 ms average).
        std::sort(times.begin(), times.end());
        const double p99 = times[std::min(times.size() - 1, times.size() * 99 / 100)];
        char m[200];
        std::snprintf(m, sizeof m, "double %s: %zu segments, %.3f ms average, %.3f ms p99, %.3f ms worst", keys.c_str(),
                      most, sum / n, p99, worst);
        MESSAGE(m);
        PERF_BUDGET(p99 < 3.0);
    }
    // Loading (the first swing and the spread are found by simulation).
    const auto t0 = std::chrono::steady_clock::now();
    Cycle c(paramsOf("Num=\"32\" Div=\"12\" Cyc=\"16\""));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("load " << ms << " ms");
    PERF_BUDGET(ms < 20);
}
