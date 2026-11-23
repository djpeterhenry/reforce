// Arcade organic `bricks` (PLAN 5Q.6, docs/arcade-design.md): every bounce on a tick line, the paddle always under
// the ball, the wall cleared exactly on the cycle line, multiball on drops, faster through breakdowns, deterministic.
#include <doctest/doctest.h>

#include <algorithm>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <string>
#include <vector>

#include "PerfBudget.hpp"
#include "arcade_harness.hpp"
#include "engine/arcade/Bricks.hpp"

using namespace reforce::engine;
using namespace reforce::engine::arcade;

namespace {

bool onLine(double t, double tick) {
    const double k = t / tick;
    return std::abs(k - std::round(k)) < 1e-6;
}

// The largest relative difference of any flight's speed from Spd seen by checkCycle.
double worstSpeed = 0;

// Bricks per hit seen by checkCycle: hits before the line by bricks broken (0 = a knock, 1, 2, 3, 4+), and per cycle
// the bricks breaking on the line (1, 2, 3, 4+).
struct Tally {
    int hits[5] = {}, line[5] = {}, cycles = 0;
    int breaking() const { return hits[1] + hits[2] + hits[3] + hits[4]; }
    double single() const { return breaking() ? double(hits[1]) / breaking() : 1; }
    int most() const {
        for (int k = 4; k > 0; --k)
            if (hits[k]) return k;
        return 0;
    }
    std::string text() const {
        char b[256];
        std::snprintf(b, sizeof b,
                      "%d cycles, hits breaking 0/1/2/3/4+: %d %d %d %d %d (%.1f %% of breaking hits single), "
                      "bricks on the line 1/2/3/4+: %d %d %d %d",
                      cycles, hits[0], hits[1], hits[2], hits[3], hits[4], 100 * single(), line[1], line[2], line[3],
                      line[4]);
        return b;
    }
};
Tally tally;

// Checks the whole plan of the cycle the game is in (call just before the line).
void checkCycle(const bricks::Game& g, double tick) {
    const auto& p = g.params();
    const double W = p.halfW, H = p.halfH, r = g.ballRadius();
    REQUIRE(!g.segs().empty());
    for (const auto& s : g.segs()) {
        CAPTURE(s.t0);
        CAPTURE(s.t1);
        // Every paddle and brick hit on a tick line (side-wall bounces land where they fall), flights forward in time,
        // inside the playfield.
        if (s.legEnd) CHECK(onLine(s.t1 - g.cycleStart(), tick));
        else CHECK((s.end == bricks::Hit::Wall || s.end == bricks::Hit::Ceiling));
        if (!s.legEnd && s.end == bricks::Hit::Ceiling) CHECK(s.y1 == doctest::Approx(H - r));
        CHECK(s.t1 > s.t0);
        CHECK(s.t1 <= g.cycleEnd() + 1e-9);
        for (double x : {s.x0, s.x1}) CHECK(std::abs(x) <= W - r + 1e-9);
        for (double y : {s.y0, s.y1}) {
            CHECK(y >= g.ballContactY() - 1e-9);
            CHECK(y <= H - r + 1e-9);
        }
        // The paddle is under ball 0 at every paddle hit; extra balls use the shield.
        if (s.end == bricks::Hit::Paddle) {
            CHECK(s.ball == 0);
            CHECK(s.y1 == doctest::Approx(g.ballContactY()));
            CHECK(std::abs(g.paddleX(s.t1) - s.x1) <= 0.5 * p.paddle);
        }
        if (s.end == bricks::Hit::Shield) CHECK(s.ball > 0);
        if (s.end == bricks::Hit::Wall && !s.legEnd) CHECK(std::abs(std::abs(s.x1) - (W - r)) < 1e-9);
    }
    // Each ball's flights join up, and each ends on the line. The speed only changes at the paddle (the up leg sets
    // it; the way back down keeps it), and stays near Spd.
    for (int b = 0; b < g.balls(); ++b) {
        const bricks::Seg* prev = nullptr;
        double v = -1;
        for (const auto& s : g.segs()) {
            if (s.ball != b) continue;
            const double sv = bricks::Game::speedOf(s);
            worstSpeed = std::max(worstSpeed, std::abs(sv / p.speed - 1));
            if (prev) {
                CHECK(s.t0 == doctest::Approx(prev->t1));
                CHECK(s.x0 == doctest::Approx(prev->x1));
                CHECK(s.y0 == doctest::Approx(prev->y1));
                const bool atPaddle = prev->end == bricks::Hit::Paddle || prev->end == bricks::Hit::Shield;
                if (!atPaddle && v > 0) CHECK(sv == doctest::Approx(v).epsilon(1e-6));
            }
            v = sv;
            prev = &s;
        }
        // (A ball split off at the line itself joins on the next cycle.)
        if (b == 0) REQUIRE(prev);
        // (An extra ball may end in the shield before the line, once its bricks are done.)
        if (prev && (b == 0 || prev->end != bricks::Hit::Shield)) CHECK(prev->t1 == doctest::Approx(g.cycleEnd()));
    }
    // Every brick breaks by the line, the last ones exactly on it, each at a bounce of the ball.
    double last = -1e300;
    int broken = 0;
    for (const auto& br : g.bricks()) {
        CHECK(br.broken <= g.cycleEnd() + 1e-9);
        last = std::max(last, br.broken);
        bool hit = false;
        for (const auto& s : g.segs()) hit = hit || (s.end == bricks::Hit::Brick && std::abs(s.t1 - br.broken) < 1e-9);
        CHECK(hit);
        ++broken;
    }
    CHECK(broken == g.brickCount());
    CHECK(last == doctest::Approx(g.cycleEnd()));
    int counted = 0, onLine = 0;
    for (const auto& s : g.segs()) {
        counted += s.end == bricks::Hit::Brick ? s.bricks : 0;
        if (s.end != bricks::Hit::Brick) continue;
        if (s.t1 >= g.cycleEnd() - 1e-9) onLine += s.bricks;
        else ++tally.hits[std::min(s.bricks, 4)];
    }
    ++tally.cycles;
    ++tally.line[std::clamp(onLine, 0, 4)];
    CHECK(counted == g.brickCount());
}

// Plays `cycles` cycles at 1/16-beat steps from bar `bar0`, checking each just before its line.
void playCycles(bricks::Game& g, double bar0, int cycles, double drop = 0, double build = 0, double dropAtBar = -1) {
    const int cyc = g.params().cycBars;
    double bar = bar0;
    for (int k = 0; k < cycles; ++k) {
        const double end = (std::floor(bar / cyc) + 1) * cyc;
        for (; bar < end - 1e-9; bar += 1.0 / 64) {
            const bool dropNow = dropAtBar >= 0 && bar >= dropAtBar && bar < dropAtBar + 0.5;
            g.update(bar, dropNow ? 1.0 : drop, build);
        }
        g.update(end - 1e-6, 0, build);
        checkCycle(g, g.tick());
        bar = end;
    }
}

}  // namespace

TEST_CASE("arcade bricks: draws, finite, on the beat clock") {
    arcade_test::Run r("Kind=\"bricks\"\n");
    const auto segs = r.play(0, 8);
    CHECK(!segs.empty());
    CHECK(arcade_test::allFinite(segs));
}

TEST_CASE("arcade bricks: bounces on beat lines, the paddle meets the ball, the wall clears on the cycle line") {
    struct Case {
        int cols, rows, cyc;
        double rate;
    };
    const Case cases[] = {{12, 5, 16, 1}, {8, 3, 8, 1}, {20, 8, 16, 2}, {1, 1, 1, 1}, {40, 16, 4, 1},
                          {6, 4, 2, 0.5}, {16, 6, 32, 1}, {10, 5, 16, 3}};
    worstSpeed = 0;
    tally = {};
    int levels[7] = {};
    for (const auto& c : cases)
        for (std::uint64_t seed : {1ULL, 7ULL, 12345ULL}) {
            CAPTURE(c.cols);
            CAPTURE(c.rows);
            CAPTURE(c.cyc);
            CAPTURE(c.rate);
            CAPTURE(seed);
            bricks::Params p;
            p.cols = c.cols;
            p.rows = c.rows;
            p.cycBars = c.cyc;
            p.rate = c.rate;
            bricks::Game g(p, seed);
            playCycles(g, 0, 3);
            for (int l = 0; l < 7; ++l) levels[l] += g.relaxed(l);
            CHECK(g.balls() == 1);
            CHECK(!g.fast());
        }
    // Constant speed: within 3 % of Spd (the planner's normal tolerance), changing only at the paddle (checkCycle).
    // Volleys per relaxation level (0: base speed, clear paths; 1: flatter paths; 2.. bend the rules).
    MESSAGE("bricks: largest speed difference from Spd ", 100 * worstSpeed, " %; volleys per level ", levels[0], " ",
            levels[1], " ", levels[2], " ", levels[3], " ", levels[4], " ", levels[5], " ", levels[6]);
    // (Sizes far from the content's, e.g. 40 x 16 in 4 bars, now and then bend the rules: counted, rare.)
    CHECK(levels[2] + levels[3] + levels[4] + levels[5] + levels[6] <= (levels[0] + levels[1]) / 500);
    MESSAGE("bricks over sizes: ", tally.text());
}

TEST_CASE("arcade bricks: the content walls break one brick per hit, one on the line") {
    // The committed Brick configs (16:9 field), 12 seeds x 2 cycles each; then with a drop in the first cycle.
    for (const char* name : {"Brick Wall", "Brick Storm"}) {
        CAPTURE(name);
        std::ifstream in(std::filesystem::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Organic.package" /
                         (std::string(name) + ".txt"));
        std::stringstream text;
        text << in.rdbuf();
        const auto cfg = arcade_test::compile(text.str());
        reforce::scene::Environment env;
        reforce::scene::OrganicInstance inst(*cfg, env);
        const bricks::Params p = bricks::paramsFrom(inst);
        for (int drops = 0; drops < 2; ++drops) {
            tally = {};
            worstSpeed = 0;
            int levels[7] = {};
            for (std::uint64_t seed = 1; seed <= 12; ++seed) {
                CAPTURE(seed);
                bricks::Game g(p, seed);
                playCycles(g, 0, 2, 0, 0, drops ? 0.3 * p.cycBars : -1);
                for (int l = 0; l < 7; ++l) levels[l] += g.relaxed(l);
            }
            char lv[160];
            std::snprintf(lv, sizeof lv, "; speed within %.2f %%; volleys per level %d %d %d %d %d %d %d",
                          100 * worstSpeed, levels[0], levels[1], levels[2], levels[3], levels[4], levels[5],
                          levels[6]);
            const std::string msg = std::string(name) + (drops ? " with a drop: " : ": ") + tally.text() + lv;
            MESSAGE(msg);
            // One brick per hit: nearly always, never three; one on the line, two at most (a drop: one per ball).
            CHECK(tally.single() >= 0.97);
            CHECK(tally.most() <= 2);
            CHECK(tally.line[0] == 0);
            if (!drops) CHECK(tally.line[3] + tally.line[4] == 0);
            if (!drops) CHECK(tally.line[1] >= tally.cycles * 3 / 4);
            // Constant speed within 3 %; after a drop the multiball now and then bends the rules (counted, rare).
            const int bent = levels[2] + levels[3] + levels[4] + levels[5] + levels[6];
            if (!drops) CHECK(worstSpeed <= 0.0301);
            CHECK(bent <= (levels[0] + levels[1]) / (drops ? 200 : 1000));
        }
    }
}

TEST_CASE("arcade bricks: at the defaults the wall wears away evenly, a small burst on the line") {
    // Per cycle: bricks in the final hit, up legs that break nothing, and the longest stretch without a break.
    int finalBricks = 0, empty = 0, ups = 0, cycles = 0;
    double longest = 0;
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        bricks::Params p;
        bricks::Game g(p, seed);
        for (int k = 0; k < 2; ++k) {
            g.update(16.0 * k + 15.99, 0, 0);
            std::vector<double> breaks;
            for (const auto& s : g.segs()) {
                if (!s.legEnd || s.y1 <= g.ballContactY() + 1e-9) continue;
                ++ups;
                if (s.end == bricks::Hit::Brick && s.bricks > 0) breaks.push_back(s.t1);
                else ++empty;
                if (s.t1 >= g.cycleEnd() - 1e-9) finalBricks += s.bricks;
            }
            double prev = g.cycleStart();
            for (double t : breaks) {
                longest = std::max(longest, t - prev);
                prev = t;
            }
            ++cycles;
        }
    }
    MESSAGE("bricks defaults: ", double(finalBricks) / cycles, " bricks in the final hit, ", 100.0 * empty / ups,
            "% of up legs break nothing, longest stretch without a break ", longest, " beats");
    CHECK(double(finalBricks) / cycles <= 1.5);
    CHECK(longest <= 16);
}

TEST_CASE("arcade bricks: starting mid-phrase, and after the clock jumps, the wall still clears on the line") {
    bricks::Params p;
    bricks::Game g(p, 99);
    playCycles(g, 37.3, 2);  // starts 5.3 bars into a cycle
    const int plans = g.plans();
    g.update(5000.25, 0, 0);  // far ahead
    CHECK(g.plans() - plans <= 2);
    playCycles(g, 5000.25, 1);
    g.update(12.0, 0, 0);  // back in time
    playCycles(g, 12.0, 1);
}

TEST_CASE("arcade bricks: a drop splits the ball, every ball still on the lines; a breakdown bounces on 8ths") {
    bricks::Params p;
    for (std::uint64_t seed : {3ULL, 4ULL, 5ULL}) {
        CAPTURE(seed);
        bricks::Game g(p, seed);
        playCycles(g, 0, 1, 0, 0, 3.3);  // drop 3.3 bars in
        CHECK(g.balls() == 1 + p.multi);
        int extra = 0;
        for (const auto& s : g.segs()) extra += s.ball > 0 ? 1 : 0;
        CHECK(extra > 6);
        // The multiball lasts to the line; a drop near the end carries it into the next cycle.
        playCycles(g, 16, 1);
        CHECK(g.balls() == 1);
        playCycles(g, 32, 1, 0, 0, 46.5);
        g.update(48.01, 0, 0);
        CHECK(g.balls() == 1 + p.multi);
        playCycles(g, 48.01, 1);
    }
    {
        // A breakdown: bounces on the 8th lines from then on, some between the beats; back to beats on the next line.
        bricks::Game g(p, 8);
        for (double bar = 0; bar < 6; bar += 1.0 / 64) g.update(bar, 0, 0);
        playCycles(g, 6, 1, 0, 0.8);
        CHECK(g.fast());
        CHECK(g.tick() == doctest::Approx(0.5));
        int offBeat = 0;
        for (const auto& s : g.segs()) offBeat += s.legEnd && !onLine(s.t1, 1.0) ? 1 : 0;
        CHECK(offBeat > 4);
        playCycles(g, 16, 1);
        CHECK(!g.fast());
    }
}

TEST_CASE("arcade bricks: drops and breakdowns at odd moments, many seeds and sizes") {
    worstSpeed = 0;
    tally = {};
    int levels[7] = {};
    for (std::uint64_t seed = 20; seed < 60; ++seed) {
        CAPTURE(seed);
        bricks::Params p;
        p.cols = 4 + int(seed % 13);
        p.rows = 1 + int(seed % 7);
        p.cycBars = 2 + int(seed % 5) * 3;
        p.multi = int(seed % 4);
        p.rate = seed % 3 ? 1 : 2;
        bricks::Game g(p, seed);
        const double cyc = p.cycBars;
        // A drop somewhere in the first cycle, a breakdown through part of the second.
        const double dropBar = 0.05 + std::fmod(seed * 0.618, 1.0) * cyc;
        playCycles(g, 0, 1, 0, 0, dropBar);
        const double buildBar = cyc + std::fmod(seed * 0.414, 1.0) * cyc;
        double bar = cyc;
        for (; bar < buildBar; bar += 1.0 / 64) g.update(bar, 0, 0);
        playCycles(g, bar, 1, 0, 0.9, bar + 0.2);
        playCycles(g, 2 * cyc, 1);
        for (int l = 0; l < 7; ++l) levels[l] += g.relaxed(l);
    }
    // Rare volleys bend the rules here (short cycles, multiball, odd sizes): counted.
    const int bent = levels[2] + levels[3] + levels[4] + levels[5] + levels[6];
    char lv[200];
    std::snprintf(lv, sizeof lv, "; largest speed difference from Spd %.1f %%, volleys per level %d %d %d %d %d %d %d",
                  100 * worstSpeed, levels[0], levels[1], levels[2], levels[3], levels[4], levels[5], levels[6]);
    const std::string msg = "bricks with drops and breakdowns: " + tally.text() + lv;
    MESSAGE(msg);
    CHECK(bent <= (levels[0] + levels[1]) / 100);
}

TEST_CASE("arcade bricks: deterministic (same seed, same frames), seeds differ") {
    const std::string text = "Kind=\"bricks\"\n";
    arcade_test::Run a(text), b(text);
    for (double beat = 0; beat < 70; beat += 0.37) {
        const auto sa = a.at(beat, 120, beat > 30 && beat < 31 ? 1 : 0);
        const auto sb = b.at(beat, 120, beat > 30 && beat < 31 ? 1 : 0);
        REQUIRE(sa.size() == sb.size());
        bool same = true;
        for (std::size_t i = 0; i < sa.size(); ++i)
            same = same && sa[i].ax == sb[i].ax && sa[i].ay == sb[i].ay && sa[i].bx == sb[i].bx &&
                   sa[i].by == sb[i].by && sa[i].penA == sb[i].penA && sa[i].radius == sb[i].radius;
        CHECK(same);
    }
    bricks::Params p;
    bricks::Game g1(p, 1), g2(p, 2);
    g1.update(1, 0, 0);
    g2.update(1, 0, 0);
    bool differ = g1.segs().size() != g2.segs().size();
    for (std::size_t i = 0; !differ && i < g1.segs().size(); ++i) differ = g1.segs()[i].x1 != g2.segs()[i].x1;
    CHECK(differ);
}

TEST_CASE("arcade bricks: without the beat clock it runs at 120 BPM from the start") {
    auto cfg = arcade_test::compile("Kind=\"bricks\"\n");
    reforce::scene::Environment env;
    OrganicRun run(cfg, env, "arcade", 0.0, 0.0, 600.0, nullptr);
    const FrameMapping map = organicMapping(1280, 720);
    SpriteAudio a;  // sync.on = 0
    std::size_t drawn = 0;
    for (int f = 0; f < 600; ++f) {
        std::vector<LineSegment> segs;
        run.step(f / 60.0, a, map, LineStyle{}, segs);
        CHECK(arcade_test::allFinite(segs));
        drawn += segs.size();
    }
    CHECK(drawn > 0);
}

TEST_CASE("arcade bricks: junk keys stay finite") {
    const char* junk[] = {
        "Kind=\"bricks\"\nCols=\"-5\"\nRows=\"1e30\"\nCyc=\"0\"\nRate=\"-1\"\nSpd=\"1e9\"\nBall=\"-1\"\nPad=\"1e9\"\n",
        "Kind=\"bricks\"\nCols=\"1e9\"\nRows=\"0\"\nCyc=\"1e9\"\nRate=\"1e9\"\nBld=\"99\"\nGap=\"5\"\nBrkH=\"9\"\n",
        "Kind=\"bricks\"\nFldW=\"0\"\nFldH=\"1e9\"\nMult=\"1e9\"\nFrag=\"-3\"\nFLif=\"0\"\nLay=\"42\"\nWall=\"-2\"\n",
        "Kind=\"bricks\"\nX=\"1e30\"\nY=\"-1e30\"\nScal=\"0\"\nRot=\"1e30\"\nPen=\"9\"\n",
    };
    for (const char* text : junk) {
        CAPTURE(text);
        arcade_test::Run r(text);
        for (double beat = 0; beat < 40; beat += 0.71) {
            const auto segs = r.at(beat, 120, beat > 20 && beat < 21 ? 1 : 0, beat > 10 ? 0.7 : 0);
            CHECK(arcade_test::allFinite(segs));
            CHECK(segs.size() < 20000);
        }
    }
}

TEST_CASE("arcade bricks: segments per frame and step cost stay bounded") {
    arcade_test::Run r("Kind=\"bricks\"\n");
    std::size_t most = 0;
    double worst = 0, total = 0;
    int frames = 0;
    const double perFrame = 120 / 60.0 / 60.0;
    for (double beat = 0; beat < 140; beat += perFrame) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto segs = r.at(beat, 120, beat > 70 && beat < 71 ? 1 : 0, beat > 100 && beat < 128 ? 0.8 : 0);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, ms);
        total += ms;
        ++frames;
        most = std::max(most, segs.size());
    }
    MESSAGE("bricks at 1920 x 1080: most segments ", most, ", mean step ", total / frames, " ms, worst ", worst, " ms");
    CHECK(most < 8000);
    PERF_BUDGET(total / frames < 0.5);
}

TEST_CASE("arcade bricks: a big beat jump catches up with at most two plans") {
    bricks::Params p;
    p.cols = 40;
    p.rows = 16;
    p.cycBars = 64;
    p.rate = 4;
    p.multi = 6;
    bricks::Game g(p, 5);
    g.update(0, 1, 1);
    const int plans = g.plans();
    const auto t0 = std::chrono::steady_clock::now();
    g.update(123456.5, 1, 1);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("bricks: jumping into a 40 x 16, 64-bar cycle (4 ticks per beat x 2, 7 balls) ", ms, " ms");
    CHECK(g.plans() - plans <= 2);
    PERF_BUDGET(ms < 200);
}


