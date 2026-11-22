#include "engine/FlowFieldBuilder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace reforce::engine {
namespace {

std::uint64_t rowSeed(std::uint64_t seed, int row) {
    std::uint64_t z = seed * 0x9e3779b97f4a7c15ULL + std::uint64_t(row) * 0xbf58476d1ce4e5b9ULL + 0x632be59bd9b4e019ULL;
    z = (z ^ (z >> 31)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 29);
}

}  // namespace

FlowFieldGrid buildFlowField(const scene::CompiledConfig& cfg, const scene::Environment& envIn,
                             const FlowFieldBuildOptions& opts) {
    const auto t0 = std::chrono::steady_clock::now();
    FlowFieldGrid g;
    g.width = std::max(opts.width, 1);
    g.height = std::max(opts.height, 1);
    scene::Environment env = envIn;
    env.width = g.width;
    env.height = g.height;
    g.srcXY.assign(std::size_t(g.width) * std::size_t(g.height) * 2, 0.0f);

    int threads = opts.threads > 0 ? opts.threads : int(std::thread::hardware_concurrency());
    threads = std::clamp(threads, 1, std::max(1, g.height));

    std::atomic<int> nextRow{0};
    std::vector<std::thread> pool;
    auto worker = [&](bool reportExtents) {
        scene::FlowFieldInstance inst(cfg, env);
        if (reportExtents) {
            g.xExtent = inst.xExtent();
            g.yExtent = inst.yExtent();
            g.polar = inst.polar();
        }
        const auto xs = inst.rowXs(g.width);
        const double ix = 1.0 / inst.xExtent(), iy = 1.0 / inst.yExtent();
        std::vector<double> sx(std::size_t(g.width)), sy(std::size_t(g.width));
        for (;;) {
            if (opts.cancel && opts.cancel->load(std::memory_order_relaxed)) return;
            const int r = nextRow.fetch_add(1);
            if (r >= g.height) return;
            inst.context().rng.reseed(rowSeed(env.seed, r));
            inst.evaluateRow(inst.rowY(r, g.height), xs, sx.data(), sy.data());
            float* dst = g.srcXY.data() + std::size_t(r) * std::size_t(g.width) * 2;
            for (int c = 0; c < g.width; ++c) {
                const double u = sx[std::size_t(c)] * ix, v = sy[std::size_t(c)] * iy;
                const bool ok = std::isfinite(u) && std::isfinite(v) && std::abs(u) < 1e6 && std::abs(v) < 1e6;
                dst[2 * c] = ok ? float(u) : kOutOfFrame;
                dst[2 * c + 1] = ok ? float(v) : kOutOfFrame;
            }
        }
    };
    for (int i = 1; i < threads; ++i) pool.emplace_back(worker, false);
    worker(true);
    for (auto& th : pool) th.join();
    g.buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return g;
}

std::future<FlowFieldGrid> buildFlowFieldAsync(std::shared_ptr<const scene::CompiledConfig> cfg,
                                               scene::Environment env, FlowFieldBuildOptions opts) {
    return std::async(std::launch::async, [cfg = std::move(cfg), env, opts]() {
        return buildFlowField(*cfg, env, opts);
    });
}

UnionMask buildUnionMask(const scene::CompiledConfig& cfg, const scene::Environment& envIn,
                         const FlowFieldBuildOptions& opts) {
    const auto t0 = std::chrono::steady_clock::now();
    UnionMask m;
    m.width = std::max(opts.width, 1);
    m.height = std::max(opts.height, 1);
    scene::Environment env = envIn;
    env.width = m.width;
    env.height = m.height;
    m.alpha.assign(std::size_t(m.width) * std::size_t(m.height), 0.0f);
    int threads = opts.threads > 0 ? opts.threads : int(std::thread::hardware_concurrency());
    threads = std::clamp(threads, 1, std::max(1, m.height));
    std::atomic<int> nextRow{0};
    auto worker = [&]() {
        scene::FlowFieldInstance inst(cfg, env);
        const auto xs = inst.rowXs(m.width);
        std::vector<double> a(std::size_t(m.width));
        for (;;) {
            if (opts.cancel && opts.cancel->load(std::memory_order_relaxed)) return;
            const int r = nextRow.fetch_add(1);
            if (r >= m.height) return;
            inst.context().rng.reseed(rowSeed(env.seed, r));
            inst.evaluateAlphaRow(inst.rowY(r, m.height), xs, a.data());
            float* dst = m.alpha.data() + std::size_t(r) * std::size_t(m.width);
            for (int c = 0; c < m.width; ++c) {
                const double v = a[std::size_t(c)];
                dst[c] = std::isfinite(v) ? float(std::clamp(v, -1e6, 1e6)) : 0.f;
            }
        }
    };
    std::vector<std::thread> pool;
    for (int i = 1; i < threads; ++i) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();
    m.buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return m;
}

std::future<UnionMask> buildUnionMaskAsync(std::shared_ptr<const scene::CompiledConfig> cfg, scene::Environment env,
                                           FlowFieldBuildOptions opts) {
    return std::async(std::launch::async, [cfg = std::move(cfg), env, opts]() {
        return buildUnionMask(*cfg, env, opts);
    });
}

FlowFieldGrid identityFlowField(int width, int height) {
    FlowFieldGrid g;
    g.width = std::max(width, 1);
    g.height = std::max(height, 1);
    g.srcXY.resize(std::size_t(g.width) * std::size_t(g.height) * 2);
    for (int r = 0; r < g.height; ++r)
        for (int c = 0; c < g.width; ++c) {
            float* d = g.srcXY.data() + (std::size_t(r) * std::size_t(g.width) + std::size_t(c)) * 2;
            // Same grid as FlowFieldInstance::rowXs/rowY (edge pixels on the frame edges).
            d[0] = g.width > 1 ? float(-1.0 + 2.0 * c / (g.width - 1.0)) : 0.f;
            d[1] = g.height > 1 ? float(1.0 - 2.0 * r / (g.height - 1.0)) : 0.f;
        }
    return g;
}

}  // namespace reforce::engine
