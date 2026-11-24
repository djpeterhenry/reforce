// Fractal zoom (PLAN 5L.14, docs/3d-design.md "Fractal zoom"): engine/FractalZoom, pure C++.
//   - every path's step g maps the fractal onto one of its pieces; p is fixed; S_inf = S within rho of p
//   - the distance estimate (the shader's twin) is exactly g-equivariant, so a pose and g o pose see the same
//   - the camera at the end of a step is g o the camera at its start (the seam), for any sway
//   - the camera never enters the fractal along a whole step, with the default sway
//   - the clock: steps land on ZBar bar lines; drops, builds; deterministic; junk and huge inputs stay bounded
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <random>
#include <string>

#include "engine/FractalZoom.hpp"
#include "engine/ObjectBeat.hpp"
#include "engine/Solids.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

using namespace reforce::engine;

namespace {

double dist(const FVec3& a, const FVec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

FVec3 randomNear(std::mt19937_64& rng, const FVec3& c, double r) {
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (;;) {
        const FVec3 v{u(rng), u(rng), u(rng)};
        if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] <= 1.0) return {c[0] + r * v[0], c[1] + r * v[1], c[2] + r * v[2]};
    }
}

std::string label(const FractalPath& p) { return std::string(fractalKindName(p.kind)) + " " + p.name; }

std::shared_ptr<const reforce::scene::CompiledConfig> compileSolid(const std::string& text) {
    auto file = reforce::scene::parseSceneText(text, "test");
    return std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
}

std::string readFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f);
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);
    return text;
}

}  // namespace

TEST_CASE("fractals: every path's step maps the fractal onto a piece of itself, about a fixed point") {
    REQUIRE(fractalPathCount() >= 4);
    for (int i = 0; i < fractalPathCount(); ++i) {
        const FractalPath& p = fractalPath(i);
        CAPTURE(label(p));
        const FractalMap& g = p.g;
        CHECK(dist(fractalApply(g, 1.0, g.p), g.p) < 1e-12);
        CHECK(fractalHullDistance(p.kind, g.p) <= 1e-9);  // p lies in the fractal's hull
        CHECK(g.rho > 0.1);
        // g's image of the fractal at a deep level lies in the fractal: points of S map to points of S.
        std::mt19937_64 rng(7 + i);
        int inside = 0;
        for (int k = 0; k < 4000; ++k) {
            const FVec3 x = randomNear(rng, {0, 0, 0}, 1.8);
            if (fractalDistanceS(p.kind, x, 3.0) > -1e-9) continue;  // in the level-3 prefractal
            ++inside;
            const int stepLevels = int(std::lround(std::log(g.lambda) / std::log(fractalPieceScale(p.kind))));
            CHECK(fractalDistanceS(p.kind, fractalApply(g, 1.0, x), 3.0 + stepLevels) < 1e-9);
        }
        CHECK(inside > 50);
        // S_inf = S within rho of p: a point there is in S at a level exactly when it is in g^-1(S) one step finer.
        int checked = 0, mismatch = 0;
        for (int k = 0; k < 4000; ++k) {
            const FVec3 y = randomNear(rng, g.p, 0.98 * g.rho);
            const double inS = fractalDistanceS(p.kind, y, 5.0);
            const int stepLevels = int(std::lround(std::log(g.lambda) / std::log(fractalPieceScale(p.kind))));
            const double inBig = fractalDistanceS(p.kind, fractalApply(g, 1.0, y), 5.0 + stepLevels);
            if (std::fabs(inS) < 1e-6 || std::fabs(inBig) < 1e-6) continue;
            ++checked;
            mismatch += (inS < 0) != (inBig < 0);
        }
        CHECK(checked > 3000);
        CHECK(mismatch == 0);
    }
}

TEST_CASE("fractals: the distance estimate is exactly invariant under the zoom step") {
    for (int i = 0; i < fractalPathCount(); ++i) {
        const FractalPath& p = fractalPath(i);
        CAPTURE(label(p));
        const FractalMap& g = p.g;
        const FractalCamera c = fractalCamera(p, 0.3);
        const FVec3 eye2 = fractalApply(g, 1.0, c.eye);
        const double detail = 0.002;
        std::mt19937_64 rng(11 + i);
        double worst = 0;
        for (int k = 0; k < 3000; ++k) {
            const FVec3 x = randomNear(rng, g.p, 4.0 * c.scale * (k % 3 == 0 ? 6.0 : 1.0));
            const double d1 = fractalDistanceInf(p.kind, g, x, c.eye, detail, c.scale);
            const double d2 = fractalDistanceInf(p.kind, g, fractalApply(g, 1.0, x), eye2, detail, g.lambda * c.scale);
            worst = std::max(worst, std::fabs(d2 / g.lambda - d1) / std::max(std::fabs(d1), 1e-3 * c.scale));
        }
        CHECK(worst < 1e-6);
    }
}

TEST_CASE("fractals: rays from the camera at the end of a step hit where they hit at its start (CPU raymarch)") {
    for (int i = 0; i < fractalPathCount(); ++i) {
        const FractalPath& p = fractalPath(i);
        CAPTURE(label(p));
        FractalSway sw;
        sw.orbit = 0.07;
        sw.lookX = 0.03;
        const FractalCamera a = fractalCamera(p, 0.0, sw), b = fractalCamera(p, 1.0, sw);
        const double tanHalf = std::tan(a.fov * M_PI / 360.0), detail = 2.0 * tanHalf / 180.0;
        auto march = [&](const FractalCamera& c, double nx, double ny) {
            FVec3 dir;
            for (int k = 0; k < 3; ++k)
                dir[std::size_t(k)] = c.forward[std::size_t(k)] + nx * tanHalf * 16.0 / 9.0 * c.right[std::size_t(k)] +
                                      ny * tanHalf * c.up[std::size_t(k)];
            const double l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            for (double& v : dir) v /= l;
            double t = 0;
            for (int s = 0; s < 300; ++s) {
                const FVec3 x{c.eye[0] + dir[0] * t, c.eye[1] + dir[1] * t, c.eye[2] + dir[2] * t};
                const double d = fractalDistanceInf(p.kind, p.g, x, c.eye, detail, c.scale);
                if (d < 0.6 * detail * std::max(t, 1e-3 * c.scale)) return t / c.scale;
                t += 0.95 * d;
                if (t > 12.0 * c.scale) break;
            }
            return -1.0;
        };
        int hits = 0;
        double worst = 0;
        for (int k = 0; k < 64; ++k) {
            const double nx = -0.95 + 1.9 * (k % 8) / 7.0, ny = -0.9 + 1.8 * (k / 8) / 7.0;
            const double ta = march(a, nx, ny), tb = march(b, nx, ny);
            CHECK((ta < 0) == (tb < 0));
            if (ta < 0 || tb < 0) continue;
            ++hits;
            worst = std::max(worst, std::fabs(ta - tb) / ta);
        }
        MESSAGE(label(p) << ": " << hits << " of 64 rays hit; worst relative hit distance difference " << worst);
        CHECK(hits > 8);
        CHECK(worst < 1e-6);
    }
}

TEST_CASE("fractals: the camera at the end of a step is the zoom step applied to the camera at its start") {
    const FractalSway sways[] = {{}, {0.4, 0.1, -0.05, 0.3, 0}, {-2.0, -0.2, 0.15, -1.0, 75}};
    double worst = 0;
    for (int i = 0; i < fractalPathCount(); ++i) {
        const FractalPath& p = fractalPath(i);
        CAPTURE(label(p));
        for (const auto& sw : sways) {
            const FractalCamera a = fractalCamera(p, 0.0, sw), b = fractalCamera(p, 1.0, sw);
            const FVec3 ge = fractalApply(p.g, 1.0, a.eye);
            worst = std::max(worst, dist(ge, b.eye) / a.scale);
            worst = std::max(worst, dist(fractalRotate(p.g, 1.0, a.forward), b.forward));
            worst = std::max(worst, dist(fractalRotate(p.g, 1.0, a.up), b.up));
            worst = std::max(worst, dist(fractalRotate(p.g, 1.0, a.right), b.right));
            worst = std::max(worst, std::fabs(b.scale / a.scale - p.g.lambda));
            CHECK(a.fov == b.fov);
            // Continuous within the step: a thousandth of a step moves the camera a little.
            const FractalCamera c = fractalCamera(p, 0.001, sw);
            CHECK(dist(c.eye, a.eye) < 0.01 * a.scale);
        }
    }
    MESSAGE("camera seam error (relative): " << worst);
    CHECK(worst < 1e-12);
}

TEST_CASE("fractals: the camera never enters the fractal along a step, with the default sway") {
    for (int i = 0; i < fractalPathCount(); ++i) {
        const FractalPath& p = fractalPath(i);
        CAPTURE(label(p));
        double worst = 1e9;
        for (int k = 0; k < 400; ++k) {
            const double u = k / 400.0;
            for (double o : {-1.0, 0.0, 1.0}) {
                FractalSway sw;
                sw.orbit = o * p.orbit;
                const FractalCamera c = fractalCamera(p, u, sw);
                const double d = fractalDistanceInf(p.kind, p.g, c.eye, c.eye, 0.001, c.scale);
                worst = std::min(worst, d / c.scale);
            }
        }
        MESSAGE(label(p) << ": closest approach " << worst << " x the distance to p");
        CHECK(worst > 0.04);
    }
}

TEST_CASE("fractals: names") {
    CHECK(parseFractalKind("Sierpinski") == FractalKind::Sierpinski);
    CHECK(parseFractalKind(" menger ") == FractalKind::Menger);
    CHECK(parseFractalKind("") == FractalKind::Sierpinski);
    CHECK_FALSE(parseFractalKind("mandelbulb"));
    CHECK(findFractalPath(FractalKind::Sierpinski, "") != nullptr);
    CHECK(findFractalPath(FractalKind::Menger, "Vortex") != nullptr);
    CHECK(findFractalPath(FractalKind::Menger, "apex") == nullptr);
    CHECK(fractalPathNames(FractalKind::Sierpinski) == "apex, dive, vortex");
}

TEST_CASE("fractals: the zoom steps land on bar lines") {
    for (double per : {0.5, 1.0, 2.0, 4.0, 8.0}) {
        for (double pulse : {0.0, 0.5, 1.0}) {
            FractalTiming t;
            t.barsPerStep = per;
            t.pulse = pulse;
            for (double bar = 0; bar < 64; bar += per) {
                const double z = FractalClock::baseZoom(t, bar);
                CHECK(std::fabs(z - std::round(z)) < 1e-9);
            }
            // Monotonic and continuous (mod 1) between the lines.
            double prev = FractalClock::baseZoom(t, 0.0);
            double total = 0;
            for (int k = 1; k <= 64 * 100; ++k) {
                const double z = FractalClock::baseZoom(t, k / 100.0);
                double d = z - prev;
                if (d < -0.5) d += 1.0;
                CHECK(d >= -1e-12);
                CHECK(d < 0.061);  // at most 3x the mean speed (Puls 1, 2 steps a bar)
                total += d;
                prev = z;
            }
            CHECK(std::fabs(total - 64.0 / per) < 1e-6);  // one step per ZBar bars
        }
    }
    // The clock stepped frame by frame lands on the lines too (no drop, no build).
    FractalClock c;
    for (int f = 0; f <= 60 * 16; ++f) {
        const double beat = f / 30.0;  // 120 BPM at 60 fps
        const FractalBeat b = c.step(beat, beat / 4.0, true, f / 60.0, 0, 0, 1.0 / 60.0);
        CHECK(b.zoom >= 0.0);
        CHECK(b.zoom < 1.0);
        if (f % 240 == 0) CHECK(std::min(b.zoom, 1.0 - b.zoom) < 1e-9);  // every 2 bars
    }
}

TEST_CASE("fractals: a drop rushes through extra steps; a build slows the zoom; both end on whole steps") {
    FractalTiming t;
    t.drop = 2;
    t.dropBeats = 4;
    t.hover = 0.75;
    auto run = [&](bool build, bool drop) {
        FractalClock c(t);
        double travelled = 0, prev = 0;
        FractalBeat b;
        for (int f = 0; f <= 60 * 48; ++f) {  // 24 bars at 120 BPM
            const double beat = f / 30.0, bar = beat / 4.0;
            const double bld = build && bar >= 4 && bar < 8 ? (bar - 4) / 4 : 0.0;
            const double drp = drop && bar >= 8 ? std::exp(-(bar - 8) * 3.0) : 0.0;
            b = c.step(beat, bar, true, f / 60.0, bld, drp, 1.0 / 60.0);
            double d = b.zoom - prev;
            if (d < -0.5) d += 1.0;
            if (d > 0.5) d -= 1.0;
            if (f > 0) travelled += d;
            prev = b.zoom;
        }
        return std::pair<double, double>(travelled, b.zoom);
    };
    const auto plain = run(false, false), dropped = run(false, true), built = run(true, false);
    CHECK(std::fabs(plain.first - 12.0) < 1e-6);
    CHECK(std::fabs(dropped.first - 14.0) < 1e-3);  // two extra steps
    // The build fell behind and caught up again to the next whole step after it.
    CHECK(std::fabs(built.first - std::round(built.first)) < 1e-3);
    CHECK(std::min(built.second, 1.0 - built.second) < 1e-3);
    CHECK(std::min(dropped.second, 1.0 - dropped.second) < 1e-3);
}

TEST_CASE("fractals: the clock is deterministic and stays bounded on junk, huge and jumping inputs") {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    auto sequence = [&] {
        FractalClock c;
        std::vector<double> z;
        const double beats[] = {0, 1, 2, 1e300, -1e300, nan, inf, -inf, 3, 1e12, 1e12 + 0.5, 4, 2, 9.25e15, 5};
        double t = 0;
        for (double b : beats) {
            for (double drop : {0.0, 1.0, nan}) {
                t += 1.0 / 60.0;
                const FractalBeat r = c.step(b, b / 4.0, true, t, drop * 0.5, drop, drop > 0 ? 1e300 : 1.0 / 60.0);
                CHECK(std::isfinite(r.zoom));
                CHECK(r.zoom >= 0.0);
                CHECK(r.zoom < 1.0);
                CHECK(std::isfinite(r.pulse));
                CHECK(std::isfinite(r.beats));
                CHECK(std::fabs(r.beats) <= 65537.0);
                z.push_back(r.zoom);
            }
        }
        // No clock: 120 BPM from the start, and junk ages.
        for (double age : {0.0, 1.0, nan, -5.0, 1e300}) {
            const FractalBeat r = c.step(0, 0, false, age, 0, 0, 0.016);
            CHECK(std::isfinite(r.zoom));
            z.push_back(r.zoom);
        }
        return z;
    };
    CHECK(sequence() == sequence());
    // Junk timing settings are clamped.
    FractalTiming t;
    t.barsPerStep = nan;
    t.pulse = 1e300;
    t.drop = -inf;
    t.dropBeats = 0;
    t.hover = nan;
    FractalClock c(t);
    for (int f = 0; f < 600; ++f) {
        const FractalBeat r = c.step(f / 30.0, f / 120.0, true, f / 60.0, 0.5, f % 120 == 0 ? 1.0 : 0.0, 1.0 / 60.0);
        CHECK(std::isfinite(r.zoom));
    }
    // A camera from junk sway stays finite.
    for (int i = 0; i < fractalPathCount(); ++i) {
        FractalSway sw{nan, inf, -inf, 1e300, nan};
        const FractalCamera cam = fractalCamera(fractalPath(i), 1e300, sw);
        for (double v : {cam.eye[0], cam.eye[1], cam.eye[2], cam.forward[0], cam.up[1], cam.scale, cam.fov})
            CHECK(std::isfinite(v));
    }
}

TEST_CASE("fractal solids: the content scenes step on their bar lines, glow on the beats, and stay finite") {
    const std::string dir = std::string(REFORCE_SOURCE_DIR) + "/content/Packages/reforce.Solid.package/";
    for (const char* name : {"Sierpinski Apex", "Sierpinski Vortex", "Menger Tunnels"}) {
        CAPTURE(name);
        auto cfg = compileSolid(readFile(dir + name + ".txt"));
        CHECK(cfg->diags.items().empty());
        REQUIRE(solidMesh(*cfg) == MeshKind::Fractal);
        reforce::scene::Environment env;
        SolidRun r(cfg, env, name, MeshKind::Fractal, 1.0, 0.9, 30.0, nullptr);
        REQUIRE(r.fractal().path != nullptr);
        CHECK(r.fractal().error.empty());
        const double per = r.fractal().timing.barsPerStep;
        SpriteAudio a;
        a.sync.on = 1;
        a.sync.bpm = 124;
        double maxGlow = 0, minGlow = 1e9;
        int lines = 0;
        for (int f = 0; f < 60 * 40; ++f) {  // 40 s at 60 fps
            const double t = 1.0 + f / 60.0;
            a.sync.beat = (t - 1.0) * 124.0 / 60.0;
            a.sync.bar = a.sync.beat / 4.0;
            const SolidSpec s = r.evaluate(t, a);
            for (double v : {s.fractalCam.eye[0], s.fractalCam.eye[1], s.fractalCam.eye[2], s.fractalCam.forward[0],
                             s.fractalCam.scale, s.fog, s.glow, s.fill, s.shade})
                CHECK(std::isfinite(v));
            CHECK(s.zoom >= 0.0);
            CHECK(s.zoom < 1.0);
            // Within a frame of every ZBar-th bar line the zoom is on a whole step (the path's base pose).
            const double steps = a.sync.bar / per, frameSteps = 124.0 / 60.0 / 4.0 / 60.0 / per;
            if (std::fabs(steps - std::round(steps)) < 0.5 * frameSteps) {
                ++lines;
                CHECK(std::min(s.zoom, 1.0 - s.zoom) < 4.0 * frameSteps);
            }
            if (t > 3.0) {
                maxGlow = std::max(maxGlow, s.glow);
                minGlow = std::min(minGlow, s.glow);
            }
        }
        CHECK(lines >= 4);
        CHECK(maxGlow > 0.4);   // a pulse on the beat lines
        CHECK(minGlow < 0.05);  // and dark between
    }
}

TEST_CASE("fractal solids: unknown names, the Zoom key, junk settings") {
    reforce::scene::Environment env;
    {
        auto cfg = compileSolid("Mesh=\"fractal\"\nFrac=\"mandelbulb\"\n");
        SolidRun r(cfg, env, "x", MeshKind::Fractal, 0.0, 1.0, 30.0, nullptr);
        CHECK(r.fractal().path == nullptr);
        CHECK(r.fractal().error.find("Frac") != std::string::npos);
        const SolidSpec s = r.evaluate(1.0, {});  // no path: no camera, but no crash either
        CHECK(std::isfinite(s.opacity));
    }
    {
        auto cfg = compileSolid("Mesh=\"fractal\"\nFrac=\"menger\"\nPath=\"apex\"\n");
        SolidRun r(cfg, env, "x", MeshKind::Fractal, 0.0, 1.0, 30.0, nullptr);
        CHECK(r.fractal().path == nullptr);
        CHECK(r.fractal().error.find("corner") != std::string::npos);  // lists the menger paths
    }
    {
        // Zoom from the scene (one step per 4 s), junk load-time numbers clamped.
        auto cfg = compileSolid("Mesh=\"fractal\"\nPath=\"vortex\"\nZoom=\"(t - START_TIME) / 4\"\nRes=\"1e300\"\n"
                                "Detl=\"-5\"\nStps=\"1e9\"\nZBar=\"0\"\nPuls=\"7\"\nOrbt=\"1e300\"\nFog=\"-1\"\n");
        CHECK(cfg->diags.items().empty());
        SolidRun r(cfg, env, "x", MeshKind::Fractal, 2.0, 1.0, 30.0, nullptr);
        REQUIRE(r.fractal().path != nullptr);
        CHECK(r.fractal().kind == FractalKind::Sierpinski);
        CHECK(std::string(r.fractal().path->name) == "vortex");
        CHECK(r.fractal().res == 1.0);
        CHECK(r.fractal().detail == 0.25);
        CHECK(r.fractal().steps == 400);
        CHECK(r.fractal().timing.barsPerStep == 0.25);
        CHECK(r.fractal().timing.pulse == 1.0);
        const SolidSpec a = r.evaluate(3.0, {}), b = r.evaluate(5.0, {});
        CHECK(a.zoom == doctest::Approx(0.25));
        CHECK(b.zoom == doctest::Approx(0.75));
        CHECK(a.fog == 0.2);
        for (double v : {a.fractalCam.eye[0], a.fractalCam.up[1], b.fractalCam.right[2]}) CHECK(std::isfinite(v));
    }
}

TEST_CASE("fractal solids: they follow the beat (the green note) unless the scene drives Zoom itself") {
    using reforce::scene::SceneType;
    auto flag = [](const std::string& text) {
        auto f = reforce::scene::parseSceneText(text, "test");
        return objectFollowsBeat(SceneType::Solid, reforce::scene::compileConfig(f, SceneType::Solid));
    };
    CHECK(flag("Mesh=\"fractal\"\n"));
    CHECK_FALSE(flag("Mesh=\"fractal\"\nZoom=\"t / 4\"\n"));
    CHECK(flag("Mesh=\"fractal\"\nZoom=\"SYNC_BAR / 2\"\n"));
}
