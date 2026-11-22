// Tier ordering / persistence and per-type evaluation, on configs written for these tests.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "scene/Evaluate.hpp"

using namespace reforce::scene;
using reforce::expr::Var;
using doctest::Approx;

namespace {

CompiledConfig compile(std::string_view text, SceneType type) {
    auto f = parseSceneText(text);
    auto cfg = compileConfig(f, type);
    for (const auto& d : cfg.diags.items())
        if (d.severity == reforce::expr::Severity::Error) FAIL_CHECK(d.kind << ": " << d.message);
    return cfg;
}

}  // namespace

TEST_CASE("tiers: keys are assigned per scene type") {
    auto ws = compile("NUM=2 Stps=4 A0=1 B0=2 C0=3 X0=s Y0=0 Pen=1 LWdt=1 ConB=1 Side=5 D0=1", SceneType::WaveShape);
    CHECK(ws.find("NUM")->tier == Tier::Pre);
    CHECK(ws.find("SIDE")->tier == Tier::Pre);
    CHECK(ws.find("A0")->tier == Tier::Load);
    CHECK(ws.find("B0")->tier == Tier::Frame);
    CHECK(ws.find("C0")->tier == Tier::Step);
    CHECK(ws.find("X0")->tier == Tier::StepOutput);
    CHECK(ws.find("PEN")->tier == Tier::StepOutput);
    CHECK(ws.find("D0")->tier == Tier::Ignored);
    CHECK(ws.diags.countKind("key-unused-for-type") == 1);

    auto ff = compile("A0=1 C0=y D0=x srcX=x srcY=y", SceneType::FlowField);
    CHECK(ff.find("C0")->tier == Tier::Step);
    CHECK(ff.find("D0")->tier == Tier::Pixel);
    CHECK(ff.find("SRCX")->tier == Tier::PixelOutput);

    auto cm = compile("A0=1 B0=t C0=i H=i S=1 V=i CCal=.05", SceneType::ColorMap);
    CHECK(cm.find("S")->tier == Tier::StepOutput);  // ColorMap S is saturation, not s
    CHECK(cm.find("CCAL")->tier == Tier::PostLoad);
}

TEST_CASE("tiers: A vars evaluate in ascending index order regardless of file order") {
    auto cfg = compile("A2=\"A1 * 10\"\nA1=\"A0 + 1\"\nA0=\"2\"", SceneType::WaveShape);
    WaveShapeInstance w(cfg, {});
    CHECK(w.context().slot(0, 2).scalar() == 30);
}

TEST_CASE("tiers: forward reference within a tier reads the previous value (initially 0)") {
    auto cfg = compile("Stps=2 A0=\"A1 + 1\" A1=\"5\" B0=\"B1\" B1=\"B1 + 1\" X0=0 Y0=0", SceneType::WaveShape);
    WaveShapeInstance w(cfg, {});
    CHECK(w.context().slot(0, 0).scalar() == 1);  // A1 not yet evaluated
    WaveShapeFrame f;
    w.evaluateFrame({}, f);
    CHECK(w.context().slot(1, 0).scalar() == 0);  // B0 saw B1 before its first write
    CHECK(w.context().slot(1, 1).scalar() == 1);
    w.evaluateFrame({}, f);
    CHECK(w.context().slot(1, 0).scalar() == 1);  // previous frame's B1
    CHECK(w.context().slot(1, 1).scalar() == 2);
}

TEST_CASE("tiers: B self-reference persists across frames") {
    auto cfg = compile("Stps=3 B0=\"B0 + 1\" X0=\"B0\" Y0=0", SceneType::WaveShape);
    WaveShapeInstance w(cfg, {});
    WaveShapeFrame f;
    for (int frame = 1; frame <= 4; ++frame) {
        w.evaluateFrame({}, f);
        REQUIRE(f.paths.size() == 1);
        CHECK(f.paths[0].x[0] == frame);
        CHECK(f.paths[0].x[2] == frame);
    }
}

TEST_CASE("tiers: C vars are per-step state persisting across frames (probe 2026-11)") {
    // Probe: Stps=5, C0="C0 + .1", Y1="C0": every step equals .1, .2, .3 ... per frame.
    auto cfg = compile("Stps=5 C0=\"C0 + .1\" X1=s Y1=\"C0\"", SceneType::WaveShape);
    WaveShapeInstance w(cfg, {});
    WaveShapeFrame f;
    for (int frame = 1; frame <= 3; ++frame) {
        w.evaluateFrame({}, f);
        REQUIRE(f.paths.size() == 1);
        for (int k = 0; k < 5; ++k) CHECK(f.paths[0].y[std::size_t(k)] == Approx(0.1 * frame));
    }
    // Ascending order within a step: C1 sees this step's new C0; C0 sees last frame's C1.
    auto cfg2 = compile("Stps=2 C0=\"C1 + 1\" C1=\"C0 * 10\" X0=C0 Y0=C1", SceneType::WaveShape);
    WaveShapeInstance w2(cfg2, {});
    w2.evaluateFrame({}, f);
    CHECK(f.paths[0].x[0] == 1);
    CHECK(f.paths[0].y[0] == 10);
    w2.evaluateFrame({}, f);
    CHECK(f.paths[0].x[1] == 11);
    CHECK(f.paths[0].y[1] == 110);
}

TEST_CASE("wave: s is inclusive and a vector in A/B tiers") {
    auto cfg = compile("Stps=5 A0=\"cols(s)\" B0=\"col(s, 1)\" X0=s Y0=\"B0 + A0\"", SceneType::WaveShape);
    WaveShapeInstance w(cfg, {});
    CHECK(w.numSteps() == 5);
    WaveShapeFrame f;
    w.evaluateFrame({}, f);
    const auto& p = f.paths[0];
    CHECK(p.x[0] == 0);
    CHECK(p.x[1] == Approx(.25));
    CHECK(p.x[4] == 1);
    CHECK(p.y[0] == Approx(5.25));
}

TEST_CASE("wave: vector X path from a B matrix is sampled per step") {
    auto cfg = compile("Stps=4 B0=\"{ vectorLR(4, 0, 3), vectorLR(4, 10, 13) }\" X0=\"row(B0, 0)\" Y0=\"row(B0, 1)\"",
                       SceneType::WaveShape);
    WaveShapeInstance w(cfg, {});
    WaveShapeFrame f;
    w.evaluateFrame({}, f);
    for (int k = 0; k < 4; ++k) {
        CHECK(f.paths[0].x[std::size_t(k)] == k);
        CHECK(f.paths[0].y[std::size_t(k)] == 10 + k);
    }
}

TEST_CASE("wave: NUM instances, ID, shared B storage, defaults") {
    auto cfg = compile("Num=\"2.9\" Stps=2 B0=\"B0 + 1\" X0=ID Y0=B0 ConB=1", SceneType::WaveShape);
    WaveShapeInstance w(cfg, {});
    CHECK(w.numInstances() == 2);
    CHECK(w.connected());
    WaveShapeFrame f;
    w.evaluateFrame({}, f);
    REQUIRE(f.paths.size() == 2);
    CHECK(f.paths[0].id == 0);
    CHECK(f.paths[1].x[0] == 1);   // ID
    CHECK(f.paths[0].y[0] == 1);
    CHECK(f.paths[1].y[0] == 2);   // B storage shared across IDs
    CHECK(f.paths[0].pen[0] == 1); // Pen default
    CHECK(f.paths[0].width[0] == 1);

    Environment env;
    env.numSampleBins = 7;
    auto def = compile("X0=s Y0=0", SceneType::WaveShape);
    WaveShapeInstance d(def, env);
    CHECK(d.numSteps() == 7);  // Stps omitted -> NUM_SAMPLE_BINS
    CHECK(d.numInstances() == 1);
    CHECK_FALSE(d.connected());
}

TEST_CASE("wave: Pen clamps, PATH_ID, named constants and key refs") {
    auto cfg = compile("Side=3 Num=\"1 + Side\" Stps=\"Side * 2\" A0=\"Stps / Side\" X0=0 Y0=0 X3=0 Y3=0 "
                       "Pen=\"PATH_ID - 1\" LWdt=\"A0\"",
                       SceneType::WaveShape);
    WaveShapeInstance w(cfg, {});
    CHECK(w.numInstances() == 4);
    CHECK(w.numSteps() == 6);
    CHECK(w.context().slot(0, 0).scalar() == 2);
    WaveShapeFrame f;
    w.evaluateFrame({}, f);
    REQUIRE(f.paths.size() == 8);
    CHECK(f.paths[0].pathIndex == 0);
    CHECK(f.paths[1].pathIndex == 3);
    CHECK(f.paths[0].pen[0] == 0);  // -1 clamped
    CHECK(f.paths[1].pen[0] == 1);  // 2 clipped at 1
    CHECK(f.paths[0].width[0] == 2);
}

TEST_CASE("wave: particle timing variables and audio") {
    auto cfg = compile("Stps=3 A0=\"end_time < 1e30\" B0=dt X0=\"mag(s)\" Y0=\"fft(s) + bass\"", SceneType::Particle);
    std::vector<float> wave = {-1, 0, 1}, spec = {0, 2, 4};
    WaveShapeFrameInput in;
    in.audio = {wave.data(), 3, spec.data(), 3};
    in.bass = .5;
    in.t = 15;
    in.startTime = 10;
    in.endTime = 20;
    WaveShapeInstance w(cfg, {});
    CHECK(w.context().slot(0, 0).scalar() == 0);  // load: END_TIME defaults to 1e30
    WaveShapeFrame f;
    w.evaluateFrame(in, f);
    CHECK(w.context().slot(1, 0).scalar() == .5);
    CHECK(f.paths[0].x[0] == -1);
    CHECK(f.paths[0].x[2] == 1);
    CHECK(f.paths[0].y[1] == 2.5);
}

TEST_CASE("wave: reforce audio extensions MID / TREBLE / BEAT / TEMPO (case-insensitive)") {
    auto cfg = compile("Stps=2 B0=\"mid + 10 * Treble\" B1=\"BEAT\" B2=\"tempo\" X0=\"B0\" Y0=\"B1 + B2\"",
                       SceneType::WaveShape);
    WaveShapeFrameInput in;
    in.mid = .25;
    in.treble = .5;
    in.beat = 1;
    in.tempo = 120;
    WaveShapeInstance w(cfg, {});
    WaveShapeFrame f;
    w.evaluateFrame(in, f);
    CHECK(f.paths[0].x[0] == Approx(5.25));
    CHECK(f.paths[0].y[0] == Approx(121));
}

TEST_CASE("wave: Ableton Link variables LINK_ON / LINK_BPM / LINK_BEAT / LINK_PHASE") {
    auto cfg = compile("Stps=2 B0=\"link_on * 1000 + Link_Bpm\" B1=\"LINK_BEAT\" B2=\"link_phase\" X0=\"B0\" "
                       "Y0=\"B1 + B2 / 10\"",
                       SceneType::WaveShape);
    WaveShapeFrameInput in;
    in.linkOn = 1;
    in.linkBpm = 128;
    in.linkBeat = 37.5;
    in.linkPhase = 1.5;
    WaveShapeInstance w(cfg, {});
    WaveShapeFrame f;
    w.evaluateFrame(in, f);
    CHECK(f.paths[0].x[0] == Approx(1128));
    CHECK(f.paths[0].y[0] == Approx(37.65));
}

TEST_CASE("colormap: HSV palette, B per recompute, C per i") {
    auto cfg = compile("A0=\".5\" B0=\"t\" C0=\"i * 2\" H=\"B0\" S=1 V=\"C0\"", SceneType::ColorMap);
    ColorMapInstance cm(cfg, {});
    CHECK(cm.usesHsv());
    std::array<Rgb, 256> pal;
    cm.evaluate(0.0, pal);  // hue 0 = red
    CHECK(pal[0].r == 0);
    CHECK(pal[64].r == Approx(128.0 / 255.0));
    CHECK(pal[255].r == 1);  // V clamps at 1
    CHECK(pal[255].g == 0);
    cm.evaluate(2.0 / 3.0, pal);  // blue
    CHECK(pal[255].b == Approx(1));
    cm.evaluate(1.0 + 1.0 / 3.0, pal);  // H wraps -> green
    CHECK(pal[255].g == Approx(1));
    CHECK(pal[255].r == Approx(0).epsilon(1e-5));
}

TEST_CASE("colormap: RGB outputs clamp to [0,1] (probe 2026-11)") {
    auto cfg = compile("R=\"2 * i\" G=\"-1 + i\" B=\"i\" CCal=\".0333\"", SceneType::ColorMap);
    ColorMapInstance cm(cfg, {});
    CHECK_FALSE(cm.usesHsv());
    CHECK(cm.recomputeInterval() == Approx(.0333));
    std::array<Rgb, 256> pal;
    cm.evaluate(0, pal);
    CHECK(pal[255].r == 1);
    CHECK(pal[200].r == 1);
    CHECK(pal[255].g == 0);
    CHECK(pal[0].g == 0);
    CHECK(pal[51].b == Approx(.2));
}

TEST_CASE("colormap: C vars are per-i state across recomputes") {
    auto cfg = compile("C0=\"C0 + i\" R=\"C0\" G=0 B=0", SceneType::ColorMap);
    ColorMapInstance cm(cfg, {});
    std::array<Rgb, 256> pal;
    cm.evaluate(0, pal);
    CHECK(pal[51].r == Approx(.2));
    cm.evaluate(0, pal);
    CHECK(pal[51].r == Approx(.4));
}

TEST_CASE("flowfield: cartesian rows, points and grid") {
    Environment env;
    env.width = 201;  // FlowField Aspc=1: X_EXTENTS = (W-1)/(H-1) = 2
    env.height = 101;
    auto cfg = compile("Aspc=1 A0=\".5\" D0=\"x * A0\" srcX=\"D0\" srcY=\"y + X_EXTENTS\"", SceneType::FlowField);
    FlowFieldInstance ff(cfg, env);
    CHECK(ff.xExtent() == 2);
    CHECK(ff.yExtent() == 1);
    auto p = ff.evaluatePoint(1.0, -0.5);
    CHECK(p[0] == .5);
    CHECK(p[1] == 1.5);
    std::vector<double> xs = {-1, 0, 1, 2};
    double sx[4], sy[4];
    ff.evaluateRow(.25, xs, sx, sy);
    CHECK(sx[0] == -.5);
    CHECK(sx[3] == 1);
    CHECK(sy[2] == 2.25);
    std::vector<float> grid;
    ff.evaluateGrid(8, 4, grid);
    CHECK(grid.size() == 64);
    const auto rx = ff.rowXs(8);
    // Edge pixels sit on the frame edges (x = -1 + 2c/(W-1), y = 1 - 2r/(H-1), times the extents).
    CHECK(rx.front() == Approx(-2));
    CHECK(rx.back() == Approx(2));
    CHECK(ff.rowY(0, 4) == Approx(1));
    CHECK(ff.rowY(3, 4) == Approx(-1));
    CHECK(grid[0] == Approx(rx[0] * .5));
    CHECK(grid[1] == Approx(1 + 2));
}

TEST_CASE("flowfield: polar sources, missing coordinates, rnd per pixel vs per row") {
    auto cfg = compile("srcR=\"r * 2\" srcT=\"theta + pi / 2\"", SceneType::FlowField);
    FlowFieldInstance ff(cfg, {});
    CHECK(ff.polar());
    auto p = ff.evaluatePoint(1, 0);
    CHECK(p[0] == Approx(0).epsilon(1e-9));
    CHECK(p[1] == Approx(2));

    auto onlyX = compile("srcX=\"x * .5\"", SceneType::FlowField);
    FlowFieldInstance fx(onlyX, {});
    auto q = fx.evaluatePoint(.8, .3);
    CHECK(q[0] == Approx(.4));
    CHECK(q[1] == Approx(.3));  // missing srcY: identity (guess)

    auto rnd = compile("C0=\"rnd(1)\" D0=\"rnd(1)\" srcX=\"C0\" srcY=\"D0\"", SceneType::FlowField);
    FlowFieldInstance fr(rnd, {});
    std::vector<double> xs(16);
    for (int i = 0; i < 16; ++i) xs[std::size_t(i)] = i / 16.0;
    std::vector<double> sx(16), sy(16);
    fr.evaluateRow(0, xs, sx.data(), sy.data());
    bool cSame = true, dVaries = false;
    for (int i = 1; i < 16; ++i) {
        cSame = cSame && sx[std::size_t(i)] == sx[0];
        dVaries = dVaries || sy[std::size_t(i)] != sy[0];
    }
    CHECK(cSame);    // C: one scalar per row
    CHECK(dVaries);  // D: per pixel
}

TEST_CASE("flowfield: vector-x idioms with map2D and noise") {
    auto cfg = compile("A0=\"srand(4) * 0 + noise(4, 4, 2)\" D0=\"(y + 1) / 2\" "
                       "srcX=\"map2D(A0, vectorLR(cols(x), 0, 1), vector(cols(x), D0), 1)\" srcY=y",
                       SceneType::FlowField);
    FlowFieldInstance ff(cfg, {});
    std::vector<float> grid;
    ff.evaluateGrid(16, 4, grid);
    for (float v : grid) CHECK(std::isfinite(v));
}

TEST_CASE("union: ALFA per pixel") {
    auto cfg = compile("A0=\".5\" ALFA=\"A0 - r\"", SceneType::FlowFieldUnion);
    FlowFieldInstance u(cfg, {});
    std::vector<double> xs = {0, .5, 1};
    double a[3];
    u.evaluateAlphaRow(0, xs, a);
    CHECK(a[0] == .5);
    CHECK(a[1] == 0);
    CHECK(a[2] == -.5);
}

TEST_CASE("sprite: per-frame outputs") {
    auto cfg = compile("A0=\".25\" Sped=\"A0 * 4\" B0=\"t * 2\" X=\"B0\" Y=\"A0\" EPen=\"dt ^ 1.5\" Flag=7",
                       SceneType::Sprite);
    CHECK(cfg.find("SPED")->tier == Tier::PostLoad);
    SpriteInstance sp(cfg, {});
    auto f = sp.evaluateFrame(3, .25);
    CHECK(f.x == 6);
    CHECK(f.y == .25);
    // Pen omitted: a background-only sprite (only EPen draws) [DS].
    CHECK(!f.hasPen);
    CHECK(f.pen == 0);
    CHECK(f.epen == Approx(.125));
    CHECK(sp.sped() == 1);
    CHECK(sp.flags() == 7);
    CHECK(sp.hasFlags());
    CHECK(!sp.invt());
    CHECK(std::isnan(sp.duration()));
}

TEST_CASE("sprite: XPOS/YPOS/FADER inputs, Pen, PDur") {
    auto cfg = compile("X=\"XPOS\" Y=\"YPOS * FADER\" Pen=\".5\" EPen=\"FADER\" Invt=1 PDur=\"12\"", SceneType::Sprite);
    SpriteInstance sp(cfg, {});
    SpriteFrameInput in;
    in.t = 1;
    in.xpos = .3;
    in.ypos = -.4;
    in.fader = .5;
    auto f = sp.evaluateFrame(in);
    CHECK(f.hasPen);
    CHECK(f.pen == .5);
    CHECK(f.x == Approx(.3));
    CHECK(f.y == Approx(-.2));
    CHECK(f.epen == .5);
    CHECK(sp.invt());
    CHECK(!sp.hasFlags());
    CHECK(sp.flags() == 0);
    CHECK(sp.duration() == 12);
}

TEST_CASE("determinism: same seed, same A-tier randomness; different seeds differ") {
    auto cfg = compile("A0=\"rnd(1)\" A1=\"choice({1, 2, 3, 4, 5, 6, 7, 8})\" X0=0 Y0=0", SceneType::WaveShape);
    Environment e1, e2;
    e1.seed = e2.seed = 99;
    WaveShapeInstance a(cfg, e1), b(cfg, e2);
    CHECK(a.context().slot(0, 0).scalar() == b.context().slot(0, 0).scalar());
    CHECK(a.context().slot(0, 1).scalar() == b.context().slot(0, 1).scalar());
    e2.seed = 100;
    WaveShapeInstance c(cfg, e2);
    CHECK(a.context().slot(0, 0).scalar() != c.context().slot(0, 0).scalar());
}

TEST_CASE("resolution: non-native and unknown identifiers warn and read 0") {
    auto f = parseSceneText("A0=\"theta + 1\" A1=\"nosuchname\" X0=0 Y0=0");
    auto cfg = compileConfig(f, SceneType::WaveShape);
    CHECK(cfg.diags.countKind("identifier-not-available") == 1);
    CHECK(cfg.diags.countKind("unknown-identifier") == 1);
    CHECK(cfg.diags.errorCount() == 0);
    WaveShapeInstance w(cfg, {});
    CHECK(w.context().slot(0, 0).scalar() == 1);
    CHECK(w.context().slot(0, 1).scalar() == 0);
    // A WaveShape key named X is a key reference, not the FlowField builtin x.
    auto g = compile("X=\"7\" A0=\"X * 2\" X0=0 Y0=0", SceneType::WaveShape);
    WaveShapeInstance wg(g, {});
    CHECK(wg.context().slot(0, 0).scalar() == 14);
}

TEST_CASE("wave: NumS is the old name of Stps (scene-format 5.1)") {
    auto old = compile("NumS=\"3 * 2\" A0=\"NumS + 1\" X0=s Y0=0", SceneType::WaveShape);
    WaveShapeInstance w(old, {});
    CHECK(w.numSteps() == 6);
    CHECK(old.find("STPS") != nullptr);
    CHECK(w.context().slot(0, 0).scalar() == 7);  // still readable by its own name

    // An explicit Stps wins; NumS is then just a named constant.
    auto both = compile("NumS=9 Stps=4 X0=s Y0=0", SceneType::Particle);
    WaveShapeInstance b(both, {});
    CHECK(b.numSteps() == 4);
}

// ---------------------------------------------------------------- Phase 5D: parallel steps

namespace {

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// Evaluates `frames` frames serially and with a pool (forced parallel) and
// requires bit-identical paths and the same final key/slot state.
void checkParallelMatchesSerial(std::string_view text, WaveShapeInstance::Parallel expected, int frames = 6) {
    auto cfg = compile(text, SceneType::WaveShape);
    Environment env;
    env.seed = 7;
    WaveShapeInstance serial(cfg, env), par(cfg, env), whole(cfg, env);
    whole.setSlicing(false);  // reference: whole-row evaluation, as before Phase 5D
    CHECK(par.parallelMode() == expected);
    WorkerPool pool(3);
    par.setPool(&pool);
    par.setMinParallelWork(0);
    std::vector<float> wave(200), fft(185);
    WaveShapeFrame fs, fp, fw;
    for (int frame = 0; frame < frames; ++frame) {
        for (std::size_t i = 0; i < wave.size(); ++i) wave[i] = float(std::sin(double(i) * .3 + frame));
        for (std::size_t i = 0; i < fft.size(); ++i) fft[i] = float(1.0 / (1.0 + double(i) * .1 + frame));
        WaveShapeFrameInput in;
        in.t = 1.0 + frame / 60.0;
        in.bass = .3 + .1 * frame;
        in.audio = {wave.data(), int(wave.size()), fft.data(), int(fft.size())};
        serial.evaluateFrame(in, fs);
        par.evaluateFrame(in, fp);
        whole.evaluateFrame(in, fw);
        REQUIRE(fs.paths.size() == fp.paths.size());
        REQUIRE(fs.paths.size() == fw.paths.size());
        for (std::size_t p = 0; p < fs.paths.size(); ++p) {
            CHECK(sameBits(fs.paths[p].x, fp.paths[p].x));
            CHECK(sameBits(fs.paths[p].y, fp.paths[p].y));
            CHECK(sameBits(fs.paths[p].pen, fp.paths[p].pen));
            CHECK(sameBits(fs.paths[p].width, fp.paths[p].width));
            CHECK(sameBits(fw.paths[p].x, fp.paths[p].x));
            CHECK(sameBits(fw.paths[p].y, fp.paths[p].y));
            CHECK(sameBits(fw.paths[p].pen, fp.paths[p].pen));
            CHECK(sameBits(fw.paths[p].width, fp.paths[p].width));
        }
    }
    // State carried into the next frame matches too.
    const auto& ks = serial.context().keys;
    const auto& kp = par.context().keys;
    REQUIRE(ks.size() == kp.size());
    for (std::size_t i = 0; i < ks.size(); ++i) CHECK(ks[i].toString() == kp[i].toString());
    for (int fam = 0; fam < 4; ++fam)
        for (std::size_t i = 0; i < serial.context().vars[std::size_t(fam)].size(); ++i)
            CHECK(serial.context().vars[std::size_t(fam)][i].toString() ==
                  par.context().vars[std::size_t(fam)][i].toString());
    for (auto v : {Var::S, Var::PATH_ID, Var::ID}) CHECK(serial.context().get(v) == par.context().get(v));
}

}  // namespace

TEST_CASE("wave: parallel step evaluation is bit-identical to serial (Phase 5D)") {
    using P = WaveShapeInstance::Parallel;
    // Per-step C history (C0 accumulates per step across frames and IDs), B
    // chained across IDs, vectors over s in B, audio, per-path Pen and LWdt.
    checkParallelMatchesSerial(
        "Num=5 Stps=203 A0=\"rnd(1)\" B0=\"B0 + .01 * ID\" B1=\".3 * mag(s) + B0\" "
        "C0=\"C0 * .9 + s\" C1=\"C0 + C2\" C2=\"sin(C1 * 3 + t)\" "
        "X0=\"cos(s * 6.28 + B0) * col(B1, 3)\" Y0=\"C1 + X0 * .1\" X2=\"-X0\" Y2=\"Y0 * fft(s)\" "
        "Pen=\"PATH_ID * .3 + s\" LWdt=\"1 + PATH_ID + C2\" ConB=1",
        P::Frame);
    // B reads C (the previous ID's last step): one dispatch per ID.
    checkParallelMatchesSerial(
        "Num=4 Stps=150 B0=\"C0 + B0 * .5\" C0=\"C0 + s * B0 + ID\" X0=\"C0 * .01\" Y0=\"B0\" Pen=\"s\"", P::PerId);
    // B reads a step-tier key: per ID too.
    checkParallelMatchesSerial("Num=3 Stps=64 B0=\"Y0 + 1\" X0=\"s * B0\" Y0=\"X0 * X0\"", P::PerId);
    // C reads PATH_ID (carried from the previous step's last path).
    checkParallelMatchesSerial("Num=2 Stps=33 C0=\"PATH_ID + s\" X0=C0 Y0=0 X4=s Y4=C0", P::Frame);
    // Key read before the step writes it (X0 reads Y0): previous step -> serial.
    checkParallelMatchesSerial("Stps=50 X0=\"Y0 + s\" Y0=\"X0 * .5\"", P::Serial);
    // rnd() in the step tier -> serial (RNG order).
    checkParallelMatchesSerial("Num=2 Stps=50 X0=\"rnd(1)\" Y0=s", P::Serial);
    // rnd() only in A/B: still parallel.
    checkParallelMatchesSerial("Num=3 Stps=80 B0=\"rnd(2)\" X0=\"s * B0\" Y0=\"B0\"", P::Frame);
    // Stps=1 and odd step counts below the lane count.
    checkParallelMatchesSerial("Num=2 Stps=1 X0=ID Y0=t", P::Frame);
    checkParallelMatchesSerial("Num=2 Stps=3 C0=\"C0 + 1\" X0=C0 Y0=s", P::Frame);
    // Lane slicing: rows over s from B read per step (C, outputs, mag of a row),
    // a 2 x N matrix (whole-row fallback), an output read by another key
    // (not sliced), integer/bit/compare operators, atan2 and max of two.
    checkParallelMatchesSerial(
        "Num=3 Stps=97 B0=\"s * 6.28 + ID\" B1=\".5 + .2 * mag(s * 4)\" B2=\"{ sin(B0), cos(B0) }\" "
        "C0=\"B1 * cos(B0 + t) + C0 * .5\" C1=\"(B0 % 3) + (B0 >= 2) + int(B0) + (int(B0 * 10) & 5) + atan2(B1, B0)\" "
        "X0=\"C0 + max(B1, .6) * -B1\" Y0=\"B1 * sin(B0) ^ 2\" X1=\"row(B2, 0)\" Y1=\"row(B2, 1) + X1 * 0\" "
        "Pen=\"B1 + fft(s)\" LWdt=\"1 + C1 * .01\" ConB=1",
        P::Frame);
    // row(): of a 2 x N matrix by a scalar (sliced), by a row of indices (several
    // rows: whole-row fallback), of a 1 x N row, negative / out-of-range indices.
    checkParallelMatchesSerial(
        "Num=2 Stps=41 B0=\"s * 3 + ID\" B1=\"{ sin(B0), cos(B0), B0 }\" "
        "C0=\"row(B1, 1) * 2\" C1=\"row(B1, B0 * 0)\" C2=\"row(B1, -1) + row(B1, 7) + row(B0, 3)\" "
        "X0=\"C0 + row(B1, ID)\" Y0=\"C1 + C2\" X1=\"row(B1, -2.7)\" Y1=\"row(B1, 0 / 0)\"",
        P::Frame);
}
