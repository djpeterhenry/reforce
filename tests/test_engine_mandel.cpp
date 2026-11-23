// The Mandelbrot dive (engine/MandelDive, Kind="mandel" organics): depth on the beat clock, resets on the dive
// lines, kicks and drops, the seeded target order, the keys, junk.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "engine/MandelDive.hpp"
#include "engine/Organics.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;

namespace {

std::shared_ptr<const reforce::scene::CompiledConfig> compileMandel(const std::string& text) {
    auto file = reforce::scene::parseSceneText(text, "test");
    return std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Organic));
}

// Depth `b` beats into a dive with Dbl 2 and no cap in sight: (b - (1 - e^-b)) / 2.
double linear(double b) { return (b - (1.0 - std::exp(-b))) / 2.0; }

}  // namespace

TEST_CASE("mandel: depth follows the beat clock, rushes back out at the end, resets on the dive line") {
    MandelDiveParams p;
    p.minis = 2;  // edge points only: their depth runs to Deep
    p.entry = 0;  // seeks to any beat (the catch-up after a start mid-dive has its own test)
    p.diveBars = 16;
    p.dbl = 2;
    p.deep = 32;
    p.out = 4;
    p.kick = 0;
    MandelDive dive(p, 7);
    auto at = [&](double beat) { return dive.step(beat, beat / 4.0, 0.0, 1.0 / 60.0, -1); };
    // On the line: the overview.
    MandelView v = at(0);
    CHECK(v.dive == 0);
    CHECK(v.depth == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(v.cx == doctest::Approx(kMandelHomeRe));
    CHECK(v.cy == doctest::Approx(0.0));
    CHECK(v.half == doctest::Approx(kMandelHomeHalf));
    // A doubling every 2 beats, easing in over the first beat.
    CHECK(at(8).depth == doctest::Approx(linear(8)).epsilon(1e-4));
    CHECK(at(40).depth == doctest::Approx(linear(40)).epsilon(1e-4));
    CHECK(at(40).half == doctest::Approx(kMandelHomeHalf * std::exp2(-linear(40))).epsilon(1e-3));
    double last = -1;
    for (double b = 0; b <= 60; b += 0.25) {
        const double d = at(b).depth;
        CHECK(d >= last);
        last = d;
    }
    // Softly capped at Deep (32): 30 doublings at beat 60 is already a little under.
    CHECK(at(60).depth == doctest::Approx(29.5 - std::log1p(std::exp(-2.5))).epsilon(1e-4));
    CHECK(at(60).depth < 29.5);
    // The rush out over the last 4 beats: half way down at beat 62, the overview on the line.
    const double lin62 = 30.5 - std::log1p(std::exp(-1.5));
    CHECK(at(62).depth == doctest::Approx(0.5 * lin62).epsilon(1e-4));
    CHECK(at(63.99).depth < 0.01);
    v = at(64);
    CHECK(v.dive == 1);
    CHECK(v.depth == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(at(64 + 40).depth == doctest::Approx(linear(40)).epsilon(1e-4));
    // Deep in, the centre is the target (to well under a pixel).
    v = at(58);
    const MandelTarget& t = mandelTarget(v.target);
    CHECK(std::abs(v.cx - t.re) < v.half * 1e-3);
    CHECK(std::abs(v.cy - t.im) < v.half * 1e-3);
    // Iterations grow with depth.
    CHECK(at(50).iter > at(10).iter);
    CHECK(at(10).iter >= p.iter);

    // Dbl 0: the dive is paced to reach Deep just before the rush out.
    MandelDiveParams q = p;
    q.dbl = 0;
    MandelDive paced(q, 7);
    const double dbl = 60.0 / 32.0, lin60 = 59.0 / dbl;  // (beat - ~1) / dbl, then the soft cap
    CHECK(paced.step(60, 15, 0, 0.016, -1).depth ==
          doctest::Approx(lin60 - std::log1p(std::exp(lin60 - 32))).epsilon(1e-4));
    CHECK(paced.step(30, 7.5, 0, 0.016, -1).depth == doctest::Approx(29.0 / dbl).epsilon(1e-3));
}

TEST_CASE("mandel: kicks surge in on the beat lines, a drop pulls out and the dive resumes") {
    MandelDiveParams p;
    p.minis = 2;  // edge points only: their depth runs to Deep
    p.entry = 0;  // seeks to any beat (the catch-up after a start mid-dive has its own test)
    p.kick = 0.3;
    p.drop = 6;
    MandelDive dive(p, 7);
    auto at = [&](double beat, double drop = 0.0, double dt = 1.0 / 60.0) {
        return dive.step(beat, beat / 4.0, drop, dt, -1).depth;
    };
    // The surge peaks just after the line (.3 x e^-.5 at a tenth of a beat) and has died away by the next.
    CHECK(at(40.1) - linear(40.1) == doctest::Approx(0.3 * std::exp(-0.5)).epsilon(1e-3));
    CHECK(at(40.9) - linear(40.9) < 0.01);
    CHECK(at(40.0) - linear(40.0) == doctest::Approx(0.0).epsilon(1e-6));
    // A drop: a fast attack (~80 ms), 6 doublings out at full strength, following the drop's decay down.
    const double base = linear(40.5);
    double d = 0;
    for (int i = 0; i < 2; ++i) d = at(40.5, 1.0);
    CHECK(d > base - 6 + 0.5);  // not all at once
    for (int i = 0; i < 60; ++i) d = at(40.5, 1.0);
    CHECK(d == doctest::Approx(base + 0.3 * std::exp(-2.5) - 6).epsilon(1e-3));
    CHECK(at(40.5, 0.5) == doctest::Approx(base + 0.3 * std::exp(-2.5) - 3).epsilon(1e-3));
    CHECK(at(40.5, 0.0) == doctest::Approx(base + 0.3 * std::exp(-2.5)).epsilon(1e-3));
    // Never above the surface.
    for (int i = 0; i < 60; ++i) CHECK(at(2.5, 1.0) >= 0.0);
}

TEST_CASE("mandel: four kinds of endings taking turns, each kind's targets in a seeded order; Tgt fixes one") {
    const int n = mandelTargetCount();
    REQUIRE(n >= 10);
    int perKind[kMandelKinds] = {};
    for (int i = 0; i < n; ++i) {
        const MandelTarget& t = mandelTarget(i);
        CAPTURE(t.name);
        CHECK(std::isfinite(t.re));
        CHECK(std::isfinite(t.im));
        CHECK(t.re * t.re + t.im * t.im < 4.0);  // on the set: inside the radius-2 disc
        CHECK(t.itA > 0);
        CHECK(t.itB >= 0);
        ++perKind[int(t.kind)];
        const bool nucleus = t.kind == MandelKind::Mini || t.kind == MandelKind::Decor;
        CHECK(t.hasMini() == nucleus);
        if (t.kind == MandelKind::Mini) {
            CHECK(t.deep() == doctest::Approx(t.miniDeep()));
            CHECK(t.deep() > 10);
            CHECK(t.deep() < 40);
        } else if (t.kind == MandelKind::Decor) {
            CHECK(t.deep() > 10);
            CHECK(t.deep() < t.miniDeep() - 3);  // well above the mini: the affine test never applies
        } else {
            CHECK(t.deep() == 0);
        }
    }
    for (int k = 0; k < kMandelKinds; ++k) CHECK(perKind[k] >= 2);
    // Mini 0: every kind; 1: minis and decorations only; 2: no minis.
    for (int mode = 0; mode <= 2; ++mode) {
        CAPTURE(mode);
        MandelDiveParams p;
        p.minis = mode;
        std::set<int> pool;
        for (int i = 0; i < n; ++i) {
            const bool mini = mandelTarget(i).hasMini();
            if (mode == 0 || (mode == 1) == mini) pool.insert(i);
        }
        const int m = int(pool.size());
        MandelDive a(p, 11), b(p, 11), c(p, 12);
        int prev = -1;
        bool differs = false;
        std::set<int> seen;
        for (long long d = -3 * m; d < 6 * m; ++d) {
            const int ta = a.targetFor(d, -1);
            CHECK(ta == b.targetFor(d, -1));  // deterministic
            CHECK(pool.count(ta) == 1);
            if (prev >= 0) CHECK(mandelTarget(ta).kind != mandelTarget(prev).kind);  // the kinds take turns
            prev = ta;
            differs = differs || ta != c.targetFor(d, -1);
            if (d >= 0) seen.insert(ta);
        }
        CHECK(differs);              // another seed, another order
        CHECK(int(seen.size()) == m);  // every target comes up
        CHECK(a.targetFor(5, 3) == 3);
        CHECK(a.targetFor(5, n + 2) == 2);
    }
}

TEST_CASE("mandel: a dive to a mini lands on it framed and upright; a decoration stops above it") {
    for (int i = 0; i < mandelTargetCount(); ++i) {
        const MandelTarget& t = mandelTarget(i);
        if (!t.hasMini()) continue;
        CAPTURE(t.name);
        MandelDiveParams p;
        p.diveBars = 16;
        p.dbl = 0;  // paced to the target's depth
        p.deep = 40;
        p.out = 4;
        p.kick = 0;
        p.entry = 0;
        p.align = 0;
        p.maxIter = 20000;
        MandelDive dive(p, 3);
        auto at = [&](double beat) { return dive.step(beat, beat / 4.0, 0.0, 1.0 / 60.0, i); };
        // Paced: the target's depth (less the soft cap's ln 2) just before the rush out.
        const double cap = t.deep();
        const double dbl = 60.0 / cap, lin = 59.0 / dbl;
        MandelView v = at(60);
        CHECK(v.depth == doctest::Approx(lin - std::log1p(std::exp(lin - cap))).epsilon(1e-4));
        CHECK(v.depth < cap);
        CHECK(std::abs(v.cx - t.frameRe()) < v.half * 1e-2);
        CHECK(std::abs(v.cy - t.frameIm()) < v.half * 1e-2);
        if (t.kind == MandelKind::Mini) {
            // Framed like the overview frames the set (nucleus - .65 size), upright, its inside skipped.
            CHECK(t.frameRe() == doctest::Approx(t.re - 0.65 * t.sre));
            CHECK(v.turn == doctest::Approx(t.angle()).epsilon(1e-6));
            CHECK(v.mini);
            CHECK(v.iter >= std::min(20000, int(45 * t.period * 1.5 * 0.999)));
        } else {
            // The ring around the mini, centred on it; the mini far too small for the affine test.
            CHECK(t.frameRe() == t.re);
            CHECK(v.turn == 0);
            CHECK_FALSE(v.mini);
            CHECK(v.iter >= int(10 * t.period * 1.5 * 0.999));
        }
        // Early in the dive: no turn, no interior shortcut, the approach's iterations.
        v = at(8);
        CHECK(std::abs(v.turn) < 1e-9);
        CHECK_FALSE(v.mini);
        CHECK(v.iter < 10 * t.period * 1.5 + t.itA * 1.5 + 200);
        // Aff 0: no interior shortcut.
        p.affine = false;
        MandelDive off(p, 3);
        CHECK_FALSE(off.step(60, 15, 0.0, 1.0 / 60.0, i).mini);
    }
}

TEST_CASE("mandel: Kind=mandel reads its keys, follows the clock, picks its precision, survives junk") {
    reforce::scene::Environment env;
    const FrameMapping map = organicMapping(1920, 1080);
    const auto cfg = compileMandel(
        "Kind=\"mandel\"\nEnt=\"0\"\nAlgn=\"0\"\nDive=\"16\"\nDbl=\"2\"\nDeep=\"30\"\nKick=\"0\"\nBand=\"8\"\nGlow=\".5\"\nRes=\".5\"\n"
        "Rot=\".25\"\nPen=\".9\"\n");
    for (const auto& d : cfg->diags.items()) {
        CAPTURE(d.message);
        CHECK(false);
    }
    REQUIRE(organicKind(*cfg) == OrganicKind::Field);
    OrganicRun r(cfg, env, "m", 0.0, 0.5, 60.0, nullptr);
    CHECK(r.kindName() == "mandel");
    std::vector<LineSegment> segs;
    SpriteAudio a;
    a.sync.on = 1;
    auto step = [&](double t, double beat) {
        a.sync.beat = beat;
        a.sync.bar = beat / 4.0;
        r.step(t, a, map, LineStyle{}, segs);
        const OrganicField* f = r.field();
        REQUIRE(f);
        return *f;
    };
    OrganicField f = step(1.0, 64 + 10);  // 10 beats into dive 1
    CHECK(segs.empty());
    CHECK(f.kind == OrganicField::Kind::Mandel);
    CHECK(f.dive == 1);
    CHECK(f.depth == doctest::Approx(linear(10)).epsilon(1e-4));
    CHECK(f.precision == 0);  // shallow: float32
    CHECK(f.bands == doctest::Approx(8));
    CHECK(f.glow == doctest::Approx(0.5));
    CHECK(f.res == doctest::Approx(0.5));
    CHECK(f.rot == doctest::Approx(0.25));
    CHECK(f.pen == doctest::Approx(0.9));
    CHECK(std::cos(f.phase) == doctest::Approx(std::cos(-2 * 3.141592653589793 * 74)).epsilon(1e-6));
    f = step(1.1, 64 + 40.25);
    CHECK(f.depth == doctest::Approx(linear(40.25)).epsilon(1e-4));
    CHECK(f.precision == 2);  // deep: perturbation
    CHECK(std::cos(f.phase) == doctest::Approx(std::cos(-2 * 3.141592653589793 * 104.25)).epsilon(1e-6));
    f = step(1.2, 128);  // the phrase line: the next dive, from the overview
    CHECK(f.dive == 2);
    CHECK(f.depth == doctest::Approx(0.0).epsilon(1e-9));

    // No beat clock: 120 BPM from the start. Prec 2: double-float throughout; Tgt fixes the target.
    const auto cfg2 = compileMandel("Kind=\"mandelbrot\"\nEnt=\"0\"\nPrec=\"2\"\nTgt=\"3\"\nKick=\"0\"\nPen=\"1\"\n");
    OrganicRun r2(cfg2, env, "m2", 5.0, 0.5, 60.0, nullptr);
    SpriteAudio none;
    r2.step(5.0 + 10.0, none, map, LineStyle{}, segs);  // 20 beats in
    REQUIRE(r2.field());
    CHECK(r2.field()->depth == doctest::Approx(linear(20)).epsilon(1e-4));
    CHECK(r2.field()->precision == 1);
    CHECK(r2.field()->target == 3);

    // Junk keys stay finite and in range.
    const auto junk = compileMandel(
        "Kind=\"mandel\"\nDive=\"0\"\nDbl=\"1/0\"\nDeep=\"-5\"\nOut=\"1e300\"\nKick=\"1e9\"\nDrop=\"-1/0\"\n"
        "Iter=\"1e30\"\nItk=\"-5\"\nMaxI=\"0\"\nPrec=\"9\"\nRes=\"0\"\nTgt=\"1e300\"\nRot=\"1/0\"\nScal=\"0\"\n"
        "X=\"1e308 * 10\"\nBand=\"-3\"\nGlow=\"1e9\"\nGain=\"0/0\"\nPhas=\"1e300\"\nPen=\"1\"\n");
    OrganicRun rj(junk, env, "j", 0.0, 0.5, 60.0, nullptr);
    for (int i = 0; i < 200; ++i) {
        a.sync.beat = i == 50 ? std::nan("") : i * 0.37;
        a.sync.bar = i == 60 ? 1e300 : a.sync.beat / 4.0;
        a.sync.drop = i % 7 == 0 ? 1.0 : 0.0;
        rj.step(i / 60.0, a, map, LineStyle{}, segs);
        const OrganicField* j = rj.field();
        if (!j) continue;
        CHECK(std::isfinite(j->cx));
        CHECK(std::isfinite(j->cy));
        CHECK(std::isfinite(j->half));
        CHECK(j->half > 0);
        CHECK(std::isfinite(j->depth));
        CHECK(j->depth >= 0);
        CHECK(j->depth <= 60);
        CHECK(j->iter >= 8);
        CHECK(j->iter <= 20000);
        CHECK(j->res >= 0.25);
        CHECK(j->res <= 1.0);
        CHECK(j->precision >= 0);
        CHECK(j->precision <= 2);
        CHECK(j->target >= 0);
        CHECK(j->target < mandelTargetCount());
        CHECK(std::isfinite(j->phase));
        CHECK(std::isfinite(j->rot));
        CHECK(std::isfinite(j->x));
        CHECK(std::isfinite(j->gain));
        CHECK(std::isfinite(j->glow));
        CHECK(std::isfinite(j->bands));
    }
}

TEST_CASE("mandel: started mid-dive it starts from the whole set and catches up to the dive") {
    MandelDiveParams p;
    p.minis = 2;
    p.kick = 0;
    p.drop = 0;
    MandelDive seek(p, 1);
    p.entry = 0;
    MandelDive ref(p, 1);
    // Started 10 beats into a dive (bar 2.5): the dive is at 10 / 2 doublings minus the ease-in.
    const double target = ref.step(10.0, 2.5, 0, 0.016, -1).depth;
    REQUIRE(target > 3.0);
    CHECK(seek.step(10.0, 2.5, 0, 0.016, -1).depth < 0.01);  // the whole set first
    double d = 0;
    int frames = 0;
    for (double b = 10.0; b < 14.0; b += 0.02, ++frames) {
        const double now = seek.step(b, b / 4.0, 0, 0.016, -1).depth;
        CHECK(now >= d - 1e-9);  // only ever zooming in
        d = now;
    }
    // Within about 2 beats (6 doublings a beat) it meets the dive and follows it from there.
    CHECK(seek.step(14.0, 3.5, 0, 0.016, -1).depth == doctest::Approx(ref.step(14.0, 3.5, 0, 0.016, -1).depth));
}

TEST_CASE("mandel: long dives count from the phrase line at the start, not from bar 0") {
    MandelDiveParams p;
    p.minis = 2;
    p.entry = 0;
    p.kick = 0;
    p.drop = 0;
    p.diveBars = 64;
    p.dbl = 8;
    MandelDive m(p, 1);
    // Started at bar 37.5: the dives count from bar 32 (the phrase line before), so bar 32 + 64 = 96 is the next.
    const MandelView a = m.step(150.0, 37.5, 0, 0.016, -1);
    CHECK(a.dive == 0);
    CHECK(a.pos == doctest::Approx(5.5 / 64.0));
    CHECK(m.step(4.0 * 95.9, 95.9, 0, 0.016, -1).dive == 0);
    CHECK(m.step(4.0 * 96.1, 96.1, 0, 0.016, -1).dive == 1);
}

TEST_CASE("mandel: shared (the organics), each new Mandelbrot object carries on with the next dive, another kind") {
    MandelDiveParams p;
    p.shared = true;
    p.entry = 0;
    p.kick = 0;
    int prev = -1;
    for (int k = 0; k < 12; ++k) {
        MandelDive d(p, 100 + std::uint64_t(k));  // the seed of the first shared one is kept
        const MandelView v = d.step(30.0, 7.5, 0.0, 1.0 / 60.0, -1);
        CHECK(v.dive == 0);  // its own first dive
        if (prev >= 0) {
            CHECK(v.target != prev);
            CHECK(mandelTarget(v.target).kind != mandelTarget(prev).kind);
        }
        prev = v.target;
    }
}
