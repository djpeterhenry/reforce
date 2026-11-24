// Organic kind `dragon` (PLAN 5O.9): the paper-folding curve itself, the fold schedule on the beat clock (landing
// exactly on the fold lines, phrase-aligned cycles, drops), determinism, big clock jumps, junk input and cost.
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
#include "engine/Dragon.hpp"

using namespace reforce::engine;
using namespace reforce::engine::dragon;

namespace {

constexpr double kPi = 3.141592653589793;

// The regular paper-folding sequence by its definition: S(n+1) = S(n), R, reverse and flip S(n); R = -1.
std::vector<int> foldingSequence(int gens) {
    std::vector<int> s;
    for (int g = 0; g < gens; ++g) {
        std::vector<int> next = s;
        next.push_back(-1);
        for (auto it = s.rbegin(); it != s.rend(); ++it) next.push_back(-*it);
        s = std::move(next);
    }
    return s;
}

Params paramsOf(const std::string& keys) {
    reforce::scene::Environment env;
    const auto cfg = arcade_test::compile("Kind=\"dragon\"\n" + keys + "\n");
    reforce::scene::OrganicInstance inst(*cfg, env);
    return paramsFrom(inst);
}

bool same(const std::vector<LineSegment>& a, const std::vector<LineSegment>& b, float tol = 0) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i].ax - b[i].ax) > tol || std::abs(a[i].ay - b[i].ay) > tol ||
            std::abs(a[i].bx - b[i].bx) > tol ||
            std::abs(a[i].by - b[i].by) > tol || std::abs(a[i].penA - b[i].penA) > tol + 1e-6f ||
            std::abs(a[i].radius - b[i].radius) > tol + 1e-6f)
            return false;
    return true;
}

}  // namespace

TEST_CASE("dragon: the curve's turns are the paper-folding sequence; 2^n unit segments; the end at (1 - i)^n") {
    CHECK(turn(1) == -1);
    CHECK(turn(2) == -1);
    CHECK(turn(3) == +1);
    for (int n = 0; n <= kMaxGens; ++n) {
        CAPTURE(n);
        const auto p = unfold(n, kPi / 2);
        REQUIRE(p.size() == (std::size_t(1) << n) + 1);
        // The end point: each generation multiplies it by (1 - i) (turned -45 degrees, sqrt 2 longer).
        double ex = 1, ey = 0;
        for (int g = 0; g < n; ++g) {
            const double x = ex + ey, y = ey - ex;
            ex = x;
            ey = y;
        }
        CHECK(p.back().x == doctest::Approx(ex).epsilon(1e-9));
        CHECK(p.back().y == doctest::Approx(ey).epsilon(1e-9));
        // Generation n - 1 is a prefix.
        if (n > 0) {
            const auto q = unfold(n - 1, kPi / 2);
            for (std::size_t i = 0; i < q.size(); ++i) REQUIRE((q[i].x == p[i].x && q[i].y == p[i].y));
        }
    }
    const int n = 12;
    const auto p = unfold(n, kPi / 2);
    const auto seq = foldingSequence(n);
    REQUIRE(seq.size() == p.size() - 2);
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
        const double dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y;
        REQUIRE(std::hypot(dx, dy) == doctest::Approx(1.0).epsilon(1e-9));
        REQUIRE((std::abs(dx) < 1e-9 || std::abs(dy) < 1e-9));  // on the lattice
        if (i == 0) continue;
        const double px = p[i].x - p[i - 1].x, py = p[i].y - p[i - 1].y;
        const double cross = px * dy - py * dx;  // + = left
        const int t = cross > 0.5 ? 1 : cross < -0.5 ? -1 : 0;
        REQUIRE(t == turn(i));
        REQUIRE(t == seq[i - 1]);
    }
    // The dragon never runs along an edge twice (it touches itself only at corners).
    std::vector<std::pair<long long, long long>> edges;
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
        const long long a = std::llround(p[i].x) * 100000 + std::llround(p[i].y);
        const long long b = std::llround(p[i + 1].x) * 100000 + std::llround(p[i + 1].y);
        edges.push_back({std::min(a, b), std::max(a, b)});
    }
    std::sort(edges.begin(), edges.end());
    CHECK(std::adjacent_find(edges.begin(), edges.end()) == edges.end());
    CHECK(bornIn(0) == 0);
    CHECK(bornIn(1) == 1);
    CHECK(bornIn(2) == 2);
    CHECK(bornIn(3) == 2);
    CHECK(bornIn(4095) == 12);
    CHECK(bornIn(4096) == 13);
}

TEST_CASE("dragon: every fold lands exactly on its line; cycles unfold, hold and refold to one segment") {
    const Params p = paramsOf("Gens=\"12\" Fold=\"1\" Cyc=\"4\" Back=\"2\" Swng=\".5\"");
    const Schedule s(p);
    CHECK(s.top() == 12);
    CHECK(s.cycleBeats() == 16);
    CHECK(s.gensAt(0) == 0);
    for (int k = 1; k <= 12; ++k) {
        CAPTURE(k);
        CHECK(s.gensAt(k) == double(k));                                       // landed on the line
        CHECK(s.gensAt(k - 1e-9) == doctest::Approx(double(k)).epsilon(1e-6));  // 90 degrees just before it
        CHECK(s.gensAt(k - 0.5) == double(k - 1));                             // the swing starts half a beat early
        CHECK(s.gensAt(k - 0.75) == double(k - 1));
        double last = k - 1;
        for (double u = k - 0.5; u < k; u += 0.01) {  // opens steadily, fastest at the end
            const double g = s.gensAt(u);
            REQUIRE(g >= last);
            last = g;
        }
    }
    CHECK(s.gensAt(13) == 12);  // held
    CHECK(s.gensAt(14) == 12);  // the refold starts
    CHECK(s.gensAt(15) == doctest::Approx(6));
    CHECK(s.gensAt(16) == 0);  // one segment on the cycle line
    CHECK(Schedule::ease(1) == 1);
    CHECK(Schedule::ease(0) == 0);

    // A cycle too short for every fold reaches fewer; a stagger delays the other dragons.
    CHECK(Schedule(paramsOf("Gens=\"12\" Fold=\"2\" Cyc=\"4\" Back=\"2\"")).top() == 7);
    const Params q = paramsOf("Num=\"4\" Gens=\"11\" Fold=\"1\" Cyc=\"4\" Back=\"2\" Stag=\".25\"");
    const Schedule t(q);
    CHECK(t.top() == 11);
    CHECK(t.gensAt(1, 0) == 1);
    CHECK(t.gensAt(1.5, 2) == 1);   // dragon 2 lands half a beat later
    CHECK(t.gensAt(1.25, 2) < 1);
    CHECK(t.gensAt(13, 3) == 11);   // all open before the refold
    CHECK(t.gensAt(16, 3) == 0);

    // Num x 2^Gens is bounded.
    CHECK(paramsOf("Num=\"4\" Gens=\"14\"").gens == 12);
    CHECK(paramsOf("Gens=\"40\"").gens == kMaxGens);
}

TEST_CASE("dragon: drawn on the clock, the copy lands at 90 degrees on the beat; drops burst; deterministic") {
    const std::string cfg = "Kind=\"dragon\"\nGens=\"10\" Fold=\"1\" Cyc=\"4\" Back=\"2\" Vary=\"0\" Flsh=\"0\"\n";
    // Just before the line the copy is at 90 degrees: the same picture as on the line (pens aside: shading moves).
    for (int k = 1; k <= 10; ++k) {
        CAPTURE(k);
        arcade_test::Run a(cfg), b(cfg);
        const auto before = a.at(64.0 + k - 1e-7);
        const auto on = b.at(64.0 + k);
        REQUIRE(before.size() == on.size());
        REQUIRE(on.size() == (std::size_t(1) << k));
        for (std::size_t i = 0; i < on.size(); ++i) {
            REQUIRE(std::abs(before[i].ax - on[i].ax) < 0.01f);
            REQUIRE(std::abs(before[i].by - on[i].by) < 0.01f);
        }
        // Mid-swing: the copy is out, not yet at the angle.
        arcade_test::Run c(cfg);
        CHECK(c.at(64.0 + k - 0.25).size() == (std::size_t(1) << k));
        CHECK(c.at(64.0 + k - 0.75).size() == (std::size_t(1) << (k - 1)));
    }
    // The whole curve fits the frame (1920 x 1080), at every moment.
    {
        arcade_test::Run r(cfg);
        for (double b = 0; b < 32; b += 0.05) {
            const auto segs = r.at(b);
            REQUIRE(arcade_test::allFinite(segs));
            for (const auto& s : segs) {
                REQUIRE(s.ax >= -2);
                REQUIRE(s.ax <= 1922);
                REQUIRE(s.ay >= -2);
                REQUIRE(s.ay <= 1082);
            }
        }
    }
    // Deterministic, and a jump of any size costs one frame: a run that played up to a beat and one that starts
    // there draw the same.
    {
        const std::string vary = "Kind=\"dragon\"\nGens=\"12\" Num=\"2\"\n";
        arcade_test::Run a(vary), b(vary), c(vary);
        a.play(0, 37.3);
        CHECK(same(a.at(37.4), b.at(37.4)));
        CHECK(same(a.at(37.5), c.at(37.5)));
        const auto t0 = std::chrono::steady_clock::now();
        const auto far = b.at(4e9 + 13.0);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(far.size() == 2 * 4096);  // held at 12 generations
        CHECK(arcade_test::allFinite(far));
        PERF_BUDGET(ms < 20);
        arcade_test::Run d(vary), e("Kind=\"dragon\"\nGens=\"12\" Num=\"2\"\n");
        CHECK(same(d.at(77.7), e.at(77.7)));
    }
    // A drop collapses to one segment and bursts open (every generation within Brst beats), then holds until the
    // refold; the next cycle is back to normal.
    {
        arcade_test::Run r(cfg);
        r.at(64 + 3.0);
        CHECK(r.at(64 + 3.02, 120, 1.0).size() <= 2);  // the drop: collapsed
        CHECK(r.at(64 + 3.6, 120, 0.8).size() >= 256);
        CHECK(r.at(64 + 4.1, 120, 0.6).size() == 1024);  // all ten, a beat after
        CHECK(r.at(64 + 9.0, 120, 0.1).size() == 1024);  // held (the plain schedule would be at 9)
        CHECK(r.at(64 + 16 + 3.0).size() == 8);          // next cycle: plain again
    }
}

TEST_CASE("dragon: variants (twin, four, rounded, other angles, spin), junk and huge clocks stay finite and bounded") {
    for (const char* keys : {"Num=\"2\"", "Num=\"4\" Stag=\".25\" Gens=\"12\"", "Rnd=\".3\"", "Angl=\"120\"",
                             "Angl=\"-60\" Spin=\"1\"", "Run=\".6\" Num=\"3\"", "Back=\"0\" Swng=\"1\""}) {
        CAPTURE(keys);
        arcade_test::Run r(std::string("Kind=\"dragon\"\n") + keys + "\n");
        std::size_t most = 0;
        for (double b = 0; b < 40; b += 0.37) {
            const auto segs = r.at(b, 120, b > 20 && b < 21 ? 1.0 : 0.0, b > 30 ? 0.8 : 0.0);
            REQUIRE(arcade_test::allFinite(segs));
            most = std::max(most, segs.size());
        }
        CHECK(most > 100);
        CHECK(most <= 4 * std::size_t(kMaxSegments));
    }
    for (const char* junk : {"Gens=\"1e9\" Num=\"-4\" Fold=\"0\" Cyc=\"-1\" Back=\"1e300\" Swng=\"0\"",
                             "Fold=\"0 / 0\" Cyc=\"1 / 0\" Rnd=\"9\" Wdth=\"1e9\" Thin=\"-1\" Fill=\"0\"",
                             "Angl=\"1e20\" Stag=\"5\" Brst=\"-3\" Spin=\"7\" Sprd=\"9\" Run=\"1e9\""}) {
        CAPTURE(junk);
        arcade_test::Run r(std::string("Kind=\"dragon\"\n") + junk + "\n");
        const double inf = std::numeric_limits<double>::infinity();
        for (double b : {0.0, 1.0, 7.5, -3.0, 1e300, -1e300, std::nan(""), inf, -inf, 1e15, 2.0})
            REQUIRE(arcade_test::allFinite(r.at(b, 120, b > 1 ? 1.0 : 0.0, 0.5)));
    }
    // A huge or broken clock (beat and bar disagreeing wildly) stays finite.
    arcade_test::Run r("Kind=\"dragon\"\nNum=\"4\"\n");
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

TEST_CASE("dragon: cost per frame at the largest curves") {
    for (const char* keys :
         {"Gens=\"14\"", "Num=\"4\" Gens=\"12\"", "Rnd=\".3\" Gens=\"12\"", "Num=\"4\" Rnd=\".3\" Gens=\"12\""}) {
        CAPTURE(keys);
        arcade_test::Run r(std::string("Kind=\"dragon\"\nFold=\"1\" Cyc=\"8\"\n") + keys + "\n");
        r.at(15.0);  // warm up
        double worst = 0, sum = 0;
        std::size_t most = 0;
        int n = 0;
        // Through the last fold (the copy swinging: the most points) and the hold.
        for (double b = 13.5; b < 16.0; b += 0.05, ++n) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto segs = r.at(b);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst = std::max(worst, ms);
            sum += ms;
            most = std::max(most, segs.size());
        }
        char m[160];
        std::snprintf(m, sizeof m, "dragon %s: %zu segments, %.3f ms average, %.3f ms worst", keys, most, sum / n,
                      worst);
        MESSAGE(m);
        PERF_BUDGET(worst < 4.0);
    }
}

namespace {
// A dragon run with a real fade (the harness has none), stepped at `bpm` on a clock whose bar can jump.
struct LiveRun {
    reforce::scene::Environment env;
    std::shared_ptr<const reforce::scene::CompiledConfig> cfg;
    std::unique_ptr<OrganicRun> run;
    FrameMapping map = organicMapping(1920, 1080);
    LineStyle style{};
    double t = 0, bar = 0;
    explicit LiveRun(const std::string& keys, double fade = 1.0) : cfg(arcade_test::compile(keys)) {
        run = std::make_unique<OrganicRun>(cfg, env, "dragon", 0.0, fade, 600.0, nullptr);
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
    // Steps until the fade-out starts; returns the bar it started at (or -1).
    double untilEnd(double bpm, double upTo) {
        while (bar < upTo) {
            frame(bpm);
            if (run->fadingOut(t)) return bar;
        }
        return -1;
    }
};
constexpr const char* kFold = "Kind=\"dragon\"\nPBar=\"32\"\nGens=\"12\"\nFold=\"2\"\nCyc=\"8\"\nBack=\"4\"\n";
}  // namespace

TEST_CASE("dragon: PBar ends on the phrase line of the live clock, through tempo changes and clock jumps") {
    const double frameBar = 140.0 / 240 / 60;  // the most a frame advances below
    {  // steady tempo
        LiveRun r(kFold);
        CHECK(r.untilEnd(125, 40) == doctest::Approx(32).epsilon(0).scale(1).epsilon(frameBar / 32 * 1.5));
    }
    {  // the DJ pushes the tempo: fixed seconds would end ~0.6 bar late
        LiveRun r(kFold);
        while (r.bar < 10) r.frame(125);
        const double end = r.untilEnd(128, 40);
        CHECK(std::abs(end - 32) < 1.5 * frameBar);
    }
    {  // the phrase clock jumps (a deck start): the end moves to the nearest phrase line of the new clock
        LiveRun r(kFold);
        while (r.bar < 10) r.frame(125);
        r.bar += 3.3;  // 13.3, with 22 bars left: 35.3 -> the line at 32
        const double end = r.untilEnd(125, 60);
        CHECK(std::abs(end - 32) < 1.5 * frameBar);
        LiveRun q(kFold);
        while (q.bar < 10) q.frame(125);
        q.bar += 9.0;  // 19 + 22 = 41 -> the line at 48
        CHECK(std::abs(q.untilEnd(125, 60) - 48) < 1.5 * frameBar);
    }
}

TEST_CASE("dragon: fading out from the line, it stays one still segment (no new fold during the fade)") {
    LiveRun r(kFold, 2.0);
    REQUIRE(r.untilEnd(125, 40) > 0);
    // Two beats on (into the first fold's swing in a normal cycle) the faded-out dragon is still one segment.
    std::vector<LineSegment> last;
    while (r.bar < 32.6) last = r.frame(125);
    CHECK(!last.empty());
    LiveRun n(kFold, 2.0);  // the same moment, not ending (no followBars end: a long hold)
    n.run->setHold(600);
    n.run->followBars(0);
    std::vector<LineSegment> live;
    while (n.bar < 32.6) live = n.frame(125);
    CHECK(last.size() < live.size());
}

TEST_CASE("dragon: Rise brightens the newest copy smoothly, full just as the next fold starts to swing") {
    auto total = [](const std::vector<LineSegment>& segs) {
        double s = 0;
        for (const auto& x : segs) s += x.penA * std::hypot(x.bx - x.ax, x.by - x.ay);
        return s;
    };
    const std::string keys = "Kind=\"dragon\"\nGens=\"12\"\nFold=\"2\"\nCyc=\"8\"\nBack=\"4\"\nFlsh=\"0\"\n";
    arcade_test::Run rise(keys + "Rise=\".75\"\n"), flat(keys);
    // Generation 6 lands on beat 12; the next swing starts on beat 13 (Swng .5 of 2 beats).
    double prev = -1;
    for (double b : {12.02, 12.3, 12.6, 12.98}) {
        const double v = total(rise.at(b));
        CHECK(v > prev);
        prev = v;
    }
    const double full = total(flat.at(12.98));
    CHECK(prev == doctest::Approx(full).epsilon(0.02));      // full by the next swing
    CHECK(total(rise.at(12.02)) < 0.8 * total(flat.at(12.02)));  // dim just after landing
}

TEST_CASE("dragon: Settle (Dragon Fold): one even level, the newest copy glowing as the next swing starts, then settling") {
    // Dragon Fold's look on the clock: generation k lands on beat 2k; its successor starts to swing on beat 2k + 1.
    arcade_test::Run r(std::string(kFold) + "Rise=\".75\"\nSetl=\".25\"\nShd=\"0\"\nFlsh=\"0\"\n");
    auto shares = [](const std::vector<LineSegment>& segs, double& bright, double& level, double& other) {
        bright = level = other = 0;
        for (const auto& s : segs) {
            const double len = std::hypot(s.bx - s.ax, s.by - s.ay);
            if (s.penA > 0.97) bright += len;
            else if (std::abs(s.penA - 0.8) < 0.01) level += len;
            else other += len;
        }
        const double all = bright + level + other;
        bright /= all, level /= all, other /= all;
    };
    double b, l, o;
    // Beat 14.995: generation 7 (landed on 14) is at its glow, generation 6 has settled, nothing swings yet.
    for (double x = 0; x <= 14.995; x += 0.05) r.at(x);
    shares(r.at(14.995), b, l, o);
    CHECK(b == doctest::Approx(0.5).epsilon(0.08));
    CHECK(l == doctest::Approx(0.5).epsilon(0.08));
    CHECK(o < 0.02);
    // Half a beat after the next landing (16.5): the newest half still rising, the one before it settling.
    for (double x = 15.0; x <= 16.5; x += 0.05) r.at(x);
    shares(r.at(16.5), b, l, o);
    CHECK(o > 0.4);   // two in motion: the newest half rising, the quarter before it settling
    CHECK(l > 0.2);   // the rest at the level
}

TEST_CASE("dragon: the running band moves without jumps and runs in at the start, out at the end (never split)") {
    // Dragon Rounded's look without the rounding (one LineSegment per curve segment, in order along the curve).
    arcade_test::Run r("Kind=\"dragon\"\nGens=\"10\"\nFold=\"2\"\nCyc=\"8\"\nBack=\"4\"\nSwng=\".4\"\nRun=\".45\"\n"
                       "Shd=\"0\"\nFlsh=\"0\"\n");
    auto peak = [](const std::vector<LineSegment>& segs) {  // the brightest segment's place, as a share of the length
        std::size_t best = 0;
        for (std::size_t i = 1; i < segs.size(); ++i)
            if (segs[i].penA > segs[best].penA) best = i;
        return (double(best) + 0.5) / double(segs.size());
    };
    // Through bars 3 and 4 (folds swinging, segment counts doubling) at 120 BPM, 60 fps: the band's place along the
    // grown length moves by about a 120th of it per frame; a doubling would jump it by up to half.
    double prevLen = 0, prevAt = -1;
    int frames = 0, bigJumps = 0;
    for (double beat = 12.05; beat < 15.95; beat += 2.0 / 60) {
        const auto segs = r.at(beat);
        REQUIRE(segs.size() > 4);
        // Along the curve in segments, as the drawn count grows the place of the peak in segments must not leap.
        const double at = peak(segs) * double(segs.size());
        if (prevAt >= 0 && std::abs(at - prevAt) > 0.08 * std::max(prevLen, double(segs.size()))) ++bigJumps;
        prevAt = at;
        prevLen = double(segs.size());
        ++frames;
    }
    CHECK(frames > 100);
    CHECK(bigJumps == 0);
    // Just after a bar line the band has not entered yet: the curve's first tenth is at the level, no brighter.
    const auto after = r.at(16.02);
    double level = after[after.size() / 2].penA, firstMax = 0;
    for (std::size_t i = 0; i < after.size() / 10; ++i) firstMax = std::max(firstMax, double(after[i].penA));
    const auto mid = r.at(18.0);  // half way through the bar: the band in the middle, well above the level
    double midMax = 0;
    for (const auto& s : mid) midMax = std::max(midMax, double(s.penA));
    CHECK(firstMax < level + 0.02);
    CHECK(midMax > level + 0.2);  // the band peaks at full (the level is .775)
}
