#pragma once
// The G-Force frame on the GPU (architecture §2), at the fixed render size:
//
//   1. flow      I_new(p) = max(0, bilinear(I_prev, field(p)) - decay)
//                field: RG32Float, normalized source coords (+-1 = frame edges, +y up);
//                |src| > 1 (incl. the out-of-frame sentinel) -> 0   (probes #6, #13)
//                During a FlowField transition: field = blend(A, B, w(p)) with the
//                layer's blend mode; w(p) = w * clamp(ALFA(p)) with a Union mask.
//   2. lines     capsules (round caps/joins; dots = zero length) drawn in order
//                into I_new with OVERWRITE: out = pen*cov + dst*(1-cov);
//                cov = 1 inside (no AA) or the AA edge coverage   (probe #15)
//                AA (Phase 5E, LineAa::PerPath): cov is the max over the
//                segments of one path (no double blend at joints), and each
//                path composites as dst = mix(dst, pen, cov) in path order.
//                cov per segment: ThinLines::Smooth (capsule distance ramp) or
//                ThinLines::GForce (G-Force's snapped column/row spans, #32).
//   2b. sprites  (only while sprites are live) the displayed frame is a copy
//                of I_new plus the sprites' Pen pass; then the EPen pass is
//                drawn into I_new itself, so EPen is what feeds back (the
//                trail) [DS]. Blend per sprite flags (engine/Sprites.hpp).
//                Movie / live sprites (Phase 5F) sample a BGRA video frame
//                (luminance computed in the shader) set each step.
//   3. colorize  RGB = palette(lut(displayed)) -> BGRA8 output texture
//                (+ the color video layer, Phase 5F: fitted, keyed, crossfaded
//                and blended over/under in the same pass; VideoLayerDraw)
//
// Steps 1+2 run once per simulation tick (encodeStep); colorize runs once per
// display frame on the latest intensity (encodeColorize).
//
// Intensity textures ping-pong. Format: R8Unorm ("gforce": G-Force's 8-bit
// buffer, so the 1/255 decay and rounding behave like it) or R16Float ("hq").
//
// No hitches (Phase 5D): FlowField / Union textures come from pools that are
// filled at startup; workers upload into them (uploadField / uploadMask, any
// thread), the render thread only swaps handles. A handle returns its texture
// to the pool when dropped; it is reused once the GPU has finished every step
// encoded before that. An interrupted FlowField transition is baked on the
// GPU (bakeField) into a pooled texture. Every pipeline state is built in the
// constructor; render-thread Metal allocations are counted (perf::renderAllocs).

#include <array>
#include <atomic>
#include <future>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/FlowFieldBuilder.hpp"
#include "engine/Blend.hpp"
#include "engine/LineGeometry.hpp"
#include "engine/Palette.hpp"
#include "engine/VideoComposite.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class RenderPipelineState; class Buffer; class Library;
}

namespace reforce::render {

class TexturePool;

// A pooled texture (see above). Destroying the last handle returns it.
class PooledTexture {
public:
    PooledTexture(std::shared_ptr<TexturePool> pool, MTL::Texture* tex) : pool_(std::move(pool)), tex_(tex) {}
    ~PooledTexture();
    PooledTexture(const PooledTexture&) = delete;
    PooledTexture& operator=(const PooledTexture&) = delete;
    MTL::Texture* texture() const { return tex_; }

private:
    std::shared_ptr<TexturePool> pool_;
    MTL::Texture* tex_;
};
using TextureHandle = std::shared_ptr<PooledTexture>;

// Thread-safe pool of same-size textures.
class TexturePool : public std::enable_shared_from_this<TexturePool> {
public:
    TexturePool(MTL::Device* device, unsigned long pixelFormat, uint32_t width, uint32_t height, bool renderTarget,
                const char* label, std::shared_ptr<std::atomic<std::uint64_t>> completedSerial);
    ~TexturePool();
    // A free texture (one the GPU is done with), or a new one. `renderThread`:
    // count a creation as a render-thread allocation.
    TextureHandle acquire(bool renderThread);
    // Creates textures until `n` are free (startup warm-up).
    void reserve(int n);
    // Fills a texture from rows of `bytesPerRow` bytes (any thread).
    TextureHandle upload(const void* data, std::size_t bytesPerRow, bool renderThread);
    void setSubmittedSerial(std::uint64_t s) { submitted_.store(s, std::memory_order_release); }
    std::size_t created() const { return created_.load(); }

private:
    friend class PooledTexture;
    MTL::Texture* make();
    void release(MTL::Texture* tex);
    MTL::Device* device_;
    unsigned long format_;
    uint32_t width_, height_;
    bool renderTarget_;
    const char* label_;
    std::shared_ptr<std::atomic<std::uint64_t>> completed_;
    std::atomic<std::uint64_t> submitted_{0};
    std::mutex mutex_;
    struct Free { MTL::Texture* tex; std::uint64_t serial; };
    std::vector<Free> free_;
    std::atomic<std::size_t> created_{0};
};

enum class IntensityFormat { GForce8, Half16 };

// Line antialiasing method (when ScenePipelineOptions::antialias is on).
//   PerPath   max coverage per path, then one overwrite-blend per path (default;
//             falls back to Analytic on GPUs without programmable blending)
//   Analytic  legacy: each segment blends on its own (double blend at joints)
//   Msaa4     4x MSAA, per-sample overwrite (comparison baseline only)
// Cost (reforce-line-lab bench, M-series GPU): per-path is about 2x the legacy
// pass but stays small: <= 0.11 ms at 1080p and <= 0.15 ms at 2160p for 6.4k-16k
// segments or 2000 short paths, so no batching/fallback for many small paths.
enum class LineAa { PerPath, Analytic, Msaa4 };
const char* lineAaName(LineAa m);
bool parseLineAa(const std::string& s, LineAa& out);

// How lines and dots are rasterized under per-path AA (settings reforce.ThinLines).
//   Smooth  antialiased capsules (round caps, sub-pixel positions); lines and dots
//           narrower than ~2 px are drawn on a footprint >= 1 px with the coverage
//           scaled so their total energy (area x intensity) matches G-Force's
//   GForce  G-Force's own rule at every width (probe #32): endpoints snapped to
//           pixel centers (col = W/2 + trunc(x W/2), row = H/2 + trunc(-y H/2)),
//           and each pixel covered by the length of the round-capped capsule
//           along the pixel's center line in the segment's major axis (vertical
//           for x-major segments and dots, horizontal for y-major), clipped to
//           the pixel. A thin horizontal line is one row at 255 x width; a dot is
//           a column-span disc (a 1-px-wide box of height = width below 1 px).
// Only the per-path method honors it (analytic / MSAA / no-AA keep their own).
enum class ThinLines { Smooth, GForce };

// How the flow pass resamples and quantizes intensity in the 8-bit ("gforce") buffer. G-Force behaves like
// integer bilinear with 4-bit sub-pixel weights and truncation (Fixed16Floor; corpus sweep, docs/sweep.md).
enum class FlowRounding : int32_t { Nearest = 0, Floor = 1, PointFloor = 2, Fixed16Floor = 3, Fixed256Floor = 4 };

struct ScenePipelineOptions {
    IntensityFormat format = IntensityFormat::GForce8;
    // Corpus sweep (docs/sweep.md): bilinear with 1/16-px weights + truncation matches G-Force on 337/349
    // comparable FlowFields (nearest rounding: 65) and keeps probe parity at 112/115.
    FlowRounding flowRounding = FlowRounding::Fixed16Floor;  // setting reforce.FlowRounding
    float decayPerFrame = 1.0f / 255.0f;  // probe #6
    bool antialias = true;                // WaveShape.AntiAlias
    LineAa lineAa = LineAa::PerPath;      // method when antialias is on
    ThinLines thinLines = ThinLines::Smooth;  // settings reforce.ThinLines (per-path AA only)
    // reforce.LineEdges = color: antialiased line edges blend the line's and the background's ColorMap colors in
    // the displayed frame (no multi-colored fringes); the feedback keeps G-Force's intensity edges.
    bool colorEdges = false;
};

struct FlowBlend {
    bool active = false;  // a transition is running (field B bound)
    float weight = 0;     // incoming weight
    int mode = 0;         // engine::blend::FieldMode
    bool useMask = false; // Union mask bound
};

// One sprite quad for the sprite pass (render pixels, origin top-left).
// Movie / live sprite luminance levels (PLAN 5K: per-movie Black / White / Gamma in .reforce.txt, times the global
// reforce.MovieSprite.Gamma): l = saturate((l - black) / (white - black))^gamma. Defaults = unchanged.
struct VideoLevels {
    float black = 0, white = 1, gamma = 1;
    bool identity() const { return black == 0.0f && white == 1.0f && gamma == 1.0f; }
};

struct SpriteQuad {
    int texture = -1;       // handle from addSpriteTexture
    float cx = 0, cy = 0;   // center
    float w = 0, h = 0;     // drawn size
    float pen = 0;          // intensity (Pen or EPen)
    float opacity = 1;      // fade envelope
    int blend = 0;          // engine::SpriteBlend
    int op = 0;             // engine::SpriteOp (0 blend, 1 keyed copy, 2 fetch: reads the target)
    float mix = 0;          // sprite Mix (engine::spriteLayerPlan): > 0 dims the target under the sprite and
                            // raises the half-way lighten (drawn by the fetch pipeline); 0 = G-Force
    // Texture sub-rectangle the quad samples (u0, v0 = top-left; 3D solids, PLAN 5L.2: the solid's box in its
    // frame-sized target). Default: the whole texture.
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    // Full-color movie sprites: opaque in the Syphon alpha (the movie's ColorKey = 0) while it is tracked.
    bool opaque = false;
    // Puzzle solids drawn in color (encodeColorize's colorSolids): 0 rubik (stickers and plastic), 1 fifteen (tiles
    // with numbers in a contrasting shade), 2 labyrinth (board, walls, ball; black holes; highlights to white).
    int puzzle = 0;
    float numeral = 0;  // fifteen: the numbers 0 = dark .. 1 = light (engine::TilePicker::numeral)
    // labyrinth (puzzle 2): the board, walls and ball intensities; an edge pixel between two of them takes a mix of
    // their two colors (not the ColorMap colors in between)
    float levels[3] = {0.0f, 1.0f, 1.0f};
};

// Color video layer for one colorize pass (Phase 5F): outgoing source A and
// incoming B (either may be null; "none" fades from / to nothing), crossfade
// weight A -> B, per-source fit transforms. Textures are BGRA8 frames owned
// by the caller until the GPU is done.
// Body tracking on the backdrop pass (docs/body.md).
struct BackdropBody {
    static constexpr int kJoints = 19;
    MTL::Texture* mask = nullptr;  // R8 person mask in camera coordinates (null = none)
    float front = 0;               // 0..1: the person cuts through the visuals
    bool show = false;             // tint the mask, mark the joints
    std::array<float, 4 * kJoints> joints{};  // per joint: screen uv x, y, weight, 0
};

struct VideoLayerDraw {
    MTL::Texture* a = nullptr;
    MTL::Texture* b = nullptr;
    engine::UvTransform uvA, uvB;
    float weight = 1;
    engine::VideoLayerParams params;
};

class ScenePipeline {
public:
    ScenePipeline(MTL::Device* device, uint32_t width, uint32_t height, const ScenePipelineOptions& opts);
    ~ScenePipeline();
    ScenePipeline(const ScenePipeline&) = delete;
    ScenePipeline& operator=(const ScenePipeline&) = delete;

    uint32_t width() const { return width_; }
    // The line AA method in use ("off" when antialias is off).
    const char* lineAaMethod() const;
    // True when lines use G-Force's rasterization (ThinLines::GForce under per-path AA).
    bool gforceLines() const { return opts_.antialias && linePathPso_ && opts_.thinLines == ThinLines::GForce; }

    // ---- live "Look" changes (engine/Look.hpp), render thread, between steps.
    // Thin lines, flow rounding and AA on/off apply from the next encodeStep.
    // AA off -> on uses the method chosen at construction (opts.lineAa).
    const ScenePipelineOptions& options() const { return opts_; }
    void setThinLines(ThinLines m) { opts_.thinLines = m; }
    void setFlowRounding(FlowRounding r) { opts_.flowRounding = r; }
    void setAntialias(bool on) { opts_.antialias = on; }
    void setColorEdges(bool on) { opts_.colorEdges = on; }
    IntensityFormat format() const { return opts_.format; }
    // Switches the trail buffer format: new intensity ping-pong + displayed-frame
    // textures (allocated here, once per switch); the next encode first copies the
    // current trail (and displayed frame) into them, so the picture carries on.
    // The other format's pipelines are compiled on first use, or ahead of time on a
    // worker by prewarmOtherFormat().
    void setIntensityFormat(IntensityFormat f);
    void prewarmOtherFormat();
    // Latest intensity frame (tests and tools read it back).
    MTL::Texture* currentIntensity() const { return intensity_[current_]; }
    uint32_t height() const { return height_; }

    // Field / Union mask textures from the pools; any thread (workers).
    TextureHandle uploadField(const engine::FlowFieldGrid& grid) const;
    TextureHandle uploadMask(const engine::UnionMask& mask) const;
    std::shared_ptr<TexturePool> fieldPool() const { return fieldPool_; }
    std::shared_ptr<TexturePool> maskPool() const { return maskPool_; }

    // Replaces the current flow field / the incoming one (must be width x
    // height). The grid overloads upload on the calling thread (startup).
    void setField(TextureHandle field);
    void setIncomingField(TextureHandle field);
    void setField(const engine::FlowFieldGrid& grid) { setField(uploadField(grid)); }
    // End of a transition: the incoming field becomes the current one.
    void promoteIncomingField();
    // Union ALFA mask for the running transition (null: none).
    void setUnionMask(TextureHandle mask);
    const TextureHandle& currentField() const { return field_[0]; }

    // Interruption: the running blend (current, incoming, Union mask, weight w,
    // mode) is baked on the GPU into a new field, which becomes the current
    // one (the incoming one and the mask are dropped). The bake is encoded at
    // the start of the next encodeStep, before its flow pass, from the textures
    // bound now. Same math as engine::bakeFieldBlend.
    TextureHandle bakeField(float w, int mode, bool useMask);

    // Fills the current intensity buffer with seeded value noise (M3 static demo)
    // or clears it.
    void encodeNoise(MTL::CommandBuffer* cmd, uint32_t seed);
    // Kick zoom (PLAN 5O.3) for the next flow passes: every source coordinate is scaled by (1 - zoom) and turned by
    // `rotate` radians about the center (aspect-correct), so the trail pushes out (and turns) for a moment. 0, 0 =
    // the plain flow, bit for bit.
    void setKick(float zoom, float rotate) {
        kickZoom_ = zoom;
        kickRotate_ = rotate;
    }
    void encodeClear(MTL::CommandBuffer* cmd);

    // One simulation step: flow (+ transition blend) and lines into the intensity buffer.
    void encodeStep(MTL::CommandBuffer* cmd, const engine::LineSegment* segments, std::size_t segmentCount,
                    const FlowBlend& blend);
    // Same, plus sprites (engine::spriteLayerPlan): `feedbackQuads` into the feedback buffer, either
    // between the flow and the lines (`feedbackBeforeLines`, G-Force order, probe #40; after the lines
    // with MSAA line AA) or after the displayed frame was taken (melt); `displayQuads` onto the
    // displayed frame only. Each list is drawn in order.
    void encodeStep(MTL::CommandBuffer* cmd, const engine::LineSegment* segments, std::size_t segmentCount,
                    const FlowBlend& blend, const SpriteQuad* feedbackQuads, std::size_t feedbackCount,
                    bool feedbackBeforeLines, const SpriteQuad* displayQuads, std::size_t displayCount);

    // Sprite images: 2 bytes per pixel (luminance, alpha), rows top to bottom.
    // Mipmaps are generated on the next encodeStep. Returns a handle (>= 0).
    int addSpriteTexture(const std::uint8_t* la, int width, int height, bool hasAlpha);
    // Same in two halves, so a worker can create and fill the texture:
    // createSpriteTexture (any thread; returns a retained texture or null),
    // then addSpriteTexture(tex) on the render thread (takes a reference).
    static MTL::Texture* createSpriteTexture(MTL::Device* device, const std::uint8_t* la, int width, int height);
    int addSpriteTexture(MTL::Texture* tex, bool hasAlpha);
    MTL::Device* device() const { return device_; }
    void removeSpriteTexture(int handle);
    // 3D solid target (PLAN 5L.2, render/SolidPipeline): an RG8 render target holding premultiplied
    // (luminance x alpha, alpha), as an MSAA resolve leaves it; the sprite shader un-premultiplies it and uses the
    // alpha as coverage. Takes a reference; no mipmaps.
    int addSolidTexture(MTL::Texture* tex);
    // 3D solids, face textures (PLAN 5L.3, render/SolidPipeline):
    //   enableTileLines(): startup only. Creates the R8Unorm line pipeline for the WaveShape tile and adds
    //     `extraLineBuffers` line buffers to the pool (the tile draws once per step besides the frame's lines).
    //   encodeTileLines(): clears `tile` (R8Unorm, mip level 0) and draws `segments` (tile pixels) with the line
    //     shaders in the analytic AA mode (line_fs, one over-blend per segment, as reforce.LineAA = analytic).
    //   previousIntensity(): the latest intensity frame (`intensity_[current_]`), which the next encodeStep reads as
    //     its flow source; pending format converts are flushed and the buffer is cleared first if nothing was
    //     drawn yet, so it always holds a defined frame.
    void enableTileLines(int extraLineBuffers = 8);
    void encodeTileLines(MTL::CommandBuffer* cmd, MTL::Texture* tile, const engine::LineSegment* segments,
                         std::size_t count);
    MTL::Texture* previousIntensity(MTL::CommandBuffer* cmd);
    // Movie / live sprite (Phase 5F): a handle whose texture is a BGRA video
    // frame set every step (setSpriteFrame; not owned, nothing allocated). The
    // shader takes the luminance (inverted when `invert`) as value and alpha.
    int addVideoSpriteTexture(bool invert, const VideoLevels& levels = {});
    void setSpriteFrame(int handle, MTL::Texture* frame);
    // Colorize the latest intensity frame into `output` (BGRA8, width x height),
    // with the color video layer composited when `video` has a source, then the
    // full-color movie sprites (PLAN 5K, reforce.MovieSprite.Color = over): each
    // quad's video sprite texture (addVideoSpriteTexture: its levels and '~' invert,
    // per channel) alpha-over at the quad's rectangle with alpha = quad.opacity, in
    // order. The output's alpha stays 1; with `trackAlpha` (reforce.Syphon.Alpha) it is 0 instead, raised by
    // the opaque quads (SpriteQuad::opaque) and later the dancers: how much encodeSyphonAlpha keeps opaque.
    // `opaqueSolids` (with trackAlpha only): 3D solid quads with ColorKey = 0; their coverage x opacity raises the
    // alpha (max), so the whole solid stays opaque even where its colors are dark.
    // `colorSolids`: rubik solids drawn over the frame in color from their targets (solid_color_fs: sticker and
    // plastic colors mixed by their shares, so borders anti-alias in color); SpriteQuad::opaque = ColorKey 0.
    void encodeColorize(MTL::CommandBuffer* cmd, const engine::Palette& palette, const engine::MapLut& lut,
                        MTL::Texture* output, const VideoLayerDraw* video = nullptr,
                        const SpriteQuad* colorQuads = nullptr, std::size_t colorCount = 0, bool trackAlpha = false,
                        const SpriteQuad* opaqueSolids = nullptr, std::size_t opaqueCount = 0,
                        const SpriteQuad* colorSolids = nullptr, std::size_t colorSolidCount = 0);
    // The Syphon output (docs/syphon-alpha.md): `frame` (colorized with trackAlpha) into `output` with its alpha
    // = opaque + (1 - opaque) * key, key = OBS's Color Key on black (similarity / smoothness 0..1 = OBS's / 1000).
    void encodeSyphonAlpha(MTL::CommandBuffer* cmd, MTL::Texture* frame, MTL::Texture* output, float similarity,
                           float smoothness, bool premultiply);
    // Camera backdrop (docs/backdrop.md): in place on `frame` (colorized with trackAlpha), `video` shows through where
    // the Syphon alpha (same key) would be transparent, fitted by `uv`, mirrored, times `opacity`; alpha becomes 1.
    // `video` null: the see-through parts go black. `body` (docs/body.md): the person mask cuts the visuals by
    // `front`; `show` tints the mask and marks the joints.
    void encodeBackdrop(MTL::CommandBuffer* cmd, MTL::Texture* frame, MTL::Texture* video,
                        const engine::UvTransform& uv, bool mirror, float opacity, float similarity, float smoothness,
                        const BackdropBody* body = nullptr);
    // The silhouette into the newest intensity frame (docs/body.md), in place: mode 1 fill, 2 outline, 3 motion
    // (against prevMask); I = max(I, amount * x). The masks are R8, in camera coordinates (mapped like the backdrop).
    void encodeBodyBurn(MTL::CommandBuffer* cmd, MTL::Texture* mask, MTL::Texture* prevMask,
                        const engine::UvTransform& uv, bool mirror, int mode, float amount);

private:
    MTL::Device* device_;
    uint32_t width_, height_;
    float kickZoom_ = 0, kickRotate_ = 0;
    ScenePipelineOptions opts_;
    MTL::Texture* intensity_[2] = {nullptr, nullptr};
    int current_ = 0;  // index of the latest intensity frame
    bool initialized_ = false;  // current intensity cleared or seeded
    std::shared_ptr<std::atomic<std::uint64_t>> completedSerial_;
    std::uint64_t submittedSerial_ = 0;
    void markSubmitted(MTL::CommandBuffer* cmd);
    std::shared_ptr<TexturePool> fieldPool_, maskPool_;
    TextureHandle field_[2];  // current/from, incoming
    TextureHandle mask_;
    MTL::Texture* dummyMask_ = nullptr;
    struct Bake {
        TextureHandle a, b, mask, out;
        float w = 0;
        int mode = 0;
        bool useMask = false;
    };
    std::vector<Bake> bakes_;  // pending, encoded by the next encodeStep
    MTL::RenderPipelineState* bakePso_ = nullptr;
    MTL::RenderPipelineState* flowPso_ = nullptr;
    MTL::RenderPipelineState* linePso_ = nullptr;
    // Per-path line AA: flow + lines with a memoryless scratch attachment.
    MTL::RenderPipelineState* flowPathPso_ = nullptr;
    MTL::RenderPipelineState* linePathPso_ = nullptr;
    MTL::Texture* scratch_ = nullptr;
    // 4x MSAA comparison: memoryless multisample intensity, resolved into the frame.
    MTL::RenderPipelineState* flowMsaaPso_ = nullptr;
    MTL::RenderPipelineState* lineMsaaPso_ = nullptr;
    MTL::Texture* msaa_ = nullptr;
    float samplePos_[4][2] = {};
    // Pipelines that depend on the intensity format (index = IntensityFormat);
    // the *Pso_ members above / below point into the active one.
    struct FormatSet {
        bool ready = false;
        MTL::RenderPipelineState *flow = nullptr, *line = nullptr, *flowPath = nullptr, *linePath = nullptr,
                                 *flowMsaa = nullptr, *lineMsaa = nullptr, *noise = nullptr, *copy = nullptr;
        MTL::RenderPipelineState* sprite[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
        MTL::RenderPipelineState* bodyBurn = nullptr;
        MTL::Texture* msaa = nullptr;
        void release();
    };
    FormatSet sets_[2];
    std::future<FormatSet> warm_;  // prewarmOtherFormat
    MTL::Library* lib_ = nullptr;
    FormatSet makeFormatSet(IntensityFormat f) const;
    void bindFormatSet(IntensityFormat f);
    MTL::Texture* newIntensityTexture(IntensityFormat f, const char* label) const;
    // Pending trail copies after a format switch (from: owned, released once encoded).
    struct Convert { MTL::Texture* from; MTL::Texture* to; };
    std::vector<Convert> converts_;
    void flushConverts(MTL::CommandBuffer* cmd);
    MTL::RenderPipelineState* colorizePso_ = nullptr;
    MTL::RenderPipelineState* colorizeVideoPso_ = nullptr;
    MTL::RenderPipelineState* colorSpritePso_ = nullptr;  // full-color movie sprites over the colorized frame
    MTL::RenderPipelineState* colorSpriteOpaquePso_ = nullptr;
    MTL::RenderPipelineState* solidOpaquePso_ = nullptr;
    MTL::RenderPipelineState* solidColorPso_ = nullptr;        // rubik in color over the colorized frame
    MTL::RenderPipelineState* solidColorOpaquePso_ = nullptr;  // ... also raising the tracked alpha  // ColorKey = 0 solids raising the tracked alpha  // ... raising the tracked Syphon alpha
    MTL::RenderPipelineState* syphonAlphaPso_ = nullptr;
    MTL::RenderPipelineState* backdropPso_ = nullptr;
    MTL::RenderPipelineState* bodyBurnPso_ = nullptr;  // in the active FormatSet
    MTL::RenderPipelineState* noisePso_ = nullptr;
    MTL::RenderPipelineState* tileLinePso_ = nullptr;  // enableTileLines: lines into an R8Unorm tile

    // Reused shared-storage buffers for the per-step line segments (no
    // per-frame Metal allocation). `busy` is cleared when the command buffer
    // that reads it completes.
    struct LineBuffer {
        MTL::Buffer* buf = nullptr;
        std::shared_ptr<std::atomic<bool>> busy;
    };
    std::vector<LineBuffer> lineBuffers_;
    MTL::Buffer* lineBuffer(MTL::CommandBuffer* cmd, const void* data, std::size_t bytes);

    // Sprites
    struct SpriteTex {
        MTL::Texture* tex = nullptr;
        bool hasAlpha = false;
        bool needsMips = false;
        bool video = false;   // BGRA frame, not owned
        bool invert = false;  // video: invert the luminance
        VideoLevels levels;   // video: luminance levels (PLAN 5K)
        bool premultiplied = false;  // 3D solid target (addSolidTexture)
    };
    std::map<int, SpriteTex> spriteTex_;
    int nextSpriteHandle_ = 0;
    MTL::Texture* display_ = nullptr;  // displayed frame while sprites draw (pre-EPen + Pen pass)
    MTL::Texture* edge_ = nullptr;     // color edges: this step's line coverage (pen x cov, cov), made on first use
    bool edgeValid_ = false;
    MTL::RenderPipelineState* lineCoverPso_ = nullptr;
    bool displayValid_ = false;
    MTL::RenderPipelineState* spritePso_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};  // over, add, max, premul, fetch
    void encodeSprites(MTL::CommandBuffer* cmd, MTL::Texture* target, const SpriteQuad* quads, std::size_t count,
                       const char* label);
};

}  // namespace reforce::render
