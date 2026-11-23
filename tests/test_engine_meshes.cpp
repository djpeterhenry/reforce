#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "engine/Meshes.hpp"

using namespace reforce::engine;

namespace {

std::array<double, 3> v3(const float* p) { return {p[0], p[1], p[2]}; }
double dot(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
std::array<double, 3> cross(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
std::array<double, 3> sub(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

// Every triangle winds counter-clockwise seen from outside: its geometric normal agrees with the stored normal and,
// for convex centered meshes, points away from the center.
void checkWinding(const Mesh& m) {
    REQUIRE(m.vertices.size() % 3 == 0);
    for (std::size_t t = 0; t < m.vertices.size(); t += 3) {
        const auto a = v3(m.vertices[t].pos), b = v3(m.vertices[t + 1].pos), c = v3(m.vertices[t + 2].pos);
        const auto g = cross(sub(b, a), sub(c, a));
        const auto n = v3(m.vertices[t].normal);
        CHECK(dot(g, n) > 0);
        if (m.convex) CHECK(dot(g, a) > 0);
    }
}

}  // namespace

TEST_CASE("meshes: Platonic solids and the prism are closed convex polyhedra") {
    struct Want {
        MeshKind k;
        int v, e, f;
    };
    for (const Want w : {Want{MeshKind::Tetrahedron, 4, 6, 4}, Want{MeshKind::Cube, 8, 12, 6},
                         Want{MeshKind::Octahedron, 6, 12, 8}, Want{MeshKind::Dodecahedron, 20, 30, 12},
                         Want{MeshKind::Icosahedron, 12, 30, 20}, Want{MeshKind::Prism, 6, 9, 5}}) {
        CAPTURE(meshKindName(w.k));
        const Mesh m = makeMesh(w.k);
        CHECK(int(m.hullVertices.size()) == w.v);
        CHECK(int(m.hullFaces.size()) == w.f);
        // edges: each shared by exactly two faces, in opposite directions (consistent orientation)
        std::map<std::pair<int, int>, int> directed;
        for (const auto& f : m.hullFaces)
            for (std::size_t i = 0; i < f.size(); ++i) ++directed[{f[i], f[(i + 1) % f.size()]}];
        std::set<std::pair<int, int>> undirected;
        for (const auto& [e, n] : directed) {
            CHECK(n == 1);
            CHECK(directed.count({e.second, e.first}) == 1);
            undirected.insert({std::min(e.first, e.second), std::max(e.first, e.second)});
        }
        CHECK(int(undirected.size()) == w.e);
        CHECK(w.v - w.e + w.f == 2);  // Euler
        for (const auto& p : m.hullVertices)
            CHECK(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) == doctest::Approx(1.0).epsilon(1e-5));
        checkWinding(m);
        // fan triangles: one per polygon edge; center edge = 1, rim = 0; UVs on the inscribed circle
        std::size_t rim = 0;
        for (const auto& f : m.hullFaces) rim += f.size();
        CHECK(m.vertices.size() == 3 * rim);
        for (std::size_t t = 0; t < m.vertices.size(); t += 3) {
            CHECK(m.vertices[t].edge == 1.0f);
            for (int i = 1; i < 3; ++i) {
                const MeshVertex& r = m.vertices[t + i];
                CHECK(r.edge == 0.0f);
                const double du = r.uv[0] - 0.5, dv = r.uv[1] - 0.5;
                CHECK(std::sqrt(du * du + dv * dv) == doctest::Approx(0.5).epsilon(1e-4));
            }
            CHECK(m.vertices[t].uv[0] == doctest::Approx(0.5).epsilon(1e-5));
            CHECK(m.vertices[t].uv[1] == doctest::Approx(0.5).epsilon(1e-5));
        }
    }
}

TEST_CASE("meshes: sphere and torus") {
    const Mesh s = makeMesh(MeshKind::Sphere, 2);
    CHECK(s.vertices.size() == 3 * 320);
    CHECK(s.convex);
    for (const auto& v : s.vertices) {
        const double r = std::sqrt(dot(v3(v.pos), v3(v.pos)));
        CHECK(r == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(dot(v3(v.normal), v3(v.pos)) == doctest::Approx(1.0).epsilon(1e-5));
    }
    checkWinding(s);
    const Mesh t = makeMesh(MeshKind::Torus, 1);
    CHECK(!t.convex);
    double maxR = 0;
    for (const auto& v : t.vertices) {
        maxR = std::max(maxR, std::sqrt(dot(v3(v.pos), v3(v.pos))));
        CHECK(std::sqrt(dot(v3(v.normal), v3(v.normal))) == doctest::Approx(1.0).epsilon(1e-5));
    }
    CHECK(maxR == doctest::Approx(1.0).epsilon(1e-5));
    checkWinding(t);
}

TEST_CASE("meshes: names") {
    CHECK(parseMeshKind("Icosahedron") == MeshKind::Icosahedron);
    CHECK(parseMeshKind(" cube ") == MeshKind::Cube);
    CHECK(!parseMeshKind("blob"));
    for (int k = 0; k <= int(MeshKind::Fifteen); ++k)
        CHECK(parseMeshKind(meshKindName(MeshKind(k))) == MeshKind(k));
}

TEST_CASE("solid transforms: rotation direction, sprite coordinates, projection") {
    // rotY by +90 degrees takes +x to -z (right-handed)
    SolidPose p;
    p.scale = 1;
    p.rotY = std::acos(-1.0) / 2;
    const auto r = transform(solidModel(p, 1.0), {1, 0, 0, 1});
    CHECK(r[0] == doctest::Approx(0).epsilon(1e-6));
    CHECK(r[2] == doctest::Approx(-1).epsilon(1e-6));
    // at z = 0, sprite coordinates map to the frame: x = 1 -> right edge, y = -1 -> bottom (any aspect)
    for (bool persp : {true, false}) {
        const double aspect = 16.0 / 9.0;
        SolidPose q;
        q.scale = 0;  // a point
        q.x = 1;
        q.y = -1;
        const Mat4 mvp = solidViewProjection(aspect, 40, persp) * solidModel(q, aspect);
        const auto c = transform(mvp, {0, 0, 0, 1});
        CHECK(c[0] / c[3] == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(c[1] / c[3] == doctest::Approx(-1.0).epsilon(1e-5));
        CHECK(c[2] / c[3] > 0);
        CHECK(c[2] / c[3] < 1);
    }
}

TEST_CASE("solid transforms: screen bounds") {
    const double aspect = 16.0 / 9.0;
    const Mesh cube = makeMesh(MeshKind::Cube);
    SolidPose p;
    p.scale = 0.5;
    // orthographic, centered, unrotated: the cube's face is a square of half-height 0.5/sqrt(3) in y
    ScreenBox b = projectBounds(cube, solidViewProjection(aspect, 40, false) * solidModel(p, aspect));
    REQUIRE(b.visible);
    const double h = 0.5 / std::sqrt(3.0);
    CHECK(b.y1 == doctest::Approx(h).epsilon(1e-4));
    CHECK(b.y0 == doctest::Approx(-h).epsilon(1e-4));
    CHECK(b.x1 == doctest::Approx(h / aspect).epsilon(1e-4));
    // perspective makes the front face bigger than orthographic
    const ScreenBox bp = projectBounds(cube, solidViewProjection(aspect, 40, true) * solidModel(p, aspect));
    CHECK(bp.y1 > b.y1);
    // half off the right edge: clipped to 1; fully off: not visible
    p.x = 1.0;
    b = projectBounds(cube, solidViewProjection(aspect, 40, true) * solidModel(p, aspect));
    CHECK(b.visible);
    CHECK(b.x1 == 1.0f);
    p.x = 3.0;
    CHECK(!projectBounds(cube, solidViewProjection(aspect, 40, true) * solidModel(p, aspect)).visible);
}

TEST_CASE("meshes: the glass button: inside its footprint and height, unit normals, wound outwards") {
    const Mesh b = makeGlassButton();
    REQUIRE(!b.vertices.empty());
    float maxZ = 0, maxXY = 0;
    for (const auto& v : b.vertices) {
        maxZ = std::max(maxZ, v.pos[2]);
        maxXY = std::max({maxXY, std::fabs(v.pos[0]), std::fabs(v.pos[1])});
        CHECK(v.pos[2] >= -1e-6f);
        CHECK(std::sqrt(dot(v3(v.normal), v3(v.normal))) == doctest::Approx(1.0).epsilon(1e-5));
    }
    CHECK(maxZ == doctest::Approx(0.32f));
    CHECK(maxXY == doctest::Approx(0.47f).epsilon(1e-3));
    checkWinding(b);
}
