// Phase 5F: video sources and the colour-layer composite math.
#include <doctest/doctest.h>

#include "PerfBudget.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

#include "engine/Prefs.hpp"
#include "engine/SceneLibrary.hpp"
#include "engine/Sprites.hpp"
#include "engine/VideoComposite.hpp"
#include "platform/SyphonPublisher.hpp"
#include "render/ScenePipeline.hpp"
#include "render/VideoMips.hpp"
#include "video/VideoSource.hpp"

namespace fs = std::filesystem;
using namespace reforce;

namespace {

fs::path tmpDir() {
    const char* env = std::getenv("REFORCE_VIDEO_TMP");
    fs::path p = env && *env ? fs::path(env) : fs::temp_directory_path() / "reforce-video-tests";
    fs::create_directories(p);
    return p;
}

struct Pixels {
    int w = 0, h = 0;
    std::vector<std::uint8_t> bgra;
    const std::uint8_t* at(int x, int y) const { return &bgra[(std::size_t(y) * w + x) * 4]; }
};

Pixels readFrame(const video::FramePtr& f) {
    Pixels p;
    if (!f || !f->texture()) return p;
    p.w = f->width();
    p.h = f->height();
    p.bgra.resize(std::size_t(p.w) * p.h * 4);
    f->texture()->getBytes(p.bgra.data(), NS::UInteger(p.w) * 4, MTL::Region(0, 0, p.w, p.h), 0);
    return p;
}

int frameIndex(const video::FramePtr& f) {
    const Pixels p = readFrame(f);
    return p.w ? video::decodeCardIndex(p.bgra.data(), p.w, p.h, std::size_t(p.w) * 4) : -1;
}

fs::path cardVideo(const char* name, video::TestCodec codec, int frames) {
    const fs::path path = tmpDir() / name;
    if (!fs::exists(path)) {
        video::TestVideoSpec s;
        s.width = 320;
        s.height = 180;
        s.fps = 30;
        s.frames = frames;
        s.codec = codec;
        std::string err;
        REQUIRE_MESSAGE(video::writeTestVideo(path, s, &err), err);
    }
    return path;
}

struct Device {
    MTL::Device* d = MTL::CreateSystemDefaultDevice();
    ~Device() {
        if (d) d->release();
    }
};

std::unique_ptr<video::VideoSource> openDet(MTL::Device* dev, const fs::path& path, double offset = 0, double rate = 1,
                                            bool loop = true) {
    video::SourceSpec s;
    s.id = path.string();
    s.offset = offset;
    s.rate = rate;
    s.loop = loop;
    video::OpenOptions o;
    o.deterministic = true;
    auto src = video::openSource(dev, s, o);
    REQUIRE(src);
    src->waitReady(20);
    REQUIRE_MESSAGE(src->ready(), src->error());
    return src;
}

}  // namespace

TEST_CASE("video: source specs") {
    auto s = video::parseSourceSpec("file:/tmp/a b.mov");
    REQUIRE(s);
    CHECK(s->kind == video::SourceKind::File);
    CHECK(s->id == "/tmp/a b.mov");
    CHECK(video::specLabel(*s) == "a b");
    s = video::parseSourceSpec("/x/y.mp4");
    REQUIRE(s);
    CHECK(s->kind == video::SourceKind::File);
    s = video::parseSourceSpec("camera");
    REQUIRE(s);
    CHECK(s->kind == video::SourceKind::Camera);
    CHECK(s->id == "default");
    s = video::parseSourceSpec("Camera:FaceTime HD Camera");
    REQUIRE(s);
    CHECK(s->id == "FaceTime HD Camera");
    s = video::parseSourceSpec("syphon:Resolume Arena - Composition");
    REQUIRE(s);
    CHECK(s->kind == video::SourceKind::Syphon);
    CHECK(video::specToString(*s) == "syphon:Resolume Arena - Composition");
    std::string err;
    CHECK_FALSE(video::parseSourceSpec("syphon:", &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(video::parseSourceSpec("  "));
}

TEST_CASE("video: file extensions") {
    CHECK(video::isVideoFile("Space/Clip.mov"));
    CHECK(video::isVideoFile("a.MP4"));
    CHECK(video::isVideoFile("a.m4v"));
    CHECK_FALSE(video::isVideoFile("a.png"));
    CHECK_FALSE(video::isVideoFile("_hidden.mov"));
    CHECK_FALSE(video::isVideoFile("Clip-icon.mov"));
}

TEST_CASE("video: media time (offset, rate, loop)") {
    CHECK(video::mediaTime(1.0, 0, 1, 10, true) == doctest::Approx(1.0));
    CHECK(video::mediaTime(1.0, 2, 2, 10, true) == doctest::Approx(4.0));
    CHECK(video::mediaTime(12.5, 0, 1, 10, true) == doctest::Approx(2.5));
    CHECK(video::mediaTime(12.5, 0, 1, 10, false) == doctest::Approx(10.0));
    CHECK(video::mediaTime(-3, 1, 1, 10, true) == doctest::Approx(1.0));
    CHECK(video::mediaTime(5, 1, 0, 10, true) == doctest::Approx(1.0));  // rate 0 = still
    // A section [start, end): loops back to start, play-once stops at end.
    CHECK(video::mediaTime(1.0, 2, 1, 10, true, 5) == doctest::Approx(3.0));
    CHECK(video::mediaTime(4.0, 2, 1, 10, true, 5) == doctest::Approx(3.0));   // 2 + (4 mod 3)
    CHECK(video::mediaTime(4.0, 2, 1, 10, false, 5) == doctest::Approx(5.0));
    CHECK(video::mediaTime(9.0, 2, 1, 10, true) == doctest::Approx(3.0));      // loops back to the start, not 0
    CHECK(video::mediaTime(1.0, 2, 1, 10, true, 20) == doctest::Approx(3.0));  // end past the movie: ignored
    video::SourceSpec sp;
    sp.offset = 2;
    sp.end = 5;
    sp.rate = 2;
    CHECK(video::playLength(sp, 10) == doctest::Approx(1.5));
    CHECK(video::sectionEnd(sp, 10) == doctest::Approx(5));
    sp.end = 0;
    CHECK(video::playLength(sp, 10) == doctest::Approx(4.0));
    // Beat-synced movies: the section is cut to whole bars (loopUnit), at least one.
    sp.rate = 1;
    sp.loopUnit = 2.5;  // a bar
    CHECK(video::sectionEnd(sp, 10) == doctest::Approx(9.5));  // 2 + 3 bars (8 s fits 3.2)
    CHECK(video::playLength(sp, 10) == doctest::Approx(7.5));
    sp.loopUnit = 9;  // longer than the section: untouched
    CHECK(video::sectionEnd(sp, 10) == doctest::Approx(10));
    sp.offset = 0;
    sp.loopUnit = 18.733 / 9.99;  // a seamless 10-bar loop, a hair short of 10 units: plays whole
    CHECK(video::sectionEnd(sp, 18.733) == doctest::Approx(18.733));
    sp.loopUnit = 18.733 / 10.01;  // a hair over 10 units: exactly 10
    CHECK(video::sectionEnd(sp, 18.733) == doctest::Approx(10 * 18.733 / 10.01));
    sp.loopUnit = 18.733 / 9.9;  // 9.9 units: cut to 9
    CHECK(video::sectionEnd(sp, 18.733) == doctest::Approx(9 * 18.733 / 9.9));
}

TEST_CASE("video: Syphon self-subscription guard") {
    video::SyphonServerInfo own{"uuid-1", "reforce", "reforce"};
    CHECK(video::isSelfSyphonServer(own, "reforce", "reforce", "uuid-1"));
    // Same uuid whatever the names.
    video::SyphonServerInfo renamed{"uuid-1", "x", "y"};
    CHECK(video::isSelfSyphonServer(renamed, "reforce", "reforce", "uuid-1"));
    // Our app + our server name (another instance, or before the uuid is known).
    video::SyphonServerInfo twin{"uuid-2", "REFORCE", "reforce"};
    CHECK(video::isSelfSyphonServer(twin, "reforce", "reforce", "uuid-1"));
    CHECK(video::isSelfSyphonServer(twin, "reforce", "reforce", ""));
    // Other apps' servers, even one called "reforce".
    video::SyphonServerInfo other{"uuid-3", "reforce", "OBS"};
    CHECK_FALSE(video::isSelfSyphonServer(other, "reforce", "reforce", "uuid-1"));
    video::SyphonServerInfo resolume{"uuid-4", "Composition", "Arena"};
    CHECK_FALSE(video::isSelfSyphonServer(resolume, "reforce", "reforce", "uuid-1"));
    CHECK(video::syphonMatches(resolume, "Arena - Composition"));
    CHECK(video::syphonMatches(resolume, "composition"));
    CHECK(video::syphonMatches(resolume, "uuid-4"));
    CHECK_FALSE(video::syphonMatches(resolume, "Arena"));
    CHECK(video::syphonDisplayName(resolume) == "Arena - Composition");
}

TEST_CASE("video composite: fit transforms") {
    using engine::VideoFit;
    // 2:1 video in a 16:9 frame. Cover fills the height: x is cropped.
    auto t = engine::fitTransform(400, 200, 1600, 900, VideoFit::Cover);
    CHECK(t.scale[1] == doctest::Approx(1.0));
    CHECK(t.scale[0] == doctest::Approx((1600.0 / 1800.0)));  // video spans 1800 px of 1600
    CHECK(t.offset[0] + t.scale[0] * 0.5f == doctest::Approx(0.5));
    // Contain: full width, letterboxed in y.
    t = engine::fitTransform(400, 200, 1600, 900, VideoFit::Contain);
    CHECK(t.scale[0] == doctest::Approx(1.0));
    CHECK(t.scale[1] == doctest::Approx(900.0 / 800.0));
    CHECK(t.offset[1] < 0);  // the top rows of the frame map above the video
    t = engine::fitTransform(400, 200, 1600, 900, VideoFit::Stretch);
    CHECK(t.scale[0] == 1.0f);
    CHECK(t.offset[1] == 0.0f);
}

TEST_CASE("video composite: keys") {
    // Luma: dark pixels keyed out.
    CHECK(engine::lumaKeyAlpha({0, 0, 0}, 0.1f, 0.1f) == 0.0f);
    CHECK(engine::lumaKeyAlpha({1, 1, 1}, 0.1f, 0.1f) == 1.0f);
    CHECK(engine::lumaKeyAlpha({0.15f, 0.15f, 0.15f}, 0.1f, 0.1f) == doctest::Approx(0.5f));
    CHECK(engine::lumaKeyAlpha({0.11f, 0.11f, 0.11f}, 0.1f, 0.0f) == 1.0f);  // hard cut
    // Chroma: the key colour is transparent, other hues opaque.
    const engine::Rgb green{0, 1, 0};
    CHECK(engine::chromaKeyAlpha(green, green, 0.15f, 0.1f) == 0.0f);
    CHECK(engine::chromaKeyAlpha({0.1f, 0.9f, 0.1f}, green, 0.15f, 0.1f) == 0.0f);  // near green
    CHECK(engine::chromaKeyAlpha({1, 0, 0}, green, 0.15f, 0.1f) == 1.0f);
    CHECK(engine::chromaKeyAlpha({1, 1, 1}, green, 0.15f, 0.1f) == 1.0f);  // white: no chroma, far from green
    engine::VideoLayerParams p;
    CHECK(engine::keyAlpha({0, 0, 0}, p) == 1.0f);  // no key
}

TEST_CASE("video composite: blend modes, order, opacity, crossfade") {
    engine::VideoLayerParams p;
    const engine::Rgb vis{0.2f, 0.4f, 0.6f};
    const engine::VideoSample vid{{1.0f, 0.5f, 0.0f}, 1.0f};
    auto near = [](const engine::Rgb& a, const engine::Rgb& b) {
        for (int i = 0; i < 3; ++i)
            if (std::fabs(a[std::size_t(i)] - b[std::size_t(i)]) > 1e-5f) return false;
        return true;
    };
    CHECK(near(engine::compositeVideo(vis, vid, p), vid.c));
    p.opacity = 0.5f;
    CHECK(near(engine::compositeVideo(vis, vid, p), {0.6f, 0.45f, 0.3f}));
    p.opacity = 1;
    p.blend = engine::VideoBlend::Add;
    CHECK(near(engine::compositeVideo(vis, vid, p), {1.0f, 0.9f, 0.6f}));
    p.blend = engine::VideoBlend::Screen;
    CHECK(near(engine::compositeVideo(vis, vid, p), {1.0f, 0.7f, 0.6f}));
    p.blend = engine::VideoBlend::Multiply;
    CHECK(near(engine::compositeVideo(vis, vid, p), {0.2f, 0.2f, 0.0f}));
    // Under, normal: the visualizer covers the video where it is bright.
    p.blend = engine::VideoBlend::Normal;
    p.order = engine::VideoOrder::Under;
    CHECK(near(engine::compositeVideo({0, 0, 0}, vid, p), vid.c));  // black visualizer: video shows
    CHECK(near(engine::compositeVideo({1, 1, 1}, vid, p), {1, 1, 1}));
    CHECK(near(engine::compositeVideo(vis, vid, p), {0.2f + 0.4f, 0.4f + 0.2f, 0.6f}));
    // Under, multiply with opacity 0: black (video faded to black, then multiplied).
    p.blend = engine::VideoBlend::Multiply;
    p.opacity = 0;
    CHECK(near(engine::compositeVideo(vis, vid, p), {0, 0, 0}));
    // Transparent video (keyed): visualizer unchanged over.
    p = {};
    CHECK(near(engine::compositeVideo(vis, {{1, 1, 1}, 0.0f}, p), vis));
    // Crossfade: premultiplied mix, alpha mixes.
    const engine::VideoSample a{{1, 0, 0}, 1}, none{{0, 0, 0}, 0};
    auto s = engine::crossfadeSamples(a, none, 0.25f);
    CHECK(s.a == doctest::Approx(0.75f));
    CHECK(s.c[0] == doctest::Approx(1.0f));  // colour kept, fades by alpha
    s = engine::crossfadeSamples(a, {{0, 0, 1}, 1}, 0.5f);
    CHECK(s.a == doctest::Approx(1.0f));
    CHECK(s.c[0] == doctest::Approx(0.5f));
    CHECK(s.c[2] == doctest::Approx(0.5f));
}

TEST_CASE("video composite: settings") {
    engine::VideoLayerParams p;
    CHECK(engine::applyVideoSetting(p, "opacity", "0.25").empty());
    CHECK(p.opacity == doctest::Approx(0.25f));
    CHECK(engine::applyVideoSetting(p, "blend", "screen").empty());
    CHECK(p.blend == engine::VideoBlend::Screen);
    CHECK(engine::applyVideoSetting(p, "fit", "contain").empty());
    CHECK(engine::applyVideoSetting(p, "order", "under").empty());
    CHECK(engine::applyVideoSetting(p, "key", "chroma").empty());
    CHECK(engine::applyVideoSetting(p, "keycolor", "#0000ff").empty());
    CHECK(p.keyColor[2] == 1.0f);
    CHECK(engine::applyVideoSetting(p, "keycolor", "255,128,0").empty());
    CHECK(p.keyColor[1] == doctest::Approx(128.0f / 255.0f));
    CHECK(engine::applyVideoSetting(p, "syphon", "off").empty());
    CHECK_FALSE(p.toSyphon);
    CHECK_FALSE(engine::applyVideoSetting(p, "opacity", "2").empty());
    CHECK_FALSE(engine::applyVideoSetting(p, "blend", "overlay").empty());
    CHECK_FALSE(engine::applyVideoSetting(p, "nope", "1").empty());
    CHECK(engine::colorToHex({0, 1, 0}) == "#00ff00");
}

TEST_CASE("video: deterministic file decoding is frame exact (H.264)") {
    Device dev;
    REQUIRE(dev.d);
    const fs::path path = cardVideo("card_h264.mov", video::TestCodec::H264, 40);
    auto src = openDet(dev.d, path);
    CHECK(src->width() == 320);
    CHECK(src->height() == 180);
    CHECK(src->frameRate() == doctest::Approx(30.0));
    CHECK(src->duration() == doctest::Approx(40.0 / 30.0).epsilon(0.01));
    // Frame k is shown for t in [k/30, (k+1)/30).
    const double ts[] = {0.0, 0.01, 1.0 / 30 - 0.001, 1.0 / 30, 0.5, 0.51, 1.0, 1.3};
    for (double t : ts) {
        const int want = int(std::floor(t * 30 + 1e-6));
        CHECK_MESSAGE(frameIndex(src->frameAt(t)) == want, "t = " << t);
    }
    // Loop: 40 frames = 1.333 s; t = 1.5 -> media .1667 -> frame 5; t = 2.7 -> 1.3667 -> frame 1.
    CHECK(frameIndex(src->frameAt(1.5)) == 5);
    CHECK(frameIndex(src->frameAt(2.7)) == 1);

    // Offset and rate: media = .5 + 2 t.
    auto fast = openDet(dev.d, path, 0.5, 2.0);
    CHECK(frameIndex(fast->frameAt(0)) == 15);
    CHECK(frameIndex(fast->frameAt(0.25)) == 30);
    // No loop: holds the last frame.
    auto once = openDet(dev.d, path, 0, 1, false);
    CHECK(frameIndex(once->frameAt(0.2)) == 6);
    CHECK(frameIndex(once->frameAt(5.0)) == 39);
    const auto st = src->stats();
    CHECK(st.decoded > 0);
    CHECK(st.decodeMsAvg > 0);
}

TEST_CASE("video: decoded colours (Card)") {
    Device dev;
    REQUIRE(dev.d);
    auto src = openDet(dev.d, cardVideo("card_h264.mov", video::TestCodec::H264, 40));
    const Pixels p = readFrame(src->frameAt(0.2));
    REQUIRE(p.w == 320);
    auto check = [&](int x, int y, int r, int g, int b) {
        const std::uint8_t* q = p.at(x, y);
        CHECK_MESSAGE(std::abs(int(q[2]) - r) <= 8, "r at " << x << "," << y << " = " << int(q[2]));
        CHECK_MESSAGE(std::abs(int(q[1]) - g) <= 8, "g at " << x << "," << y << " = " << int(q[1]));
        CHECK_MESSAGE(std::abs(int(q[0]) - b) <= 8, "b at " << x << "," << y << " = " << int(q[0]));
    };
    check(80, 67, 255, 0, 0);     // red
    check(240, 67, 128, 128, 128);
    check(80, 135, 255, 255, 255);
    check(240, 135, 0, 255, 0);   // green
}

TEST_CASE("video: HEVC and ProRes decode frame exact") {
    Device dev;
    REQUIRE(dev.d);
    for (auto [name, codec] : {std::pair{"card_hevc.mov", video::TestCodec::HEVC},
                               std::pair{"card_prores.mov", video::TestCodec::ProRes422}}) {
        auto src = openDet(dev.d, cardVideo(name, codec, 12));
        CHECK_MESSAGE(frameIndex(src->frameAt(0.0)) == 0, name);
        CHECK_MESSAGE(frameIndex(src->frameAt(0.2)) == 6, name);
        CHECK_MESSAGE(frameIndex(src->frameAt(0.39)) == 11, name);
    }
}

TEST_CASE("video: live (non-deterministic) file playback prerolls and never blocks") {
    Device dev;
    REQUIRE(dev.d);
    video::SourceSpec s;
    s.id = cardVideo("card_h264.mov", video::TestCodec::H264, 40).string();
    auto src = video::openSource(dev.d, s, {});
    REQUIRE(src);
    CHECK(src->frameAt(0) == nullptr);  // not decoded yet: nothing, no wait
    src->waitReady(20);
    REQUIRE(src->ready());
    auto f = src->frameAt(0);
    REQUIRE(f);
    CHECK(frameIndex(f) == 0);
    video::SourceSpec bad;
    bad.id = "/nonexistent/clip.mov";
    auto missing = video::openSource(dev.d, bad, {});
    missing->waitReady(5);
    CHECK(missing->failed());
    CHECK(missing->error().find("no such file") != std::string::npos);
}

TEST_CASE("video: settings keys and media folders") {
    engine::PrefsTable t;
    t.set("reforce.Video.Layer.Opacity", "0.4", false);
    t.set("reforce.video.layer.blend", "multiply", true);
    t.set("reforce.Video.Layer.Fit", "sideways", true);  // bad: ignored, reported
    t.set("reforce.ExternalSpritesDir", "/x/sprites", true);
    t.set("reforce.Syphon.AllowFeedback", "1", false);
    engine::Settings st;
    st.apply(t);
    CHECK(st.videoLayer.opacity == doctest::Approx(0.4f));
    CHECK(st.videoLayer.blend == engine::VideoBlend::Multiply);
    CHECK(st.videoLayer.fit == engine::VideoFit::Cover);
    CHECK(st.settingErrors.size() == 1);
    CHECK(st.externalSpritesDir == "/x/sprites");
    CHECK(st.syphonAllowFeedback);

    const fs::path dir = tmpDir() / "media";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    for (const char* f : {"a.mov", "sub/b.mp4", "c.png", "_d.mov", "e-icon.mov", "f.txt"}) std::ofstream(dir / f) << "x";
    auto all = engine::listMediaFolder(dir, "External/", false);
    REQUIRE(all.size() == 3);
    CHECK(all[0].id == "External/a");
    CHECK(all[1].id == "External/c");
    CHECK(all[2].id == "External/sub/b");
    auto vids = engine::listMediaFolder(dir, "Videos/", true);
    CHECK(vids.size() == 2);
    CHECK(engine::isSpriteFile("x.jpg"));
    CHECK(engine::isSpriteFile("x.mov"));
}

// ---------------------------------------------------------------- camera permission gate (mocked TCC)

namespace {
struct MockTcc {
    video::CameraAccess status = video::CameraAccess::NotDetermined;
    std::vector<std::function<void(bool)>> pending;  // prompts shown, not answered yet
    video::CameraGate gate{[this] { return status; },
                           [this](std::function<void(bool)> done) { pending.push_back(std::move(done)); }};
    void answer(bool granted) {
        status = granted ? video::CameraAccess::Granted : video::CameraAccess::Denied;
        auto p = std::move(pending);
        pending.clear();
        for (auto& f : p) f(granted);
    }
};
}  // namespace

TEST_CASE("camera gate: not asked -> one shared prompt, callers never block") {
    MockTcc tcc;
    CHECK(tcc.gate.access() == video::CameraAccess::NotDetermined);
    int granted = 0, denied = 0;
    auto cb = [&](bool g) { (g ? granted : denied)++; };
    tcc.gate.whenDecided(cb);  // returns at once: the prompt is up, nobody answered
    tcc.gate.whenDecided(cb);  // a second camera source (layer + sprite): same prompt
    CHECK(tcc.pending.size() == 1);
    CHECK(tcc.gate.requests() == 1);
    CHECK(tcc.gate.access() == video::CameraAccess::Requesting);
    CHECK(std::string(video::cameraAccessName(tcc.gate.access())) == "waiting");
    CHECK(granted + denied == 0);
    tcc.answer(true);
    CHECK(granted == 2);
    CHECK(denied == 0);
    CHECK(tcc.gate.access() == video::CameraAccess::Granted);
    tcc.gate.whenDecided(cb);  // decided: immediate, no new prompt
    CHECK(granted == 3);
    CHECK(tcc.gate.requests() == 1);
}

TEST_CASE("camera gate: denied, restricted and missing usage key never prompt") {
    MockTcc tcc;
    int denied = 0;
    tcc.gate.whenDecided([&](bool g) { denied += !g; });
    tcc.answer(false);
    CHECK(denied == 1);
    CHECK(tcc.gate.access() == video::CameraAccess::Denied);
    tcc.gate.whenDecided([&](bool g) { denied += !g; });
    CHECK(denied == 2);
    CHECK(tcc.gate.requests() == 1);
    CHECK(video::cameraAccessMessage(video::CameraAccess::Denied).find("System Settings") != std::string::npos);

    MockTcc noKey;
    noKey.status = video::CameraAccess::NoUsageKey;
    bool got = true;
    noKey.gate.whenDecided([&](bool g) { got = g; });
    CHECK_FALSE(got);
    CHECK(noKey.gate.requests() == 0);  // asking would get the app killed by TCC
    CHECK(std::string(video::cameraAccessName(video::CameraAccess::NoUsageKey)) == "unavailable");
}

TEST_CASE("camera gate: a request that answers synchronously, and a new prompt after an answer") {
    video::CameraAccess status = video::CameraAccess::NotDetermined;
    int prompts = 0;
    video::CameraGate gate([&] { return status; },
                           [&](std::function<void(bool)> done) {
                               ++prompts;
                               done(false);  // answered inside the call (no lock may be held)
                           });
    int answers = 0;
    gate.whenDecided([&](bool) { ++answers; });
    CHECK(answers == 1);
    CHECK(gate.access() == video::CameraAccess::NotDetermined);  // not stuck in Requesting
    gate.whenDecided([&](bool) { ++answers; });                  // still undetermined: asks again
    CHECK(prompts == 2);
    CHECK(answers == 2);
}

TEST_CASE("camera: the system gate reports without prompting") {
    // Only reads the status (no prompt): any value is fine, it must not block.
    const int p = video::cameraPermission();
    CHECK(p >= 0);
    CHECK(p <= 4);
}

// ---------------------------------------------------------------- video mips (minified frames)

namespace {
MTL::Texture* makeBgra(MTL::Device* dev, int w, int h, MTL::StorageMode mode, bool fillChecker) {
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(w), NS::UInteger(h), false);
    td->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    td->setStorageMode(mode);
    MTL::Texture* t = dev->newTexture(td);
    if (t && fillChecker) {
        // 1-pixel checkerboard black/white: the worst case for aliasing; any
        // correct prefilter gives flat mid-gray.
        std::vector<std::uint8_t> px(std::size_t(w) * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::uint8_t v = ((x + y) & 1) ? 255 : 0;
                px[std::size_t(x) * 4 + 0] = px[std::size_t(x) * 4 + 1] = px[std::size_t(x) * 4 + 2] = v;
                px[std::size_t(x) * 4 + 3] = 255;
            }
            t->replaceRegion(MTL::Region(0, NS::UInteger(y), NS::UInteger(w), 1), 0, px.data(), NS::UInteger(w) * 4);
        }
    }
    return t;
}

// Reads mip `level` of a private texture through a blit.
std::vector<std::uint8_t> readLevel(MTL::Device* dev, MTL::CommandQueue* q, MTL::Texture* t, int level, int* w, int* h) {
    *w = std::max(1, int(t->width()) >> level);
    *h = std::max(1, int(t->height()) >> level);
    MTL::Texture* dst = makeBgra(dev, *w, *h, MTL::StorageModeShared, false);
    MTL::CommandBuffer* cmd = q->commandBuffer();
    MTL::BlitCommandEncoder* b = cmd->blitCommandEncoder();
    b->copyFromTexture(t, 0, NS::UInteger(level), MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(*w), NS::UInteger(*h), 1),
                       dst, 0, 0, MTL::Origin(0, 0, 0));
    b->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    std::vector<std::uint8_t> px(std::size_t(*w) * *h * 4);
    dst->getBytes(px.data(), NS::UInteger(*w) * 4, MTL::Region(0, 0, NS::UInteger(*w), NS::UInteger(*h)), 0);
    dst->release();
    return px;
}

double gpuMs(MTL::CommandBuffer* cmd) { return (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0; }
}  // namespace

TEST_CASE("video mips: minified frames get a box-filtered mip chain, others pass through") {
    Device dev;
    REQUIRE(dev.d);
    MTL::CommandQueue* q = dev.d->newCommandQueue();
    render::VideoMips mips(dev.d);
    MTL::Texture* frame = makeBgra(dev.d, 640, 360, MTL::StorageModeShared, true);
    REQUIRE(frame);
    auto id1 = std::make_shared<int>(1);
    int slot = 0;

    MTL::CommandBuffer* cmd = q->commandBuffer();
    CHECK(mips.prepare(cmd, &slot, frame, id1, 1.0) == frame);   // 1:1: untouched
    CHECK(mips.prepare(cmd, &slot, frame, id1, 0.75) == frame);  // mild: bilinear is fine
    CHECK(mips.builds() == 0);
    MTL::Texture* m = mips.prepare(cmd, &slot, frame, id1, 0.25);
    REQUIRE(m != frame);
    CHECK(m->width() == 320);
    CHECK(m->height() == 180);
    CHECK(int(m->mipmapLevelCount()) == render::VideoMips::levelsFor(320, 180));
    CHECK(mips.builds() == 1);
    CHECK(mips.prepare(cmd, &slot, frame, id1, 0.3) == m);  // same frame: no second pass
    CHECK(mips.builds() == 1);
    cmd->commit();
    cmd->waitUntilCompleted();

    for (int level : {0, 2}) {
        int w = 0, h = 0;
        const auto px = readLevel(dev.d, q, m, level, &w, &h);
        int lo = 255, hi = 0;
        for (std::size_t i = 0; i < px.size(); i += 4) {
            lo = std::min<int>(lo, px[i + 1]);
            hi = std::max<int>(hi, px[i + 1]);
        }
        INFO("level " << level << " " << w << "x" << h);
        CHECK(lo >= 125);  // flat gray: the checkerboard is averaged, not aliased
        CHECK(hi <= 130);
    }

    // A new frame (new id) refilters; a different slot gets its own texture.
    auto id2 = std::make_shared<int>(2);
    cmd = q->commandBuffer();
    CHECK(mips.prepare(cmd, &slot, frame, id2, 0.25) == m);
    CHECK(mips.builds() == 2);
    int slot2 = 0;
    CHECK(mips.prepare(cmd, &slot2, frame, id2, 0.25) != m);
    CHECK(mips.builds() == 3);
    cmd->commit();
    cmd->waitUntilCompleted();
    // Unused slots are recycled after a while.
    for (int i = 0; i < 200; ++i) mips.collect();
    frame->release();
    q->release();
}

TEST_CASE("video mips: GPU cost with a 4K source at 1080p and 4K output") {
    Device dev;
    REQUIRE(dev.d);
    MTL::CommandQueue* q = dev.d->newCommandQueue();
    render::VideoMips mips(dev.d);
    MTL::Texture* frame = makeBgra(dev.d, 3840, 2160, MTL::StorageModePrivate, false);
    REQUIRE(frame);
    int slot = 0;
    const int n = 60;
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    // The filter pass depends on the source only (half size + mips); measured
    // per new frame, as for a 4K movie sprite or a 4K colour layer at 1080p.
    std::vector<double> prep;
    for (int i = 0; i < n + 5; ++i) {
        MTL::CommandBuffer* cmd = q->commandBuffer();
        mips.prepare(cmd, &slot, frame, std::make_shared<int>(i), 0.5);
        cmd->commit();
        cmd->waitUntilCompleted();
        if (i >= 5) prep.push_back(gpuMs(cmd));  // after warm-up (allocation, pipeline)
    }
    MESSAGE("4K source -> half-size + mips: median " << median(prep) << " ms, max "
                                                     << *std::max_element(prep.begin(), prep.end()) << " ms per new frame");
    PERF_BUDGET(median(prep) < 2.0);  // generous: ~0.3 ms on Apple silicon

    // The colour-layer pass at 1080p / 4K output: raw 4K frame vs the
    // prefiltered copy (at 4K output the scale is 1: prepare returns the raw
    // frame and costs nothing). Interleaved so GPU clock changes hit both.
    for (auto [ow, oh] : {std::pair{1920, 1080}, std::pair{3840, 2160}}) {
        render::ScenePipelineOptions po;
        render::ScenePipeline pipe(dev.d, uint32_t(ow), uint32_t(oh), po);
        MTL::Texture* out = makeBgra(dev.d, ow, oh, MTL::StorageModePrivate, false);
        engine::Palette pal{};
        engine::MapLut lut{};
        for (int k = 0; k < engine::kPaletteSize; ++k) lut[std::size_t(k)] = float(k) / 255.f;
        std::vector<double> raw, withMips;
        for (int i = 0; i < 2 * (n + 5); ++i) {
            const bool prepOn = i & 1;
            MTL::CommandBuffer* cmd = q->commandBuffer();
            render::VideoLayerDraw d;
            d.b = prepOn ? mips.prepare(cmd, &slot, frame, std::make_shared<int>(1000 + i), double(ow) / 3840.0) : frame;
            pipe.encodeColorize(cmd, pal, lut, out, &d);
            cmd->commit();
            cmd->waitUntilCompleted();
            if (i >= 10) (prepOn ? withMips : raw).push_back(gpuMs(cmd));
        }
        MESSAGE("colour layer, 4K source at " << ow << "x" << oh << ": colorize+video median " << median(raw)
                                              << " ms raw, " << median(withMips)
                                              << " ms incl. the filter pass (new frame every time)");
        out->release();
    }
    frame->release();
    q->release();
}

// ---------------------------------------------------------------- Syphon output alpha (docs/syphon-alpha.md)

TEST_CASE("video: Syphon alpha: colorize tracks it, ColorKey = 0 movies raise it, the key matches OBS's Color Key; the camera backdrop mixes by it") {
    Device dev;
    REQUIRE(dev.d);
    MTL::CommandQueue* q = dev.d->newCommandQueue();
    const int W = 64, H = 36;
    render::ScenePipelineOptions po;
    render::ScenePipeline pipe(dev.d, uint32_t(W), uint32_t(H), po);
    MTL::Texture* out = makeBgra(dev.d, W, H, MTL::StorageModeShared, false);
    MTL::Texture* white = makeBgra(dev.d, 16, 16, MTL::StorageModeShared, false);
    {
        std::vector<std::uint8_t> px(16 * 16 * 4, 255);
        white->replaceRegion(MTL::Region(0, 0, 16, 16), 0, px.data(), 16 * 4);
    }
    const int tex = pipe.addVideoSpriteTexture(false);
    pipe.setSpriteFrame(tex, white);
    engine::Palette pal{};  // black visuals
    engine::MapLut lut{};
    for (int k = 0; k < engine::kPaletteSize; ++k) lut[std::size_t(k)] = float(k) / 255.f;
    render::SpriteQuad quads[2];
    quads[0].texture = tex;  // ColorKey = 0 at half opacity
    quads[0].cx = 16;
    quads[0].cy = 18;
    quads[0].w = quads[0].h = 16;
    quads[0].opacity = 0.5f;
    quads[0].opaque = true;
    quads[1] = quads[0];  // keyed
    quads[1].cx = 48;
    quads[1].opacity = 1;
    quads[1].opaque = false;
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    auto run = [&](auto encode) {
        MTL::CommandBuffer* cmd = q->commandBuffer();
        encode(cmd);
        cmd->commit();
        cmd->waitUntilCompleted();
        REQUIRE(cmd->status() == MTL::CommandBufferStatusCompleted);
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    };
    auto at = [&](int x, int y, int c) { return int(px[std::size_t((y * W + x) * 4 + c)]); };

    // Off: alpha 1 everywhere, as before.
    run([&](MTL::CommandBuffer* cmd) { pipe.encodeColorize(cmd, pal, lut, out, nullptr, quads, 2, false); });
    CHECK(at(2, 2, 3) == 255);
    CHECK(at(16, 18, 3) == 255);
    CHECK(at(48, 18, 3) == 255);
    const int rgbOpaque = at(16, 18, 2), rgbKeyed = at(48, 18, 2);
    // Tracked: 0 for the visuals and the keyed movie, the opacity under the ColorKey = 0 movie; colour unchanged.
    run([&](MTL::CommandBuffer* cmd) { pipe.encodeColorize(cmd, pal, lut, out, nullptr, quads, 2, true); });
    CHECK(at(2, 2, 3) == 0);
    CHECK(at(48, 18, 3) == 0);
    CHECK(std::abs(at(16, 18, 3) - 128) <= 1);
    CHECK(at(16, 18, 2) == rgbOpaque);
    CHECK(at(48, 18, 2) == rgbKeyed);

    // The key: alpha = opaque + (1 - opaque) * clamp((|rgb| - similarity) / smoothness), colour unchanged.
    struct P { std::uint8_t r, g, b, a; };
    const P cases[] = {{0, 0, 0, 0}, {26, 0, 0, 0}, {51, 0, 0, 0}, {10, 10, 10, 0}, {0, 0, 0, 128}, {26, 0, 0, 128},
                       {200, 30, 90, 0}, {0, 0, 0, 255}};
    MTL::Texture* frame = makeBgra(dev.d, W, H, MTL::StorageModeShared, false);
    {
        std::vector<std::uint8_t> in(std::size_t(W * H * 4), 0);
        for (std::size_t i = 0; i < std::size(cases); ++i) {
            in[i * 4 + 0] = cases[i].b;
            in[i * 4 + 1] = cases[i].g;
            in[i * 4 + 2] = cases[i].r;
            in[i * 4 + 3] = cases[i].a;
        }
        frame->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, in.data(), NS::UInteger(W * 4));
    }
    run([&](MTL::CommandBuffer* cmd) { pipe.encodeSyphonAlpha(cmd, frame, out, 0.08f, 0.05f, false); });
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        const P& c = cases[i];
        const double len = std::sqrt(double(c.r * c.r + c.g * c.g + c.b * c.b)) / 255.0;
        const double k = std::clamp((len - 0.08) / 0.05, 0.0, 1.0);
        const double f = c.a / 255.0;
        const int want = int(std::lround((f + (1 - f) * k) * 255.0));
        INFO("case " << i);
        CHECK(std::abs(at(int(i), 0, 3) - want) <= 1);
        CHECK(at(int(i), 0, 2) == c.r);
        CHECK(at(int(i), 0, 0) == c.b);
    }
    // Premultiplied (reforce.Syphon.AlphaPremultiplied): colour times the alpha.
    run([&](MTL::CommandBuffer* cmd) { pipe.encodeSyphonAlpha(cmd, frame, out, 0.08f, 0.05f, true); });
    CHECK(at(0, 0, 2) == 0);
    CHECK(std::abs(at(1, 0, 2) - int(std::lround(26 * at(1, 0, 3) / 255.0))) <= 1);
    CHECK(at(2, 0, 2) == 51);

    // Camera backdrop (docs/backdrop.md), in place: out = frame * a + video * opacity * (1 - a) with the same a,
    // alpha 1. The white video at opacity 0.5 shows as 128 where the key is fully transparent.
    auto loadCases = [&]() {
        std::vector<std::uint8_t> in(std::size_t(W * H * 4), 0);
        for (std::size_t i = 0; i < std::size(cases); ++i) {
            in[i * 4 + 0] = cases[i].b;
            in[i * 4 + 1] = cases[i].g;
            in[i * 4 + 2] = cases[i].r;
            in[i * 4 + 3] = cases[i].a;
        }
        out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, in.data(), NS::UInteger(W * 4));
    };
    loadCases();
    const engine::UvTransform identity;
    run([&](MTL::CommandBuffer* cmd) { pipe.encodeBackdrop(cmd, out, white, identity, false, 0.5f, 0.08f, 0.05f); });
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        const P& c = cases[i];
        const double len = std::sqrt(double(c.r * c.r + c.g * c.g + c.b * c.b)) / 255.0;
        const double k = std::clamp((len - 0.08) / 0.05, 0.0, 1.0);
        const double f = c.a / 255.0;
        const double a = f + (1 - f) * k;
        INFO("backdrop case " << i);
        CHECK(at(int(i), 0, 3) == 255);
        CHECK(std::abs(at(int(i), 0, 2) - int(std::lround(c.r * a + 255 * 0.5 * (1 - a)))) <= 1);
        CHECK(std::abs(at(int(i), 0, 0) - int(std::lround(c.b * a + 255 * 0.5 * (1 - a)))) <= 1);
    }
    // No video (still opening): the see-through parts go black, alpha 1.
    loadCases();
    run([&](MTL::CommandBuffer* cmd) { pipe.encodeBackdrop(cmd, out, nullptr, identity, false, 1.0f, 0.08f, 0.05f); });
    CHECK(at(0, 0, 2) == 0);
    CHECK(at(0, 0, 3) == 255);
    CHECK(at(6, 0, 2) == 200);  // opaque visuals unchanged
    // Mirror: a video red on the left half, blue on the right; the frame's left edge (black, see-through).
    {
        MTL::Texture* lr = makeBgra(dev.d, 2, 1, MTL::StorageModeShared, false);
        const std::uint8_t px2[8] = {0, 0, 255, 255, 255, 0, 0, 255};  // BGRA: red, blue
        lr->replaceRegion(MTL::Region(0, 0, 2, 1), 0, px2, 8);
        for (bool mirror : {false, true}) {
            loadCases();
            run([&](MTL::CommandBuffer* cmd) { pipe.encodeBackdrop(cmd, out, lr, identity, mirror, 1.0f, 0.08f, 0.05f); });
            INFO("mirror " << mirror);
            CHECK(at(0, H - 1, 2) == (mirror ? 0 : 255));
            CHECK(at(0, H - 1, 0) == (mirror ? 255 : 0));
        }
        lr->release();
    }

    frame->release();
    white->release();
    out->release();
    q->release();
}

// ---------------------------------------------------------------- body tracking passes (docs/body.md)

TEST_CASE("video: body tracking: the person mask cuts the visuals on the backdrop, and burns into the trails") {
    Device dev;
    REQUIRE(dev.d);
    MTL::CommandQueue* q = dev.d->newCommandQueue();
    const int W = 64, H = 36;
    render::ScenePipelineOptions po;
    render::ScenePipeline pipe(dev.d, uint32_t(W), uint32_t(H), po);
    auto run = [&](auto encode) {
        MTL::CommandBuffer* cmd = q->commandBuffer();
        encode(cmd);
        cmd->commit();
        cmd->waitUntilCompleted();
        REQUIRE(cmd->status() == MTL::CommandBufferStatusCompleted);
    };
    // The mask: the left half is the person.
    auto r8 = [&](int w, int h, int fill, bool leftHalf) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatR8Unorm,
                                                                                 NS::UInteger(w), NS::UInteger(h), false);
        td->setUsage(MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModeShared);
        MTL::Texture* t = dev.d->newTexture(td);
        std::vector<std::uint8_t> px(std::size_t(w * h));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) px[std::size_t(y * w + x)] = std::uint8_t(!leftHalf || x < w / 2 ? fill : 0);
        t->replaceRegion(MTL::Region(0, 0, NS::UInteger(w), NS::UInteger(h)), 0, px.data(), NS::UInteger(w));
        return t;
    };
    MTL::Texture* mask = r8(16, 12, 255, true);
    MTL::Texture* empty = r8(16, 12, 0, false);
    const engine::UvTransform identity;

    // Backdrop: opaque visuals (alpha 1, bright) everywhere; with front = 1 the white camera shows on the person.
    MTL::Texture* out = makeBgra(dev.d, W, H, MTL::StorageModeShared, false);
    MTL::Texture* white = makeBgra(dev.d, 4, 4, MTL::StorageModeShared, false);
    {
        std::vector<std::uint8_t> px(16 * 4, 255);
        white->replaceRegion(MTL::Region(0, 0, 4, 4), 0, px.data(), 16);
    }
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    auto fillFrame = [&]() {
        std::vector<std::uint8_t> in(std::size_t(W * H * 4));
        for (std::size_t i = 0; i < std::size_t(W * H); ++i) {
            in[i * 4 + 0] = 200;  // B
            in[i * 4 + 1] = 0;
            in[i * 4 + 2] = 0;
            in[i * 4 + 3] = 255;  // opaque (dancer-like)
        }
        out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, in.data(), NS::UInteger(W * 4));
    };
    auto read = [&]() {
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    };
    auto at = [&](int x, int y, int c) { return int(px[std::size_t((y * W + x) * 4 + c)]); };
    render::BackdropBody body;
    body.mask = mask;
    body.front = 1.0f;
    fillFrame();
    run([&](MTL::CommandBuffer* cmd) {
        pipe.encodeBackdrop(cmd, out, white, identity, false, 1.0f, 0.08f, 0.05f, &body);
    });
    read();
    CHECK(at(4, 18, 2) == 255);  // person: the camera (white) through the visuals
    CHECK(at(4, 18, 0) == 255);
    CHECK(at(60, 18, 2) == 0);   // elsewhere the opaque visuals stay
    CHECK(at(60, 18, 0) == 200);
    // Mirrored backdrop: the mask mirrors with it.
    fillFrame();
    run([&](MTL::CommandBuffer* cmd) {
        pipe.encodeBackdrop(cmd, out, white, identity, true, 1.0f, 0.08f, 0.05f, &body);
    });
    read();
    CHECK(at(4, 18, 2) == 0);
    CHECK(at(60, 18, 2) == 255);
    // Half: halfway.
    body.front = 0.5f;
    fillFrame();
    run([&](MTL::CommandBuffer* cmd) {
        pipe.encodeBackdrop(cmd, out, white, identity, false, 1.0f, 0.08f, 0.05f, &body);
    });
    read();
    CHECK(std::abs(at(4, 18, 2) - 128) <= 2);
    // Show: joint dots (green) at their screen uv with their weight.
    body.front = 0;
    body.mask = nullptr;
    body.show = true;
    body.joints[0] = 0.5f;  // nose at the centre
    body.joints[1] = 0.5f;
    body.joints[2] = 1.0f;
    fillFrame();
    run([&](MTL::CommandBuffer* cmd) {
        pipe.encodeBackdrop(cmd, out, white, identity, false, 1.0f, 0.08f, 0.05f, &body);
    });
    read();
    CHECK(at(32, 18, 1) > 200);  // green
    CHECK(at(4, 4, 1) == 0);

    // Silhouette into the trail buffer (newest intensity frame, cleared): fill / mirror / motion / outline.
    MTL::Texture* inten = pipe.currentIntensity();
    MTL::TextureDescriptor* rd = MTL::TextureDescriptor::texture2DDescriptor(inten->pixelFormat(), NS::UInteger(W),
                                                                             NS::UInteger(H), false);
    rd->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* back = dev.d->newTexture(rd);
    std::vector<float> I(std::size_t(W * H));
    auto burn = [&](MTL::Texture* m, MTL::Texture* prev, bool mirror, int mode, float amount) {
        run([&](MTL::CommandBuffer* cmd) {
            pipe.encodeClear(cmd);
            pipe.encodeBodyBurn(cmd, m, prev, identity, mirror, mode, amount);
            MTL::BlitCommandEncoder* bl = cmd->blitCommandEncoder();
            bl->copyFromTexture(pipe.currentIntensity(), back);
            bl->endEncoding();
        });
        if (back->pixelFormat() == MTL::PixelFormatR16Float) {
            std::vector<__fp16> h(std::size_t(W * H));
            back->getBytes(h.data(), NS::UInteger(W * 2), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
            for (std::size_t i = 0; i < h.size(); ++i) I[i] = float(h[i]);
        } else {
            std::vector<std::uint8_t> b(std::size_t(W * H));
            back->getBytes(b.data(), NS::UInteger(W), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
            for (std::size_t i = 0; i < b.size(); ++i) I[i] = b[i] / 255.0f;
        }
    };
    auto iat = [&](int x, int y) { return I[std::size_t(y * W + x)]; };
    burn(mask, nullptr, false, 1, 0.5f);
    CHECK(iat(4, 18) == doctest::Approx(0.5).epsilon(0.02));
    CHECK(iat(60, 18) == 0);
    burn(mask, nullptr, true, 1, 0.5f);
    CHECK(iat(4, 18) == 0);
    CHECK(iat(60, 18) == doctest::Approx(0.5).epsilon(0.02));
    burn(mask, empty, false, 3, 1.0f);  // motion: where it changed (all of the person, against an empty mask)
    CHECK(iat(4, 18) == doctest::Approx(1.0).epsilon(0.02));
    burn(mask, mask, false, 3, 1.0f);   // no change: nothing
    CHECK(iat(4, 18) == 0);
    burn(mask, nullptr, false, 2, 1.0f);  // outline: only near the edge (the middle column)
    CHECK(iat(4, 18) == 0);
    CHECK(iat(60, 18) == 0);
    CHECK(std::max(iat(31, 18), iat(32, 18)) > 0.5f);
    burn(mask, nullptr, false, 0, 1.0f);  // off
    CHECK(iat(4, 18) == 0);

    back->release();
    mask->release();
    empty->release();
    white->release();
    out->release();
    q->release();
}

// ---------------------------------------------------------------- Syphon input: server quits and comes back

TEST_CASE("video: a Syphon input shows nothing while its server is gone and reconnects") {
    Device dev;
    REQUIRE(dev.d);
    MTL::CommandQueue* q = dev.d->newCommandQueue();
    video::initSyphonDirectory();
    const std::string name = "reforce-test-" + std::to_string(::getpid());
    MTL::Texture* tex = makeBgra(dev.d, 64, 36, MTL::StorageModeShared, true);
    auto publishFor = [&](platform::SyphonPublisher& pub, double seconds, auto until) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end && !until()) {
            MTL::CommandBuffer* cmd = q->commandBuffer();
            pub.publish(tex, cmd);
            cmd->commit();
            cmd->waitUntilCompleted();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);  // directory announcements
        }
    };
    video::SourceSpec spec;
    spec.kind = video::SourceKind::Syphon;
    spec.id = name;
    auto src = video::openSource(dev.d, spec, {});
    REQUIRE(src);
    {
        auto pub = std::make_unique<platform::SyphonPublisher>(dev.d, name);
        REQUIRE(pub->valid());
        publishFor(*pub, 10, [&] { return src->frameAt(0) != nullptr; });
        REQUIRE_MESSAGE(src->frameAt(0) != nullptr, "no frame from the test Syphon server");
        CHECK(src->waiting().empty());
        pub.reset();  // the server quits
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end && src->frameAt(0) != nullptr)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        CHECK(src->frameAt(0) == nullptr);  // nothing rather than a frozen frame
        CHECK(src->waiting().find("quit") != std::string::npos);
        CHECK_FALSE(src->failed());
    }
    {
        platform::SyphonPublisher pub(dev.d, name);  // back again
        publishFor(pub, 10, [&] { return src->frameAt(0) != nullptr; });
        CHECK(src->frameAt(0) != nullptr);
        CHECK(src->waiting().empty());
    }
    src.reset();
    tex->release();
    q->release();
}

TEST_CASE("video: truncated and garbage files fail cleanly (no hang, no crash)") {
    Device dev;
    REQUIRE(dev.d);
    const fs::path good = cardVideo("card_h264.mov", video::TestCodec::H264, 40);
    const fs::path cut = tmpDir() / "truncated.mov";
    const fs::path junk = tmpDir() / "junk.mov";
    {
        std::ifstream in(good, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ofstream(cut, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size() / 2));
        std::string noise(64 * 1024, '\0');
        for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = char((i * 2654435761u) >> 13);
        std::ofstream(junk, std::ios::binary).write(noise.data(), std::streamsize(noise.size()));
    }
    for (const fs::path& p : {cut, junk}) {
        INFO(p.filename().string());
        video::SourceSpec s;
        s.id = p.string();
        auto src = video::openSource(dev.d, s, {});
        REQUIRE(src);
        // Live playback: frameAt never blocks; the source either plays what it
        // can or reports a failure, within a few seconds.
        const auto t0 = std::chrono::steady_clock::now();
        double t = 0;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3) && !src->failed()) {
            const auto c0 = std::chrono::steady_clock::now();
            (void)src->frameAt(t);
            CHECK(std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(50));
            t += 1.0 / 60.0;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (src->failed()) CHECK_FALSE(src->error().empty());
        MESSAGE(p.filename().string() << ": " << (src->failed() ? "failed: " + src->error() : "plays (partial)"));
    }
}
