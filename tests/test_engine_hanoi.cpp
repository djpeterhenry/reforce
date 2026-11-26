// The Tower of Hanoi solid (docs/3d-design.md "Tower of Hanoi"): the solve (engine/Hanoi), every move landing on its
// beat line, the run ending solved on its last line, runs chaining across the line, no disc ever passing through a peg
// or another disc, determinism, clock jumps and tempo changes, junk input, the per-move style (a drop) and the content
// scenes.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "engine/Hanoi.hpp"
#include "engine/Solids.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

using namespace reforce::engine;
namespace hn = reforce::engine::hanoi;
namespace fs = std::filesystem;

namespace {

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<hn::Settings> variants() {
    std::vector<hn::Settings> v;
    for (int n = hn::kMinDiscs; n <= hn::kMaxDiscs; ++n)
        for (double each : {1.0, 0.5, 2.0})
            for (int reset : {0, 1}) {
                hn::Settings s;
                s.discs = n;
                s.each = each;
                s.beats = hn::moveCount(n) * each + (n % 2 ? each : 3.0);  // one move slot, or 3 beats, for the reset
                s.reset = reset;
                v.push_back(s);
            }
    return v;
}

double restY(int level) { return hn::kThick * (level + 0.5); }

// Every disc at rest on a peg, stacked largest first, no gaps (no hover).
void checkStacks(const hn::Frame& f, double tol = 1e-9) {
    for (int p = 0; p < hn::kPegs; ++p) {
        std::vector<int> on;
        for (int d = 0; d < f.discs; ++d)
            if (f.disc[std::size_t(d)].peg == p) on.push_back(d);
        std::sort(on.begin(), on.end(), [](int a, int b) { return a > b; });  // largest first = bottom
        for (std::size_t l = 0; l < on.size(); ++l) {
            const hn::Disc& o = f.disc[std::size_t(on[l])];
            CHECK(o.x == doctest::Approx(hn::pegX(p)).epsilon(tol));
            CHECK(std::fabs(o.y - restY(int(l))) < 1e-3);  // (a landing's squash moves it a hair)
        }
    }
}

// The mover against the pegs and the resting discs: never inside one (conservative boxes around tilted discs).
int collisions(const hn::Frame& f, double* worst = nullptr) {
    int hits = 0;
    const int n = f.discs;
    const double top = hn::pegHeight(n);
    for (int m = 0; m < n; ++m) {
        const hn::Disc& a = f.disc[std::size_t(m)];
        if (!a.moving) continue;
        const double r = hn::discRadius(m, n), h = hn::kThick / 2 * a.squash;
        const double c = std::fabs(std::cos(a.tilt)), s = std::fabs(std::sin(a.tilt));
        const double hv = r * s + h * c, hh = r * c + h * s;
        for (int p = 0; p < hn::kPegs; ++p) {
            const double dx = std::fabs(a.x - hn::pegX(p));
            if (dx >= hh + hn::kPegR) continue;
            const bool threaded = dx + hn::kPegR <= hn::kHole + 1e-9 && s < 1e-6;
            const double gap = (a.y - hv) - top;
            if (!threaded && gap < -1e-9) {
                ++hits;
                if (worst) *worst = std::min(*worst, gap);
            }
        }
        for (int o = 0; o < n; ++o) {
            const hn::Disc& b = f.disc[std::size_t(o)];
            if (o == m || b.moving) continue;
            if (std::fabs(a.x - b.x) >= hh + hn::discRadius(o, n)) continue;
            const double gap = std::max((a.y - hv) - (b.y + hn::kThick / 2), (b.y - hn::kThick / 2) - (a.y + hv));
            if (gap < -1e-6) {
                ++hits;
                if (worst) *worst = std::min(*worst, gap);
            }
        }
    }
    return hits;
}

}  // namespace

TEST_CASE("hanoi: the plan is the minimal solve, every move legal, ending with the tower on peg 2") {
    for (int n = hn::kMinDiscs; n <= hn::kMaxDiscs; ++n) {
        hn::Settings s;
        s.discs = n;
        const hn::Plan plan(s);
        REQUIRE(plan.moves() == (1 << n) - 1);
        std::vector<std::vector<int>> pegs(3);
        for (int d = n - 1; d >= 0; --d) pegs[0].push_back(d);
        for (int k = 0; k < plan.moves(); ++k) {
            const hn::Move& m = plan.move(k);
            REQUIRE(!pegs[std::size_t(m.from)].empty());
            CHECK(pegs[std::size_t(m.from)].back() == m.disc);  // the top disc
            CHECK(m.fromLevel == int(pegs[std::size_t(m.from)].size()) - 1);
            CHECK(m.toLevel == int(pegs[std::size_t(m.to)].size()));
            if (!pegs[std::size_t(m.to)].empty()) CHECK(pegs[std::size_t(m.to)].back() > m.disc);  // onto a larger one
            CHECK(m.from != m.to);
            pegs[std::size_t(m.from)].pop_back();
            pegs[std::size_t(m.to)].push_back(m.disc);
            // The tables agree with the stacks.
            for (int p = 0; p < 3; ++p)
                for (std::size_t l = 0; l < pegs[std::size_t(p)].size(); ++l) {
                    CHECK(plan.pegAfter(k + 1, pegs[std::size_t(p)][l]) == p);
                    CHECK(plan.levelAfter(k + 1, pegs[std::size_t(p)][l]) == int(l));
                }
            // The smallest disc moves every other time (the minimal solve's signature).
            CHECK((m.disc == 0) == (k % 2 == 0));
        }
        CHECK(pegs[0].empty());
        CHECK(pegs[1].empty());
        CHECK(int(pegs[2].size()) == n);
    }
}

TEST_CASE("hanoi: every move lands exactly on its beat line, the last on the run's end; legal stacks at every line") {
    for (const hn::Settings& s : variants()) {
        const hn::Plan plan(s);
        const hn::Settings& c = plan.settings();
        CHECK(plan.landing(plan.moves()) == doctest::Approx(c.beats).epsilon(1e-12));
        for (double seed : {0.0, 1.0, 2.0, 7.0, -4.0}) {
            const auto run = hn::Plan::runOf(seed);
            for (int k = 1; k <= plan.moves(); ++k) {
                const double line = plan.landing(k);
                const hn::Move& m = plan.move(k - 1);
                const hn::Frame before = plan.frameAt(seed, line - 1e-7);
                const hn::Frame at = plan.frameAt(seed, line);
                const hn::Disc& a = at.disc[std::size_t(m.disc)];
                const hn::Disc& b = before.disc[std::size_t(m.disc)];
                // In flight a hair before (still above its landing place), resting on it exactly on the line.
                CHECK(b.moving);
                CHECK(b.y > restY(m.toLevel));
                CHECK(a.y == restY(m.toLevel));
                const int to = m.to == 0 ? plan.startPeg(run) : m.to == 2 ? plan.targetPeg(run)
                                                                          : 3 - plan.startPeg(run) - plan.targetPeg(run);
                CHECK(a.x == hn::pegX(to));
                CHECK(at.done == k);
                if (k % 7 == 1 || k == plan.moves()) checkStacks(at);
            }
            // The run's end: solved, every disc on the target peg; just before: the last disc still in flight.
            const hn::Frame end = plan.frameAt(seed, c.beats);
            CHECK(end.solved);
            for (int d = 0; d < c.discs; ++d) CHECK(end.disc[std::size_t(d)].peg == plan.targetPeg(run));
            checkStacks(end);
            const hn::Frame late = plan.frameAt(seed, c.beats - 0.01);
            CHECK(!late.solved);
            CHECK(late.move == plan.moves() - 1);
        }
    }
}

TEST_CASE("hanoi: runs chain across the line: the next run starts where the last one ended") {
    for (int reset : {0, 1}) {
        hn::Settings s;
        s.discs = 5;
        s.reset = reset;
        s.beats = 32;
        const hn::Plan plan(s);
        for (int r = -7; r < 12; ++r) {
            CHECK(plan.targetPeg(r) != plan.startPeg(r));
            const hn::Frame end = plan.frameAt(r, plan.settings().beats);
            const hn::Frame next = plan.frameAt(r + 1, 0.0);
            for (int d = 0; d < 5; ++d) {
                CHECK(next.disc[std::size_t(d)].x == doctest::Approx(end.disc[std::size_t(d)].x));
                CHECK(next.disc[std::size_t(d)].y == doctest::Approx(end.disc[std::size_t(d)].y));
            }
            if (reset == 0) CHECK(plan.startPeg(r + 1) == plan.targetPeg(r));
            else CHECK(plan.startPeg(r) == 0);
            // The reset ends on its line: every disc resting on the start peg.
            const hn::Frame ready = plan.frameAt(r + 1, plan.resetBeats());
            for (int d = 0; d < 5; ++d) CHECK(ready.disc[std::size_t(d)].peg == plan.startPeg(r + 1));
            checkStacks(ready);
        }
    }
    // Reset 0 walks the tower round the pegs: 0 -> 2 -> 1 -> 0.
    hn::Settings s;
    const hn::Plan plan(s);
    CHECK(plan.startPeg(0) == 0);
    CHECK(plan.targetPeg(0) == 2);
    CHECK(plan.targetPeg(1) == 1);
    CHECK(plan.targetPeg(2) == 0);
}

TEST_CASE("hanoi: motion is continuous and no disc ever passes through a peg or another disc") {
    {
        // (The check itself: a disc carried low beside a peg, or into a resting disc, is caught.)
        hn::Settings s;
        hn::Frame f = hn::Plan(s).frameAt(0, 0.0);
        hn::Disc& d = f.disc[0];
        d.moving = true;
        d.x = hn::pegX(1) + 0.4;
        d.y = 1.0;
        CHECK(collisions(f) == 1);
        d.x = hn::pegX(0) + 0.3;
        d.y = hn::kThick * 2;
        CHECK(collisions(f) > 0);
    }
    hn::Style plain, drop = hn::makeStyle(2.5, 1.0), low = hn::makeStyle(0.0, 0.0);
    CHECK(drop.flips == 2);
    for (const hn::Settings& s : variants()) {
        if (s.each != 1.0) continue;
        const hn::Plan plan(s);
        for (const hn::Style& st : {plain, drop, low})
            for (double hover : {0.0, 1.0}) {
                hn::Controls c;
                c.style = st;
                c.hover = hover;
                hn::Frame prev = plan.frameAt(3, 0.0, c);
                double worst = 0, jump = 0;
                int hits = 0;
                const double step = 1.0 / 512;
                for (double t = step; t <= plan.settings().beats; t += step) {
                    const hn::Frame f = plan.frameAt(3, t, c);
                    hits += collisions(f, &worst);
                    for (int d = 0; d < f.discs; ++d) {
                        const hn::Disc& a = f.disc[std::size_t(d)];
                        const hn::Disc& b = prev.disc[std::size_t(d)];
                        jump = std::max(jump, std::hypot(a.x - b.x, a.y - b.y));
                    }
                    prev = f;
                }
                INFO("discs " << s.discs << " reset " << s.reset << " lift " << st.lift << " flips " << st.flips
                              << " hover " << hover);
                CHECK(hits == 0);
                CHECK(worst == 0.0);
                // At most ~6 units a beat at full speed: 1/512 beat moves a disc far less than its thickness.
                CHECK(jump < 0.06);
            }
    }
}

TEST_CASE("hanoi: a drop's style: higher arcs, a full flip tumbling forward, level again on landing") {
    hn::Settings s;
    const hn::Plan plan(s);
    hn::Controls c;
    c.style = hn::makeStyle(2.5, 1.0);
    // Move 32 (the largest disc, 0 -> 2): its arc's top, then its landing.
    const double start = plan.landing(31);
    double highest = 0, highestPlain = 0, maxTilt = 0;
    for (double u = 0; u < 1.0; u += 1.0 / 256) {
        const hn::Frame f = plan.frameAt(0, start + u, c);
        const hn::Frame g = plan.frameAt(0, start + u);
        const hn::Disc& a = f.disc[5];
        highest = std::max(highest, a.y);
        highestPlain = std::max(highestPlain, g.disc[5].y);
        maxTilt = std::max(maxTilt, std::fabs(a.tilt));
    }
    CHECK(highest > highestPlain + 0.5);
    CHECK(maxTilt > 6.0);  // a full turn
    const hn::Frame landed = plan.frameAt(0, plan.landing(32), c);
    CHECK(std::fabs(std::remainder(landed.disc[5].tilt, 2 * 3.141592653589793)) < 1e-9);
    // Lift 0: no arc, still clear of the pegs (covered by the sweep above); flips: 0, 1 (a half turn) or 2.
    CHECK(hn::makeStyle(1, 0.2).flips == 0);
    CHECK(hn::makeStyle(1, 0.5).flips == 1);
    CHECK(hn::makeStyle(1, 1e9).flips == 2);
}

TEST_CASE("hanoi: deterministic and a pure function of the beat: jumps are lookups, frames played in order agree") {
    hn::Settings s;
    s.discs = 7;
    s.beats = 130;
    const hn::Plan a(s), b(s);
    for (double t : {0.0, 0.37, 1.0, 2.5, 17.25, 64.9, 100.0, 129.999, 130.0}) {
        const hn::Frame fa = a.frameAt(5, t), fb = b.frameAt(5, t);
        for (int d = 0; d < 7; ++d) {
            CHECK(fa.disc[std::size_t(d)].x == fb.disc[std::size_t(d)].x);
            CHECK(fa.disc[std::size_t(d)].y == fb.disc[std::size_t(d)].y);
            CHECK(fa.disc[std::size_t(d)].tilt == fb.disc[std::size_t(d)].tilt);
        }
        CHECK(fa.done == fb.done);
    }
}

TEST_CASE("hanoi: junk input: huge, negative and non-finite numbers stay safe") {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    hn::Settings junk;
    junk.discs = 1000;
    junk.beats = nan;
    junk.each = -inf;
    junk.reset = 7;
    const hn::Plan plan(junk);
    CHECK(plan.settings().discs == hn::kMaxDiscs);
    CHECK(std::isfinite(plan.settings().beats));
    CHECK(plan.settings().reset == 0);
    for (double seed : {nan, inf, -inf, 1e300, -1e300, -3.0})
        for (double turn : {nan, inf, -inf, 1e300, -5.0, 3.5}) {
            hn::Controls c;
            c.hover = nan;
            c.style.lift = inf;
            c.style.flips = 99;
            const hn::Frame f = plan.frameAt(seed, turn, c);
            for (int d = 0; d < f.discs; ++d) {
                const hn::Disc& o = f.disc[std::size_t(d)];
                CHECK(std::isfinite(o.x));
                CHECK(std::isfinite(o.y));
                CHECK(std::isfinite(o.tilt));
                CHECK(std::isfinite(o.squash));
            }
            CHECK(f.start >= 0);
            CHECK(f.start < 3);
        }
    CHECK(plan.moveAt(nan) == -2);
    hn::Settings few;
    few.discs = -5;
    CHECK(hn::Plan(few).moves() == 7);
}

TEST_CASE("hanoi: the content scenes land a move on every beat and stand solved on their lines, at any tempo") {
    struct Case { const char* name; int discs; double run; };
    for (const Case& cs : {Case{"Hanoi Tower", 6, 64.0}}) {
        const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package" / (std::string(cs.name) + ".txt");
        REQUIRE(fs::exists(f));
        auto file = reforce::scene::parseSceneText(readAll(f), cs.name);
        auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
        REQUIRE(solidMesh(*cfg) == MeshKind::Hanoi);
        reforce::scene::Environment env;
        SolidRun slow(cfg, env, cs.name, MeshKind::Hanoi, 0.0, 0.0, 400.0, nullptr);
        SolidRun fast(cfg, env, cs.name, MeshKind::Hanoi, 0.0, 0.0, 400.0, nullptr);
        REQUIRE(slow.hanoi());
        CHECK(slow.hanoi()->settings().discs == cs.discs);
        CHECK(slow.hanoi()->settings().beats == cs.run);
        auto at = [&](SolidRun& run, double bpm, double beat, double drop = 0) {
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.bpm = bpm;
            a.sync.beat = beat;
            a.sync.bar = beat / 4;
            a.sync.phrase = beat / 64;
            a.sync.drop = drop;
            return run.evaluate(beat * 60.0 / bpm, a);
        };
        const int moves = slow.hanoi()->moves();
        for (int cycle = 0; cycle < 3; ++cycle) {
            const double b0 = cs.run * cycle;
            // Just before the line the last move is landing; past it the new run (Seed) starts.
            const SolidSpec end = at(slow, 120, b0 + cs.run - 1e-6);
            CHECK(end.hanoi.done == moves - 1);
            const SolidSpec line = at(slow, 120, b0 + cs.run + 0.25);
            CHECK(line.seed == doctest::Approx(double(cycle + 1)));
            CHECK(line.hanoi.resetting);
            // Each beat line 2..run lands one more move.
            for (int k = 2; k <= int(cs.run); k += 3) CHECK(at(slow, 120, b0 + k).hanoi.done == k - 1);
            // The same frame at any tempo.
            for (double beat = b0 + 0.3; beat < b0 + cs.run; beat += 2.71) {
                const SolidSpec a = at(slow, 90, beat), b = at(fast, 140, beat);
                for (int d = 0; d < cs.discs; ++d) {
                    CHECK(a.hanoi.disc[std::size_t(d)].x == b.hanoi.disc[std::size_t(d)].x);
                    CHECK(a.hanoi.disc[std::size_t(d)].y == b.hanoi.disc[std::size_t(d)].y);
                }
            }
        }
        // A drop does nothing special (Peter 2026-11-26): no jump mid-move, no flip on the next move.
        SolidRun run(cfg, env, cs.name, MeshKind::Hanoi, 0.0, 0.0, 400.0, nullptr);
        const double mid = 3.0;  // beat 2..3: move 2 in flight
        const SolidSpec s0 = at(run, 120, mid + 0.5);
        const SolidSpec s1 = at(run, 120, mid + 0.51, 1.0);
        const int d = s0.hanoi.move >= 0 ? run.hanoi()->move(s0.hanoi.move).disc : 0;
        CHECK(std::hypot(s1.hanoi.disc[std::size_t(d)].x - s0.hanoi.disc[std::size_t(d)].x,
                         s1.hanoi.disc[std::size_t(d)].y - s0.hanoi.disc[std::size_t(d)].y) < 0.1);
        double tilt = 0;
        for (double u = 0; u < 1; u += 1.0 / 64) {
            const SolidSpec sp = at(run, 120, mid + 1 + u, 1.0);
            for (int k = 0; k < cs.discs; ++k) tilt = std::max(tilt, std::fabs(sp.hanoi.disc[std::size_t(k)].tilt));
        }
        CHECK(tilt < 3.0);
    }
}

TEST_CASE("hanoi: started mid-run or jumping about, a frame is the frame of playing it in order") {
    const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package/Hanoi Tower.txt";
    auto file = reforce::scene::parseSceneText(readAll(f), "Hanoi Tower");
    auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
    reforce::scene::Environment env;
    SolidRun ordered(cfg, env, "Hanoi Tower", MeshKind::Hanoi, 0.0, 0.0, 400.0, nullptr);
    SolidRun jumpy(cfg, env, "Hanoi Tower", MeshKind::Hanoi, 0.0, 0.0, 400.0, nullptr);
    auto at = [&](SolidRun& run, double beat) {
        SpriteAudio a;
        a.sync.on = 1;
        a.sync.bpm = 124;
        a.sync.beat = beat;
        a.sync.bar = beat / 4;
        a.sync.phrase = beat / 64;
        return run.evaluate(beat * 60.0 / 124, a);
    };
    std::vector<SolidSpec> seq;
    for (int i = 0; i < 160 * 8; ++i) seq.push_back(at(ordered, i / 8.0));
    for (int i : {1100, 3, 777, 512, 513, 64, 1279, 0, 900}) {
        const SolidSpec j = at(jumpy, i / 8.0);
        for (int d = 0; d < 6; ++d) {
            CHECK(j.hanoi.disc[std::size_t(d)].x == seq[std::size_t(i)].hanoi.disc[std::size_t(d)].x);
            CHECK(j.hanoi.disc[std::size_t(d)].y == seq[std::size_t(i)].hanoi.disc[std::size_t(d)].y);
        }
    }
}
