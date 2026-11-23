#pragma once
// reforce extension (reforce.Supersample, Peter 2026-11-22): the frame rendered at `factor` x the output size is
// averaged down to the output size, factor x factor pixels per output pixel, in linear light (the BGRA8 frame holds
// sRGB-encoded color), alpha averaged too (the Syphon key). Colorizing before averaging is what makes the edges
// right with any ColorMap: the average of colors, not the color of an average intensity.

#include <cstdint>

namespace MTL { class Device; class Texture; class CommandBuffer; class RenderPipelineState; }

namespace reforce::render {

class Downsampler {
public:
    // Output textures (BGRA8, outW x outH) are made here: `slots` of them, one per distinct source per frame.
    Downsampler(MTL::Device* device, std::uint32_t outW, std::uint32_t outH, int factor, int slots = 2);
    ~Downsampler();
    Downsampler(const Downsampler&) = delete;
    Downsampler& operator=(const Downsampler&) = delete;

    int factor() const { return factor_; }
    // Averages `src` (outW x factor by outH x factor) into slot `slot`'s texture and returns it.
    MTL::Texture* encode(MTL::CommandBuffer* cmd, MTL::Texture* src, int slot);

private:
    int factor_;
    MTL::RenderPipelineState* pso_ = nullptr;
    MTL::Texture* out_[4] = {};
    int slots_ = 0;
};

}  // namespace reforce::render
