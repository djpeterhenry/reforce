// Now-playing text (render/TextOverlay): Core Text layout and the blend over the frame.
//   REFORCE_TEXT_PNG=path writes the composited test frame for a look.
#include <doctest/doctest.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "render/ImageFiles.hpp"
#include "render/TextOverlay.hpp"

using namespace reforce::render;

namespace {
TextBlockSpec nowPlaying(const std::string& artist, const std::string& title, const std::string& year, double s = 1) {
    TextBlockSpec b;
    b.uppercase = true;
    b.maxWidth = 1840 * s;
    b.lines = {{artist, 57.6 * s, true, 0, 0}, {title, 52.8 * s, false, 4 * s, 4 * s}, {year, 43.2 * s, false, 4 * s, 4 * s}};
    return b;
}
}  // namespace

TEST_CASE("text: layout") {
    const TextImage a = rasterizeText(nowPlaying("Kolter", "Trapped", "2019"));
    REQUIRE(a.width > 100);
    // Three line boxes of 1.2 em plus the margins (4 + 4 + 4), inside room for the shadow all round.
    CHECK(a.inset > 10);
    CHECK(a.height == int(std::ceil(1.2 * (57.6 + 52.8 + 43.2) + 12)) + 2 * a.inset);
    bool lit = false;
    for (std::size_t i = 3; i < a.bgra.size(); i += 4) lit |= a.bgra[i] > 200;
    CHECK(lit);
    // Premultiplied white: color never above alpha.
    bool premul = true;
    for (std::size_t i = 0; i < a.bgra.size(); i += 4) premul &= a.bgra[i] <= a.bgra[i + 3] && a.bgra[i + 2] <= a.bgra[i + 3];
    CHECK(premul);
    // No year: one line box fewer.
    CHECK(rasterizeText(nowPlaying("Kolter", "Trapped", "")).height < a.height);
    // Too long: cut to the width with an ellipsis.
    const TextImage l = rasterizeText(nowPlaying(std::string(300, 'W'), "x", "", 0.5));
    CHECK(l.width <= int(1840 * 0.5) + 2 * l.inset + 1);
    CHECK(rasterizeText(TextBlockSpec{}).width == 0);
}

TEST_CASE("text: blended over the frame, opaque in alpha") {
    MTL::Device* dev = MTL::CreateSystemDefaultDevice();
    REQUIRE(dev);
    const int W = 1920, H = 1080;
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* out = dev->newTexture(td);
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    for (int i = 0; i < W * H; ++i) {  // a dark blue frame, transparent (as a tracked Syphon alpha could be)
        px[std::size_t(i * 4)] = 90;
        px[std::size_t(i * 4 + 1)] = 30;
        px[std::size_t(i * 4 + 2)] = 20;
        px[std::size_t(i * 4 + 3)] = 0;
    }
    out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, px.data(), NS::UInteger(W * 4));
    MTL::CommandQueue* q = dev->newCommandQueue();
    TextOverlay text(dev);
    text.set(0, nowPlaying("Bermuda Triangle", "Tay Do", "2003"));
    TextBlockSpec vocal;
    vocal.uppercase = true;
    vocal.maxWidth = 1800;
    vocal.lines = {{"Luna Vox", 57.6, true, 2, 2}, {"Say My Name (Vocal)", 52.8, false, 2, 2}};
    text.set(1, vocal);
    for (int i = 0; i < 200 && (text.width(0) == 0 || text.width(1) == 0); ++i) {
        MTL::CommandBuffer* c = q->commandBuffer();
        text.encode(c, out, nullptr, 0);  // picks up finished layouts
        c->commit();
        c->waitUntilCompleted();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(text.width(0) > 0);
    REQUIRE(text.width(1) > 0);
    TextOverlay::Draw d[2];
    d[0].x = 20;
    d[0].y = H - 20 - text.height(0);
    d[1].slot = 1;
    d[1].x = 60;
    d[1].y = d[0].y - 48 - text.height(1);
    MTL::CommandBuffer* c = q->commandBuffer();
    text.encode(c, out, d, 2);
    c->commit();
    c->waitUntilCompleted();
    out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    int white = 0, opaque = 0;
    for (int y = int(d[0].y); y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::uint8_t* p = &px[std::size_t((y * W + x) * 4)];
            white += p[0] > 240 && p[1] > 240 && p[2] > 240;
            opaque += p[3] > 200;
        }
    CHECK(white > 2000);
    CHECK(opaque >= white);
    CHECK(px[3] == 0);              // untouched elsewhere
    CHECK(px[0] == 90);
    if (const char* path = std::getenv("REFORCE_TEXT_PNG")) writePngBgra8(path, px.data(), W, H, std::size_t(W * 4));
    out->release();
    q->release();
}

TEST_CASE("text: the shadow shows only on a bright background") {
    MTL::Device* dev = MTL::CreateSystemDefaultDevice();
    REQUIRE(dev);
    const int W = 1920, H = 1080;
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::CommandQueue* q = dev->newCommandQueue();
    TextOverlay text(dev);
    text.set(0, nowPlaying("Bermuda Triangle", "Tay Do 22", "2003"));
    // Left half dark blue, right half near white.
    auto frame = [&](MTL::Texture* t) {
        std::vector<std::uint8_t> px(std::size_t(W * H * 4));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::uint8_t* p = &px[std::size_t((y * W + x) * 4)];
                const bool white = x >= 300;
                p[0] = white ? 245 : 90;
                p[1] = white ? 240 : 30;
                p[2] = white ? 235 : 20;
                p[3] = 255;
            }
        t->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, px.data(), NS::UInteger(W * 4));
    };
    auto render = [&](double strength) {
        MTL::Texture* out = dev->newTexture(td);
        frame(out);
        for (int i = 0; i < 200 && text.width(0) == 0; ++i) {
            MTL::CommandBuffer* c = q->commandBuffer();
            text.encode(c, out, nullptr, 0);
            c->commit();
            c->waitUntilCompleted();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        TextOverlay::Draw d;
        d.x = 20;
        d.y = H - 20 - text.height(0);
        TextShadow sh;
        sh.strength = strength;
        MTL::CommandBuffer* c = q->commandBuffer();
        text.encode(c, out, &d, 1, sh, 1.0 / 60);
        c->commit();
        c->waitUntilCompleted();
        std::vector<std::uint8_t> px(std::size_t(W * H * 4));
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
        out->release();
        return px;
    };
    const std::vector<std::uint8_t> plain = render(0), shaded = render(0.8);
    REQUIRE(text.width(0) > 400);
    int darkSideDiff = 0, darkened = 0;
    for (int y = H - 300; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = std::size_t((y * W + x) * 4);
            if (x < 200) darkSideDiff += plain[i] != shaded[i] || plain[i + 1] != shaded[i + 1];
            if (x > 400 && shaded[i + 1] + 60 < plain[i + 1]) ++darkened;
        }
    CHECK(darkSideDiff == 0);   // dark background: exactly the plain text
    CHECK(darkened > 2000);     // bright background: a dark halo around the letters
    if (const char* path = std::getenv("REFORCE_TEXT_SHADOW_PNG"))
        writePngBgra8(path, shaded.data(), W, H, std::size_t(W * 4));
    q->release();
}

TEST_CASE("text: karaoke lines report their words and shrink to fit") {
    TextBlockSpec k;
    k.maxWidth = 1700;
    k.fit = true;
    k.spans = true;
    k.lines = {{"If the river runs away, will you find your way?", 64, true, 0, 0}};
    const TextImage a = rasterizeText(k);
    REQUIRE(a.spans.size() == 2 * 10);
    for (std::size_t i = 0; i + 1 < a.spans.size(); ++i) CHECK(a.spans[i] <= a.spans[i + 1]);  // in order
    CHECK(a.spans[0] == doctest::Approx(0).epsilon(0.5));
    CHECK(a.spans.back() <= a.width - 2 * a.inset + 1);
    // Too wide at 64 px for 600 px: smaller type, every word still there (no ellipsis).
    k.maxWidth = 600;
    const TextImage b = rasterizeText(k);
    REQUIRE(b.spans.size() == 2 * 10);
    CHECK(b.width - 2 * b.inset <= 601);
    CHECK(b.spans.back() <= 601);
    CHECK(b.height < a.height);
    // Without `fit`, the same line is cut and keeps its size.
    k.fit = false;
    CHECK(rasterizeText(k).height == a.height);
}

TEST_CASE("text: karaoke fill, full left of the split and dim right of it") {
    MTL::Device* dev = MTL::CreateSystemDefaultDevice();
    REQUIRE(dev);
    const int W = 1920, H = 1080;
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* out = dev->newTexture(td);
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    for (std::size_t i = 0; i < px.size(); i += 4) px[i + 3] = 255;   // black, opaque
    out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, px.data(), NS::UInteger(W * 4));
    MTL::CommandQueue* q = dev->newCommandQueue();
    TextOverlay text(dev);
    TextBlockSpec k;
    k.maxWidth = 1700;
    k.fit = k.spans = true;
    k.lines = {{"SLOW BOATS OVER SILVER LAKES", 64, true, 0, 0}};
    text.set(0, k);
    for (int i = 0; i < 200 && text.width(0) == 0; ++i) {
        text.adoptReady();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(text.width(0) > 0);
    REQUIRE(text.spans(0).size() == 10);
    TextOverlay::Draw d;
    d.x = 100;
    d.y = 500;
    d.fillX = text.spans(0)[3];   // through "DIVE"
    d.dim = 0.4;
    MTL::CommandBuffer* c = q->commandBuffer();
    text.encode(c, out, &d, 1);
    c->commit();
    c->waitUntilCompleted();
    out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    int leftMax = 0, rightMax = 0;
    const int split = int(d.x + d.fillX);
    for (int y = int(d.y); y < int(d.y) + text.height(0); ++y)
        for (int x = int(d.x); x < int(d.x) + text.width(0); ++x) {
            const int v = px[std::size_t((y * W + x) * 4 + 1)];
            if (x < split - 2) leftMax = std::max(leftMax, v);
            if (x > split + 2) rightMax = std::max(rightMax, v);
        }
    CHECK(leftMax > 240);
    CHECK(rightMax > 60);
    CHECK(rightMax < 0.4 * 255 + 8);
    if (const char* path = std::getenv("REFORCE_TEXT_KARAOKE_PNG")) writePngBgra8(path, px.data(), W, H, std::size_t(W * 4));
    out->release();
    q->release();
}
