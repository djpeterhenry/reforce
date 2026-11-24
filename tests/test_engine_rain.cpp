// Rain on glass (docs/3d-design.md "Rain on glass"): engine/Rain's simulation and the rain solid's keys.
//   - merges conserve volume (and wobble), a long window run conserves volume overall (drops + exited + evaporated +
//     film), the floor likewise (drops + pooled + evaporated + exited)
//   - window drops never leave the pane except at the bottom; big drops run, small ones stay
//   - determinism (same seed, and the same at 30 / 60 / 144 fps without a clock), seeds differ
//   - spawns land on the 16th grid: drops on beat lines, clusters on bar lines, drizzle between; the phrase-line
//     cascade; a drop is a downpour
//   - bounded catch-up and jumps of the clock, junk inputs (NaN, inf, 1e300) stay finite and in bounds
//   - the wave equation's coefficients are stable at every dt / speed / grid, and a long CPU run never blows up
//   - the solid's keys (Rain literal, defaults, the scene files)
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

#include "engine/Rain.hpp"
#include "engine/Solids.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
using namespace reforce::engine::rain;

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

void run(Sim& s, const rain::Controls& c, double seconds, double fps, bool withClock, double t0 = 0.0) {
    const int n = int(std::lround(seconds * fps));
    for (int i = 1; i <= n; ++i) {
        const double t = t0 + double(i) / fps;
        s.step(t, c, withClock ? clock120(t) : Clock{});
    }
}

bool sameDrops(const Sim& a, const Sim& b) {
    if (a.drops().size() != b.drops().size()) return false;
    for (std::size_t i = 0; i < a.drops().size(); ++i) {
        const Drop& x = a.drops()[i];
        const Drop& y = b.drops()[i];
        if (x.id != y.id || x.x != y.x || x.y != y.y || x.r != y.r || x.sliding != y.sliding) return false;
    }
    return true;
}

double accounted(const Sim& s) {
    const Tally& t = s.tally();
    return s.volume() + t.exited + t.evaporated + t.film + t.pooled;
}

void checkBounds(const Sim& s) {
    for (const Drop& d : s.drops()) {
        REQUIRE(std::isfinite(d.x));
        REQUIRE(std::isfinite(d.y));
        REQUIRE(std::isfinite(d.r));
        CHECK(d.r > 0);
        CHECK(d.x - d.r >= -1e-9);
        CHECK(d.x + d.r <= s.aspect() + 1e-9);
        CHECK(d.y - d.r >= -1e-9);
        if (s.settings().mode == Mode::Floor) CHECK(d.y <= 1.0 + 1e-9);
    }
}

}  // namespace

TEST_CASE("rain: volumes, modes, puddles") {
    CHECK(volumeOf(0.01, 90) == doctest::Approx(2.0 / 3.0 * 3.141592653589793 * 1e-6));  // a hemisphere
    CHECK(radiusOf(volumeOf(0.013, 62), 62) == doctest::Approx(0.013));
    CHECK(volumeOf(0.01, 30) < volumeOf(0.01, 60));
    CHECK(parseMode("window") == Mode::Window);
    CHECK(parseMode(" Floor ") == Mode::Floor);
    CHECK(parseMode("1") == Mode::Floor);
    CHECK(parseMode("") == Mode::Window);
    CHECK_FALSE(parseMode("ceiling"));
    // Puddles: 0..1, none when dry, everywhere when wet, more cover with more Wet.
    double prev = -1;
    for (double wet : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        double cover = 0;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 100; ++x) {
                const double p = puddle(x / 100.0 * kAspect, y / 60.0, wet, 7);
                REQUIRE(p >= 0.0);
                REQUIRE(p <= 1.0);
                cover += p;
            }
        cover /= 6000;
        CHECK(cover >= prev);
        prev = cover;
        if (wet == 0.0) CHECK(cover == 0.0);
        if (wet == 1.0) CHECK(cover == 1.0);
        if (wet == 0.5) CHECK(cover > 0.2);
        if (wet == 0.5) CHECK(cover < 0.8);
    }
    CHECK(puddle(0.3, 0.4, std::nan(""), 1) == 0.0);
}

TEST_CASE("rain: merging conserves volume and wobbles along the merge axis") {
    rain::Settings st;
    st.slide = 1000;  // nothing runs
    Sim s(st, kAspect, 0.0);
    s.place(0.5, 0.5, 0.010);
    s.place(0.5 + 0.017, 0.5, 0.008);  // touching (0.017 < 0.018)
    s.place(1.2, 0.3, 0.005);           // apart
    const double v0 = s.volume();
    rain::Controls quiet;
    quiet.density = quiet.drizzle = quiet.cluster = 0;
    s.step(0.0, quiet, {});
    s.step(1.0 / 60.0, quiet, {});
    REQUIRE(s.drops().size() == 2);
    CHECK(s.volume() == doctest::Approx(v0).epsilon(1e-12));
    CHECK(s.tally().merges == 1);
    const Drop& m = s.drops()[0].r > s.drops()[1].r ? s.drops()[0] : s.drops()[1];
    CHECK(m.r == doctest::Approx(std::cbrt(0.010 * 0.010 * 0.010 + 0.008 * 0.008 * 0.008)));
    CHECK(m.x > 0.5);         // pulled towards the smaller one, by volume
    CHECK(m.x < 0.5 + 0.017 / 2);
    CHECK(m.wobble > 0.1);
    CHECK(std::fabs(std::cos(m.wobbleAxis)) == doctest::Approx(1.0).epsilon(1e-6));  // along x
}

TEST_CASE("rain: a long window run conserves volume and keeps drops on the pane, leaving only at the bottom") {
    rain::Settings st;
    st.seed = 3;
    Sim s(st, kAspect, 0.0);
    rain::Controls c;
    c.density = 8;
    c.size = 12;
    c.drizzle = 2;
    double worst = 0, sum = 0;
    for (int i = 1; i <= 60 * 90; ++i) {
        const double t = i / 60.0;
        const auto t0 = std::chrono::steady_clock::now();
        s.step(t, c, clock120(t));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (i > 60 * 30) {
            sum += ms;
            worst = std::max(worst, ms);
        }
        if (i % 30 == 0) {
            checkBounds(s);
            CHECK(accounted(s) == doctest::Approx(s.tally().spawned).epsilon(1e-9));
        }
    }
    MESSAGE("window step with " << s.drops().size() << " drops: " << sum / (60 * 60) << " ms avg, " << worst
                                 << " ms worst");
    const Tally& t = s.tally();
    CHECK(t.spawned > 0);
    CHECK(accounted(s) == doctest::Approx(t.spawned).epsilon(1e-9));
    CHECK(t.slides > 20);       // drops ran
    CHECK(t.residuals > 20);    // and left droplets behind
    CHECK(t.merges > 100);
    CHECK(t.exited > 0);        // ran off the bottom
    CHECK(t.exitMinY > 1.0);    // only there
    CHECK(t.film > 0);
    CHECK(t.cascades >= 2);     // a phrase line every 32 s at 120 BPM
    CHECK(int(s.drops().size()) <= st.maxDrops);
    // Sliding drops are big ones; beads stay small.
    for (const Drop& d : s.drops())
        if (d.sliding) CHECK(d.r > 0.5 * d.slideAt);
}

TEST_CASE("rain: the floor conserves volume, throws crowns and hands impulses to the waves") {
    rain::Settings st;
    st.mode = Mode::Floor;
    st.seed = 11;
    Sim s(st, kAspect, 0.0);
    rain::Controls c;
    c.density = 6;
    c.wet = 0.5;
    int impulses = 0, airborne = 0;
    for (int i = 1; i <= 60 * 40; ++i) {
        const double t = i / 60.0;
        s.step(t, c, clock120(t));
        impulses += int(s.impulses().size());
        CHECK(int(s.impulses().size()) <= kMaxImpulses);
        CHECK(s.wipes().empty());
        for (const Drop& d : s.drops()) {
            airborne += d.flight > 0;
            CHECK_FALSE(d.sliding);
        }
        if (i % 60 == 0) checkBounds(s);
    }
    CHECK(impulses > 200);
    CHECK(airborne > 0);
    CHECK(s.tally().pooled > 0);
    CHECK(accounted(s) == doctest::Approx(s.tally().spawned).epsilon(1e-9));
    // All dry: beads, no waves; all wet: waves, no beads.
    for (double wet : {0.0, 1.0}) {
        Sim f(st, kAspect, 0.0);
        c.wet = wet;
        int imp = 0;
        for (int i = 1; i <= 600; ++i) {
            const double t = i / 60.0;
            f.step(t, c, clock120(t));
            imp += int(f.impulses().size());
        }
        int beads = 0;
        for (const Drop& d : f.drops()) beads += d.flight <= 0;
        if (wet == 0.0) {
            CHECK(imp == 0);
            CHECK(beads > 10);
        } else {
            CHECK(imp > 50);
            CHECK(beads == 0);
        }
    }
}

TEST_CASE("rain: deterministic, and the same at any frame rate without a clock") {
    rain::Settings st;
    st.seed = 42;
    rain::Controls c;
    c.density = 5;
    Sim a(st, kAspect, 10.0), b(st, kAspect, 10.0), c30(st, kAspect, 10.0), c144(st, kAspect, 10.0);
    run(a, c, 20, 60, false, 10.0);
    run(b, c, 20, 60, false, 10.0);
    run(c30, c, 20, 30, false, 10.0);
    run(c144, c, 20, 144, false, 10.0);
    REQUIRE(a.drops().size() > 20);
    CHECK(sameDrops(a, b));
    CHECK(sameDrops(a, c30));
    CHECK(sameDrops(a, c144));
    // With the clock too (the same clock values): repeatable.
    Sim d(st, kAspect, 0.0), e(st, kAspect, 0.0);
    run(d, c, 15, 60, true);
    run(e, c, 15, 60, true);
    CHECK(sameDrops(d, e));
    // Another seed rains differently.
    st.seed = 43;
    Sim f(st, kAspect, 10.0);
    run(f, c, 20, 60, false, 10.0);
    CHECK_FALSE(sameDrops(a, f));
}

TEST_CASE("rain: spawns land on the 16th grid; drops on beats, clusters on bars, drizzle between") {
    rain::Settings st;
    st.seed = 5;
    st.slide = 1000;
    rain::Controls c;
    c.density = 2;
    c.cluster = 0;
    c.drizzle = 0;
    Sim s(st, kAspect, 0.0);
    // One substep per step (120 fps): a drop count rising only where a beat line was crossed.
    std::int64_t before = 0;
    int stepsWithDrops = 0, offBeat = 0;
    for (int i = 1; i <= 120 * 8; ++i) {
        const double t = i / 120.0;
        const Clock k = clock120(t);
        const double prevBeat = 2.0 * (i - 1) / 120.0;
        s.step(t, c, k);
        if (s.tally().drops > before) {
            ++stepsWithDrops;
            if (std::floor(k.beat + 1e-9) == std::floor(prevBeat + 1e-9)) ++offBeat;
        }
        before = s.tally().drops;
    }
    CHECK(stepsWithDrops >= 14);  // 16 beats in 8 s (a few lines may draw 0 drops)
    CHECK(offBeat == 0);
    // Every 16th line is visited once, in order.
    const auto& lines = s.spawnLines();
    REQUIRE(lines.size() > 60);
    for (std::size_t i = 1; i < lines.size(); ++i) CHECK(lines[i] == lines[i - 1] + 1);
    // Clusters only on bar lines: with Clus and nothing else, drops appear on every 4th beat only.
    c.density = 0;
    c.cluster = 1;
    Sim b(st, kAspect, 0.0);
    std::int64_t was = 0;
    std::vector<double> at;
    for (int i = 1; i <= 120 * 10; ++i) {
        const double t = i / 120.0;
        b.step(t, c, clock120(t));
        if (b.tally().drops > was) at.push_back(2.0 * t);
        was = b.tally().drops;
    }
    REQUIRE(at.size() >= 4);
    for (double beat : at) CHECK(std::fmod(std::floor(beat + 1e-9), 4.0) == 0.0);
    // Drizzle on the 16ths: more lines carry drops.
    c.cluster = 0;
    c.drizzle = 3;
    Sim z(st, kAspect, 0.0);
    int lines16 = 0;
    std::int64_t w = 0;
    for (int i = 1; i <= 120 * 4; ++i) {
        const double t = i / 120.0;
        z.step(t, c, clock120(t));
        if (z.tally().drops > w) ++lines16;
        w = z.tally().drops;
    }
    CHECK(lines16 > 20);  // 24 non-beat 16ths in 4 s
}

TEST_CASE("rain: a drop is a downpour, a build thickens the drizzle, a phrase line lets the big drops go") {
    rain::Settings st;
    st.seed = 9;
    rain::Controls c;
    auto count = [&](double drop, double build) {
        Sim s(st, kAspect, 0.0);
        for (int i = 1; i <= 60 * 8; ++i) {
            const double t = i / 60.0;
            Clock k = clock120(t);
            k.drop = drop;
            k.build = build;
            s.step(t, c, k);
        }
        return s.tally().drops;
    };
    const auto calm = count(0, 0), downpour = count(1, 0), build = count(0, 1);
    CHECK(downpour > 3 * calm);
    CHECK(build > calm);  // the drizzle rises more than the beats thin
    // The cascade: on the phrase line (beat 64) drops above 0.4 x their threshold run.
    st.slide = 20;
    Sim s(st, kAspect, 0.0);
    rain::Controls quiet;
    quiet.density = quiet.drizzle = quiet.cluster = 0;
    s.place(0.4, 0.3, 12.0 / 1080.0);  // below its threshold (~16-24 px)
    for (int i = 1; i <= 60 * 31; ++i) {
        const double t = i / 60.0;
        s.step(t, quiet, clock120(t));
    }
    REQUIRE(s.drops().size() == 1);
    CHECK_FALSE(s.drops()[0].sliding);
    const double y0 = s.drops()[0].y;
    for (int i = 60 * 31 + 1; i <= 60 * 33; ++i) {
        const double t = i / 60.0;
        s.step(t, quiet, clock120(t));
    }
    CHECK(s.tally().cascades == 1);
    REQUIRE_FALSE(s.drops().empty());
    CHECK(s.drops()[0].y > y0 + 0.01);  // it ran
    // Wipe 0: no cascade.
    st.wipe = false;
    Sim n(st, kAspect, 0.0);
    for (int i = 1; i <= 60 * 33; ++i) n.step(i / 60.0, quiet, clock120(i / 60.0));
    CHECK(n.tally().cascades == 0);
}

TEST_CASE("rain: catch-up is bounded, clock jumps re-base") {
    rain::Settings st;
    rain::Controls c;
    c.density = 10;
    c.drizzle = 10;
    Sim s(st, kAspect, 0.0);
    run(s, c, 2, 60, true);
    const auto lines = s.tally().lines;
    // A minute in one step: at most kMaxCatchUp substeps and kMaxLines lines.
    s.step(62.0, c, clock120(62.0));
    CHECK(s.lastDt() <= kMaxCatchUp * kSubstep + 1e-12);
    CHECK(s.tally().lines - lines <= kMaxLines);
    // The clock jumping far back and forth: lines re-base, nothing explodes.
    for (double beat : {10.0, 1e6, -5.0, 3e8, 0.0}) {
        Clock k = clock120(63.0);
        k.beat = beat;
        k.bar = beat / 4;
        k.phrase = beat / 64;
        const auto l0 = s.tally().lines;
        s.step(63.0 + beat * 1e-9 + 0.1, c, k);
        CHECK(s.tally().lines - l0 <= kMaxLines);
        checkBounds(s);
    }
    // Time going backwards: no substeps.
    s.step(10.0, c, clock120(10.0));
    CHECK(s.lastDt() == 0.0);
    CHECK(int(s.drops().size()) <= st.maxDrops);
}

TEST_CASE("rain: junk inputs stay finite and in bounds") {
    const double inf = std::numeric_limits<double>::infinity(), nan = std::nan("");
    for (Mode mode : {Mode::Window, Mode::Floor}) {
        rain::Settings st;
        st.mode = mode;
        st.slide = nan;
        st.contact = 1e300;
        st.maxDrops = -5;
        Sim s(st, nan, nan);
        CHECK(std::isfinite(s.aspect()));
        for (double junk : {nan, inf, -inf, 1e300, -1e300, 0.0, 1e15}) {
            rain::Controls c;
            c.density = junk;
            c.size = junk;
            c.drizzle = junk;
            c.cluster = junk;
            c.wet = junk;
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
            CHECK(std::isfinite(s.volume()));
            for (const Impulse& p : s.impulses()) {
                CHECK(std::isfinite(p.x));
                CHECK(std::isfinite(p.amp));
            }
        }
        // Normal rain afterwards still works.
        run(s, rain::Controls{}, 4, 60, false, 6.0);
        checkBounds(s);
    }
    const WaveStep w = waveStep(std::nan(""), std::nan(""), std::nan(""), -3);
    CHECK(w.stable());
    CHECK(std::isfinite(w.c2));
}

TEST_CASE("rain: the wave equation is stable at every dt, speed and grid, and a long run never blows up") {
    for (double dt : {0.0, 1e-4, 1.0 / 240, 1.0 / 60, 1.0 / 30, 0.1, 0.25, 5.0, 1e9})
        for (double speed : {0.0, 0.05, 0.3, 1.0, 2.0, 100.0})
            for (int rows : {16, 64, 270, 540, 2160}) {
                const WaveStep w = waveStep(dt, speed, 1.6, rows);
                CAPTURE(dt);
                CAPTURE(speed);
                CAPTURE(rows);
                CHECK(w.stable());
                CHECK(w.substeps >= 1);
                CHECK(w.substeps <= kMaxWaveSubsteps);
                CHECK(w.damp <= 1.0);
                CHECK(w.damp > 0.0);
            }
    // A grid stepped 20,000 times at the largest coefficients, struck now and then: bounded, and still once the
    // rain stops. (No decay at all: damp 1, only the viscosity.)
    WaveGrid g(48, 27);
    WaveStep w = waveStep(1.0 / 30, 4.0, 1e9, 27);
    w.damp = 1.0;
    REQUIRE(w.stable());
    double peak = 0;
    std::vector<Impulse> none, hit;
    for (int i = 0; i < 20000; ++i) {
        hit.clear();
        if (i % 97 == 0 && i < 15000) hit.push_back({0.2 + 0.013 * (i % 89), 0.1 + 0.007 * (i % 113), 0.05, 0.02});
        g.substep(w, hit.empty() ? none : hit);
        peak = std::max(peak, g.maxAbs());
        if (!(peak < 1e3)) break;
    }
    CHECK(peak < 50.0);
    CHECK(std::isfinite(g.maxAbs()));
    // With the scene's decay it settles.
    WaveStep d = waveStep(1.0 / 60, 0.3, 1.0, 27);
    for (int i = 0; i < 3000; ++i) g.substep(d, none);
    CHECK(g.maxAbs() < 0.05 * std::max(peak, 1e-9) + 1e-6);
}

TEST_CASE("rain: the solid's keys and the content scenes") {
    namespace fs = std::filesystem;
    const fs::path pkg = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package";
    for (const char* name : {"Rain Floor.txt", "Rain Pool.txt", "Rain Downpour.txt"}) {
        CAPTURE(name);
        std::ifstream in(pkg / name);
        REQUIRE(in);
        std::stringstream ss;
        ss << in.rdbuf();
        auto file = reforce::scene::parseSceneText(ss.str(), name);
        auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
        REQUIRE(solidMesh(*cfg) == MeshKind::Rain);
        reforce::scene::Environment env;
        env.seed = 77;
        SolidRun r(cfg, env, name, MeshKind::Rain, 0.0, 1.0, 40.0, nullptr);
        r.setAspect(kAspect);
        double vol = 0;
        bool reset = false;
        std::size_t strikes = 0;  // floor impulses (drops striking water) over the run
        for (int i = 0; i < 600; ++i) {
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.beat = 2.0 * i / 60.0;
            a.sync.bar = a.sync.beat / 4;
            a.sync.phrase = a.sync.bar / 16;
            const SolidSpec sp = r.evaluate(i / 60.0, a);
            REQUIRE(sp.mesh == MeshKind::Rain);
            REQUIRE(sp.rain);
            if (i == 0) reset = sp.rainReset;
            else CHECK_FALSE(sp.rainReset);
            CHECK(std::isfinite(sp.rainLook.depth));
            CHECK(sp.rainLook.opacity >= 0.0f);
            vol = sp.rain->volume();
            strikes += sp.rain->impulses().size();
        }
        CHECK(reset);
        if (std::string(name) == "Rain Pool.txt") {
            CHECK(vol == 0);      // all water: no beads anywhere
            CHECK(strikes > 50);  // every drop strikes the water
        } else {
            CHECK(vol > 0);
        }
        CHECK(r.rainSim()->aspect() == doctest::Approx(kAspect));
    }
    // Defaults and the literal.
    auto compile = [](const std::string& text) {
        auto file = reforce::scene::parseSceneText(text, "test");
        return std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
    };
    reforce::scene::Environment env;
    env.seed = 5;
    {
        SolidRun r(compile("Mesh=\"rain\"\n"), env, "w", MeshKind::Rain, 0.0, 1.0, 40.0, nullptr);
        CHECK(r.rainSettings().mode == Mode::Window);
        const SolidSpec sp = r.evaluate(0.5);
        CHECK(sp.rainLook.fog > 0.0f);
        CHECK(sp.rainLook.depth == doctest::Approx(0.5));
    }
    {
        SolidRun r(compile("Mesh=\"rain\"\nRain=\"floor\"\nSeed=\"12\"\nDpth=\".1 + .1\"\nWet=\"2\"\n"), env, "f",
                   MeshKind::Rain, 0.0, 1.0, 40.0, nullptr);
        CHECK(r.rainSettings().mode == Mode::Floor);
        CHECK(r.rainSettings().seed == 12);
        const SolidSpec sp = r.evaluate(0.5);
        CHECK(sp.rainLook.fog == 0.0f);
        CHECK(sp.rainLook.depth == doctest::Approx(0.2));
        CHECK(sp.rainLook.wet == doctest::Approx(1.0));  // clamped
    }
}
