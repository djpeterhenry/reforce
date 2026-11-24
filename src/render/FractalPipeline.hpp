#pragma once
// reforce extension (PLAN 5L.14, docs/3d-design.md "Fractal zoom"): the GPU side of fractal Objects (Mesh="fractal").
//
// One raymarching pass per live fractal into its solid slot target (render/SolidPipeline's RGBA8 targets): a
// full-screen triangle over the target's top-left Res x the render size (half by default), so the cost follows the
// resolution; the returned SolidQuad covers the whole frame and samples that sub-rectangle (bilinear upscale), and the
// app composites it through the sprite pass like any solid (Pen / EPen / Flag / Over, the fade, trails).
//
// frac_fs, per pixel (all lengths relative to the camera, so the picture is invariant under the zoom step g; see
// engine/FractalZoom.hpp):
//   - the distance to S_inf: the point mapped by g^m into the shell about p, then the Sierpinski fold DE or the
//     Menger DE at a fractional level (the two neighboring levels blended) chosen so the smallest pieces are about
//     Detl pixels at their distance;
//   - sphere tracing until the distance is under .6 px, at most `maxSteps`, out to `far` x the camera's distance to p;
//   - shading: a key light from the camera's upper left, ambient occlusion from five distance samples along the normal
//     (at a share of the hit distance), a Fill floor, fog (fog x the camera's distance to p), and the beat glow (Glow:
//     a rim on the surfaces and a halo of near misses);
//   - out: (lum x a, a) premultiplied (r, g), a = fog x coverage; the sprite pass un-premultiplies it.

#include <array>

#include "engine/FractalZoom.hpp"
#include "render/SolidPipeline.hpp"

namespace MTL {
class Device; class CommandBuffer; class RenderPipelineState;
}

namespace reforce::render {

struct FractalDraw {
    engine::FractalKind kind = engine::FractalKind::Sierpinski;
    engine::FractalMap g;
    engine::FractalCamera cam;
    int slot = -1;
    float res = 0.5f;      // Res: the render scale, .25 .. 1
    float detail = 2.5f;   // Detl: the smallest pieces' size in pixels (larger = coarser, cheaper; under ~2 the
                           // regular hole grids alias into moire rings)
    int maxSteps = 120;    // Stps
    float fill = 0.06f;    // Fill: the floor of a lit surface
    float shade = 0.85f;   // Shad: 0 flat .. 1 full key light
    float glow = 0.0f;     // the beat glow this frame (Glow x the beat pulse)
    float fog = 3.0f;      // fog distance in units of the camera's distance to p
    float ao = 1.0f;       // ambient occlusion strength 0..1 (Ao)
    std::array<float, 3> light{-0.45f, 0.6f, 0.65f};  // towards the key light, camera space (x right, y up, z back)
};

class FractalPipeline {
public:
    // Shares `solids`' slot targets; `solids` must outlive this.
    explicit FractalPipeline(SolidPipeline& solids);
    ~FractalPipeline();
    FractalPipeline(const FractalPipeline&) = delete;
    FractalPipeline& operator=(const FractalPipeline&) = delete;

    // Raymarches d into d.slot's target; composite the returned quad with the sprite pass (encode before that step's
    // sprite pass).
    SolidQuad encode(MTL::CommandBuffer* cmd, const FractalDraw& d);
    // The sub-rectangle encode() renders for a given Res (tests).
    std::array<int, 2> renderSize(float res) const;

private:
    SolidPipeline& solids_;
    MTL::Device* device_;
    std::uint32_t width_ = 0, height_ = 0;
    MTL::RenderPipelineState* pso_ = nullptr;
};

}  // namespace reforce::render
