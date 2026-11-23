#pragma once
// reforce extension (PLAN 5R.1, docs/model-design.md): loads a user-supplied glTF 2.0 model (.glb or .gltf) for the
// model Object. Pure C++ (cgltf; no Metal): the render side uploads the vertices, indices and textures.
//
// - Every triangle primitive of every node in the default scene (or all root nodes when there is no scene) is baked
//   with its node's world transform into one vertex / index list; points and lines are skipped, strips and fans are
//   turned into triangle lists. Mirrored nodes (negative determinant) get their winding flipped, so triangles always
//   wind counter-clockwise from outside, like engine/Meshes.
// - Missing normals are generated (area-weighted smooth normals per primitive); missing tangents are generated from
//   the UVs (per-triangle, accumulated, Gram-Schmidt); missing UVs are 0.
// - Normalization (ModelLoadOptions::normalize): the model is centered on its bounding box, turned so its front points
//   along -z and its top along +y, and scaled so the bounding sphere around that center has radius 1. Every camera
//   position in docs/model-design.md is in these units.
// - Textures are not decoded here: a ModelTexture holds the encoded bytes (embedded in a .glb) or the file path
//   (an external .gltf image), and whether it is color (sRGB) or data (linear).

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reforce::engine {

struct ModelVertex {
    float pos[3];
    float normal[3];
    float tangent[4];  // xyz + handedness w (+-1), glTF convention: bitangent = cross(normal, tangent) * w
    float uv[2];
};
static_assert(sizeof(ModelVertex) == 48);

struct ModelTexture {
    std::string name;
    std::string mime;                  // "image/png", "image/jpeg", ... (may be empty for external files)
    std::vector<std::uint8_t> bytes;   // encoded image, when embedded
    std::filesystem::path path;        // external image file, when not embedded
    bool srgb = true;                  // false: used only as normal / metallic-roughness data
};

enum class ModelAlpha { Opaque, Mask, Blend };

struct ModelMaterial {
    std::string name;
    std::array<float, 4> baseColor{1, 1, 1, 1};  // linear factors
    std::array<float, 3> emissive{0, 0, 0};      // linear, times KHR_materials_emissive_strength
    float metallic = 1.0f, roughness = 1.0f;
    float normalScale = 1.0f;
    float alphaCutoff = 0.5f;
    ModelAlpha alpha = ModelAlpha::Opaque;
    bool doubleSided = false;
    // Indices into Model::textures (-1 = none).
    int baseColorTex = -1, normalTex = -1, emissiveTex = -1, metallicRoughnessTex = -1;
    bool emits() const { return emissiveTex >= 0 || emissive[0] > 0 || emissive[1] > 0 || emissive[2] > 0; }
};

struct ModelPrimitive {
    std::uint32_t firstIndex = 0, indexCount = 0;
    int material = -1;  // index into Model::materials, -1 = default material
};

// An axis given as "+x", "-x", "+y", "-y", "+z" or "-z" (case-insensitive; a missing sign means +).
std::optional<std::array<float, 3>> parseModelAxis(std::string_view text);

struct ModelLoadOptions {
    bool normalize = true;
    std::array<float, 3> front{0, 0, -1};  // the model's forward axis in the file (after node transforms)
    std::array<float, 3> up{0, 1, 0};     // its top
};

struct Model {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;  // triangle list
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    std::array<float, 3> boundsMin{0, 0, 0}, boundsMax{0, 0, 0};  // after normalization
    float radius = 0;                     // bounding sphere radius about the origin (1 when normalized)
    float sourceRadius = 0;               // the same in the file's units, before scaling
    std::array<float, 3> sourceCenter{};  // the bounding-box center in the file's units
    std::vector<std::string> warnings;    // things skipped (non-triangle primitives, data-URI images, ...)

    std::size_t triangleCount() const { return indices.size() / 3; }
    bool anyEmissive() const;
};

// Loads `path`. On failure returns nullopt and sets *error.
std::optional<Model> loadModel(const std::filesystem::path& path, const ModelLoadOptions& opts = {},
                               std::string* error = nullptr);
// The same from memory (a .glb, or a .gltf whose buffers and images are all embedded).
std::optional<Model> loadModelFromMemory(const void* data, std::size_t size, const ModelLoadOptions& opts = {},
                                         std::string* error = nullptr);

}  // namespace reforce::engine
