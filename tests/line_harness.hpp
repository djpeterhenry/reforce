#pragma once
// Offscreen harness around render::ScenePipeline for line-pass tests and the
// line lab tool (tools/line_lab): synthetic segments, a synthetic flow field,
// one step per call, and a readback of the intensity buffer in levels (0..255).
// Header-only; links reforce_render.

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "engine/FlowFieldBuilder.hpp"
#include "engine/LineGeometry.hpp"
#include "render/ScenePipeline.hpp"

namespace reforce::linetest {

enum class Field { Void, Identity, ZoomRotate };

// Source coordinates for a field (normalized frame coords, +y up).
// Void: every source out of frame (the buffer clears to 0 each step).
// ZoomRotate: content moves outward by `zoom` per frame and turns by `turn` rad (aspect-correct).
inline engine::FlowFieldGrid makeField(int w, int h, Field kind, double zoom = 1.004, double turn = 0.003) {
    engine::FlowFieldGrid g;
    g.width = w;
    g.height = h;
    g.srcXY.resize(std::size_t(w) * h * 2);
    const double c = std::cos(turn), s = std::sin(turn), aspect = double(w) / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double sx = (x + 0.5) / w * 2.0 - 1.0, sy = 1.0 - (y + 0.5) / h * 2.0;
            if (kind == Field::Void) sx = sy = 1e9;
            else if (kind == Field::ZoomRotate) {
                const double px = sx * aspect, py = sy;
                const double rx = (c * px + s * py) / zoom, ry = (-s * px + c * py) / zoom;
                sx = rx / aspect;
                sy = ry;
            }
            g.srcXY[(std::size_t(y) * w + x) * 2] = float(sx);
            g.srcXY[(std::size_t(y) * w + x) * 2 + 1] = float(sy);
        }
    return g;
}

class Harness {
public:
    Harness(MTL::Device* device, int w, int h, render::ScenePipelineOptions opts, Field field)
        : device_(device), w_(w), h_(h), half_(opts.format == render::IntensityFormat::Half16) {
        queue_ = device_->newCommandQueue();
        pipe_ = std::make_unique<render::ScenePipeline>(device_, uint32_t(w), uint32_t(h), opts);
        pipe_->setField(makeField(w, h, field));
        MTL::CommandBuffer* cmd = queue_->commandBuffer();
        pipe_->encodeClear(cmd);
        cmd->commit();
        cmd->waitUntilCompleted();
        readBuf_ = device_->newBuffer(NS::UInteger(std::size_t(w) * h * 2), MTL::ResourceStorageModeShared);
    }
    ~Harness() {
        pipe_.reset();
        readBuf_->release();
        queue_->release();
    }
    render::ScenePipeline& pipeline() { return *pipe_; }
    MTL::CommandQueue* queue() { return queue_; }

    // `steps` simulation steps with the same segments in one command buffer; returns its GPU time (ms).
    double step(const std::vector<engine::LineSegment>& segs, int steps = 1) {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        MTL::CommandBuffer* cmd = queue_->commandBuffer();
        for (int i = 0; i < steps; ++i) pipe_->encodeStep(cmd, segs.data(), segs.size(), render::FlowBlend{});
        cmd->commit();
        cmd->waitUntilCompleted();
        if (cmd->status() == MTL::CommandBufferStatusError) throw std::runtime_error("GPU error");
        const double ms = (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
        pool->release();
        return ms;
    }

    // Intensity in levels (0..255), row-major, rows top to bottom.
    std::vector<float> read() {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        MTL::CommandBuffer* cmd = queue_->commandBuffer();
        half_ = pipe_->format() == render::IntensityFormat::Half16;  // may change live (Look)
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        const std::size_t bpp = half_ ? 2 : 1;
        blit->copyFromTexture(pipe_->currentIntensity(), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w_, h_, 1), readBuf_, 0,
                              NS::UInteger(w_ * bpp), NS::UInteger(std::size_t(w_) * h_ * bpp));
        blit->endEncoding();
        cmd->commit();
        cmd->waitUntilCompleted();
        std::vector<float> out(std::size_t(w_) * h_);
        if (half_) {
            const __fp16* p = static_cast<const __fp16*>(readBuf_->contents());
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = float(p[i]) * 255.0f;
        } else {
            const std::uint8_t* p = static_cast<const std::uint8_t*>(readBuf_->contents());
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = float(p[i]);
        }
        pool->release();
        return out;
    }

private:
    MTL::Device* device_;
    int w_, h_;
    bool half_;
    MTL::CommandQueue* queue_ = nullptr;
    std::unique_ptr<render::ScenePipeline> pipe_;
    MTL::Buffer* readBuf_ = nullptr;
};

// One polyline path (consecutive segments share the path id).
inline void addPolyline(std::vector<engine::LineSegment>& out, const std::vector<float>& xy, float pen, float radius) {
    const float id = float(out.size());
    for (std::size_t i = 2; i + 1 < xy.size(); i += 2)
        out.push_back({xy[i - 2], xy[i - 1], xy[i], xy[i + 1], pen, pen, radius, id});
}

inline void addDot(std::vector<engine::LineSegment>& out, float x, float y, float pen, float radius) {
    out.push_back({x, y, x, y, pen, pen, radius, float(out.size())});
}

}  // namespace reforce::linetest
