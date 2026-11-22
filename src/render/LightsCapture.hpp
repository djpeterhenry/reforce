#pragma once
// Room lights (docs/lights.md): a small copy of the visuals read back to the CPU without stalling the render thread.
// The frame is copied into a mipmapped texture, its mips are generated (box-filtered averages), and one level about
// 100-160 pixels wide is copied into one of three shared buffers; when the command buffer completes, `sink` gets it
// (on a Metal thread). A buffer still being read is skipped. All allocation happens in the constructor.
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace MTL { class Device; class Texture; class CommandBuffer; class Buffer; }

namespace reforce::render {

class LightsCapture {
public:
    using Sink = std::function<void(const std::uint8_t* bgra, int w, int h, int rowBytes)>;

    LightsCapture(MTL::Device* device, std::uint32_t width, std::uint32_t height, Sink sink);
    ~LightsCapture();
    LightsCapture(const LightsCapture&) = delete;
    LightsCapture& operator=(const LightsCapture&) = delete;

    bool valid() const;
    int gridWidth() const;
    int gridHeight() const;
    // `frame`: BGRA8, the size given to the constructor. False when skipped (all buffers busy).
    bool encode(MTL::CommandBuffer* cmd, MTL::Texture* frame);

private:
    struct Shared;
    std::shared_ptr<Shared> s_;  // shared with the completion handlers
};

}  // namespace reforce::render
