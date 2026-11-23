// Arcade organic `invaders` (PLAN 5Q.1, docs/arcade-design.md).
#include <doctest/doctest.h>

#include <algorithm>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PerfBudget.hpp"
#include "arcade_harness.hpp"

using namespace reforce::engine;

namespace {

// Only the formation: no shots, cannon, saucer or debris, so the picture changes only when the formation moves.
const char* const kMarchOnly = "Kind=\"invaders\"\nFire=\"0\"\nSauc=\"0\"\nCann=\"0\"\nDebr=\"0\"\nShot=\"0\"\n";

// A number from the run's summary ("...; waves 2, hits 31, ..."), -1 if absent.
int tally(const arcade_test::Run& r, const char* name) {
    const std::string s = r.run->summary();
    const std::string key = std::string(name) + " ";
    const auto p = s.find(key, s.find(';'));
    if (p == std::string::npos) return -1;
    return std::atoi(s.c_str() + p + key.size());
}

bool same(const std::vector<LineSegment>& a, const std::vector<LineSegment>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(LineSegment)) != 0) return false;
    return true;
}

// Steps at 60 fps over [b0, b1) and returns the beats (as 16th ticks) at which the picture changed.
std::vector<int> changeTicks(arcade_test::Run& r, double b0, double b1, double build = 0) {
    std::vector<int> ticks;
    std::vector<LineSegment> prev;
    const double perFrame = 2.0 / 60.0;
    bool first = true;
    for (double b = b0; b < b1; b += perFrame) {
        auto segs = r.at(b, 120, 0, build);
        if (!first && !same(segs, prev)) ticks.push_back(int(std::floor(b * 4 + 1e-9)));
        prev = std::move(segs);
        first = false;
    }
    return ticks;
}

}  // namespace

TEST_CASE("arcade invaders: draws, finite, on the beat clock") {
    arcade_test::Run r("Kind=\"invaders\"\n");
    r.play(0, 3.9);
    CHECK(tally(r, "left") == 55);  // still descending: no shots yet
    const auto segs = r.play(3.9, 8);
    CHECK(!segs.empty());
    CHECK(arcade_test::allFinite(segs));
    CHECK(tally(r, "left") < 55);
    CHECK(r.run->summary().find("11 x 5 invaders") == 0);
}

TEST_CASE("arcade invaders: the march steps on beat lines, and only there") {
    arcade_test::Run r(kMarchOnly);
    r.play(0, 5.5);  // the wave has descended (one bar) and marches
    const auto ticks = changeTicks(r, 5.5, 13.5);
    REQUIRE(ticks.size() == 8);  // one step per beat, nothing in between
    for (int k : ticks) CHECK(k % 4 == 0);
    // Rate 2: on the 8ths.
    arcade_test::Run r2(std::string(kMarchOnly) + "Rate=\"2\"\n");
    r2.play(0, 5.5);
    const auto t2 = changeTicks(r2, 5.55, 9.55);
    CHECK(t2.size() == 8);
    for (int k : t2) CHECK(k % 2 == 0);
}

TEST_CASE("arcade invaders: Pix sizes the pixels; the intensity keys follow per-frame expressions") {
    auto width = [](const std::vector<LineSegment>& segs) {
        float x0 = 1e9f, x1 = -1e9f;
        for (const auto& s : segs) {
            x0 = std::min({x0, s.ax - s.radius, s.bx - s.radius});
            x1 = std::max({x1, s.ax + s.radius, s.bx + s.radius});
        }
        return x1 - x0;
    };
    const std::string base = std::string(kMarchOnly) + "Cols=\"4\"\nRows=\"2\"\n";
    arcade_test::Run small(base + "Pix=\".008\"\n"), big(base + "Pix=\".016\"\n");
    const double ratio = width(big.play(0, 6)) / width(small.play(0, 6));
    CHECK(ratio > 1.85);
    CHECK(ratio < 2.15);

    // The aliens' intensity ramps with the beat: a frame key, not a load-time constant.
    arcade_test::Run r(std::string(kMarchOnly) + "Aln=\"SYNC_BEAT / 16\"\n");
    auto most = [](const std::vector<LineSegment>& segs) {
        float m = 0;
        for (const auto& s : segs) m = std::max(m, s.penA);
        return m;
    };
    const float early = most(r.play(0, 5));
    const float late = most(r.play(5, 10));
    CHECK(early == doctest::Approx(5.0 / 16).epsilon(0.02));
    CHECK(late == doctest::Approx(10.0 / 16).epsilon(0.02));
}

TEST_CASE("arcade invaders: a new wave descends in 8th-note steps over its first bar") {
    arcade_test::Run r(kMarchOnly);
    const auto ticks = changeTicks(r, 0, 4.0);
    CHECK(ticks.size() >= 6);
    for (int k : ticks) CHECK(k % 2 == 0);
}

TEST_CASE("arcade invaders: the march hurries through a build, quantised to 8ths then 16ths") {
    for (const auto& [build, every] : {std::pair{0.0, 4}, std::pair{0.5, 2}, std::pair{0.9, 1}}) {
        CAPTURE(build);
        arcade_test::Run r(kMarchOnly);
        r.play(0, 5.5);
        const auto ticks = changeTicks(r, 5.55, 7.55, build);
        CHECK(int(ticks.size()) == 8 / every);
        for (int k : ticks) CHECK(k % every == 0);
    }
}

TEST_CASE("arcade invaders: the cannon hits aliens on the grid; hits burst into debris; misses happen") {
    arcade_test::Run none(std::string("Kind=\"invaders\"\nFire=\"0\"\n"));
    none.play(0, 32);
    CHECK(tally(none, "hits") == 0);
    CHECK(tally(none, "left") == 55);

    arcade_test::Run r("Kind=\"invaders\"\nAim=\"1\"\nSauc=\"0\"\n");
    r.play(0, 24);
    // 2 shots per beat from beat 4 on (the wave descends first): nearly every one lands (a shot takes a beat or so).
    const int hits = tally(r, "hits");
    CHECK(hits >= 30);
    CHECK(hits <= 40);
    CHECK(tally(r, "left") == 55 - hits);

    // Debris only (aliens, cannon and shots off): dots right after the hits.
    arcade_test::Run d("Kind=\"invaders\"\nAim=\"1\"\nSauc=\"0\"\nAln=\"0\"\nCann=\"0\"\nShot=\"0\"\n");
    const auto segs = d.play(0, 12);
    REQUIRE(segs.size() > 20);
    for (const auto& s : segs) {
        CHECK(s.ax == s.bx);
        CHECK(s.ay == s.by);
    }

    // Aim 0: every shot goes through a gap.
    arcade_test::Run m("Kind=\"invaders\"\nAim=\"0\"\nSauc=\"0\"\n");
    m.play(0, 24);
    CHECK(tally(m, "hits") == 0);
}

TEST_CASE("arcade invaders: fresh waves start on Wave-bar lines, and a bar line after the formation is cleared") {
    arcade_test::Run r("Kind=\"invaders\"\nWave=\"4\"\nFire=\"4\"\nAim=\"1\"\nSauc=\"0\"\n");
    r.play(0, 15.9);
    CHECK(tally(r, "waves") == 1);
    CHECK(tally(r, "left") < 55);
    r.play(16.0, 16.2);
    CHECK(tally(r, "waves") == 2);  // on the line (bar 4)
    CHECK(tally(r, "left") == 55);
    r.play(16.3, 31.9);
    CHECK(tally(r, "waves") == 2);
    r.play(32.0, 32.1);
    CHECK(tally(r, "waves") == 3);

    // Wave 0: only when cleared, then on the next bar line. 16ths, a small formation: cleared quickly.
    arcade_test::Run c("Kind=\"invaders\"\nWave=\"0\"\nFire=\"4\"\nAim=\"1\"\nSauc=\"0\"\nCols=\"4\"\nRows=\"2\"\n");
    int waves = 1, newWaveTick = -1;
    for (double b = 0; b < 40 && newWaveTick < 0; b += 1.0 / 30) {
        c.at(b);
        if (tally(c, "waves") != waves) newWaveTick = int(std::floor(b * 4 + 1e-9));
    }
    REQUIRE(newWaveTick > 0);
    CHECK(newWaveTick % 16 == 0);
}

TEST_CASE("arcade invaders: a drop explodes every alien within about a beat, then a fresh wave") {
    arcade_test::Run r("Kind=\"invaders\"\nFire=\"0\"\nSauc=\"0\"\n");
    r.play(0, 9.0);
    CHECK(tally(r, "left") == 55);
    double b = 9.0 + 1.0 / 30;
    r.at(b, 120, 1.0);  // SYNC_DROP rises
    CHECK(tally(r, "drops") == 1);
    r.at(b += 1.0 / 30, 120, 0.98);
    const int early = tally(r, "left");
    CHECK(early > 0);  // a cascade, not all at once
    for (; b < 10.3; b += 1.0 / 30) r.at(b, 120, 0.8);
    CHECK(tally(r, "left") == 0);
    CHECK(tally(r, "hits") == 55);
    // The drop keeps decaying: no second cascade. The next bar line (beat 12) brings a fresh wave.
    for (; b < 12.2; b += 1.0 / 30) r.at(b, 120, 0.6);
    CHECK(tally(r, "drops") == 1);
    CHECK(tally(r, "waves") == 2);
    CHECK(tally(r, "left") == 55);
}

TEST_CASE("arcade invaders: a saucer crosses each phrase and is shot down on bar 12's line") {
    arcade_test::Run r("Kind=\"invaders\"\nFire=\"0\"\n");
    r.play(0, 47.9);
    CHECK(tally(r, "saucers") == 0);
    r.play(48.0, 48.1);
    CHECK(tally(r, "saucers") == 1);
    r.play(48.2, 64 + 48.1);
    CHECK(tally(r, "saucers") == 2);
}

TEST_CASE("arcade invaders: a formation that reaches the cannon beams back up; nobody loses") {
    // 16ths all the way (a long build), no shooting: the formation comes down a row every bar.
    arcade_test::Run r("Kind=\"invaders\"\nFire=\"0\"\nSauc=\"0\"\nWave=\"0\"\n");
    for (double b = 0; b < 48; b += 1.0 / 30) r.at(b, 120, 0, 1.0);
    CHECK(tally(r, "beam-ups") >= 1);
    CHECK(tally(r, "left") == 55);
    CHECK(tally(r, "waves") == 1);
}

TEST_CASE("arcade invaders: deterministic for a seed") {
    const std::string cfg = "Kind=\"invaders\"\nFire=\"3\"\nWave=\"8\"\n";
    arcade_test::Run a(cfg), b(cfg);
    for (double t = 0; t < 70; t += 1.0 / 30) {
        const double drop = std::fmod(t, 32.0) < 2 ? 1.0 - std::fmod(t, 32.0) / 4 : 0.0;
        const double build = std::fmod(t, 32.0) > 20 ? (std::fmod(t, 32.0) - 20) / 12 : 0.0;
        REQUIRE(same(a.at(t, 120, drop, build), b.at(t, 120, drop, build)));
    }
    CHECK(tally(a, "hits") > 50);

    // Another seed plays another game.
    arcade_test::Run c(cfg);
    reforce::scene::Environment env;
    env.seed = 12345;
    c.run = std::make_unique<OrganicRun>(c.cfg, env, "arcade", 0.0, 0.0, 600.0, nullptr);
    bool differs = false;
    arcade_test::Run d(cfg);
    for (double t = 0; t < 20; t += 1.0 / 30) differs = differs || !same(c.at(t), d.at(t));
    CHECK(differs);
}

TEST_CASE("arcade invaders: without the beat clock it steps at 120 BPM from its start") {
    reforce::scene::Environment env;
    OrganicRun run(arcade_test::compile(kMarchOnly), env, "arcade", 0.0, 0.0, 600.0, nullptr);
    const FrameMapping map = organicMapping(1920, 1080);
    SpriteAudio a;  // sync.on = 0
    std::vector<LineSegment> prev, segs;
    std::vector<int> changes;
    for (int f = 0; f < 60 * 8; ++f) {
        const double t = f / 60.0;
        segs.clear();
        run.step(t, a, map, LineStyle{}, segs);
        if (f > 0 && !same(segs, prev) && t > 2.5) changes.push_back(int(std::floor(t * 2 + 1e-9)));
        prev = segs;
    }
    CHECK(!segs.empty());
    REQUIRE(changes.size() >= 9);
    for (std::size_t i = 1; i < changes.size(); ++i) CHECK(changes[i] == changes[i - 1] + 1);  // every half second
}

TEST_CASE("arcade invaders: junk keys and junk audio stay finite") {
    arcade_test::Run r(
        "Kind=\"invaders\"\nCols=\"1/0\"\nRows=\"-5\"\nRate=\"log(-1)\"\nFire=\"1e9\"\nPix=\"0/0\"\nWave=\"-3\"\n"
        "Aim=\"sqrt(-1)\"\nSauc=\"1e30\"\nAln=\"1/0\"\nCann=\"log(-1)\"\nShot=\"-4\"\nDebr=\"1e30\"\nX=\"0/0\"\n"
        "Scal=\"1e30\"\nRot=\"1/0\"\n");
    for (double b = 0; b < 40; b += 0.1) CHECK(arcade_test::allFinite(r.at(b, 120, NAN, INFINITY)));
    CHECK(arcade_test::allFinite(r.at(NAN)));
    CHECK(arcade_test::allFinite(r.at(1e300)));
    CHECK(arcade_test::allFinite(r.at(-1e300)));
    // Extremes that are allowed: the largest formation still fits and stays finite.
    arcade_test::Run big("Kind=\"invaders\"\nCols=\"24\"\nRows=\"8\"\nRate=\"4\"\nFire=\"4\"\nPix=\".05\"\n");
    const auto segs = big.play(0, 16);
    CHECK(arcade_test::allFinite(segs));
    CHECK(!segs.empty());
}

TEST_CASE("arcade invaders: segments per frame and the cost of a step stay bounded") {
    // The defaults, and a busy 16 x 6 swarm setting.
    for (const char* cfg : {"Kind=\"invaders\"\n",
                            "Kind=\"invaders\"\nCols=\"16\"\nRows=\"6\"\nPix=\".0085\"\nRate=\"2\"\nFire=\"4\"\n"
                            "Aim=\".55\"\nWave=\"8\"\n"}) {
        CAPTURE(cfg);
        arcade_test::Run r(cfg);
        std::size_t most = 0;
        double worstMs = 0, totalMs = 0;
        int frames = 0;
        for (double b = 0; b < 160; b += 1.0 / 30) {
            // A drop every 32 beats, a build before it: everything at once.
            const double ph = std::fmod(b, 32.0);
            const double drop = ph < 4 ? std::exp(-ph / 4) : 0.0;
            const double build = ph > 20 ? (ph - 20) / 12 : 0.0;
            const auto t0 = std::chrono::steady_clock::now();
            const auto segs = r.at(b, 120, drop, build);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worstMs = std::max(worstMs, ms);
            totalMs += ms;
            ++frames;
            most = std::max(most, segs.size());
        }
        MESSAGE("invaders at 1920 x 1080: at most " << most << " segments per frame; step " << totalMs / frames
                                                    << " ms on average, " << worstMs << " ms at worst");
        CHECK(most < 8000);
        PERF_BUDGET(totalMs / frames < 0.5);
    }
}

TEST_CASE("arcade invaders: big beat jumps catch up boundedly; a jump back restarts") {
    arcade_test::Run r("Kind=\"invaders\"\n");
    r.play(0, 6);
    const auto t0 = std::chrono::steady_clock::now();
    const auto segs = r.at(1e7);  // ~40 million ticks later
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    PERF_BUDGET(ms < 20);
    CHECK(!segs.empty());
    CHECK(arcade_test::allFinite(segs));
    CHECK(segs.size() < 8000);
    // Then it plays on from there.
    const auto later = r.play(1e7, 1e7 + 4);
    CHECK(!later.empty());
    // Back to the start: a fresh game.
    const int waves = tally(r, "waves");
    r.at(0.5);
    CHECK(tally(r, "waves") == waves + 1);
    CHECK(!r.play(0.5, 8).empty());
}
