// M6: particles (probe #19 envelope, spawn timing), sprites (flags, fit,
// names, luminance, bouncing), catalogs and commands. All scene text here is
// our own.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "engine/Commands.hpp"
#include "engine/MediaOptions.hpp"
#include "engine/Particles.hpp"
#include "engine/SceneLibrary.hpp"
#include "engine/Sprites.hpp"
#include "engine/Themes.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;
using reforce::scene::SceneType;

namespace {

std::shared_ptr<const reforce::scene::CompiledConfig> compileText(const std::string& text, SceneType type) {
    auto file = reforce::scene::parseSceneText(text, "test");
    return std::make_shared<reforce::scene::CompiledConfig>(reforce::scene::compileConfig(file, type));
}

FadeEnvelope probeEnvelope(double fade, double hold) {
    FadeEnvelope e;
    e.fade = fade;
    e.hold = hold;
    e.fn = std::make_shared<TransitionFunction>("1-(1-t)^1.5");  // G-Force default Particle.Transition.Function
    return e;
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

}  // namespace

// ---------------------------------------------------------------- particle envelope

TEST_CASE("particle envelope: probe #19 numbers") {
    // Run 4: PDur 4, 6 s fades. Pixels 165/237/255/255 at 3/5/7/9 s, 197/93/20 at 11/13/15 s.
    const FadeEnvelope e = probeEnvelope(6.0, 4.0);
    CHECK(e.total() == doctest::Approx(16.0));
    CHECK(e(3.0) == doctest::Approx(0.6464).epsilon(0.001));   // 1 - .5^1.5
    CHECK(std::abs(e(3.0) * 255 - 165) < 2.0);
    CHECK(std::abs(e(5.0) * 255 - 237) < 2.0);
    CHECK(e(7.0) == 1.0);
    CHECK(e(9.0) == 1.0);
    // Fade-out = 1 - fn(tau/D) = (1 - tau/D)^1.5.
    CHECK(e(11.0) == doctest::Approx(std::pow(5.0 / 6.0, 1.5)).epsilon(1e-6));
    CHECK(e(13.0) == doctest::Approx(std::pow(0.5, 1.5)).epsilon(1e-6));
    // Measured: the fade-out start fits 10.05-10.1 s, i.e. one Transition.Duration.Deviation
    // sample (6 * (1 + .02 N)) above 6 s. The model is within ~.012 of the pixels.
    CHECK(std::abs(e(11.0) - 197.0 / 255) < 0.015);  // ~0.773
    CHECK(std::abs(e(13.0) - 93.0 / 255) < 0.015);
    CHECK(std::abs(e(15.0) - 20.0 / 255) < 0.015);
    const FadeEnvelope e605 = probeEnvelope(6.05, 4.0);  // a deviation sample
    CHECK(e605(11.0) == doctest::Approx(0.773).epsilon(0.005));
    // Outside the life: 0.
    CHECK(e(-0.1) == 0.0);
    CHECK(e(16.0) == 0.0);
    CHECK(e(100.0) == 0.0);
    // Run 3 fade-in (no PDur in the probe, so the default hold): monotone rise to 1.
    double prev = 0;
    for (double a = 0.5; a < 6.0; a += 0.5) {
        CHECK(e(a) > prev);
        prev = e(a);
    }
}

TEST_CASE("particle envelope: linear function, early fade-out is continuous") {
    FadeEnvelope lin;
    lin.fade = 2;
    lin.hold = 1;
    CHECK(lin(1.0) == doctest::Approx(0.5));
    CHECK(lin(4.0) == doctest::Approx(0.5));  // 1 - tau/D
    FadeEnvelope e = probeEnvelope(6.0, 25.0);
    const double v = e(10.0);
    REQUIRE(v == 1.0);
    e.fadeOutAt(10.0);
    CHECK(e(10.0) == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(e(13.0) == doctest::Approx(std::pow(0.5, 1.5)).epsilon(1e-3));
    CHECK(e.total() == doctest::Approx(16.0).epsilon(1e-3));
    // Mid fade-in: the fade-out starts from the current value.
    FadeEnvelope f = probeEnvelope(6.0, 25.0);
    const double mid = f(3.0);
    f.fadeOutAt(3.0);
    CHECK(f(3.0) == doctest::Approx(mid).epsilon(1e-3));
    CHECK(f(3.5) < mid);
    CHECK(f.total() < 9.0);
}

TEST_CASE("particle envelope: kill fade-out over its own (quick) duration") {
    FadeEnvelope e;
    e.fade = 6;
    e.hold = 25;  // linear fn
    e.fadeOutAt(10.0, 1.0);  // kill at full pen, Quick = 1 s
    CHECK(e(10.0) == doctest::Approx(1.0));
    CHECK(e(10.5) == doctest::Approx(0.5));
    CHECK(e.total() == doctest::Approx(11.0));
    CHECK(e(11.0) == 0.0);
    // A slower fade requested later does not lengthen it; a faster one shortens it.
    e.fadeOutAt(10.5, 6.0);
    CHECK(e.total() == doctest::Approx(11.0));
    e.fadeOutAt(10.5, 0.25);
    CHECK(e(10.5) == doctest::Approx(0.5));
    CHECK(e.total() == doctest::Approx(10.625));
    // Mid regular fade-out: a quick kill continues from the current value, faster.
    FadeEnvelope g;
    g.fade = 2;
    g.hold = 1;
    g.fadeOutAt(4.0, 0.5);  // value 0.5 at age 4
    CHECK(g(4.0) == doctest::Approx(0.5));
    CHECK(g.total() == doctest::Approx(4.25));
    // A 0 s kill cuts.
    FadeEnvelope h;
    h.fadeOutAt(2.0, 0.0);
    CHECK(h(2.0) == 0.0);
    CHECK(h.total() == doctest::Approx(2.0));
}

TEST_CASE("particle system: kill fades out instead of vanishing") {
    auto cfg = compileText("Stps=2 ConB=1 X0=\"s\" Y0=\"0\" Pen=\"1\" PDur=\"100\"", SceneType::Particle);
    reforce::scene::Environment env;
    ParticleSystem ps;
    ps.spawn(std::make_unique<ParticleInstance>(cfg, env, "a", 0.0, 1.0, 5.0, nullptr));
    ps.fadeOutAll(10.0, 2.0);
    std::vector<LineSegment> segs;
    reforce::scene::WaveShapeFrameInput in;
    in.t = 11.0;
    ps.step(in, FrameMapping{320, 180, 1, 1}, LineStyle{}, segs);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].penA == doctest::Approx(0.5f));
    in.t = 12.0;
    segs.clear();
    ps.step(in, FrameMapping{320, 180, 1, 1}, LineStyle{}, segs);
    CHECK(ps.alive().empty());
    ps.spawn(std::make_unique<ParticleInstance>(cfg, env, "b", 12.0, 1.0, 5.0, nullptr));
    ps.fadeOutAll(12.5, 0.0);  // Quick = 0: cut
    CHECK(ps.alive().empty());
}

TEST_CASE("particle spawn timer: interval statistics with a fake clock") {
    LayerPrefs lp;  // Particle defaults: 120 s +- 10 %
    lp.slideshowInterval = 120;
    lp.slideshowDeviation = 0.1;
    LayerSlideshow show(lp);
    SchedRng rng(42);
    std::vector<double> spawns;
    for (double t = 0; t < 120.0 * 1500; t += 0.05)
        if (pollSpawn(show, t, rng)) spawns.push_back(t);
    REQUIRE(spawns.size() > 1000);
    CHECK(spawns.front() > 60.0);  // no spawn at startup: the first one comes after an interval
    double sum = 0, sum2 = 0;
    for (std::size_t i = 1; i < spawns.size(); ++i) {
        const double d = spawns[i] - spawns[i - 1];
        sum += d;
        sum2 += d * d;
    }
    const double n = double(spawns.size() - 1), mean = sum / n, sd = std::sqrt(sum2 / n - mean * mean);
    CHECK(mean == doctest::Approx(120.0).epsilon(0.01));
    CHECK(sd == doctest::Approx(12.0).epsilon(0.1));
    // Held / disabled: no spawns.
    show.setHeld(true);
    CHECK(!pollSpawn(show, 1e9, rng));
}

TEST_CASE("particle instance: START_TIME/END_TIME/dt/FADER, Pen x envelope, NUM default 1 (probe #43)") {
    auto cfg = compileText("Stps=2 ConB=0\nX0=\"dt\" Y0=\"FADER\"\nX1=\"END_TIME - START_TIME\" Y1=\"START_TIME\"\n"
                           "Pen=\"1\" PDur=\"4\"\n",
                           SceneType::Particle);
    reforce::scene::Environment env;
    auto fn = std::make_shared<TransitionFunction>("1-(1-t)^1.5");
    ParticleInstance p(cfg, env, "Test", 10.0, 6.0, 25.0, fn);
    CHECK(p.instance().numInstances() == 1);
    CHECK(p.envelope().hold == 4.0);  // PDur wins over the default
    CHECK(p.endTime() == 26.0);
    reforce::scene::WaveShapeFrameInput in;
    in.t = 13.0;
    MorphFrame out;
    p.evaluate(in, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].x[0] == doctest::Approx(3.0 / 16.0));
    CHECK(out[0].y[0] == doctest::Approx(0.6464).epsilon(0.001));
    CHECK(out[0].pen[0] == doctest::Approx(0.6464).epsilon(0.001));
    CHECK(out[1].x[0] == doctest::Approx(10.0));  // END_TIME = START + 5 + (6 + 4) / 2 (probe #43)
    CHECK(out[1].y[0] == doctest::Approx(10.0));
    // Without PDur the default hold applies.
    auto noPdur = compileText("X0=\"0\" Y0=\"0\"", SceneType::Particle);
    ParticleInstance q(noPdur, env, "Q", 0.0, 6.0, 25.0, fn);
    CHECK(q.envelope().hold == 25.0);
    CHECK(q.expired(37.0));
    CHECK(!q.expired(36.9));
}

TEST_CASE("particle system: spawn order, expiry, cap") {
    auto cfg = compileText("Stps=4 ConB=1 X0=\"s\" Y0=\"0\" Pen=\"1\" PDur=\"1\"", SceneType::Particle);
    reforce::scene::Environment env;
    ParticleSystem ps;
    ps.spawn(std::make_unique<ParticleInstance>(cfg, env, "a", 0.0, 1.0, 5.0, nullptr));
    ps.spawn(std::make_unique<ParticleInstance>(cfg, env, "b", 1.0, 1.0, 5.0, nullptr));
    CHECK(ps.names() == std::vector<std::string>{"a", "b"});
    std::vector<LineSegment> segs;
    reforce::scene::WaveShapeFrameInput in;
    in.t = 1.5;
    ps.step(in, FrameMapping{320, 180, 1, 1}, LineStyle{}, segs);
    CHECK(segs.size() == 6);       // 3 segments each
    CHECK(segs[0].penA == 1.0f);   // a: full pen
    CHECK(segs[3].penA == doctest::Approx(0.5f));  // b: half-way through a linear 1 s fade-in
    in.t = 3.2;                    // a ended at 3.0
    segs.clear();
    ps.step(in, FrameMapping{320, 180, 1, 1}, LineStyle{}, segs);
    CHECK(ps.names() == std::vector<std::string>{"b"});
    for (int i = 0; i < 20; ++i)
        ps.spawn(std::make_unique<ParticleInstance>(cfg, env, "x", 3.0, 1.0, 5.0, nullptr));
    CHECK(ps.alive().size() == ParticleSystem::kMaxAlive);
    ps.killAll();
    CHECK(ps.alive().empty());
}

// ---------------------------------------------------------------- sprites

TEST_CASE("sprite flags: parsing and blend precedence") {
    CHECK(describeSpriteFlags(16397) == "AlphaBlend|DontCrop|MaintainAspect|ShrinkToFit");
    CHECK(16397u == (sprite::AlphaBlend | sprite::DontCrop | sprite::MaintainAspect | sprite::ShrinkToFit));
    CHECK(describeSpriteFlags(7) == "MaintainAspect|EnlargeToFit|ShrinkToFit");
    CHECK(describeSpriteFlags(20) == "Invert|MaintainAspect");  // [DS] "Flags=20" example
    CHECK(describeSpriteFlags(0) == "0");
    CHECK(describeSpriteFlags(32 | 1) == "ShrinkToFit|32");
    CHECK(spriteBlendMode(16397) == SpriteBlend::AlphaBlend);
    CHECK(spriteBlendMode(7) == SpriteBlend::Copy);
    CHECK(spriteBlendMode(sprite::ThresholdCopy) == SpriteBlend::Threshold);
    CHECK(spriteBlendMode(sprite::AdditiveBlend | sprite::ThresholdCopy) == SpriteBlend::Additive);
    CHECK(spriteBlendMode(sprite::SelectiveBlend | sprite::AdditiveBlend) == SpriteBlend::Selective);
    CHECK(spriteBlendMode(sprite::AlphaMask | sprite::SelectiveBlend) == SpriteBlend::AlphaMask);
}

TEST_CASE("sprite fit: shrink/enlarge, aspect, crop") {
    // Default Flag=7: cover the frame (aspect kept, overflow cropped).
    SpriteSize s = fitSprite(1388, 842, 1920, 1080, 7);
    CHECK(s.w == doctest::Approx(1920));
    CHECK(s.h == doctest::Approx(842 * 1920.0 / 1388));
    CHECK(coversFrame(s, 1920, 1080));
    // + DontCrop: fit inside.
    s = fitSprite(1388, 842, 1920, 1080, 7 | sprite::DontCrop);
    CHECK(s.h == doctest::Approx(1080));
    CHECK(s.w == doctest::Approx(1388 * 1080.0 / 842));
    CHECK(!coversFrame(s, 1920, 1080));
    // G-Force Logo (16397: shrink only, aspect, don't crop): native when it fits...
    s = fitSprite(440, 300, 1920, 1080, 16397);
    CHECK(s.w == 440);
    CHECK(s.h == 300);
    // ...shrunk to fit inside a small frame.
    s = fitSprite(440, 300, 320, 180, 16397);
    CHECK(s.h == doctest::Approx(180));
    CHECK(s.w == doctest::Approx(264));
    // Shrink only never enlarges; enlarge only never shrinks.
    s = fitSprite(100, 50, 1920, 1080, sprite::ShrinkToFit | sprite::MaintainAspect);
    CHECK(s.w == 100);
    s = fitSprite(4000, 3000, 1920, 1080, sprite::EnlargeToFit | sprite::MaintainAspect);
    CHECK(s.w == 4000);
    // Without MaintainAspect each axis is fitted on its own (stretch).
    s = fitSprite(3000, 500, 1920, 1080, sprite::ShrinkToFit);
    CHECK(s.w == 1920);
    CHECK(s.h == 500);
    s = fitSprite(100, 100, 1920, 1080, sprite::ShrinkToFit | sprite::EnlargeToFit);
    CHECK(s.w == 1920);
    CHECK(s.h == 1080);
    // No flags: native size.
    s = fitSprite(297, 284, 1920, 1080, 0);
    CHECK(s.w == 297);
    CHECK(s.h == 284);
}

TEST_CASE("sprite names: ~ invert, parentheses, spaces, image files") {
    CHECK(parseSpriteName("Classic~").invert);
    CHECK(parseSpriteName("Andy~ ").invert);
    CHECK(!parseSpriteName("Comet").invert);
    CHECK(!parseSpriteName("Some~thing").invert);
    CHECK(parseSpriteName("(G-Force Logo)").scriptOnly);
    const SpriteName dj = parseSpriteName("  dj test logo");
    CHECK(dj.name == "  dj test logo");
    CHECK(!dj.invert);
    CHECK(!dj.scriptOnly);
    CHECK(isSpriteImageFile("Pkg/  dj test logo.png"));
    CHECK(isSpriteImageFile("Space/Comet.JPG"));
    CHECK(isSpriteImageFile("x.jpeg"));
    CHECK(!isSpriteImageFile("Space/Comet-icon.jpg"));
    CHECK(!isSpriteImageFile("_default_sprite_params.txt"));
    CHECK(!isSpriteImageFile("_hidden.png"));
    CHECK(!isSpriteImageFile("Space/Astronaut.txt"));
}

TEST_CASE("sprite luminance: Rec. 601, invert, premultiplied alpha") {
    const std::uint8_t opaque[] = {255, 255, 255, 255, 0, 0, 0, 255, 255, 0, 0, 255};
    LumaImage l = toLumaAlpha(opaque, 3, 1, false, false);
    CHECK(l.la[0] == 255);
    CHECK(l.la[1] == 255);  // no alpha channel: alpha = luminance
    CHECK(l.la[2] == 0);
    CHECK(l.la[3] == 0);
    CHECK(l.la[4] == 76);   // .299 * 255
    l = toLumaAlpha(opaque, 3, 1, true, false);
    CHECK(l.la[0] == 0);
    CHECK(l.la[2] == 255);
    CHECK(l.la[3] == 255);  // inverted luminance is the alpha
    CHECK(!rgbaHasAlpha(opaque, 3, 1));
    // Premultiplied half-transparent white -> straight luminance 1, alpha .5.
    const std::uint8_t half[] = {128, 128, 128, 128, 0, 0, 0, 0};
    CHECK(rgbaHasAlpha(half, 2, 1));
    l = toLumaAlpha(half, 2, 1, false, true);
    CHECK(l.la[0] == 255);
    CHECK(l.la[1] == 128);
    CHECK(l.la[2] == 0);
    CHECK(l.la[3] == 0);
    l = toLumaAlpha(half, 2, 1, true, true);
    CHECK(l.la[0] == 0);
    CHECK(l.la[1] == 128);  // invert keeps the alpha channel
}

TEST_CASE("sprite XPOS/YPOS: bouncing inside the frame") {
    Bouncer b;
    b.x0 = 0.9;
    b.vx = 0.2;
    b.y0 = -0.5;
    b.vy = -1.0;
    auto p = b.at(1.0);
    CHECK(p[0] == doctest::Approx(0.9));   // 1.1 reflected
    CHECK(p[1] == doctest::Approx(-0.5));  // -1.5 reflected
    p = b.at(10.0);
    CHECK(std::abs(p[0]) <= 1.0);
    CHECK(std::abs(p[1]) <= 1.0);
    // Speed: Sped pixels/s on a 1260 px tall frame.
    const Bouncer m = makeBouncer(126.0, 1980, 1260, 0.0, 0.0, 0.5, 0.5);
    CHECK(m.x0 == 0.0);
    CHECK(m.vx == doctest::Approx(126.0 * 2 / 1980));
    CHECK(m.vy == doctest::Approx(0.0).epsilon(1e-12));
    for (double t = 0; t < 200; t += 0.37) {
        const auto q = m.at(t);
        CHECK(std::abs(q[0]) <= 1.0);
    }
}

TEST_CASE("sprite run: params, flags, envelope as opacity, background-only") {
    auto fn = std::make_shared<TransitionFunction>("1-(1-t)^1.5");
    reforce::scene::Environment env;
    env.width = 320;
    env.height = 180;
    // Logo-like params (our own text): EPen only, Flag 16397.
    auto logo = compileText("X=\"0\" Y=\"0\" EPen=\".4\" Flag=\"16397\"", SceneType::Sprite);
    SpriteRun r(logo, env, "Logo", false, 0.0, 6.0, 45.0, fn);
    CHECK(r.flags() == 16397u);
    CHECK(!r.hasPen());
    CHECK(!r.invert());
    r.setImageSize(440, 300, 320, 180);
    const SpriteDrawSpec d = r.evaluate(3.0, 320, 180);
    CHECK(d.cx == 160);
    CHECK(d.cy == 90);
    CHECK(d.w == doctest::Approx(264));
    CHECK(!d.hasPen);
    CHECK(d.epen == doctest::Approx(0.4));  // raw; the x0.5 of probe #22 is applied by the layering rules
    CHECK(d.hasEPen);
    CHECK(d.opacity == doctest::Approx(0.6464).epsilon(0.001));
    CHECK(d.blend == SpriteBlend::AlphaBlend);
    CHECK(r.endTime() == doctest::Approx(57.0));
    // A sibling .txt replaces the defaults as a whole: no Flag -> 0 (native, copy).
    auto astro = compileText("X=\"XPOS\" Y=\"-.5\" Pen=\"1\" EPen=\"FADER\" Invt=1 PDur=\"10\"", SceneType::Sprite);
    SpriteRun a(astro, env, "Astro~", true, 1.0, 2.0, 45.0, nullptr);
    CHECK(a.flags() == 0u);
    CHECK(a.invert());
    CHECK(a.hasPen());
    CHECK(a.envelope().hold == 10.0);
    a.setImageSize(30, 20, 320, 180);
    const SpriteDrawSpec e = a.evaluate(2.0, 320, 180);
    CHECK(e.w == 30);
    CHECK(e.cy == doctest::Approx(130));  // Y = -.5 x (1 - 20/180) (probe #41)
    CHECK(e.opacity == doctest::Approx(0.5));
    CHECK(e.epen == doctest::Approx(0.5));  // EPen = FADER
    CHECK(e.blend == SpriteBlend::Copy);
}

TEST_CASE("sprite run: play-once movies fit their lifetime to the movie") {
    reforce::scene::Environment env;
    env.width = 320;
    env.height = 180;
    auto deflt = compileText("X=\"0\" Y=\"0\" EPen=\"1\" Flag=7", SceneType::Sprite);
    // Longer than the default hold (45 s): extended so the whole movie plays.
    SpriteRun lng(deflt, env, "long", false, 10.0, 0.9, 45.0, nullptr);
    lng.fitLifetime(162.0);
    CHECK(lng.endTime() == doctest::Approx(172.0));
    CHECK(lng.envelope().fade == doctest::Approx(0.9));
    CHECK(!lng.expired(171.9));
    // Shorter: cut down, the fade-out ends with the movie.
    SpriteRun shrt(deflt, env, "short", false, 0.0, 6.0, 45.0, nullptr);
    shrt.fitLifetime(20.0);
    CHECK(shrt.endTime() == doctest::Approx(20.0));
    CHECK(shrt.envelope().hold == doctest::Approx(8.0));
    SpriteRun tiny(deflt, env, "tiny", false, 0.0, 6.0, 45.0, nullptr);
    tiny.fitLifetime(4.0);
    CHECK(tiny.envelope().fade == doctest::Approx(2.0));
    CHECK(tiny.endTime() == doctest::Approx(4.0));
    // PDur in the params: kept when the movie is longer, cut when shorter.
    auto pdur = compileText("X=\"0\" Y=\"0\" EPen=\"1\" PDur=\"10\"", SceneType::Sprite);
    SpriteRun a(pdur, env, "pdur", false, 0.0, 1.0, 45.0, nullptr);
    a.fitLifetime(60.0);
    CHECK(a.endTime() == doctest::Approx(12.0));
    a.fitLifetime(5.0);
    CHECK(a.endTime() == doctest::Approx(5.0));
}

// ---------------------------------------------------------------- catalogs, params, commands

TEST_CASE("catalog: sprite images, particle-also WaveShapes, sprite params lookup") {
    const fs::path root = fs::temp_directory_path() / "reforce_m6_catalog";
    fs::remove_all(root);
    const fs::path sp = root / "Packages/G-Force.Sprite.package";
    const fs::path ws = root / "Packages/G-Force.WaveShape.package";
    writeFile(sp / "_default_sprite_params.txt", "X=\"0\" Y=\"0\" EPen=\"1\" Flag=7\n");
    writeFile(sp / "Logo.png", "x");
    writeFile(sp / "Logo.txt", "EPen=\".4\" Flag=\"16397\"\n");
    writeFile(sp / "Logo-icon.jpg", "x");
    writeFile(sp / "Space/Comet.jpg", "x");
    writeFile(sp / "mine/  dj test logo.png", "x");
    writeFile(sp / "(Script Only).png", "x");
    writeFile(ws / "WaveShapes/Also.txt", "// #waveshape-particle-also\nX0=\"s\" Y0=\"0\"\n");
    writeFile(ws / "WaveShapes/Plain.txt", "X0=\"s\" Y0=\"0\"\n");
    writeFile(ws / "Particles/Dots.txt", "// #waveshape-particle-only\nX0=\"s\" Y0=\"0\"\n");

    const auto sprites = listScenes(root, SceneType::Sprite);
    std::vector<std::string> ids;
    for (const auto& e : sprites) ids.push_back(e.id);
    CHECK(ids == std::vector<std::string>{"(Script Only)", "Logo", "mine/  dj test logo", "Space/Comet"});
    const auto parts = listScenes(root, SceneType::Particle);
    ids.clear();
    for (const auto& e : parts) ids.push_back(e.id);
    CHECK(ids == std::vector<std::string>{"Particles/Dots", "WaveShapes/Also"});
    const auto waves = listScenes(root, SceneType::WaveShape);
    CHECK(waves.size() == 2);  // Also + Plain (the tag adds, it does not move)

    auto logo = spriteParamsFor(sp / "Logo.png");
    CHECK(logo.text.find("16397") != std::string::npos);
    CHECK(fs::path(logo.origin).filename() == "Logo.txt");
    auto comet = spriteParamsFor(sp / "Space/Comet.jpg");  // default found one folder up
    CHECK(fs::path(comet.origin).filename() == "_default_sprite_params.txt");
    auto dj = spriteParamsFor(sp / "mine/  dj test logo.png");
    CHECK(fs::path(dj.origin).filename() == "_default_sprite_params.txt");
    fs::remove(sp / "_default_sprite_params.txt");
    CHECK(spriteParamsFor(sp / "Space/Comet.jpg").origin == "builtin");
    fs::remove_all(root);
}

TEST_CASE("themes: Particle and Sprite expressions filter like the other types") {
    ThemeLibrary lib;
    lib.addCategory("Sprite", "CategoryTitle = \"Space\"\nInclude.Pattern = \"^(Comet|Moon)$\"\n");
    lib.addCategory("Particle", "CategoryTitle = \"Calm\"\nInclude.List = \"\nDots\n\"\n");
    lib.addTheme("ThemeName = \"Night\"\nSprite.CategoryExpr = \"+Space\"\nParticle.CategoryExpr = \"+(Suggested)\\t-Calm\"\n");
    const Theme* night = lib.find("Night");
    REQUIRE(night);
    CHECK(lib.filter(night, "Sprite", {"Comet", "Logo", "Moon"}) == std::vector<std::string>{"Comet", "Moon"});
    CHECK(lib.filter(night, "Particle", {"Dots", "Rain"}) == std::vector<std::string>{"Rain"});
}

TEST_CASE("commands: start / kill particles and sprites") {
    auto c = parseCommand("start:particle:Raindrops");
    REQUIRE(c);
    CHECK(c->type == CommandType::Switch);
    CHECK(c->layer == Layer::Particle);
    CHECK(c->text == "Raindrops");
    CHECK_FALSE(c->quick);  // scripted start (vis.StartParticle): the regular Transition.Duration
    c = parseCommand("switch:sprite:  dj test logo");
    REQUIRE(c);
    CHECK(c->quick);  // manual: Transition.Duration.Quick
    CHECK(c->layer == Layer::Sprite);
    CHECK(c->text == "  dj test logo");
    c = parseCommand("kill:particles");
    REQUIRE(c);
    CHECK(c->type == CommandType::Kill);
    CHECK(c->layer == Layer::Particle);
    CHECK(describe(Command::kill(Layer::Sprite)) == "Sprite kill");
    c = parseCommand("next:sprite");
    REQUIRE(c);
    CHECK(c->target == SwitchTarget::Next);
    CHECK(c->arg.empty());
    // Image / Movie sprites cards.
    c = parseCommand("kill:sprite:images");
    REQUIRE(c);
    CHECK((c->type == CommandType::Kill && c->text == "images"));
    CHECK(describe(*c) == "Sprite kill images");
    c = parseCommand("kill:sprite:movies");
    REQUIRE(c);
    CHECK(c->text == "movies");
    CHECK_FALSE(parseCommand("kill:sprite:cats"));
    c = parseCommand("nextmovie:sprite");
    REQUIRE(c);
    CHECK((c->type == CommandType::Switch && c->target == SwitchTarget::Next && c->arg == "movie"));
    CHECK(describe(*c) == "Sprite switch next movie (quick)");
    CHECK_FALSE(parseCommand("nextmovie:particle"));
    c = parseCommand("kill:union");
    REQUIRE(c);
    CHECK((c->type == CommandType::Kill && c->layer == Layer::Union));
}

TEST_CASE("skip list: reforce.Exclude parsing and matching") {
    using reforce::engine::SceneEntry;
    using reforce::scene::SceneType;
    std::vector<std::string> errors;
    const auto rules = reforce::engine::parseExcludeList(
        "Sprite:Space/Moon; sprite: space\\Astronaut.jpg ;\nSprite:G-Force Logo;Bogus:Thing; Lust", &errors);
    REQUIRE(rules.size() == 4);
    CHECK(errors.size() == 1);
    const SceneEntry moon{"Space/Moon", "Moon", "x/Space/Moon.jpg"};
    const SceneEntry astro{"Space/Astronaut", "Astronaut", "x/Space/Astronaut.jpg"};
    const SceneEntry logo{"G-Force Logo", "G-Force Logo", "x/G-Force Logo.png"};
    const SceneEntry bigMoon{"Space/Big Moon", "Big Moon", "x/Space/Big Moon.jpg"};
    const SceneEntry lust{"Gold/Lust", "Lust", "x/Gold/Lust.txt"};
    CHECK(reforce::engine::isExcluded(rules, SceneType::Sprite, moon));
    CHECK(reforce::engine::isExcluded(rules, SceneType::Sprite, astro));
    CHECK(reforce::engine::isExcluded(rules, SceneType::Sprite, logo));
    CHECK_FALSE(reforce::engine::isExcluded(rules, SceneType::Sprite, bigMoon));
    CHECK_FALSE(reforce::engine::isExcluded(rules, SceneType::WaveShape, moon));  // typed rule
    CHECK(reforce::engine::isExcluded(rules, SceneType::ColorMap, lust));        // untyped rule, by stem
    std::vector<SceneEntry> v{moon, bigMoon, logo};
    CHECK(reforce::engine::removeExcluded(v, SceneType::Sprite, rules) == 2);
    REQUIRE(v.size() == 1);
    CHECK(v[0].id == "Space/Big Moon");
    CHECK(reforce::engine::parseExcludeList("").empty());
}

TEST_CASE("id list: reforce.Sprite.Hidden parsing and matching") {
    using namespace reforce::engine;
    const auto ids = parseIdList("Abstract/Rose; External/images/  dj peter henry;\r\nabstract/ROSE;;  \nSpace\\Comet");
    REQUIRE(ids.size() == 3);  // the case-different duplicate and the empty items dropped
    CHECK(idListHas(ids, "Abstract/Rose"));
    CHECK(idListHas(ids, "abstract/rose"));
    CHECK(idListHas(ids, "External/images/  dj peter henry"));  // inner spaces are part of the id
    CHECK_FALSE(idListHas(ids, "External/images/ dj peter henry"));
    CHECK(idListHas(ids, "Space/Comet"));
    CHECK_FALSE(idListHas(ids, "Rose"));  // a bare stem is not an id
    CHECK_FALSE(idListHas(ids, ""));
    CHECK(parseIdList(joinIdList(ids)) == ids);
    CHECK(parseIdList("").empty());
}

TEST_CASE("media options: <stem>.reforce.txt beside a movie") {
    using namespace reforce::engine;
    CHECK(mediaOptionsPath("/m/ dance_static.mov") == std::filesystem::path("/m/ dance_static.reforce.txt"));
    const MediaOptions o = parseMediaOptions("Loop = 1  // repeat\nRate=\"0.5\"\n start=12\nBogus=3\nRate=-1\n");
    REQUIRE(o.loop);
    CHECK(*o.loop);
    REQUIRE(o.rate);
    CHECK(*o.rate == doctest::Approx(0.5));  // the negative one is ignored
    REQUIRE(o.start);
    CHECK(*o.start == doctest::Approx(12));
    CHECK(parseMediaOptions("Loop=off").loop == std::optional<bool>(false));
    CHECK(!parseMediaOptions("// nothing\n").loop);
    CHECK(!loadMediaOptions("/nonexistent/x.mov").loop);
}

TEST_CASE("media options: setMediaColorKey writes and clears ColorKey") {
    using namespace reforce::engine;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "reforce_test_colorkey";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path movie = dir / "a.mov";
    const fs::path opts = mediaOptionsPath(movie);
    auto read = [&] {
        std::ifstream f(opts);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    CHECK(setMediaColorKey(movie, true).empty());  // keyed is the default: no file made
    CHECK_FALSE(fs::exists(opts));
    CHECK(setMediaColorKey(movie, false).empty());
    CHECK(read() == "ColorKey = 0\n");
    CHECK(loadMediaOptions(movie).colorKey == std::optional<bool>(false));
    CHECK(setMediaColorKey(movie, true).empty());  // nothing else in it: removed
    CHECK_FALSE(fs::exists(opts));
    {
        std::ofstream f(opts);
        f << "Loop = 1\ncolorkey=\"1\"  // old\nBPM = 120\n";
    }
    CHECK(setMediaColorKey(movie, false).empty());  // in place of the old line, the rest kept
    CHECK(read() == "Loop = 1\nColorKey = 0\nBPM = 120\n");
    CHECK(setMediaColorKey(movie, true).empty());
    CHECK(read() == "Loop = 1\nBPM = 120\n");
    CHECK(loadMediaOptions(movie).loop == std::optional<bool>(true));
    fs::remove_all(dir);
}

TEST_CASE("media options: End, and m:ss times") {
    using namespace reforce::engine;
    const MediaOptions o = parseMediaOptions("Start = 0:12\nEnd = 1:05.5\n");
    REQUIRE(o.start);
    CHECK(*o.start == doctest::Approx(12));
    REQUIRE(o.end);
    CHECK(*o.end == doctest::Approx(65.5));
    CHECK(!parseMediaOptions("Start=20\nEnd=10").end);  // End must be after Start
    CHECK(parseMediaOptions("End=10").end == std::optional<double>(10.0));
}

TEST_CASE("media options: Black / White / Gamma levels (PLAN 5K)") {
    using namespace reforce::engine;
    const MediaOptions o = parseMediaOptions("Black = 0.05\nWhite = \"0.9\"  // levels\ngamma=0.7\nBPM = 120\n");
    REQUIRE(o.black);
    CHECK(*o.black == doctest::Approx(0.05));
    REQUIRE(o.white);
    CHECK(*o.white == doctest::Approx(0.9));
    REQUIRE(o.gamma);
    CHECK(*o.gamma == doctest::Approx(0.7));
    CHECK(o.bpm == std::optional<double>(120.0));  // the other keys stay
    const MediaOptions none = parseMediaOptions("Loop = 1\n");
    CHECK((!none.black && !none.white && !none.gamma));
    // Out of range: ignored.
    CHECK(!parseMediaOptions("Black = -0.1").black);
    CHECK(!parseMediaOptions("White = 1.5").white);
    CHECK(!parseMediaOptions("Gamma = 0").gamma);
    CHECK(!parseMediaOptions("Gamma = -2").gamma);
    CHECK(!parseMediaOptions("Gamma = x").gamma);
    // White must be above Black (also against the defaults 0 and 1): otherwise both are ignored.
    const MediaOptions flipped = parseMediaOptions("Black = 0.6\nWhite = 0.4\nGamma = 2\n");
    CHECK((!flipped.black && !flipped.white));
    CHECK(flipped.gamma == std::optional<double>(2.0));
    CHECK(!parseMediaOptions("White = 0").white);
    CHECK(parseMediaOptions("Black = 0.3").black == std::optional<double>(0.3));
    // An interpolated copy reads its original's options file.
    CHECK(mediaOptionsPath("/m/clip.rife4x.mov") == std::filesystem::path("/m/clip.reforce.txt"));
}

TEST_CASE("sprite layering plans (probes #40, #42)") {
    SpriteDrawSpec epenOnly;
    epenOnly.hasEPen = true;
    epenOnly.epen = 1.0;
    // gforce: EPen-only = display-only half-way lighten, nothing into the feedback.
    SpriteLayerPlan g = spriteLayerPlan(epenOnly, SpriteLayering::GForce, 0.35);
    CHECK(g.feedbackCount == 0);
    CHECK(g.hasDisplay);
    CHECK(g.display.op == SpriteOp::Fetch);
    CHECK(g.display.blend == SpriteBlend::Copy);  // Copy under Fetch = the half-way lighten
    CHECK(g.display.pen == doctest::Approx(1.0));
    CHECK(g.feedbackBeforeLines);
    // trails: the same display plus a max feed at EPen * amount.
    SpriteLayerPlan tr = spriteLayerPlan(epenOnly, SpriteLayering::Trails, 0.35);
    CHECK(tr.hasDisplay);
    REQUIRE(tr.feedbackCount == 1);
    CHECK(tr.feedback[0].op == SpriteOp::Blend);
    CHECK(tr.feedback[0].blend == SpriteBlend::Selective);
    CHECK(tr.feedback[0].pen == doctest::Approx(0.35));
    // melt: the old behavior, EPen x0.5 into the feedback after the lines.
    SpriteLayerPlan m = spriteLayerPlan(epenOnly, SpriteLayering::Melt, 0.35);
    CHECK(!m.hasDisplay);
    REQUIRE(m.feedbackCount == 1);
    CHECK(m.feedback[0].pen == doctest::Approx(0.5));
    CHECK(!m.feedbackBeforeLines);
    // Pen + EPen .5: Pen keyed into the feedback, EPen keyed onto the display.
    SpriteDrawSpec both = epenOnly;
    both.hasPen = true;
    both.pen = 1.0;
    both.epen = 0.5;
    SpriteLayerPlan b = spriteLayerPlan(both, SpriteLayering::GForce, 0.35);
    REQUIRE(b.feedbackCount == 1);
    CHECK(b.feedback[0].op == SpriteOp::KeyedCopy);
    CHECK(b.display.op == SpriteOp::KeyedCopy);
    CHECK(b.display.pen == doctest::Approx(0.5));
    // Pen only (EPen absent): no display draw, the stamp shows.
    SpriteDrawSpec pen = both;
    pen.hasEPen = false;
    pen.epen = 0;
    SpriteLayerPlan p = spriteLayerPlan(pen, SpriteLayering::GForce, 0.35);
    CHECK(p.feedbackCount == 1);
    CHECK(!p.hasDisplay);
    // Probe #42: other blend flags read the target with their measured formula, at the full pen.
    SpriteDrawSpec add = epenOnly;
    add.blend = SpriteBlend::Additive;
    SpriteLayerPlan a = spriteLayerPlan(add, SpriteLayering::GForce, 0.35);
    CHECK(a.display.op == SpriteOp::Fetch);
    CHECK(a.display.blend == SpriteBlend::Additive);
    CHECK(a.display.pen == doctest::Approx(1.0));
    SpriteDrawSpec penAdd = pen;
    penAdd.blend = SpriteBlend::AlphaBlend;
    SpriteLayerPlan pa = spriteLayerPlan(penAdd, SpriteLayering::GForce, 0.35);
    REQUIRE(pa.feedbackCount == 1);
    CHECK(pa.feedback[0].op == SpriteOp::Fetch);
    CHECK(pa.feedback[0].blend == SpriteBlend::AlphaBlend);
    // ThresholdCopy: a keyed copy even for an EPen-only sprite; with Pen like the default.
    SpriteDrawSpec thr = epenOnly;
    thr.blend = SpriteBlend::Threshold;
    CHECK(spriteLayerPlan(thr, SpriteLayering::GForce, 0.35).display.op == SpriteOp::KeyedCopy);
    SpriteDrawSpec penThr = pen;
    penThr.blend = SpriteBlend::Threshold;
    CHECK(spriteLayerPlan(penThr, SpriteLayering::GForce, 0.35).feedback[0].op == SpriteOp::KeyedCopy);
    // Selective EPen-only: the same half-way lighten as the default.
    SpriteDrawSpec sel = epenOnly;
    sel.blend = SpriteBlend::Selective;
    CHECK(spriteLayerPlan(sel, SpriteLayering::GForce, 0.35).display.op == SpriteOp::Fetch);
    // Sprite Mix (PLAN 5K): 0 changes nothing; > 0 goes on the display draw (gforce / trails), never on the feedback.
    CHECK(g.display.mix == 0.0);
    SpriteLayerPlan gm = spriteLayerPlan(epenOnly, SpriteLayering::GForce, 0.35, 0.6);
    CHECK(gm.display.mix == doctest::Approx(0.6));
    CHECK(gm.display.op == SpriteOp::Fetch);
    SpriteLayerPlan trm = spriteLayerPlan(epenOnly, SpriteLayering::Trails, 0.35, 1.0);
    CHECK(trm.display.mix == doctest::Approx(1.0));
    REQUIRE(trm.feedbackCount == 1);
    CHECK(trm.feedback[0].mix == 0.0);
    CHECK(trm.feedback[0].pen == doctest::Approx(0.35));
    // A Pen-only sprite gets a display copy of its stamp that carries the mix.
    SpriteLayerPlan pm = spriteLayerPlan(pen, SpriteLayering::GForce, 0.35, 0.5);
    REQUIRE(pm.feedbackCount == 1);
    CHECK(pm.feedback[0].mix == 0.0);
    REQUIRE(pm.hasDisplay);
    CHECK(pm.display.op == SpriteOp::KeyedCopy);
    CHECK(pm.display.pen == doctest::Approx(1.0));
    CHECK(pm.display.mix == doctest::Approx(0.5));
    // melt: on the feedback draw, whose EPen-only 0.5 factor becomes 0.5 + 0.5 mix.
    SpriteLayerPlan mm = spriteLayerPlan(epenOnly, SpriteLayering::Melt, 0.35, 1.0);
    REQUIRE(mm.feedbackCount == 1);
    CHECK(mm.feedback[0].pen == doctest::Approx(1.0));
    CHECK(mm.feedback[0].mix == doctest::Approx(1.0));
    CHECK(spriteLayerPlan(epenOnly, SpriteLayering::GForce, 0.35, 7.0).display.mix == doctest::Approx(1.0));  // clamped
    // Draw order: G-Force puts the oldest sprite on top; melt keeps start order.
    CHECK(spriteOldestOnTop(SpriteLayering::GForce));
    CHECK(spriteOldestOnTop(SpriteLayering::Trails));
    CHECK(!spriteOldestOnTop(SpriteLayering::Melt));
    // The key threshold: 9.9 levels skipped, 10 copied (G-Force truncates to 8 bits; run 32).
    CHECK(9.9f / 255.0f < kSpriteKeyThreshold);
    CHECK(10.0f / 255.0f >= kSpriteKeyThreshold);
    CHECK(parseSpriteLayering("Melt") == SpriteLayering::Melt);
    CHECK(!parseSpriteLayering("x"));
}

TEST_CASE("sprite run: X/Y keep the sprite inside the frame (probe #41)") {
    reforce::scene::Environment env;
    env.width = 1980;
    env.height = 1260;
    // Native 600x382 at X .5, Y .3: center at .5 x (1 - 600/1980) = .348 (G-Force: .349), y .3 x .697 = .209.
    auto n5 = compileText("X=\".5\" Y=\".3\" EPen=\"1\" Flag=\"0\"", SceneType::Sprite);
    SpriteRun a(n5, env, "n5", false, 0.0, 1.0, 45.0, nullptr);
    a.setImageSize(600, 382, 1980, 1260);
    const SpriteDrawSpec d = a.evaluate(5.0, 1980, 1260);
    CHECK((d.cx / 1980 * 2 - 1) == doctest::Approx(0.348).epsilon(0.003));
    CHECK((1 - d.cy / 1260 * 2) == doctest::Approx(0.209).epsilon(0.003));
    // Cover fit (larger than the frame): X is ignored.
    auto cover = compileText("X=\"1\" Y=\"0\" EPen=\"1\" Flag=\"7\"", SceneType::Sprite);
    SpriteRun b(cover, env, "w10", false, 0.0, 1.0, 45.0, nullptr);
    b.setImageSize(800, 400, 1980, 1260);
    CHECK(b.evaluate(5.0, 1980, 1260).cx == doctest::Approx(990));
    // Contain fit of a 2:1 image: full width (X ignored), Y .3 x (1 - 990/1260) = .064.
    auto contain = compileText("X=\".2\" Y=\".3\" EPen=\"1\" Flag=\"15\"", SceneType::Sprite);
    SpriteRun c(contain, env, "c", false, 0.0, 1.0, 45.0, nullptr);
    c.setImageSize(800, 400, 1980, 1260);
    const SpriteDrawSpec e = c.evaluate(5.0, 1980, 1260);
    CHECK(e.cx == doctest::Approx(990));
    CHECK((1 - e.cy / 1260 * 2) == doctest::Approx(0.064).epsilon(0.003));
}

TEST_CASE("sprite replacement fade: linear from the current value over 1.3 s (probe #44)") {
    FadeEnvelope e;
    e.fade = 2.0;
    e.hold = 30.0;
    e.fn = std::make_shared<TransitionFunction>("1-(1-t)^1.5");
    e.fadeOutAt(5.0, kSpriteReplaceFade, true);
    CHECK(e(5.0) == doctest::Approx(1.0));
    CHECK(e(5.6) == doctest::Approx(1.0 - 0.6 / 1.3));  // G-Force .533-.541
    CHECK(e(6.2) == doctest::Approx(1.0 - 1.2 / 1.3));  // .063-.078
    CHECK(e(6.35) == 0.0);
    CHECK(e.total() == doctest::Approx(6.3));
    // From a partial envelope (still fading in): the same slope, starting at the current value.
    FadeEnvelope f = e;
    f.earlyOut = -1.0;
    f.earlyLinear = false;
    const double cur = f(1.0);
    f.fadeOutAt(1.0, kSpriteReplaceFade, true);
    CHECK(f(1.0) == doctest::Approx(cur));
    CHECK(f(1.3) == doctest::Approx(cur - 0.3 / 1.3));
}

TEST_CASE("spawn timer: interval 0 starts none, and a new interval resumes") {
    LayerPrefs lp;
    lp.slideshowInterval = 10;
    lp.slideshowDeviation = 0;
    LayerSlideshow show(lp);
    SchedRng rng(1);
    show.setInterval(0);
    for (double t = 0; t < 1000; t += 0.5) CHECK_FALSE(pollSpawn(show, t, rng));
    show.setInterval(10);
    int n = 0;
    for (double t = 1000; t < 1100.25; t += 0.5) n += pollSpawn(show, t, rng);
    CHECK(n == 10);  // first one an interval after re-arming, then every 10 s
}

TEST_CASE("slideshow settings from the panel map to settings.json keys") {
    using reforce::engine::Command;
    using reforce::engine::Layer;
    using reforce::engine::slideshowSaveEntries;
    auto one = [](const reforce::engine::PrefsTable& t, const char* key) { return t.string(key, "?"); };
    CHECK(one(slideshowSaveEntries(Command::interval(Layer::WaveShape, 30)), "WaveShape.Slideshow.Interval.Duration") == "30");
    CHECK(slideshowSaveEntries(Command::interval(Layer::WaveShape, 0)).entries().empty());  // engine ignores 0 there
    CHECK(one(slideshowSaveEntries(Command::interval(Layer::Sprite, 0)), "Sprite.Slideshow.Interval.Duration") == "0");
    CHECK(one(slideshowSaveEntries(Command::transitionDuration(Layer::FlowField, 7.5)), "FlowField.Transition.Duration") == "7.5");
    CHECK(one(slideshowSaveEntries(Command::spawnDuration(Layer::Particle, 40)), "Particle.Duration") == "40");
    CHECK(one(slideshowSaveEntries(Command::movieInterval(300)), "reforce.MovieSprite.Slideshow.Interval.Duration") == "300");
    CHECK(slideshowSaveEntries(Command::kill(Layer::Sprite)).entries().empty());
    Command all = Command::interval(Layer::WaveShape, 30);
    all.allLayers = true;
    CHECK(slideshowSaveEntries(all).entries().empty());
    // Sprite Mix (PLAN 5K): absolute values are saved, nudges (hotkeys) are not.
    CHECK(one(slideshowSaveEntries(Command::spriteMix(false, 0.5)), "reforce.Sprite.Mix") == "0.5");
    CHECK(one(slideshowSaveEntries(Command::spriteMix(true, 1)), "reforce.MovieSprite.Mix") == "1");
    CHECK(slideshowSaveEntries(Command::spriteMix(false, 0.1, true)).entries().empty());
    // Full-color movie sprites toggle (videoset spritecolor, PLAN 5K).
    CHECK(one(slideshowSaveEntries(Command::videoSet("spritecolor", "over")), "reforce.MovieSprite.Color") == "over");
    CHECK(one(slideshowSaveEntries(Command::videoSet("spritecolor", "off")), "reforce.MovieSprite.Color") == "off");
    CHECK(slideshowSaveEntries(Command::videoSet("spritecolor", "under")).entries().empty());
    // Dancer Duration 0 = forever (∞ on the card): saved; 0 for other layers is not.
    CHECK(one(slideshowSaveEntries(Command::spawnDuration(Layer::Dancer, 0)), "Dancer.Duration") == "0");
    CHECK(slideshowSaveEntries(Command::spawnDuration(Layer::Object, 0)).entries().empty());
    // Syphon alpha (docs/syphon-alpha.md).
    CHECK(one(slideshowSaveEntries(Command::videoSet("syphonalpha", "on")), "reforce.Syphon.Alpha") == "1");
    CHECK(one(slideshowSaveEntries(Command::videoSet("syphonalpha", "off")), "reforce.Syphon.Alpha") == "0");
    // The sync test flash is never saved (it must not come back on during a stream).
    CHECK(slideshowSaveEntries(Command::videoSet("beatflash", "on")).entries().empty());
    CHECK(one(slideshowSaveEntries(Command::videoSet("windowvisuals", "on")), "reforce.Window.VisualsOnly") == "1");
    CHECK(one(slideshowSaveEntries(Command::videoSet("windowvisuals", "off")), "reforce.Window.VisualsOnly") == "0");
    CHECK(one(slideshowSaveEntries(Command::videoSet("keysimilarity", "120")), "reforce.Syphon.KeySimilarity") == "120");
    CHECK(one(slideshowSaveEntries(Command::videoSet("keysmoothness", "35.5")), "reforce.Syphon.KeySmoothness") ==
          "35.5");
    CHECK(slideshowSaveEntries(Command::videoSet("keysmoothness", "0")).entries().empty());
    // Camera backdrop (docs/backdrop.md): the source keeps its spelling; off saves "".
    CHECK(one(slideshowSaveEntries(Command::videoSet("backdrop", "camera:Logitech BRIO")), "reforce.Backdrop.Source") ==
          "camera:Logitech BRIO");
    CHECK(one(slideshowSaveEntries(Command::videoSet("backdrop", "off")), "reforce.Backdrop.Source").empty());
    CHECK(one(slideshowSaveEntries(Command::videoSet("backdropmirror", "on")), "reforce.Backdrop.Mirror") == "1");
    CHECK(one(slideshowSaveEntries(Command::videoSet("backdropfit", "Contain")), "reforce.Backdrop.Fit") == "contain");
    CHECK(one(slideshowSaveEntries(Command::videoSet("backdropopacity", "0.75")), "reforce.Backdrop.Opacity") == "0.75");
    CHECK(slideshowSaveEntries(Command::videoSet("backdropopacity", "2")).entries().empty());
    CHECK(slideshowSaveEntries(Command::videoSet("backdropfit", "tile")).entries().empty());
    // Body tracking (docs/body.md).
    CHECK(one(slideshowSaveEntries(Command::videoSet("bodytrack", "on")), "reforce.Body.Track") == "1");
    CHECK(one(slideshowSaveEntries(Command::videoSet("bodyburn", "Fill")), "reforce.Body.Burn") == "fill");
    CHECK(one(slideshowSaveEntries(Command::videoSet("bodyfront", "0.6")), "reforce.Body.Front") == "0.6");
    CHECK(slideshowSaveEntries(Command::videoSet("bodyfront", "3")).entries().empty());
    CHECK(slideshowSaveEntries(Command::videoSet("bodynothing", "1")).entries().empty());
}

TEST_CASE("spritemix command: parse, describe, settings") {
    using reforce::engine::Command;
    using reforce::engine::CommandType;
    using reforce::engine::parseCommand;
    auto c = parseCommand("spritemix:sprite:0.25");
    REQUIRE(c);
    CHECK(c->type == CommandType::SetSpriteMix);
    CHECK(c->text == "sprite");
    CHECK(c->value == doctest::Approx(0.25));
    CHECK(!c->flag);
    CHECK(describe(*c) == "image sprite mix 0.25");
    c = parseCommand("spritemix:movie:+0.1");
    REQUIRE(c);
    CHECK(c->text == "movie");
    CHECK(c->flag);
    CHECK(describe(*c) == "movie sprite mix +0.10");
    CHECK(parseCommand("spritemix:movie:-0.1")->flag);
    CHECK(!parseCommand("spritemix:sprite:2"));
    CHECK(!parseCommand("spritemix:sprite:x"));
    CHECK(!parseCommand("spritemix:particle:0.5"));
    CHECK(!parseCommand("spritemix:sprite"));
    PrefsTable t;
    t.set("reforce.Sprite.Mix", "0.4", false);
    t.set("reforce.MovieSprite.Mix", "1", false);
    Settings st;
    CHECK(st.spriteMix == 0.0);
    CHECK(st.movieSpriteMix == 0.0);
    CHECK(st.movieSpriteColor == "off");
    CHECK(st.movieSpriteGamma == 1.0);
    t.set("reforce.MovieSprite.Color", "over", true);
    t.set("reforce.MovieSprite.Gamma", "0.7", false);
    st.apply(t);
    CHECK(st.movieSpriteColor == "over");
    CHECK(st.movieSpriteGamma == doctest::Approx(0.7));
    CHECK(st.spriteMix == doctest::Approx(0.4));
    CHECK(st.movieSpriteMix == doctest::Approx(1.0));
}
