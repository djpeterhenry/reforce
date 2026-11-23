#pragma once
// reforce extension (PLAN 5P, docs/dancers-design.md): the GPU pass for the dancers, in full color over the
// colorized frame.
//
// encode() runs inside the colorize step (app/Visualizer colorize), after the frame (and the color video layer and
// movie sprites) is in `output`:
//   1. light: `output` is downsampled into a small mipmapped light texture (256 px wide). The visuals light the
//      dancers: each surface samples it near its own screen position, pushed in the direction its normal faces, at a
//      blurry mip; a rim light comes from the frame around the silhouette; the floor reflects it.
//   2. stage: into a frame-size BGRA8 target (premultiplied alpha), 4x MSAA with memoryless color and depth:
//      the disco floor (with blob shadows and the dancers' reflection, drawn just before into its own half-size
//      target, opaque so overlapping parts never double up), then every body part as an instance of one "capsule" mesh
//      (engine/DancerRig.hpp BodyPart), then the outline pass (the same instances, front faces culled, pushed out
//      along the normal by a pixel width).
//   3. composite: the target alpha-over `output` (premultiplied: out = stage + output * (1 - a)).
// Nothing is encoded while no dancer is live and the floor is faded out. Every pipeline state and buffer is made in
// the constructor (no render-thread allocations); instance data goes into a ring of shared buffers.

#include <cstdint>
#include <vector>

#include "engine/DancerRig.hpp"
#include "engine/Dancers.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState; class DepthStencilState; class Buffer;
}

namespace reforce::render {

struct DancerPipelineOptions {
    int msaa = 4;          // 4 | 1
    int maxParts = 6144;   // instances per frame (about 40-110 per dancer: 32 on stage with big hair, plus leavers)
};

// One frame of the stage (filled on the sim step, drawn at colorize).
struct DancerStage {
    engine::StageCamera cam;
    std::vector<engine::dance::BodyOut> bodies;
    int bodyCount = 0;     // bodies[0 .. bodyCount) are live this frame (the vector keeps its capacity)
    float floor = 0;       // floor visibility 0..1
    float shadows = 0;     // shadow strength 0..1 (fades with the dancers)
    double beat = 0;       // for the tile patterns
    int pattern = 0;       // floor pattern (changes every bar or two)
    float lighting = 0.75f;  // 0 studio light .. 1 lit only by the visuals
    float outline = 2.0f;    // outline width, pixels at 1080p (0 = none)
    float pulse = 0;         // 0..1 beat pulse for the floor
    // Reflections of the dancers in the floor (reforce.Dancer.Reflection): 0 = none, 1 = strong.
    float reflection = 0.35f;
    // The whole stage (dancers, floor) faded by this: hiding the dancers (Visualizer, dancehide).
    float opacity = 1.0f;
    float zoom = 1.0f;     // the crowd zoom (1 = the set dancer size; smaller: the camera pulled back), for the tags
};

class DancerPipeline {
public:
    DancerPipeline(MTL::Device* device, uint32_t width, uint32_t height, const DancerPipelineOptions& opts = {});
    ~DancerPipeline();
    DancerPipeline(const DancerPipeline&) = delete;
    DancerPipeline& operator=(const DancerPipeline&) = delete;

    int msaa() const { return msaa_; }
    // Draws the stage over `output` (BGRA8, the render size). Returns false when there was nothing to draw.
    // Color only; with `trackAlpha` (reforce.Syphon.Alpha) the output's alpha also gains how much must stay
    // opaque: the bodies fully, the floor (and its shadows) by its coverage (ScenePipeline::encodeSyphonAlpha).
    bool encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const DancerStage& stage, bool trackAlpha = false);
    // Parts drawn by the last encode (tests, perf).
    int lastParts() const { return lastParts_; }

private:
    MTL::Device* device_;
    uint32_t width_, height_;
    int msaa_ = 4;
    int maxParts_;
    uint32_t meshIndices_ = 0;
    MTL::Buffer* mesh_ = nullptr;     // unique capsule vertices
    MTL::Buffer* meshIdx_ = nullptr;  // 16-bit triangle indices
    std::vector<MTL::Buffer*> ring_;
    std::size_t ringNext_ = 0;
    MTL::Texture* target_ = nullptr;
    MTL::Texture* msaaColor_ = nullptr;
    MTL::Texture* opaque_ = nullptr;      // R8: how much of the Syphon alpha stays opaque (resolved)
    MTL::Texture* msaaOpaque_ = nullptr;
    MTL::Texture* depth_ = nullptr;
    MTL::Texture* light_ = nullptr;
    MTL::Texture* reflect_ = nullptr;       // the dancers mirrored under the floor (half size, premultiplied)
    MTL::Texture* reflectDepth_ = nullptr;
    MTL::RenderPipelineState* lightPso_ = nullptr;
    MTL::RenderPipelineState* floorPso_ = nullptr;
    MTL::RenderPipelineState* bodyPso_ = nullptr;
    MTL::RenderPipelineState* reflectPso_ = nullptr;
    MTL::RenderPipelineState* outlinePso_ = nullptr;
    MTL::RenderPipelineState* compositePso_ = nullptr;
    MTL::RenderPipelineState* compositeAlphaPso_ = nullptr;
    MTL::DepthStencilState* depthWrite_ = nullptr;
    MTL::DepthStencilState* depthFloor_ = nullptr;
    int lastParts_ = 0;
};

}  // namespace reforce::render
