#pragma once
// reforce extension (docs/3d-design.md "Jigsaw"): the jigsaw of the live frame, drawn over the colorized frame like
// the glass 15 puzzle (Visualizer::colorize, right after encodeColorize), so its pieces wear the finished picture.
//
// encode(), per step with a jigsaw that is not whole:
//   1. The frame is copied (blit), and a half-size sharp and blurred copy made (the empty table's frost).
//   2. The cut map (only when the cut changes, about once a cycle; two are kept): per pixel the piece at home there,
//      the nearest other piece and the signed distance to the outline (RGBA8Uint: owner, other, distance in 1/256
//      px), from engine/JigsawShape.inc, the same code the engine and its tests use.
//   3. The board (full screen): where a piece is home, the frame with the cut's bevel and dark seam (Frame::seam,
//      faded out after completion) and the snap's flash; where it is not, the empty table: the frame frosted,
//      desaturated and darkened (Gap / Dim), recessed at its edges. Owner / other blend over a pixel (anti-aliased).
//   4. The loose pieces, bottom to top: each a soft drop shadow on what lies below (offset and blurred by its lift),
//      then the piece: a quad in 3D (turned, tilted, lifted, perspective) whose pixels look up their home position,
//      take their coverage from the cut map (anti-aliased) and the frame there, with the bevel lit from the upper
//      left and the seam line.
// Textures (made at startup): a frame-size BGRA8 copy, three half-size BGRA8, the frame-size cut map.

#include <cstdint>

#include "engine/Jigsaw.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState;
}

namespace reforce::render {

// The empty table's look (scene key Gap): frosted (default), gray, dark, ghost.
struct JigsawTable {
    float frost = 1.0f;  // 0 sharp .. 1 the blurred copy
    float sat = 0.5f;    // saturation kept
    float dim = 0.45f;   // brightness kept
};
JigsawTable jigsawTable(int gap);

struct JigsawDraw {
    engine::jigsaw::Frame frame;
    JigsawTable table;
    float frost = 10.0f;   // the table's blur, pixels at 1080p
    float opacity = 1.0f;  // fade envelope
    bool opaque = false;   // ColorKey = 0: the tracked alpha goes to 1 (the whole frame)
};

class JigsawPipeline {
public:
    JigsawPipeline(MTL::Device* device, uint32_t width, uint32_t height);
    ~JigsawPipeline();
    JigsawPipeline(const JigsawPipeline&) = delete;
    JigsawPipeline& operator=(const JigsawPipeline&) = delete;

    // `output`: BGRA8, the render size, readable (the colorized frame). Nothing is encoded for a whole frame.
    void encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const JigsawDraw& d, bool trackAlpha);

    MTL::Texture* cutMap() const { return cut_[lastCut_]; }  // tests: the one used last
    int cutBuilds() const { return cutBuilds_; }

private:
    MTL::Device* device_;
    uint32_t width_, height_;
    MTL::Texture* frame_ = nullptr;
    MTL::Texture* halfSharp_ = nullptr;
    MTL::Texture* halfBlur_ = nullptr;
    MTL::Texture* halfScratch_ = nullptr;
    // Two cut maps (two jigsaws cross-fading, or the cycle's previous and current cut), the least recently used
    // one rebuilt.
    MTL::Texture* cut_[2] = {nullptr, nullptr};
    MTL::RenderPipelineState* downPso_ = nullptr;
    MTL::RenderPipelineState* blurPso_ = nullptr;
    MTL::RenderPipelineState* cutPso_ = nullptr;
    MTL::RenderPipelineState* boardPso_[2] = {nullptr, nullptr};   // [alpha written]
    MTL::RenderPipelineState* piecePso_[2] = {nullptr, nullptr};
    MTL::RenderPipelineState* shadowPso_[2] = {nullptr, nullptr};
    engine::jigsaw::Grid cutGrid_[2];
    bool cutValid_[2] = {false, false};
    uint64_t cutUsed_[2] = {0, 0};
    uint64_t uses_ = 0;
    int lastCut_ = 0;
    int cutBuilds_ = 0;
};

}  // namespace reforce::render
