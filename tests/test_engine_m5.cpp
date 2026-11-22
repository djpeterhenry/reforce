// M5: transitions, blend modes, scheduler, themes, fixed tick, commands.
// Probe numbers from docs/probe-results.md; scene/theme data written here.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "engine/Blend.hpp"
#include "engine/Commands.hpp"
#include "engine/FixedStep.hpp"
#include "engine/SceneLibrary.hpp"
#include "engine/Scheduler.hpp"
#include "engine/Themes.hpp"
#include "engine/Transition.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- transition function

TEST_CASE("transition function: G-Force curves") {
    const TransitionFunction cm("1-(1-t)^1.5");
    CHECK(cm.error().empty());
    CHECK(cm(0) == doctest::Approx(0));
    CHECK(cm(1) == doctest::Approx(1));
    // Probe #9: 25% of the ColorMap transition -> w = .349 measured, fn .350.
    CHECK(cm(0.25) == doctest::Approx(0.3505).epsilon(0.002));
    CHECK(cm(0.5) == doctest::Approx(0.6464).epsilon(0.002));
    // Clamped input and output.
    CHECK(cm(-1) == doctest::Approx(0));
    CHECK(cm(2) == doctest::Approx(1));
    const TransitionFunction ws("1-(1-t)^1.6");
    CHECK(ws.inverse(ws(0.3)) == doctest::Approx(0.3).epsilon(1e-4));
    // Broken expression -> identity, with the reason.
    const TransitionFunction bad("1-(1-t");
    CHECK(!bad.error().empty());
    CHECK(bad(0.4) == doctest::Approx(0.4));
    const TransitionFunction over("2*t");  // results clamp to [0,1]
    CHECK(over(0.75) == doctest::Approx(1));
}

TEST_CASE("transition clock: progress, crossfader hold and release") {
    auto fn = std::make_shared<TransitionFunction>("t^2");
    TransitionClock c(10.0, 4.0, fn);
    CHECK(c.progress(12.0) == doctest::Approx(0.5));
    CHECK(c.weight(12.0) == doctest::Approx(0.25));
    CHECK(!c.done(13.9));
    CHECK(c.done(14.0));
    c.hold(0.64);
    CHECK(c.weight(20.0) == doctest::Approx(0.64));
    CHECK(!c.done(100.0));  // held: never completes on its own
    c.release(20.0);        // resumes from progress .8 (0.8^2 = .64)
    CHECK(c.weight(20.0) == doctest::Approx(0.64).epsilon(1e-4));
    CHECK(c.progress(20.4) == doctest::Approx(0.9).epsilon(1e-3));
}

// ---------------------------------------------------------------- scheduler

TEST_CASE("scheduler: interval and duration statistics (relative deviation)") {
    LayerPrefs p;
    p.slideshowInterval = 15;
    p.slideshowDeviation = 0.1;
    p.transitionDuration = 10;
    p.transitionDeviation = 0.02;
    LayerSlideshow ss(p);
    SchedRng rng(42);
    const int n = 20000;
    double sum = 0, sum2 = 0, dsum = 0, dsum2 = 0;
    for (int i = 0; i < n; ++i) {
        const double v = ss.sampleInterval(rng);
        sum += v;
        sum2 += v * v;
        const double d = ss.sampleTransitionDuration(rng);
        dsum += d;
        dsum2 += d * d;
    }
    const double mean = sum / n, sd = std::sqrt(sum2 / n - mean * mean);
    CHECK(mean == doctest::Approx(15.0).epsilon(0.005));
    CHECK(sd == doctest::Approx(1.5).epsilon(0.05));  // .1 x 15 s
    const double dmean = dsum / n, dsd = std::sqrt(dsum2 / n - dmean * dmean);
    CHECK(dmean == doctest::Approx(10.0).epsilon(0.005));
    CHECK(dsd == doctest::Approx(0.2).epsilon(0.05));
}

TEST_CASE("scheduler: fake clock, interval counts from the end of each transition") {
    LayerPrefs p;
    p.slideshowInterval = 20;
    p.slideshowDeviation = 0.1;
    LayerSlideshow ss(p);
    SchedRng rng(7);
    const double dt = 1.0 / 60.0, transition = 10.0;
    double busyUntil = -1, lastEnd = 0;
    std::vector<double> gaps;
    for (long k = 0; k < 60L * 60 * 60; ++k) {  // one simulated hour
        const double t = double(k) * dt;
        const bool busy = t < busyUntil;
        if (busy) continue;
        if (!ss.armed()) {
            ss.arm(t, rng);
            lastEnd = t;
        }
        if (ss.due(t)) {
            gaps.push_back(t - lastEnd);
            ss.disarm();
            busyUntil = t + transition;
        }
    }
    REQUIRE(gaps.size() > 100);
    double sum = 0;
    for (double g : gaps) sum += g;
    CHECK(sum / double(gaps.size()) == doctest::Approx(20.0).epsilon(0.02));
    // Hold stops switching.
    ss.arm(0, rng);
    ss.setHeld(true);
    CHECK(!ss.due(1e9));
    ss.setHeld(false);
    CHECK(ss.due(1e9));
}

TEST_CASE("playlist: shuffle without immediate repeats, full cycles, previous") {
    Playlist pl;
    pl.setItems({"e", "a", "c", "b", "d"});
    SchedRng rng(3);
    std::string last;
    for (int cycle = 0; cycle < 200; ++cycle) {
        std::set<std::string> seen;
        for (int i = 0; i < 5; ++i) {
            const std::string n = pl.next(rng);
            CHECK(n != last);
            seen.insert(n);
            pl.noteCurrent(n);
            last = n;
        }
        CHECK(seen.size() == 5);
    }
    // Manual pick of the peeked item: next() skips it.
    const std::string peek = pl.peek(rng);
    pl.noteCurrent(peek);
    CHECK(pl.next(rng) != peek);
    // History.
    Playlist h;
    h.setItems({"a", "b", "c"});
    h.noteCurrent("a");
    h.noteCurrent("c");
    CHECK(h.previous() == "a");
    // One item: it is all there is.
    Playlist one;
    one.setItems({"only"});
    one.noteCurrent("only");
    CHECK(one.next(rng) == "only");
}

TEST_CASE("slideshow: parenthesized names are excluded") {
    CHECK(excludedFromSlideshow("(G-Force Logo)"));
    CHECK(excludedFromSlideshow("Sprites/(Script Only)"));
    CHECK(!excludedFromSlideshow("Friday Morning (No Coffee)"));
    CHECK(!excludedFromSlideshow("Plain"));
}

// ---------------------------------------------------------------- themes

TEST_CASE("themes: reforce's own first, then G-Force's listed as \"g-force: NAME\"; either name finds them") {
    ThemeLibrary gforce;
    gforce.addTheme("ThemeName = \"Infernal\"\n");
    gforce.addTheme("ThemeName = \"Voltage\"\n");
    gforce.prefixNames("g-force: ");
    ThemeLibrary lib;
    lib.addTheme("ThemeName = \"Dance Party\"\n");
    lib.merge(gforce);
    REQUIRE(lib.themes().size() == 3);
    CHECK(lib.themes()[0].name == "Dance Party");
    CHECK(lib.themes()[1].name == "g-force: Infernal");
    CHECK(lib.themes()[2].name == "g-force: Voltage");
    REQUIRE(lib.find("Infernal") != nullptr);  // settings saved before, G-Force's own prefs, --theme
    CHECK(lib.find("Infernal")->name == "g-force: Infernal");
    CHECK(lib.find("G-Force: voltage") == &lib.themes()[2]);
    CHECK(lib.find("Dance Party") == &lib.themes()[0]);
}

TEST_CASE("themes: categories, literal \\t separators, (Suggested), regex") {
    ThemeLibrary lib;
    lib.addCategory("ColorMap",
                    "About = \"\"\nCategoryFlags = 1\nCategoryTitle = \"Hot\"\nInclude.Pattern = \"\"\n"
                    "Include.List = \"\nFire\nLava Flow\n\"\nExclude.List = \"\n\"\n");
    lib.addCategory("ColorMap", "CategoryTitle = \"Gray\"\nInclude.Pattern = \"^Gr.*\"\nExclude.List = \"\nGreen\n\"\n");
    lib.addCategory("ColorMap", "CategoryTitle = \"Molten\"\nInclude.List = \"\nLava Flow\n\"\n");
    lib.addCategory("FlowField", "CategoryTitle = \"Spin\"\nInclude.List = \"\nTwirl\nVortex\n\"\n");
    lib.addTheme("ThemeName = \"Warm\"\nColorMap.CategoryExpr = \"+Hot\"\n"
                 "FlowField.CategoryExpr = \"+(Suggested)\\t-Spin\"\nLocked = 1\n");
    lib.addTheme("ThemeName = \"Mixed\"\nColorMap.CategoryExpr = \"+Hot\\t+Gray\\t-Molten\"\n");
    REQUIRE(lib.themes().size() == 2);
    CHECK(lib.find("(Standard)") == nullptr);
    CHECK(lib.find("warm") != nullptr);

    const std::vector<std::string> maps{"Fire", "Lava Flow", "Grime", "Green", "Ocean"};
    const std::vector<std::string> fields{"Twirl", "Expand", "Vortex", "Shrink"};
    const Theme* warm = lib.find("Warm");
    CHECK(lib.filter(warm, "ColorMap", maps) == std::vector<std::string>{"Fire", "Lava Flow"});
    // (Suggested) without a category file = everything; minus Spin.
    CHECK(lib.filter(warm, "FlowField", fields) == std::vector<std::string>{"Expand", "Shrink"});
    // A type without an expression is unrestricted; no theme = all.
    CHECK(lib.filter(warm, "WaveShape", fields) == fields);
    CHECK(lib.filter(nullptr, "ColorMap", maps) == maps);
    // Union of + minus union of -; regex include with an exclude list.
    CHECK(lib.filter(lib.find("Mixed"), "ColorMap", maps) == std::vector<std::string>{"Fire", "Grime"});
    std::vector<std::string> warnings;
    lib.addTheme("ThemeName = \"Broken\"\nColorMap.CategoryExpr = \"+Nope\"\n");
    CHECK(lib.filter(lib.find("Broken"), "ColorMap", maps, &warnings).empty());
    CHECK(warnings.size() == 1);
}

TEST_CASE("scene catalog: listing, Particles split, ColorMap png/txt") {
    const fs::path root = fs::temp_directory_path() / "reforce_m5_catalog";
    fs::remove_all(root);
    const fs::path ws = root / "Packages/G-Force.WaveShape.package";
    const fs::path cm = root / "Packages/G-Force.ColorMap.package";
    fs::create_directories(ws / "WaveShapes/Sub");
    fs::create_directories(ws / "Particles");
    fs::create_directories(cm);
    fs::create_directories(root / "Packages/G-Force.FlowField.package");
    for (const fs::path& p : {ws / "WaveShapes/A.txt", ws / "WaveShapes/Sub/B.txt", ws / "Particles/P.txt",
                              ws / "_package.txt", cm / "Red.txt", cm / "Red.png", cm / "Blue.png",
                              cm / "Blue-icon.png"})
        std::ofstream(p) << "x";
    const auto w = listScenes(root, reforce::scene::SceneType::WaveShape);
    REQUIRE(w.size() == 2);
    CHECK(w[0].id == "WaveShapes/A");
    CHECK(w[1].name == "B");
    CHECK(listScenes(root, reforce::scene::SceneType::Particle).size() == 1);
    const auto c = listScenes(root, reforce::scene::SceneType::ColorMap);
    REQUIRE(c.size() == 2);
    CHECK(c[1].id == "Red");
    CHECK(c[1].path.extension() == ".txt");
    fs::remove_all(root);
}

// ---------------------------------------------------------------- ColorMap blend

TEST_CASE("blend registry: names, gforce default, aliases") {
    CHECK(blendModeNames(Layer::ColorMap).front() == "gforce");
    CHECK(blendModeNames(Layer::FlowField).front() == "gforce");
    CHECK(blendModeNames(Layer::WaveShape).front() == "gforce");
    CHECK(findBlendMode(Layer::ColorMap, "OKLab") == blend::ColorOklab);
    CHECK(findBlendMode(Layer::ColorMap, "rgb") == blend::ColorGForce);
    CHECK(findBlendMode(Layer::FlowField, "polar") == blend::FieldPolar);
    CHECK(findBlendMode(Layer::WaveShape, "aligned") == blend::WaveAligned);
    CHECK(!findBlendMode(Layer::WaveShape, "oklab").has_value());
    CHECK(blendModeName(Layer::FlowField, blend::FieldCrossfade) == "crossfade");
}

TEST_CASE("ColorMap gforce blend = RGB lerp (probe #9: red->green at w=.349 -> (165,89,0))") {
    const PaletteEntry red{1, 0, 0, 1}, green{0, 1, 0, 1};
    // G-Force's captured pixels sum to 254 (165 + 89): one 8-bit level of
    // quantization in its path, so the check allows 1.5 levels.
    const PaletteEntry c = blendColor(blend::ColorGForce, red, green, 0.349);
    CHECK(std::abs(c.r * 255.0 - 165) <= 1.5);
    CHECK(std::abs(c.g * 255.0 - 89) <= 1.5);
    CHECK(c.b == doctest::Approx(0));
    const PaletteEntry d = blendColor(blend::ColorGForce, red, green, 0.647);  // 50% of the morph
    CHECK(std::abs(d.r * 255.0 - 89) <= 1.5);
    CHECK(std::abs(d.g * 255.0 - 165) <= 1.5);
    // Whole palette.
    Palette a{}, b{}, out{};
    a.fill(red);
    b.fill(green);
    blendPalette(blend::ColorGForce, a, b, 0.349, out);
    CHECK(out[17].r == doctest::Approx(c.r));
    // Alternatives keep the end points.
    for (int mode : {blend::ColorOklab, blend::ColorHsvShort}) {
        const PaletteEntry e0 = blendColor(mode, red, green, 0), e1 = blendColor(mode, red, green, 1);
        CHECK(e0.r == doctest::Approx(1).epsilon(1e-3));
        CHECK(e1.g == doctest::Approx(1).epsilon(1e-3));
    }
    // hsv-short: red -> blue goes through magenta (not green).
    const PaletteEntry m = blendColor(blend::ColorHsvShort, red, PaletteEntry{0, 0, 1, 1}, 0.5);
    CHECK(m.r > 0.9f);
    CHECK(m.b > 0.9f);
    CHECK(m.g < 0.05f);
    // OKLab midpoint of red/green stays brighter than the RGB lerp's muddy one.
    const PaletteEntry ok = blendColor(blend::ColorOklab, red, green, 0.5);
    const PaletteEntry lin = blendColor(blend::ColorGForce, red, green, 0.5);
    CHECK(ok.r + ok.g > lin.r + lin.g);
}

// ---------------------------------------------------------------- FlowField blend

TEST_CASE("FlowField gforce blend = linear source lerp; union mask; polar") {
    const double aspect = 1.0;
    auto s = blendFieldPoint(blend::FieldGForce, 0.5f, 0.0f, 0.4f, 0.0f, 0.6f, 0.2f, 0.25, aspect);
    CHECK(s[0] == doctest::Approx(0.45));
    CHECK(s[1] == doctest::Approx(0.05));
    // Probe #10: +-0.3 rad rotations at mid-morph contract by cos .3 under a lerp...
    const float px = 0.5f, py = 0.0f;
    const float ax = float(0.5 * std::cos(0.3)), ay = float(0.5 * std::sin(0.3));
    const float bx = float(0.5 * std::cos(-0.3)), by = float(0.5 * std::sin(-0.3));
    s = blendFieldPoint(blend::FieldGForce, px, py, ax, ay, bx, by, 0.5, aspect);
    CHECK(s[0] == doctest::Approx(0.5 * std::cos(0.3)).epsilon(1e-5));
    CHECK(s[1] == doctest::Approx(0).epsilon(1e-6));
    // ...while polar blends them as rotations (no contraction).
    s = blendFieldPoint(blend::FieldPolar, px, py, ax, ay, bx, by, 0.5, aspect);
    CHECK(s[0] == doctest::Approx(0.5).epsilon(1e-5));
    CHECK(std::abs(s[1]) < 1e-5);
    // Out-of-frame sentinel stays out mid-blend.
    s = blendFieldPoint(blend::FieldGForce, 0, 0, kOutOfFrame, kOutOfFrame, 0, 0, 0.5, aspect);
    CHECK(std::abs(s[0]) > 1.0f);

    // Union: per-pixel weight w * clamp(ALFA, 0, 1) (probe #18 model).
    CHECK(unionWeight(0.6, -2.0) == doctest::Approx(0));
    CHECK(unionWeight(0.6, 0.5) == doctest::Approx(0.3));
    CHECK(unionWeight(0.6, 7.0) == doctest::Approx(0.6));

    // Bake: identity -> zoomed field, ALFA = (x > 0): left half stays, right half lerps.
    const int W = 8, H = 4;
    FlowFieldGrid a = identityFlowField(W, H), b = identityFlowField(W, H);
    for (float& v : b.srcXY) v *= 0.5f;
    UnionMask m;
    m.width = W;
    m.height = H;
    m.alpha.resize(std::size_t(W * H));
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) m.alpha[std::size_t(r * W + c)] = c >= W / 2 ? 1.f : 0.f;
    const FlowFieldGrid baked = bakeFieldBlend(blend::FieldGForce, a, b, &m, 0.5);
    const std::size_t left = std::size_t(1 * W + 0) * 2, right = std::size_t(1 * W + 7) * 2;
    CHECK(baked.srcXY[left] == doctest::Approx(a.srcXY[left]));
    CHECK(baked.srcXY[right] == doctest::Approx(0.75 * a.srcXY[right]));
    const FlowFieldGrid noMask = bakeFieldBlend(blend::FieldGForce, a, b, nullptr, 0.5);
    CHECK(noMask.srcXY[left] == doctest::Approx(0.75 * a.srcXY[left]));
}

// ---------------------------------------------------------------- WaveShape morph

namespace {
MorphPath line(std::vector<float> xs, std::vector<float> ys, float pen = 1.f, float width = 1.f) {
    MorphPath p;
    p.x = std::move(xs);
    p.y = std::move(ys);
    p.pen.assign(p.x.size(), pen);
    p.width.assign(p.x.size(), width);
    return p;
}
}  // namespace

TEST_CASE("WaveShape gforce morph: matched per-index lerp with resampling") {
    MorphFrame from{line({-1, 0, 1}, {0, 0, 0}, 1.0f, 1.f)};
    MorphFrame to{line({-1, -0.5f, 0, 0.5f, 1}, {1, 1, 1, 1, 1}, 0.2f, 3.f)};
    MorphFrame out;
    morphFrames(blend::WaveGForce, from, to, 0.25, out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].size() == 5);  // common count = max(3, 5)
    // From resampled by s index: x = -1, -.5, 0, .5, 1 (same as to).
    CHECK(out[0].x[1] == doctest::Approx(-0.5));
    CHECK(out[0].y[3] == doctest::Approx(0.25));
    CHECK(out[0].pen[2] == doctest::Approx(0.8));
    CHECK(out[0].width[4] == doctest::Approx(1.5));
    // Index matching, not geometry: a reversed path collapses through the middle.
    MorphFrame rev{line({1, 0, -1}, {0, 0, 0})};
    morphFrames(blend::WaveGForce, from, rev, 0.5, out);
    CHECK(out[0].x[0] == doctest::Approx(0));
    CHECK(out[0].x[2] == doctest::Approx(0));
}

TEST_CASE("WaveShape gforce morph: unmatched paths dilate by 1 + 15.5 u^4.16 (probe #12)") {
    CHECK(dilationFactor(0) == doctest::Approx(1));
    CHECK(dilationFactor(0.3) == doctest::Approx(1.104).epsilon(0.001));
    CHECK(dilationFactor(1) == doctest::Approx(16.5));
    // Probe run 3 fit: f - 1 at w = .155/.229/.30/.369/.435/.498.
    const double ws[] = {.155, .229, .30, .369, .435, .498};
    const double fit[] = {.007, .034, .104, .240, .480, .851};
    for (int i = 0; i < 6; ++i) CHECK(dilationFactor(ws[i]) - 1.0 == doctest::Approx(fit[i]).epsilon(0.03));

    // One matched path + one outgoing dot path vs. one matched + one incoming.
    MorphFrame from{line({0, 1}, {0, 0}), line({0.2f}, {0.3f}, 0.9f)};
    MorphFrame to{line({0, 1}, {1, 1})};
    MorphFrame out;
    morphFrames(blend::WaveGForce, from, to, 0.3, out);
    REQUIRE(out.size() == 2);
    CHECK(out[1].x[0] == doctest::Approx(0.2 * dilationFactor(0.3)).epsilon(1e-5));
    CHECK(out[1].y[0] == doctest::Approx(0.3 * 1.104).epsilon(0.001));
    CHECK(out[1].pen[0] == doctest::Approx(0.9));  // full pen, no fade
    // Incoming: u = 1 - w.
    morphFrames(blend::WaveGForce, to, from, 0.7, out);
    REQUIRE(out.size() == 2);
    CHECK(out[1].x[0] == doctest::Approx(0.2 * dilationFactor(0.3)).epsilon(1e-5));
    // At the end of the morph the incoming path is in place.
    morphFrames(blend::WaveGForce, to, from, 1.0, out);
    CHECK(out[1].x[0] == doctest::Approx(0.2));
}

TEST_CASE("WaveShape aligned morph: cyclic shift of closed loops; crossfade pens") {
    // Two identical circles, the second starting a quarter turn later.
    const int n = 33;  // 32 distinct points + closing point
    std::vector<float> ax, ay, bx, by;
    for (int k = 0; k < n; ++k) {
        const double a = 2 * M_PI * k / (n - 1), b = a + M_PI / 2;
        ax.push_back(float(std::cos(a)));
        ay.push_back(float(std::sin(a)));
        bx.push_back(float(std::cos(b)));
        by.push_back(float(std::sin(b)));
    }
    MorphFrame from{line(ax, ay)}, to{line(bx, by)}, out;
    AlignCache cache;
    morphFrames(blend::WaveAligned, from, to, 0.5, out, &cache);
    // Aligned: the circle stays a unit circle mid-morph.
    for (int k = 0; k < n; k += 5) CHECK(std::hypot(out[0].x[k], out[0].y[k]) == doctest::Approx(1).epsilon(1e-3));
    // gforce (index lerp) shrinks it to radius cos(45 deg).
    morphFrames(blend::WaveGForce, from, to, 0.5, out);
    CHECK(std::hypot(out[0].x[0], out[0].y[0]) == doctest::Approx(std::sqrt(0.5)).epsilon(1e-3));
    morphFrames(blend::WaveCrossfade, from, to, 0.25, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].pen[0] == doctest::Approx(0.75));
    CHECK(out[1].pen[0] == doctest::Approx(0.25));
}

// ---------------------------------------------------------------- fixed tick

TEST_CASE("fixed tick: 120 Hz display -> 60 simulation steps per second") {
    FixedStepClock clock(60.0, 4);
    int steps = 0, frameWithTwo = 0;
    for (int f = 0; f < 120 * 10; ++f) {
        const int n = clock.advance(1.0 / 120.0);
        steps += n;
        frameWithTwo += n > 1;
    }
    CHECK(steps == 600);
    CHECK(frameWithTwo == 0);
    // With timestamp jitter it still averages 60/s.
    FixedStepClock j(60.0, 4);
    steps = 0;
    for (int f = 0; f < 1200; ++f) steps += j.advance(1.0 / 120.0 + ((f % 2) ? 0.0004 : -0.0004));
    CHECK(std::abs(steps - 600) <= 1);
    // 30 Hz display: two steps per frame. 60 Hz sim at 60 Hz display: one.
    FixedStepClock s(60.0, 4);
    CHECK(s.advance(1.0 / 30.0) == 2);
    CHECK(s.advance(1.0 / 60.0) == 1);
    // A stall: capped at 4 steps, the backlog is dropped.
    CHECK(s.advance(0.4) == 4);
    CHECK(s.backlog() < s.stepSeconds());
    CHECK(s.advance(1.0 / 60.0) == 1);
    // TargetFrameRate 30: 30 steps/s on a 120 Hz display.
    FixedStepClock t30(30.0);
    steps = 0;
    for (int f = 0; f < 120; ++f) steps += t30.advance(1.0 / 120.0);
    CHECK(steps == 30);
}

// ---------------------------------------------------------------- commands

TEST_CASE("commands: thread-safe queue, describe, layers") {
    CommandQueue q;
    std::thread producer([&] {
        for (int i = 0; i < 1000; ++i) q.push(Command::next(Layer::FlowField));
    });
    std::size_t got = 0;
    while (got < 1000) got += q.drain().size();
    producer.join();
    CHECK(got == 1000);
    CHECK(describe(Command::switchTo(Layer::WaveShape, "Gold/X")) == "WaveShape switch \"Gold/X\" (quick)");
    CHECK(describe(Command::toggleHoldAll()) == "all toggle hold");
    CHECK(describe(Command::morphTo(Layer::ColorMap, "Red", 10)) == "ColorMap switch \"Red\" (10.00 s)");
    CHECK(describe(Command::blendMode(Layer::ColorMap, "oklab")) == "ColorMap blend oklab");
    CHECK(parseLayer("flowfield") == Layer::FlowField);
    CHECK(parseLayer("FlowField Union") == Layer::Union);
    CHECK(parseLayer("ws") == Layer::WaveShape);
    CHECK(!parseLayer("nope").has_value());
    StateBox box;
    EngineState s;
    s.simSteps = 5;
    box.publish(s);
    CHECK(box.snapshot().simSteps == 5);
}

TEST_CASE("commands: morph with an explicit duration (scripted StartConfig/SetFlowField)") {
    auto c = parseCommand("morph:flowfield:6:zz reforce zoom in");
    REQUIRE(c);
    CHECK(c->type == CommandType::Switch);
    CHECK(c->layer == Layer::FlowField);
    CHECK(c->target == SwitchTarget::Name);
    CHECK(c->text == "zz reforce zoom in");
    CHECK(c->seconds == doctest::Approx(6));
    c = parseCommand("morph:ws:0:Pkg/Name: with colon");  // 0 = cut; the name keeps its ':'
    REQUIRE(c);
    CHECK(c->seconds == 0);
    CHECK(c->text == "Pkg/Name: with colon");
    c = parseCommand("switch:cm:Red");
    REQUIRE(c);
    CHECK(c->seconds < 0);  // unset: Transition.Duration.Quick
    std::string err;
    CHECK(!parseCommand("morph:cm:Red", &err));
    CHECK(!parseCommand("morph:cm:-1:Red", &err));
    CHECK(!parseCommand("morph:cm:2.5", &err));
    CHECK(!parseCommand("morph:particle:2:P", &err));
}

namespace {
// The pre-M7b morph (resampled copies + shifted copy), kept as the reference.
MorphPath refMorphPair(const MorphPath& from, const MorphPath& to, double w, int shift, int dir) {
    const std::size_t n = std::max(from.size(), to.size());
    const MorphPath a = resamplePath(from, n);
    MorphPath b = resamplePath(to, n);
    if (dir != 0) {
        const MorphPath src = b;
        const std::size_t m = n - 1;
        for (std::size_t j = 0; j < m; ++j) {
            const std::size_t k = dir > 0 ? (j + std::size_t(shift)) % m : (std::size_t(shift) + m - j % m) % m;
            b.x[j] = src.x[k], b.y[j] = src.y[k], b.pen[j] = src.pen[k], b.width[j] = src.width[k];
        }
        b.x[m] = b.x[0], b.y[m] = b.y[0], b.pen[m] = b.pen[0], b.width[m] = b.width[0];
    }
    MorphPath r;
    r.connected = w < 0.5 ? a.connected : b.connected;
    for (std::size_t j = 0; j < n; ++j) {
        auto l = [&](float x, float y) { return float(double(x) + (double(y) - double(x)) * w); };
        r.x.push_back(l(a.x[j], b.x[j]));
        r.y.push_back(l(a.y[j], b.y[j]));
        r.pen.push_back(l(a.pen[j], b.pen[j]));
        r.width.push_back(l(a.width[j], b.width[j]));
    }
    return r;
}

MorphPath ring(std::size_t n, double r, double phase, double pen) {
    MorphPath p;
    for (std::size_t j = 0; j < n; ++j) {
        const double a = 2 * 3.14159265358979 * double(j) / double(n - 1) + phase;
        p.x.push_back(float(r * std::cos(a)));
        p.y.push_back(float(r * std::sin(a)));
        p.pen.push_back(float(pen * double(j) / double(n)));
        p.width.push_back(float(1 + double(j % 3)));
    }
    p.x.back() = p.x.front();
    p.y.back() = p.y.front();
    return p;
}
}  // namespace

TEST_CASE("wave morph: in-place (allocation-free) morph matches the copy-based reference exactly") {
    MorphFrame out;  // reused across calls with different shapes, like the visualizer
    for (int mode : {blend::WaveGForce, blend::WaveAligned}) {
        for (auto sizes : {std::pair{64, 64}, std::pair{40, 101}, std::pair{257, 33}}) {
            const MorphFrame from{ring(std::size_t(sizes.first), .5, 0, 1), ring(10, .2, 0, .5)};
            const MorphFrame to{ring(std::size_t(sizes.second), .4, 1.3, .7)};
            AlignCache cache;
            for (double w : {0.0, 0.3, 0.5, 0.9, 1.0}) {
                morphFrames(mode, from, to, w, out, &cache);
                REQUIRE(out.size() == 2);
                const int dir = mode == blend::WaveAligned ? cache.dir[0] : 0;
                if (mode == blend::WaveAligned) CHECK(dir != 0);
                const MorphPath ref = refMorphPair(from[0], to[0], w, dir ? cache.shift[0] : 0, dir);
                CHECK(out[0].x == ref.x);
                CHECK(out[0].y == ref.y);
                CHECK(out[0].pen == ref.pen);
                CHECK(out[0].width == ref.width);
                CHECK(out[1].x.size() == 10);  // unmatched outgoing path, dilated
                CHECK(out[1].x[0] == doctest::Approx(0.2 * dilationFactor(w)).epsilon(1e-5));
            }
        }
    }
}

TEST_CASE("wave morph: parallel shift search gives the serial answer (Phase 5D)") {
    // Brute force: first (dir, s) candidate with the strictly smallest full sum.
    auto brute = [](const MorphPath& a, const MorphPath& b, int& shift, int& dir) {
        const std::size_t m = a.size() - 1, stride = m > 512 ? m / 512 : 1;
        double best = std::numeric_limits<double>::infinity();
        shift = 0;
        dir = 1;
        for (int d : {1, -1})
            for (std::size_t s = 0; s < m; s += stride) {
                double sum = 0;
                for (std::size_t j = 0; j < m; ++j) {
                    const std::size_t k = d > 0 ? (j + s) % m : (s + m - j % m) % m;
                    const double dx = a.x[j] - b.x[k], dy = a.y[j] - b.y[k];
                    sum += dx * dx + dy * dy;
                }
                if (sum < best) {
                    best = sum;
                    shift = int(s);
                    dir = d;
                }
            }
    };
    // Loops large enough for several chunks (and the > 512 stride), many small
    // ones, a symmetric square (exact ties between shifts), an open path.
    MorphFrame from, to;
    for (std::size_t n : {1201u, 2049u, 65u}) {
        from.push_back(ring(n, .5, 0.3, 1));
        to.push_back(ring(n, .45, 2.1, .6));
    }
    for (int i = 0; i < 20; ++i) {
        from.push_back(ring(std::size_t(40 + i), .1 + .01 * i, .1 * i, 1));
        to.push_back(ring(std::size_t(60 - i), .2, -.2 * i, 1));
    }
    MorphPath square;
    for (int j = 0; j <= 16; ++j) {
        const int q = (j / 4) % 4;
        square.x.push_back(q == 0 || q == 3 ? 1.f : -1.f);
        square.y.push_back(q < 2 ? 1.f : -1.f);
        square.pen.push_back(1);
        square.width.push_back(1);
    }
    from.push_back(square);
    to.push_back(square);
    MorphPath open = ring(100, .3, 0, 1);
    open.x.back() = 5;
    from.push_back(open);
    to.push_back(ring(100, .3, 1, 1));

    reforce::scene::WorkerPool pool(3);
    AlignCache serialCache, parCache;
    MorphFrame outS, outP;
    morphFrames(blend::WaveAligned, from, to, 0.4, outS, &serialCache);
    morphFrames(blend::WaveAligned, from, to, 0.4, outP, &parCache, &pool);
    REQUIRE(serialCache.shift.size() == from.size());
    CHECK(serialCache.shift == parCache.shift);
    CHECK(serialCache.dir == parCache.dir);
    CHECK(serialCache.dir.back() == 0);  // the open path is not aligned
    for (std::size_t k = 0; k + 1 < from.size(); ++k) {
        const std::size_t n = std::max(from[k].size(), to[k].size());
        int s = 0, d = 0;
        brute(resamplePath(from[k], n), resamplePath(to[k], n), s, d);
        CHECK(parCache.shift[k] == s);
        CHECK(parCache.dir[k] == d);
    }
    REQUIRE(outS.size() == outP.size());
    for (std::size_t k = 0; k < outS.size(); ++k) {
        CHECK(outS[k].x == outP[k].x);
        CHECK(outS[k].y == outP[k].y);
        CHECK(outS[k].pen == outP[k].pen);
    }
}
