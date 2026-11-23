#pragma once
// reforce extension (PLAN 5R.2 / 5R.4, docs/model-design.md): the GPU side of 3D model Objects (Mesh="model").
//
// A model (engine::Model, loaded from the user's own .glb) is uploaded once by upload(), which may run on any thread
// (the app's loader worker): vertex / index buffers, every texture decoded (render/ImageFiles), capped to
// `maxTexture`, with mips. The returned ModelGpu is immutable and shared; the render thread only binds it.
//
// Two looks, both drawn into the model's solid slot target (render/SolidPipeline's RGBA8 targets, its MSAA color and
// depth scratch), one pass at a time:
//   - Look A, encodeLuminance(): the model as luminance (model_lum_fs: the base texture's brightness, a key light
//     and a rim, the emissive texture lifting it to at least Glow x its brightness), returned as a SolidQuad that the
//     app composites through the sprite pass with the solid's Pen / EPen / Flag, like every solid. So it trails and
//     takes the ColorMap.
//   - Look B, encodeColor(): the model in its own colors over the colorized frame (model_color_fs: base color and
//     normal maps, metallic-roughness, a warm key, a cool fill from below, a rim from behind, emissive x Glow, ACES
//     tone curve, Tint towards a palette color), its emissive light also drawn into a second target, blurred at
//     half size and added as bloom (Blom; skipped when the model has no emissive material or Blom is 0). Then
//     composited over the frame, premultiplied (ColorKey = 0 also raises the tracked alpha).
//
// The camera is engine::ModelShot (engine/CameraMoves): eye, aim, roll and fov in normalized model units (radius 1,
// front -z, top +y); the model's own spin and bank are applied by the model matrix.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/CameraMoves.hpp"
#include "engine/ModelFile.hpp"
#include "render/SolidPipeline.hpp"

namespace MTL {
class Device; class Texture; class CommandBuffer; class CommandQueue; class RenderPipelineState;
class DepthStencilState; class Buffer; class SamplerState;
}

namespace reforce::render {

// An uploaded model. Created by ModelPipeline::upload, released with the last shared_ptr.
class ModelGpu {
public:
    ~ModelGpu();
    ModelGpu(const ModelGpu&) = delete;
    ModelGpu& operator=(const ModelGpu&) = delete;

    std::size_t triangles() const { return triangles_; }
    std::size_t textureBytes() const { return textureBytes_; }
    bool emissive() const { return emissive_; }
    const std::array<float, 3>& boundsMin() const { return bmin_; }
    const std::array<float, 3>& boundsMax() const { return bmax_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    friend class ModelPipeline;
    ModelGpu() = default;
    struct Material {
        float baseColor[4]{1, 1, 1, 1};
        float emissive[3]{0, 0, 0};
        float metallic = 1, roughness = 1, normalScale = 1, alphaCutoff = 0.5f;
        bool mask = false, doubleSided = false;
        int base = -1, normal = -1, emission = -1, mr = -1;  // into textures_
    };
    struct Primitive {
        std::uint32_t firstIndex = 0, indexCount = 0;
        int material = -1;
    };
    MTL::Buffer* vertices_ = nullptr;
    MTL::Buffer* indices_ = nullptr;
    std::vector<MTL::Texture*> textures_;
    std::vector<Material> materials_;
    std::vector<Primitive> primitives_;
    std::size_t triangles_ = 0, textureBytes_ = 0;
    bool emissive_ = false;
    std::array<float, 3> bmin_{}, bmax_{};
    std::vector<std::string> warnings_;
};

// One model for one step.
struct ModelDraw {
    std::shared_ptr<const ModelGpu> model;
    engine::ModelShot shot;
    double spin = 0, bank = 0;  // radians: about the model's up axis, about its front axis
    int slot = -1;              // the solid slot whose target it draws into
    // Look A
    float fill = 0.15f, shade = 0.6f;
    // Both
    float glow = 1.0f;
    std::array<float, 3> light{-0.45f, 0.6f, 0.65f};  // towards the key light, camera space
    // Look B
    float bloom = 0.6f, exposure = 1.0f, tint = 0.0f;
    std::array<float, 3> tintColor{1, 1, 1};  // sRGB 0..1 (a ColorMap color)
    float opacity = 1.0f;
    bool opaque = false;  // ColorKey = 0: raises the tracked alpha by the coverage
};

class ModelPipeline {
public:
    // Shares `solids`' slot targets and MSAA / depth scratch; `solids` must outlive this.
    explicit ModelPipeline(SolidPipeline& solids);
    ~ModelPipeline();
    ModelPipeline(const ModelPipeline&) = delete;
    ModelPipeline& operator=(const ModelPipeline&) = delete;

    // Any thread. Textures larger than maxTexture on a side are halved until they fit. Null (and *error) on
    // failure. Blocks until the mipmaps are built.
    std::shared_ptr<const ModelGpu> upload(const engine::Model& model, int maxTexture = 2048,
                                           std::string* error = nullptr) const;

    // Look A into d.slot's target; composite the returned quad with the sprite pass. Encode before that step's
    // sprite pass.
    SolidQuad encodeLuminance(MTL::CommandBuffer* cmd, const ModelDraw& d);
    // Look B over `output` (BGRA8, the render size; the colorized frame). `trackAlpha`: the Syphon alpha is tracked.
    void encodeColor(MTL::CommandBuffer* cmd, MTL::Texture* output, const ModelDraw& d, bool trackAlpha);

    // The matrices both passes use (tests): model and view-projection for the frame's aspect.
    static engine::Mat4 modelMatrix(const ModelDraw& d);
    engine::Mat4 viewProjection(const ModelDraw& d) const;

private:
    void drawModel(MTL::CommandBuffer* cmd, const ModelDraw& d, bool color, int px0, int py0, int bw, int bh,
                   const char* label);

    SolidPipeline& solids_;
    MTL::Device* device_;
    MTL::CommandQueue* uploadQueue_ = nullptr;  // upload(): mipmap blits off the render queue
    std::uint32_t width_, height_;
    int msaa_;
    MTL::RenderPipelineState* lumPso_ = nullptr;
    MTL::RenderPipelineState* colorPso_ = nullptr;
    MTL::RenderPipelineState* downPso_ = nullptr;
    MTL::RenderPipelineState* blurPso_ = nullptr;
    MTL::RenderPipelineState* compositePso_ = nullptr;
    MTL::RenderPipelineState* compositeOpaquePso_ = nullptr;
    MTL::DepthStencilState* depth_ = nullptr;
    MTL::SamplerState* sampler_ = nullptr;
    MTL::Texture* white_ = nullptr;      // default base / emissive / metallic-roughness
    MTL::Texture* flatNormal_ = nullptr; // default normal map
    MTL::Texture* emitMsaa_ = nullptr;   // memoryless MSAA second color (look B's emissive)
    MTL::Texture* emit_ = nullptr;       // its resolve, full size
    MTL::Texture* halfA_ = nullptr;      // bloom: half-size down + blur
    MTL::Texture* halfB_ = nullptr;
    MTL::Texture* black_ = nullptr;      // 1x1: no bloom
};

}  // namespace reforce::render
