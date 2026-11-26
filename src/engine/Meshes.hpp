#pragma once
// reforce extension (PLAN 5L, docs/3d-design.md): meshes and transform math for 3D solids. Pure C++ (no Metal).
//
// Meshes are built once and live at circumradius 1 around the origin:
//   - Platonic solids and the prism come from their vertex lists. Faces are found as the planes that have every
//     vertex on one side and at least three on them (a convex hull, fine for <= 20 vertices). Each face is a
//     fan of triangles around its center:
//       - vertices are duplicated per face (flat normals)
//       - the face polygon's UVs lie on the inscribed circle of the [0,1]^2 texture (radius 0.5 around
//         (0.5, 0.5)), so a centered WaveShape tile sits centered on every face
//       - `edge` is 1 at the face center and 0 on its border, for edge highlights.
//   - The geodesic sphere (a subdivided icosahedron) and the torus are smooth (vertex normals) with spherical /
//     parametric UVs, and edge = 1.
// Triangles wind counter-clockwise seen from outside (cross(b - a, c - a) points along the outward normal), so
// back faces can be culled.
//
// Coordinates: positions X, Y use the sprite convention (+-1 = the frame edges), so solidModel scales X by the
// aspect ratio and shapes stay undistorted. The camera looks down -z from +cameraZ, where cameraZ = 1 / tan(fov / 2):
// at z = 0, y = +-1 is the top / bottom edge and x = +-aspect the sides. Matrices are column-major (Metal float4x4),
// and clip-space depth runs from 0 (near) to 1 (far), like Metal.

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <string_view>
#include <vector>

namespace reforce::engine {

// Rubik: one cubie of the Rubik's cube solid (engine/Rubik.hpp), a unit cube [-.5, .5]^3: the plastic body (uv.y = 0)
// and a sticker on each face, slightly raised (uv = (direction 0..5, 1), engine::rubik direction order). The solid
// pass draws it once per cubie with its own transform and sticker colors; Scal is the whole cube's circumradius.
// Fifteen: one tile of the 15-puzzle solid (engine/Fifteen.hpp), a unit box [-.5, .5]^3 whose +z face has uv 0..1
// (u right, v up, seen from +z) and whose other faces have uv (-1, -1). Drawn once per tile and once for the tray.
// Soma: one unit cube of the soma solid (engine/Soma.hpp), [-.5, .5]^3, full size (cubes of a piece meet flush); per
// face direction d (+x, -x, +y, -y, +z, -z) uv 0..1 along the face's axes (somaFaceAxes) and edge = d, so soma_fs knows
// which face and which of its four borders a fragment is near. Drawn once per cube of each piece.
// Model: a user-supplied glTF model (engine/ModelFile, render/ModelPipeline; docs/model-design.md). makeMesh gives an
// empty mesh: its geometry comes from the scene's File, not from here.
// Rain: rain on glass over the frame (engine/Rain, render/RainPipeline; docs/3d-design.md "Rain on glass"). makeMesh
// gives an empty mesh.
// Labyrinth: the tilt maze solid (engine/Labyrinth.hpp): its mesh is the Fifteen box (the board, the walls); the
// solid pass also draws the sphere (the ball) and makeFlatRing discs / rings (the holes, the start).
// Fractal: a self-similar zoom into a 3D fractal (engine/FractalZoom, render/FractalPipeline; docs/3d-design.md
// "Fractal zoom"), raymarched: makeMesh gives an empty mesh.
// Bubbles: soap bubbles over the frame (engine/Bubbles, render/BubblePipeline; docs/3d-design.md "Bubbles"). makeMesh
// gives an empty mesh.
// Knight: the chess knight of the knight's-tour solid (engine/Knight.hpp makePiece): z up, facing +x, base on z = 0.
// Jigsaw: the jigsaw of the live frame (engine/Jigsaw.hpp, render/JigsawPipeline over the colorized frame): no mesh.
// Hanoi: the glass Tower of Hanoi (engine/Hanoi.hpp, render/HanoiPipeline over the colorized frame; docs/3d-design.md
// "Tower of Hanoi"): its discs, pegs and base are lathed in the vertex shader, so makeMesh gives an empty mesh.
// Shatter: a glass pane over the frame that cracks and shatters (engine/Shatter, render/ShatterPipeline; docs/3d-design.md
// "Shatter"): no mesh.
enum class MeshKind {
    Tetrahedron, Cube, Octahedron, Dodecahedron, Icosahedron, Sphere, Torus, Prism, Rubik, Fifteen, Labyrinth, Knight,
    Soma, Jigsaw, Model, Rain, Fractal, Hanoi, Bubbles, Shatter
};
// The puzzles (rubik, fifteen, labyrinth, soma): drawn in their own colors (engine/StickerColors, engine/PieceColors,
// the colorize pass's color overlay).
inline bool isPuzzleMesh(MeshKind k) {
    return k == MeshKind::Rubik || k == MeshKind::Fifteen || k == MeshKind::Labyrinth || k == MeshKind::Soma;
}
// A face's in-plane axes (the soma mesh's uv): face d (+x, -x, +y, -y, +z, -z) has u along axis (a + 1) % 3 and v
// along (a + 2) % 3 (a = d / 2), swapped on the negative faces, so u x v points out of the face.
inline void somaFaceAxes(int d, int& u, int& v) {
    const int a = d / 2;
    u = (a + 1) % 3;
    v = (a + 2) % 3;
    if (d % 2) std::swap(u, v);
}
std::optional<MeshKind> parseMeshKind(std::string_view name);  // case-insensitive; "tetrahedron", "cube", ...
const char* meshKindName(MeshKind k);

struct MeshVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float edge;  // 1 at a face center, 0 on its border (smooth meshes: 1)
};
static_assert(sizeof(MeshVertex) == 36);

struct Mesh {
    MeshKind kind = MeshKind::Cube;
    std::vector<MeshVertex> vertices;  // triangle list, 3 per triangle
    bool convex = true;                // false: needs a depth buffer
    // Polyhedra: the hull, for tests and outlines (positions + faces as ordered vertex indices). Empty when smooth.
    std::vector<std::array<float, 3>> hullVertices;
    std::vector<std::vector<int>> hullFaces;
};

// A glass button for the glass 15 puzzle: a rounded square footprint (half size `half`, corner radius `corner`) from
// z = 0 up to `height`, its top edge rounded with radius `bevel` (< corner), a flat cap on top; no bottom. Smooth
// normals, triangles counter-clockwise from outside; uv = the vertex's x, y (tile units), edge = 1 on the cap.
Mesh makeGlassButton(double half = 0.47, double corner = 0.17, double height = 0.32, double bevel = 0.13,
                     int cornerSegments = 8, int bevelSegments = 6);

// A flat ring in the z = 0 plane facing +z, outer radius 1, inner radius `inner` (0: a disc), uv = 0.5 + 0.5 xy.
Mesh makeFlatRing(double inner, int segments = 48);

// detail: sphere subdivisions (0 = icosahedron, 2 = 320 triangles) / torus segments scale. Ignored for polyhedra.
Mesh makeMesh(MeshKind kind, int detail = 2);

// ---- transforms ----
struct Mat4 {
    float m[16];  // column-major: m[col * 4 + row]
    static Mat4 identity();
};
Mat4 operator*(const Mat4& a, const Mat4& b);
std::array<float, 4> transform(const Mat4& m, const std::array<float, 4>& v);

struct SolidPose {
    double x = 0, y = 0, z = 0;           // sprite convention: x, y +-1 = frame edges at z = 0
    double scale = 0.5;                   // circumradius in frame half-heights
    double rotX = 0, rotY = 0, rotZ = 0;  // radians, applied X, then Y, then Z
};
// Model matrix: scale, rotate (X then Y then Z), translate to (x * aspect, y, z).
Mat4 solidModel(const SolidPose& p, double aspect);
// View + projection. perspective = false: orthographic at the z = 0 scale.
Mat4 solidViewProjection(double aspect, double fovDegrees = 40.0, bool perspective = true);
double solidCameraZ(double fovDegrees);

// Screen rectangle (normalized device coords, x, y in [-1, 1]) covered by the mesh under mvp, clipped to the frame.
struct ScreenBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool visible = false;
};
ScreenBox projectBounds(const Mesh& mesh, const Mat4& mvp);

}  // namespace reforce::engine
