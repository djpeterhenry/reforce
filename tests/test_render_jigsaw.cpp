// The jigsaw (docs/3d-design.md "Jigsaw"): render/JigsawPipeline on the GPU, offscreen.
//   - the GPU cut map agrees with the CPU cut (engine/JigsawShape.inc compiled both ways)
//   - a whole frame is left alone; every piece home with the seams faded out gives the frame back exactly
//   - empty slots show the darker table; a loose piece shows the frame from its home place wherever it is
//   - cost at 1920x1080, no render-thread allocations
#include <doctest/doctest.h>

#include "PerfBudget.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/Jigsaw.hpp"
#include "engine/Perf.hpp"
#include "render/JigsawPipeline.hpp"

using namespace reforce;
namespace jig = reforce::engine::jigsaw;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

struct Target {
    int w, h;
    MTL::Texture* tex = nullptr;
    std::vector<std::uint8_t> px;
    Target(int w_, int h_) : w(w_), h(h_), px(std::size_t(w_ * h_ * 4)) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm,
                                                                                 NS::UInteger(w), NS::UInteger(h), false);
        td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModeShared);
        tex = device()->newTexture(td);
    }
    ~Target() { tex->release(); }
    // A colorful test picture: a gradient in red / green, blue by position on a coarse grid.
    void fill() {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint8_t* p = &px[std::size_t((y * w + x) * 4)];
                p[0] = std::uint8_t(60 + ((x / 23 + y / 17) % 3) * 60);  // B
                p[1] = std::uint8_t(40 + 200 * y / h);                     // G
                p[2] = std::uint8_t(40 + 200 * x / w);                     // R
                p[3] = 255;
            }
        tex->replaceRegion(MTL::Region(0, 0, NS::UInteger(w), NS::UInteger(h)), 0, px.data(), NS::UInteger(w * 4));
    }
    std::vector<std::uint8_t> read() {
        std::vector<std::uint8_t> out(px.size());
        tex->getBytes(out.data(), NS::UInteger(w * 4), MTL::Region(0, 0, NS::UInteger(w), NS::UInteger(h)), 0);
        return out;
    }
};

void run(MTL::CommandQueue* q, render::JigsawPipeline& jp, Target& t, const render::JigsawDraw& d, double* gpuMs = nullptr) {
    MTL::CommandBuffer* cmd = q->commandBuffer();
    jp.encode(cmd, t.tex, d, false);
    cmd->commit();
    cmd->waitUntilCompleted();
    REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
    if (gpuMs) *gpuMs = (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
}

// Every piece home and flat.
jig::Frame homeFrame(const jig::Grid& g) {
    jig::Frame f;
    f.grid = g;
    f.count = g.count();
    for (int p = 0; p < f.count; ++p) {
        const auto h = jig::homeCenter(g, p);
        f.pieces[std::size_t(p)].x = h.x;
        f.pieces[std::size_t(p)].y = h.y;
    }
    f.seam = 0;
    f.glow = 0;
    f.whole = false;
    return f;
}

}  // namespace

TEST_CASE("jigsaw: the GPU cut map agrees with the CPU cut") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::JigsawPipeline jp(device(), W, H);
    MTL::CommandQueue* q = device()->newCommandQueue();
    Target t(W, H);
    t.fill();
    for (uint64_t seed : {3ull, 41ull}) {
        render::JigsawDraw d;
        d.frame = homeFrame(jig::fitGrid(32, double(W) / H, seed));
        d.frame.seam = 1;
        run(q, jp, t, d);
        // Read the (private) cut map back.
        MTL::Buffer* buf = device()->newBuffer(NS::UInteger(W * H * 4), MTL::ResourceStorageModeShared);
        MTL::CommandBuffer* cmd = q->commandBuffer();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->copyFromTexture(jp.cutMap(), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(W), NS::UInteger(H), 1), buf, 0,
                              NS::UInteger(W * 4), NS::UInteger(W * H * 4));
        blit->endEncoding();
        cmd->commit();
        cmd->waitUntilCompleted();
        const auto* m = static_cast<const std::uint8_t*>(buf->contents());
        const jig::Grid& g = d.frame.grid;
        const float unit = float(H) / float(g.rows);
        int checked = 0, agree = 0, near = 0;
        for (int y = 0; y < H; y += 3)
            for (int x = 0; x < W; x += 3) {
                const jig::Point p{(float(x) + 0.5f) / unit, (float(y) + 0.5f) / unit};
                const auto o = jig::owner(g, p);
                const std::uint8_t* c = &m[std::size_t((y * W + x) * 4)];
                CHECK(c[0] < g.count());
                const float gpx = float(c[2] * 256 + c[3]) / 256.0f - 128.0f;
                if (o.index < 0 || std::fabs(o.g * unit) < 1.0f) continue;  // on an outline
                ++checked;
                agree += c[0] == o.index ? 1 : 0;
                near += std::fabs(gpx - std::max(-127.99f, o.g * unit)) < 0.05f ? 1 : 0;
            }
        CHECK(checked > W * H / 9 * 9 / 10);
        CHECK(agree == checked);
        CHECK(near > checked * 99 / 100);
        buf->release();
    }
    CHECK(jp.cutBuilds() == 2);
    q->release();
}

TEST_CASE("jigsaw: whole leaves the frame alone; home with no seams is the frame; holes show the table; loose pieces "
          "show their home picture") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::JigsawPipeline jp(device(), W, H);
    MTL::CommandQueue* q = device()->newCommandQueue();
    Target t(W, H);
    const jig::Grid g = jig::fitGrid(32, double(W) / H, 7);
    const float unit = float(H) / float(g.rows);
    auto pix = [&](const std::vector<std::uint8_t>& v, float bx, float by, int c) {
        const int x = std::clamp(int(bx * unit), 0, W - 1), y = std::clamp(int(by * unit), 0, H - 1);
        return int(v[std::size_t((y * W + x) * 4 + c)]);
    };
    // Whole: nothing is drawn.
    {
        t.fill();
        render::JigsawDraw d;
        d.frame = homeFrame(g);
        d.frame.whole = true;
        run(q, jp, t, d);
        CHECK(t.read() == t.px);
    }
    // Every piece home, seams faded out: exactly the frame.
    {
        t.fill();
        render::JigsawDraw d;
        d.frame = homeFrame(g);
        run(q, jp, t, d);
        const auto out = t.read();
        int diff = 0;
        for (std::size_t i = 0; i < out.size(); ++i) diff = std::max(diff, std::abs(int(out[i]) - int(t.px[i])));
        CHECK(diff <= 1);
    }
    // With seams: the cut shows (dark lines), the inside stays the frame.
    {
        t.fill();
        render::JigsawDraw d;
        d.frame = homeFrame(g);
        d.frame.seam = 1;
        run(q, jp, t, d);
        const auto out = t.read();
        int darker = 0;
        for (std::size_t i = 1; i < out.size(); i += 4) darker += int(out[i]) + 20 < int(t.px[i]) ? 1 : 0;
        CHECK(darker > 1000);
        const auto h = jig::homeCenter(g, 10);
        CHECK(std::abs(pix(out, h.x, h.y, 1) - pix(t.px, h.x, h.y, 1)) <= 1);
    }
    // Piece 5 away: its slot is the empty table (darker); piece 5 loose over slot 10's center, flat and upright, shows
    // its own home picture there.
    {
        t.fill();
        render::JigsawDraw d;
        d.frame = homeFrame(g);
        d.frame.seam = 1;
        d.frame.pieces[5].landed = false;
        const auto h5 = jig::homeCenter(g, 5), h10 = jig::homeCenter(g, 10);
        d.frame.pieces[5].x = h10.x;
        d.frame.pieces[5].y = h10.y;
        d.frame.pieces[5].z = 0.0f;
        d.frame.order[0] = 5;
        d.frame.loose = 1;
        run(q, jp, t, d);
        const auto out = t.read();
        auto lum = [&](const std::vector<std::uint8_t>& v, float bx, float by) {
            return pix(v, bx, by, 0) + pix(v, bx, by, 1) + pix(v, bx, by, 2);
        };
        CHECK(lum(out, h5.x, h5.y) < lum(t.px, h5.x, h5.y) * 0.6);  // the table, dimmed
        for (int c = 0; c < 3; ++c)
            CHECK(std::abs(pix(out, h10.x, h10.y, c) - pix(t.px, h5.x, h5.y, c)) <= 2);  // piece 5's picture
        CHECK(std::abs(pix(out, h10.x, h10.y, 2) - pix(t.px, h10.x, h10.y, 2)) > 20);  // not slot 10's own
        // Lifted, it casts a shadow on the board beside it (towards the lower right).
        d.frame.pieces[5].z = 0.3f;
        run(q, jp, t, d);  // (t holds the last result; refill)
        t.fill();
        run(q, jp, t, d);
        const auto lifted = t.read();
        // Just past the piece's right edge, below: darker than the frame.
        int shadowed = 0;
        for (float dx = 0.5f; dx < 0.75f; dx += 0.02f)
            shadowed += pix(lifted, h10.x + dx * g.cw, h10.y + 0.35f, 1) + 8 < pix(t.px, h10.x + dx * g.cw, h10.y + 0.35f, 1);
        CHECK(shadowed > 3);
    }
    q->release();
}

TEST_CASE("jigsaw: cost at 1920x1080 (the board and 32 loose pieces; the cut map once); no render-thread allocations") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::JigsawPipeline jp(device(), W, H);
    MTL::CommandQueue* q = device()->newCommandQueue();
    Target t(W, H);
    t.fill();
    jig::Solver solver;
    jig::Config c;
    jig::Input in;
    double cutMs = 0, sum = 0, worst = 0;
    int n = 0;
    std::uint64_t allocs0 = 0;
    const int frames = 80, warm = 10;
    for (int f = 0; f < frames; ++f) {
        if (f == warm) allocs0 = perf::renderAllocs().load();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        in.turn = 4.5 + 0.02 * f;  // the burst and the pile: every piece loose
        render::JigsawDraw d;
        d.frame = solver.frame(f < warm - 1 ? 9 : 10, c, in);  // a new cut just before measuring (pipelines warm)
        double ms = 0;
        run(q, jp, t, d, &ms);
        if (f == warm - 1) cutMs = ms;
        if (f >= warm) {
            sum += ms;
            worst = std::max(worst, ms);
            ++n;
        }
        pool->release();
    }
    const double avg = sum / n;
    MESSAGE("jigsaw at 1920x1080: " << avg << " ms GPU avg (max " << worst << "), first frame with the cut map "
                                    << cutMs << " ms");
    CHECK(jp.cutBuilds() == 2);
    PERF_BUDGET(cutMs < 8.0);
    PERF_BUDGET(avg < 1.5);
    CHECK(perf::renderAllocs().load() == allocs0);
    q->release();
}
