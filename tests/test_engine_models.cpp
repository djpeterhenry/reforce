// 3D model Objects phase 1 (PLAN 5R.1, docs/model-design.md): engine::loadModel on a generated .glb
// (tests/model_fixture.hpp), so no third-party model is committed.
#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <filesystem>
#include <fstream>

#include "engine/ModelFile.hpp"
#include "model_fixture.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;

namespace {

Model load(const reforce::testmodel::Options& o = {}, const ModelLoadOptions& lo = {}) {
    const auto glb = reforce::testmodel::makeGlb(o);
    std::string error;
    auto m = loadModelFromMemory(glb.bytes.data(), glb.bytes.size(), lo, &error);
    REQUIRE_MESSAGE(m.has_value(), error);
    return std::move(*m);
}

float len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

}  // namespace

TEST_CASE("model: counts, materials and embedded textures") {
    const Model m = load();
    CHECK(m.vertices.size() == 24 + 3);
    CHECK(m.triangleCount() == 12 + 1);
    REQUIRE(m.primitives.size() == 2);  // the points primitive is skipped
    CHECK(m.primitives[0].material == 0);
    CHECK(m.primitives[1].material == 1);
    CHECK(m.warnings.size() == 1);

    REQUIRE(m.materials.size() == 2);
    const ModelMaterial& hull = m.materials[0];
    CHECK(hull.name == "Hull");
    CHECK(hull.metallic == doctest::Approx(0));
    CHECK(hull.roughness == doctest::Approx(.6));
    CHECK(hull.normalScale == doctest::Approx(.8));
    CHECK(hull.emits());
    CHECK(m.materials[1].doubleSided);
    CHECK_FALSE(m.materials[1].emits());
    CHECK(m.anyEmissive());

    REQUIRE(m.textures.size() == 3);
    const ModelTexture& base = m.textures[std::size_t(hull.baseColorTex)];
    CHECK(base.mime == "image/png");
    CHECK(base.srgb);
    REQUIRE(base.bytes.size() > 8);
    CHECK(base.bytes[1] == 'P');
    CHECK_FALSE(m.textures[std::size_t(hull.normalTex)].srgb);
    CHECK(m.textures[std::size_t(hull.emissiveTex)].srgb);
}

TEST_CASE("model: normalized to the unit sphere, centered") {
    const Model m = load();
    CHECK(m.radius == doctest::Approx(1).epsilon(1e-5));
    for (int k = 0; k < 3; ++k)
        CHECK((m.boundsMin[std::size_t(k)] + m.boundsMax[std::size_t(k)]) * .5f == doctest::Approx(0).epsilon(1e-5));
    CHECK(m.sourceRadius > 2);  // the node's scale 2 was applied before normalizing
    CHECK(m.sourceCenter[0] > 9);
}

TEST_CASE("model: the front option turns the long axis onto -z") {
    // The hull is long along x; with front +x it must end up long along z (front at -z).
    ModelLoadOptions lo;
    lo.front = *parseModelAxis("+x");
    lo.up = *parseModelAxis("y");
    const Model m = load({}, lo);
    const float lx = m.boundsMax[0] - m.boundsMin[0], lz = m.boundsMax[2] - m.boundsMin[2];
    CHECK(lz > 1.5f * lx);
    // Default options leave the long axis on x.
    const Model d = load();
    CHECK(d.boundsMax[0] - d.boundsMin[0] > 1.5f * (d.boundsMax[2] - d.boundsMin[2]));

    ModelLoadOptions bad;
    bad.front = {0, 1, 0};
    const auto glb = reforce::testmodel::makeGlb();
    std::string error;
    CHECK_FALSE(loadModelFromMemory(glb.bytes.data(), glb.bytes.size(), bad, &error).has_value());
    CHECK(error.find("perpendicular") != std::string::npos);
}

TEST_CASE("model: unit normals and tangents, outward, counter-clockwise") {
    const Model m = load();
    for (const auto& v : m.vertices) {
        CHECK(len3(v.normal) == doctest::Approx(1).epsilon(1e-4));
        CHECK(len3(v.tangent) == doctest::Approx(1).epsilon(1e-4));
        const float d = v.normal[0] * v.tangent[0] + v.normal[1] * v.tangent[1] + v.normal[2] * v.tangent[2];
        CHECK(std::fabs(d) < 1e-3f);
        CHECK(std::fabs(v.tangent[3]) == 1.0f);
    }
    // Hull triangles: the geometric normal (counter-clockwise) agrees with the vertex normals and points away
    // from the center.
    const auto& p = m.primitives[0];
    float hc[3] = {0, 0, 0};  // the hull's own center (the fin raises the model's bounding box)
    for (std::uint32_t i = p.firstIndex; i < p.firstIndex + p.indexCount; ++i)
        for (int k = 0; k < 3; ++k) hc[k] += m.vertices[m.indices[i]].pos[k] / float(p.indexCount);
    for (std::uint32_t i = p.firstIndex; i < p.firstIndex + p.indexCount; i += 3) {
        const auto &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &c = m.vertices[m.indices[i + 2]];
        const float e1[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
        const float e2[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        CHECK(n[0] * a.normal[0] + n[1] * a.normal[1] + n[2] * a.normal[2] > 0);
        const float center[3] = {(a.pos[0] + b.pos[0] + c.pos[0]) / 3 - hc[0],
                                 (a.pos[1] + b.pos[1] + c.pos[1]) / 3 - hc[1], (a.pos[2] + b.pos[2] + c.pos[2]) / 3 - hc[2]};
        CHECK(n[0] * center[0] + n[1] * center[1] + n[2] * center[2] > 0);
    }
    // The mirrored triangle (no normals in the file): generated normal agrees with its (flipped) winding.
    const auto& q = m.primitives[1];
    const auto &a = m.vertices[m.indices[q.firstIndex]], &b = m.vertices[m.indices[q.firstIndex + 1]],
               &c = m.vertices[m.indices[q.firstIndex + 2]];
    const float e1[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
    const float e2[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
    const float nz = e1[0] * e2[1] - e1[1] * e2[0];
    CHECK(nz * a.normal[2] > 0);
}

TEST_CASE("model: axes, files and errors") {
    CHECK(*parseModelAxis("-Y") == std::array<float, 3>{0, -1, 0});
    CHECK(*parseModelAxis(" z ") == std::array<float, 3>{0, 0, 1});
    CHECK_FALSE(parseModelAxis("w").has_value());

    std::string error;
    CHECK_FALSE(loadModel("/nonexistent/none.glb", {}, &error).has_value());
    CHECK_FALSE(error.empty());
    const char junk[] = "not a model";
    CHECK_FALSE(loadModelFromMemory(junk, sizeof junk, {}, &error).has_value());

    // From a file on disk, too.
    const fs::path tmp = fs::temp_directory_path() / "reforce_test_model.glb";
    {
        const auto glb = reforce::testmodel::makeGlb();
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(glb.bytes.data()), std::streamsize(glb.bytes.size()));
    }
    const auto m = loadModel(tmp, {}, &error);
    CHECK(m.has_value());
    fs::remove(tmp);
}

TEST_CASE("model: without emission nothing emits") {
    reforce::testmodel::Options o;
    o.emissive = false;
    o.mirrorNode = false;
    const Model m = load(o);
    CHECK_FALSE(m.anyEmissive());
    CHECK(m.warnings.empty());
}

// ---- camera paths: keyframes and presets (PLAN 5R.7) ----
#include "engine/CameraMoves.hpp"
#include "engine/Solids.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

namespace {
using V3 = std::array<double, 3>;
double dist3(const V3& a, const V3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
const std::filesystem::path kCamera = std::filesystem::path(REFORCE_SOURCE_DIR) / "content" / "Camera";
CameraPath preset(const char* name) {
    std::string err;
    auto p = loadCameraPreset(kCamera / (std::string(name) + ".txt"), &err);
    REQUIRE_MESSAGE(p.has_value(), err);
    CHECK(err.empty());
    return *p;
}

// The moves as they were written in C++ before they became presets (Side +1, Dist 1, Hite 0): each preset must
// reproduce its path.
namespace legacy {
constexpr double kPi = 3.14159265358979323846;
double smooth(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); }
double ease(double p, double k) { return p + (smooth(p) - p) * k; }
template <std::size_t N>
V3 spline(const std::array<V3, N>& pts, double t) {
    const double x = std::clamp(t, 0.0, 1.0) * double(N - 1);
    const std::size_t i = std::min(std::size_t(x), N - 2);
    const double u = x - double(i);
    const V3 &p1 = pts[i], &p2 = pts[i + 1], &p0 = i > 0 ? pts[i - 1] : p1, &p3 = i + 2 < N ? pts[i + 2] : p2;
    V3 o{};
    for (std::size_t j = 0; j < 3; ++j)
        o[j] = 0.5 * ((2 * p1[j]) + (-p0[j] + p2[j]) * u + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * u * u +
                      (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * u * u * u);
    return o;
}
std::pair<V3, V3> shot(const std::string& move, double p) {
    if (move == "dolly") {
        const double t = ease(p, 0.6);
        static const std::array<V3, 5> eye{V3{0.35, 0.30, 1.30}, V3{0.28, 0.26, 0.55}, V3{0.26, 0.28, -0.25},
                                           V3{0.55, 0.50, -1.35}, V3{1.45, 0.95, -2.55}};
        static const std::array<V3, 5> aim{V3{0, 0.06, -0.3}, V3{0, 0.03, -1.0}, V3{0, 0.0, -1.5}, V3{0, 0, -0.9},
                                           V3{0, 0, 0}};
        return {spline(eye, t), spline(aim, t)};
    }
    if (move == "pass") {
        const double t = ease(p, 0.25);
        return {V3{2.4, 0.3, -6.5 + 13.0 * t}, V3{0, 0, 0}};
    }
    if (move == "orbit") {
        const double t = ease(p, 0.3), a = kPi - 0.7 + kPi * (t - 0.5);
        return {V3{3.0 * std::sin(a), 0.45 + 0.25 * std::sin(kPi * t), 3.0 * std::cos(a)}, V3{0, 0, 0}};
    }
    if (move == "under") {
        const double t = ease(p, 0.5);
        static const std::array<V3, 4> eye{V3{0.45, -0.15, -2.6}, V3{0.28, -0.8, -0.9}, V3{0.12, -0.75, 0.7},
                                           V3{0.18, 0.4, 2.4}};
        static const std::array<V3, 4> aim{V3{0, 0, 0}, V3{0, -0.1, 0}, V3{0, 0, 0.3}, V3{0, 0, -0.6}};
        return {spline(eye, t), spline(aim, t)};
    }
    const double t = ease(p, 0.1);  // flyby
    const double z = -8.0 + 13.0 * t;
    return {V3{0.75, 0.2, z}, V3{0, 0, z * 0.15}};
}
}  // namespace legacy
}  // namespace

TEST_CASE("camera presets reproduce the moves they replaced") {
    for (const char* name : {"dolly", "pass", "under", "flyby", "orbit"}) {
        CAPTURE(name);
        const CameraPath path = preset(name);
        // Straight moves are exact. Dolly and under pass through the same keyframes, but paths now continue straight
        // past their ends (they used to hold the end points), so near the ends they differ a little. Orbit's circle
        // becomes 13 keyframes: within a percent of its radius.
        const std::string n = name;
        const double tol = n == "orbit" ? 0.03 : n == "dolly" || n == "under" ? 0.2 : 0.002;
        double worst = 0;
        for (int i = 0; i <= 200; ++i) {
            const double p = i / 200.0;
            const ModelShot s = pathShot(path, p, {});
            const auto [eye, aim] = legacy::shot(name, p);
            worst = std::max({worst, dist3(s.eye, eye), dist3(s.aim, aim)});
        }
        CHECK(worst < tol);
    }
}

TEST_CASE("camera presets: finite, smooth, looking at the model") {
    for (const char* name : {"dolly", "pass", "orbit", "under", "flyby"}) {
        CAPTURE(name);
        const CameraPath path = preset(name);
        for (double side : {1.0, -1.0}) {
            CameraMoveParams prm;
            prm.side = side;
            // A 20 s move at 60 fps: no frame-to-frame jump of the eye beyond what the fastest move needs.
            const int n = 20 * 60;
            ModelShot prev = pathShot(path, 0, prm);
            double maxStep = 0;
            for (int i = 0; i <= n; ++i) {
                const ModelShot s = pathShot(path, double(i) / n, prm);
                for (std::size_t j = 0; j < 3; ++j) {
                    REQUIRE(std::isfinite(s.eye[j]));
                    REQUIRE(std::isfinite(s.aim[j]));
                }
                CHECK(dist3(s.eye, s.aim) > 0.05);
                CHECK(s.fov > 5);
                maxStep = std::max(maxStep, dist3(s.eye, prev.eye));
                CHECK(dist3(s.aim, {0, 0, 0}) < 1.6);  // every preset aims at the model
                prev = s;
            }
            CHECK(maxStep < 0.08);  // model radii per frame
        }
    }
    // Side mirrors x (and the roll).
    const CameraPath dolly = preset("dolly");
    CameraMoveParams l, r;
    l.side = -1;
    const auto a = pathShot(dolly, .3, r), b = pathShot(dolly, .3, l);
    CHECK(a.eye[0] == doctest::Approx(-b.eye[0]));
    CHECK(a.eye[2] == doctest::Approx(b.eye[2]));
    CHECK(a.roll == doctest::Approx(-b.roll));
}

TEST_CASE("camera paths: Step cuts change keyframe only on cut lines; Rand never repeats one") {
    for (bool random : {false, true}) {
        CameraPath path = preset("cuts");
        path.random = random;
        CameraMoveParams p;
        p.cutBeats = 4;
        int changes = 0;
        ModelShot prev = pathShot(path, 0, p);
        for (int i = 1; i <= 64 * 16; ++i) {
            p.beat = i / 16.0;
            const ModelShot s = pathShot(path, 0, p);
            if (dist3(s.eye, prev.eye) > 0.2) {
                ++changes;
                CHECK(std::fmod(p.beat, 4.0) == doctest::Approx(0));
            }
            prev = s;
        }
        CHECK(changes == 16);  // every cut line
    }
}

TEST_CASE("camera paths: keyframe parsing, sorting, ease, fov and roll") {
    std::string err;
    auto k = parseCameraKey(" .5, 1 2 3  0 0 -1  55  .1", &err);
    REQUIRE(k.has_value());
    CHECK(k->t == doctest::Approx(.5));
    CHECK(k->eye == V3{1, 2, 3});
    CHECK(k->fov == doctest::Approx(55));
    CHECK(k->roll == doctest::Approx(.1));
    CHECK_FALSE(parseCameraKey("0 1 2 3", &err).has_value());
    CHECK_FALSE(parseCameraKey("0 1 2 x 0 0 0", &err).has_value());
    // Keys out of order are sorted by t; between them the fov and roll glide too; the ends hold.
    std::map<std::string, std::string> text{{"K2", "1  0 0 5  0 0 0  60 .2"}, {"K1", "0  0 0 3  0 0 0  30"},
                                            {"K3", "oops"}};
    std::string errors;
    const CameraPath path = cameraPathFrom([&](const char* key) { auto it = text.find(key); return it == text.end() ? std::string() : it->second; }, &errors);
    REQUIRE(path.keys.size() == 2);
    CHECK(errors.find("K3:") != std::string::npos);
    const ModelShot mid = pathShot(path, .5, {});
    CHECK(mid.eye[2] == doctest::Approx(4));
    CHECK(mid.fov == doctest::Approx(45));
    CHECK(mid.roll == doctest::Approx(.1));
    CHECK(pathShot(path, 1.5, {}).eye[2] == doctest::Approx(5));
    // An empty path is the default view; Dist and Hite place it.
    CameraMoveParams far;
    far.dist = 2;
    far.height = .5;
    const ModelShot def = pathShot(CameraPath{}, .3, far);
    CHECK(def.eye == V3{0, 0.3 * 2 + .5, 6});
}

TEST_CASE("camera presets: the model folder's Camera/ wins over content/Camera/") {
    const auto dir = std::filesystem::temp_directory_path() / "reforce_camera_presets";
    std::filesystem::create_directories(dir / "Camera");
    { std::ofstream(dir / "Camera" / "orbit.txt") << "K1=\"0  0 0 9  0 0 0\"\n"; }
    const auto content = std::filesystem::path(REFORCE_SOURCE_DIR) / "content";
    CHECK(findCameraPreset("orbit", dir, content) == dir / "Camera" / "orbit.txt");
    CHECK(findCameraPreset("dolly", dir, content) == content / "Camera" / "dolly.txt");
    CHECK(findCameraPreset("spiral", dir, content).empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("model solid scene: keys load, own keyframes win, presets keep the scene's options") {
    auto make = [](const std::string& text) {
        auto file = reforce::scene::parseSceneText(text, "test");
        auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
        CHECK(cfg->diags.empty());
        reforce::scene::Environment env;
        return std::make_unique<SolidRun>(cfg, env, "test", MeshKind::Model, 0.0, 1.0, 30.0, nullptr);
    };
    auto run = make("Vers=100\nMesh=\"model\"\nFile=\"test.glb\"\nLook=\"1\"\nMove=\"Under\"\nFront=\"+x\"\n"
                    "MDur=\"10\"\nGlow=\"2\"\nSide=\"-1\"\nEase=\"0\"\n");
    CHECK(run->model().file == "test.glb");
    CHECK(run->model().look == 1);
    CHECK(run->model().move == "under");
    CHECK_FALSE(run->model().ownPath);
    CHECK(run->model().front == std::array<float, 3>{1, 0, 0});
    CHECK(run->model().moveSeconds == doctest::Approx(10));
    CHECK(run->model().error.empty());
    run->setCameraPath(preset("under"));
    CHECK(run->model().path.ease == doctest::Approx(0));  // the scene's Ease wins over the preset's .5
    const SolidSpec early = run->evaluate(2.0);
    CHECK(early.shot.eye[0] < 0);  // Side -1
    CHECK(early.glow == doctest::Approx(2).epsilon(.01));

    // The scene's own keyframes: played as written, a preset is ignored.
    auto own = make("Vers=100\nMesh=\"model\"\nFile=\"m.glb\"\nMDur=\"10\"\n"
                    "K1=\"0  0 0 3  0 0 0\"\nK2=\"1  0 0 6  0 0 0\"\n");
    CHECK(own->model().ownPath);
    own->setCameraPath(preset("orbit"));
    CHECK(own->evaluate(5.0).shot.eye[2] == doctest::Approx(4.5));
}

#include "engine/Themes.hpp"

TEST_CASE("model scenes: the Models group matches by catalog id (Include.Id), whatever the scene is called") {
    ThemeLibrary lib;
    lib.addCategory("Object", "CategoryTitle = \"Models\"\nInclude.Id = \"Model/.*\"\n");
    lib.addTheme("ThemeName = \"Objects: Models\"\nObject.CategoryExpr = \"+Models\"\n");
    const Category* c = lib.category("Object", "Models");
    REQUIRE(c);
    CHECK(c->matches("Anything Orbit", "Model/Anything Orbit"));
    CHECK_FALSE(c->matches("Cube Roll", "Solids/Cube Roll"));
    CHECK_FALSE(c->matches("Model Orbit"));  // no id: name alone does not match
    const std::vector<std::string> names{"Cube Roll", "Anything Orbit"}, ids{"Solids/Cube Roll", "Model/Anything Orbit"};
    const auto kept = lib.filter(lib.find("Objects: Models"), "Object", names, nullptr, &ids);
    CHECK(kept == std::vector<std::string>{"Anything Orbit"});
}

#include "engine/ObjectBeat.hpp"
#include "engine/SceneLibrary.hpp"

TEST_CASE("objects: the green note (objectFollowsBeat) by kind, for content and model scenes") {
    using reforce::scene::SceneType;
    auto flag = [](SceneType t, const std::string& text) {
        auto f = reforce::scene::parseSceneText(text, "test");
        return objectFollowsBeat(t, reforce::scene::compileConfig(f, t));
    };
    // Model scenes: MBar or cuts follow the beat; MDur alone does not.
    CHECK(flag(SceneType::Solid, "Mesh=\"model\"\nFile=\"m.glb\"\nMBar=\"16\"\n"));
    CHECK(flag(SceneType::Solid, "Mesh=\"model\"\nFile=\"m.glb\"\nMove=\"cuts\"\n"));
    CHECK_FALSE(flag(SceneType::Solid, "Mesh=\"model\"\nFile=\"m.glb\"\nMDur=\"20\"\n"));
    CHECK(flag(SceneType::Solid, "Mesh=\"model\"\nFile=\"m.glb\"\nGlow=\"1 + BEAT\"\n"));
    // Organics by kind.
    CHECK(flag(SceneType::Organic, "Kind=\"ripples\"\n"));
    CHECK(flag(SceneType::Organic, "Kind=\"invaders\"\n"));
    CHECK(flag(SceneType::Organic, "Kind=\"pendulums\"\n"));
    CHECK_FALSE(flag(SceneType::Organic, "Kind=\"pendulums\"\nPhas=\".3 * t\"\n"));
    CHECK_FALSE(flag(SceneType::Organic, "Kind=\"harmonograph\"\n"));
    CHECK_FALSE(flag(SceneType::Organic, "Kind=\"tree\"\nGrow=\"12\"\n"));
    CHECK(flag(SceneType::Organic, "Kind=\"tree\"\nSwyB=\"2\"\n"));
    // Content: every arcade game, field and growth kind follows the beat; these time-driven ones don't.
    const auto content = std::filesystem::path(REFORCE_SOURCE_DIR) / "content";
    for (SceneType t : {SceneType::Solid, SceneType::Organic})
        for (const auto& e : listScenes(content, t)) {
            const auto f = reforce::scene::loadSceneFile(e.path);
            REQUIRE(f);
            const bool beat = objectFollowsBeat(t, reforce::scene::compileConfig(*f, t));
            CAPTURE(e.id);
            const bool timeOnly = e.name == "Icosahedron Drift" || e.name == "Octahedron Mirror" ||
                                  e.name == "Prism Flyby" || e.name == "Harmonograph";
            CHECK(beat == !timeOnly);
        }
}
