// GPU organics (PLAN 5O.6): render/FieldPipeline offscreen.
//   - ripples: a drop spreads as a ring at the set speed, stays bounded (stable) and dies away; a reset clears it
//   - Julia: the set is dark, its outside lit, the quad covers the frame
//   - Mandelbrot: dark inside, lit at the edge, matching a CPU reference at depth 0 and 2^30 (double-float and
//     perturbation; float32 cannot resolve 2^30); Res renders a corner
#include <doctest/doctest.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
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
    // The drop: a bump about 1 high at the center (it has not moved yet).
    CHECK(at(cw / 2, ch / 2) == doctest::Approx(1.0).epsilon(0.25));
    float peak0 = 0;
    for (float v : h) peak0 = std::max(peak0, std::abs(v));
    CHECK(peak0 < 1.5f);

    // One second later the ring's crest is about speed * 1 s = 45 cells from the center.
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

namespace {

// CPU reference (double): escaped or not within `iter`, and the distance estimate in pixels.
struct MandelRef {
    bool escaped = false;
    double dist = 0;
};

MandelRef mandelRef(double cr, double ci, int iter, double pix) {
    double zr = 0, zi = 0, dr = 0, di = 0, r2 = 0;
    int i = 0;
    for (; i < iter; ++i) {
        const double ndr = 2 * (zr * dr - zi * di) + 1, ndi = 2 * (zr * di + zi * dr);
        dr = ndr;
        di = ndi;
        const double t = zr * zr - zi * zi + cr;
        zi = 2 * zr * zi + ci;
        zr = t;
        r2 = zr * zr + zi * zi;
        if (r2 > 256) break;
    }
    if (i >= iter) return {};
    return {true, 0.5 * std::sqrt(r2) * std::log(r2) / std::hypot(dr, di) / pix};
}

// Renders `f` (Band 0) and compares every 5th pixel with the CPU: inside must be dark, escaped within 64 px of the
// set lit (`agree`), and the shading (from the distance estimate) the same to within .05 wherever it is over .4
// (`shade`). Also the number of distinct pixel values.
struct MandelCheck {
    double agree = 0, shade = 0;
    int checked = 0, shaded = 0, distinct = 0, insideSeen = 0, edgeSeen = 0;
};

MandelCheck checkMandel(render::FieldPipeline& fields, MTL::CommandQueue* q, const engine::OrganicField& f, uint32_t W,
                        uint32_t H) {
    MTL::CommandBuffer* cmd = q->commandBuffer();
    fields.encode(cmd, 0, f);
    cmd->commit();
    cmd->waitUntilCompleted();
    const Lum t = readTarget(q, fields.target(0));
    const double aspect = double(W) / H, k = f.half / f.scale, pix = 2.0 * k / H;
    MandelCheck r;
    int ok = 0, shadeOk = 0;
    std::set<std::pair<int, int>> values;
    for (uint32_t y = 2; y < H; y += 5)
        for (uint32_t x = 2; x < W; x += 5) {
            const double px = ((x + 0.5) / W * 2 - 1) * aspect - f.x, py = 1 - 2 * (y + 0.5) / H - f.y;
            const double cr = f.cx + k * (std::cos(f.rot) * px - std::sin(f.rot) * py);
            const double ci = f.cy + k * (std::sin(f.rot) * px + std::cos(f.rot) * py);
            const MandelRef m = mandelRef(cr, ci, f.iter, pix);
            const float a = t.alpha[std::size_t(y) * W + x];
            values.insert({int(std::lround(t.lum[std::size_t(y) * W + x] * 255)), int(std::lround(a * 255))});
            if (!m.escaped) {
                ++r.insideSeen;
                ++r.checked;
                ok += a == 0.0f ? 1 : 0;
            } else if (m.dist < 64) {
                ++r.edgeSeen;
                ++r.checked;
                ok += a > 0.5f ? 1 : 0;
            }
            if (m.escaped) {
                const double nearV = std::clamp(1 - std::log2(std::max(m.dist, 1.0)) / 12, 0.0, 1.0);
                const double g = std::clamp(f.glow / std::max(m.dist, 1e-6), 0.0, 1.0);
                const double lum = std::clamp(std::max(nearV * f.gain, f.gain * g * g), 0.0, 1.0);
                if (lum > 0.4) {
                    ++r.shaded;
                    shadeOk += std::abs(t.lum[std::size_t(y) * W + x] - lum) < 0.05 ? 1 : 0;
                }
            }
        }
    r.agree = r.checked ? double(ok) / r.checked : 0;
    r.shade = r.shaded ? double(shadeOk) / r.shaded : 0;
    r.distinct = int(values.size());
    return r;
}

}  // namespace

TEST_CASE("fields: Mandelbrot dark inside, lit at the edge, at depth 0 and 2^30 (double-float, perturbation)") {
    const uint32_t W = 320, H = 180;
    render::ScenePipeline scene(device(), W, H, {});
    render::FieldPipelineOptions o;
    o.maxLive = 1;
    render::FieldPipeline fields(scene, o);
    MTL::CommandQueue* q = device()->newCommandQueue();
    engine::OrganicField f;
    f.kind = engine::OrganicField::Kind::Mandel;
    f.gain = 1;
    f.bands = 0;
    f.glow = 0.5;
    // Depth 0: the whole set (float32), every mode agrees with the CPU.
    f.cx = engine::kMandelHomeRe;
    f.cy = 0;
    f.half = engine::kMandelHomeHalf;
    f.iter = 200;
    for (int mode = 0; mode <= 2; ++mode) {
        CAPTURE(mode);
        f.precision = mode;
        const MandelCheck c = checkMandel(fields, q, f, W, H);
        CHECK(c.insideSeen > 200);
        CHECK(c.edgeSeen > 200);
        CHECK(c.agree > 0.97);
        CHECK(c.shaded > 500);
        CHECK(c.shade > 0.95);
    }
    // 2^30 into the seahorse valley's double spiral (the dive's view there): float32 sees one value; double-float
    // and perturbation match the CPU, turned and offset too.
    const engine::MandelTarget& t = engine::mandelTarget(0);
    const double depth = 30;
    const double s = std::exp2(-1.5 * depth);
    f.cx = t.re + (engine::kMandelHomeRe - t.re) * s;
    f.cy = t.im - t.im * s;
    f.half = engine::kMandelHomeHalf * std::exp2(-depth);
    f.iter = int(1.5 * (t.itA + t.itB * depth));
    f.precision = 0;
    const MandelCheck flat = checkMandel(fields, q, f, W, H);
    CHECK(flat.distinct <= 3);
    CHECK(flat.shade < 0.5);
    for (int mode = 1; mode <= 2; ++mode) {
        CAPTURE(mode);
        f.precision = mode;
        f.rot = 0;
        f.x = 0;
        MandelCheck c = checkMandel(fields, q, f, W, H);
        CHECK(c.edgeSeen > 300);
        CHECK(c.agree > 0.97);
        CHECK(c.shaded > 1000);
        CHECK(c.shade > 0.95);
        CHECK(c.distinct > 50);
        f.rot = 0.7;
        f.x = 0.2;
        c = checkMandel(fields, q, f, W, H);
        CHECK(c.edgeSeen > 300);
        CHECK(c.agree > 0.97);
        CHECK(c.shade > 0.95);
    }
    // A mini filling the frame: the affine interior test (most of its inside skipped) changes nothing against the
    // CPU (no false insides), for every mini in the table, upright and turned.
    for (int i = 0; i < engine::mandelTargetCount(); ++i) {
        const engine::MandelTarget& mt = engine::mandelTarget(i);
        if (mt.kind != engine::MandelKind::Mini) continue;
        CAPTURE(mt.name);
        f.cx = mt.frameRe();
        f.cy = mt.frameIm();
        f.half = engine::kMandelHomeHalf * std::exp2(-mt.miniDeep());
        f.iter = std::min(20000, int(1.5 * 45 * mt.period));
        f.precision = 2;
        f.x = 0;
        f.nre = mt.re;
        f.nim = mt.im;
        f.sre = mt.sre;
        f.sim = mt.sim;
        for (double rot : {mt.angle(), 0.3}) {
            f.rot = rot;
            f.mini = false;
            const MandelCheck plain = checkMandel(fields, q, f, W, H);
            f.mini = true;
            const MandelCheck fast = checkMandel(fields, q, f, W, H);
            CHECK(plain.insideSeen > 100);
            CHECK(plain.agree > 0.99);
            CHECK(fast.agree > 0.99);
            CHECK(fast.agree >= plain.agree - 0.002);
            CHECK(fast.shade > 0.97);
        }
    }
    f.mini = false;
    // Res .5: only the top-left quarter is drawn; the quad samples just that.
    f.rot = 0;
    f.x = 0;
    f.precision = 2;
    f.res = 0.5;
    MTL::CommandBuffer* cmd = q->commandBuffer();
    const render::SolidQuad sq = fields.encode(cmd, 0, f);
    cmd->commit();
    cmd->waitUntilCompleted();
    CHECK(sq.visible);
    CHECK(sq.u1 == doctest::Approx((160 - 0.5) / 320.0));
    CHECK(sq.v1 == doctest::Approx((90 - 0.5) / 180.0));
    CHECK(sq.w == float(W));
    q->release();
}
