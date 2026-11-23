// Arcade organic `cycles` (PLAN 5Q.3, docs/arcade-design.md "Light cycles"): the game's rules move by move
// (engine/arcade/Cycles.hpp) and the organic drawing it.
#include <doctest/doctest.h>

#include <algorithm>

#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "PerfBudget.hpp"
#include "arcade_harness.hpp"
#include "engine/arcade/Cycles.hpp"

using namespace reforce::engine;
using reforce::engine::arcade::CyclesConfig;
using reforce::engine::arcade::CyclesSim;

namespace {

// Steps a sim at 60 fps (120 BPM) from beat b0 to b1, with bar = beat / 4.
void play(CyclesSim& sim, double b0, double b1, double drop = 0, double build = 0) {
    for (double b = b0; b <= b1 + 1e-9; b += 2.0 / 60.0) sim.advance(b, b / 4.0, drop, build);
}

bool onCenter(double v) { return std::abs(v - std::round(v)) < 1e-9; }

}  // namespace

TEST_CASE("arcade cycles: turns only on turn lines, heads on cell centers on every beat line") {
    for (int turn : {1, 2}) {
        for (double build : {0.0, 1.0}) {
            CAPTURE(turn);
            CAPTURE(build);
            CyclesConfig cfg;
            cfg.turn = turn;
            CyclesSim sim(cfg, 7);
            long turns = 0, offBeatTurns = 0, ticks = 0;
            sim.onTick = [&](const CyclesSim& s, double t, bool turnLine) {
                ++ticks;
                // Every tick lands on a 1 / rate fraction of the beat.
                const double perBeat = (t - std::floor(t)) * s.rate();
                CHECK(std::abs(perBeat - std::round(perBeat)) < 1e-6);
                for (const auto& c : s.cycles()) {
                    if (!c.turned) continue;
                    ++turns;
                    CHECK(turnLine);
                    // Turn lines: beats, or (Turn 2, or through a breakdown) 8ths.
                    const int div = std::min(4, turn * (build >= 0.5 ? 2 : 1));
                    CHECK(onCenter(t * div));
                    if (!onCenter(t)) ++offBeatTurns;
                }
            };
            for (int b = 0; b < 200; ++b) {
                play(sim, b, b + 0.97, 0, build);
                sim.advance(b + 1, (b + 1) / 4.0, 0, build);
                for (int i = 0; i < cfg.num; ++i) {
                    if (!sim.cycles()[std::size_t(i)].alive) continue;
                    double x, y, a;
                    sim.head(i, b + 1, x, y, a);
                    CHECK(onCenter(x));
                    CHECK(onCenter(y));
                    CHECK(x >= 0);
                    CHECK(y >= 0);
                    CHECK(x <= cfg.cols - 1);
                    CHECK(y <= cfg.rows - 1);
                    // Half way through a move the head points along it.
                    const auto& tr = sim.cycles()[std::size_t(i)].trail;
                    const double mid = 0.5 * (tr[tr.size() - 2].t + tr.back().t);
                    sim.head(i, mid, x, y, a);
                    const int dx = tr.back().x - tr[tr.size() - 2].x, dy = tr.back().y - tr[tr.size() - 2].y;
                    CHECK(std::cos(a) == doctest::Approx(dx).epsilon(1e-9));
                    CHECK(std::sin(a) == doctest::Approx(dy).epsilon(1e-9));
                }
            }
            CHECK(turns > 50);
            CHECK(sim.stats().offLineTurns == 0);
            if (turn == 2 || build >= 0.5) CHECK(offBeatTurns > 0);  // 8th-note turns do happen
            else CHECK(offBeatTurns == 0);
            // Twice the speed through a breakdown.
            CHECK(ticks == doctest::Approx(200.0 * cfg.spd * (build >= 0.5 ? 2 : 1)).epsilon(0.02));
            // Turns favour the bar lines (a quarter of the beat lines).
            if (build == 0 && turn == 1) CHECK(double(sim.stats().barTurns) / double(turns) > 0.3);
        }
    }
}

// Watches a sim tick by tick: walls retract after Tail bars, no head ever rides into a wall (it crashes instead),
// crashes only when boxed in (or rammed by a boxed-in cycle), and crashed cycles rez in on a bar line Out bars later
// one in from the border (or with the next round).
struct Watch {
    long entered = 0, rezzes = 0, lateRezzes = 0;
    std::vector<char> alive;
    void attach(CyclesSim& sim) {
        alive.assign(sim.cycles().size(), 1);
        sim.onTick = [this](const CyclesSim& s, double t, bool) {
            std::multiset<std::pair<int, int>> standing;
            int points = 0;
            for (const auto& c : s.cycles()) {
                if (!c.alive) {
                    CHECK(c.trail.empty());
                    continue;
                }
                REQUIRE(c.trail.size() >= 1);
                if (c.trail.size() >= 2) CHECK(c.trail[1].t > t - s.tailBeats());
                for (const auto& p : c.trail) standing.insert({p.x, p.y});
                points += int(c.trail.size());
            }
            CHECK(s.wallCells() <= points);
            for (std::size_t i = 0; i < s.cycles().size(); ++i) {
                const auto& c = s.cycles()[i];
                if (c.alive && standing.count({c.trail.back().x, c.trail.back().y}) > 1) ++entered;
                if (c.alive && !alive[i]) {
                    // Back in: on a bar line, Out bars after the crash at the earliest, at the border unless a round
                    // started.
                    ++rezzes;
                    CHECK(c.spawnBeat == t);
                    CHECK(onCenter(t / 4.0));
                    if (t != s.roundBeat()) {
                        CHECK(t >= c.outSince + 4.0 * s.config().out - 1e-9);
                        const auto& p = c.trail.front();
                        CHECK((p.x == 1 || p.y == 1 || p.x == s.config().cols - 2 || p.y == s.config().rows - 2));
                        if (t > c.outSince + 4.0 * s.config().out + 4.0) ++lateRezzes;  // no room on the first line
                    }
                }
                alive[i] = c.alive ? 1 : 0;
            }
        };
    }
};

TEST_CASE("arcade cycles: walls retract after Tail bars; crashes only when boxed in, rarely; out for Out bars") {
    // The defaults with 2, 4 and 8 cycles, and the swarm's settings (8 on a finer grid, turning on 8ths).
    for (int variant = 0; variant < 4; ++variant) {
        const int num = variant == 0 ? 2 : variant == 1 ? 4 : 8;
        CAPTURE(variant);
        CyclesConfig cfg;
        cfg.num = num;
        cfg.tail = variant == 3 ? 2 : 4;
        if (variant == 3) {
            cfg.cols = 64;
            cfg.rows = 36;
            cfg.turn = 2;
            cfg.mirror = 0.5;
        }
        CyclesSim sim(cfg, 11 + std::uint64_t(num));
        Watch w;
        w.attach(sim);
        play(sim, 0, 1000);
        const auto& st = sim.stats();
        MESSAGE(num << " cycles: " << st.crashes << " crashes (" << st.boxedCrashes << " boxed in, " << st.rammed
                    << " rammed, " << st.headOns << " head-on) in 1000 beats, " << st.respawns << " rezzed in, "
                    << st.rounds << " rounds");
        CHECK(w.entered == 0);  // nobody rides through a wall
        CHECK(st.unexplained == 0);
        CHECK(st.crashes == st.boxedCrashes + st.rammed);
        CHECK(w.lateRezzes <= w.rezzes / 4 + 1);
        // Rare (8 cycles with four-bar walls crowd the default arena: the swarm's settings are what to use).
        CHECK(st.crashes <= (variant == 2 ? 200 : variant == 3 ? 12 : 8));
        // A wall laid now is gone Tail bars later.
        int i0 = 0;
        while (!sim.cycles()[std::size_t(i0)].alive) ++i0;
        const auto p = sim.cycles()[std::size_t(i0)].trail.back();
        play(sim, 1000, 1000 + sim.tailBeats() + 1.5);
        bool still = false;
        for (const auto& c : sim.cycles())
            for (const auto& q : c.trail) still = still || (q.x == p.x && q.y == p.y && q.t == p.t);
        CHECK(!still);
    }
}

TEST_CASE("arcade cycles: a crash explodes, the wall dissolves over a beat, the cycle rezzes in on a later bar line") {
    // A crowded little arena with long walls: crashes and head-ons are certain.
    CyclesConfig cfg;
    cfg.num = 8;
    cfg.cols = 16;
    cfg.rows = 9;
    cfg.tail = 16;
    cfg.reset = 0;
    cfg.mirror = 0.5;
    CyclesSim a(cfg, 21), b(cfg, 21);
    Watch w;
    w.attach(a);
    std::size_t mostSparks = 0;
    long seenCrashes = 0;
    for (double beat = 0; beat < 400; beat += 2.0 / 60.0) {
        a.advance(beat, beat / 4.0, 0, 0);
        b.advance(beat, beat / 4.0, 0, 0);
        mostSparks = std::max(mostSparks, a.sparks().size());
        for (const auto& s : a.sparks()) {
            REQUIRE(std::isfinite(s.x + s.y + s.vx + s.vy + s.life + s.t0 + s.ang + s.spin));
            CHECK(beat <= s.t0 + s.life);  // pruned once faded
        }
        for (const auto& g : a.ghosts()) CHECK(beat <= g.t0 + CyclesSim::kDissolveBeats);
        for (const auto& k : a.blasts()) CHECK(beat <= k.t + 2.0);
        if (a.stats().crashes > seenCrashes) {
            // This frame's crashes: an explosion (sparks and spinning fragments from the blast) and a dissolving wall.
            seenCrashes = a.stats().crashes;
            const auto& k = a.crashLog().back();
            CHECK(!a.cycles()[std::size_t(k.cycle)].alive);
            CHECK(a.cycles()[std::size_t(k.cycle)].outSince == k.t);
            int fragments = 0, sparks = 0;
            for (const auto& s : a.sparks())
                if (s.t0 == k.t && s.x == k.x && s.y == k.y) (s.len > 0 ? fragments : sparks)++;
            CHECK(fragments >= 8);
            CHECK(sparks >= 20);
            bool ghost = false;
            for (const auto& g : a.ghosts()) ghost = ghost || g.t0 == k.t;
            CHECK(ghost);
        }
        // Deterministic: the same seed and beats, the same game.
        REQUIRE(a.stats().crashes == b.stats().crashes);
        for (std::size_t i = 0; i < a.cycles().size(); ++i) {
            REQUIRE(a.cycles()[i].alive == b.cycles()[i].alive);
            if (a.cycles()[i].alive) CHECK(a.cycles()[i].trail.back().x == b.cycles()[i].trail.back().x);
        }
    }
    const auto& st = a.stats();
    MESSAGE("crowded: " << st.crashes << " crashes, " << st.headOns << " head-on, " << st.respawns << " rezzed in, "
                        << mostSparks << " sparks at most");
    CHECK(st.crashes > 10);
    CHECK(st.headOns > 0);
    CHECK(st.respawns > 5);
    CHECK(st.unexplained == 0);
    CHECK(w.entered == 0);
    CHECK(w.rezzes == st.respawns);
    CHECK(mostSparks <= 4000);

    // A phrase line brings everyone back with the formation, out or not.
    cfg.reset = 16;
    CyclesSim r(cfg, 21);
    Watch wr;
    wr.attach(r);
    play(r, 0, 63.9);
    int out = 0;
    for (const auto& c : r.cycles()) out += c.alive ? 0 : 1;
    r.advance(64.0, 16.0, 0, 0);
    for (const auto& c : r.cycles()) {
        CHECK(c.spawnBeat == 64.0);
        CHECK((c.alive || c.outSince == 64.0));  // (or crashed again at once)
    }
    MESSAGE(out << " out at the phrase line");
}

TEST_CASE("arcade cycles: phrase lines start a new round, the walls bursting into sparks") {
    CyclesConfig cfg;
    CyclesSim sim(cfg, 3);
    play(sim, 0, 63.9);
    CHECK(sim.stats().rounds == 1);
    CHECK(sim.sparks().empty());
    int before = 0;
    for (const auto& c : sim.cycles()) before += int(c.trail.size());
    CHECK(before > 30);
    sim.advance(64.0, 16.0, 0, 0);  // bar 16: a phrase line
    CHECK(sim.stats().rounds == 2);
    CHECK(sim.roundBeat() == 64.0);
    CHECK(int(sim.sparks().size()) >= before);
    for (const auto& c : sim.cycles()) CHECK(c.trail.size() == 2);  // the start cell and the first move
    // Formations are mirror images pair by pair.
    const auto& a = sim.cycles()[0].trail.front();
    const auto& b = sim.cycles()[1].trail.front();
    const bool lr = a.x + b.x == cfg.cols - 1 && a.y == b.y;
    const bool ud = a.x == b.x && a.y + b.y == cfg.rows - 1;
    const bool pt = a.x + b.x == cfg.cols - 1 && a.y + b.y == cfg.rows - 1;
    CHECK((lr || ud || pt));
    play(sim, 64.0, 127.9);
    CHECK(sim.stats().rounds == 2);
    // Rsd 0: never.
    cfg.reset = 0;
    CyclesSim never(cfg, 3);
    play(never, 0, 200);
    CHECK(never.stats().rounds == 1);
}

TEST_CASE("arcade cycles: a drop doubles the speed for a bar from the next beat line") {
    CyclesConfig cfg;
    CyclesSim sim(cfg, 5);
    std::map<long, int> perBeat;
    sim.onTick = [&](const CyclesSim&, double t, bool) { ++perBeat[long(std::floor(t + 1e-9))]; };
    play(sim, 0, 10.3);
    sim.advance(10.32, 10.32 / 4, 1.0, 0);  // SYNC_DROP rises
    CHECK(sim.dropBeat() == doctest::Approx(10.32));
    for (double b = 10.34; b < 20; b += 2.0 / 60.0) sim.advance(b, b / 4, std::exp(-(b - 10.32) / 4), 0);
    CHECK(perBeat[10] == cfg.spd);
    for (long b = 11; b <= 14; ++b) CHECK(perBeat[b] == 2 * cfg.spd);
    CHECK(perBeat[15] == cfg.spd);
    CHECK(sim.stats().boostBeats == 4);
    CHECK(sim.stats().offLineTurns == 0);
}

TEST_CASE("arcade cycles: draws, finite, deterministic, bounded; the walls flash on a drop") {
    const std::string text = "Kind=\"cycles\"\nGrid=\".15\"\n";
    arcade_test::Run a(text), b(text);
    std::size_t most = 0;
    double total = 0, worst = 0;
    int frames = 0;
    for (double beat = 0; beat < 300; beat += 2.0 / 60.0) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto sa = a.at(beat);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total += ms;
        worst = std::max(worst, ms);
        ++frames;
        const auto sb = b.at(beat);
        REQUIRE(sa.size() == sb.size());
        for (std::size_t k = 0; k < sa.size(); ++k) {
            CHECK(sa[k].ax == sb[k].ax);
            CHECK(sa[k].by == sb[k].by);
            CHECK(sa[k].penA == sb[k].penA);
        }
        CHECK(arcade_test::allFinite(sa));
        most = std::max(most, sa.size());
    }
    MESSAGE("cycles: at most " << most << " segments per frame, " << total / frames << " ms per step (worst " << worst
                                << " ms)");
    CHECK(most < 8000);
    CHECK(most > 50);
    PERF_BUDGET(total / frames < 0.5);

    // The swarm's settings (8 cycles, 64 x 36, turns on 8ths): still cheap.
    {
        arcade_test::Run sw("Kind=\"cycles\"\nNum=\"8\"\nCols=\"64\"\nRows=\"36\"\nTurn=\"2\"\nTail=\"2\"\n");
        double sum = 0, top = 0;
        std::size_t segs = 0;
        int n = 0;
        for (double beat = 0; beat < 300; beat += 2.0 / 60.0, ++n) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto s = sw.at(beat, 120, 0, std::fmod(beat, 64) > 40 && std::fmod(beat, 64) < 56 ? 1 : 0);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            sum += ms;
            top = std::max(top, ms);
            segs = std::max(segs, s.size());
        }
        MESSAGE("swarm: at most " << segs << " segments per frame, " << sum / n << " ms per step (worst " << top
                                  << " ms)");
        CHECK(segs < 8000);
        CHECK(sum / n < 0.5);
    }

    // The same beat, with and without a drop: the walls are brighter.
    arcade_test::Run c(text), d(text);
    c.play(0, 30);
    d.play(0, 30);
    const auto plain = c.at(30.05);
    const auto flash = d.at(30.05, 120, 1.0);
    REQUIRE(plain.size() == flash.size());
    double sp = 0, sf = 0;
    for (std::size_t k = 0; k < plain.size(); ++k) {
        sp += plain[k].penA;
        sf += flash[k].penA;
    }
    CHECK(sf > sp + 3.0);  // the walls and the border (most segments are the heads' fills)
}

TEST_CASE("arcade cycles: seeds, no beat clock, big jumps, junk keys") {
    // Another seed, another game.
    const auto cfgText = arcade_test::compile("Kind=\"cycles\"\n");
    const auto runWith = [&](std::uint64_t seed) {
        reforce::scene::Environment env;
        env.seed = seed;
        reforce::engine::OrganicRun r(cfgText, env, "cycles", 0.0, 0.0, 600.0, nullptr);
        SpriteAudio au;
        au.sync.on = 1;
        std::vector<reforce::engine::LineSegment> segs;
        for (double t = 0; t < 20; t += 1.0 / 30) {
            au.sync.beat = 2 * t;
            au.sync.bar = t / 2;
            segs.clear();
            r.step(t, au, reforce::engine::organicMapping(1920, 1080), reforce::engine::LineStyle{}, segs);
        }
        return segs;
    };
    const auto s1 = runWith(1), s1b = runWith(1), s2 = runWith(2);
    REQUIRE(s1.size() == s1b.size());
    for (std::size_t k = 0; k < s1.size(); ++k) CHECK(s1[k].ax == s1b[k].ax);
    bool differ = s1.size() != s2.size();
    for (std::size_t k = 0; !differ && k < s1.size(); ++k) differ = s1[k].ax != s2[k].ax || s1[k].ay != s2[k].ay;
    CHECK(differ);

    // No beat clock: 120 BPM from the start, still moving.
    {
        arcade_test::Run r("Kind=\"cycles\"\n");
        SpriteAudio au;  // sync.on = 0
        std::vector<reforce::engine::LineSegment> early, late;
        r.run->step(1.0, au, r.map, reforce::engine::LineStyle{}, early);
        for (double t = 1.0; t < 6.0; t += 1.0 / 60) {
            late.clear();
            r.run->step(t, au, r.map, reforce::engine::LineStyle{}, late);
        }
        REQUIRE(!early.empty());
        REQUIRE(!late.empty());
        CHECK((late.size() != early.size() || late.back().ax != early.back().ax));  // moving
        CHECK(arcade_test::allFinite(late));
    }

    // Big jumps: a few beats replay exactly; hundreds start a fresh round without replaying them.
    {
        CyclesConfig cfg;
        CyclesSim smooth(cfg, 9), jump(cfg, 9);
        play(smooth, 0, 9.9);
        smooth.advance(10, 2.5, 0, 0);
        play(jump, 0, 4);
        jump.advance(10, 2.5, 0, 0);
        REQUIRE(smooth.cycles().size() == jump.cycles().size());
        for (std::size_t i = 0; i < smooth.cycles().size(); ++i) {
            CHECK(smooth.cycles()[i].trail.size() == jump.cycles()[i].trail.size());
            CHECK(smooth.cycles()[i].trail.back().x == jump.cycles()[i].trail.back().x);
            CHECK(smooth.cycles()[i].trail.back().y == jump.cycles()[i].trail.back().y);
        }
        const long ticks = jump.stats().ticks;
        jump.advance(5000.5, 1250.125, 0, 0);
        CHECK(jump.stats().ticks - ticks <= 2 * CyclesSim::kMaxRate);
        CHECK(jump.stats().skips == 1);
        CHECK(jump.roundBeat() == 5000.0);
        jump.advance(3.0, 0.75, 0, 0);  // the clock jumps back
        CHECK(jump.stats().skips == 2);
        CHECK(jump.roundBeat() == 3.0);
        jump.advance(NAN, NAN, NAN, NAN);
        jump.advance(1e300, 1e300, 0, 0);
        CHECK(jump.roundBeat() == 3.0);
    }

    // Junk keys stay in range and finite.
    for (const char* junk : {"Num=\"1e9\" Cols=\"-3\" Rows=\"1e7\" Spd=\"-1\" Turn=\"3\" Tail=\"0\" Wdth=\"1e6\" Grid=\"5\"",
                             "Num=\"0\" Cols=\"1e9\" Rows=\"2\" Spd=\"1e9\" Turn=\"-4\" Tail=\"1e9\" Rsd=\"-1\" Mirr=\"7\"",
                             "Num=\"8\" Cols=\"8\" Rows=\"6\" Spd=\"8\" Tail=\"64\" Scal=\"1e9\" Rot=\"1e30\" X=\"1e20\""}) {
        CAPTURE(junk);
        arcade_test::Run r(std::string("Kind=\"cycles\"\n") + junk + "\n");
        std::size_t most = 0;
        for (double beat = 0; beat < 80; beat += 0.37) {
            const auto segs = r.at(beat, 120, std::fmod(beat, 7) < 0.4 ? 1 : 0, std::fmod(beat, 20) < 8 ? 1 : 0);
            CHECK(arcade_test::allFinite(segs));
            most = std::max(most, segs.size());
        }
        CHECK(most < 20000);
    }
}
