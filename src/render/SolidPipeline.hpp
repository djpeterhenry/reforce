#pragma once
// reforce extension (PLAN 5L.2, docs/3d-design.md): the GPU solid pass for 3D solids.
//
// Each live solid is drawn into its own RGBA8 target (luminance, alpha; b: the rubik's sticker share) and then
// composited by the existing sprite
// pass (render/ScenePipeline) as an ordinary SpriteQuad, so it gets the sprite blend flags, Pen / EPen layering,
// opacity, fades and trails.
//
//   - Startup (constructor, no render-thread allocations): one vertex buffer with every engine::makeMesh mesh
//     (offsets per MeshKind), the pipeline and depth states, `maxLive` RGBA8 targets at the full render size
//     (registered as sprite textures with premultiplied luminance), and one memoryless 4x MSAA colour + Depth32Float
//     pair shared by all slots (memoryless: no memory; private storage on GPUs without memoryless support).
//   - encode(): the solid is drawn only over its screen bounding box (engine::projectBounds, snapped out to whole
//     pixels plus a 1 px margin). The clip-space position is shifted so that box lands at the target's top-left
//     corner and the pass's render area is limited to the box size, so the clear / resolve cost follows the
//     solid's size. The sprite quad then covers the box in the frame and samples the matching sub-rectangle
//     (u1, v1 = box size / target size).
//   - 4x MSAA (reforce.Solid.Msaa = 4 | 1) resolves into the target, so the anti-aliased silhouette lands in
//     alpha and the luminance is premultiplied by it (the sprite shader un-premultiplies). Back faces are culled
//     (meshes wind counter-clockwise from outside) and the depth test is on (non-convex meshes: torus).
//
// Face texture sources (PLAN 5L.3), shared by all live solids and prepared once per step before encode():
//   - Text=0, the WaveShape tile (encodeWaveTile): a tileSize^2 R8Unorm texture with a full mip chain. The caller
//     maps the current WaveShape output into it with its own FrameMapping (app/VisualizerSolids.cpp: the scene's own
//     square [-1, 1]^2 in authored units fills the tile, so circles stay round for Aspc = 0 and 1) and the lines
//     are drawn by ScenePipeline::encodeTileLines (the analytic line shader), then the mips are generated.
//   - Text=1, the frame (encodeFrameTile): the previous intensity frame (ScenePipeline::previousIntensity, the
//     buffer this step's flow pass reads), its centred square crop (aspect kept; the full height of a landscape
//     frame) downsampled into a second mipmapped tile with a 4-tap filter. Recursive (Droste-like): the faces show
//     the frame that contains the solid.
//   - Text=2: flat (tex = 1), the phase-2 look.
//   Faces sample the tile with trilinear filtering (mip_filter::linear) at the mesh UVs.
//
// solid_fs:
//   lum = fill + (1 - fill) * tex                                     tex = the face source above, 0..1
//   lum *= mix(1, max(0, dot(n, light)), shade)                       n = world-space normal, light = unit vector
//   lum = max(lum, edge * (1 - smoothstep(0, edgeWidth, e)))         e = the mesh's interpolated edge value (0 on a
//                                                                     face border, 1 at its centre; smooth meshes 1)
//   out = (saturate(lum), 1)
//
// rubik_fs (MeshKind::Rubik, drawn once per cubie: the whole cube's model x the turning layer's rotation x the
// cubie's position): lum = the sticker colour's intensity on an outer sticker, else the plastic's (body), times the
// same shading term; the cube's circumradius is Scal.
//
// fifteen_fs (MeshKind::Fifteen, drawn once per tile; no tray): r = the tile's row-colour intensity everywhere on the
// tile, b = the share outside its number's seven-segment strokes (analytic, anti-aliased). The colour pass draws the
// strokes in a contrasting shade of the tile's colour (dark on light tiles, light on dark ones).
//
// Solids come from scene files (engine/Solids.hpp SolidRun, reforce.Solid.package; app/Visualizer startSolid), or
// from the test hook `--solid-test SPEC` (app/main.cpp), parsed by parseSolidTest below.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/LineGeometry.hpp"
#include "engine/Meshes.hpp"
#include "engine/Fifteen.hpp"
#include "engine/Rubik.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState; class DepthStencilState; class Buffer;

}

namespace reforce::render {

class ScenePipeline;

struct SolidPipelineOptions {
    int maxLive = 3;      // render targets (reforce.Object.MaxLive + 1)
    int msaa = 4;         // reforce.Solid.Msaa: 4 or 1
    bool cullBack = true; // tests: false draws back faces too (the depth test still picks the front)
    int tileSize = 512;   // reforce.Solid.TileSize: the face texture tiles (Text=0 / 1), R8Unorm, mipmapped
};

// Face texture source (scene key Text).
enum class SolidText { Wave = 0, Frame = 1, Flat = 2 };

struct SolidShading {
    float fill = 0.15f;       // Fill: floor of the face texture (phase 2: flat, so faces are 1 before shading)
    float shade = 0.6f;       // Shad: 0 = flat, 1 = full Lambert
    float edge = 0.8f;        // Edge: brightness of the face-border highlight (0 = none)
    float edgeWidth = 0.08f;  // in edge units (0 = border, 1 = face centre)
    std::array<float, 3> light{0.4f, 0.6f, 1.0f};  // direction towards the light (normalized when used)
};

// One solid for one step.
struct SolidDraw {
    engine::MeshKind mesh = engine::MeshKind::Cube;
    engine::SolidPose pose;
    bool perspective = true;
    double fovDegrees = 40.0;
    SolidText text = SolidText::Flat;  // the test hook's default; scene files default to Wave
    SolidShading shading;
    // mesh == Rubik (engine/Rubik.hpp): the cube's state and turn in progress, the intensity of each sticker colour
    // (engine::StickerPicker) and of the plastic. Text, Fill and Edge are not used; Shad is (best 0: shading changes
    // a sticker's intensity and so its colour).
    engine::rubik::Frame rubik = engine::rubik::frameAt(0, 0, 0);
    std::array<float, 6> stickers{0.3f, 0.44f, 0.58f, 0.72f, 0.86f, 1.0f};
    float body = 0.0f;
    // mesh == Fifteen (engine/Fifteen.hpp): the tiles' places; row r of the solved board (tiles 4r+1..4r+4) takes
    // stickers[kFifteenRowColour[r]]; the numbers get a contrasting shade of it in the colour pass.
    engine::fifteen::Frame fifteen = engine::fifteen::frameAt(0, 0, 0);
};
// The 15 puzzle as glass (scene key Glas = 1; docs/3d-design.md "Glass"): drawn over the colorized frame
// (encodeGlass), not into the visuals, so the WaveShape and flow keep running under it. Each tile is a 3D glass
// button (engine::makeGlassButton: rounded square, walls, rounded top edge) drawn with 4x MSAA and depth into its
// slot target, showing the frame blurred (Fros) and bent by its surface (Bend), tinted with its row colour (Tint),
// with a highlight, a Fresnel sheen and its number; the resolved target is then laid over the frame.
struct GlassDraw {
    engine::SolidPose pose;
    bool perspective = true;
    engine::fifteen::Frame fifteen = engine::fifteen::frameAt(0, 0, 0);
    std::array<std::array<float, 3>, 4> tint{};     // per row of the solved board, sRGB 0..1
    std::array<std::array<float, 3>, 4> numeral{};  // the numbers' colour per row
    float frost = 12.0f;  // blur sigma in pixels at 1080p
    float tintAmount = 0.35f;
    float bend = 1.0f;    // refraction at the rim
    float opacity = 1.0f; // fade envelope
    float numeralMode = 0.0f;  // engine::TilePicker::numeral: 0 dark numbers (light glass) .. 1 light (smoky glass)
    bool opaque = false;  // ColorKey = 0: raises the tracked alpha
    int slot = -1;        // the solid slot whose target holds the buttons (MSAA resolve) before the composite
};

// The sticker colours the 15 puzzle's rows use: one of each opposite-face pair first (those are the farthest apart).
inline constexpr int kFifteenRowColour[4] = {0, 2, 4, 1};

// Where a drawn solid landed: the sprite quad (render pixels, origin top-left) and the target sub-rectangle.
struct SolidQuad {
    bool visible = false;
    int texture = -1;  // ScenePipeline sprite texture handle of the slot
    float cx = 0, cy = 0, w = 0, h = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};

// CPU reference for tests and the test hook's log: projected extent, hull area and visible faces.
struct SolidReference {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // projected vertex extent, render pixels (origin top-left, +y down)
    double area = 0;                       // area of the 2D convex hull of the projected vertices, pixels^2
    struct Face {
        float x = 0, y = 0;  // projected face centre, render pixels
        float lum = 0;       // expected solid_fs luminance at the centre (no edge highlight there; flat source)
        float facing = 0;    // cos of the angle between the normal and the direction to the camera
    };
    std::vector<Face> faces;  // front faces of polyhedra (hull faces), most camera-facing first
};
SolidReference solidReference(const engine::Mesh& mesh, const SolidDraw& d, int width, int height);

// ---- phase-2 test hook: --solid-test SPEC
// SPEC = MESH[,key=value...] (or MESH:key=value:...), e.g. "cube,rotx=.5,roty=.7,scale=.6". Keys (case-insensitive):
//   x y z scale rotx roty rotz             pose (sprite coordinates, radians; engine::SolidPose)
//   spinx spiny spinz                      constant spin, rad/s added to the rotations (0 = fixed)
//   fill shade edge edgewidth              solid_fs parameters
//   lightx lighty lightz                   light direction
//   pen epen                               Pen / EPen (absent EPen = none; epen=-1 also means none)
//   flag                                   sprite blend flags (engine::sprite::Flag, e.g. 4096 = SelectiveBlend)
//   opacity                                fade envelope stand-in (constant)
//   pers                                   1 = perspective (default), 0 = orthographic
//   text                                   face source: 0 = WaveShape tile, 1 = frame, 2 = flat (hook default 2)
//   turn seed moves                        rubik / fifteen: position in the cycle, scramble seed, scramble length (30)
//   shuf                                   fifteen: the leap into the scramble, 0..1 (1)
//   glass                                  fifteen: 1 = as glass over the frame (encodeGlass; Fros 12, Tint .35)
struct SolidTestSpec {
    SolidDraw draw;
    double spin[3] = {0, 0, 0};
    double pen = 1, epen = -1;  // epen < 0: no EPen
    unsigned flags = 0;
    double opacity = 1;
    std::string text;  // as given
    double turn = -1, seed = 0;  // rubik / fifteen (turn < 0: solved)
    double shuffle = 1;          // fifteen
    bool glass = false;          // fifteen: drawn as glass (encodeGlass)
    int moves = 30;
};
bool parseSolidTest(const std::string& spec, SolidTestSpec& out, std::string* error = nullptr);

class SolidPipeline {
public:
    // Registers the slot targets as sprite textures of `scene`, which must outlive this.
    SolidPipeline(ScenePipeline& scene, const SolidPipelineOptions& opts);
    ~SolidPipeline();
    SolidPipeline(const SolidPipeline&) = delete;
    SolidPipeline& operator=(const SolidPipeline&) = delete;

    int maxLive() const { return int(slots_.size()); }
    int msaa() const { return msaa_; }
    const engine::Mesh& mesh(engine::MeshKind k) const { return meshes_[std::size_t(k)]; }
    MTL::Texture* target(int slot) const { return slots_[std::size_t(slot)].tex; }
    int spriteTexture(int slot) const { return slots_[std::size_t(slot)].handle; }

    int tileSize() const { return tileSize_; }
    MTL::Texture* waveTile() const { return waveTile_; }
    MTL::Texture* frameTile() const { return frameTile_; }

    // Face sources for this step (call once per step, before encode(), only when a live solid uses them):
    // Text=0: draws `segments` (tile pixels, origin top-left) into the WaveShape tile and builds its mips.
    void encodeWaveTile(MTL::CommandBuffer* cmd, const engine::LineSegment* segments, std::size_t count);
    // Text=1: the centred square crop of the previous intensity frame into the frame tile, with mips.
    void encodeFrameTile(MTL::CommandBuffer* cmd);

    // Draws `d` into slot's target (clear + mesh) on `cmd`; encode before the ScenePipeline step that composites
    // the returned quad. Nothing is encoded when the solid is off screen (visible = false). A Text=0 / 1 source
    // that was never prepared reads as 0 (Fill only).
    SolidQuad encode(MTL::CommandBuffer* cmd, int slot, const SolidDraw& d);
    // The glass 15 puzzle onto `output` (BGRA8, readable, the render size; the colorized frame): blurs the frame at
    // half size, draws the buttons into g.slot's target, then over the frame. `trackAlpha`: the Syphon alpha is
    // tracked (opaque glass raises it by its coverage).
    void encodeGlass(MTL::CommandBuffer* cmd, MTL::Texture* output, const GlassDraw& g, bool trackAlpha);

private:
    ScenePipeline& scene_;
    MTL::Device* device_;
    uint32_t width_, height_;
    int msaa_ = 4;
    std::vector<engine::Mesh> meshes_;  // index = MeshKind
    struct Range { uint32_t first = 0, count = 0; };
    std::vector<Range> ranges_;         // index = MeshKind, in vertices
    MTL::Buffer* vertices_ = nullptr;
    MTL::RenderPipelineState* pso_ = nullptr;
    MTL::RenderPipelineState* rubikPso_ = nullptr;  // rubik_fs: the cubies
    MTL::RenderPipelineState* fifteenPso_ = nullptr;  // fifteen_fs: the tray and tiles
    // Glass (encodeGlass): half-size copies of the frame (sharp, then blurred through a scratch) and the pipelines.
    MTL::Texture* halfSharp_ = nullptr;
    MTL::Texture* halfBlur_ = nullptr;
    MTL::Texture* halfScratch_ = nullptr;
    MTL::RenderPipelineState* downPso_ = nullptr;
    MTL::RenderPipelineState* blurPso_ = nullptr;
    MTL::RenderPipelineState* glassPso_ = nullptr;                 // the buttons (MSAA, depth) into a slot target
    MTL::RenderPipelineState* glassCompositePso_ = nullptr;        // ... then over the frame
    MTL::RenderPipelineState* glassCompositeOpaquePso_ = nullptr;  // ... raising the tracked alpha
    MTL::Buffer* glassVertices_ = nullptr;                         // engine::makeGlassButton
    uint32_t glassVertexCount_ = 0;
    MTL::RenderPipelineState* framePso_ = nullptr;  // previous frame crop -> frame tile
    int tileSize_ = 512;
    MTL::Texture* waveTile_ = nullptr;   // R8Unorm, mipmapped
    MTL::Texture* frameTile_ = nullptr;  // R8Unorm, mipmapped
    bool waveValid_ = false, frameValid_ = false;
    MTL::DepthStencilState* depth_ = nullptr;
    MTL::Texture* msaaColor_ = nullptr;  // memoryless, msaa_ > 1 only
    MTL::Texture* depthTex_ = nullptr;   // memoryless
    bool cullBack_ = true;
    struct Slot { MTL::Texture* tex = nullptr; int handle = -1; };
    std::vector<Slot> slots_;
};

}  // namespace reforce::render
