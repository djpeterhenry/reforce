#pragma once
// GPU organics (PLAN 5O.6, engine::OrganicField): Julia and Mandelbrot sets, each drawn into its own RG8 target and
// composited by the sprite pass as an ordinary SpriteQuad, exactly like a 3D solid (render/SolidPipeline): the
// target holds premultiplied (luminance x alpha, alpha) and is registered with ScenePipeline::addSolidTexture.
//
//   - Startup (constructor, no render-thread allocations): `maxLive` slots, each a full-size RG8 target plus the
//     Mandelbrot reference-orbit buffers.
//   - Julia: one pass, z -> z^2 + c from each pixel's point (tree units through X / Y / Scal / Rot, the set's +-1.6
//     filling the frame height at Scal 1), a smooth escape count mu (n + 1 - log2(log |z|)), luminance
//     (mu / Iter)^0.4 x Gain, optionally banded by cos(2 pi Band mu / Iter + Phas); the set itself is dark.
//   - Mandel: one pass, c = the view center (double) + the pixel's offset (float); z -> z^2 + c in one of
//     (engine::OrganicField::precision): float32 (shallow: clean to ~2^12); double-float (float2 hi + lo, ~48 bits;
//     its own library compiled without fast math; ~5x float's cost); perturbation (the default when deep: the view
//     center's orbit in double on the CPU, each pixel iterating only its float difference from it, with
//     rebasing: about float's cost, clean as far as the double center carries, ~2^40). Shading from the distance
//     estimate in pixels (the same at every depth) and bands of the escape count (Band iterations wide, faded
//     where they would alias); the set is dark. `res` < 1 renders only that corner of the target (the quad samples
//     it), for a quarter of the cost at .5.

#include <array>
#include <cstdint>
#include <vector>

#include "engine/Organics.hpp"
#include "render/SolidPipeline.hpp"  // SolidQuad

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState; class Buffer;
}

namespace reforce::render {

class ScenePipeline;

struct FieldPipelineOptions {
    int maxLive = 3;        // slots (reforce.Object.MaxLive + 1)
};

class FieldPipeline {
public:
    // Registers the slot targets as sprite textures of `scene`, which must outlive this.
    FieldPipeline(ScenePipeline& scene, const FieldPipelineOptions& opts);
    ~FieldPipeline();
    FieldPipeline(const FieldPipeline&) = delete;
    FieldPipeline& operator=(const FieldPipeline&) = delete;

    int maxLive() const { return int(slots_.size()); }
    static constexpr int kMaxRef = 20000;  // Mandel: the longest reference orbit (= the iteration cap)

    // Draws `f` into slot's target on `cmd`; encode before the ScenePipeline step that composites the returned quad
    // (the whole frame).
    SolidQuad encode(MTL::CommandBuffer* cmd, int slot, const engine::OrganicField& f);
    MTL::Texture* target(int slot) const { return slots_[std::size_t(slot)].tex; }

private:
    ScenePipeline& scene_;
    MTL::Device* device_;
    uint32_t width_, height_;
    MTL::RenderPipelineState* juliaPso_ = nullptr;
    MTL::RenderPipelineState* mandelPso_ = nullptr;
    struct Slot {
        MTL::Texture* tex = nullptr;  // RG8 target
        int handle = -1;              // sprite texture
        MTL::Buffer* ref[3] = {nullptr, nullptr, nullptr};  // Mandel reference orbits, a ring (frames in flight <= 2)
        int refNext = 0;
    };
    std::vector<Slot> slots_;
};

}  // namespace reforce::render
