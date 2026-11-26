#pragma once
// reforce extension (docs/3d-design.md "Bubbles", Peter 2026-11-26): the GPU side of the bubbles object
// (Mesh="bubbles"; the simulation is engine/Bubbles). Like the rain and the glass puzzle it is not drawn into the
// visuals: it is laid over the colorized frame (Visualizer::colorize, after the rain), so the visuals keep running
// under the bubbles.
//
// Each bubble is a thin spherical shell drawn as one instanced quad straight over the frame (framebuffer fetch: what is
// already there, other bubbles included, shows through it). Per pixel the shell has two films, the front one and the
// back one seen through it, each with:
//   - a thickness: the bubble's mean (thinning with age) times a drainage profile (thin at the top, thick at the
//     bottom), times swirling 3D value noise on the sphere (domain-warped, flowing downward over time; Swrl, Drn); a
//     coming pop drains a black film down from the top (thinner than a quarter wave: no reflection at all);
//   - thin-film interference computed physically: the Airy reflectance of a soap film (n 1.33, the single-surface
//     Fresnel reflectance at the angle, the phase 2 pi 2 n d cos(theta_t) / lambda with the half-wave flip) at ten
//     wavelengths 406..694 nm, weighted into linear RGB by the CIE 1931 matching functions (a multi-lobe Gaussian fit)
//     and the sRGB matrix, normalized so a flat spectrum is white; Iris scales its saturation;
//   - what it reflects: the reflected ray (the front film's convex mirror, the back film's concave one, so the back's
//     reflections are upside down) sees the visuals Dpth behind (rays towards the screen) or the room in front (the
//     visuals mirrored and soft, a faint sky), plus a soft four-pane window highlight (Spec), lit by a light tinted a
//     little by the ColorMap (Tint).
// The visuals through the bubble shift only slightly (a thin shell is not a lens), most near the rim (Bend; the shift
// is taken from a mipmapped copy of the frame and added to what is there). Rim widens the Fresnel band at the edge.
// Shapes: the outline is r (1 + a2 cos 2t + b2 sin 2t + a3 cos 3t + b3 sin 3t) from the sim's shape modes; a cluster's
// shared wall cuts both bubbles at the plane where their spheres meet and is drawn (once, by the lower id) as a film
// seen edge-on: a bright line. A pop: the film is gone within an angle of the tear point that grows to pi in
// kPopSeconds, its retracting rim a bright thick band; the spray of droplets (sim droplets) are tiny lenses.
//
// Resources at startup (no render-thread allocations): the frame copy (BGRA8, full size, mips).

#include <cstdint>

#include "engine/Bubbles.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState;
}

namespace reforce::render {

class SolidPipeline;

using BubbleLook = engine::bubbles::Look;

// One bubble's instance (the vertex and fragment stages read it).
struct BubbleGpu {
    float a[4];     // center x, y (px), radius (px, inflated), alpha (inflation)
    float m[4];     // shape modes: n = 2 cos, sin; n = 3 cos, sin (relative)
    float f[4];     // mean film thickness (nm), doom 0..1, seed, film time (s)
    float p[4];     // pop seconds (< 0 intact), tear direction x, y, z
    float w[3][4];  // shared walls: normal x, y, distance from the center (px), half length (px; > 0: this bubble
                    // draws the wall, < 0: the other does, 0: none)
};
static_assert(sizeof(BubbleGpu) == 112);
// Bubble `i` of `sim` for an output `H` px high (its walls from its bonds).
BubbleGpu bubbleGpu(const engine::bubbles::Sim& sim, std::size_t i, const BubbleLook& look, double H);

struct DropletGpu {
    float cx, cy, r, alpha;  // px, px, px, 0..1
};
static_assert(sizeof(DropletGpu) == 16);

// The CPU copy of the shader's film reflectance (tests): linear RGB reflectance of a soap film `nm` thick, light
// arriving at cos(incidence) `cosI` (the Airy sum at ten wavelengths, CIE weighted, a flat spectrum normalized to 1).
void filmReflectance(double nm, double cosI, double rgb[3]);

class BubblePipeline {
public:
    explicit BubblePipeline(SolidPipeline& solids);
    ~BubblePipeline();
    BubblePipeline(const BubblePipeline&) = delete;
    BubblePipeline& operator=(const BubblePipeline&) = delete;

    // Over `output` (BGRA8, readable; the colorized frame): copies it (with mips) and draws the bubbles and the spray.
    // Nothing is encoded while there are none. `trackAlpha`: the Syphon alpha is tracked (opaque bubbles raise it).
    void encodeComposite(MTL::CommandBuffer* cmd, MTL::Texture* output, const engine::bubbles::Sim& sim,
                         const BubbleLook& look, bool trackAlpha);

private:
    MTL::Device* device_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    MTL::RenderPipelineState* copyPso_ = nullptr;
    MTL::RenderPipelineState* bubblePso_ = nullptr;
    MTL::RenderPipelineState* dropletPso_ = nullptr;
    MTL::Texture* copy_ = nullptr;  // the frame, mipmapped
};

}  // namespace reforce::render
