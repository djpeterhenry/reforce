#include "app/Offline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "app/AudioDriver.hpp"
#include "app/Director.hpp"
#include "app/Visualizer.hpp"
#include "render/ImageFiles.hpp"

namespace reforce::app {

int runOffline(MTL::Device* device, unsigned width, unsigned height, const OfflineOptions& opts, Director& director,
               AudioDriver* audio) {
    Visualizer& vis = director.visualizer();
    if (!opts.outDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opts.outDir, ec);
        if (ec) {
            std::fprintf(stderr, "[reforce] cannot create %s: %s\n", opts.outDir.c_str(), ec.message().c_str());
            return 1;
        }
    }
    MTL::CommandQueue* queue = device->newCommandQueue();
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, width,
                                                                             height, false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(device->hasUnifiedMemory() ? MTL::StorageModeShared : MTL::StorageModeManaged);
    MTL::Texture* target = device->newTexture(td);

    vis.waitForField();  // deterministic: frame 0 already flows

    std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4);
    double cpuSum = 0, gpuSum = 0, gpuMax = 0, cpuMax = 0;
    double waveSum = 0, geomSum = 0, colorSum = 0;
    std::size_t segMax = 0;
    std::vector<double> cpuFrames, waveFrames;  // per measured frame (percentiles)
    double waveMax = 0;
    int slowFrames = 0;
    int written = 0;
    const int warmup = std::min(5, opts.frames / 4);
    int measured = 0;
    for (int f = 0; f < opts.frames; ++f) {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const double t = double(f) / opts.fps;
        for (const auto& [at, c] : opts.commands)
            if (at <= t + 1e-9 && at > t - 1.0 / opts.fps + 1e-9) director.commands().push(c);
        const auto c0 = std::chrono::steady_clock::now();
        const audio::AudioFrame* af = audio ? &audio->step(opts.fps) : nullptr;
        MTL::CommandBuffer* cmd = queue->commandBuffer();
        if (f == 0 && opts.seedNoise) vis.seedNoise(cmd);
        director.step(cmd, t, af);
        director.colorize(cmd, target);
        const bool save = !opts.outDir.empty() &&
                          (opts.saveFrames.count(f) || (opts.saveEvery > 0 && (f + 1) % opts.saveEvery == 0) ||
                           (opts.saveFrames.empty() && opts.saveEvery <= 0 && f == opts.frames - 1));
        if (save && !device->hasUnifiedMemory()) {
            MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
            blit->synchronizeResource(target);
            blit->endEncoding();
        }
        const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        cmd->commit();
        cmd->waitUntilCompleted();
        if (cmd->status() == MTL::CommandBufferStatusError) {
            std::fprintf(stderr, "[reforce] GPU error at frame %d: %s\n", f,
                         cmd->error() ? cmd->error()->localizedDescription()->utf8String() : "?");
            pool->release();
            return 1;
        }
        const double gpuMs = (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
        if (f >= warmup) {
            const auto& tm = vis.lastTimings();
            cpuSum += cpuMs;
            gpuSum += gpuMs;
            cpuMax = std::max(cpuMax, cpuMs);
            gpuMax = std::max(gpuMax, gpuMs);
            waveSum += tm.waveEvalMs;
            waveMax = std::max(waveMax, tm.waveEvalMs + tm.geometryMs);
            cpuFrames.push_back(cpuMs);
            waveFrames.push_back(tm.waveEvalMs + tm.geometryMs);
            // Frames over 1.5 x the 60 Hz budget (Phase 5D): which part was slow.
            if (cpuMs > 25.0 && ++slowFrames <= 20)
                std::fprintf(stderr, "[reforce] slow frame %d (t=%.2f): cpu %.2f ms (wave %.2f, geometry %.2f)\n", f,
                             t, cpuMs, tm.waveEvalMs, tm.geometryMs);
            geomSum += tm.geometryMs;
            colorSum += tm.colorMs;
            segMax = std::max(segMax, tm.segments);
            ++measured;
        }
        if (save) {
            target->getBytes(pixels.data(), NS::UInteger(width) * 4, MTL::Region(0, 0, width, height), 0);
            char name[64];
            std::snprintf(name, sizeof name, "%s_%05d.png", opts.prefix.c_str(), f);
            const std::string path = (std::filesystem::path(opts.outDir) / name).string();
            if (!render::writePngBgra8(path, pixels.data(), int(width), int(height), std::size_t(width) * 4)) {
                std::fprintf(stderr, "[reforce] failed to write %s\n", path.c_str());
                pool->release();
                return 1;
            }
            ++written;
        }
        pool->release();
    }
    if (measured > 0) {
        std::fprintf(stderr,
                     "[reforce] %d frames at %ux%u: cpu %.2f ms avg (max %.2f; wave %.2f, geometry %.2f, color %.2f), "
                     "gpu %.2f ms avg (max %.2f), max %zu segments; %d PNG(s) written\n",
                     opts.frames, width, height, cpuSum / measured, cpuMax, waveSum / measured, geomSum / measured,
                     colorSum / measured, gpuSum / measured, gpuMax, segMax, written);
        auto p99 = [](std::vector<double>& v) {
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, std::size_t(double(v.size()) * 0.99))];
        };
        std::fprintf(stderr, "[reforce] cpu p99 %.2f ms; WaveShape eval+geometry p99 %.2f ms, max %.2f ms\n",
                     p99(cpuFrames), p99(waveFrames), waveMax);
    }
    target->release();
    queue->release();
    return 0;
}

}  // namespace reforce::app
