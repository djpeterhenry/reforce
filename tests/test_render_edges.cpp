// Anti-aliasing in color (Peter 2026-11-22): reforce.LineEdges = color and the reforce.Supersample downsampler.
//   - On a ColorMap that is not a ramp (black -> red -> green -> white), a white antialiased line on black fringes
//     red / green with G-Force's edges; with color edges its edge pixels are grays (black and white mixed).
//   - The downsampler averages in linear light: a black / white checker becomes sRGB ~188, not 128.
#include <doctest/doctest.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "line_harness.hpp"
#include "render/Downsample.hpp"

using namespace reforce;
using namespace reforce::linetest;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

MTL::Texture* bgra(int w, int h) {
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(w), NS::UInteger(h), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    return device()->newTexture(td);
}

// Chromatic pixels (max - min channel > 60 of 255) in a white-line-on-black frame.
int fringes(bool colorEdges) {
    const int W = 160, H = 64;
    render::ScenePipelineOptions o;
    o.antialias = true;
    o.colorEdges = colorEdges;
    Harness hs(device(), W, H, o, Field::Void);
    std::vector<engine::LineSegment> segs{{10.3f, 20.2f, 150.7f, 44.6f, 1.0f, 1.0f, 2.2f, 0.0f},
                                          {20.0f, 50.5f, 140.0f, 12.25f, 1.0f, 1.0f, 1.6f, 1.0f}};
    hs.step(segs);
    engine::Palette pal{};
    engine::MapLut lut{};
    for (int i = 0; i < engine::kPaletteSize; ++i) {
        const float x = float(i) / 255.0f;
        lut[std::size_t(i)] = x;
        auto& e = pal[std::size_t(i)];
        if (x < 1.0f / 3.0f) e = {3.0f * x, 0, 0, 1};                              // black -> red
        else if (x < 2.0f / 3.0f) e = {2.0f - 3.0f * x, 3.0f * x - 1.0f, 0, 1};    // red -> green
        else e = {3.0f * x - 2.0f, 1, 3.0f * x - 2.0f, 1};                         // green -> white
    }
    MTL::Texture* out = bgra(W, H);
    MTL::CommandBuffer* cmd = hs.queue()->commandBuffer();
    hs.pipeline().encodeColorize(cmd, pal, lut, out, nullptr, nullptr, 0, false, nullptr, 0, nullptr, 0);
    cmd->commit();
    cmd->waitUntilCompleted();
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    out->release();
    int n = 0, lit = 0;
    for (int i = 0; i < W * H; ++i) {
        const int b = px[std::size_t(i) * 4], g = px[std::size_t(i) * 4 + 1], r = px[std::size_t(i) * 4 + 2];
        n += std::max({r, g, b}) - std::min({r, g, b}) > 60;
        lit += std::max({r, g, b}) > 200;
    }
    CHECK(lit > 300);  // the lines are there either way
    return n;
}

}  // namespace

TEST_CASE("color edges: no multi-colored fringes on a non-ramp ColorMap; G-Force's edges have them") {
    if (!device()) return;
    const int gforce = fringes(false), color = fringes(true);
    MESSAGE("chromatic edge pixels: G-Force edges " << gforce << ", color edges " << color);
    CHECK(gforce > 100);
    CHECK(color < gforce / 20);
}

TEST_CASE("supersample: the downsampler averages 2x2 in linear light and keeps alpha") {
    if (!device()) return;
    const int W = 8, H = 8;
    MTL::Texture* src = bgra(W * 2, H * 2);
    std::vector<std::uint8_t> init(std::size_t(W * 2 * H * 2 * 4));
    for (int y = 0; y < H * 2; ++y)
        for (int x = 0; x < W * 2; ++x) {
            const std::uint8_t v = (x + y) % 2 ? 255 : 0;
            std::uint8_t* p = &init[(std::size_t(y) * W * 2 + std::size_t(x)) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = v;  // alpha follows: averages to 128
        }
    src->replaceRegion(MTL::Region(0, 0, NS::UInteger(W * 2), NS::UInteger(H * 2)), 0, init.data(), NS::UInteger(W * 2 * 4));
    render::Downsampler down(device(), W, H, 2, 1);
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::CommandBuffer* cmd = q->commandBuffer();
    MTL::Texture* out = down.encode(cmd, src, 0);
    MTL::Texture* shared = bgra(W, H);
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->copyFromTexture(out, shared);
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    shared->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    for (int i = 0; i < W * H; ++i) {
        CHECK(int(px[std::size_t(i) * 4 + 1]) == doctest::Approx(188).epsilon(0.02));  // linear 0.5 in sRGB
        CHECK(int(px[std::size_t(i) * 4 + 3]) == doctest::Approx(128).epsilon(0.02));
    }
    shared->release();
    src->release();
    q->release();
}
