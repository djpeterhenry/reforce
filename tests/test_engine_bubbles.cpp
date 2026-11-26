// Bubbles (docs/3d-design.md "Bubbles"): engine/Bubbles' simulation and the bubble solid's keys.
//   - pops land on beat lines: planned a beat ahead (the film's doom rising over that beat), started in the substep
//     the beat line is crossed; a drop pops many at once (on the next beat line) and blows a burst in
//   - bubbles are blown in on the bar's 16ths; a build blows more in and makes them tremble; a breakdown's start is calm
//   - determinism (same seed; the same at 30 / 60 / 144 fps without a clock), seeds differ
//   - bounded catch-up and clock jumps (re-based), junk inputs (NaN, inf, 1e300) stay finite, the count stays bounded
//   - the air is divergence free; touching bubbles bounce or cluster (walls at the 120-degree distance); the spray
//   - the solid's keys (Src literal, defaults, the scene files)
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "engine/Bubbles.hpp"
#include "engine/Solids.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
using namespace reforce::engine::bubbles;

namespace {

constexpr double kAspect = 16.0 / 9.0;

Clock clock120(double t) {
    Clock c;
    c.on = true;
    c.beat = 2.0 * t;
    c.bar = c.beat / 4.0;
    c.phrase = c.bar / 16.0;
    return c;
}

void run(Sim& s, const bubbles::Controls& c, double seconds, double fps, bool withClock, double t0 = 0.0) {
    const int n = int(std::lround(seconds * fps));
    for (int i = 1; i <= n; ++i) {
        const double t = t0 + double(i) / fps;
        s.step(t, c, withClock ? clock120(t) : Clock{});
    }
}

bool same(const Sim& a, const Sim& b) {
    if (a.bubbles().size() != b.bubbles().size()) return false;
    for (std::size_t i = 0; i < a.bubbles().size(); ++i) {
        const Bubble& x = a.bubbles()[i];
        const Bubble& y = b.bubbles()[i];
        if (x.id != y.id || x.x != y.x || x.y != y.y || x.r != y.r || x.pop != y.pop || x.mode != y.mode) return false;
    }
    return a.droplets().size() == b.droplets().size() && a.tally().popped == b.tally().popped;
}

void checkBounds(const Sim& s) {
    CHECK(int(s.bubbles().size()) <= kMaxBubbles);
    CHECK(int(s.droplets().size()) <= kMaxDroplets);
    for (const Bubble& b : s.bubbles()) {
        REQUIRE(std::isfinite(b.x));
        REQUIRE(std::isfinite(b.y));
        REQUIRE(std::isfinite(b.vx));
        REQUIRE(std::isfinite(b.vy));
        REQUIRE(std::isfinite(b.r));
        CHECK(b.r > 0);
        CHECK(b.x > -0.6);
        CHECK(b.x < s.aspect() + 0.6);
        CHECK(b.y > -0.6);
        CHECK(b.y < 2.5);
        for (double m : b.mode) CHECK(std::fabs(m) <= 0.12 + 1e-12);
        CHECK(b.doom >= 0.0);
        CHECK(b.doom <= 1.0);
    }
    for (const Droplet& d : s.droplets()) {
        REQUIRE(std::isfinite(d.x));
        REQUIRE(std::isfinite(d.y));
    }
}

}  // namespace

TEST_CASE("bubbles: sources parse; the air is divergence free and smooth") {
    CHECK(parseSource("wand") == Source::Wand);
    CHECK(parseSource(" Edge ") == Source::Edge);
    CHECK(parseSource("AIR") == Source::Air);
    CHECK(parseSource("") == Source::Wand);
    CHECK_FALSE(parseSource("bathtub"));
    CHECK(std::string(sourceName(Source::Edge)) == "edge");
    // div v = d vx / dx + d vy / dy = 0 (the curl of a potential), and the speeds are of order 1.
    double maxDiv = 0, maxSpeed = 0, meanSpeed = 0;
    int n = 0;
    for (double t : {0.0, 3.3, 17.0})
        for (int y = 1; y < 20; ++y)
            for (int x = 1; x < 35; ++x) {
                const double px = x / 20.0, py = y / 20.0, e = 1e-3;
                double vx, vy, ax, ay, bx, by, cx, cy, dx, dy;
                Sim::airAt(px, py, t, 7, vx, vy);
                Sim::airAt(px + e, py, t, 7, ax, ay);
                Sim::airAt(px - e, py, t, 7, bx, by);
                Sim::airAt(px, py + e, t, 7, cx, cy);
                Sim::airAt(px, py - e, t, 7, dx, dy);
                const double div = (ax - bx) / (2 * e) + (cy - dy) / (2 * e);
                maxDiv = std::max(maxDiv, std::fabs(div));
                maxSpeed = std::max(maxSpeed, std::hypot(vx, vy));
                meanSpeed += std::hypot(vx, vy);
                ++n;
            }
    meanSpeed /= n;
    CHECK(maxDiv < 0.05 * std::max(1.0, maxSpeed));
    CHECK(meanSpeed > 0.1);
    CHECK(maxSpeed < 6.0);
    double vx, vy;
    Sim::airAt(std::nan(""), 0.5, 0.0, 1, vx, vy);
    CHECK(vx == 0.0);
}

TEST_CASE("bubbles: pops land on beat lines, planned a beat ahead, the film draining black first") {
    bubbles::Settings st;
    st.seed = 11;
    bubbles::Controls c;
    c.blow = 8;
    c.pop = 1.5;
    Sim s(st, kAspect, 0.0);
    // One substep per step (120 fps): a pop must start in a step whose beat crossed a whole number.
    std::int64_t popped = 0;
    int checked = 0;
    double maxDoomBefore = 0;
    for (int i = 1; i <= 120 * 40; ++i) {
        const double t = i / 120.0;
        const Clock k = clock120(t);
        const double prevBeat = 2.0 * (i - 1) / 120.0;
        for (const Bubble& b : s.bubbles())
            if (b.popLine != 0 && b.pop < 0) maxDoomBefore = std::max(maxDoomBefore, b.doom);
        s.step(t, c, k);
        if (s.tally().popped > popped) {
            CHECK(std::floor(k.beat + 1e-9) != std::floor(prevBeat + 1e-9));  // a beat line was crossed
            ++checked;
        }
        popped = s.tally().popped;
    }
    CHECK(checked > 20);
    CHECK(s.tally().offBeatPops == 0);
    for (const PopEvent& e : s.pops()) {
        CHECK(e.line % 4 == 0);
        CHECK(std::floor(e.beat * 4.0 + 1e-9) == double(e.line));  // started at the line, in its substep
        CHECK(e.beat - double(e.line) / 4.0 < 1.0 / 120.0 * 2.0 + 1e-9);
    }
    CHECK(maxDoomBefore > 0.9);  // the planned pops' films drained over the beat before
    checkBounds(s);
    // A planned pop goes on the next beat line, and not before.
    bubbles::Controls quiet;
    quiet.blow = 0;
    quiet.pop = 0;
    Sim p(st, kAspect, 0.0);
    p.step(0.0, quiet, clock120(0.0));
    const std::uint32_t id = p.place(0.8, 0.5, 0.06);
    p.step(0.1, quiet, clock120(0.1));  // beat .2
    p.planPop(id);
    REQUIRE_FALSE(p.bubbles().empty());
    CHECK(p.bubbles()[0].popLine == 4);  // beat 1
    for (int i = 1; i <= 20; ++i) p.step(0.1 + i / 120.0, quiet, clock120(0.1 + i / 120.0));  // to beat .53
    CHECK(p.tally().popped == 0);
    CHECK(p.bubbles()[0].doom > 0.3);
    for (int i = 21; i <= 60; ++i) p.step(0.1 + i / 120.0, quiet, clock120(0.1 + i / 120.0));  // past beat 1
    CHECK(p.tally().popped == 1);
    REQUIRE(p.pops().size() == 1);
    CHECK(p.pops()[0].line == 4);
    CHECK(p.pops()[0].planned);
    // The pop takes kPopSeconds, then the bubble is gone and a spray is in the air.
    CHECK(p.bubbles().empty());
    CHECK(p.droplets().size() >= 8);
}

TEST_CASE("bubbles: blown in on the bar's 16ths; a build blows more and trembles, a breakdown's start is calm") {
    bubbles::Settings st;
    st.seed = 3;
    st.maxBubbles = 96;
    bubbles::Controls c;
    c.blow = 6;
    c.pop = 0;
    c.life = 4096;
    Sim s(st, kAspect, 0.0);
    std::int64_t was = 0;
    std::vector<int> sixteenths;
    for (int i = 1; i <= 120 * 8; ++i) {
        const double t = i / 120.0;
        s.step(t, c, clock120(t));
        if (s.tally().blown > was) sixteenths.push_back(int(std::floor(2.0 * t * 4.0 + 1e-9)) % 16);
        was = s.tally().blown;
    }
    REQUIRE(sixteenths.size() >= 12);
    for (int k : sixteenths) CHECK(k < 8);  // the puff comes out over the bar's first half
    CHECK(s.tally().blown >= 6 * 3);
    auto blown = [&](double build) {
        Sim b(st, kAspect, 0.0);
        for (int i = 1; i <= 60 * 16; ++i) {
            Clock k = clock120(i / 60.0);
            k.build = build;
            b.step(i / 60.0, c, k);
        }
        return b;
    };
    const Sim calm = blown(0.0), tense = blown(0.9), early = blown(0.15);
    CHECK(tense.tally().blown > calm.tally().blown * 2);
    CHECK(tense.tremble() > 0.7);
    CHECK(calm.tremble() == 0.0);
    CHECK(early.calm() > 0.5);
    // Early in a breakdown they come bigger.
    auto meanR = [](const Sim& x) {
        double r = 0;
        for (const Bubble& b : x.bubbles()) r += b.r;
        return x.bubbles().empty() ? 0.0 : r / double(x.bubbles().size());
    };
    CHECK(meanR(early) > meanR(calm) * 1.2);
    // Trembling bubbles wobble more.
    auto wobble = [](const Sim& x) {
        double m = 0;
        for (const Bubble& b : x.bubbles()) m = std::max(m, std::hypot(b.mode[0], b.mode[1]));
        return m;
    };
    CHECK(wobble(tense) > wobble(calm));
}

TEST_CASE("bubbles: a drop pops many at once on the next beat line and blows a burst in") {
    bubbles::Settings st;
    st.seed = 21;
    bubbles::Controls c;
    c.blow = 10;
    c.pop = 0;
    c.life = 4096;
    Sim s(st, kAspect, 0.0);
    run(s, c, 12, 120, true);
    int alive = 0;
    for (const Bubble& b : s.bubbles()) alive += b.pop < 0 && b.grow >= 1;
    REQUIRE(alive >= 10);
    const auto popped0 = s.tally().popped, blown0 = s.tally().blown;
    // The drop fires a third of a beat after beat 24 (t = 12.0): nothing pops until beat 25 (t = 12.5).
    for (int i = 1; i <= 60; ++i) {
        const double t = 12.0 + i / 120.0;
        Clock k = clock120(t);
        k.drop = t >= 12.17 ? std::exp(-(t - 12.17) / 2.0) : 0.0;
        s.step(t, c, k);
        if (t < 12.5 - 1e-9) CHECK(s.tally().popped == popped0);
    }
    CHECK(s.tally().bursts == 1);
    CHECK(s.tally().dropPops >= int(0.5 * alive));
    CHECK(s.tally().popped - popped0 >= int(0.5 * alive));
    CHECK(s.tally().blown - blown0 >= 10);
    for (const PopEvent& e : s.pops())
        if (!e.planned) CHECK(e.line == 100);  // beat 25
    // The drop staying high does not fire again.
    for (int i = 61; i <= 480; ++i) {
        const double t = 12.0 + i / 120.0;
        Clock k = clock120(t);
        k.drop = 0.8;
        s.step(t, c, k);
    }
    CHECK(s.tally().bursts == 1);
    checkBounds(s);
}

TEST_CASE("bubbles: deterministic, and the same at any frame rate without a clock") {
    bubbles::Settings st;
    st.seed = 42;
    bubbles::Controls c;
    c.blow = 8;
    for (Source src : {Source::Wand, Source::Edge, Source::Air}) {
        CAPTURE(int(src));
        st.source = src;
        Sim a(st, kAspect, 10.0), b(st, kAspect, 10.0), c30(st, kAspect, 10.0), c144(st, kAspect, 10.0);
        run(a, c, 20, 60, false, 10.0);
        run(b, c, 20, 60, false, 10.0);
        run(c30, c, 20, 30, false, 10.0);
        run(c144, c, 20, 144, false, 10.0);
        REQUIRE(a.bubbles().size() > 5);
        CHECK(a.tally().popped > 5);
        CHECK(same(a, b));
        CHECK(same(a, c30));
        CHECK(same(a, c144));
        // With the clock too: repeatable.
        Sim d(st, kAspect, 0.0), e(st, kAspect, 0.0);
        run(d, c, 15, 60, true);
        run(e, c, 15, 60, true);
        CHECK(same(d, e));
    }
    st.source = Source::Wand;
    Sim x(st, kAspect, 10.0);
    run(x, c, 20, 60, false, 10.0);
    st.seed = 43;
    Sim y(st, kAspect, 10.0);
    run(y, c, 20, 60, false, 10.0);
    CHECK_FALSE(same(x, y));
}

TEST_CASE("bubbles: touching bubbles bounce softly or cluster at the 120-degree distance") {
    bubbles::Controls quiet;
    quiet.blow = 0;
    quiet.pop = 0;
    quiet.air = 0;
    quiet.rise = 0;
    quiet.life = 4096;
    // Never stick: two pushed together end up apart (no overlap left).
    {
        bubbles::Settings st;
        st.cluster = 0;
        Sim s(st, kAspect, 0.0);
        s.step(0.0, quiet, {});
        s.place(0.8, 0.5, 0.05);
        s.place(0.88, 0.5, 0.05);  // overlapping by .02
        run(s, quiet, 3, 60, false);
        REQUIRE(s.bubbles().size() == 2);
        const Bubble& a = s.bubbles()[0];
        const Bubble& b = s.bubbles()[1];
        CHECK(std::hypot(a.x - b.x, a.y - b.y) > 0.097);
        CHECK(s.tally().bonds == 0);
    }
    // Always stick: they settle where their films meet at 120 degrees (equal radii: centers one radius apart).
    {
        bubbles::Settings st;
        st.cluster = 1;
        Sim s(st, kAspect, 0.0);
        s.step(0.0, quiet, {});
        s.place(0.8, 0.5, 0.05);
        s.place(0.89, 0.5, 0.05);
        run(s, quiet, 6, 60, false);
        REQUIRE(s.bubbles().size() == 2);
        const Bubble& a = s.bubbles()[0];
        const Bubble& b = s.bubbles()[1];
        CHECK(s.tally().bonds == 1);
        CHECK(a.bonds() == 1);
        CHECK(std::hypot(a.x - b.x, a.y - b.y) == doctest::Approx(0.05).epsilon(0.08));
        // One pops: the other is free again.
        s.planPop(a.id);
        run(s, quiet, 1, 60, false, 6.0);
        REQUIRE(s.bubbles().size() == 1);
        CHECK(s.bubbles()[0].bonds() == 0);
    }
}

TEST_CASE("bubbles: catch-up is bounded, clock jumps re-base, the count stays bounded") {
    bubbles::Settings st;
    st.maxBubbles = 30;
    bubbles::Controls c;
    c.blow = 64;
    c.pop = 0;
    c.life = 4096;
    Sim s(st, kAspect, 0.0);
    run(s, c, 8, 60, true);
    int alive = 0;
    for (const Bubble& b : s.bubbles()) alive += b.pop < 0;
    CHECK(alive <= 30);
    const auto lines = s.tally().lines;
    s.step(70.0, c, clock120(70.0));
    CHECK(s.lastDt() <= kMaxCatchUp * kSubstep + 1e-12);
    CHECK(s.tally().lines - lines <= kMaxLines);
    for (double beat : {10.0, 1e6, -5.0, 3e8, 0.0}) {
        Clock k = clock120(71.0);
        k.beat = beat;
        k.bar = beat / 4;
        k.phrase = beat / 64;
        const auto l0 = s.tally().lines;
        s.step(71.0 + beat * 1e-9 + 0.1, c, k);
        CHECK(s.tally().lines - l0 <= kMaxLines);
        checkBounds(s);
    }
    // After a jump back, planned pops still happen (re-planned near the clock).
    bubbles::Controls popping = c;
    popping.pop = 2;
    popping.blow = 6;
    const auto p0 = s.tally().popped;
    for (int i = 1; i <= 600; ++i) s.step(72.0 + i / 60.0, popping, clock120(i / 60.0));
    CHECK(s.tally().popped > p0 + 10);
    // Time going backwards: no substeps.
    s.step(10.0, c, clock120(10.0));
    CHECK(s.lastDt() == 0.0);
    // A drop burst may overfill Max for a moment; the extras pop on the next beat lines.
    bubbles::Settings sb;
    sb.maxBubbles = 12;
    sb.seed = 2;
    Sim b(sb, kAspect, 0.0);
    bubbles::Controls bc;
    bc.blow = 12;
    bc.pop = 0;
    bc.life = 4096;
    int peak = 0;
    for (int i = 1; i <= 120 * 20; ++i) {
        const double t = i / 120.0;
        Clock k = clock120(t);
        k.drop = (t > 8.1 && t < 9) ? 1.0 : 0.0;
        b.step(t, bc, k);
        int n = 0;
        for (const Bubble& x : b.bubbles()) n += x.pop < 0;
        peak = std::max(peak, n);
    }
    int n = 0;
    for (const Bubble& x : b.bubbles()) n += x.pop < 0;
    CHECK(peak <= 12 + 6);
    CHECK(n <= 12);
}

TEST_CASE("bubbles: junk inputs stay finite and in bounds") {
    const double inf = std::numeric_limits<double>::infinity(), nan = std::nan("");
    for (Source src : {Source::Wand, Source::Edge, Source::Air}) {
        bubbles::Settings st;
        st.source = src;
        st.maxBubbles = -5;
        st.cluster = nan;
        Sim s(st, nan, nan);
        CHECK(std::isfinite(s.aspect()));
        for (double junk : {nan, inf, -inf, 1e300, -1e300, 0.0, 1e15}) {
            bubbles::Controls c;
            c.blow = junk;
            c.size = junk;
            c.pop = junk;
            c.life = junk;
            c.air = junk;
            c.rise = junk;
            Clock k;
            k.on = true;
            k.beat = junk;
            k.bar = junk;
            k.phrase = junk;
            k.build = junk;
            k.drop = junk;
            for (int i = 0; i < 20; ++i) s.step(junk == junk ? 1.0 + i / 60.0 : junk, c, k);
            s.step(5.0, c, k);
            s.place(junk, junk, junk);
            checkBounds(s);
        }
        run(s, bubbles::Controls{}, 6, 60, false, 6.0);
        checkBounds(s);
        CHECK(s.tally().blown > 0);
    }
}

TEST_CASE("bubbles: a long run stays bounded, finite and lively") {
    bubbles::Settings st;
    st.seed = 5;
    st.maxBubbles = 60;
    bubbles::Controls c;
    c.blow = 16;
    c.pop = 1;
    Sim s(st, kAspect, 0.0);
    std::size_t maxDroplets = 0;
    double worst = 0, total = 0;
    for (int i = 1; i <= 60 * 300; ++i) {
        const double t = i / 60.0;
        Clock k = clock120(t);
        const double phrase = std::fmod(t, 32.0);
        k.build = phrase > 16 && phrase < 30 ? (phrase - 16) / 14 : 0;
        k.drop = phrase >= 30 ? 1.0 : 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        s.step(t, c, k);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, ms);
        total += ms;
        maxDroplets = std::max(maxDroplets, s.droplets().size());
        if (i % 600 == 0) checkBounds(s);
    }
    MESSAGE("CPU per 60 fps step (up to 60 bubbles): " << total / (60 * 300) << " ms mean, " << worst << " ms worst");
    CHECK(s.tally().popped > 300);
    CHECK(s.tally().bursts >= 8);
    CHECK(maxDroplets > 20);
    CHECK(s.tally().offBeatPops == 0);
}

TEST_CASE("bubbles: the solid's keys and the content scenes") {
    namespace fs = std::filesystem;
    const fs::path pkg = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package";
    for (const char* name : {"Bubbles Drift.txt", "Bubbles Burst.txt"}) {
        CAPTURE(name);
        std::ifstream in(pkg / name);
        REQUIRE(in);
        std::stringstream ss;
        ss << in.rdbuf();
        auto file = reforce::scene::parseSceneText(ss.str(), name);
        auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
        REQUIRE(solidMesh(*cfg) == MeshKind::Bubbles);
        reforce::scene::Environment env;
        env.seed = 77;
        SolidRun r(cfg, env, name, MeshKind::Bubbles, 0.0, 1.0, 40.0, nullptr);
        r.setAspect(kAspect);
        std::size_t most = 0;
        for (int i = 0; i < 60 * 30; ++i) {
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.beat = 2.0 * i / 60.0;
            a.sync.bar = a.sync.beat / 4;
            a.sync.phrase = a.sync.bar / 16;
            const SolidSpec sp = r.evaluate(i / 60.0, a);
            REQUIRE(sp.mesh == MeshKind::Bubbles);
            REQUIRE(sp.bubbles);
            CHECK(std::isfinite(sp.bubbleLook.film));
            CHECK(sp.bubbleLook.opacity >= 0.0f);
            most = std::max(most, sp.bubbles->bubbles().size());
        }
        CHECK(most >= 3);
        CHECK(r.bubbleSim()->tally().popped > 0);
        CHECK(r.bubbleSim()->aspect() == doctest::Approx(kAspect));
    }
    auto compile = [](const std::string& text) {
        auto file = reforce::scene::parseSceneText(text, "test");
        return std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
    };
    reforce::scene::Environment env;
    env.seed = 5;
    {
        SolidRun r(compile("Mesh=\"bubbles\"\n"), env, "b", MeshKind::Bubbles, 0.0, 1.0, 40.0, nullptr);
        CHECK(r.bubbleSettings().source == Source::Wand);
        CHECK(r.bubbleSettings().maxBubbles == 40);
        const SolidSpec sp = r.evaluate(0.5);
        CHECK(sp.bubbleLook.film == doctest::Approx(520.0));
        CHECK(sp.bubbleLook.depth == doctest::Approx(0.3));
    }
    {
        SolidRun r(compile("Mesh=\"bubbles\"\nSrc=\"edge\"\nSeed=\"12\"\nMax=\"500\"\nClst=\"0\"\nFilm=\"300 + 100\"\n"
                           "Iris=\"9\"\nTint=\".5\"\n"),
                   env, "e", MeshKind::Bubbles, 0.0, 1.0, 40.0, nullptr);
        CHECK(r.bubbleSettings().source == Source::Edge);
        CHECK(r.bubbleSettings().seed == 12);
        CHECK(r.bubbleSettings().maxBubbles == kMaxBubbles);  // clamped
        CHECK(r.bubbleSettings().cluster == 0.0);
        const SolidSpec sp = r.evaluate(0.5);
        CHECK(sp.bubbleLook.film == doctest::Approx(400.0));
        CHECK(sp.bubbleLook.iris == doctest::Approx(3.0));  // clamped
        CHECK(sp.bubbleLook.tint == doctest::Approx(0.5));
    }
    CHECK(parseMeshKind("Bubbles") == MeshKind::Bubbles);
    CHECK(std::string(meshKindName(MeshKind::Bubbles)) == "bubbles");
}
