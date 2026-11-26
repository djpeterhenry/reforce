// Organics (PLAN 5N, docs/organics-design.md): tree growth by space colonisation.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "engine/Organics.hpp"
#include "engine/SceneLibrary.hpp"
#include "engine/Themes.hpp"
#include "engine/TreeGrowth.hpp"
#include "expr/Rng.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;

namespace {

// A dome of `n` points: radius rx x ry around (cx, cy), upper half only.
TreeParams dome(int n, double cx = 0, double cy = 0.25, double rx = 0.9, double ry = 0.6, std::uint64_t seed = 7) {
    TreeParams p;
    reforce::expr::Pcg32 rng(seed);
    for (int i = 0; i < n; ++i) {
        const double a = rng.uniform01() * 3.141592653589793;
        const double r = std::sqrt(rng.uniform01());
        p.pointsX.push_back(cx + rx * r * std::cos(a));
        p.pointsY.push_back(cy + ry * r * std::sin(a));
    }
    p.roots.push_back({0.0, -1.0, 1.5707963267948966});
    return p;
}

int growAll(TreeGrowth& g, int cap = 5000) {
    int it = 0;
    while (!g.done() && it < cap) {
        g.iterate();
        ++it;
    }
    return it;
}

}  // namespace

TEST_CASE("trees: a trunk grows to the crown, branches use up the points, then it stops") {
    TreeGrowth g(dome(300), 1);
    CHECK(g.nodes().size() == 1);
    CHECK(g.livePoints() == 300);
    const int iterations = growAll(g);
    CHECK(g.done());
    CHECK(iterations < 1000);
    // Most of the crown was reached.
    CHECK(g.livePoints() < 30);
    const auto& n = g.nodes();
    CHECK(n.size() > 200);
    CHECK(n.size() < 6000);
    // Parents always come first; every segment has the configured length (roots excepted).
    double topY = -10;
    for (std::size_t i = 0; i < n.size(); ++i) {
        CAPTURE(i);
        if (n[i].parent < 0) continue;
        REQUIRE(n[i].parent < int(i));
        const auto& pa = n[std::size_t(n[i].parent)];
        CHECK(std::hypot(n[i].x - pa.x, n[i].y - pa.y) == doctest::Approx(0.02).epsilon(1e-3));
        CHECK(n[i].gen > pa.gen);
        topY = std::max(topY, double(n[i].y));
    }
    CHECK(topY > 0.6);  // reached the top of the dome
    // It branched: many tips, and the root feeds them all (pipe model).
    int tips = 0;
    for (const auto& nd : n) tips += nd.children == 0 ? 1 : 0;
    CHECK(tips > 20);
    CHECK(n[0].tips == doctest::Approx(tips));
    CHECK(g.maxTips() == doctest::Approx(tips));
    // Iterating a finished tree does nothing.
    const std::size_t before = n.size();
    CHECK(g.iterate() == 0);
    CHECK(g.nodes().size() == before);
}

TEST_CASE("trees: the same seed grows the same tree, another seed a different one") {
    TreeGrowth a(dome(250), 42), b(dome(250), 42), c(dome(250), 43);
    growAll(a);
    growAll(b);
    growAll(c);
    REQUIRE(a.nodes().size() == b.nodes().size());
    for (std::size_t i = 0; i < a.nodes().size(); ++i) {
        CHECK(a.nodes()[i].x == b.nodes()[i].x);
        CHECK(a.nodes()[i].y == b.nodes()[i].y);
    }
    bool differs = a.nodes().size() != c.nodes().size();
    for (std::size_t i = 0; !differs && i < a.nodes().size(); ++i) differs = a.nodes()[i].x != c.nodes()[i].x;
    CHECK(differs);
}

TEST_CASE("trees: tropism bends the growth, maxNodes and maxTrunk bound it") {
    // Wind to the right: the branches grow rightwards on average; the trunk ignores it.
    auto meanDx = [](const TreeGrowth& g, bool trunk) {
        double s = 0;
        int n = 0;
        for (const auto& nd : g.nodes())
            if (nd.parent >= 0 && (nd.y < -0.5) == trunk) {
                s += nd.dx;
                ++n;
            }
        return s / std::max(1, n);
    };
    TreeGrowth calm(dome(300), 3), windy(dome(300), 3);
    growAll(calm);
    while (!windy.done()) windy.iterate(1.0, 0.0);  // below ~1 the points mostly pull the growth back
    CHECK(meanDx(windy, false) > meanDx(calm, false) + 0.1);
    CHECK(std::abs(meanDx(windy, true)) < 0.1);  // the trunk (below y = -.5) stays upright

    TreeParams p = dome(2000);
    p.maxNodes = 150;
    TreeGrowth capped(p, 5);
    growAll(capped);
    CHECK(capped.done());
    CHECK(capped.nodes().size() <= 150);

    // Points out of reach above: the trunk gives up after maxTrunk iterations.
    TreeParams far = dome(50, 0, 50.0, 0.5, 0.5);
    far.maxTrunk = 30;
    TreeGrowth lost(far, 1);
    const int it = growAll(lost);
    CHECK(lost.done());
    CHECK(it <= 32);
    CHECK(lost.nodes().size() == 31);
}

TEST_CASE("trees: several roots, roots downwards, no points, junk input") {
    // Roots: points below a root at the top, growing down.
    TreeParams roots;
    reforce::expr::Pcg32 rng(9);
    for (int i = 0; i < 200; ++i) {
        roots.pointsX.push_back((rng.uniform01() * 2 - 1) * 1.2);
        roots.pointsY.push_back(0.8 - rng.uniform01() * 1.6);
    }
    roots.roots.push_back({0.0, 1.0, -1.5707963267948966});
    TreeGrowth down(roots, 2);
    growAll(down);
    CHECK(down.livePoints() < 40);
    double minY = 10;
    for (const auto& n : down.nodes()) minY = std::min(minY, double(n.y));
    CHECK(minY < -0.5);

    // Three roots share one cloud; each grows.
    TreeParams three = dome(300, 0, 0.2, 1.6, 0.8);
    three.roots = {{-1.2, -1, 1.57}, {0, -1, 1.57}, {1.2, -1, 1.57}};
    TreeGrowth g3(three, 4);
    growAll(g3);
    int rootsWithChildren = 0;
    for (const auto& n : g3.nodes())
        if (n.parent < 0 && n.children > 0) ++rootsWithChildren;
    CHECK(rootsWithChildren == 3);

    TreeParams none;
    TreeGrowth empty(none, 1);
    CHECK(empty.done());
    CHECK(empty.nodes().size() == 1);  // a default root

    TreeParams junk = dome(100);
    junk.pointsX.push_back(NAN);
    junk.pointsY.push_back(0.5);
    junk.pointsX.push_back(1e9);
    junk.pointsY.push_back(0.5);
    junk.segment = -3;
    junk.influence = INFINITY;
    junk.roots[0].angle = NAN;
    TreeGrowth j(junk, 1);
    CHECK(j.livePoints() == 100);
    growAll(j, 20000);
    CHECK(j.done());
    for (const auto& n : j.nodes()) {
        CHECK(std::isfinite(n.x));
        CHECK(std::isfinite(n.y));
    }
}

namespace {

std::shared_ptr<const reforce::scene::CompiledConfig> compileOrganic(const std::string& text) {
    auto file = reforce::scene::parseSceneText(text, "test");
    return std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Organic));
}


// Steps a run at 60 Hz from `t0` to `t1`, returning the last frame's segments.
std::vector<LineSegment> runTo(OrganicRun& r, double t0, double t1, const SpriteAudio& a = {}) {
    const FrameMapping map = organicMapping(1920, 1080);
    LineStyle style;
    std::vector<LineSegment> segs;
    for (double t = t0; t <= t1 + 1e-9; t += 1.0 / 60.0) {
        segs.clear();
        r.step(t, a, map, style, segs);
    }
    return segs;
}

}  // namespace

TEST_CASE("organics: tree scene keys compile cleanly, Kind picks the algorithm") {
    const auto cfg = compileOrganic(
        "Kind=\"tree\"\nPDur=\"20\"\nPts=\"200\"\nPX=\".8 * (2 * rnd(1) - 1)\"\nPY=\".2 + .5 * s\"\n"
        "Rts=\"2\"\nRtX=\"s - .5\"\nRtY=\"-1\"\nRtA=\"PI / 2\"\nSeg=\".025\"\nInfl=\".3\"\nKill=\".05\"\n"
        "Jitr=\".1\"\nWdth=\"6\"\nTip=\"1\"\nBudW=\"3\"\nWthr=\"1\"\nA0=\"2\"\n"
        "B0=\"t - START_TIME\"\nGrow=\"10 * A0\"\nTrpX=\".1 * sin(B0)\"\nTrpY=\".2\"\nSway=\".1 * BASS\"\n"
        "X=\"0\"\nY=\"0\"\nScal=\"1\"\nRot=\"0\"\nPen=\".9\"\nBud=\"exp(-4 * wrap(SYNC_BEAT))\"\n");
    for (const auto& d : cfg->diags.items()) {
        CAPTURE(d.message);
        CHECK(false);
    }
    CHECK(organicKind(*cfg) == OrganicKind::Tree);
    CHECK(organicKind(*compileOrganic("Grow=\"5\"\n")) == OrganicKind::Tree);  // default
    std::string written;
    CHECK_FALSE(organicKind(*compileOrganic("Kind=\"physarum\"\n"), &written).has_value());
    CHECK(written == "physarum");

    reforce::scene::Environment env;
    OrganicRun r(cfg, env, "test", 10.0, 2.0, 30.0, nullptr);
    CHECK(r.endTime() == doctest::Approx(10.0 + 2.0 + 20.0 + 2.0));  // PDur wins over the default hold
    CHECK(r.tree().nodes().size() == 2);                              // two roots
    CHECK(r.tree().livePoints() <= 200);
}

TEST_CASE("organics: a tree grows in simulation time, sways, and withers at the end") {
    reforce::scene::Environment env;
    env.seed = 11;
    const auto cfg = compileOrganic("Grow=\"20\"\nSway=\".3\"\nBud=\"1\"\n");
    OrganicRun r(cfg, env, "oak", 0.0, 2.0, 30.0, nullptr);
    // One second at 20 iterations per second: about 20 generations.
    auto segs = runTo(r, 0.0, 1.0);
    CHECK(r.tree().generation() >= 18);
    CHECK(r.tree().generation() <= 21);
    CHECK(!segs.empty());
    for (const auto& s : segs) {
        CHECK(std::isfinite(s.ax));
        CHECK(std::isfinite(s.by));
        CHECK(s.radius > 0.f);
        CHECK(s.path == 0.f);  // one path per tree
    }
    // The trunk grows up from the bottom center of the frame (y = -1 -> pixel row 1080).
    CHECK(segs.front().ax == doctest::Approx(960.5).epsilon(0.01));
    CHECK(segs.front().ay == doctest::Approx(1080.5).epsilon(0.01));
    // Grown out by the middle of its life; the root's segment is the thickest.
    segs = runTo(r, 1.0 + 1.0 / 60.0, 15.0);
    CHECK(r.tree().done());
    const std::size_t full = segs.size();
    CHECK(full > 300);
    float maxR = 0;
    for (const auto& s : segs) maxR = std::max(maxR, s.radius);
    CHECK(segs.front().radius == maxR);
    // Sway moves the tips over time but not the root.
    const auto a = runTo(r, 15.0 + 1.0 / 60.0, 16.0);
    const auto b = runTo(r, 16.0 + 1.0 / 60.0, 17.5);
    REQUIRE(a.size() == b.size());
    CHECK(a.front().ax == b.front().ax);
    double moved = 0;
    for (std::size_t i = 0; i < a.size(); ++i) moved = std::max(moved, double(std::abs(a[i].bx - b[i].bx)));
    CHECK(moved > 2.0);
    // Withering: half way through the fade-out about half of it is left; nothing at the end.
    const double end = r.endTime();
    const auto half = runTo(r, 17.5 + 1.0 / 60.0, end - 1.0);
    CHECK(half.size() < full * 3 / 4);
    CHECK(half.size() > full / 16);  // most nodes are in the late generations: they go first
    CHECK(runTo(r, end - 1.0 + 1.0 / 60.0, end - 0.01).size() < full / 8);
    CHECK(r.expired(end));
}

TEST_CASE("organics: Wthr=0 dims instead, Pen 0 draws nothing, junk values stay finite") {
    reforce::scene::Environment env;
    OrganicRun dim(compileOrganic("Grow=\"60\"\nWthr=\"0\"\nPDur=\"4\"\n"), env, "d", 0.0, 1.0, 30.0, nullptr);
    const auto mid = runTo(dim, 0.0, 3.0);
    const auto late = runTo(dim, 3.0 + 1.0 / 60.0, 5.5);
    REQUIRE(!late.empty());
    CHECK(late.size() >= mid.size());  // no ungrowing
    CHECK(late.front().penA < 0.6f);   // dimmed by FADER

    OrganicRun off(compileOrganic("Pen=\"0\"\n"), env, "o", 0.0, 1.0, 30.0, nullptr);
    CHECK(runTo(off, 0.0, 2.0).empty());

    OrganicRun junk(compileOrganic("Grow=\"1/0\"\nSway=\"log(-1)\"\nScal=\"1e30\"\nX=\"0/0\"\nSeg=\"-1\"\n"
                                   "Pts=\"1e9\"\nRts=\"-4\"\nWdth=\"1/0\"\n"),
                    env, "j", 0.0, 1.0, 30.0, nullptr);
    for (const auto& s : runTo(junk, 0.0, 2.0)) {
        CHECK(std::isfinite(s.ax));
        CHECK(std::isfinite(s.radius));
    }
}

TEST_CASE("organics: every committed organic compiles cleanly, grows, and stays finite; the theme starts them") {
    namespace fs = std::filesystem;
    const fs::path content = fs::path(REFORCE_SOURCE_DIR) / "content";
    const auto organics = listScenes(content, reforce::scene::SceneType::Organic);
    REQUIRE(organics.size() >= 5);
    for (const auto& e : organics) {
        CAPTURE(e.id);
        std::ifstream in(e.path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto cfg = compileOrganic(ss.str());
        for (const auto& d : cfg->diags.items()) {
            CAPTURE(d.message);
            CHECK(false);  // no warnings or errors at all
        }
        CHECK(organicKind(*cfg).has_value());
        CHECK((cfg->find("META") && !cfg->find("META")->text.empty()));
        reforce::scene::Environment env;
        OrganicRun r(cfg, env, e.name, 0.0, 2.0, 20.0, nullptr);
        SpriteAudio a;
        a.bass = 0.7;
        a.sync.on = 1;
        std::vector<LineSegment> segs;
        const FrameMapping map = organicMapping(1920, 1080);
        std::size_t most = 0, fields = 0;
        for (double t = 0; t < r.endTime(); t += 1.0 / 30.0) {
            a.sync.beat = t * 2.0;
            a.sync.bar = t * 0.5;  // the phrase clock's bars
            a.sync.phrase = a.sync.bar / 16.0;
            segs.clear();
            r.step(t, a, map, LineStyle{}, segs);
            most = std::max(most, segs.size());
            if (const OrganicField* f = r.field()) {
                ++fields;
                REQUIRE(std::isfinite(f->pen));
                REQUIRE(std::isfinite(f->cre));
                REQUIRE(std::isfinite(f->cim));
                REQUIRE(std::isfinite(f->scale));
            }
            for (const auto& s : segs) {
                REQUIRE(std::isfinite(s.ax));
                REQUIRE(std::isfinite(s.by));
                REQUIRE(std::isfinite(s.radius));
            }
        }
        if (r.kind() == OrganicKind::Tree) {
            CHECK(r.tree().done());  // grows out within its life
            CHECK(r.tree().livePoints() < r.tree().params().pointsX.size() / 3 + 1);  // reached most of its points
            CHECK(most > 100);
        } else if (r.kind() == OrganicKind::Field) {
            CHECK(most == 0);  // drawn on the GPU (render/FieldPipeline), no line segments
            CHECK(fields > 100);
            REQUIRE(r.field());
        } else {
            CHECK(most > 10);
        }
    }
    const ThemeLibrary themes = ThemeLibrary::load(content);
    const Theme* t = themes.find("Objects");
    REQUIRE(t);
    REQUIRE(t->objectInterval);
    CHECK(*t->objectInterval > 0);
    // Every object (organics and solids) is in exactly one group (Categories/Object), and each group has an
    // "Objects: <group>" theme that admits just that group and starts objects.
    std::vector<std::string> names;
    for (const auto& e : organics) names.push_back(e.name);
    for (const auto& e : listScenes(content, reforce::scene::SceneType::Solid)) names.push_back(e.name);
    static const char* const kGroups[] = {"Trees", "Pendulums", "Curves", "Cells", "Overlay", "Fractals",
                                          "Arcade", "Solids", "Puzzles"};
    std::size_t grouped = 0;
    for (const char* g : kGroups) {
        CAPTURE(g);
        const Theme* gt = themes.find(std::string("Objects: ") + g);
        REQUIRE(gt);
        REQUIRE(gt->objectInterval);
        const auto admitted = themes.filter(gt, "Object", names);
        CHECK(admitted.size() >= 2);
        grouped += admitted.size();
        REQUIRE(themes.category("Object", g));
    }
    CHECK(grouped == names.size());
    for (const auto& n : names) {
        CAPTURE(n);
        int in = 0;
        for (const char* g : kGroups) in += themes.category("Object", g)->matches(n) ? 1 : 0;
        CHECK(in == 1);
    }
}

TEST_CASE("organics: Julia follows its keys; ripples are no longer a kind") {
    reforce::scene::Environment env;
    const FrameMapping map = organicMapping(1920, 1080);
    // Ripples were removed (Peter 2026-11-26): the kind is unknown now.
    CHECK_FALSE(organicKind(*compileOrganic("Kind=\"ripples\"\n")).has_value());
    {
        const auto cfg = compileOrganic(
            "Kind=\"julia\"\nIter=\"64\"\nCRe=\"-.4\"\nCIm=\".6\"\nScal=\"1.5\"\nBand=\"3\"\nPen=\"1\"\n");
        for (const auto& d : cfg->diags.items()) {
            CAPTURE(d.message);
            CHECK(false);
        }
        OrganicRun r(cfg, env, "j", 0.0, 0.5, 60.0, nullptr);
        SpriteAudio a;
        std::vector<LineSegment> segs;
        r.step(1.0, a, map, LineStyle{}, segs);
        const OrganicField* f = r.field();
        REQUIRE(f);
        CHECK(f->kind == OrganicField::Kind::Julia);
        CHECK(f->iter == 64);
        CHECK(f->cre == doctest::Approx(-0.4));
        CHECK(f->cim == doctest::Approx(0.6));
        CHECK(f->scale == doctest::Approx(1.5));
        CHECK(f->bands == doctest::Approx(3));
        CHECK(f->opacity == doctest::Approx(1.0));
    }
}

TEST_CASE("organics: pendulum waves line up on every cycle line of the phrase clock, drift apart between") {
    reforce::scene::Environment env;
    const auto cfg = compileOrganic("Kind=\"pendulums\"\nNum=\"10\"\nCyc=\"4\"\nAmp=\".5\"\nStrg=\"0\"\nBobW=\"6\"\n");
    for (const auto& d : cfg->diags.items()) {
        CAPTURE(d.message);
        CHECK(false);
    }
    REQUIRE(organicKind(*cfg) == OrganicKind::Pendulums);
    OrganicRun r(cfg, env, "p", 0.0, 0.5, 60.0, nullptr);
    CHECK(r.kind() == OrganicKind::Pendulums);
    const FrameMapping map = organicMapping(1920, 1080);
    auto bobs = [&](double bar) {
        SpriteAudio a;
        a.sync.on = 1;
        a.sync.bar = bar;
        std::vector<LineSegment> segs;
        r.step(10.0 + bar, a, map, LineStyle{}, segs);
        return segs;
    };
    // On a cycle line (bar 8 = 2 cycles of 4 bars) every bob is at the top of its swing: one row.
    auto line = bobs(8.0);
    REQUIRE(line.size() == 10);  // dots only (Strg 0)
    for (const auto& s : line) CHECK(s.ay == doctest::Approx(line.front().ay).epsilon(1e-4));
    CHECK(line.front().ay < 540.0f);  // above the middle (+Amp)
    // Half way through a cycle they are spread out.
    const auto mid = bobs(9.3);
    float lo = 1e9f, hi = -1e9f;
    for (const auto& s : mid) {
        lo = std::min(lo, s.ay);
        hi = std::max(hi, s.ay);
    }
    CHECK(hi - lo > 200.0f);
    // Without the beat clock they still move (120 BPM from the start).
    std::vector<LineSegment> a1, a2;
    r.step(20.0, SpriteAudio{}, map, LineStyle{}, a1);
    r.step(20.3, SpriteAudio{}, map, LineStyle{}, a2);
    REQUIRE(a1.size() == a2.size());
    CHECK(a1[3].ay != a2[3].ay);
}

TEST_CASE("organics: pendulum ring and side views") {
    reforce::scene::Environment env;
    const FrameMapping map = organicMapping(1920, 1080);
    SpriteAudio a;
    a.sync.on = 1;
    a.sync.bar = 16.0;  // a cycle line
    // Ring: on a cycle line every bob is Len / 2 + Amp from the center.
    OrganicRun ring(compileOrganic("Kind=\"pendulums\"\nView=\"2\"\nNum=\"12\"\nLen=\"1\"\nAmp=\".2\"\nStrg=\"0\"\n"), env,
                    "r", 0.0, 0.5, 60.0, nullptr);
    std::vector<LineSegment> segs;
    ring.step(5.0, a, map, LineStyle{}, segs);
    REQUIRE(segs.size() == 12);
    for (const auto& sg : segs) CHECK(std::hypot(sg.ax - 960.5, sg.ay - 540.5) == doctest::Approx(0.7 * 540).epsilon(0.01));
    // Side: strings from y = .9 (pixel 54.5) to bobs Len below at the top of the swing (angle Amp).
    OrganicRun side(compileOrganic("Kind=\"pendulums\"\nView=\"1\"\nNum=\"5\"\nLen=\"1\"\nAmp=\".3\"\n"), env, "s",
                    0.0, 0.5, 60.0, nullptr);
    segs.clear();
    side.step(5.0, a, map, LineStyle{}, segs);
    REQUIRE(segs.size() == 10);  // string + bob each
    CHECK(segs[0].ay == doctest::Approx(0.05 * 1080 + 0.5).epsilon(1e-3));
    CHECK(std::hypot(segs[0].bx - segs[0].ax, segs[0].by - segs[0].ay) == doctest::Approx(540).epsilon(0.01));
}

TEST_CASE("PBar: holds in bars end on bar lines, whole phrases on phrase lines; fadingOut") {
    reforce::scene::SyncInput s;
    // No beat clock: bars at 120 BPM (2 s a bar), whatever the fade.
    CHECK(holdForBars(8, 3, s, 16) == doctest::Approx(16.0));
    CHECK(std::isnan(holdForBars(NAN, 3, s, 16)));
    CHECK(std::isnan(holdForBars(0, 3, s, 16)));
    CHECK(std::isnan(holdForBars(-2, 3, s, 16)));
    // 128 BPM, 1.875 s a bar, started at bar 10.3 with a 4 s fade-in.
    s.on = 1;
    s.bpm = 128;
    s.bar = 10.3;
    const double bar = 240.0 / 128;
    auto endBar = [&](double hold, double fade) { return s.bar + (fade + hold) / bar; };
    const double h8 = holdForBars(8, 4, s, 16);
    CHECK(endBar(h8, 4) == doctest::Approx(std::round(endBar(h8, 4))));  // on a bar line
    CHECK(std::abs(endBar(h8, 4) - (s.bar + 4 / bar + 8)) <= 0.5 + 1e-9);  // the nearest one
    // 32 bars = 2 phrases: the fade-out starts on a phrase line (a multiple of 16 on SYNC_BAR).
    const double h32 = holdForBars(32, 4, s, 16);
    CHECK(std::fmod(endBar(h32, 4) + 1e-9, 16.0) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(h32 >= 16 * bar);
    // Very short: never below half of it; 8-bar phrases; 3 beats to the bar.
    CHECK(holdForBars(1, 0, s, 16) >= 0.5 * bar);
    const double h16 = holdForBars(16, 4, s, 8);
    CHECK(std::fmod(endBar(h16, 4) + 1e-9, 8.0) == doctest::Approx(0.0).epsilon(1e-6));
    const double h3 = holdForBars(4, 0, s, 16, 3.0);
    CHECK(h3 == doctest::Approx((14 - 10.3) * 3 * 60 / 128.0));

    FadeEnvelope e;
    e.fade = 2;
    e.hold = 10;
    CHECK(!e.fadingOut(1));
    CHECK(!e.fadingOut(11.9));
    CHECK(e.fadingOut(12.1));
    e.fadeOutAt(5, 1);
    CHECK(!e.fadingOut(4.9));
    CHECK(e.fadingOut(5.0));
}

// ------------------------------------------------------------ trees on the beat (strikes, beat sway, vibration, pulses)

namespace {

std::shared_ptr<const reforce::scene::CompiledConfig> committedOrganic(const std::string& name) {
    const std::filesystem::path p =
        std::filesystem::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Organic.package" / (name + ".txt");
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    auto cfg = compileOrganic(ss.str());
    for (const auto& d : cfg->diags.items()) {
        CAPTURE(d.message);
        CHECK(false);
    }
    return cfg;
}

// One frame at `t` with the beat clock locked at 120 BPM, beat 0 (and bar 0) at t = 0.
std::vector<LineSegment> clockStep(OrganicRun& r, double t, double drop = 0, double bass = 0) {
    SpriteAudio a;
    a.bass = bass;
    a.sync.on = 1;
    a.sync.bpm = 120;
    a.sync.beat = 2.0 * t;
    a.sync.bar = 0.5 * t;
    a.sync.phrase = a.sync.bar / 16.0;
    a.sync.drop = drop;
    std::vector<LineSegment> segs;
    r.step(t, a, organicMapping(1920, 1080), LineStyle{}, segs);
    return segs;
}

struct Strikes {
    std::vector<double> at, xs;
    std::vector<std::size_t> sizes;
};

Strikes runStrikes(const std::shared_ptr<const reforce::scene::CompiledConfig>& cfg, std::uint64_t seed) {
    reforce::scene::Environment env;
    env.seed = seed;
    OrganicRun r(cfg, env, "Lightning", 0.3, 4.0, 40.0, nullptr);  // starts at beat .6
    Strikes out;
    int last = -2;
    std::size_t most = 0;
    for (double t = 0.3; t < r.endTime(); t += 1.0 / 60.0) {
        const auto segs = clockStep(r, t);
        if (r.strike() != last) {
            if (last >= 0) out.sizes.push_back(most);
            last = r.strike();
            most = 0;
            if (last >= 0) {
                out.at.push_back(2.0 * t);  // beat of the first frame drawn
                out.xs.push_back(r.tree().nodes().front().x);
                CHECK(r.tree().nodes().front().y == doctest::Approx(1.0));  // the top edge
            }
        }
        if (r.strike() < 0) CHECK(segs.empty());  // nothing before the first beat line
        most = std::max(most, segs.size());
    }
    out.sizes.push_back(most);
    return out;
}

}  // namespace

TEST_CASE("trees: Lightning strikes on every bar line for 8 bars, each from a new point on the top edge") {
    const auto cfg = committedOrganic("Lightning");
    const Strikes s = runStrikes(cfg, 5);
    REQUIRE(s.at.size() == 8);
    for (std::size_t k = 0; k < s.at.size(); ++k) {
        CAPTURE(k);
        // Bar lines (beats 4, 8, ..., 32: the first bar line after the start at beat .6, then every SGap = 4), within
        // a frame.
        CHECK(s.at[k] >= 4.0 + 4.0 * double(k) - 1e-9);
        CHECK(s.at[k] < 4.0 + 4.0 * double(k) + 2.0 / 60.0 + 1e-9);
        CHECK(std::abs(s.xs[k]) <= 1.3 + 1e-6);
        CHECK(s.sizes[k] > 100);  // each grows a full bolt
        for (std::size_t j = 0; j < k; ++j) CHECK(std::abs(s.xs[k] - s.xs[j]) > 1e-3);  // a different place
    }
    // Deterministic per seed; another seed strikes elsewhere.
    const Strikes again = runStrikes(cfg, 5);
    CHECK(again.xs == s.xs);
    CHECK(again.sizes == s.sizes);
    CHECK(runStrikes(cfg, 6).xs != s.xs);

    // Each strike flashes once it lands and fades out before the next.
    reforce::scene::Environment env;
    OrganicRun r(cfg, env, "Lightning", 0.0, 4.0, 40.0, nullptr);
    float lead = 0, flash = 0, late = 0;
    for (double t = 0.0; t < 2.0; t += 1.0 / 60.0) {
        const auto segs = clockStep(r, t);
        float m = 0;
        for (const auto& sg : segs) m = std::max(m, sg.penA);
        const double beat = 2.0 * t;
        if (beat > 0.2 && beat < 0.5) lead = std::max(lead, m);
        if (beat > 0.6 && beat < 0.8) flash = std::max(flash, m);
        if (beat > 3.5) late = std::max(late, m);
    }
    CHECK(r.strike() == 0);
    CHECK(flash > lead);
    CHECK(late < 0.05f);
}

TEST_CASE("trees: strikes survive a jumping clock and junk keys") {
    reforce::scene::Environment env;
    OrganicRun r(compileOrganic("Strk=\"3\"\nSGap=\"2\"\nGrow=\"100\"\nPts=\"60\"\n"), env, "s", 0.0, 1.0, 30.0,
                 nullptr);
    SpriteAudio a;
    a.sync.on = 1;
    std::vector<LineSegment> segs;
    const FrameMapping map = organicMapping(1920, 1080);
    int most = -1;
    for (int i = 0; i < 1200; ++i) {
        const double t = i / 60.0;
        a.sync.beat = i < 400 ? 2.0 * t : i < 700 ? 1000.0 + 2.0 * t : -50.0 + t;  // forwards, then back
        segs.clear();
        r.step(t, a, map, LineStyle{}, segs);
        most = std::max(most, r.strike());
        CHECK(r.strike() <= 2);
        for (const auto& s : segs) REQUIRE(std::isfinite(s.ax));
    }
    CHECK(most == 2);

    OrganicRun junk(compileOrganic("Strk=\"1e9\"\nSGap=\"0/0\"\nSwyB=\"-1\"\nVib=\"1/0\"\nVibF=\"1/0\"\n"
                                   "Puls=\"1/0\"\nPlsB=\"0\"\nPlsE=\"-3\"\nPAll=\"0/0\"\nPen=\"s\"\nGrow=\"60\"\n"),
                    env, "j", 0.0, 1.0, 20.0, nullptr);
    for (double t = 0; t < 12.0; t += 1.0 / 30.0)
        for (const auto& s : clockStep(junk, t)) {
            REQUIRE(std::isfinite(s.ax));
            REQUIRE(std::isfinite(s.by));
            REQUIRE(std::isfinite(s.radius));
            REQUIRE(std::isfinite(s.penA));
        }
    CHECK(junk.strike() <= 15);
}

TEST_CASE("trees: Oak sways back and forth over 4 beats about its fixed base") {
    reforce::scene::Environment env;
    OrganicRun r(committedOrganic("Oak"), env, "Oak", 0.0, 4.0, 40.0, nullptr);
    for (double t = 0; t < 25.0; t += 1.0 / 60.0) clockStep(r, t);
    REQUIRE(r.tree().done());
    // The branches' mean x (leaves, hard-edged, left out: they flutter) at beat lines.
    float bx0 = 0, by0 = 0;
    auto meanX = [&](double beat, bool base = false) {
        const auto segs = clockStep(r, beat / 2.0);
        REQUIRE(!segs.empty());
        if (base) {
            bx0 = segs.front().ax;
            by0 = segs.front().ay;
        } else {
            CHECK(segs.front().ax == bx0);  // the base stays put
            CHECK(segs.front().ay == by0);
        }
        double sx = 0;
        int k = 0;
        for (const auto& s : segs)
            if (s.radius > 0) {
                sx += s.bx;
                ++k;
            }
        REQUIRE(k > 100);
        return sx / double(k);
    };
    // Extremes on the bar lines (beats 52 and 56, bars 13 and 14), a period of 2 bars, the middle half way.
    const double m52 = meanX(52.0, true);
    const double m54 = meanX(54.0);
    const double m56 = meanX(56.0);
    const double m58 = meanX(58.0);
    const double m60 = meanX(60.0);
    CHECK(std::abs(m52 - m56) > 8.0);      // over to one side on one bar line, the other side on the next
    CHECK(std::abs(m52 - m60) < 0.5);      // a period of 8 beats
    CHECK(std::abs(m54 - m58) > 4.0);
    CHECK((m52 - m54) * (m54 - m56) > 0);  // passing through the middle between
    CHECK(std::abs(m54 - 0.5 * (m52 + m56)) < 0.25 * std::abs(m52 - m56));
    CHECK(std::abs(m52 - m56) < 120.0);    // natural, not falling over
}

namespace {

std::size_t leafSegments(const std::vector<LineSegment>& segs) {
    std::size_t n = 0;
    for (const auto& s : segs) n += s.radius < 0 ? 1 : 0;  // leaves are the hard-edged segments
    return n;
}

float biggestLeaf(const std::vector<LineSegment>& segs) {
    float m = 0;
    for (const auto& s : segs) m = std::max(m, -s.radius);
    return m;
}

struct LeafRun {
    std::size_t most = 0;
    std::vector<std::size_t> leaves;  // leaf segments every 2 s
    std::vector<std::vector<LineSegment>> frames;  // at 6, 12, 20 s
};

LeafRun runLeaves(const std::shared_ptr<const reforce::scene::CompiledConfig>& cfg, std::uint64_t seed) {
    reforce::scene::Environment env;
    env.seed = seed;
    OrganicRun r(cfg, env, "Oak", 0.0, 4.0, 30.0, nullptr);
    LeafRun out;
    for (int i = 0; i < 60 * 40; ++i) {
        const auto segs = clockStep(r, i / 60.0);
        out.most = std::max(out.most, segs.size());
        for (const auto& s : segs) {
            REQUIRE(std::isfinite(s.ax));
            REQUIRE(std::isfinite(s.radius));
        }
        if (i % 120 == 0) out.leaves.push_back(leafSegments(segs));
        if (i == 60 * 6 || i == 60 * 12 || i == 60 * 20) out.frames.push_back(segs);
    }
    return out;
}

}  // namespace

TEST_CASE("trees: leaves unfurl on the outer twigs, bounded and deterministic; without Leaf nothing changes") {
    const auto oak = committedOrganic("Oak");
    const LeafRun lr = runLeaves(oak, 3);
    CHECK(lr.most < 4000);             // bounded
    CHECK(lr.leaves.front() == 0);     // none on the bare trunk
    CHECK(lr.leaves[2] == 0);          // 4 s: still a trunk
    std::size_t full = 0;
    for (auto n : lr.leaves) full = std::max(full, n);
    CHECK(full > 200);                 // leafed out (two segments a leaf)
    CHECK(lr.leaves.back() < full / 4);  // fallen by the end of the wither
    REQUIRE(lr.frames.size() == 3);
    CHECK(leafSegments(lr.frames[1]) > leafSegments(lr.frames[0]));  // leafing out with the growth

    // The first leaves start small and reach full size over LeafU (4) beats.
    {
        reforce::scene::Environment env;
        OrganicRun g(oak, env, "Oak", 0.0, 4.0, 30.0, nullptr);
        double first = -1;
        float early = 0, later = 0;
        for (double t = 0; t < 25.0; t += 1.0 / 60.0) {
            const float b = biggestLeaf(clockStep(g, t));
            if (first < 0 && b > 0) first = t;
            if (first >= 0 && t - first < 0.1) early = std::max(early, b);
            if (first >= 0 && t - first > 2.2 && t - first < 2.4) later = std::max(later, b);
        }
        REQUIRE(first > 0);
        CHECK(early < 0.5f * later);
    }
    // Deterministic per seed.
    const LeafRun again = runLeaves(oak, 3);
    CHECK(again.most == lr.most);
    CHECK(again.leaves == lr.leaves);

    // Off by default: the leaf keys without Leaf (or Leaf 0) change nothing.
    const std::string base = "Grow=\"30\"\nSway=\".2\"\nPts=\"200\"\nPDur=\"8\"\n";
    reforce::scene::Environment env;
    OrganicRun a(compileOrganic(base), env, "a", 0.0, 1.0, 30.0, nullptr);
    OrganicRun b(compileOrganic(base + "LeafW=\".1\"\nLeafD=\"1\"\nLeafA=\"1\"\nLeafU=\"2\"\nLFlt=\".3\"\n"), env, "b",
                 0.0, 1.0, 30.0, nullptr);
    OrganicRun c(compileOrganic(base + "Leaf=\"0\"\n"), env, "c", 0.0, 1.0, 30.0, nullptr);
    for (double t = 0; t < 10.0; t += 1.0 / 30.0) {
        const auto sa = clockStep(a, t), sb = clockStep(b, t), sc = clockStep(c, t);
        REQUIRE(sa.size() == sb.size());
        REQUIRE(sa.size() == sc.size());
        for (std::size_t i = 0; i < sa.size(); ++i) {
            CHECK(sa[i].bx == sb[i].bx);
            CHECK(sa[i].radius == sb[i].radius);
            CHECK(sa[i].bx == sc[i].bx);
        }
    }
    // Junk leaf keys stay finite.
    OrganicRun j(compileOrganic(base + "Leaf=\"1/0\"\nLeafW=\"1e30\"\nLeafD=\"0/0\"\nLeafA=\"log(-1)\"\n"
                                       "LeafU=\"-1\"\nLFlt=\"1/0\"\n"),
                 env, "j", 0.0, 1.0, 30.0, nullptr);
    for (double t = 0; t < 10.0; t += 1.0 / 30.0)
        for (const auto& s : clockStep(j, t)) {
            REQUIRE(std::isfinite(s.ax));
            REQUIRE(std::isfinite(s.by));
            REQUIRE(std::isfinite(s.radius));
        }
}

TEST_CASE("trees: Radial Bloom vibrates outwards on every beat, harder on the bar line") {
    reforce::scene::Environment env;
    OrganicRun r(committedOrganic("Radial Bloom"), env, "Radial Bloom", 0.0, 4.0, 40.0, nullptr);
    for (double t = 0; t < 20.0; t += 1.0 / 60.0) clockStep(r, t);
    REQUIRE(r.tree().done());
    auto meanR = [&](double beat, double drop = 0) {
        const auto segs = clockStep(r, beat / 2.0, drop);
        REQUIRE(!segs.empty());
        double sr = 0;
        for (const auto& s : segs) sr += std::hypot(s.bx - 960.0, s.by - 540.0);
        return sr / double(segs.size());
    };
    const double rest = meanR(43.95), bar = meanR(44.0);    // beat 44: a bar line
    const double rest2 = meanR(44.95), beat = meanR(45.0);  // beat 45: not a bar line
    const double rest3 = meanR(45.95);
    CHECK(bar > rest * 1.08);    // pushed out on the bar line
    CHECK(beat > rest2 * 1.04);  // and on every beat
    CHECK(bar - rest > 1.5 * (beat - rest2));
    CHECK(std::abs(rest3 - rest2) < 0.01 * rest2);  // still again by the end of the beat
    // Rings within the beat: in and out again before it dies away.
    bool inward = false;
    for (double b = 46.0; b < 46.5; b += 1.0 / 120.0) inward = inward || meanR(b) < rest3 * 0.99;
    CHECK(inward);
    const double dropped = meanR(47.0, 1.0);
    CHECK(dropped - rest3 > 1.5 * (beat - rest2));  // a drop rings harder
}

TEST_CASE("trees: Neurons keep firing after they are grown: a pulse a beat, a drop fires them all") {
    reforce::scene::Environment env;
    OrganicRun r(committedOrganic("Neurons"), env, "Neurons", 0.0, 4.0, 40.0, nullptr);
    for (double t = 0; t < 30.0; t += 1.0 / 60.0) clockStep(r, t);
    REQUIRE(r.tree().done());
    auto bright = [](const std::vector<LineSegment>& segs) {
        std::size_t n = 0;
        for (const auto& s : segs) n += s.penA > 0.8f ? 1 : 0;
        return n;
    };
    // Every beat a pulse runs out: the bright segments move; the picture differs from beat to beat.
    std::vector<std::vector<LineSegment>> frames;
    for (double b = 60.3; b < 64.0; b += 1.0) {
        frames.push_back(clockStep(r, b / 2.0));
        CHECK(bright(frames.back()) > 5);
        CHECK(r.livePulses() >= 1);
        CHECK(r.livePulses() <= 4);
    }
    for (std::size_t k = 1; k < frames.size(); ++k) {
        bool differs = frames[k].size() != frames[k - 1].size();
        for (std::size_t i = 0; !differs && i < frames[k].size(); ++i)
            differs = frames[k][i].penA != frames[k - 1][i].penA || frames[k][i].radius != frames[k - 1][i].radius;
        CHECK(differs);
    }
    // The pulse runs outwards over the beat: more is lit half way than just after it fired.
    // (beat 64 is a phrase line, where they all fire: look at beat 65.)
    for (double t = 64.0 / 2.0; t < 65.0 / 2.0; t += 1.0 / 60.0) clockStep(r, t);
    const std::size_t early = bright(clockStep(r, 65.1 / 2.0));
    const std::size_t mid = bright(clockStep(r, 65.5 / 2.0));
    CHECK(mid > early);
    // A drop fires every neuron at once.
    const std::size_t before = r.livePulses();
    clockStep(r, 65.6 / 2.0, 1.0);
    CHECK(r.livePulses() >= before + 5);
    const std::size_t all = bright(clockStep(r, 66.0 / 2.0, 0.9));
    CHECK(all > 2 * mid);
}

TEST_CASE("organics: the Clifford attractor never collapses to a few dots (it seemed to blink off)") {
    // Over its whole parameter glide (bars 0..3360, the periods' common multiple), every frame covers a real cloud:
    // collapsed parameters are replaced by the last full ones.
    std::ifstream in(std::filesystem::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Organic.package/Clifford Attractor.txt");
    std::stringstream ss;
    ss << in.rdbuf();
    const auto cfg = compileOrganic(ss.str());
    reforce::scene::Environment env;
    OrganicRun r(cfg, env, "clifford", 0.0, 0.0, 1e6, nullptr);
    const FrameMapping map = organicMapping(1920, 1080);
    SpriteAudio a;
    a.sync.on = 1;
    int worst = 1 << 30;
    for (double bar = 0; bar < 3360; bar += 0.5) {
        a.sync.bar = bar;
        a.sync.beat = bar * 4;
        std::vector<LineSegment> segs;
        r.step(10.0 + bar, a, map, LineStyle{}, segs);
        std::vector<int> cells;
        for (const auto& s : segs) cells.push_back(int(s.ax) / 8 * 1000 + int(s.ay) / 8);
        std::sort(cells.begin(), cells.end());
        const int n = int(std::unique(cells.begin(), cells.end()) - cells.begin());
        worst = std::min(worst, n);
    }
    MESSAGE("Clifford: at least " << worst << " 8 px cells covered in every frame");
    CHECK(worst > 300);
}

TEST_CASE("trees: prune removes whole sub-branches but never roots; added points grow new branches") {
    TreeGrowth g(dome(300), 4);
    growAll(g);
    REQUIRE(g.done());
    const auto before = g.nodes();
    const int rev = g.revision();
    // Remove a side branch: the first node (past the trunk) whose parent branches.
    std::vector<char> remove(before.size(), 0);
    int cut = -1;
    for (std::size_t i = 1; i < before.size() && cut < 0; ++i)
        if (before[std::size_t(before[i].parent)].children >= 2 && before[i].tips >= 3) cut = int(i);
    REQUIRE(cut > 0);
    remove[0] = 1;  // the root: ignored
    remove[std::size_t(cut)] = 1;
    const auto map = g.prune(remove);
    CHECK(g.revision() != rev);
    CHECK(map[0] == 0);
    CHECK(map[std::size_t(cut)] == -1);
    const auto& after = g.nodes();
    CHECK(after.size() < before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        // Descendants of the cut are gone, the rest kept in order with the same positions.
        bool under = false;
        for (int a = int(i); a >= 0; a = before[std::size_t(a)].parent) under = under || a == cut;
        CHECK((map[i] < 0) == under);
        if (map[i] >= 0) {
            CHECK(after[std::size_t(map[i])].x == before[i].x);
            CHECK(after[std::size_t(map[i])].depth == before[i].depth);
        }
    }
    int children = 0;
    for (const auto& nd : after) {
        CHECK(nd.parent < int(&nd - after.data()));
        if (nd.parent >= 0) CHECK(after[std::size_t(nd.parent)].depth == nd.depth - 1);
        children += nd.children;
    }
    CHECK(children == int(after.size()) - 1);  // one root
    // New points where the branch was: it grows back.
    std::vector<double> xs, ys;
    for (std::size_t i = 0; i < before.size(); ++i)
        if (map[i] < 0) {
            xs.push_back(before[i].x + 0.01);
            ys.push_back(before[i].y);
        }
    CHECK(g.addPoints(xs, ys) > 0);
    CHECK_FALSE(g.done());
    const std::size_t pruned = after.size();
    growAll(g);
    CHECK(g.nodes().size() > pruned + 3);
    // Junk points are ignored.
    std::vector<double> jx = {std::nan(""), 1e9, 0.0}, jy = {0.0, 0.0, std::nan("")};
    CHECK(g.addPoints(jx, jy) == 0);
}

TEST_CASE("trees: Neurons follow bars and phrases: a stronger lead on every bar line, all together on the phrase "
          "line, and the network rewires over the phrase while the cell bodies stay put") {
    const auto cfg = committedOrganic("Neurons");
    reforce::scene::Environment env;
    env.seed = 9;
    OrganicRun r(cfg, env, "Neurons", 0.0, 4.0, 40.0, nullptr);
    for (double t = 0; t < 29.0; t += 1.0 / 60.0) clockStep(r, t);  // to beat 58
    REQUIRE(r.tree().done());
    auto firedAt = [&](double beat) {
        std::vector<OrganicRun::Pulse> out;
        for (const auto& p : r.pulses())
            if (std::abs(p.beat - beat) < 1e-6) out.push_back(p);
        return out;
    };
    auto maxRadius = [](const std::vector<LineSegment>& segs) {
        float m = 0;
        for (const auto& s : segs) m = std::max(m, s.radius);
        return m;
    };
    // Bar lines at beats 60 and 68 (bars 15, 17): one lead pulse each, 1.7 strong, different leads; plain beats 1.
    float barR = 0, beatR = 0;
    std::vector<OrganicRun::Pulse> at60, at61, at62;
    for (double t = 29.0; t < 31.95; t += 1.0 / 60.0) {
        const auto segs = clockStep(r, t);
        const double b = 2.0 * t;
        if (b > 60.3 && b < 60.4) barR = std::max(barR, maxRadius(segs));
        if (b > 61.3 && b < 61.4) beatR = std::max(beatR, maxRadius(segs));
        if (b > 60.2 && b < 60.25) at60 = firedAt(60.0);
        if (b > 61.2 && b < 61.25) at61 = firedAt(61.0);
        if (b > 62.2 && b < 62.25) at62 = firedAt(62.0);
    }
    {
        REQUIRE(at60.size() == 1);
        CHECK(at60[0].strength == doctest::Approx(1.7));
        REQUIRE(at61.size() == 1);
        CHECK(at61[0].strength == doctest::Approx(1.0));
        REQUIRE(at62.size() == 1);
        CHECK(barR > 1.2f * beatR);  // the lead's front is thicker
    }
    // The cell bodies (the roots) and the network just before the phrase line (beat 64 = bar 16).
    std::vector<std::pair<float, float>> bodies, network;
    const std::size_t roots = r.tree().params().roots.size();
    for (std::size_t i = 0; i < roots; ++i) bodies.push_back({r.tree().nodes()[i].x, r.tree().nodes()[i].y});
    for (const auto& nd : r.tree().nodes()) network.push_back({nd.x, nd.y});
    const std::size_t grownSegs = clockStep(r, 31.98).size();
    // The phrase line: every neuron at once, at strength 1, and the rewiring starts.
    clockStep(r, 32.0 + 1.0 / 120.0);
    {
        const auto at64 = firedAt(64.0);
        CHECK(at64.size() == roots);
        for (const auto& p : at64) CHECK(p.strength == doctest::Approx(1.0));
    }
    CHECK(r.rewiring());
    // Half way through the 4 withering bars fewer segments are drawn.
    std::size_t mid = 0;
    for (double t = 32.0 + 1.0 / 60.0; t < 36.0; t += 1.0 / 60.0) mid = clockStep(r, t).size();
    CHECK(mid < grownSegs * 95 / 100);
    // Through the phrase: withered, pruned, regrown.
    for (double t = 36.0; t < 60.0; t += 1.0 / 60.0) {
        clockStep(r, t);
        REQUIRE(r.tree().nodes().size() <= 6000);
    }
    CHECK_FALSE(r.rewiring());
    for (std::size_t i = 0; i < roots; ++i) {
        CHECK(r.tree().nodes()[i].x == bodies[i].first);  // the cell bodies stay put
        CHECK(r.tree().nodes()[i].y == bodies[i].second);
    }
    std::sort(network.begin(), network.end());
    std::size_t gone = 0, fresh = 0;
    for (const auto& nd : r.tree().nodes())
        fresh += std::binary_search(network.begin(), network.end(), std::make_pair(nd.x, nd.y)) ? 0 : 1;
    std::vector<std::pair<float, float>> now;
    for (const auto& nd : r.tree().nodes()) now.push_back({nd.x, nd.y});
    std::sort(now.begin(), now.end());
    for (const auto& p : network) gone += std::binary_search(now.begin(), now.end(), p) ? 0 : 1;
    CHECK(gone > network.size() / 6);  // a different network: branches gone ...
    CHECK(fresh > network.size() / 10);  // ... and new ones grown
    CHECK(r.tree().nodes().size() > network.size() / 2);
    // Deterministic: the same seed rewires the same way.
    OrganicRun again(cfg, env, "Neurons", 0.0, 4.0, 40.0, nullptr);
    for (double t = 0; t < 60.0; t += 1.0 / 60.0) clockStep(again, t);
    REQUIRE(again.tree().nodes().size() == r.tree().nodes().size());
    for (std::size_t i = 0; i < r.tree().nodes().size(); ++i) CHECK(again.tree().nodes()[i].x == r.tree().nodes()[i].x);
}

TEST_CASE("trees: bar leads take turns, rewiring stays bounded over many phrases and is off by default") {
    reforce::scene::Environment env;
    // Leads: consecutive bar lines start from different neurons.
    {
        OrganicRun r(committedOrganic("Neurons"), env, "Neurons", 0.0, 4.0, 400.0, nullptr);
        std::map<long, int> leads;  // bar -> lead root
        for (double t = 0; t < 40.0; t += 1.0 / 60.0) {
            clockStep(r, t);
            for (const auto& p : r.pulses())
                if (p.strength > 1.5) leads[std::lround(p.beat / 4.0)] = p.root;
        }
        REQUIRE(leads.size() >= 10);
        for (const auto& [bar, root] : leads)
            if (leads.count(bar - 1)) CHECK(root != leads[bar - 1]);
    }
    // Many phrases with a 4000-node cap: nodes and points stay bounded, everything finite.
    {
        OrganicRun r(committedOrganic("Neurons"), env, "Neurons", 0.0, 4.0, 600.0, nullptr, 4000);
        std::size_t most = 0;
        for (double t = 0; t < 320.0; t += 1.0 / 30.0) {
            const auto segs = clockStep(r, t);
            most = std::max(most, segs.size());
            for (const auto& s : segs) REQUIRE(std::isfinite(s.ax));
            REQUIRE(r.tree().nodes().size() <= 4000);
        }
        CHECK(most < 12000);
        CHECK(r.tree().params().pointsX.size() < 600 + 10 * 200);
    }
    // Without Rwir / PlsL / PlsP nothing rewires and no pulse is stronger than 1 (except a drop).
    {
        const auto cfg = compileOrganic("Rts=\"3\"\nGrow=\"40\"\nPuls=\"1\"\nPen=\".5\"\nPDur=\"200\"\n");
        OrganicRun r(cfg, env, "p", 0.0, 1.0, 200.0, nullptr);
        std::size_t nodes = 0;
        for (double t = 0; t < 70.0; t += 1.0 / 30.0) {
            clockStep(r, t);
            CHECK_FALSE(r.rewiring());
            for (const auto& p : r.pulses()) CHECK(p.strength == 1.0);
            if (t > 30.0 && nodes == 0) nodes = r.tree().nodes().size();
        }
        CHECK(r.tree().nodes().size() == nodes);  // no regrowth
    }
}
