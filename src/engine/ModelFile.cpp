// reforce extension (PLAN 5R.1, docs/model-design.md): glTF model loading. See ModelFile.hpp.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "engine/ModelFile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace reforce::engine {

namespace {

using Vec3 = std::array<float, 3>;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3& a, const Vec3& fallback = {0, 0, 1}) {
    const float l = length(a);
    return l > 1e-20f && std::isfinite(l) ? scale(a, 1.0f / l) : fallback;
}

// Column-major 4x4 (cgltf_node_transform_world), applied to points and directions.
struct Xform {
    float m[16];
    Vec3 point(const float* p) const {
        return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
    }
    Vec3 dir(const float* d) const {
        return {m[0] * d[0] + m[4] * d[1] + m[8] * d[2], m[1] * d[0] + m[5] * d[1] + m[9] * d[2],
                m[2] * d[0] + m[6] * d[1] + m[10] * d[2]};
    }
    float det3() const {
        return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
               m[8] * (m[1] * m[6] - m[5] * m[2]);
    }
    // Normals: the inverse transpose of the upper 3x3 (up to scale: the cofactor matrix), then normalized.
    Vec3 normal(const float* n) const {
        const Vec3 c0{m[0], m[1], m[2]}, c1{m[4], m[5], m[6]}, c2{m[8], m[9], m[10]};
        const Vec3 r0 = cross(c1, c2), r1 = cross(c2, c0), r2 = cross(c0, c1);  // cofactor columns
        const Vec3 out = add(add(scale(r0, n[0]), scale(r1, n[1])), scale(r2, n[2]));
        return det3() < 0 ? scale(out, -1.0f) : out;
    }
};

const cgltf_accessor* findAttribute(const cgltf_primitive& prim, cgltf_attribute_type type, int index = 0) {
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        const auto& a = prim.attributes[i];
        if (a.type == type && a.index == index) return a.data;
    }
    return nullptr;
}

std::vector<float> unpack(const cgltf_accessor* acc, int components) {
    std::vector<float> out;
    if (!acc) return out;
    out.resize(acc->count * std::size_t(components));
    if (cgltf_num_components(acc->type) != cgltf_size(components) ||
        cgltf_accessor_unpack_floats(acc, out.data(), out.size()) != out.size())
        out.clear();
    return out;
}

// Triangle-list indices for a primitive (local to its vertices); empty for non-triangle modes.
std::vector<std::uint32_t> triangleIndices(const cgltf_primitive& prim, std::size_t vertexCount) {
    std::vector<std::uint32_t> raw;
    const std::size_t n = prim.indices ? prim.indices->count : vertexCount;
    raw.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        raw.push_back(prim.indices ? std::uint32_t(cgltf_accessor_read_index(prim.indices, i)) : std::uint32_t(i));
    std::vector<std::uint32_t> tris;
    switch (prim.type) {
    case cgltf_primitive_type_triangles:
        tris.assign(raw.begin(), raw.begin() + std::ptrdiff_t(n - n % 3));
        break;
    case cgltf_primitive_type_triangle_strip:
        for (std::size_t i = 2; i < n; ++i) {
            if (i % 2 == 0) tris.insert(tris.end(), {raw[i - 2], raw[i - 1], raw[i]});
            else tris.insert(tris.end(), {raw[i - 1], raw[i - 2], raw[i]});
        }
        break;
    case cgltf_primitive_type_triangle_fan:
        for (std::size_t i = 2; i < n; ++i) tris.insert(tris.end(), {raw[0], raw[i - 1], raw[i]});
        break;
    default:
        break;
    }
    // Drop triangles with out-of-range indices rather than reading past the vertices.
    std::vector<std::uint32_t> ok;
    ok.reserve(tris.size());
    for (std::size_t i = 0; i + 2 < tris.size(); i += 3)
        if (tris[i] < vertexCount && tris[i + 1] < vertexCount && tris[i + 2] < vertexCount)
            ok.insert(ok.end(), {tris[i], tris[i + 1], tris[i + 2]});
    return ok;
}

void generateNormals(ModelVertex* v, std::size_t count, const std::uint32_t* idx, std::size_t indexCount) {
    std::vector<Vec3> acc(count, Vec3{0, 0, 0});
    for (std::size_t i = 0; i + 2 < indexCount; i += 3) {
        const Vec3 a{v[idx[i]].pos[0], v[idx[i]].pos[1], v[idx[i]].pos[2]};
        const Vec3 b{v[idx[i + 1]].pos[0], v[idx[i + 1]].pos[1], v[idx[i + 1]].pos[2]};
        const Vec3 c{v[idx[i + 2]].pos[0], v[idx[i + 2]].pos[1], v[idx[i + 2]].pos[2]};
        const Vec3 n = cross(sub(b, a), sub(c, a));  // area-weighted
        for (int k = 0; k < 3; ++k) acc[idx[i + std::size_t(k)]] = add(acc[idx[i + std::size_t(k)]], n);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3 n = normalized(acc[i]);
        std::copy(n.begin(), n.end(), v[i].normal);
    }
}

void generateTangents(ModelVertex* v, std::size_t count, const std::uint32_t* idx, std::size_t indexCount) {
    std::vector<Vec3> tan(count, Vec3{0, 0, 0}), bit(count, Vec3{0, 0, 0});
    for (std::size_t i = 0; i + 2 < indexCount; i += 3) {
        const ModelVertex &a = v[idx[i]], &b = v[idx[i + 1]], &c = v[idx[i + 2]];
        const Vec3 e1{b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
        const Vec3 e2{c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
        const float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1];
        const float du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
        const float d = du1 * dv2 - du2 * dv1;
        if (std::fabs(d) < 1e-20f) continue;
        const float r = 1.0f / d;
        const Vec3 t = scale(sub(scale(e1, dv2), scale(e2, dv1)), r);
        const Vec3 bt = scale(sub(scale(e2, du1), scale(e1, du2)), r);
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t j = idx[i + std::size_t(k)];
            tan[j] = add(tan[j], t);
            bit[j] = add(bit[j], bt);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3 n{v[i].normal[0], v[i].normal[1], v[i].normal[2]};
        Vec3 t = sub(tan[i], scale(n, dot(n, tan[i])));  // Gram-Schmidt
        if (length(t) < 1e-12f) {
            // No usable UV gradient: any unit vector perpendicular to the normal.
            t = cross(std::fabs(n[0]) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}, n);
        }
        t = normalized(t, {1, 0, 0});
        const float w = dot(cross(n, t), bit[i]) < 0 ? -1.0f : 1.0f;
        v[i].tangent[0] = t[0];
        v[i].tangent[1] = t[1];
        v[i].tangent[2] = t[2];
        v[i].tangent[3] = w;
    }
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

struct Loader {
    const cgltf_data* data;
    std::filesystem::path baseDir;
    const ModelLoadOptions& opts;
    Model model;
    std::unordered_map<const cgltf_image*, int> imageIndex;

    int texture(const cgltf_texture_view& view, bool srgb) {
        if (!view.texture || !view.texture->image) return -1;
        const cgltf_image* img = view.texture->image;
        if (auto it = imageIndex.find(img); it != imageIndex.end()) {
            if (srgb) model.textures[std::size_t(it->second)].srgb = true;
            return it->second;
        }
        ModelTexture t;
        t.name = img->name ? img->name : (img->uri && std::string_view(img->uri).rfind("data:", 0) != 0 ? img->uri : "");
        t.mime = img->mime_type ? img->mime_type : "";
        t.srgb = srgb;
        if (img->buffer_view) {
            const auto* p = static_cast<const std::uint8_t*>(cgltf_buffer_view_data(img->buffer_view));
            if (!p) {
                model.warnings.push_back("image without buffer data: " + t.name);
                return -1;
            }
            t.bytes.assign(p, p + img->buffer_view->size);
        } else if (img->uri && std::string_view(img->uri).rfind("data:", 0) == 0) {
            model.warnings.push_back("data-URI image not supported (use a .glb): " + t.name);
            return -1;
        } else if (img->uri) {
            std::string uri = img->uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            t.path = baseDir / uri;
        } else {
            return -1;
        }
        const int index = int(model.textures.size());
        model.textures.push_back(std::move(t));
        imageIndex[img] = index;
        return index;
    }

    void materials() {
        for (cgltf_size i = 0; i < data->materials_count; ++i) {
            const cgltf_material& src = data->materials[i];
            ModelMaterial m;
            m.name = src.name ? src.name : "";
            if (src.has_pbr_metallic_roughness) {
                const auto& pbr = src.pbr_metallic_roughness;
                std::copy(pbr.base_color_factor, pbr.base_color_factor + 4, m.baseColor.begin());
                m.metallic = pbr.metallic_factor;
                m.roughness = pbr.roughness_factor;
                m.baseColorTex = texture(pbr.base_color_texture, true);
                m.metallicRoughnessTex = texture(pbr.metallic_roughness_texture, false);
            }
            const float strength = src.has_emissive_strength ? src.emissive_strength.emissive_strength : 1.0f;
            for (int k = 0; k < 3; ++k) m.emissive[std::size_t(k)] = src.emissive_factor[k] * strength;
            m.emissiveTex = texture(src.emissive_texture, true);
            m.normalTex = texture(src.normal_texture, false);
            if (src.normal_texture.texture) m.normalScale = src.normal_texture.scale;
            m.alpha = src.alpha_mode == cgltf_alpha_mode_mask    ? ModelAlpha::Mask
                      : src.alpha_mode == cgltf_alpha_mode_blend ? ModelAlpha::Blend
                                                                 : ModelAlpha::Opaque;
            m.alphaCutoff = src.alpha_cutoff;
            m.doubleSided = src.double_sided;
            model.materials.push_back(std::move(m));
        }
    }

    void primitive(const cgltf_primitive& prim, const Xform& xf, const std::string& meshName) {
        const cgltf_accessor* posAcc = findAttribute(prim, cgltf_attribute_type_position);
        if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip &&
            prim.type != cgltf_primitive_type_triangle_fan) {
            model.warnings.push_back("skipped a non-triangle primitive in mesh " + meshName);
            return;
        }
        const auto pos = unpack(posAcc, 3);
        if (pos.empty()) {
            model.warnings.push_back("skipped a primitive without positions in mesh " + meshName);
            return;
        }
        const std::size_t count = pos.size() / 3;
        const auto nrm = unpack(findAttribute(prim, cgltf_attribute_type_normal), 3);
        const auto tng = unpack(findAttribute(prim, cgltf_attribute_type_tangent), 4);
        const auto uv = unpack(findAttribute(prim, cgltf_attribute_type_texcoord), 2);
        const bool hasN = nrm.size() == count * 3, hasT = hasN && tng.size() == count * 4, hasUV = uv.size() == count * 2;

        auto tris = triangleIndices(prim, count);
        if (tris.empty()) return;
        const bool mirrored = xf.det3() < 0;
        if (mirrored)
            for (std::size_t i = 0; i + 2 < tris.size(); i += 3) std::swap(tris[i + 1], tris[i + 2]);

        const std::size_t base = model.vertices.size();
        model.vertices.resize(base + count);
        ModelVertex* v = model.vertices.data() + base;
        for (std::size_t i = 0; i < count; ++i) {
            const Vec3 p = xf.point(&pos[i * 3]);
            std::copy(p.begin(), p.end(), v[i].pos);
            if (hasN) {
                const Vec3 n = normalized(xf.normal(&nrm[i * 3]));
                std::copy(n.begin(), n.end(), v[i].normal);
            }
            if (hasT) {
                const Vec3 t = normalized(xf.dir(&tng[i * 4]), {1, 0, 0});
                std::copy(t.begin(), t.end(), v[i].tangent);
                v[i].tangent[3] = (tng[i * 4 + 3] < 0 ? -1.0f : 1.0f) * (mirrored ? -1.0f : 1.0f);
            }
            v[i].uv[0] = hasUV ? uv[i * 2] : 0.0f;
            v[i].uv[1] = hasUV ? uv[i * 2 + 1] : 0.0f;
        }
        if (!hasN) generateNormals(v, count, tris.data(), tris.size());
        if (!hasT) generateTangents(v, count, tris.data(), tris.size());

        ModelPrimitive out;
        out.firstIndex = std::uint32_t(model.indices.size());
        out.indexCount = std::uint32_t(tris.size());
        out.material = prim.material ? int(prim.material - data->materials) : -1;
        for (auto i : tris) model.indices.push_back(std::uint32_t(base) + i);
        model.primitives.push_back(out);
    }

    void node(const cgltf_node* n) {
        if (n->mesh) {
            Xform xf{};
            cgltf_node_transform_world(n, xf.m);
            const std::string name = n->mesh->name ? n->mesh->name : (n->name ? n->name : "");
            for (cgltf_size i = 0; i < n->mesh->primitives_count; ++i) primitive(n->mesh->primitives[i], xf, name);
        }
        for (cgltf_size i = 0; i < n->children_count; ++i) node(n->children[i]);
    }

    bool build(std::string* error) {
        materials();
        const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) node(scene->nodes[i]);
        } else {
            for (cgltf_size i = 0; i < data->nodes_count; ++i)
                if (!data->nodes[i].parent) node(&data->nodes[i]);
        }
        if (model.indices.empty()) {
            if (error) *error = "no triangle meshes in the model";
            return false;
        }
        return normalize(error);
    }

    bool normalize(std::string* error) {
        Vec3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
        for (const auto& v : model.vertices)
            for (int k = 0; k < 3; ++k) {
                lo[std::size_t(k)] = std::min(lo[std::size_t(k)], v.pos[k]);
                hi[std::size_t(k)] = std::max(hi[std::size_t(k)], v.pos[k]);
            }
        const Vec3 center = scale(add(lo, hi), 0.5f);
        float r = 0;
        for (const auto& v : model.vertices) r = std::max(r, length(sub(Vec3{v.pos[0], v.pos[1], v.pos[2]}, center)));
        if (!std::isfinite(r)) {
            if (error) *error = "the model has non-finite positions";
            return false;
        }
        model.sourceCenter = center;
        model.sourceRadius = r;

        if (opts.normalize) {
            const Vec3 front = normalized(opts.front), up = normalized(opts.up);
            if (std::fabs(dot(front, up)) > 1e-3f) {
                if (error) *error = "the front and up axes must be perpendicular";
                return false;
            }
            // Rows of the rotation: x = front x up, y = up, z = -front (so R * front = -z, R * up = +y, right-handed).
            const Vec3 rx = cross(front, up), ry = up, rz = scale(front, -1.0f);
            const float s = r > 0 ? 1.0f / r : 1.0f;
            auto rot = [&](const float* d) { return Vec3{dot(rx, {d[0], d[1], d[2]}), dot(ry, {d[0], d[1], d[2]}),
                                                         dot(rz, {d[0], d[1], d[2]})}; };
            for (auto& v : model.vertices) {
                const Vec3 p0 = sub(Vec3{v.pos[0], v.pos[1], v.pos[2]}, center);
                const Vec3 p = scale(rot(p0.data()), s), n = rot(v.normal), t = rot(v.tangent);
                std::copy(p.begin(), p.end(), v.pos);
                std::copy(n.begin(), n.end(), v.normal);
                std::copy(t.begin(), t.end(), v.tangent);
            }
        }
        model.boundsMin = {INFINITY, INFINITY, INFINITY};
        model.boundsMax = {-INFINITY, -INFINITY, -INFINITY};
        model.radius = 0;
        for (const auto& v : model.vertices) {
            for (int k = 0; k < 3; ++k) {
                model.boundsMin[std::size_t(k)] = std::min(model.boundsMin[std::size_t(k)], v.pos[k]);
                model.boundsMax[std::size_t(k)] = std::max(model.boundsMax[std::size_t(k)], v.pos[k]);
            }
            model.radius = std::max(model.radius, length(Vec3{v.pos[0], v.pos[1], v.pos[2]}));
        }
        return true;
    }
};

const char* resultText(cgltf_result r) {
    switch (r) {
    case cgltf_result_success: return "ok";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "not a glTF file";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
    default: return "error";
    }
}

std::optional<Model> finish(cgltf_data* data, const std::filesystem::path& baseDir, const ModelLoadOptions& opts,
                            std::string* error) {
    if (cgltf_result r = cgltf_validate(data); r != cgltf_result_success) {
        if (error) *error = std::string("invalid model: ") + resultText(r);
        cgltf_free(data);
        return std::nullopt;
    }
    Loader loader{data, baseDir, opts, {}, {}};
    const bool ok = loader.build(error);
    cgltf_free(data);
    if (!ok) return std::nullopt;
    return std::move(loader.model);
}

}  // namespace

bool Model::anyEmissive() const {
    return std::any_of(materials.begin(), materials.end(), [](const ModelMaterial& m) { return m.emits(); });
}

std::optional<std::array<float, 3>> parseModelAxis(std::string_view text) {
    std::string s = lower(text);
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
            s.end());
    float sign = 1;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        sign = s[0] == '-' ? -1.0f : 1.0f;
        s.erase(0, 1);
    }
    if (s == "x") return std::array<float, 3>{sign, 0, 0};
    if (s == "y") return std::array<float, 3>{0, sign, 0};
    if (s == "z") return std::array<float, 3>{0, 0, sign};
    return std::nullopt;
}

std::optional<Model> loadModel(const std::filesystem::path& path, const ModelLoadOptions& opts, std::string* error) {
    cgltf_options o{};
    cgltf_data* data = nullptr;
    const std::string p = path.string();
    if (cgltf_result r = cgltf_parse_file(&o, p.c_str(), &data); r != cgltf_result_success) {
        if (error) *error = p + ": " + resultText(r);
        return std::nullopt;
    }
    if (cgltf_result r = cgltf_load_buffers(&o, data, p.c_str()); r != cgltf_result_success) {
        if (error) *error = p + ": buffers: " + resultText(r);
        cgltf_free(data);
        return std::nullopt;
    }
    return finish(data, path.parent_path(), opts, error);
}

std::optional<Model> loadModelFromMemory(const void* bytes, std::size_t size, const ModelLoadOptions& opts,
                                         std::string* error) {
    cgltf_options o{};
    cgltf_data* data = nullptr;
    if (cgltf_result r = cgltf_parse(&o, bytes, size, &data); r != cgltf_result_success) {
        if (error) *error = resultText(r);
        return std::nullopt;
    }
    if (cgltf_result r = cgltf_load_buffers(&o, data, nullptr); r != cgltf_result_success) {
        if (error) *error = std::string("buffers: ") + resultText(r);
        cgltf_free(data);
        return std::nullopt;
    }
    return finish(data, {}, opts, error);
}

}  // namespace reforce::engine
