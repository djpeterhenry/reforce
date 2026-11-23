#pragma once
// Prefiltered minification for video frames (Phase 5F polish).
//
// Decoder / camera / Syphon frames are single-level textures, so a 4K movie
// drawn as a small sprite (or a 4K color layer at 1080p) aliases and
// shimmers under plain bilinear sampling. For a frame drawn at less than
// kMinScale of its size, prepare() returns a private mipmapped texture: level
// 0 is the frame box-filtered to half size (one render pass, 2x2 box exactly
// via one bilinear tap per texel), levels 1+ come from the blit encoder's
// generateMipmaps. The sprite and color-layer samplers use mip_filter::linear,
// so the GPU picks the level. Frames drawn at >= kMinScale are returned as is
// (bilinear is fine there; nothing is encoded).
//
// Work is done once per new frame per slot (a slot = one consumer: a video
// sprite, a color-layer source), so a 30 fps movie on a 60 Hz sim or a 120 Hz
// display costs one pass per decoded frame. Textures are pooled per size; a
// new size allocates once (counted as a render-thread allocation). Render
// thread only.

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace MTL {
class CommandBuffer;
class Device;
class RenderPipelineState;
class Texture;
}  // namespace MTL

namespace reforce::render {

class VideoMips {
public:
    static constexpr double kMinScale = 0.6;  // drawn size / frame size below which mips are used

    explicit VideoMips(MTL::Device* device);
    ~VideoMips();
    VideoMips(const VideoMips&) = delete;
    VideoMips& operator=(const VideoMips&) = delete;

    // The texture to sample for `frame` drawn at `scale` (drawn pixels per
    // frame pixel, the more minified axis). `id` identifies the frame's
    // pixels (its owner, e.g. the video::FramePtr): the same id is not
    // filtered twice. `slot` is any key unique per consumer (e.g. the source
    // pointer). Encodes into `cmd` before the pass that samples the result.
    MTL::Texture* prepare(MTL::CommandBuffer* cmd, const void* slot, MTL::Texture* frame,
                          const std::shared_ptr<const void>& id, double scale);
    // Call once per simulation step: slots not used for a while go back to the pool.
    void collect();

    std::uint64_t builds() const { return builds_; }  // filter passes encoded so far
    static int levelsFor(int w, int h);               // full mip chain length for w x h
    static int halfSize(int n) { return n > 1 ? (n + 1) / 2 : 1; }

private:
    struct Slot {
        MTL::Texture* tex = nullptr;  // owned
        std::weak_ptr<const void> id; // frame it holds
        bool valid = false;
        std::uint64_t used = 0;
    };
    MTL::Texture* acquire(int w, int h);
    void recycle(MTL::Texture* t);

    MTL::Device* device_;
    MTL::RenderPipelineState* pso_ = nullptr;
    std::map<const void*, Slot> slots_;
    std::vector<MTL::Texture*> free_;
    std::uint64_t tick_ = 0, builds_ = 0;
};

}  // namespace reforce::render
