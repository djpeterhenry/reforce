// Arcade organic `hopper` (PLAN 5Q.2, docs/arcade-design.md): the game's rules (the model in engine/arcade/Hopper.hpp)
// and the drawn organic (through the arcade harness).
#include <doctest/doctest.h>

#include <algorithm>

#include <chrono>
#include <cmath>
#include <cstdio>

#include "PerfBudget.hpp"
#include "arcade_harness.hpp"
#include "engine/arcade/Hopper.hpp"

using namespace reforce::engine::arcade::hopper;
using reforce::engine::LineSegment;
using reforce::engine::LineStyle;
using reforce::engine::SpriteAudio;

namespace {

// Checks one cycle's plan: the route stays on the pyramid, every cube is stepped levels - 1 times (dark to bright), the
// last step exactly at the line.
void checkPlan(const Game& g) {
    const Plan& p = g.plan();
    REQUIRE(int(p.pos.size()) == g.hops() + 1);
    CHECK(p.searched);
    for (int k = 0; k <= g.hops(); ++k) {
        const int q = p.pos[std::size_t(k)];
        CHECK((q >= -1 && q < g.cubes()));
        if (k > 0 && q >= 0 && p.pos[std::size_t(k - 1)] >= 0) CHECK(adjacent(p.pos[std::size_t(k - 1)], q));
        if (q < 0) CHECK(k == p.rideTo - 1);  // only on the disc
    }
    const int steps = g.levels() - 1;
    int last = 0, atLine = 0;
    for (int i = 0; i < g.cubes(); ++i) {
        int landed = 0;
        for (int k = 1; k <= g.hops(); ++k) landed += p.pos[std::size_t(k)] == i;
        CHECK(landed >= steps);
        for (int s = 0; s < steps; ++s) CHECK(p.stepAt[std::size_t(i * steps + s)] >= 1);
        CHECK(p.doneAt[std::size_t(i)] >= 1);
        CHECK(p.doneAt[std::size_t(i)] <= g.hops());
        last = std::max(last, p.doneAt[std::size_t(i)]);
        atLine += p.doneAt[std::size_t(i)] == g.hops();
    }
    CHECK(last == g.hops());
    CHECK(atLine == 1);
    CHECK(p.doneAt[std::size_t(p.pos[std::size_t(g.hops())])] == g.hops());
}

// No ball lands where the player is (or swaps cubes with it over a beat); off-beat balls miss the cube the player
// leaves or lands on.
void checkBalls(const Game& g, bool offbeat) {
    const Plan& p = g.plan();
    for (const auto& b : p.balls) {
        if (!b.alive) continue;
        CHECK(b.col[0] == 0);
        for (int j = 0; j < g.rows(); ++j) {
            CHECK((b.col[j] >= 0 && b.col[j] <= j));
            if (j > 0) CHECK((b.col[j] - b.col[j - 1] == 0 || b.col[j] - b.col[j - 1] == 1));
            const int cube = cubeIndex(j, b.col[j]), t = b.t0 + j;
            if (b.popAt <= t) break;
            REQUIRE(t + 1 <= g.hops());
            CHECK(p.pos[std::size_t(t)] != cube);
            if (offbeat) CHECK(p.pos[std::size_t(t + 1)] != cube);
            if (!offbeat && j > 0)
                CHECK(!(p.pos[std::size_t(t - 1)] == cube && p.pos[std::size_t(t)] == cubeIndex(j - 1, b.col[j - 1])));
        }
    }
}

}  // namespace

TEST_CASE("arcade hopper: the board turns its last top exactly on the cycle line, for many sizes and seeds") {
    double worstMs = 0;
    std::vector<double> planMs;
    int plans = 0;
    for (int levels : {2, 3})
    for (int rows = kMinRows; rows <= kMaxRows; ++rows)
        for (int cyc : {1, 2, 4, 8, 16})
            for (std::uint64_t seed = 1; seed <= (levels == 2 ? 10u : 5u); ++seed) {
                Config c;
                c.rows = rows;
                c.cycBars = cyc;
                c.levels = levels;
                c.seed = seed * 7777;
                c.ballsPerBar = 1;
                Game g(c);
                CAPTURE(levels);
                CAPTURE(rows);
                CAPTURE(cyc);
                CAPTURE(seed);
                CHECK(g.hops() >= (levels - 1) * g.cubes());
                CHECK(g.cycleBars() >= cyc);
                CHECK(g.cycleBars() % cyc == 0);
                for (int cycle = 0; cycle < 4; ++cycle) {
                    const auto t0 = std::chrono::steady_clock::now();
                    g.advance(double(cycle) * g.hops() + 0.01, false);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    worstMs = std::max(worstMs, ms);
                    planMs.push_back(ms);
                    ++plans;
                    checkPlan(g);
                    checkBalls(g, false);
                    // Starts all dark; played through: one step left just before the line.
                    for (int i = 0; i < g.cubes(); ++i) CHECK(g.state(i) == 0);
                    g.advance(double(cycle + 1) * g.hops() - 0.5, false);
                    int left = 0;
                    for (int i = 0; i < g.cubes(); ++i) left += levels - 1 - g.state(i);
                    CHECK(left == 1);
                }
            }
    // The third slowest, not the slowest: one plan preempted on a busy machine (parallel builds) must not fail it.
    std::sort(planMs.begin(), planMs.end());
    const double third = planMs.size() >= 3 ? planMs[planMs.size() - 3] : worstMs;
    MESSAGE("hopper: " << plans << " plans, the slowest " << worstMs << " ms, the third slowest " << third << " ms");
    PERF_BUDGET(third < 10);  // once per cycle; 7-row plans are far below (see the next test)
}

TEST_CASE("arcade hopper: the default board fits 8 bars, larger ones take longer cycles") {
    CHECK(cycleBarsFor(7, 8) == 8);
    CHECK(cycleBarsFor(7, 4) == 8);  // 28 cubes do not fit 16 hops
    CHECK(cycleBarsFor(8, 8) == 16);  // 36 cubes: 20 on odd rows need 39 hops
    CHECK(cycleBarsFor(3, 2) == 2);
    CHECK(cycleBarsFor(7, 8, 3) == 16);  // dark, middle, bright: two landings on each of the 16 even-row cubes
    CHECK(cycleBarsFor(4, 4, 3) == 8);
    // The default board's plan, once per cycle.
    Config c;
    c.seed = 5;
    Game g(c);
    double total = 0, worst = 0;
    for (int cycle = 0; cycle < 200; ++cycle) {
        const auto t0 = std::chrono::steady_clock::now();
        g.advance(double(cycle) * g.hops(), false);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total += ms;
        worst = std::max(worst, ms);
        CHECK(g.plan().searched);
    }
    MESSAGE("hopper: 7-row plans " << total / 200 << " ms on average, worst " << worst << " ms");
    PERF_BUDGET(total / 200 < 0.5);
    CHECK(cycleBarsFor(2, 1) == 2);  // clamped to 3 rows: 6 cubes
}

TEST_CASE("arcade hopper: landings exactly on the beat lines, in the air between them") {
    Config c;
    c.seed = 42;
    Game g(c);
    for (int k = 0; k < 3 * g.hops(); ++k) {
        CAPTURE(k);
        g.advance(k, false);
        const Pose on = g.pose(0.55);
        const int cube = g.plan().pos[std::size_t(k % g.hops())];
        CHECK(on.air == 0);
        CHECK(on.x == doctest::Approx(topX(cube)));
        CHECK(on.y == doctest::Approx(topY(cube)));
        // The tops turn on the landing, not before.
        const int done = g.plan().doneAt[std::size_t(cube)];
        if (done == k % g.hops() && k % g.hops() > 0) {
            CHECK(g.bright(cube));
            g.advance(k - 0.01, false);
            CHECK(!g.bright(cube));
            g.advance(k, false);
        }
        g.advance(k + 0.6, false);
        CHECK(g.pose(0.55).air > 0.1);
    }
    // Three levels: every cycle goes dark, middle, bright; each landing steps at most one state, only upwards.
    Config c3 = c;
    c3.levels = 3;
    Game g3(c3);
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::vector<int> was(std::size_t(g3.cubes()), 0);
        for (int k = 0; k < g3.hops(); ++k) {
            g3.advance(cycle * g3.hops() + k + 0.5, false);
            int changed = 0;
            for (int i = 0; i < g3.cubes(); ++i) {
                const int st = g3.state(i);
                if (k == 0) CHECK(st == 0);
                CHECK((st == was[std::size_t(i)] || st == was[std::size_t(i)] + 1));
                changed += st != was[std::size_t(i)];
                if (st != was[std::size_t(i)]) CHECK(i == g3.plan().pos[std::size_t(k)]);
                was[std::size_t(i)] = st;
            }
            CHECK(changed <= 1);
        }
        int bright = 0;
        for (int i = 0; i < g3.cubes(); ++i) bright += g3.state(i) == 2;
        CHECK(bright == g3.cubes() - 1);  // the last one on the line
    }
}

TEST_CASE("arcade hopper: balls never meet the player, on the beat or the off-beat") {
    for (bool off : {false, true})
        for (double per : {0.5, 1.0, 2.0, 4.0})
            for (int rows : {4, 7, 9})
                for (std::uint64_t seed = 1; seed <= 5; ++seed) {
                    Config c;
                    c.rows = rows;
                    c.ballsPerBar = per;
                    c.offbeat = off;
                    c.seed = seed;
                    Game g(c);
                    int alive = 0;
                    for (int cycle = 0; cycle < 3; ++cycle) {
                        g.advance(cycle * g.hops(), false);
                        checkBalls(g, off);
                        for (const auto& b : g.plan().balls) alive += b.alive;
                        // And drawn: whenever either is down on a cube, they are at least a cube apart.
                        for (double hc = 0; hc < g.hops(); hc += 0.05) {
                            g.advance(cycle * g.hops() + hc, false);
                            const Pose p = g.pose(0.55);
                            for (const auto& b : g.plan().balls) {
                                const BallPose bp = g.ballPose(b, 0.4);
                                if (!bp.visible || p.air > 0.2) continue;
                                CHECK(std::hypot(bp.x - p.x, bp.y - p.y) > 0.45);
                            }
                        }
                    }
                    CHECK(alive > 0);
                }
}

TEST_CASE("arcade hopper: a drop rides the disc to the apex and the board still finishes on the line") {
    int rides = 0, spins = 0, apex = 0;
    for (std::uint64_t seed = 1; seed <= 12; ++seed)
        for (double at : {0.1, 0.7, 5.3, 13.0, 24.5, 30.2}) {
            Config c;
            c.seed = seed;
            c.levels = seed % 2 ? 3 : 2;
            Game g(c);
            g.advance(g.hops() + at, false);
            g.advance(g.hops() + at, true);
            CAPTURE(seed);
            CAPTURE(at);
            checkPlan(g);
            checkBalls(g, false);
            const Plan& p = g.plan();
            if (p.rideTo >= 0) {
                ++rides;
                CHECK(p.rideTo == int(std::floor(at)) + 2);
                apex += p.pos[std::size_t(p.rideTo)] == 0;
                g.advance(g.hops() + (p.rideFrom + p.rideTo) / 2, false);
                CHECK(g.pose(0.55).onDisc);
                g.advance(g.hops() + p.rideTo, false);
                CHECK(!g.pose(0.55).onDisc);
                CHECK(g.pose(0.55).x == doctest::Approx(topX(p.pos[std::size_t(p.rideTo)])));
            } else {
                ++spins;
                if (at < g.hops() - 1) CHECK(p.spinHop == int(std::floor(at)) + 1);
            }
            // Balls on the board burst.
            for (const auto& b : p.balls)
                if (b.popAt < 1e299) CHECK(b.popAt == doctest::Approx(at));
            // A second drop right after is ignored.
            const int made = g.plansMade();
            g.advance(g.hops() + at + 1, true);
            CHECK(g.plansMade() == made);
            g.advance(2.0 * g.hops() - 0.5, false);
            int left = 0;
            for (int i = 0; i < g.cubes(); ++i) left += !g.bright(i);
            CHECK(left == 1);
        }
    MESSAGE("hopper drops: " << rides << " rides (" << apex << " to the apex), " << spins << " spin jumps");
    CHECK(apex >= 24);  // at the start of a cycle there is always a route from the apex
}

TEST_CASE("arcade hopper: draws, finite, deterministic, bounded, on and off the beat clock") {
    const char* text = "Kind=\"hopper\"\n";
    arcade_test::Run a(text), b(text);
    std::size_t most = 0;
    double worstMs = 0, total = 0;
    int frames = 0;
    for (double beat = 0; beat < 70; beat += 2.0 / 60) {
        const double drop = std::fmod(beat, 32) < 4 && beat > 30 ? 1 - std::fmod(beat, 32) / 4 : 0;  // one at beat 32
        const double build = beat > 20 && beat < 32 ? (beat - 20) / 12 : 0;
        const auto t0 = std::chrono::steady_clock::now();
        const auto sa = a.at(beat, 120, drop, build);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worstMs = std::max(worstMs, ms);
        total += ms;
        ++frames;
        const auto sb = b.at(beat, 120, drop, build);
        REQUIRE(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i) {
            CHECK(sa[i].ax == sb[i].ax);
            CHECK(sa[i].ay == sb[i].ay);
            CHECK(sa[i].penA == sb[i].penA);
        }
        CHECK(!sa.empty());
        CHECK(arcade_test::allFinite(sa));
        most = std::max(most, sa.size());
    }
    MESSAGE("hopper: at most " << most << " segments at 1080p, " << total / frames << " ms per step on average, worst "
                               << worstMs << " ms");
    CHECK(most < 8000);
    PERF_BUDGET(total / frames < 0.5);

    // Without the beat clock: 2 beats per second of age.
    arcade_test::Run n(text);
    SpriteAudio quiet;
    for (double t = 0; t < 20; t += 1.0 / 30) {
        std::vector<LineSegment> segs;
        n.run->step(t, quiet, n.map, LineStyle{}, segs);
        CHECK(!segs.empty());
        CHECK(arcade_test::allFinite(segs));
    }
}

TEST_CASE("arcade hopper: junk keys and big beat jumps stay finite and bounded") {
    arcade_test::Run r(
        "Kind=\"hopper\"\nRows=\"1e9\"\nCyc=\"-5\"\nLevels=\"sqrt(-1)\"\nBall=\"1e6\"\nOffb=\"1/0\"\nSize=\"-3\"\n"
        "Hop=\"1e12\"\nTLo=\"-1\"\nTHi=\"sqrt(-1)\"\nSidL=\"5\"\nCPen=\"0/0\"\nX=\"1e30\"\nScal=\"-1/0\"\n");
    for (double beat : {0.0, 3.5, 1e6, 1e6 + 0.5, -40.0, 1e14, 7.25}) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto segs = r.at(beat);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(arcade_test::allFinite(segs));
        CHECK(segs.size() < 20000);
        PERF_BUDGET(ms < 50);
    }
    // A sane board jumping far ahead re-plans once and carries on from where the player stood.
    arcade_test::Run j("Kind=\"hopper\"\nRows=\"7\"\n");
    j.play(0, 6);
    for (double beat : {1000.25, 1000.5, 123456.0, 50.0}) {
        const auto segs = j.at(beat);
        CHECK(!segs.empty());
        CHECK(segs.size() < 8000);
    }
    Config c;
    Game g(c);
    g.advance(5.5, false);
    const int start = g.plan().pos[5];
    g.advance(1e6 * 32 + 3, false);
    CHECK(g.plan().pos[0] == start);
    checkPlan(g);
}

TEST_CASE("arcade hopper: tops read on any ColorMap: picked levels, hollow dark tops") {
    using namespace reforce::engine;
    // A map whose upper half is all white: the default tops (.6 and 1) would look the same.
    Palette pal{};
    MapLut lut{};
    for (int k = 0; k < kPaletteSize; ++k) {
        const float v = std::min(1.0f, float(k) / 127.0f);
        pal[std::size_t(k)] = {v, v, v, 1.0f};
        lut[std::size_t(k)] = float(k) / 255.0f;
    }
    OrganicColors colors;
    effectiveColors(pal, lut, colors.lab);
    colors.version = 1;
    const TopPick pick = pickTops(colors.lab, TopSpec{});
    const double lo = pick.level[0] / 255.0;
    REQUIRE(lo < 0.5);  // the picked dark top is below the white half

    auto near = [](const std::vector<LineSegment>& segs, double v) {
        int n = 0;
        for (const auto& s : segs) n += std::abs(s.penA - v) < 0.004;
        return n;
    };
    const char* text = "Kind=\"hopper\"\n";
    arcade_test::Run plain(text), picked(text), off("Kind=\"hopper\"\nTAut=\"0\"\n");
    picked.colors = &colors;
    off.colors = &colors;
    std::vector<LineSegment> sp, sc, so;
    for (double beat = 0; beat <= 8.5; beat += 0.25) sp = plain.at(beat), sc = picked.at(beat), so = off.at(beat);
    CHECK(near(sp, 0.6) > 20);  // the fixed dark tops' rims
    CHECK(near(sc, lo) > 20);   // the picked ones
    CHECK(near(sc, 0.6) < near(sp, 0.6) / 4);
    // TAut 0: the fixed levels, exactly as without colors.
    REQUIRE(so.size() == sp.size());
    for (std::size_t i = 0; i < so.size(); ++i) CHECK(so[i].penA == sp[i].penA);

    // Dark tops are hollow: many more seam-colored strokes on a dark board than on the finished one.
    arcade_test::Run h(text);
    const auto dark = h.play(0, 2.5);
    const auto done = h.play(2.5, 31.9);
    CHECK(near(dark, 0.04) > near(done, 0.04) + 100);
}
