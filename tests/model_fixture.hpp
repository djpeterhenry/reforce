#pragma once
// Test models for 3D model Objects (PLAN 5R, docs/model-design.md): a small glTF binary (.glb) built in memory, so
// no third-party model is ever committed. Header-only; used by the engine and render tests.
//
// The default model is a "hull": a box 2 long (x) x .5 high (y) x 1 wide (z) whose front is +x, placed by a node with
// translation (10, 0, 0) and scale 2 (so normalization has something to undo). Material 0 has a base color texture
// (flat `base` color), a flat normal map and an emissive texture: black with a bright stripe across the middle
// (v .4 .. .6) of every face, emissive factor 1. A second node holds one triangle without normals (they are
// generated) under a mirroring scale (-1, 1, 1) with the untextured material 1, and a points primitive that the
// loader skips.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reforce::testmodel {

inline std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t c = 0) {
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// An uncompressed (stored-deflate) RGBA PNG.
inline std::vector<std::uint8_t> makePng(int w, int h, const std::vector<std::array<std::uint8_t, 4>>& px) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < w; ++x) {
            const auto& c = px[std::size_t(y * w + x)];
            raw.insert(raw.end(), c.begin(), c.end());
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::size_t off = 0;
    do {
        const std::size_t len = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = off + len == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(std::uint8_t(len & 0xff));
        z.push_back(std::uint8_t(len >> 8));
        z.push_back(std::uint8_t(~len & 0xff));
        z.push_back(std::uint8_t((~len >> 8) & 0xff));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(off), raw.begin() + std::ptrdiff_t(off + len));
        off += len;
    } while (off < raw.size());
    std::uint32_t a = 1, b = 0;
    for (auto v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(std::uint8_t(adler >> s));

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        const std::uint32_t n = std::uint32_t(data.size());
        for (int s = 24; s >= 0; s -= 8) png.push_back(std::uint8_t(n >> s));
        std::vector<std::uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        png.insert(png.end(), td.begin(), td.end());
        const std::uint32_t c = crc32(td.data(), td.size());
        for (int s = 24; s >= 0; s -= 8) png.push_back(std::uint8_t(c >> s));
    };
    std::vector<std::uint8_t> ihdr;
    for (std::uint32_t v : {std::uint32_t(w), std::uint32_t(h)})
        for (int s = 24; s >= 0; s -= 8) ihdr.push_back(std::uint8_t(v >> s));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

struct Options {
    std::array<std::uint8_t, 4> base{200, 120, 40, 255};
    int textureSize = 16;
    bool emissive = true;   // false: no emissive texture or factor
    bool mirrorNode = true; // include the mirrored triangle node and the points primitive
};

struct Glb {
    std::vector<std::uint8_t> bytes;
};

inline Glb makeGlb(const Options& o = {}) {
    std::vector<std::uint8_t> bin;
    auto align4 = [&] { while (bin.size() % 4) bin.push_back(0); };
    auto put = [&](const void* p, std::size_t n) {
        align4();
        const std::size_t off = bin.size();
        bin.insert(bin.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
        return off;
    };

    // Box: 6 faces x 4 vertices, half sizes 1 x .25 x .5.
    const float hx = 1, hy = .25f, hz = .5f;
    std::vector<float> pos, nrm, uv;
    std::vector<std::uint16_t> idx;
    const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        const float* n = faces[f];
        // Two tangent axes u, v with cross(u, v) = n.
        float u[3], v[3];
        if (std::fabs(n[1]) > .5f) { u[0] = 1; u[1] = 0; u[2] = 0; } else { u[0] = 0; u[1] = 1; u[2] = 0; }
        // v = n x u, then u = v x n, so (u, v, n) is right-handed.
        v[0] = n[1] * u[2] - n[2] * u[1]; v[1] = n[2] * u[0] - n[0] * u[2]; v[2] = n[0] * u[1] - n[1] * u[0];
        u[0] = v[1] * n[2] - v[2] * n[1]; u[1] = v[2] * n[0] - v[0] * n[2]; u[2] = v[0] * n[1] - v[1] * n[0];
        const std::uint16_t b = std::uint16_t(pos.size() / 3);
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto& c : corners) {
            const float p[3] = {(n[0] + c[0] * u[0] + c[1] * v[0]) * hx, (n[1] + c[0] * u[1] + c[1] * v[1]) * hy,
                                (n[2] + c[0] * u[2] + c[1] * v[2]) * hz};
            pos.insert(pos.end(), p, p + 3);
            nrm.insert(nrm.end(), n, n + 3);
            uv.push_back(.5f + .5f * c[0]);
            uv.push_back(.5f - .5f * c[1]);  // glTF v runs down the image
        }
        idx.insert(idx.end(), {b, std::uint16_t(b + 1), std::uint16_t(b + 2), b, std::uint16_t(b + 2),
                               std::uint16_t(b + 3)});
    }
    const std::size_t posOff = put(pos.data(), pos.size() * 4), nrmOff = put(nrm.data(), nrm.size() * 4),
                      uvOff = put(uv.data(), uv.size() * 4), idxOff = put(idx.data(), idx.size() * 2);
    const float tri[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::size_t triOff = put(tri, sizeof tri);

    const int ts = o.textureSize;
    std::vector<std::array<std::uint8_t, 4>> basePx(std::size_t(ts * ts), o.base),
        normalPx(std::size_t(ts * ts), {128, 128, 255, 255}), emitPx(std::size_t(ts * ts), {0, 0, 0, 255});
    for (int y = 0; y < ts; ++y)
        for (int x = 0; x < ts; ++x)
            if (y >= ts * 4 / 10 && y < ts * 6 / 10) emitPx[std::size_t(y * ts + x)] = {255, 255, 255, 255};
    const auto pngBase = makePng(ts, ts, basePx), pngNormal = makePng(ts, ts, normalPx),
               pngEmit = makePng(ts, ts, emitPx);
    const std::size_t baseOff = put(pngBase.data(), pngBase.size()), normalOff = put(pngNormal.data(), pngNormal.size()),
                      emitOff = put(pngEmit.data(), pngEmit.size());
    align4();

    auto view = [](std::size_t off, std::size_t len) {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len) + "}";
    };
    std::string views = "[" + view(posOff, pos.size() * 4) + "," + view(nrmOff, nrm.size() * 4) + "," +
                        view(uvOff, uv.size() * 4) + "," + view(idxOff, idx.size() * 2) + "," +
                        view(triOff, sizeof tri) + "," + view(baseOff, pngBase.size()) + "," +
                        view(normalOff, pngNormal.size()) + "," + view(emitOff, pngEmit.size()) + "]";
    const std::string accessors =
        "[{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\",\"min\":[-1,-0.25,-0.5],"
        "\"max\":[1,0.25,0.5]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]}]";
    const std::string emissive = o.emissive ? ",\"emissiveTexture\":{\"index\":2},\"emissiveFactor\":[1,1,1]" : "";
    const std::string materials =
        "[{\"name\":\"Hull\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,"
        "\"roughnessFactor\":0.6},\"normalTexture\":{\"index\":1,\"scale\":0.8}" + emissive + "},"
        "{\"name\":\"Plain\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.5,0.5,1]},\"doubleSided\":true}]";
    std::string meshes = "[{\"name\":\"Hull\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
                         "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}";
    std::string nodes = "[{\"name\":\"Hull\",\"mesh\":0,\"translation\":[10,0,0],\"scale\":[2,2,2]}";
    std::string sceneNodes = "[0";
    if (o.mirrorNode) {
        meshes += ",{\"name\":\"Fin\",\"primitives\":[{\"attributes\":{\"POSITION\":4},\"material\":1},"
                  "{\"attributes\":{\"POSITION\":4},\"mode\":0}]}";
        nodes += ",{\"name\":\"Fin\",\"mesh\":1,\"translation\":[10,0.5,0],\"scale\":[-1,1,1]}";
        sceneNodes += ",1";
    }
    meshes += "]";
    nodes += "]";
    sceneNodes += "]";
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":" + sceneNodes +
                       "}],\"nodes\":" + nodes + ",\"meshes\":" + meshes + ",\"materials\":" + materials +
                       ",\"textures\":[{\"source\":0},{\"source\":1},{\"source\":2}],"
                       "\"images\":[{\"name\":\"base\",\"bufferView\":5,\"mimeType\":\"image/png\"},"
                       "{\"name\":\"normal\",\"bufferView\":6,\"mimeType\":\"image/png\"},"
                       "{\"name\":\"emit\",\"bufferView\":7,\"mimeType\":\"image/png\"}],"
                       "\"accessors\":" + accessors + ",\"bufferViews\":" + views +
                       ",\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
    while (json.size() % 4) json.push_back(' ');

    Glb g;
    auto u32 = [&](std::uint32_t v) { for (int s = 0; s < 32; s += 8) g.bytes.push_back(std::uint8_t(v >> s)); };
    const std::uint32_t total = 12 + 8 + std::uint32_t(json.size()) + 8 + std::uint32_t(bin.size());
    u32(0x46546C67);  // "glTF"
    u32(2);
    u32(total);
    u32(std::uint32_t(json.size()));
    u32(0x4E4F534A);  // "JSON"
    g.bytes.insert(g.bytes.end(), json.begin(), json.end());
    u32(std::uint32_t(bin.size()));
    u32(0x004E4942);  // "BIN"
    g.bytes.insert(g.bytes.end(), bin.begin(), bin.end());
    return g;
}

}  // namespace reforce::testmodel
