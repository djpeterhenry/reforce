#pragma once
// reforce extension (docs/3d-design.md "Tower of Hanoi", Peter 2026-11-26): the GPU side of the glass Tower of Hanoi
// (Mesh="hanoi"; the solve is engine/Hanoi). Like the glass 15 puzzle and the rain it is not drawn into the visuals:
// it is laid over the colorized frame (Visualizer::colorize, after the glass puzzle), so the visuals keep running
// behind it and show through it.
//
// Geometry: every part is a lathe around a short segment, made in the vertex shader from a profile (no vertex
// buffers): a disc is a glass washer (an annulus with half-round inner and outer rims), a peg a rod with a round top,
// the base a stadium slab with rounded edges. Drawn with 4x MSAA and depth into the solid's slot target (resolved
// premultiplied), then over the frame (SolidPipeline::encodeGlassComposite). The pass and the composite cover only the
// tower's screen box. Each part sees the frame behind the tower (not the other parts).
//
// Optics (hanoi_fs, per pixel): the part's exact distance field (the same shape as the mesh) gives the true entry
// point and normal; the view ray refracts in (Snell, n = 1 + .5 Bend, a touch more for blue: dispersion), is traced
// through the glass to where it leaves (sphere tracing inside the field, bisected; total internal reflection bounces
// once), refracts out and runs on to the visuals, a plane Dpth frame heights behind the tower; that point is projected
// back to the screen and sampled. So a rim is a strong lens (the visuals behind it minified and flipped), the flat
// tops shift them a little, a peg is a rod lens. Flat faces and the pegs are left analytically; the rims are traced.
// The sample comes from a mipmapped half-size copy of the frame at the ray's screen footprint (no shimmer where the
// glass minifies), a share of it frosted (a coarser level, Fros); the glass absorbs along the path in its tint (Tint;
// thicker = deeper); Fresnel reflects a dim room with a soft highlight (Spec); rims darken a little and catch a sheen,
// so the glass reads over white as well as black; a landing flashes the disc's rim (glint).
//
// Resources at startup (no render-thread allocations): the mipmapped half-size copy (BGRA8, ~2.8 MB at 1080p) and the
// pipelines. It shares the solids' MSAA color and depth and the object's slot target.

#include <array>
#include <cstdint>

#include "engine/Hanoi.hpp"
#include "engine/Meshes.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState; class RenderCommandEncoder;
class DepthStencilState;
}

namespace reforce::render {

class SolidPipeline;

struct HanoiDraw {
    engine::SolidPose pose;  // Scal = half the base's length
    bool perspective = true;
    engine::hanoi::Frame frame;
    std::array<std::array<float, 3>, engine::hanoi::kMaxDiscs> tint{};  // per disc size (sRGB 0..1)
    std::array<float, 3> clear{0.9f, 0.93f, 0.95f};  // the pegs' and base's tint (faint)
    bool oneGlass = false;  // Mono: the pegs and base as deeply tinted as the discs (all one glass)
    float frost = 6;      // Fros: blur sigma in px at 1080p of the frosted copy
    float tintAmount = 0.5f;  // Tint: 0 clear .. 1 deep color
    float bend = 1;       // Bend: n = 1 + .5 Bend
    float depth = 0.5f;   // Dpth: frame heights from the tower to the visuals
    float spec = 1;       // Spec: highlight strength
    float opacity = 1;    // fade envelope
    bool opaque = false;  // ColorKey = 0: raises the tracked alpha
    int slot = -1;        // the solid slot whose target holds the glass before the composite
};

// The tower's model matrix (tower units: Scal = kRadius; centered on half the pegs' height).
engine::Mat4 hanoiModel(const engine::SolidPose& pose, int discs, double aspect);

class HanoiPipeline {
public:
    explicit HanoiPipeline(SolidPipeline& solids);
    ~HanoiPipeline();
    HanoiPipeline(const HanoiPipeline&) = delete;
    HanoiPipeline& operator=(const HanoiPipeline&) = delete;

    // Over `output` (BGRA8, readable; the colorized frame). `trackAlpha`: the Syphon alpha is tracked.
    void encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const HanoiDraw& d, bool trackAlpha);

private:
    struct Part;
    void drawPart(MTL::RenderCommandEncoder* enc, const Part& p, const HanoiDraw& d, const engine::Mat4& adj,
                  const engine::Mat4& vp, float W, float H);

    SolidPipeline& solids_;
    MTL::Device* device_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    MTL::RenderPipelineState* glassPso_ = nullptr;
    MTL::RenderPipelineState* downPso_ = nullptr;   // the frame at half size into the copy
    MTL::DepthStencilState* depth_ = nullptr;
    MTL::Texture* mip_ = nullptr;                   // the half-size frame, mipmapped
};

}  // namespace reforce::render
