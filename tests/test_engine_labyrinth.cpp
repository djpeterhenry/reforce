// The labyrinth solid (docs/3d-design.md "Labyrinth"): the maze (engine/Labyrinth), the timed route, the physics
// (no tunneling, the goal on the run's last beat line, clacks on beats), determinism, junk input, and the content
// scenes' timing.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>

#include "PerfBudget.hpp"
#include "engine/Labyrinth.hpp"
#include "engine/Solids.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

using namespace reforce::engine;
namespace lab = reforce::engine::labyrinth;
namespace fs = std::filesystem;

namespace {

// Distance from the ball's surface to the nearest wall (test-side reference: every walled side as a box).
double wallClearance(const lab::Maze& m, double x, double y) {
    double best = 1e9;
    for (int cell = 0; cell < m.n * m.n; ++cell)
        for (int d = 0; d < 4; ++d) {
            if (!m.wall(cell, d)) continue;
            const bool vertical = d == lab::East || d == lab::West;
            const double bx = m.cellX(cell) + (d == lab::East ? 0.5 : d == lab::West ? -0.5 : 0.0);
            const double by = m.cellY(cell) + (d == lab::North ? 0.5 : d == lab::South ? -0.5 : 0.0);
            const double hx = vertical ? lab::kWall / 2 : 0.5 + lab::kWall / 2;
            const double hy = vertical ? 0.5 + lab::kWall / 2 : lab::kWall / 2;
            const double qx = std::fabs(x - bx) - hx, qy = std::fabs(y - by) - hy;
            const double out = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
            best = std::min(best, out + std::min(std::max(qx, qy), 0.0) - lab::kBall);
        }
    return best;
}

int cellOf(const lab::Maze& m, double x, double y) {
    const int c = std::clamp(int(std::floor(x + 0.5 * m.n)), 0, m.n - 1);
    const int r = std::clamp(int(std::floor(0.5 * m.n - y)), 0, m.n - 1);
    return r * m.n + c;
}

bool connected(const lab::Maze& m, int a, int b) {
    for (int d = 0; d < 4; ++d)
        if (m.neighbor(a, d) == b && !m.wall(a, d)) return true;
    return false;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("labyrinth: mazes are perfect and seeded; the route runs from the start to a far goal") {
    for (int grid = lab::kMinGrid; grid <= lab::kMaxGrid; ++grid)
        for (uint64_t seed = 0; seed < 25; ++seed) {
            const lab::Route r = lab::makeRoute(seed, grid, 32, 3);
            const lab::Maze& m = r.maze;
            REQUIRE(m.n == grid);
            CHECK(m.isPerfect());
            REQUIRE(m.pathLength >= 2);
            CHECK(m.path[0] == m.start);
            CHECK(m.path[std::size_t(m.pathLength - 1)] == m.goal);
            for (int i = 1; i < m.pathLength; ++i)
                CHECK(connected(m, m.path[std::size_t(i - 1)], m.path[std::size_t(i)]));
            // Far apart: at least the grid's side along the route (tiny grids excepted).
            if (grid >= 6) CHECK(m.pathLength > grid);
            // Traps: dead ends off the route, not next to the start or the goal.
            for (int t = 0; t < m.trapCount; ++t) {
                const int c = m.traps[std::size_t(t)];
                int open = 0;
                for (int d = 0; d < 4; ++d) open += !m.wall(c, d);
                CHECK(open == 1);
                for (int i = 0; i < m.pathLength; ++i) CHECK(m.path[std::size_t(i)] != c);
                for (int p : {m.start, m.goal}) CHECK(std::abs(p % grid - c % grid) + std::abs(p / grid - c / grid) > 1);
            }
            // Same seed, same maze.
            const lab::Route again = lab::makeRoute(seed, grid, 32, 3);
            CHECK(again.maze.open == m.open);
            CHECK(again.maze.goal == m.goal);
        }
    CHECK(lab::makeRoute(1, 8, 32, 3).maze.open != lab::makeRoute(2, 8, 32, 3).maze.open);
}

TEST_CASE("labyrinth: the route is cut into straight segments on whole beats, the last ending on the run's end") {
    for (int beats : {8, 16, 32, 64, 128})
        for (int grid : {4, 8, 12})
            for (uint64_t seed = 0; seed < 10; ++seed) {
                const lab::Route r = lab::makeRoute(seed, grid, beats, 3);
                REQUIRE(r.count >= 1);
                const lab::Maze& m = r.maze;
                int prevNext = r.segments[0].start, cells = 0;
                CHECK(prevNext >= r.interlude);
                for (int k = 0; k < r.count; ++k) {
                    const lab::Segment& s = r.segments[std::size_t(k)];
                    CHECK(s.start == prevNext);
                    CHECK(s.end > s.start);
                    CHECK(s.next >= s.end);
                    if (k > 0) CHECK(s.dir % 2 != r.segments[std::size_t(k - 1)].dir % 2);  // a turn every time
                    if (k + 1 < r.count) {
                        CHECK(s.stop == (m.wall(s.to, s.dir) ? lab::Stop::Wall : lab::Stop::Brake));
                    } else {
                        CHECK(s.stop == lab::Stop::Goal);
                        CHECK(s.to == m.goal);
                        CHECK(s.end == beats);
                        CHECK(s.next == beats);
                    }
                    cells += s.cells;
                    prevNext = s.next;
                }
                CHECK(cells == m.pathLength - 1);
            }
}

TEST_CASE("labyrinth: the ball drops into the goal on the run's last beat line and clacks into walls on beats") {
    double worstGoal = 0, worstClack = 0, worstBrake = 0, deepest = 0, wander = 0, wanderMean = 0, rate = 0;
    int runs = 0, clacks = 0, brakes = 0, illegal = 0, offRoute = 0;
    for (int beats : {16, 32, 64})
        for (int grid : {4, 6, 8, 10, 12})
            for (uint64_t seed = 0; seed < 12; ++seed) {
                lab::Player p(beats);
                p.at(seed, grid, beats, 0.0);
                p.planAll();
                const lab::Route& r = p.route();
                ++runs;
                for (int k = 0; k < r.count; ++k) {
                    const lab::Segment& s = r.segments[std::size_t(k)];
                    if (s.stop == lab::Stop::Goal) {
                        worstGoal = std::max(worstGoal, std::fabs(s.event - beats));
                    } else if (s.stop == lab::Stop::Wall) {
                        ++clacks;
                        worstClack = std::max(worstClack, std::fabs(s.event - s.end));
                        // The clack is in the hit list (the flash), at the same time.
                        bool found = false;
                        for (int h = 0; h < p.hitCount(); ++h) found = found || std::fabs(p.hits()[h].time - s.event) < 1e-9;
                        CHECK(found);
                    } else {
                        ++brakes;
                        worstBrake = std::max(worstBrake, s.endError);
                    }
                }
                deepest = std::max(deepest, p.maxPenetration());
                illegal += p.illegalCrossings();
                offRoute += p.offRoute();
                wander = std::max(wander, p.maxWander());
                wanderMean += p.meanWander();
                rate = std::max(rate, p.maxTiltRate());
                // At the line the ball is dropping into the goal hole.
                const lab::Frame& f = p.at(seed, grid, beats, double(beats));
                CHECK(std::hypot(f.ballX - r.maze.cellX(r.maze.goal), f.ballY - r.maze.cellY(r.maze.goal)) <
                      lab::kHole - 0.5 * lab::kBall + 1e-3);
                CHECK(f.ballZ < 0.0f);
            }
    wanderMean /= runs;
    MESSAGE("labyrinth: " << runs << " runs, " << clacks << " clacks, " << brakes << " braked stops; worst goal "
                          << worstGoal << " beats off the line, clack " << worstClack << " off its beat, brake "
                          << worstBrake << " cells off; deepest into a wall " << deepest << "; side drift mean "
                          << wanderMean << " max " << wander << " cells, " << offRoute << " samples off the route; "
                          << "fastest tilt " << rate << " / beat");
    CHECK(worstGoal < 1e-3);
    CHECK(worstClack < 1e-3);
    CHECK(worstBrake < 0.12);  // a braked stop: at rest within this of its cell's center (was .004 with the robot)
    CHECK(deepest < 1e-6);
    CHECK(illegal == 0);
    // The human hand: the ball wanders across its corridor (grazing the side walls at kFree), never off the route.
    CHECK(wanderMean > 0.02);
    CHECK(wanderMean < lab::kFree);
    CHECK(wander >= lab::kFree - 1e-3);
    CHECK(offRoute == 0);
    CHECK(rate <= lab::kTiltRate + 1e-9);
}

TEST_CASE("labyrinth: the ball never passes through a wall, at any speed or tilt") {
    uint64_t s = 12345;
    auto rnd = [&]() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return double(s >> 11) * 0x1.0p-53;
    };
    double worst = 0;
    int illegal = 0, steps = 0;
    for (int trial = 0; trial < 60; ++trial) {
        const int grid = lab::kMinGrid + trial % (lab::kMaxGrid - lab::kMinGrid + 1);
        const lab::Maze m = lab::makeRoute(uint64_t(trial), grid, 32, 0).maze;
        lab::Player::Ball b;
        const int start = int(rnd() * m.n * m.n) % (m.n * m.n);
        b.x = m.cellX(start);
        b.y = m.cellY(start);
        const double speed = std::pow(10.0, -1 + 10 * rnd());  // 0.1 .. 1e9 cells / beat
        const double a = 6.2831853 * rnd();
        b.vx = speed * std::cos(a);
        b.vy = speed * std::sin(a);
        for (int i = 0; i < 1500; ++i) {
            const double tx = (rnd() - 0.5) * 2, ty = (rnd() - 0.5) * 2;  // far beyond the steepest tilt
            if (i % 97 == 0) {  // kicks
                b.vx = (rnd() - 0.5) * 1e6;
                b.vy = (rnd() - 0.5) * 1e6;
            }
            const int before = cellOf(m, b.x, b.y);
            lab::Player::step(m, b, tx, ty, 1.0 / lab::kStepsPerBeat, 0.0);
            ++steps;
            const int after = cellOf(m, b.x, b.y);
            if (before != after && !connected(m, before, after)) ++illegal;
            if (b.z > -lab::kBall) worst = std::min(worst, wallClearance(m, b.x, b.y));
            REQUIRE(std::isfinite(b.x));
            REQUIRE(std::isfinite(b.y));
        }
    }
    MESSAGE("labyrinth tunneling: " << steps << " steps, deepest into a wall " << -worst << ", " << illegal
                                    << " crossings through a wall");
    CHECK(worst > -1e-6);
    CHECK(illegal == 0);
}

TEST_CASE("labyrinth: deterministic, and lazy planning gives the same frames as planning ahead") {
    for (int grid : {5, 8, 11}) {
        lab::Player lazy(32), ahead(32), other(32);
        ahead.at(7, grid, 32, 0.0);
        ahead.planAll();
        uint64_t s = 99;
        double turn = 0;
        for (int i = 0; i < 200; ++i) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            turn = std::fmod(turn + double(s >> 40) / double(1ull << 24) * 3.0, 32.0);
            const lab::Frame a = lazy.at(7, grid, 32, turn);
            const lab::Frame& b = ahead.at(7, grid, 32, turn);
            CHECK(a.ballX == b.ballX);
            CHECK(a.ballY == b.ballY);
            CHECK(a.ballZ == b.ballZ);
            CHECK(a.tiltX == b.tiltX);
            CHECK(a.tiltY == b.tiltY);
            CHECK(a.segment == b.segment);
        }
        // A jump: a few frames catch up (two segments each), then the same frame.
        int frames = 1;
        while (other.at(7, grid, 32, 20.3).catchingUp && frames < 100) ++frames;
        CHECK(frames > 1);
        CHECK(frames <= 1 + other.route().count / 2);
        const lab::Frame& c = other.at(7, grid, 32, 20.3);
        CHECK(!c.catchingUp);
        CHECK(c.ballX == ahead.at(7, grid, 32, 20.3).ballX);
    }
}

TEST_CASE("labyrinth: frames: the interlude, the ball rolling, tilting and flashing on clacks") {
    lab::Player p(32);
    const lab::Frame f0 = p.at(3, 8, 32, 0.0);
    CHECK(f0.sink == doctest::Approx(1.0f));  // the last maze's walls still up
    CHECK(f0.rise == doctest::Approx(0.0f));
    CHECK(f0.previous.n == 8);
    CHECK(f0.ballZ <= 0.0f);  // the last ball, in its goal
    const lab::Frame f1 = p.at(3, 8, 32, 1.0);
    CHECK(f1.rise == doctest::Approx(1.0f));
    CHECK(f1.sink == doctest::Approx(0.0f));
    CHECK(f1.ballZ < 0.0f);  // popping up out of the start
    CHECK(p.at(3, 8, 32, 1.2).ballZ > 0.0f);
    CHECK(p.at(3, 8, 32, 1.99).ballZ == doctest::Approx(0.0f));
    p.planAll();
    const lab::Route r = p.route();
    for (int k = 0; k < r.count; ++k) {
        const lab::Segment& s = r.segments[std::size_t(k)];
        const lab::Frame a = p.at(3, 8, 32, s.start + 0.5 * (s.end - s.start));
        CHECK(std::hypot(a.tiltX, a.tiltY) > 0.005f);  // the board is tilted while the ball rolls
        if (s.stop == lab::Stop::Wall) CHECK(p.at(3, 8, 32, s.end + 0.02).flash > 0.05f);  // just after the clack
        CHECK(std::fabs(p.at(3, 8, 32, s.start).tiltX) <= float(lab::kMaxTilt) + 1e-6f);
    }
}

TEST_CASE("labyrinth: junk input: huge, negative and non-finite numbers stay safe") {
    lab::Player p(32);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    for (double turn : {nan, inf, -inf, -1e300, 1e300, 31.9999999, 0.0})
        for (int grid : {-5, 0, 3, 13, 1000000})
            for (uint64_t seed : {uint64_t(0), ~uint64_t(0), uint64_t(1) << 63}) {
                const lab::Frame& f = p.at(seed, grid, 1 << 30, turn, -3);
                CHECK(std::isfinite(f.ballX));
                CHECK(std::isfinite(f.ballY));
                CHECK(std::isfinite(f.ballZ));
                CHECK(f.maze.n >= lab::kMinGrid);
                CHECK(f.maze.n <= lab::kMaxGrid);
            }
    lab::Player::Ball b;
    b.vx = nan;
    b.x = inf;
    const lab::Maze m = lab::makeRoute(0, 8, 32, 0).maze;
    lab::Player::step(m, b, nan, inf, nan, 0.0);
    lab::Player::step(m, b, nan, inf, 1.0 / 240, 0.0);
    CHECK(std::isfinite(b.x));
    CHECK(std::isfinite(b.vx));
}

TEST_CASE("labyrinth: the content scene drops the ball on every phrase line, the same at any tempo") {
    for (const char* name : {"Tilt Maze"}) {  // (11 x 11, 16 bars; the 8 x 8 one removed 2026-11-23)
        const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package" / (std::string(name) + ".txt");
        REQUIRE(fs::exists(f));
        auto file = reforce::scene::parseSceneText(readAll(f), name);
        auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
        REQUIRE(solidMesh(*cfg) == MeshKind::Labyrinth);
        reforce::scene::Environment env;
        SolidRun slow(cfg, env, name, MeshKind::Labyrinth, 0.0, 0.0, 400.0, nullptr);
        SolidRun fast(cfg, env, name, MeshKind::Labyrinth, 0.0, 0.0, 400.0, nullptr);
        auto at = [&](SolidRun& run, double bpm, double beat) {
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.bpm = bpm;
            a.sync.beat = beat;
            a.sync.bar = beat / 4;
            a.sync.phrase = beat / 64;
            SolidSpec spec = run.evaluate(beat * 60.0 / bpm, a);
            for (int i = 0; i < 100 && spec.labyrinth.catchingUp; ++i) spec = run.evaluate(beat * 60.0 / bpm, a);
            return spec;
        };

        const int runBeats = 64;
        for (int cycle = 0; cycle < 3; ++cycle) {
            const double b0 = double(runBeats) * cycle;
            // At the line the last run's ball is in its goal; the new maze rises.
            const SolidSpec line = at(slow, 90, b0 + runBeats - 1e-4);
            CHECK(line.labyrinth.ballZ < 0.0f);
            CHECK(line.labyrinth.turn > runBeats - 0.01);
            const SolidSpec start = at(slow, 90, b0 + 0.25);
            CHECK(start.labyrinth.turn == doctest::Approx(0.25).epsilon(1e-6));
            CHECK(start.seed == doctest::Approx(double(cycle)));
            CHECK(start.labyrinth.sink > 0.0f);
            // The same frame at any tempo (the physics runs in beats).
            for (double beat = b0 + 2.3; beat < b0 + runBeats; beat += 3.7) {
                const SolidSpec a = at(slow, 90, beat), b = at(fast, 140, beat);
                CHECK(a.labyrinth.ballX == b.labyrinth.ballX);
                CHECK(a.labyrinth.ballY == b.labyrinth.ballY);
                CHECK(a.labyrinth.tiltX == b.labyrinth.tiltX);
            }
        }
    }
}

TEST_CASE("labyrinth: the board moves like a wrist: no jumps in tilt, its rate bounded, no jolt after a clack") {
    for (int grid : {6, 11}) {
        lab::Player p(64);
        p.at(9, grid, 64, 0.0);
        p.planAll();
        const double dt = 1.0 / lab::kStepsPerBeat;
        float px = p.at(9, grid, 64, 2.0).tiltX, py = p.at(9, grid, 64, 2.0).tiltY;
        double fastest = 0;
        for (double t = 2.0 + dt; t <= 64.0; t += dt) {
            const lab::Frame& f = p.at(9, grid, 64, t);
            fastest = std::max(fastest, double(std::max(std::fabs(f.tiltX - px), std::fabs(f.tiltY - py))) / dt);
            px = f.tiltX;
            py = f.tiltY;
        }
        MESSAGE("labyrinth " << grid << " x " << grid << ": fastest tilt change " << fastest << " / beat");
        CHECK(fastest <= lab::kTiltRate * 1.01);
        // Around each clack the tilt stays near what it was: the hand leans in, it does not snap to the steepest.
        const lab::Route& r = p.route();
        for (int k = 0; k < r.count; ++k) {
            const lab::Segment& s = r.segments[std::size_t(k)];
            if (s.stop != lab::Stop::Wall) continue;
            const lab::Frame a = p.at(9, grid, 64, s.event);
            const lab::Frame& b = p.at(9, grid, 64, s.event + 0.15);
            CHECK(std::hypot(b.tiltX - a.tiltX, b.tiltY - a.tiltY) < 0.15 * lab::kTiltRate + 1e-6);
        }
    }
}

TEST_CASE("labyrinth: collisions: glancing hits keep sliding, head-on clacks rebound and tick to rest") {
    const lab::Maze m = lab::makeRoute(3, 8, 32, 0).maze;
    // A cell with a wall on its east side.
    int cell = -1;
    for (int c = 0; c < m.n * m.n && cell < 0; ++c)
        if (m.wall(c, lab::East) && !m.wall(c, lab::North) && !m.wall(c, lab::South)) cell = c;
    if (cell < 0)
        for (int c = 0; c < m.n * m.n && cell < 0; ++c)
            if (m.wall(c, lab::East)) cell = c;
    REQUIRE(cell >= 0);
    const double dt = 1.0 / lab::kStepsPerBeat;
    {  // glancing: 1 cell / beat into the wall, 3 along it
        lab::Player::Ball b;
        b.x = m.cellX(cell) + lab::kFree - 0.01;
        b.y = m.cellY(cell);
        b.vx = 1.0;
        b.vy = 0.5;
        lab::Hit h;
        h.time = -1;
        for (int i = 0; i < 6 && h.time < 0; ++i) lab::Player::step(m, b, 0, 0, dt, 0.0, &h);
        REQUIRE(h.time >= 0);
        CHECK(b.vx == doctest::Approx(-lab::kRestitution).epsilon(0.05));     // the normal speed rebounds
        CHECK(b.vy > 0.5 - lab::kWallFriction * (1 + lab::kRestitution) - 0.01);  // the slide keeps going
        CHECK(b.vy > 0.3);
    }
    {  // head-on under a steady lean into the wall: a clack, then smaller ticks, then at rest against it
        lab::Player::Ball b;
        b.x = m.cellX(cell) - 0.1;
        b.y = m.cellY(cell);
        b.vx = 1.7;
        std::vector<double> hits;
        for (int i = 0; i < 2 * lab::kStepsPerBeat; ++i) {
            lab::Hit h;
            h.time = -1;
            lab::Player::step(m, b, 0.04, 0, dt, i * dt, &h);
            if (h.time >= 0) hits.push_back(h.speed);
        }
        MESSAGE("clack-tick: " << hits.size() << " hits");
        REQUIRE(hits.size() >= 3);
        for (std::size_t i = 1; i < hits.size(); ++i) CHECK(hits[i] < hits[i - 1]);  // each tick smaller
        CHECK(std::fabs(b.vx) < 0.05);                                               // settled
        CHECK(b.x == doctest::Approx(m.cellX(cell) + lab::kFree).epsilon(1e-3));     // against the wall
    }
}

TEST_CASE("labyrinth: played frame by frame the plan never falls behind and gives the frames of planning ahead") {
    for (int grid : {8, 11, 12}) {
        lab::Player played(64), ahead(64);
        ahead.setSegmentsPerFrame(0);
        int behind = 0;
        for (uint64_t seed = 3; seed < 5; ++seed) {  // two runs: the start and a run change
            ahead.at(seed, grid, 64, 0.0);
            ahead.planAll();
            for (int i = 0; i <= 64 * 60; ++i) {  // 60 frames a beat (120 BPM at 120 fps, or 60 BPM at 60)
                const double turn = i / 60.0;
                const lab::Frame a = played.at(seed, grid, 64, turn);
                behind += a.catchingUp;
                const lab::Frame& b = ahead.at(seed, grid, 64, turn);
                if (a.ballX != b.ballX || a.ballY != b.ballY || a.tiltX != b.tiltX || a.ballZ != b.ballZ) {
                    FAIL_CHECK("frame differs at seed " << seed << " beat " << turn);
                    break;
                }
            }
        }
        CHECK(behind == 0);
    }
}

TEST_CASE("labyrinth: no evaluate() of the content scene costs more than a couple of ms, start and run changes included") {
    const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package/Tilt Maze.txt";
    REQUIRE(fs::exists(f));
    auto file = reforce::scene::parseSceneText(readAll(f), "Tilt Maze");
    auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
    reforce::scene::Environment env;
    double worst = 0, worstAt = 0;
    int behind = 0;
    // From the phrase line and from the middle of a phrase (a jump: catches up over a few frames).
    for (double startBeat : {0.0, 37.0}) {
        const auto t0 = std::chrono::steady_clock::now();
        SolidRun run(cfg, env, "Tilt Maze", MeshKind::Labyrinth, startBeat / 2, 0.0, 400.0, nullptr);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > worst) worst = ms, worstAt = -1;
        for (int i = 0; i <= 140 * 60; ++i) {  // 140 beats at 60 frames per beat: two run changes
            const double beat = startBeat + i / 60.0;
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.bpm = 120;
            a.sync.beat = beat;
            a.sync.bar = beat / 4;
            a.sync.phrase = beat / 64;
            const auto t1 = std::chrono::steady_clock::now();
            const SolidSpec s = run.evaluate(beat / 2, a);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            behind += s.labyrinth.catchingUp;
            if (ms > worst) worst = ms, worstAt = beat;
        }
    }
    MESSAGE("labyrinth: the most expensive evaluate() " << worst << " ms (at beat " << worstAt
                                                        << "; -1 = constructing), frames catching up " << behind);
    PERF_BUDGET(worst < 3.0);
    CHECK(behind < 30);  // only after the mid-phrase start
}
