#pragma once
// Owns the fixed-size, IOSurface-backed offscreen frame and the pipelines that
// draw into it and present it (aspect-fit) to a window drawable.

#include <cstdint>

#include <IOSurface/IOSurfaceRef.h>

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState;
}
namespace CA { class MetalDrawable; }

namespace reforce::render {

class Renderer {
public:
    Renderer(MTL::Device* device, uint32_t width, uint32_t height);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // BGRA8Unorm, top-left origin, backed by ioSurface().
    MTL::Texture* frameTexture() const { return frame_; }
    IOSurfaceRef ioSurface() const { return surface_; }

    // Placeholder content until the scene engine exists.
    void encodeTestPattern(MTL::CommandBuffer* cmd, float timeSeconds);

    // Scales the frame (or `source`, a texture of the frame's size) into the
    // drawable, letterboxed on black. Thread-safe (no mutable state).
    void encodePresent(MTL::CommandBuffer* cmd, CA::MetalDrawable* drawable, MTL::Texture* source = nullptr) const;

private:
    MTL::Device* device_;
    uint32_t width_, height_;
    IOSurfaceRef surface_ = nullptr;
    MTL::Texture* frame_ = nullptr;
    MTL::RenderPipelineState* patternPipeline_ = nullptr;
    MTL::RenderPipelineState* presentPipeline_ = nullptr;
};

}  // namespace reforce::render
