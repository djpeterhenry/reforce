#include "engine/Meshes.hpp"
#include "engine/Knight.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>

namespace reforce::engine {

namespace {

using V3 = std::array<double, 3>;

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double len(const V3& a) { return std::sqrt(dot(a, a)); }
V3 norm(const V3& a) {
    const double l = len(a);
    return l > 0 ? mul(a, 1.0 / l) : a;
}

constexpr double kPi = 3.14159265358979323846;
const double kPhi = (1.0 + std::sqrt(5.0)) / 2.0;

MeshVertex vert(const V3& p, const V3& n, double u, double v, double edge) {
    MeshVertex m{};
    for (int i = 0; i < 3; ++i) {
        m.pos[i] = float(p[i]);
        m.normal[i] = float(n[i]);
    }
    m.uv[0] = float(u);
    m.uv[1] = float(v);
    m.edge = float(edge);
    return m;
}

// Unit-circumradius copies of the vertex set.
std::vector<V3> normalized(std::vector<V3> v) {
    for (auto& p : v) p = norm(p);
    return v;
}

std::vector<V3> platonicVertices(MeshKind k) {
    std::vector<V3> v;
    switch (k) {
        case MeshKind::Tetrahedron: v = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}}; break;
        case MeshKind::Cube:
            for (int i = 0; i < 8; ++i) v.push_back({i & 1 ? 1.0 : -1.0, i & 2 ? 1.0 : -1.0, i & 4 ? 1.0 : -1.0});
            break;
        case MeshKind::Octahedron: v = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}; break;
        case MeshKind::Icosahedron:
            for (double a : {-1.0, 1.0})
                for (double b : {-kPhi, kPhi}) {
                    v.push_back({0, a, b});
                    v.push_back({a, b, 0});
                    v.push_back({b, 0, a});
                }
            break;
        case MeshKind::Dodecahedron: {
            for (int i = 0; i < 8; ++i) v.push_back({i & 1 ? 1.0 : -1.0, i & 2 ? 1.0 : -1.0, i & 4 ? 1.0 : -1.0});
            const double ip = 1.0 / kPhi;
            for (double a : {-1.0, 1.0})
                for (double b : {-1.0, 1.0}) {
                    v.push_back({0, a * ip, b * kPhi});
                    v.push_back({a * ip, b * kPhi, 0});
                    v.push_back({a * kPhi, 0, b * ip});
                }
            break;
        }
        case MeshKind::Prism: {  // triangular prism, its height about equal to its side
            const double h = 0.75;
            for (int i = 0; i < 3; ++i) {
                const double a = 2 * kPi * i / 3 + kPi / 2;
                v.push_back({std::cos(a), std::sin(a), h});
                v.push_back({std::cos(a), std::sin(a), -h});
            }
            break;
        }
        default: break;
    }
    return normalized(v);
}

// Faces of the convex hull of a small, centered point set: ordered (counter-clockwise from outside) index lists.
std::vector<std::vector<int>> hullFaces(const std::vector<V3>& v) {
    const int n = int(v.size());
    const double eps = 1e-7;
    std::map<std::vector<int>, std::vector<int>> faces;  // sorted set -> ordered
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            for (int k = j + 1; k < n; ++k) {
                V3 nrm = cross(sub(v[j], v[i]), sub(v[k], v[i]));
                if (len(nrm) < 1e-9) continue;
                nrm = norm(nrm);
                double d = dot(nrm, v[i]);
                if (d < 0) {
                    nrm = mul(nrm, -1);
                    d = -d;
                }
                bool ok = true;
                std::vector<int> on;
                for (int q = 0; q < n && ok; ++q) {
                    const double s = dot(nrm, v[q]) - d;
                    if (s > eps) ok = false;
                    else if (s > -eps) on.push_back(q);
                }
                if (!ok || on.size() < 3) continue;
                if (faces.count(on)) continue;
                // order around the face center, counter-clockwise seen from outside (along nrm)
                V3 c{0, 0, 0};
                for (int q : on) c = add(c, v[q]);
                c = mul(c, 1.0 / double(on.size()));
                const V3 ax = norm(sub(v[on[0]], c));
                const V3 ay = cross(nrm, ax);
                std::vector<int> ordered = on;
                std::sort(ordered.begin(), ordered.end(), [&](int a, int b) {
                    const V3 pa = sub(v[a], c), pb = sub(v[b], c);
                    return std::atan2(dot(pa, ay), dot(pa, ax)) < std::atan2(dot(pb, ay), dot(pb, ax));
                });
                faces[on] = ordered;
            }
    std::vector<std::vector<int>> out;
    for (auto& [k, f] : faces) out.push_back(f);
    return out;
}

Mesh polyhedron(MeshKind kind) {
    Mesh m;
    m.kind = kind;
    const std::vector<V3> v = platonicVertices(kind);
    const auto faces = hullFaces(v);
    for (const V3& p : v) m.hullVertices.push_back({float(p[0]), float(p[1]), float(p[2])});
    m.hullFaces = faces;
    for (const auto& f : faces) {
        V3 c{0, 0, 0};
        for (int q : f) c = add(c, v[q]);
        c = mul(c, 1.0 / double(f.size()));
        const V3 n = norm(cross(sub(v[f[1]], v[f[0]]), sub(v[f[2]], v[f[0]])));
        // UVs: the polygon on the tile's inscribed circle, unmirrored seen from outside, with one edge flat at the
        // bottom (squares sit upright, triangles and pentagons point up). The bottom edge is the one facing world
        // down (-y projected into the face; faces normal to y use -z / +z), so at the rest pose the faces read
        // upright. Tile right = cross(up, n); u = 0.5, v = 0 is the top of the tile.
        V3 pref = sub(V3{0, 1, 0}, mul(n, n[1]));
        if (len(pref) < 1e-6) pref = n[1] > 0 ? V3{0, 0, -1} : V3{0, 0, 1};
        V3 up{0, 1, 0};
        double best = 1e30;
        for (std::size_t i = 0; i < f.size(); ++i) {
            const V3 mid = sub(mul(add(v[f[i]], v[f[(i + 1) % f.size()]]), 0.5), c);
            const double d = dot(norm(mid), norm(pref));
            if (d < best - 1e-9) {
                best = d;
                up = norm(mul(mid, -1.0));
            }
        }
        const V3 right = cross(up, n);
        const double r = len(sub(v[f[0]], c));
        auto uv = [&](const V3& p, double& u, double& w) {
            const V3 d = sub(p, c);
            const double px = dot(d, right) / r, py = dot(d, up) / r;
            u = 0.5 + 0.5 * px;
            w = 0.5 - 0.5 * py;
        };
        double cu, cv;
        uv(c, cu, cv);
        for (std::size_t i = 0; i < f.size(); ++i) {
            const V3& a = v[f[i]];
            const V3& b = v[f[(i + 1) % f.size()]];
            double au, av, bu, bv;
            uv(a, au, av);
            uv(b, bu, bv);
            m.vertices.push_back(vert(c, n, cu, cv, 1.0));
            m.vertices.push_back(vert(a, n, au, av, 0.0));
            m.vertices.push_back(vert(b, n, bu, bv, 0.0));
        }
    }
    return m;
}

Mesh sphere(int detail) {
    Mesh m;
    m.kind = MeshKind::Sphere;
    const std::vector<V3> v0 = platonicVertices(MeshKind::Icosahedron);
    std::vector<std::array<V3, 3>> tris;
    for (const auto& f : hullFaces(v0)) tris.push_back({v0[f[0]], v0[f[1]], v0[f[2]]});
    for (int s = 0; s < std::clamp(detail, 0, 5); ++s) {
        std::vector<std::array<V3, 3>> next;
        for (const auto& t : tris) {
            const V3 ab = norm(mul(add(t[0], t[1]), 0.5)), bc = norm(mul(add(t[1], t[2]), 0.5)),
                     ca = norm(mul(add(t[2], t[0]), 0.5));
            next.push_back({t[0], ab, ca});
            next.push_back({ab, t[1], bc});
            next.push_back({ca, bc, t[2]});
            next.push_back({ab, bc, ca});
        }
        tris.swap(next);
    }
    for (const auto& t : tris) {
        // per-triangle u unwrapping across the seam (atan2 jumps from +pi to -pi)
        double us[3], vs[3];
        for (int i = 0; i < 3; ++i) {
            us[i] = 0.5 + std::atan2(t[i][0], t[i][2]) / (2 * kPi);
            vs[i] = std::acos(std::clamp(t[i][1], -1.0, 1.0)) / kPi;
        }
        const double mx = std::max({us[0], us[1], us[2]});
        for (double& u : us)
            if (mx - u > 0.5) u += 1.0;
        for (int i = 0; i < 3; ++i) m.vertices.push_back(vert(t[i], t[i], us[i], vs[i], 1.0));
    }
    return m;
}

Mesh torus(int detail) {
    Mesh m;
    m.kind = MeshKind::Torus;
    m.convex = false;
    const double R = 0.7, r = 0.3;  // circumradius R + r = 1
    const int nu = 24 * std::max(1, detail), nv = 12 * std::max(1, detail);
    auto at = [&](int i, int j, V3& p, V3& n) {
        const double a = 2 * kPi * i / nu, b = 2 * kPi * j / nv;
        n = {std::cos(a) * std::cos(b), std::sin(b), std::sin(a) * std::cos(b)};
        p = {std::cos(a) * (R + r * std::cos(b)), r * std::sin(b), std::sin(a) * (R + r * std::cos(b))};
    };
    for (int i = 0; i < nu; ++i)
        for (int j = 0; j < nv; ++j) {
            V3 p[4], n[4];
            at(i, j, p[0], n[0]);
            at(i + 1, j, p[1], n[1]);
            at(i + 1, j + 1, p[2], n[2]);
            at(i, j + 1, p[3], n[3]);
            const double u0 = double(i) / nu * 4, u1 = double(i + 1) / nu * 4;  // the tile 4 times around
            const double v0 = double(j) / nv, v1 = double(j + 1) / nv;
            const double us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
            // counter-clockwise from outside: (0, 3, 2), (0, 2, 1)
            for (int q : {0, 3, 2, 0, 2, 1}) m.vertices.push_back(vert(p[q], n[q], us[q], vs[q], 1.0));
        }
    return m;
}

// One Rubik cubie (see MeshKind::Rubik): per direction d (+x, -x, +y, -y, +z, -z) the body face (uv (d, 0)) and a
// raised sticker (uv (d, 1)); `edge` runs 0 at the sticker border to 1 at its center.
Mesh cubie() {
    Mesh m;
    m.kind = MeshKind::Rubik;
    m.convex = false;
    auto quad = [&](int d, double offset, double half, double uvY, bool sticker) {
        const int a = d / 2;
        const double s = d % 2 == 0 ? 1.0 : -1.0;
        V3 n{0, 0, 0}, u{0, 0, 0}, v{0, 0, 0};
        n[std::size_t(a)] = s;
        u[std::size_t((a + 1) % 3)] = 1;  // cross(u, v) = +axis
        v[std::size_t((a + 2) % 3)] = 1;
        if (s < 0) std::swap(u, v);
        V3 c[4];
        const double cu[4] = {-1, 1, 1, -1}, cv[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 3; ++j)
                c[k][std::size_t(j)] = n[std::size_t(j)] * offset + (u[std::size_t(j)] * cu[k] + v[std::size_t(j)] * cv[k]) * half;
        // Stickers: a fan around the center so `edge` is 1 there and 0 on the border; body faces: two triangles.
        if (sticker) {
            V3 ctr{n[0] * offset, n[1] * offset, n[2] * offset};
            for (int k = 0; k < 4; ++k) {
                const int k2 = (k + 1) % 4;
                m.vertices.push_back(vert(ctr, n, double(d), uvY, 1.0));
                m.vertices.push_back(vert(c[k], n, double(d), uvY, 0.0));
                m.vertices.push_back(vert(c[k2], n, double(d), uvY, 0.0));
            }
        } else {
            for (int q : {0, 1, 2, 0, 2, 3}) m.vertices.push_back(vert(c[q], n, double(d), uvY, 1.0));
        }
    };
    for (int d = 0; d < 6; ++d) {
        quad(d, 0.5, 0.5, 0.0, false);
        quad(d, 0.503, 0.43, 1.0, true);
    }
    return m;
}

// One 15-puzzle tile (see MeshKind::Fifteen): a unit box, the +z face with uv 0..1, the others uv (-1, -1).
Mesh puzzleTile() {
    Mesh m;
    m.kind = MeshKind::Fifteen;
    m.convex = true;
    for (int d = 0; d < 6; ++d) {
        const int a = d / 2;
        const double s = d % 2 == 0 ? 1.0 : -1.0;
        V3 n{0, 0, 0}, u{0, 0, 0}, v{0, 0, 0};
        n[std::size_t(a)] = s;
        u[std::size_t((a + 1) % 3)] = 1;
        v[std::size_t((a + 2) % 3)] = 1;
        if (s < 0) std::swap(u, v);
        const double cu[4] = {-1, 1, 1, -1}, cv[4] = {-1, -1, 1, 1};
        V3 c[4];
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 3; ++j)
                c[k][std::size_t(j)] = 0.5 * (n[std::size_t(j)] + u[std::size_t(j)] * cu[k] + v[std::size_t(j)] * cv[k]);
        const bool top = d == 4;  // +z: u = +x, v = +y
        for (int q : {0, 1, 2, 0, 2, 3})
            m.vertices.push_back(vert(c[q], n, top ? 0.5 * (cu[q] + 1) : -1.0, top ? 0.5 * (cv[q] + 1) : -1.0, 1.0));
    }
    return m;
}

// One soma cube (see MeshKind::Soma): a full unit cube, per face uv along its axes and edge = the face direction.
Mesh somaCube() {
    Mesh m;
    m.kind = MeshKind::Soma;
    m.convex = true;
    for (int d = 0; d < 6; ++d) {
        const int a = d / 2;
        const double s = d % 2 == 0 ? 1.0 : -1.0;
        int ua = 0, va = 0;
        somaFaceAxes(d, ua, va);
        V3 n{0, 0, 0}, u{0, 0, 0}, v{0, 0, 0};
        n[std::size_t(a)] = s;
        u[std::size_t(ua)] = 1;
        v[std::size_t(va)] = 1;
        const double cu[4] = {-1, 1, 1, -1}, cv[4] = {-1, -1, 1, 1};
        V3 c[4];
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 3; ++j)
                c[k][std::size_t(j)] = 0.5 * (n[std::size_t(j)] + u[std::size_t(j)] * cu[k] + v[std::size_t(j)] * cv[k]);
        for (int q : {0, 1, 2, 0, 2, 3}) m.vertices.push_back(vert(c[q], n, 0.5 * (cu[q] + 1), 0.5 * (cv[q] + 1), double(d)));
    }
    return m;
}

}  // namespace

std::optional<MeshKind> parseMeshKind(std::string_view name) {
    std::string s;
    for (char c : name)
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    static const std::pair<const char*, MeshKind> kNames[] = {
        {"tetrahedron", MeshKind::Tetrahedron}, {"cube", MeshKind::Cube},       {"octahedron", MeshKind::Octahedron},
        {"dodecahedron", MeshKind::Dodecahedron}, {"icosahedron", MeshKind::Icosahedron}, {"sphere", MeshKind::Sphere},
        {"torus", MeshKind::Torus},             {"prism", MeshKind::Prism},           {"rubik", MeshKind::Rubik},
        {"fifteen", MeshKind::Fifteen},           {"labyrinth", MeshKind::Labyrinth},   {"knight", MeshKind::Knight},
        {"soma", MeshKind::Soma},                     {"jigsaw", MeshKind::Jigsaw},
        {"model", MeshKind::Model},                   {"rain", MeshKind::Rain},
        {"fractal", MeshKind::Fractal}};
    for (const auto& [n, k] : kNames)
        if (s == n) return k;
    return std::nullopt;
}

const char* meshKindName(MeshKind k) {
    switch (k) {
        case MeshKind::Tetrahedron: return "tetrahedron";
        case MeshKind::Cube: return "cube";
        case MeshKind::Octahedron: return "octahedron";
        case MeshKind::Dodecahedron: return "dodecahedron";
        case MeshKind::Icosahedron: return "icosahedron";
        case MeshKind::Sphere: return "sphere";
        case MeshKind::Torus: return "torus";
        case MeshKind::Prism: return "prism";
        case MeshKind::Rubik: return "rubik";
        case MeshKind::Fifteen: return "fifteen";
        case MeshKind::Knight: return "knight";
        case MeshKind::Labyrinth: return "labyrinth";
        case MeshKind::Soma: return "soma";
        case MeshKind::Jigsaw: return "jigsaw";
        case MeshKind::Model: return "model";
        case MeshKind::Rain: return "rain";
        case MeshKind::Fractal: return "fractal";
    }
    return "?";
}

Mesh makeMesh(MeshKind kind, int detail) {
    switch (kind) {
        case MeshKind::Sphere: return sphere(detail);
        case MeshKind::Torus: return torus(detail);
        case MeshKind::Rubik: return cubie();
        case MeshKind::Fifteen: return puzzleTile();
        case MeshKind::Knight: return knight::makePiece();
        case MeshKind::Labyrinth: {
            Mesh m = puzzleTile();  // the board, its walls and the hole markers are boxes and flat rings
            m.kind = MeshKind::Labyrinth;
            return m;
        }
        case MeshKind::Soma: return somaCube();
        case MeshKind::Jigsaw:  // drawn by render/JigsawPipeline over the colorized frame: no mesh
        case MeshKind::Model:
        case MeshKind::Rain:
        case MeshKind::Fractal: {
            Mesh m;
            m.kind = kind;
            m.convex = false;
            return m;
        }
        default: return polyhedron(kind);
    }
}

// ---- transforms ----

Mat4 Mat4::identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

std::array<float, 4> transform(const Mat4& m, const std::array<float, 4>& v) {
    std::array<float, 4> r{};
    for (int row = 0; row < 4; ++row)
        r[row] = m.m[row] * v[0] + m.m[4 + row] * v[1] + m.m[8 + row] * v[2] + m.m[12 + row] * v[3];
    return r;
}

namespace {
Mat4 rotation(int axis, double a) {
    Mat4 r = Mat4::identity();
    const float c = float(std::cos(a)), s = float(std::sin(a));
    const int i = (axis + 1) % 3, j = (axis + 2) % 3;  // the rotated plane (i, j); right-handed
    r.m[i * 4 + i] = c;
    r.m[j * 4 + i] = -s;
    r.m[i * 4 + j] = s;
    r.m[j * 4 + j] = c;
    return r;
}
}  // namespace

Mat4 solidModel(const SolidPose& p, double aspect) {
    Mat4 s = Mat4::identity();
    s.m[0] = s.m[5] = s.m[10] = float(p.scale);
    Mat4 t = Mat4::identity();
    t.m[12] = float(p.x * aspect);
    t.m[13] = float(p.y);
    t.m[14] = float(p.z);
    return t * rotation(2, p.rotZ) * rotation(1, p.rotY) * rotation(0, p.rotX) * s;
}

double solidCameraZ(double fovDegrees) {
    return 1.0 / std::tan(std::clamp(fovDegrees, 5.0, 150.0) * kPi / 360.0);
}

Mat4 solidViewProjection(double aspect, double fovDegrees, bool perspective) {
    const double camZ = solidCameraZ(fovDegrees);
    const double nearZ = 0.05, farZ = camZ + 10.0;
    Mat4 view = Mat4::identity();
    view.m[14] = float(-camZ);
    Mat4 proj{};
    if (perspective) {
        const double f = camZ;  // = 1 / tan(fov / 2): y = +-1 at z = 0 lands on the frame edge
        proj.m[0] = float(f / aspect);
        proj.m[5] = float(f);
        proj.m[10] = float(farZ / (nearZ - farZ));
        proj.m[11] = -1.0f;
        proj.m[14] = float(nearZ * farZ / (nearZ - farZ));
    } else {
        proj.m[0] = float(1.0 / aspect);
        proj.m[5] = 1.0f;
        proj.m[10] = float(1.0 / (nearZ - farZ));
        proj.m[14] = float(nearZ / (nearZ - farZ));  // depth 0 at -nearZ, 1 at -farZ (view space)
        proj.m[15] = 1.0f;
    }
    return proj * view;
}

ScreenBox projectBounds(const Mesh& mesh, const Mat4& mvp) {
    ScreenBox b;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    bool any = false;
    for (const MeshVertex& v : mesh.vertices) {
        const auto c = transform(mvp, {v.pos[0], v.pos[1], v.pos[2], 1.0f});
        if (c[3] <= 1e-6f) continue;  // behind the camera
        const float x = c[0] / c[3], y = c[1] / c[3];
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
        any = true;
    }
    if (!any || x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1) return b;
    b.x0 = std::max(x0, -1.0f);
    b.y0 = std::max(y0, -1.0f);
    b.x1 = std::min(x1, 1.0f);
    b.y1 = std::min(y1, 1.0f);
    b.visible = b.x1 > b.x0 && b.y1 > b.y0;
    return b;
}

Mesh makeFlatRing(double inner, int segments) {
    Mesh m;
    m.kind = MeshKind::Labyrinth;
    segments = std::max(3, segments);
    inner = std::clamp(inner, 0.0, 0.999);
    const V3 n{0, 0, 1};
    const double tau = 2.0 * std::acos(-1.0);
    for (int i = 0; i < segments; ++i) {
        const double a0 = tau * i / segments, a1 = tau * (i + 1) / segments;
        const V3 o0{std::cos(a0), std::sin(a0), 0}, o1{std::cos(a1), std::sin(a1), 0};
        const V3 i0 = mul(o0, inner), i1 = mul(o1, inner);
        auto put = [&](const V3& p) { m.vertices.push_back(vert(p, n, 0.5 + 0.5 * p[0], 0.5 + 0.5 * p[1], 1.0)); };
        if (inner <= 0) {
            put({0, 0, 0});
            put(o0);
            put(o1);
        } else {
            put(i0);
            put(o0);
            put(o1);
            put(i0);
            put(o1);
            put(i1);
        }
    }
    return m;
}

Mesh makeGlassButton(double half, double corner, double height, double bevel, int cornerSegments, int bevelSegments) {
    Mesh m;
    m.kind = MeshKind::Fifteen;
    m.convex = true;
    corner = std::clamp(corner, 1e-3, half);
    bevel = std::clamp(bevel, 1e-3, std::min(corner, height));
    // The footprint, counter-clockwise from +z: four quarter circles of radius `corner` around (+-c, +-c).
    const double c = half - corner;
    std::vector<V3> foot, out;  // points (z 0) and their outward normals
    const double ctr[4][2] = {{c, c}, {-c, c}, {-c, -c}, {c, -c}};
    for (int q = 0; q < 4; ++q)
        for (int k = 0; k <= cornerSegments; ++k) {
            const double a = kPi / 2 * (q + double(k) / cornerSegments);
            const V3 nrm{std::cos(a), std::sin(a), 0};
            foot.push_back({ctr[q][0] + corner * nrm[0], ctr[q][1] + corner * nrm[1], 0});
            out.push_back(nrm);
        }
    const int n = int(foot.size());
    // Profile rings, bottom to top: the wall, then the bevel's quarter circle. inset, z, normal (horizontal, z).
    struct Ring { double inset, z, nh, nz; };
    std::vector<Ring> rings{{0, 0, 1, 0}};
    for (int k = 0; k <= bevelSegments; ++k) {
        const double th = kPi / 2 * double(k) / bevelSegments;
        rings.push_back({bevel * (1 - std::cos(th)), height - bevel + bevel * std::sin(th), std::cos(th), std::sin(th)});
    }
    auto at = [&](int r, int i, V3& p, V3& nrm) {
        const Ring& g = rings[std::size_t(r)];
        const V3& f = foot[std::size_t(i % n)];
        const V3& o = out[std::size_t(i % n)];
        p = {f[0] - o[0] * g.inset, f[1] - o[1] * g.inset, g.z};
        nrm = {o[0] * g.nh, o[1] * g.nh, g.nz};
    };
    for (std::size_t r = 0; r + 1 < rings.size(); ++r)
        for (int i = 0; i < n; ++i) {
            V3 p[4], nn[4];
            at(int(r), i, p[0], nn[0]);
            at(int(r), i + 1, p[1], nn[1]);
            at(int(r) + 1, i + 1, p[2], nn[2]);
            at(int(r) + 1, i, p[3], nn[3]);
            for (int q : {0, 1, 2, 0, 2, 3}) m.vertices.push_back(vert(p[q], norm(nn[q]), p[q][0], p[q][1], 0.0));
        }
    // The cap: a fan from the center over the top ring.
    const int top = int(rings.size()) - 1;
    const V3 up{0, 0, 1}, center{0, 0, height};
    for (int i = 0; i < n; ++i) {
        V3 a, b, na, nb;
        at(top, i, a, na);
        at(top, i + 1, b, nb);
        m.vertices.push_back(vert(center, up, 0.0, 0.0, 1.0));
        m.vertices.push_back(vert(a, up, a[0], a[1], 1.0));
        m.vertices.push_back(vert(b, up, b[0], b[1], 1.0));
    }
    return m;
}

}  // namespace reforce::engine
