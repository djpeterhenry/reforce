#pragma once
// reforce extension (docs/3d-design.md "Shatter", Peter 2026-11-26): the glass pane over the visuals that cracks on
// the kicks and shatters on the phrase line (Mesh="shatter"; the plan and the shards' flight are engine/Shatter).
// Like the rain and the glass puzzle it is laid over the colorized frame (Visualizer::colorize, after the rain), not
// drawn into the visuals, so they keep running behind the glass.
//
// encode(), per step with a pane or shards in flight:
//   1. The frame is copied with mips (the refraction samples it by the ray's derivatives, so the image in a turned
//      shard does not shimmer).
//   2. The crack map (only when a plan is new; two kept, the pane's and the falling shards'): per pixel the shard
//      (slot), the nearest crack element, the position along it and the distance to it (RGBA16Uint, 1/32 px), from
//      engine/ShatterShape.inc, the code the engine and its tests run.
//   3. The pane (full screen): each pixel looks through the glass. A crack that has grown past this point is a fine
//      silvery line (light scattered in the fracture face) with a darker fringe, its chipped edges lean into it (a
//      narrow lens: the visuals bend and split into colors along it, Snell with n 1.50 / 1.52 / 1.545), glints
//      twinkle along it, and it flashes as it lands; the pieces it cut loose tilt a hair, so the visuals shift a
//      little across it (Dpth); the crushed spot of each impact and its flash; the faint sheen of the intact glass
//      (Gls), and the pane's thick bottom edge while it slides in.
//   4. The shards in flight, back to front: each a quad in 3D (the camera 2.2 frame heights above the pane, so a
//      shard thrown at it grows) whose pixels look up their home in the crack map (coverage, anti-aliased, and the
//      distance to the edge) and show what lies behind them now: the visuals shifted through the thick slab, bent
//      and split at the beveled edges, a Fresnel reflection that flashes as the shard turns edge-on, a highlight
//      from the upper left, the green of the glass at its edges, the shatter's flash.
// Resources at startup (no render-thread allocations): the frame copy (BGRA8 + mips), two crack maps (RGBA16Uint,
// frame size), eight shared buffers for the per-frame state (crack and tilt arrays, the shard instances).

#include <cstdint>

#include "engine/Shatter.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState; class Buffer;
}

namespace reforce::render {

class ShatterPipeline {
public:
    ShatterPipeline(MTL::Device* device, uint32_t width, uint32_t height);
    ~ShatterPipeline();
    ShatterPipeline(const ShatterPipeline&) = delete;
    ShatterPipeline& operator=(const ShatterPipeline&) = delete;

    // `output`: BGRA8, the render size, readable (the colorized frame). Nothing is encoded for an empty frame.
    // `trackAlpha`: the Syphon alpha is tracked (look.opaque raises it under cracks and shards).
    void encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const engine::shatter::Frame& f,
                const engine::shatter::Look& look, bool trackAlpha);

    MTL::Texture* crackMap(uint64_t planKey) const;  // tests: the map of a plan (null if not resident)
    int mapBuilds() const { return mapBuilds_; }

private:
    int mapFor(MTL::CommandBuffer* cmd, const engine::shatter::Plan& p, uint64_t keepKey);

    MTL::Device* device_;
    uint32_t width_, height_;
    MTL::Texture* copy_ = nullptr;
    MTL::Texture* map_[2] = {nullptr, nullptr};
    uint64_t mapKey_[2] = {0, 0};
    uint64_t mapUse_[2] = {0, 0};
    uint64_t uses_ = 0;
    int mapBuilds_ = 0;
    MTL::Buffer* ring_[8] = {};
    int ringNext_ = 0;
    MTL::RenderPipelineState* mapPso_ = nullptr;
    MTL::RenderPipelineState* panePso_[2] = {nullptr, nullptr};   // [alpha written]
    MTL::RenderPipelineState* shardPso_[2] = {nullptr, nullptr};
};

}  // namespace reforce::render
