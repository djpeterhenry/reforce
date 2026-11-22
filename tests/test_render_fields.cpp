// GPU organics (PLAN 5O.6): render/FieldPipeline offscreen.
//   - ripples: a drop spreads as a ring at the set speed, stays bounded (stable) and dies away; a reset clears it
//   - Julia: the set is dark, its outside lit, the quad covers the frame
#include <doctest/doctest.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/FieldPipeline.hpp"
#include "render/ScenePipeline.hpp"

using namespace reforce;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

std::vector<float> readHeights(MTL::CommandQueue* q, render::FieldPipeline& f, int slot) {
    const int w = f.rippleCols(), h = f.rippleRows();
    MTL::Buffer* buf = device()->newBuffer(NS::UInteger(w * h * 4), MTL::ResourceStorageModeShared);
    MTL::CommandBuffer* cmd = q->commandBuffer();
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->copyFromTexture(f.height(slot), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(w), NS::UInteger(h), 1),
                          buf, 0, NS::UInteger(w * 4), NS::UInteger(w * h * 4));
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    const auto* p = static_cast<const float*>(buf->contents());
    std::vector<float> out(p, p + w * h);
    buf->release();
    return out;
}

struct Lum {
    int w = 0, h = 0;
    std::vector<float> lum, alpha;
};

Lum readTarget(MTL::CommandQueue* q, MTL::Texture* tex) {
    Lum r;
    r.w = int(tex->width());
    r.h = int(tex->height());
    MTL::Buffer* buf = device()->newBuffer(NS::UInteger(r.w * r.h * 2), MTL::ResourceStorageModeShared);
    MTL::CommandBuffer* cmd = q->commandBuffer();
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->copyFromTexture(tex, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(r.w), NS::UInteger(r.h), 1), buf, 0,
                          NS::UInteger(r.w * 2), NS::UInteger(r.w * r.h * 2));
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    const auto* p = static_cast<const std::uint8_t*>(buf->contents());
    for (int i = 0; i < r.w * r.h; ++i) {
        r.lum.push_back(p[i * 2] / 255.0f);
        r.alpha.push_back(p[i * 2 + 1] / 255.0f);
    }
    buf->release();
    return r;
}

}  // namespace

TEST_CASE("fields: a ripple spreads at its speed, stays bounded and dies away") {
    const uint32_t W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::FieldPipelineOptions o;
    o.maxLive = 1;
    o.rippleRows = 180;
    render::FieldPipeline fields(scene, o);
    REQUIRE(fields.rippleRows() == 180);
    REQUIRE(fields.rippleCols() == 320);
    MTL::CommandQueue* q = device()->newCommandQueue();

    engine::OrganicField f;
    f.kind = engine::OrganicField::Kind::Ripples;
    f.speed = 0.25;  // frame heights per second: 45 cells per second
    f.decay = 4;
    f.dt = 1.0 / 60.0;
    auto step = [&](int n) {
        for (int i = 0; i < n; ++i) {
            MTL::CommandBuffer* cmd = q->commandBuffer();
            fields.encode(cmd, 0, f);
            cmd->commit();
            cmd->waitUntilCompleted();
            f.reset = false;
            f.drops.clear();
        }
    };
    f.reset = true;
    f.drops.push_back({0.5, 0.5, 0.02, 1.0});
    step(1);
    auto h = readHeights(q, fields, 0);
    const int cw = fields.rippleCols(), ch = fields.rippleRows();
    auto at = [&](int x, int y) { return h[std::size_t(y * cw + x)]; };
    // The drop: a bump about 1 high at the centre (it has not moved yet).
    CHECK(at(cw / 2, ch / 2) == doctest::Approx(1.0).epsilon(0.25));
    float peak0 = 0;
    for (float v : h) peak0 = std::max(peak0, std::abs(v));
    CHECK(peak0 < 1.5f);

    // One second later the ring's crest is about speed * 1 s = 45 cells from the centre.
    step(60);
    h = readHeights(q, fields, 0);
    int bestR = -1;
    float best = 0, peak = 0;
    for (int r = 0; r < ch / 2 - 2; ++r) {
        const float v = at(cw / 2 + r, ch / 2);
        if (v > best) {
            best = v;
            bestR = r;
        }
    }
    for (float v : h) {
        CHECK(std::isfinite(v));
        peak = std::max(peak, std::abs(v));
    }
    CHECK(bestR > 30);
    CHECK(bestR < 60);
    CHECK(peak < 0.6f);
    // Absorbing edges and the decay: still water after a while (no instability).
    step(600);
    h = readHeights(q, fields, 0);
    float late = 0;
    for (float v : h) late = std::max(late, std::abs(v));
    CHECK(late < 0.6f * std::exp(-10.0f / 4.0f));  // at most the decay alone
    // A reset clears the water.
    f.reset = true;
    f.drops.clear();
    step(1);
    h = readHeights(q, fields, 0);
    float cleared = 0;
    for (float v : h) cleared = std::max(cleared, std::abs(v));
    CHECK(cleared == 0.0f);
    q->release();
}

TEST_CASE("fields: Julia set dark inside, lit outside") {
    const uint32_t W = 320, H = 180;
    render::ScenePipeline scene(device(), W, H, {});
    render::FieldPipelineOptions o;
    o.maxLive = 1;
    render::FieldPipeline fields(scene, o);
    MTL::CommandQueue* q = device()->newCommandQueue();
    engine::OrganicField f;
    f.kind = engine::OrganicField::Kind::Julia;
    f.cre = -0.8;
    f.cim = 0.156;
    f.iter = 120;
    f.gain = 1;
    MTL::CommandBuffer* cmd = q->commandBuffer();
    const render::SolidQuad sq = fields.encode(cmd, 0, f);
    cmd->commit();
    cmd->waitUntilCompleted();
    CHECK(sq.visible);
    CHECK(sq.w == float(W));
    CHECK(sq.h == float(H));
    const Lum t = readTarget(q, fields.target(0));
    // Pixel (x, y) -> tree units (x +-W/H, y +-1) -> z = 1.6 * p. The origin is inside this set (c = -.8 + .156i);
    // a point far out (z = 1.6 * (1.7, .9)) escapes at once.
    auto px = [&](double zx, double zy, float& lum, float& alpha) {
        const double ux = (zx / 1.6 / (double(W) / H) + 1) * 0.5, uy = (1 - zy / 1.6) * 0.5;
        const int x = std::clamp(int(ux * W), 0, int(W) - 1), y = std::clamp(int(uy * H), 0, int(H) - 1);
        lum = t.lum[std::size_t(y) * W + std::size_t(x)];
        alpha = t.alpha[std::size_t(y) * W + std::size_t(x)];
    };
    float l, a;
    px(0, 0, l, a);
    CHECK(a == 0.0f);
    CHECK(l == 0.0f);
    // The boundary is bright somewhere: the brightest pixel is well lit, the mean is low (mostly dark).
    double sum = 0;
    float mx = 0;
    for (std::size_t i = 0; i < t.lum.size(); ++i) {
        const float v = t.alpha[i] > 0 ? t.lum[i] / t.alpha[i] : 0.0f;
        sum += v;
        mx = std::max(mx, v);
    }
    CHECK(mx > 0.8f);
    CHECK(sum / double(t.lum.size()) < 0.5);
    q->release();
}
