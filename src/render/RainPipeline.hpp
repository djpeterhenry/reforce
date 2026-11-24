#pragma once
// reforce extension (docs/3d-design.md "Rain on glass", Peter 2026-11-23): the GPU side of the rain object
// (Mesh="rain"; the simulation is engine/Rain). Like the glass 15 puzzle it is not drawn into the visuals: it is laid
// over the colorized frame (Visualizer::colorize, after the glass), so the visuals keep running under the water.
//
// Water is a height field over the frame, and each pixel looks through it:
//   - drops (window beads, runs, residuals; floor beads and crown droplets): splatted per colorize as spherical caps
//     (contact angle Bead) into a full-size R16F height (r) with a halo (g) just outside each contact line, by max
//     blending (merges happen on the CPU on contact, so overlaps are brief); runs are stretched along their motion
//     with a tail, merges wobble, fresh drops squash and recoil;
//   - floor ripples: the wave equation h' = (2 h - h_prev + c2 L h - b L L h + nu L (h - h_prev)) damp on a sim-size
//     R32F grid (three textures in a ring), stepped per sim step with the step's impulses (engine::rain::waveStep:
//     substeps, capillary dispersion b, viscosity nu; the dry glass between puddles damps it, the frame edges absorb);
//     the puddles' meniscus (engine::rain::puddle, the same noise here) adds its own slope;
//   - window fog: condensation (RG32F sim-size ping-pong: r fog 0..1, g wet film) easing back towards Fog over Refg
//     seconds, wiped clear by each running drop's capsule this step and left with a wet film that fades.
// The optics (rain_composite_fs): the surface normal N from the total slope; the view ray refracts into the water
// (Snell, n = 1.33) and out through the flat glass into air, then travels Dpth frame heights to the visuals: the
// sample offset is that ray's lateral run. A bead is a thick lens: the visuals behind it show minified and upside
// down, as on a real window. Rays that run off the frame see the dark room; a Fresnel reflection of a dim
// environment and a sharp + broad highlight from a soft light at the upper left; a dark contact line and halo
// (Rim) so drops read on any background; the fog shows the frosted half-size copy (SolidPipeline::encodeFrost)
// where no water is. Minified samples come from a mipmapped copy of the frame (trilinear, by the sample's
// screen-space derivatives), so the image in a small drop does not shimmer.
//
// Resources at startup (no render-thread allocations): the frame copy (BGRA8, full size, mips), the drop height
// (RG16F, full size), the wave ring (3 x R32F) and the fog pair (2 x RG32F) at the sim size (half the render size,
// at most 540 rows). They are cleared on the rain's first step (reset), so nothing reads undefined memory.

#include <cstdint>

#include "engine/Rain.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState;
}

namespace reforce::render {

class SolidPipeline;

using RainLook = engine::rain::Look;

// The drop instance the splat pass reads (one per drop), built by RainPipeline from engine::rain::Drop.
struct RainDropGpu {
    float cx, cy;      // center, output px
    float r;           // contact radius, px
    float k;           // cap height / contact radius
    float ax, ay;      // the stretch axis (unit)
    float stretch;     // along the axis (area kept)
    float tail;        // a run's tail: the back half longer
};
static_assert(sizeof(RainDropGpu) == 32);
// One drop's instance for an output of height `H` px (the wobble, the run's stretch and tail, the impact squash).
RainDropGpu rainDropGpu(const engine::rain::Drop& d, double contactDegrees, double H);

class RainPipeline {
public:
    explicit RainPipeline(SolidPipeline& solids);
    ~RainPipeline();
    RainPipeline(const RainPipeline&) = delete;
    RainPipeline& operator=(const RainPipeline&) = delete;

    int simCols() const { return cols_; }
    int simRows() const { return rows_; }
    MTL::Texture* waveTexture() const;  // the current wave heights (tests)
    MTL::Texture* fogTexture() const;   // the current fog (tests)
    MTL::Texture* dropTexture() const { return drops_; }

    // Once per sim step, after sim.step(): the floor's wave substeps (with this step's impulses) or the window's fog
    // (with this step's wipes), over sim.lastDt(). `reset` (the rain's first step) clears the fields first: waves
    // still, fog at look.fog.
    void encodeStep(MTL::CommandBuffer* cmd, const engine::rain::Sim& sim, const RainLook& look, bool reset);
    // Over `output` (BGRA8, readable; the colorized frame): copies it (with mips), frosts it, splats the drops and
    // draws the water. `trackAlpha`: the Syphon alpha is tracked (opaque water raises it).
    void encodeComposite(MTL::CommandBuffer* cmd, MTL::Texture* output, const engine::rain::Sim& sim,
                         const RainLook& look, bool trackAlpha);

private:
    void clearFields(MTL::CommandBuffer* cmd, float fog);

    SolidPipeline& solids_;
    MTL::Device* device_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    int cols_ = 0, rows_ = 0;
    MTL::RenderPipelineState* copyPso_ = nullptr;
    MTL::RenderPipelineState* wavePso_ = nullptr;
    MTL::RenderPipelineState* fogPso_ = nullptr;
    MTL::RenderPipelineState* dropPso_ = nullptr;
    MTL::RenderPipelineState* compositePso_ = nullptr;
    MTL::Texture* copy_ = nullptr;   // the frame, mipmapped
    MTL::Texture* drops_ = nullptr;  // RG16F: height px, halo
    MTL::Texture* wave_[3] = {nullptr, nullptr, nullptr};
    MTL::Texture* fog_[2] = {nullptr, nullptr};
    int waveCur_ = 0, fogCur_ = 0;
    bool valid_ = false;  // the fields were cleared (a step with reset ran)
};

}  // namespace reforce::render
