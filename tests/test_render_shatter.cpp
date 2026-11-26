// Shatter (docs/3d-design.md "Shatter"): render/ShatterPipeline on the GPU, offscreen.
//   - the GPU crack map agrees with the CPU fracture (engine/ShatterShape.inc compiled both ways), its values in range
//   - an intact pane leaves the frame as it is; a piece cracked loose shifts the visuals behind it a little
//   - grown cracks are visible lines (over dark and bright), cracks not grown yet are not
//   - a shard refracts the visuals: flat at home it shows the frame, turned it shows them shifted and bent
//   - the shards at the moment of breaking cover the pane: the frame shows through them as through the pane
//   - cost at 1920x1080, no render-thread allocations
#include <doctest/doctest.h>

#include "PerfBudget.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/Perf.hpp"
#include "engine/Shatter.hpp"
#include "render/ShatterPipeline.hpp"

using namespace reforce;
namespace sh = reforce::engine::shatter;

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
    // A test picture: `mode` 0 a colorful gradient on a coarse grid, 1 flat dark gray, 2 flat light gray.
    void fill(int mode = 0) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint8_t* p = &px[std::size_t((y * w + x) * 4)];
                if (mode == 0) {
                    p[0] = std::uint8_t(60 + ((x / 23 + y / 17) % 3) * 60);  // B
                    p[1] = std::uint8_t(40 + 200 * y / h);                     // G
                    p[2] = std::uint8_t(40 + 200 * x / w);                     // R
                } else {
                    p[0] = p[1] = p[2] = mode == 1 ? 30 : 200;
                }
                p[3] = 255;
            }
        tex->replaceRegion(MTL::Region(0, 0, NS::UInteger(w), NS::UInteger(h)), 0, px.data(), NS::UInteger(w * 4));
    }
    std::vector<std::uint8_t> read() const {
        std::vector<std::uint8_t> out(px.size());
        tex->getBytes(out.data(), NS::UInteger(w * 4), MTL::Region(0, 0, NS::UInteger(w), NS::UInteger(h)), 0);
        return out;
    }
    int at(const std::vector<std::uint8_t>& v, int x, int y, int c) const {
        return int(v[std::size_t((std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)) * 4 + c)]);
    }
};

void run(MTL::CommandQueue* q, render::ShatterPipeline& sp, Target& t, const sh::Frame& f, const sh::Look& look,
         double* gpuMs = nullptr) {
    MTL::CommandBuffer* cmd = q->commandBuffer();
    sp.encode(cmd, t.tex, f, look, false);
    cmd->commit();
    cmd->waitUntilCompleted();
    REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
    if (gpuMs) *gpuMs = (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
}

// A frame with plan p as the intact pane: nothing grown, nothing tilted.
std::unique_ptr<sh::Frame> paneFrame(const sh::Plan& p) {
    auto f = std::make_unique<sh::Frame>();
    f->pane = &p;
    f->paneBeat = 10;
    f->offset = 0;
    f->sheen = 1;
    f->impactAge.fill(-1.0f);
    return f;
}

sh::Look plainLook() {
    sh::Look l;
    l.sheen = 0;  // (the intact glass's faint sheen off: flat glass then leaves the frame exactly as it is)
    return l;
}

std::vector<std::uint16_t> readMap(MTL::CommandQueue* q, MTL::Texture* map, int W, int H) {
    MTL::Buffer* buf = device()->newBuffer(NS::UInteger(W * H * 8), MTL::ResourceStorageModeShared);
    MTL::CommandBuffer* cmd = q->commandBuffer();
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->copyFromTexture(map, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(W), NS::UInteger(H), 1), buf, 0,
                          NS::UInteger(W * 8), NS::UInteger(W * H * 8));
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    const auto* m = static_cast<const std::uint16_t*>(buf->contents());
    std::vector<std::uint16_t> out(m, m + std::size_t(W * H * 4));
    buf->release();
    return out;
}

}  // namespace

TEST_CASE("shatter: the GPU crack map agrees with the CPU fracture") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ShatterPipeline sp(device(), W, H);
    MTL::CommandQueue* q = device()->newCommandQueue();
    Target t(W, H);
    t.fill();
    for (int impacts : {1, 4}) {
        sh::Config c;
        c.impacts = impacts;
        auto p = std::make_unique<sh::Plan>();
        sh::makePlan(*p, 17 + impacts, c, double(W) / H, 62, 2, 2);
        auto f = paneFrame(*p);
        run(q, sp, t, *f, plainLook());
        MTL::Texture* map = sp.crackMap(p->key);
        REQUIRE(map != nullptr);
        const auto m = readMap(q, map, W, H);
        int checked = 0, agree = 0, elemAgree = 0, near = 0, inRange = 0;
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 2) {
                const std::uint16_t* c4 = &m[std::size_t((y * W + x) * 4)];
                inRange += c4[0] < sh::kSlots && c4[1] < sh::kElemIds ? 1 : 0;
                const sh::Hit h = sh::owner(p->pane, (float(x) + 0.5f) / H, (float(y) + 0.5f) / H);
                const float dpx = h.dist * float(H);
                if (dpx < 1.0f) continue;  // on a crack: either side
                ++checked;
                agree += c4[0] == h.slot ? 1 : 0;
                elemAgree += c4[1] == h.elem ? 1 : 0;
                near += std::fabs(float(c4[3]) / 32.0f - dpx) < 0.1f ? 1 : 0;
            }
        CAPTURE(impacts);
        CHECK(inRange == (W / 2) * (H / 2));
        CHECK(checked > (W / 2) * (H / 2) * 8 / 10);
        CHECK(agree > checked * 999 / 1000);
        CHECK(elemAgree > checked * 99 / 100);
        CHECK(near > checked * 99 / 100);
    }
    CHECK(sp.mapBuilds() == 2);
    q->release();
}

TEST_CASE("shatter: the intact pane leaves the frame alone; grown cracks show as lines; loose pieces shift the "
          "visuals") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ShatterPipeline sp(device(), W, H);
    MTL::CommandQueue* q = device()->newCommandQueue();
    Target t(W, H);
    auto p = std::make_unique<sh::Plan>();
    sh::makePlan(*p, 5, sh::Config{}, double(W) / H, 62, 2, 2);
    auto maxDiff = [&](const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        int d = 0;
        for (std::size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(int(a[i]) - int(b[i])));
        return d;
    };
    // Intact: exactly the frame.
    {
        t.fill();
        auto f = paneFrame(*p);
        run(q, sp, t, *f, plainLook());
        CHECK(maxDiff(t.read(), t.px) <= 1);
    }
    // Every crack grown: lines over dark (brighter) and over light (they show: lighter core, darker fringe), the
    // shards' insides unchanged.
    for (int mode : {1, 2}) {
        t.fill(mode);
        auto f = paneFrame(*p);
        for (int q2 = 0; q2 < p->elemCount; ++q2) f->reveal[std::size_t(p->elemList[std::size_t(q2)])] = 1.0f;
        run(q, sp, t, *f, plainLook());
        const auto out = t.read();
        int lines = 0, same = 0, far = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const sh::Hit h = sh::owner(p->pane, (float(x) + 0.5f) / H, (float(y) + 0.5f) / H);
                const float d = h.dist * float(H);
                const int diff = std::abs(t.at(out, x, y, 1) - t.at(t.px, x, y, 1));
                if (d < 0.4f) lines += diff > 25 ? 1 : 0;
                if (d > 8.0f) {
                    ++far;
                    same += diff <= 1 ? 1 : 0;
                }
            }
        CAPTURE(mode);
        CHECK(lines > 500);
        CHECK(same > far * 98 / 100);  // (the impact's crushed spot is not a crack's inside)
    }
    // Grown only halfway: the cracks show on one half of their length.
    {
        t.fill(1);
        auto f = paneFrame(*p);
        for (int q2 = 0; q2 < p->elemCount; ++q2) f->reveal[std::size_t(p->elemList[std::size_t(q2)])] = 0.5f;
        run(q, sp, t, *f, plainLook());
        const auto out = t.read();
        int early = 0, earlyLit = 0, late = 0, lateLit = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const sh::Hit h = sh::owner(p->pane, (float(x) + 0.5f) / H, (float(y) + 0.5f) / H);
                if (h.dist * float(H) > 0.4f) continue;
                const bool lit = t.at(out, x, y, 1) > t.at(t.px, x, y, 1) + 25;
                if (h.param < 0.4f) {
                    ++early;
                    earlyLit += lit;
                } else if (h.param > 0.6f) {
                    ++late;
                    lateLit += lit;
                }
            }
        CHECK(earlyLit > early * 8 / 10);
        CHECK(lateLit < late / 10);
    }
    // A piece cracked loose (a tilt on one shard): the visuals behind it shift; the other shards are unchanged.
    {
        t.fill();
        auto f = paneFrame(*p);
        // The biggest shard.
        int big = 0;
        for (int s = 1; s < p->shardCount; ++s)
            if (p->shards[std::size_t(s)].area > p->shards[std::size_t(big)].area) big = s;
        const sh::Shard& sd = p->shards[std::size_t(big)];
        f->tilt[std::size_t(2 * sd.slot)] = 0.08f;
        sh::Look look = plainLook();
        look.depth = 0.5f;
        run(q, sp, t, *f, look);
        const auto out = t.read();
        int inside = 0, moved = 0, outside = 0, kept = 0;
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 2) {
                const sh::Hit h = sh::owner(p->pane, (float(x) + 0.5f) / H, (float(y) + 0.5f) / H);
                if (h.dist * float(H) < 10.0f) continue;
                const int diff = std::abs(t.at(out, x, y, 2) - t.at(t.px, x, y, 2)) +
                                 std::abs(t.at(out, x, y, 0) - t.at(t.px, x, y, 0));
                if (h.slot == sd.slot) {
                    ++inside;
                    moved += diff >= 2 ? 1 : 0;
                } else {
                    ++outside;
                    kept += diff <= 1 ? 1 : 0;
                }
            }
        CHECK(inside > 50);
        CHECK(moved > inside * 6 / 10);
        CHECK(kept > outside * 99 / 100);
    }
    q->release();
}

TEST_CASE("shatter: a shard refracts the visuals; at the moment of breaking the shards show the pane") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ShatterPipeline sp(device(), W, H);
    MTL::CommandQueue* q = device()->newCommandQueue();
    Target t(W, H);
    auto p = std::make_unique<sh::Plan>();
    sh::makePlan(*p, 5, sh::Config{}, double(W) / H, 62, 2, 2);
    int big = 0;
    for (int s = 1; s < p->shardCount; ++s)
        if (p->shards[std::size_t(s)].area > p->shards[std::size_t(big)].area) big = s;
    const sh::Shard& sd = p->shards[std::size_t(big)];
    auto shardFrame = [&](float ay) {
        auto f = std::make_unique<sh::Frame>();
        f->fallen = p.get();
        f->loose = 1;
        sh::ShardPose& ps = f->poses[0];
        ps.shard = int16_t(big);
        ps.x = sd.cx;
        ps.y = sd.cy;
        ps.z = 0;
        const float c = std::cos(ay), s = std::sin(ay);
        ps.rot = {c, 0, s, 0, 1, 0, -s, 0, c};  // turned about the vertical axis
        ps.flash = 0;
        return f;
    };
    sh::Look look = plainLook();
    // Flat, at home: its inside shows the frame as it is; its edges show (the bevel, lit).
    t.fill();
    run(q, sp, t, *shardFrame(0.0f), look);
    auto out = t.read();
    int inside = 0, same = 0, edge = 0, edgeChanged = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const sh::Hit h = sh::owner(p->pane, (float(x) + 0.5f) / H, (float(y) + 0.5f) / H);
            if (h.slot != sd.slot) continue;
            const float d = h.dist * float(H);
            const int diff = std::abs(t.at(out, x, y, 1) - t.at(t.px, x, y, 1)) + std::abs(t.at(out, x, y, 2) - t.at(t.px, x, y, 2));
            if (d > 10.0f) {
                ++inside;
                same += diff <= 3 ? 1 : 0;
            } else if (d < 1.5f) {
                ++edge;
                edgeChanged += diff > 6 ? 1 : 0;
            }
        }
    CHECK(inside > 200);
    CHECK(same > inside * 97 / 100);
    CHECK(edgeChanged > edge / 2);
    // Turned .8 rad: the visuals through it shift sideways (the slab), so its inside no longer matches the frame
    // there, while the frame around it is untouched.
    t.fill();
    run(q, sp, t, *shardFrame(0.8f), look);
    out = t.read();
    int moved = 0, seen = 0, around = 0, kept = 0;
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 2) {
            const float hx = (float(x) + 0.5f) / H - sd.cx;
            const int diff = std::abs(t.at(out, x, y, 2) - t.at(t.px, x, y, 2)) + std::abs(t.at(out, x, y, 0) - t.at(t.px, x, y, 0));
            const sh::Hit h = sh::owner(p->pane, sd.cx + hx / std::cos(0.8f), (float(y) + 0.5f) / H);
            if (h.slot == sd.slot && h.dist * float(H) > 12.0f) {
                ++seen;
                moved += diff > 4 ? 1 : 0;
            }
            const float bx = (float(x) + 0.5f) / H, by = (float(y) + 0.5f) / H;
            if (bx < sd.cx + sd.x0 - 0.05f || bx > sd.cx + sd.x1 + 0.05f || by < sd.cy + sd.y0 - 0.05f ||
                by > sd.cy + sd.y1 + 0.05f) {
                ++around;
                kept += diff == 0 ? 1 : 0;
            }
        }
    CHECK(seen > 50);
    CHECK(moved > seen * 6 / 10);
    CHECK(kept == around);
    // The moment of breaking: every shard at home, flat: the frame shows through them as through the pane (only the
    // cuts between them show).
    {
        t.fill();
        auto f = std::make_unique<sh::Frame>();
        f->fallen = p.get();
        f->loose = p->shardCount;
        for (int s = 0; s < p->shardCount; ++s) {
            sh::ShardPose& ps = f->poses[std::size_t(s)];
            ps.shard = int16_t(s);
            ps.x = p->shards[std::size_t(s)].cx;
            ps.y = p->shards[std::size_t(s)].cy;
            ps.rot = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        }
        run(q, sp, t, *f, look);
        const auto whole = t.read();
        int far = 0, ok = 0;
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 2) {
                const sh::Hit h = sh::owner(p->pane, (float(x) + 0.5f) / H, (float(y) + 0.5f) / H);
                if (h.dist * float(H) < 12.0f || x < 12 || y < 12 || x > W - 13 || y > H - 13) continue;
                ++far;
                int d = 0;
                for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(t.at(whole, x, y, c) - t.at(t.px, x, y, c)));
                ok += d <= 3 ? 1 : 0;
            }
        CHECK(far > 5000);
        CHECK(ok > far * 98 / 100);  // no gaps (the background would not differ) and no doubled shards (they would)
    }
    q->release();
}

TEST_CASE("shatter: cost at 1920x1080 (the cracked pane, then the shards falling; one and five impacts); no "
          "render-thread allocations") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::ShatterPipeline sp(device(), W, H);
    MTL::CommandQueue* q = device()->newCommandQueue();
    Target t(W, H);
    t.fill();
    for (int impacts : {1, 5}) {
        CAPTURE(impacts);
        sh::Config c;
        c.cycle = 32;
        c.impacts = impacts;
        auto player = std::make_unique<sh::Player>(3, c, double(W) / H, 0.0);
        sh::Look look;
        // Warm up through a pane (the map is built as it arrives).
        double mapMs = 0;
        int builds = 0;
        auto clockAt = [](double b) {
            sh::Clock k;
            k.on = true;
            k.bar = b / 4.0;
            return k;
        };
        for (double b = 0; b < 20.0; b += 0.25) {
            const int before = sp.mapBuilds();
            double ms = 0;
            run(q, sp, t, player->step(0, clockAt(b), sh::Controls{}), look, &ms);
            if (sp.mapBuilds() > before) {
                mapMs = std::max(mapMs, ms);
                ++builds;
            }
        }
        CHECK(builds == 1);
        const std::uint64_t allocs0 = perf::renderAllocs().load();
        double paneSum = 0, paneMax = 0, fallSum = 0, fallMax = 0;
        int paneN = 0, fallN = 0, maxLoose = 0;
        for (int i = 0; i < 240; ++i) {
            NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
            const double b = 20.0 + i * 0.075;  // beats 20..38: the cracked pane, then the line and the fall
            const sh::Frame& f = player->step(0, clockAt(b), sh::Controls{});
            const int before = sp.mapBuilds();
            double ms = 0;
            run(q, sp, t, f, look, &ms);
            if (sp.mapBuilds() == before) {
                if (f.loose > 0) {
                    fallSum += ms;
                    fallMax = std::max(fallMax, ms);
                    ++fallN;
                    maxLoose = std::max(maxLoose, f.loose);
                } else if (f.pane) {
                    paneSum += ms;
                    paneMax = std::max(paneMax, ms);
                    ++paneN;
                }
            }
            pool->release();
        }
        REQUIRE(paneN > 20);
        REQUIRE(fallN > 5);
        const double pane = paneSum / paneN, fall = fallSum / fallN;
        MESSAGE("shatter at 1920x1080, " << impacts << " impact(s): the cracked pane " << pane << " ms GPU avg (max " << paneMax << "), shards falling "
                                                          << fall << " ms (max " << fallMax << ", " << maxLoose
                                                          << " shards), the crack map once per pane " << mapMs << " ms");
        PERF_BUDGET(mapMs < 12.0);
        PERF_BUDGET(pane < 1.5);
        PERF_BUDGET(fall < 2.0);
        CHECK(perf::renderAllocs().load() == allocs0);
    }
    q->release();
}
