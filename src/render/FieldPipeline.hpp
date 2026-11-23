#pragma once
// GPU organics (PLAN 5O.6, engine::OrganicField): ripples, Julia and Mandelbrot sets, each drawn into its own RG8 target and
// composited by the sprite pass as an ordinary SpriteQuad, exactly like a 3D solid (render/SolidPipeline): the
// target holds premultiplied (luminance x alpha, alpha) and is registered with ScenePipeline::addSolidTexture.
//
//   - Startup (constructor, no render-thread allocations): `maxLive` slots, each a full-size RG8 target plus three
//     R32Float height fields (rippleRows rows, the frame's aspect) for the wave equation.
//   - Ripples: per step, n substeps of the wave equation (a render pass each, ping-pong over the three fields)
//       h' = (2 h - h_prev + c^2 lap(h)) * damp + drops
//     with c = speed * rows * dt / n <= 0.5 cells per substep (stable), damp = exp(-dt / n / decay), the drops
//     (Gaussian bumps) added in the first substep, and the edges either reflecting (clamped reads) or absorbing (a
//     sponge a few cells wide). Then the show pass draws the crests (and a little of the slope) into the target.
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
    int rippleRows = 270;   // reforce.Organic.RippleRows: rows of the ripple grid (clamped to 32..render height)
};

class FieldPipeline {
public:
    // Registers the slot targets as sprite textures of `scene`, which must outlive this.
    FieldPipeline(ScenePipeline& scene, const FieldPipelineOptions& opts);
    ~FieldPipeline();
    FieldPipeline(const FieldPipeline&) = delete;
    FieldPipeline& operator=(const FieldPipeline&) = delete;

    int maxLive() const { return int(slots_.size()); }
    int rippleCols() const { return cols_; }
    int rippleRows() const { return rows_; }
    // Substeps a ripple step of `dt` seconds at `speed` frame heights per second takes (1..kMaxSubsteps).
    int substeps(double speed, double dt) const;
    static constexpr int kMaxSubsteps = 16;
    static constexpr int kMaxDrops = 16;
    static constexpr int kMaxRef = 20000;  // Mandel: the longest reference orbit (= the iteration cap)

    // Simulates (ripples) and draws `f` into slot's target on `cmd`; encode before the ScenePipeline step that
    // composites the returned quad (the whole frame). `f.reset` clears the ripple fields first.
    SolidQuad encode(MTL::CommandBuffer* cmd, int slot, const engine::OrganicField& f);
    MTL::Texture* target(int slot) const { return slots_[std::size_t(slot)].tex; }
    // The slot's latest height field (tests read it back): R32Float, rippleCols x rippleRows.
    MTL::Texture* height(int slot) const;

private:
    void clearFields(MTL::CommandBuffer* cmd, int slot);
    ScenePipeline& scene_;
    MTL::Device* device_;
    uint32_t width_, height_;
    int cols_ = 0, rows_ = 0;
    MTL::RenderPipelineState* stepPso_ = nullptr;
    MTL::RenderPipelineState* showPso_ = nullptr;
    MTL::RenderPipelineState* juliaPso_ = nullptr;
    MTL::RenderPipelineState* mandelPso_ = nullptr;
    struct Slot {
        MTL::Texture* tex = nullptr;  // RG8 target
        int handle = -1;              // sprite texture
        MTL::Texture* h[3] = {nullptr, nullptr, nullptr};
        int cur = 1;                  // h[cur] = now, h[(cur + 2) % 3] = the step before
        std::vector<std::array<float, 4>> fresh, undo;  // drops (cells: x, y, radius, amplitude) to take back
        MTL::Buffer* ref[3] = {nullptr, nullptr, nullptr};  // Mandel reference orbits, a ring (frames in flight <= 2)
        int refNext = 0;
    };
    std::vector<Slot> slots_;
};

}  // namespace reforce::render
