// Dancers (PLAN 5P, docs/dancers-design.md): render/DancerPipeline on the GPU, offscreen.
//   - nothing is drawn without dancers and floor; a dancer lands where the stage camera puts it
//   - the visuals light the dancers (a red frame tints them red)
//   - cost with five dancers and the floor at 1920x1080, and no render-thread allocations
#include <doctest/doctest.h>

#include "PerfBudget.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/Dancers.hpp"
#include "engine/Perf.hpp"
#include "render/DancerPipeline.hpp"
#include "render/ImageFiles.hpp"

using namespace reforce;
using namespace reforce::engine::dance;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

MTL::Texture* makeOutput(int W, int H) {
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    return device()->newTexture(td);
}

void fill(MTL::Texture* t, std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a = 255) {
    const int W = int(t->width()), H = int(t->height());
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    for (int i = 0; i < W * H; ++i) {
        px[std::size_t(i * 4)] = b;
        px[std::size_t(i * 4 + 1)] = g;
        px[std::size_t(i * 4 + 2)] = r;
        px[std::size_t(i * 4 + 3)] = a;
    }
    t->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, px.data(), NS::UInteger(W * 4));
}

std::vector<std::uint8_t> read(MTL::Texture* t) {
    const int W = int(t->width()), H = int(t->height());
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    t->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    return px;
}

render::DancerStage stageWith(int n, int W, int H, float floor) {
    render::DancerStage st;
    st.cam = engine::stageCamera(double(W) / double(H), 0.42);
    const auto slots = engine::stageSlots(n, st.cam);
    st.bodies.resize(std::size_t(n));
    for (int k = 0; k < n; ++k) {
        const engine::DancerSpec spec = engine::generateDancer(engine::dance::Style(k % kStyleCount), std::uint32_t(k));
        const Skeleton sk = makeSkeleton(spec.look);
        Frame place;
        place.o = {slots[std::size_t(k)].first, 0, slots[std::size_t(k)].second};
        buildBody(spec.look, sk, restPose(sk), place, 1.0f, st.bodies[std::size_t(k)]);
    }
    st.bodyCount = n;
    st.floor = floor;
    st.shadows = n > 0 ? 1.0f : 0.0f;
    return st;
}

double encodeOnce(render::DancerPipeline& p, MTL::CommandQueue* q, MTL::Texture* out, const render::DancerStage& st,
                  bool* drew = nullptr, bool trackAlpha = false) {
    MTL::CommandBuffer* cmd = q->commandBuffer();
    const bool d = p.encode(cmd, out, st, trackAlpha);
    if (drew) *drew = d;
    cmd->commit();
    cmd->waitUntilCompleted();
    REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
    return (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
}

}  // namespace

TEST_CASE("dancers: nothing without dancers or floor; a dancer lands where the camera puts it; the visuals light it") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::DancerPipeline p(device(), W, H, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::Texture* out = makeOutput(W, H);

    fill(out, 10, 20, 30);
    bool drew = true;
    encodeOnce(p, q, out, stageWith(0, W, H, 0), &drew);
    CHECK_FALSE(drew);
    auto px = read(out);
    CHECK(px[0] == 10);
    CHECK(px[std::size_t((H / 2 * W + W / 2) * 4)] == 10);

    // One dancer: its chest is drawn (opaque, not the background), the sky above the heads is untouched.
    const render::DancerStage one = stageWith(1, W, H, 0);
    const V3 chest = engine::projectStage(one.cam, {0, 0.6f, 0});
    fill(out, 10, 20, 30);
    encodeOnce(p, q, out, one, &drew);
    CHECK(drew);
    px = read(out);
    auto at = [&](float u, float v) {
        const int x = std::clamp(int(u * W), 0, W - 1), y = std::clamp(int(v * H), 0, H - 1);
        return std::array<int, 3>{px[std::size_t((y * W + x) * 4 + 2)], px[std::size_t((y * W + x) * 4 + 1)],
                                  px[std::size_t((y * W + x) * 4)]};
    };
    const auto c = at(chest.x, chest.y);
    CHECK((c[0] != 30 || c[1] != 20 || c[2] != 10));
    const auto sky = at(0.5f, 0.1f);
    CHECK(sky[0] == 30);
    CHECK(sky[2] == 10);

    // Lit by the visuals: on a red frame the dancer comes out redder than on a blue one (Lighting 1).
    render::DancerStage lit = one;
    lit.lighting = 1;
    fill(out, 0, 0, 230);  // red (BGRA)
    encodeOnce(p, q, out, lit);
    px = read(out);
    const auto onRed = at(chest.x, chest.y);
    fill(out, 230, 0, 0);  // blue
    encodeOnce(p, q, out, lit);
    px = read(out);
    const auto onBlue = at(chest.x, chest.y);
    CHECK(onRed[0] - onRed[2] > onBlue[0] - onBlue[2] + 20);

    out->release();
    q->release();
}

TEST_CASE("dancers: reflections in the floor: under the dancer only, by the Reflection strength") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::DancerPipeline p(device(), W, H, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::Texture* out = makeOutput(W, H);
    {
        render::DancerStage st = stageWith(1, W, H, 1.0f);
        st.shadows = 0;  // the blob shadow would darken the same spot
        auto render = [&](float strength) {
            st.reflection = strength;
            fill(out, 40, 40, 40);
            encodeOnce(p, q, out, st);
            return read(out);
        };
        const auto off = render(0), on = render(1);
        auto lum = [&](const std::vector<std::uint8_t>& px, V3 w) {
            const V3 s = engine::projectStage(st.cam, w);
            const int x = std::clamp(int(s.x * W), 0, W - 1), y = std::clamp(int(s.y * H), 0, H - 1);
            const std::size_t i = std::size_t((y * W + x) * 4);
            return int(px[i]) + int(px[i + 1]) + int(px[i + 2]);
        };
        // Just under the feet: the mirrored shins change the floor there.
        int changed = 0;
        for (float y : {-0.06f, -0.1f, -0.14f})
            for (float x : {-0.05f, 0.0f, 0.05f}) changed += std::abs(lum(on, {x, y, 0}) - lum(off, {x, y, 0})) > 12;
        CHECK(changed >= 3);
        // The body above the floor and the floor away from the dancer are the same either way.
        CHECK(std::abs(lum(on, {0, 0.6f, 0}) - lum(off, {0, 0.6f, 0})) <= 2);
        CHECK(std::abs(lum(on, {0.9f, 0, 0.8f}) - lum(off, {0.9f, 0, 0.8f})) <= 2);
    }
    out->release();
    q->release();
}

TEST_CASE("dancers: the Syphon alpha: bodies and floor opaque, shadows partly, the rest untouched; color the same") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::DancerPipeline p(device(), W, H, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::Texture* out = makeOutput(W, H);
    const render::DancerStage one = stageWith(1, W, H, 0);  // no floor, the shadows on
    const V3 chest = engine::projectStage(one.cam, {0, 0.6f, 0});
    const V3 root = one.bodies[0].rootFloor;
    const V3 shadow = engine::projectStage(one.cam, {root.x, 0, root.z + 0.15f});  // in front of the feet

    // Not tracked: the frame's alpha stays 1.
    fill(out, 10, 20, 30);
    encodeOnce(p, q, out, one);
    const auto plain = read(out);
    // Tracked (the frame's alpha starts at 0): the color is the same, the alpha says what stays opaque.
    fill(out, 10, 20, 30, 0);
    encodeOnce(p, q, out, one, nullptr, true);
    const auto tracked = read(out);
    auto idx = [&](float u, float v) {
        const int x = std::clamp(int(u * W), 0, W - 1), y = std::clamp(int(v * H), 0, H - 1);
        return std::size_t((y * W + x) * 4);
    };
    for (std::size_t i = 0; i < plain.size(); i += 4) {
        REQUIRE(plain[i + 3] == 255);
        REQUIRE(tracked[i] == plain[i]);
        REQUIRE(tracked[i + 1] == plain[i + 1]);
        REQUIRE(tracked[i + 2] == plain[i + 2]);
    }
    CHECK(tracked[idx(chest.x, chest.y) + 3] == 255);
    CHECK(tracked[idx(0.5f, 0.1f) + 3] == 0);
    const int sa = tracked[idx(shadow.x, shadow.y) + 3];
    INFO("shadow alpha " << sa);
    CHECK(sa > 20);
    CHECK(sa < 250);

    // The disco floor is not keyed either: opaque where it covers the frame.
    render::DancerStage floorOnly = stageWith(0, W, H, 1);
    fill(out, 10, 20, 30, 0);
    encodeOnce(p, q, out, floorOnly, nullptr, true);
    const auto fl = read(out);
    const V3 front = engine::projectStage(floorOnly.cam, {0, 0, 1.0f});
    CHECK(fl[idx(front.x, front.y) + 3] == 255);
    CHECK(fl[idx(0.5f, 0.1f) + 3] == 0);  // the sky above the floor is still keyed
    CHECK((fl[idx(front.x, front.y)] != 10 || fl[idx(front.x, front.y) + 2] != 30));  // the floor is there

    out->release();
    q->release();
}

TEST_CASE("dancers: cost at 1920x1080 with five dancers and the floor; no render-thread allocations") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::DancerPipeline p(device(), W, H, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::Texture* out = makeOutput(W, H);
    fill(out, 40, 80, 120);
    render::DancerStage st = stageWith(5, W, H, 1);
    double sum = 0;
    int n = 0;
    std::uint64_t allocs0 = 0;
    const int frames = 90, warm = 15;
    for (int f = 0; f < frames; ++f) {
        if (f == warm) allocs0 = perf::renderAllocs().load();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        st.beat = f * 0.05;
        const double ms = encodeOnce(p, q, out, st);
        if (f >= warm) {
            sum += ms;
            ++n;
        }
        pool->release();
    }
    int parts = 0;
    for (int k = 0; k < st.bodyCount; ++k) parts += int(st.bodies[std::size_t(k)].parts.size());
    const double avg = sum / n;
    MESSAGE("1920x1080, 5 dancers (" << parts << " parts) + floor, msaa " << p.msaa() << ": " << avg << " ms GPU");
    CHECK(p.lastParts() == parts);
    PERF_BUDGET(avg < 3.0);  // target 0.5 ms; loose here (shared GPU)
    CHECK(perf::renderAllocs().load() == allocs0);
    out->release();
    q->release();
}

TEST_CASE("dancers: generated characters (every fabric, print and sunglasses shape) draw, at a similar cost") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::DancerPipeline p(device(), W, H, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::Texture* out = makeOutput(W, H);
    fill(out, 40, 80, 120);
    render::DancerStage st;
    st.cam = engine::stageCamera(double(W) / double(H), 0.42);
    const int n = 9;
    const auto slots = engine::stageSlots(n, st.cam);
    st.bodies.resize(std::size_t(n));
    for (int k = 0; k < n; ++k) {
        engine::DancerSpec spec = engine::generateDancer(engine::dance::Style(k % kStyleCount), std::uint32_t(k * 31 + 5));
        // Make sure every lens shape, fabric and print is on stage.
        spec.look.glasses = Glasses(1 + k % (int(Glasses::Count) - 1));
        spec.look.topFabric = Fabric(k % int(Fabric::Count));
        spec.look.topPattern = Pattern(k % int(Pattern::Count));
        spec.look.mirrored = k % 2 == 0;
        spec.look.glowSticks = k % 3 == 0;
        const Skeleton sk = makeSkeleton(spec.look);
        Frame place;
        place.o = {slots[std::size_t(k)].first, 0, slots[std::size_t(k)].second};
        buildBody(spec.look, sk, restPose(sk), place, 1.0f, st.bodies[std::size_t(k)]);
    }
    st.bodyCount = n;
    st.floor = 1;
    st.shadows = 1;
    bool drew = false;
    double sum = 0;
    for (int f = 0; f < 40; ++f) {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        st.beat = f * 0.05;
        const double ms = encodeOnce(p, q, out, st, &drew);
        if (f >= 10) sum += ms;
        pool->release();
    }
    MESSAGE("9 generated dancers (" << p.lastParts() << " parts) + floor at 1080p: " << sum / 30 << " ms GPU");
    CHECK(drew);
    PERF_BUDGET(sum / 30 < 5.0);  // ~1.7 ms alone; 3.35 ms seen with ctest -j8 sharing the GPU
    out->release();
    q->release();
}

TEST_CASE("dancers: long hair that bends: drawn with the tube mesh; still, as the straight tube; bent, the hair moves") {
    if (!device()) return;
    const int W = 640, H = 640;
    render::DancerPipeline p(device(), W, H, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::Texture* out = makeOutput(W, H);
    engine::DancerSpec spec = engine::generateDancer(engine::dance::Style::Jazz, 5);
    spec.look.hairStyle = HairStyle::Long;
    spec.look.hat = Hat::None;
    spec.look.glasses = Glasses::None;
    const Skeleton sk = makeSkeleton(spec.look);
    // Close up, from behind and a little to the side (the hair fills the frame).
    render::DancerStage st;
    st.cam = engine::stageCamera(1.0, 0.95);
    st.bodies.resize(1);
    st.bodyCount = 1;
    Frame place;
    place.o = {0, 0, 0};
    place = place.rotatedY(2.6f);
    place.o = {0, 0, 0};
    auto render = [&](const BodyOut& body) {
        st.bodies[0] = body;
        fill(out, 0, 0, 0);
        encodeOnce(p, q, out, st);
        return read(out);
    };
    auto differ = [](const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        int n = 0;
        for (std::size_t i = 0; i < a.size(); i += 4)
            n += std::abs(int(a[i]) - int(b[i])) + std::abs(int(a[i + 1]) - int(b[i + 1])) +
                     std::abs(int(a[i + 2]) - int(b[i + 2])) > 24;
        return n;
    };
    auto unbent = [](BodyOut b) {
        for (BodyPart& part : b.parts) part.bent = false;
        return b;
    };
    auto drawn = [](const std::vector<std::uint8_t>& a) {
        int n = 0;
        for (std::size_t i = 0; i < a.size(); i += 4) n += a[i] + a[i + 1] + a[i + 2] > 0;
        return n;
    };
    // Still: the bend changes nothing (only the tube mesh's extra rings: a few edge pixels).
    HairState still;
    still.bendLong = true;
    BodyOut rest;
    buildBody(spec.look, sk, restPose(sk), place, 1.0f, rest, &still);
    const auto a = render(rest), b = render(unbent(rest));
    const int body = drawn(a);
    MESSAGE("still: " << differ(a, b) << " of " << body << " body pixels differ");
    CHECK(body > 10000);
    CHECK(differ(a, b) * 200 < body);
    // Spun: the columns swing out and the drawn hair follows them (unlike the straight tube with the same parts).
    engine::DancerRun run(spec, 5, 0.0, 0.0, 0.0);
    for (int i = 0; i < 60; ++i) run.step(i / 60.0, 1.0 / 60, MusicState{}, 1.0f);
    DancePose spin = restPose(sk);
    for (int i = 0; i < 50; ++i) {
        spin.bodyYaw = float(i) / 60.0f * 9.0f;
        run.stepHairWithPose(spin, 1.0 / 60);
    }
    spin.bodyYaw = 0;  // (the hair as the spin left it, the body facing as before)
    BodyOut bent;
    buildBody(spec.look, sk, spin, place, 1.0f, bent, &run.hairState());
    const auto c = render(bent), d = render(unbent(bent));
    MESSAGE("spun: " << differ(c, d) << " of " << drawn(c) << " body pixels differ from the straight tube");
    CHECK(differ(c, d) * 20 > drawn(c));
    out->release();
    q->release();
}

// Not part of the suite: still views of the long styles, rigid (LongHair 0) and bending (1), each settled standing,
// from the front, 3/4 and behind, written as PNGs (REFORCE_REST_VIEWS=dir).
// Run: REFORCE_REST_VIEWS=/tmp/x reforce_render_tests -tc="*rest views*" --no-skip
TEST_CASE("dancers: long hair that bends: rest views" * doctest::skip()) {
    const char* dir = std::getenv("REFORCE_REST_VIEWS");
    if (!device() || !dir) return;
    const int W = 520, H = 520;
    render::DancerPipeline p(device(), W, H, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::Texture* out = makeOutput(W, H);
    render::DancerStage st;
    st.cam = engine::stageCamera(1.0, 1.0, engine::kFloorZMin, 0.75);
    st.bodies.resize(1);
    st.bodyCount = 1;
    const struct {
        const char* name;
        HairStyle style;
        engine::dance::Style dance;
    } looks[] = {{"long", HairStyle::Long, engine::dance::Style::Jazz},
                 {"layers", HairStyle::Layers, engine::dance::Style::House},
                 {"bangs", HairStyle::Bangs, engine::dance::Style::Pop},
                 {"hugecurls", HairStyle::HugeCurls, engine::dance::Style::Disco}};
    for (const auto& l : looks) {
        engine::DancerSpec spec = engine::generateDancer(l.dance, 3);
        spec.look.hairStyle = l.style;
        spec.look.hat = Hat::None;
        const Skeleton sk = makeSkeleton(spec.look);
        for (int bend = 0; bend < 2; ++bend) {
            engine::DancerRun run(spec, 3, 0.0, 0.0, 0.0);
            run.setLongHair(bend == 1);
            for (int i = 0; i < 60; ++i) run.step(i / 60.0, 1.0 / 60, MusicState{}, 1.0f);
            for (int i = 0; i < 300; ++i) run.stepHairWithPose(restPose(sk), 1.0 / 60);
            const char* views[] = {"front", "34", "back"};
            const float yaw[] = {0.0f, 0.8f, 3.1416f};
            for (int v = 0; v < 3; ++v) {
                Frame place;
                place = place.rotatedY(yaw[v]);
                place.o = {0, 0, 0.2f};
                BodyOut body;
                buildBody(spec.look, sk, restPose(sk), place, 1.0f, body, &run.hairState());
                st.bodies[0] = body;
                fill(out, 0, 0, 0);
                encodeOnce(p, q, out, st);
                const auto px = read(out);
                const std::string path = std::string(dir) + "/" + l.name + "_" + views[v] + "_" + (bend ? "after" : "before") + ".png";
                CHECK(render::writePngBgra8(path, px.data(), W, H, std::size_t(W) * 4));
            }
        }
    }
    out->release();
    q->release();
}
