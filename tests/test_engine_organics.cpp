// Organics (PLAN 5N, docs/organics-design.md): tree growth by space colonisation.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
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
    // The trunk grows up from the bottom centre of the frame (y = -1 -> pixel row 1080).
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
        std::size_t most = 0, drops = 0, fields = 0;
        for (double t = 0; t < r.endTime(); t += 1.0 / 30.0) {
            a.sync.beat = t * 2.0;
            a.sync.bar = t * 0.5;  // the phrase clock's bars
            a.sync.phrase = a.sync.bar / 16.0;
            segs.clear();
            r.step(t, a, map, LineStyle{}, segs);
            most = std::max(most, segs.size());
            if (const OrganicField* f = r.field()) {
                ++fields;
                drops += f->drops.size();
                REQUIRE(std::isfinite(f->pen));
                REQUIRE(std::isfinite(f->cre));
                REQUIRE(std::isfinite(f->cim));
                REQUIRE(std::isfinite(f->scale));
                for (const auto& d : f->drops) {
                    CHECK(d.x >= 0.0);
                    CHECK(d.x <= 1.0);
                    CHECK(d.y >= 0.0);
                    CHECK(d.y <= 1.0);
                }
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
            const OrganicField* f = r.field();
            REQUIRE(f);
            if (f->kind == OrganicField::Kind::Ripples) CHECK(drops > 10);  // drops on the beat over its life
        } else {
            CHECK(most > 10);
        }
    }
    const ThemeLibrary themes = ThemeLibrary::load(content);
    const Theme* t = themes.find("Organics");
    REQUIRE(t);
    REQUIRE(t->organicInterval);
    CHECK(*t->organicInterval > 0);
    // Every organic is in exactly one group (Categories/Organic), and each group has an "Organics: <group>" theme
    // that admits just that group and starts organics.
    std::vector<std::string> names;
    for (const auto& e : organics) names.push_back(e.name);
    std::size_t grouped = 0;
    for (const char* g : {"Trees", "Pendulums", "Spirals", "Curves", "Cells", "Water", "Fractals"}) {
        CAPTURE(g);
        const Theme* gt = themes.find(std::string("Organics: ") + g);
        REQUIRE(gt);
        REQUIRE(gt->organicInterval);
        const auto admitted = themes.filter(gt, "Organic", names);
        CHECK(admitted.size() >= 2);
        grouped += admitted.size();
        REQUIRE(themes.category("Organic", g));
    }
    CHECK(grouped == organics.size());
    for (const auto& n : names) {
        CAPTURE(n);
        int in = 0;
        for (const char* g : {"Trees", "Pendulums", "Spirals", "Curves", "Cells", "Water", "Fractals"})
            in += themes.category("Organic", g)->matches(n) ? 1 : 0;
        CHECK(in == 1);
    }
}

TEST_CASE("organics: ripples drop on the beat lines (Rate per beat, DX / DY placed), Julia follows its keys") {
    reforce::scene::Environment env;
    const FrameMapping map = organicMapping(1920, 1080);
    {
        const auto cfg = compileOrganic(
            "Kind=\"ripples\"\nRate=\"2\"\nRad=\".05\"\nDX=\".5\"\nDY=\"-.5\"\nAmp=\".7\"\nPen=\".8\"\n");
        for (const auto& d : cfg->diags.items()) {
            CAPTURE(d.message);
            CHECK(false);
        }
        REQUIRE(organicKind(*cfg) == OrganicKind::Field);
        OrganicRun r(cfg, env, "r", 0.0, 0.5, 60.0, nullptr);
        CHECK(r.kindName() == "ripples");
        SpriteAudio a;
        a.sync.on = 1;
        std::vector<LineSegment> segs;
        std::size_t drops = 0;
        bool first = true;
        for (int i = 0; i <= 240; ++i) {  // 4 s at 120 BPM: 8 beats, 16 drops at Rate 2 (plus the one at once)
            const double t = i / 60.0;
            a.sync.beat = 0.25 + t * 2.0;
            r.step(t, a, map, LineStyle{}, segs);
            const OrganicField* f = r.field();
            REQUIRE(f);
            CHECK(f->reset == first);
            first = false;
            CHECK(f->pen == doctest::Approx(0.8));
            for (const auto& d : f->drops) {
                // DX .5, DY -.5 (tree units) -> frame UV: x .5 + .25 / (16/9), y .75
                CHECK(d.x == doctest::Approx(0.5 + 0.25 / (1920.0 / 1080.0)));
                CHECK(d.y == doctest::Approx(0.75));
                CHECK(d.radius == doctest::Approx(0.05));
                CHECK(d.amp == doctest::Approx(0.7));
            }
            drops += f->drops.size();
        }
        CHECK(segs.empty());
        CHECK(drops == 17);
    }
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
    // Ring: on a cycle line every bob is Len / 2 + Amp from the centre.
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
