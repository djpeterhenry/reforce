// Arcade organic `chase` (PLAN 5Q.8, docs/arcade-design.md "Chase"): the projection, the director (whom the camera
// follows, when it changes), the painter's order, determinism, the fallback clock, junk, bounds and cost.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <vector>

#include "PerfBudget.hpp"
#include "arcade_harness.hpp"
#include "engine/arcade/Chase.hpp"

using namespace reforce::engine;
using reforce::engine::arcade::ChaseCamera;
using reforce::engine::arcade::ChaseConfig;
using reforce::engine::arcade::ChaseView;
using reforce::engine::arcade::V3;

namespace {

// A ChaseView drawn into a 1920 x 1080 frame, stepped at 60 fps of 120 BPM (bar = beat / 4).
struct View {
    ChaseView v;
    FrameMapping map = organicMapping(1920, 1080);
    LineStyle style;
    std::vector<LineSegment> segs;
    explicit View(const ChaseConfig& cfg, std::uint64_t seed = 1) : v(cfg, seed) {}
    const std::vector<LineSegment>& at(double beat, double drop = 0, double build = 0) {
        segs.clear();
        OrganicCanvas c(map, style, segs, 0, 0, 1, 0);
        v.step(beat, beat / 4.0, drop, build, 0, 1, c);
        return segs;
    }
};

double heading(const ChaseView& v, int i, double beat, V3& p) {
    double x, y, a;
    v.sim().head(i, beat, x, y, a);
    p = {x, 0, y};
    return a;
}

}  // namespace

TEST_CASE("arcade chase: draws, finite, on the beat clock") {
    arcade_test::Run r("Kind=\"chase\"\n");
    const auto segs = r.play(0, 8);
    CHECK(!segs.empty());
    CHECK(arcade_test::allFinite(segs));
}

TEST_CASE("arcade chase: projection, near-plane and frame clipping") {
    const ChaseCamera cam = ChaseCamera::lookAt({0, 2, -5}, {0, 0, 5}, 60, 0);
    double x, y;
    // Straight ahead: the middle of the frame; points in view project inside it, to the right / above as expected.
    REQUIRE(cam.project({0, 0, 5}, x, y));
    CHECK(std::abs(x) < 1e-9);
    CHECK(std::abs(y) < 1e-9);
    REQUIRE(cam.project({2, 0, 5}, x, y));
    CHECK(x > 0.1);
    CHECK(x < 1.78);
    REQUIRE(cam.project({0, 3, 5}, x, y));
    CHECK(y > 0.1);
    CHECK(y < 1.0);
    // The field of view: a point on the top edge of a 60 degree view is at y = 1.
    const ChaseCamera flat = ChaseCamera::lookAt({0, 0, 0}, {0, 0, 1}, 60, 0);
    REQUIRE(flat.project({0, std::tan(30 * 3.141592653589793 / 180) * 10, 10}, x, y));
    CHECK(y == doctest::Approx(1.0));
    // Behind the camera: not projected.
    CHECK_FALSE(cam.project({0, 2, -6}, x, y));
    CHECK_FALSE(cam.project({0, 0, -20}, x, y));
    // A line crossing the near plane is cut there; a line wholly behind is dropped.
    V3 a = flat.toView({0, 0, -3}), b = flat.toView({0, 0, 7});
    double ta, tb;
    REQUIRE(flat.clipNear(a, b, ta, tb));
    CHECK(a.z == doctest::Approx(flat.nearZ));
    CHECK(b.z == doctest::Approx(7));
    CHECK(ta == doctest::Approx((3 + flat.nearZ) / 10));
    CHECK(tb == 1);
    V3 c = flat.toView({1, 0, -3}), d = flat.toView({-1, 0, -1});
    CHECK_FALSE(flat.clipNear(c, d, ta, tb));
    // Polygons: a quad straddling the near plane keeps only its part in front.
    const V3 quad[4] = {{-1, -1, -2}, {1, -1, -2}, {1, -1, 4}, {-1, -1, 4}};
    V3 out[8];
    const int n = flat.clipNearPoly(quad, 4, out);
    REQUIRE(n == 4);
    for (int i = 0; i < n; ++i) CHECK(out[i].z >= flat.nearZ - 1e-12);
    // Screen clipping: segments and polygons to the frame rectangle.
    double x0 = -5, y0 = 0, x1 = 5, y1 = 0.5, s0, s1;
    REQUIRE(arcade::clipRect2(x0, y0, x1, y1, 2, 1, s0, s1));
    CHECK(x0 == doctest::Approx(-2));
    CHECK(x1 == doctest::Approx(2));
    CHECK(s0 == doctest::Approx(0.3));
    CHECK(s1 == doctest::Approx(0.7));
    double e0 = 3, f0 = 3, e1 = 4, f1 = 5;
    CHECK_FALSE(arcade::clipRect2(e0, f0, e1, f1, 2, 1, s0, s1));
    const double px[3] = {-10, 10, 0}, py[3] = {-10, -10, 10};
    double ox[32], oy[32];
    const int m = arcade::clipRectPoly(px, py, 3, 2, 1, ox, oy);
    REQUIRE(m >= 3);
    for (int i = 0; i < m; ++i) {
        CHECK(std::abs(ox[i]) <= 2 + 1e-9);
        CHECK(std::abs(oy[i]) <= 1 + 1e-9);
    }
    // Roll turns the screen.
    const ChaseCamera rolled = ChaseCamera::lookAt({0, 0, 0}, {0, 0, 1}, 60, 3.141592653589793 / 2);
    REQUIRE(rolled.project({1, 0, 5}, x, y));
    CHECK(std::abs(x) < 1e-9);
    CHECK(y > 0);
}

TEST_CASE("arcade chase: the chase camera stays behind its cycle; it changes cycle only on Cut lines or after a crash") {
    ChaseConfig cfg;
    cfg.shot = 1;  // always the chase shot
    View w(cfg, 3);
    const double D = cfg.cam;
    int prev = -1, checked = 0, behind = 0, changes = 0;
    double lastChange = 0;
    bool prevAlive = true;
    for (int f = 0; f <= 60 * 2 * 300 / 4; ++f) {  // 150 beats... at 30 frames per beat
        const double beat = f / 30.0;
        w.at(beat);
        const auto& v = w.v;
        const int t = v.target();
        if (prev >= 0 && t != prev) {
            ++changes;
            const double bar = beat / 4.0, prevBar = (f - 1) / 30.0 / 4.0;
            const bool cutLine = std::floor(bar / cfg.cut + 1e-6) != std::floor(prevBar / cfg.cut + 1e-6);
            CAPTURE(beat);
            CHECK((cutLine || !prevAlive || v.reason() == ChaseView::kCrashed));
            lastChange = beat;
        }
        prevAlive = v.sim().cycles()[std::size_t(t)].alive;
        prev = t;
        // Settled chase frames: no crane, no orbit, a beat after a change or a respawn.
        const auto& cy = v.sim().cycles()[std::size_t(t)];
        if (v.crane() > 0 || v.orbiting() || !cy.alive || beat - lastChange < 1.5 || beat - cy.spawnBeat < 1.5 ||
            beat < 2)
            continue;
        V3 p;
        const double a = heading(v, t, beat, p);
        const V3 e = v.camera().eye;
        const double dx = p.x - e.x, dz = p.z - e.z, dist = std::hypot(dx, dz);
        ++checked;
        CHECK(dist > 0.3 * D);
        CHECK(dist < 2.5 * D);
        CHECK(e.y > 0.1);
        CHECK(e.y < 3 * cfg.camH);
        // Behind it: the cycle ahead of the eye, within 30 degrees of its heading most of the time (swinging wide
        // round its turns), never in front of it.
        const double cosA = (dx * std::cos(a) + dz * std::sin(a)) / std::max(dist, 1e-9);
        CHECK(cosA > -0.2);
        if (cosA > std::cos(30 * 3.141592653589793 / 180)) ++behind;
        // The camera looks at the cycle: it projects inside the frame.
        double sx, sy;
        CHECK(v.camera().project(p, sx, sy));
        CHECK(std::abs(sx) < 1.78);
        CHECK(std::abs(sy) < 1.0);
    }
    MESSAGE("chase: " << checked << " settled frames, " << behind << " within 30 degrees behind; " << changes
                      << " target changes");
    CHECK(checked > 1000);
    CHECK(behind > 0.7 * checked);
    CHECK(changes >= 3);
}

TEST_CASE("arcade chase: the director: shots change on cut lines, a crane on phrase lines, a drop shot, a crash orbit") {
    ChaseConfig cfg;
    cfg.game.num = 8;  // crowded: crashes
    View w(cfg, 5);
    int prevShot = -1;
    bool sawCrane = false, sawDrop = false, sawOrbit = false, sawCrashCut = false;
    int shots[ChaseView::kShots] = {};
    for (int f = 0; f <= 30 * 400; ++f) {
        const double beat = f / 30.0;
        const double drop = std::fmod(beat, 64.0) >= 40 && std::fmod(beat, 64.0) < 40.5 ? 1.0 : 0.0;
        const auto& segs = w.at(beat, drop);
        REQUIRE(arcade_test::allFinite(segs));
        const auto& v = w.v;
        if (prevShot >= 0 && v.shot() != prevShot) {
            const double bar = beat / 4.0, prevBar = (f - 1) / 30.0 / 4.0;
            CHECK(std::floor(bar / cfg.cut + 1e-6) != std::floor(prevBar / cfg.cut + 1e-6));
        }
        prevShot = v.shot();
        ++shots[v.shot()];
        // The crane: fully up around each phrase line (16 bars = 64 beats), down again 5 beats later.
        const double ph = std::fmod(beat, 64.0);
        if (ph < 1.4 || ph > 63.95) {
            CHECK(v.crane() > 0.9);
            sawCrane = true;
            // High over the arena.
            CHECK(v.camera().eye.y > 10);
        }
        if (ph > 5 && ph < 61.5) CHECK(v.crane() == 0);
        if (ph > 41.5 && ph < 43.5) {
            CHECK((v.dropShot() > 0.9 || v.orbiting()));
            sawDrop = sawDrop || v.dropShot() > 0.9;
        }
        if (v.orbiting()) sawOrbit = true;
        if (v.reason() == ChaseView::kCrashed) sawCrashCut = true;
    }
    CHECK(sawCrane);
    CHECK(sawDrop);
    CHECK(sawOrbit);
    CHECK(sawCrashCut);
    int used = 0;
    for (int s : shots) used += s > 0;
    CHECK(used >= 3);
}

TEST_CASE("arcade chase: painter's order: far to near, each occluder on a later path") {
    View w(ChaseConfig{}, 2);
    long frames = 0;
    for (int f = 0; f <= 30 * 120; ++f) {
        const auto& segs = w.at(f / 30.0);
        const auto& order = w.v.drawOrder();
        bool ok = true;
        for (std::size_t i = 1; i < order.size(); ++i) ok = ok && order[i] <= order[i - 1] + 1e-9;
        CHECK(ok);
        frames += order.size() > 3;
        // Paths only ever start later in the output.
        for (std::size_t i = 1; i < segs.size(); ++i) REQUIRE(segs[i].path >= segs[i - 1].path);
    }
    CHECK(frames > 1000);
    // A nearer wall's path covers a farther one: the paths of occluders increase with nearness. Count paths.
    std::vector<float> paths;
    for (const auto& s : w.segs)
        if (paths.empty() || s.path != paths.back()) paths.push_back(s.path);
    CHECK(paths.size() > 4);
}

TEST_CASE("arcade chase: deterministic per seed") {
    const std::string text = "Kind=\"chase\"\nNum=\"6\"\n";
    arcade_test::Run a(text), b(text);
    for (int f = 0; f < 30 * 80; ++f) {
        const double beat = f / 30.0;
        const auto sa = a.at(beat), sb = b.at(beat);
        REQUIRE(sa.size() == sb.size());
        bool same = true;
        for (std::size_t i = 0; i < sa.size(); ++i)
            same = same && sa[i].ax == sb[i].ax && sa[i].ay == sb[i].ay && sa[i].bx == sb[i].bx &&
                   sa[i].by == sb[i].by && sa[i].penA == sb[i].penA && sa[i].radius == sb[i].radius;
        REQUIRE(same);
    }
    // Another seed: another chase.
    View x(ChaseConfig{}, 1), y(ChaseConfig{}, 99);
    bool differ = false;
    for (int f = 0; f < 30 * 40 && !differ; ++f) {
        const auto sx = x.at(f / 30.0);
        const auto sy = y.at(f / 30.0);
        differ = sx.size() != sy.size();
    }
    CHECK(differ);
}

TEST_CASE("arcade chase: without a beat clock it runs on 120 BPM from the start") {
    auto cfg = arcade_test::compile("Kind=\"chase\"\n");
    reforce::scene::Environment env;
    OrganicRun run(cfg, env, "chase", 0.0, 0.0, 600.0, nullptr);
    SpriteAudio a;
    std::size_t most = 0;
    for (int i = 0; i < 1200; ++i) {
        std::vector<LineSegment> segs;
        run.step(i / 60.0, a, organicMapping(1280, 720), LineStyle{}, segs);
        CHECK(arcade_test::allFinite(segs));
        most = std::max(most, segs.size());
    }
    CHECK(most > 100);
}

TEST_CASE("arcade chase: junk keys and junk beats stay finite and bounded") {
    const char* configs[] = {
        "Kind=\"chase\"\nNum=\"1e9\"\nCam=\"-5\"\nCamH=\"0/0\"\nFOV=\"1e300\"\nWalH=\"-1\"\nFog=\"0\"\nGrid=\"9\"\n"
        "Cut=\"0\"\nWdth=\"1e300\"\nShot=\"7\"\n",
        "Kind=\"chase\"\nNum=\"-4\"\nCols=\"1e300\"\nRows=\"0\"\nSpd=\"1e300\"\nCam=\"1e300\"\nCamH=\"1e300\"\nFOV=\"0\"\n"
        "WalH=\"1e300\"\nFog=\"1e300\"\nCut=\"1e300\"\n",
        "Kind=\"chase\"\nX=\"1e300\"\nScal=\"0/0\"\nRot=\"1e300\"\nPen=\"-1\"\nTurn=\"4\"\nTail=\"0\"\nRsd=\"0\"\n",
    };
    for (const char* text : configs) {
        CAPTURE(text);
        arcade_test::Run r(text);
        for (int i = 0; i < 400; ++i) {
            const auto segs = r.at(i / 30.0, 120, i == 200 ? 1 : 0, i > 250 ? 1 : 0);
            REQUIRE(arcade_test::allFinite(segs));
            CHECK(segs.size() < 8000);
        }
        for (double junk : {std::nan(""), 1e300, -1e300, 1e15, -50.0, 3.0}) {
            const auto segs = r.at(junk);
            CHECK(arcade_test::allFinite(segs));
        }
    }
}

TEST_CASE("arcade chase: segments and step cost stay bounded") {
    struct Case {
        const char* text;
        const char* name;
    } cases[] = {{"Kind=\"chase\"\n", "defaults"},
                 {"Kind=\"chase\"\nNum=\"8\"\nCols=\"64\"\nRows=\"36\"\nSpd=\"3\"\nTurn=\"2\"\nTail=\"3\"\n", "swarm"},
                 {"Kind=\"chase\"\nNum=\"2\"\nCols=\"24\"\nRows=\"14\"\nTail=\"16\"\nWalH=\"2\"\nCam=\"2\"\nCamH=\".5\"\n",
                  "close, tall walls"}};
    for (const auto& k : cases) {
        CAPTURE(k.name);
        arcade_test::Run r(k.text);
        std::size_t most = 0;
        double total = 0, worst = 0;
        int frames = 0;
        for (int i = 0; i <= 30 * 300; ++i) {
            const double beat = i / 30.0;
            const double drop = std::fmod(beat, 48.0) < 0.5 ? 1.0 : 0.0;
            const double build = std::fmod(beat, 96.0) > 70 ? 1.0 : 0.0;
            const auto t0 = std::chrono::steady_clock::now();
            const auto segs = r.at(beat, 120, drop, build);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            total += ms;
            worst = std::max(worst, ms);
            ++frames;
            most = std::max(most, segs.size());
        }
        MESSAGE("chase " << std::string(k.name) << ": most " << most << " segments; step " << total / frames << " ms avg, "
                         << worst << " max");
        CHECK(most < 8000);
        PERF_BUDGET(total / frames < 1.0);
    }
}

TEST_CASE("arcade chase: a big jump in the beat starts afresh, then plays on") {
    View w(ChaseConfig{}, 4);
    for (int f = 0; f < 300; ++f) w.at(f / 30.0);
    for (double b : {5000.0, 5000.5, 12.0, 1e9, 1e9 + 1}) {
        const auto& segs = w.at(b);
        CHECK(arcade_test::allFinite(segs));
        CHECK(!segs.empty());
        CHECK(std::isfinite(w.v.camera().eye.x));
    }
    // Afterwards it plays on normally from where it is.
    std::size_t most = 0;
    for (int f = 1; f < 300; ++f) most = std::max(most, w.at(1e9 + 1 + f / 30.0).size());
    CHECK(most > 100);
}

TEST_CASE("arcade chase: every machine design draws, finite, bounded and deterministic, from every fixed shot") {
    for (int bike = 0; bike <= 2; ++bike) {
        for (int shot = 1; shot <= ChaseView::kShots; ++shot) {
            CAPTURE(bike);
            CAPTURE(shot);
            ChaseConfig cfg;
            cfg.bike = bike;
            cfg.shot = shot;
            cfg.game.reset = 0;
            View a(cfg, 7), b(cfg, 7);
            long most = 0, total = 0;
            std::size_t mostSegs = 0;
            for (int f = 0; f <= 30 * 40; ++f) {
                const double beat = f / 30.0;
                const double drop = std::fmod(beat, 32.0) < 0.3 ? 1.0 : 0.0;
                const auto sa = a.at(beat, drop);
                const auto& sb = b.at(beat, drop);
                REQUIRE(sa.size() == sb.size());
                REQUIRE(arcade_test::allFinite(sa));
                CHECK(a.v.shot() == shot - 1);
                most = std::max(most, a.v.bikeSegments());
                total += a.v.bikeSegments();
                mostSegs = std::max(mostSegs, sa.size());
            }
            MESSAGE("bike " << bike << " shot " << shot << ": machines at most " << most << " segments, frame at most "
                            << mostSegs);
            CHECK(total > 30 * 40 * 20);  // the machines are drawn (and seen) nearly all the time
            CHECK(most < 6000);  // several machines close to the camera (a round's formation): lines, halos, fills
            CHECK(mostSegs < 8000);
        }
    }
    // The wheels roll with the distance ridden, smoothly: no ticking on the beat (the same turn per frame anywhere
    // in the beat), one cell ridden turns them by 1 / radius.
    const double dt = 1.0 / 60;
    for (double b : {0.0, 0.01, 0.33, 0.5, 0.97, 12.25}) {
        CHECK(ChaseView::wheelTurn(b + dt, 2, 0.4) - ChaseView::wheelTurn(b, 2, 0.4) == doctest::Approx(2 * dt / 0.4));
    }
    CHECK(ChaseView::wheelTurn(1, 1, 0.5) == doctest::Approx(2.0));
    CHECK(std::isfinite(ChaseView::wheelTurn(1e300, 8, 0)));
    // Junk designs are clamped.
    arcade_test::Run r("Kind=\"chase\"\nBike=\"1e300\"\nShot=\"-3\"\n");
    CHECK(arcade_test::allFinite(r.play(0, 4)));
}
