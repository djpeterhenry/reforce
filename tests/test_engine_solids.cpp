// 3D solids phase 4 (PLAN 5L.4): Solid scene files, SolidRun, SYNC_* variables, the Solid layer's commands,
// settings and themes, and reforce's committed content/ (every solid compiles cleanly and evaluates finitely).
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "engine/Commands.hpp"
#include "engine/Prefs.hpp"
#include "engine/Scheduler.hpp"
#include "engine/SceneLibrary.hpp"
#include "engine/Solids.hpp"
#include "engine/Themes.hpp"
#include "scene/Evaluate.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;
using reforce::scene::SceneType;

namespace {

std::shared_ptr<const reforce::scene::CompiledConfig> compile(const std::string& text) {
    auto file = reforce::scene::parseSceneText(text, "test");
    return std::make_shared<reforce::scene::CompiledConfig>(reforce::scene::compileConfig(file, SceneType::Solid));
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const fs::path kContent = fs::path(REFORCE_SOURCE_DIR) / "content";

}  // namespace

TEST_CASE("solids: scene type, package, layer names") {
    using reforce::scene::sceneTypeFromPackage;
    CHECK(sceneTypeFromPackage("reforce.Solid.package") == SceneType::Solid);
    CHECK(packageDirName(SceneType::Solid) == "reforce.Solid.package");
    CHECK(reforce::scene::sceneTypeName(SceneType::Solid) == "Solid");
    CHECK(reforce::scene::sceneTypeFromPath("/x/reforce.Solid.package/Spin/Cube.txt") == SceneType::Solid);
    CHECK_FALSE(parseLayer("solids"));  // Solids and Organics are one layer, Objects (2026-11-22)
    CHECK_FALSE(parseLayer("organic"));
    CHECK(parseLayer("Objects") == Layer::Object);
    CHECK(std::string(layerName(Layer::Object)) == "Object");
    CHECK(isSpawnLayer(Layer::Object));
    CHECK_FALSE(isSpawnLayer(Layer::FlowField));
    CHECK(std::string(defaultReferenceScene(SceneType::Solid)).empty());
}

TEST_CASE("solids: compile tiers, the Mesh literal, defaults") {
    auto cfg = compile("Vers=100\nMesh=\"Dodecahedron\"\nText=\"1\"\nFlag=\"4096\"\nPDur=\"7\"\nPers=\"0\"\n"
                       "A0=\".25\"\nB0=\"B0 + 1\"\nX=\"A0\"\nRotY=\"B0\"\nC0=\"1\"\n");
    CHECK(cfg->find("MESH")->tier == reforce::scene::Tier::Ignored);
    CHECK(cfg->find("MESH")->text == "Dodecahedron");
    CHECK(cfg->find("X")->tier == reforce::scene::Tier::FrameOutput);
    CHECK(cfg->find("ROTY")->tier == reforce::scene::Tier::FrameOutput);
    CHECK(cfg->find("TEXT")->tier == reforce::scene::Tier::Pre);
    CHECK(cfg->diags.hasKind("key-unused-for-type"));  // C vars: not evaluated by solids
    CHECK_FALSE(cfg->diags.hasKind("unknown-identifier"));  // the mesh name is not compiled
    REQUIRE(solidMesh(*cfg) == MeshKind::Dodecahedron);

    reforce::scene::Environment env;
    reforce::scene::SolidInstance inst(*cfg, env);
    CHECK(inst.mesh() == "Dodecahedron");
    CHECK(inst.text() == 1);
    CHECK(inst.flags() == 4096u);
    CHECK(inst.duration() == doctest::Approx(7));
    CHECK_FALSE(inst.perspective());
    reforce::scene::SolidFrameInput in;
    auto f = inst.evaluateFrame(in);
    CHECK(f.x == doctest::Approx(.25));
    CHECK(f.rotY == doctest::Approx(1));
    f = inst.evaluateFrame(in);
    CHECK(f.rotY == doctest::Approx(2));  // B vars persist across frames
    CHECK(f.scale == doctest::Approx(.5));
    CHECK(f.fill == doctest::Approx(.15));
    CHECK((f.hasPen && !f.hasEPen && f.pen == 1.0));  // no Pen, no EPen: Pen 1

    auto bare = compile("X=\"0\"\n");
    CHECK(solidMesh(*bare) == MeshKind::Icosahedron);  // default
    reforce::scene::SolidInstance b(*bare, env);
    CHECK(b.text() == 0);  // the WaveShape by default
    CHECK(b.perspective());
    std::string written;
    CHECK_FALSE(solidMesh(*compile("Mesh=\"teapot\"\n"), &written));
    CHECK(written == "teapot");

    auto epenOnly = compile("EPen=\".5\"\n");
    reforce::scene::SolidInstance e(*epenOnly, env);
    f = e.evaluateFrame(in);
    CHECK((!f.hasPen && f.hasEPen && f.epen == doctest::Approx(.5) && f.pen == 0.0));
}

TEST_CASE("solids: SYNC_* variables in solids, WaveShapes and sprites") {
    reforce::scene::SyncInput sync;
    sync.on = 1;
    sync.bpm = 124;
    sync.beat = 37.25;
    sync.lock = .8;
    reforce::scene::Environment env;
    auto cfg = compile("X=\"SYNC_ON\"\nY=\"SYNC_BPM\"\nZ=\"SYNC_BEAT\"\nScal=\"SYNC_LOCK\"\n");
    CHECK(cfg->diags.empty());  // native for solids: no "not native" warnings
    reforce::scene::SolidInstance s(*cfg, env);
    reforce::scene::SolidFrameInput in;
    in.sync = sync;
    auto f = s.evaluateFrame(in);
    CHECK((f.x == 1 && f.y == 124 && f.z == doctest::Approx(37.25) && f.scale == doctest::Approx(.8)));

    auto wfile = reforce::scene::parseSceneText("Stps=2\nX0=\"wrap(SYNC_BEAT)\"\nY0=\"SYNC_BPM/1000\"\n", "w");
    auto wcfg = reforce::scene::compileConfig(wfile, SceneType::WaveShape);
    CHECK(wcfg.diags.empty());
    reforce::scene::WaveShapeInstance w(wcfg, env);
    reforce::scene::WaveShapeFrameInput win;
    win.sync = sync;
    reforce::scene::WaveShapeFrame wf;
    w.evaluateFrame(win, wf);
    REQUIRE(!wf.paths.empty());
    CHECK(wf.paths[0].x[0] == doctest::Approx(.25));
    CHECK(wf.paths[0].y[0] == doctest::Approx(.124));

    auto sfile = reforce::scene::parseSceneText("X=\"SYNC_ON\"\nY=\"SYNC_BEAT\"\n", "s");
    auto scfg = reforce::scene::compileConfig(sfile, SceneType::Sprite);
    CHECK(scfg.diags.empty());
    reforce::scene::SpriteInstance sp(scfg, env);
    reforce::scene::SpriteFrameInput sin;
    sin.sync = sync;
    auto sf = sp.evaluateFrame(sin);
    CHECK((sf.x == 1 && sf.y == doctest::Approx(37.25)));
}

TEST_CASE("solid run: envelope, PDur, clamping, sync passed through") {
    reforce::scene::Environment env;
    auto fn = std::make_shared<TransitionFunction>("t");
    auto cfg = compile("Mesh=\"torus\"\nFill=\"3\"\nShad=\"-1\"\nPen=\"2\"\nScal=\"-1\"\nRotX=\"SYNC_BEAT\"\n");
    SolidRun r(cfg, env, "T", MeshKind::Torus, 10.0, 2.0, 5.0, fn);
    CHECK(r.endTime() == doctest::Approx(19));  // 2 + 5 + 2
    SpriteAudio a;
    a.sync.beat = 3.5;
    auto s = r.evaluate(11.0, a);
    CHECK(s.mesh == MeshKind::Torus);
    CHECK(s.opacity == doctest::Approx(.5));  // halfway through a linear fade-in
    CHECK((s.fill == 1.0 && s.shade == 0.0 && s.pen == 1.0 && s.pose.scale == 0.0));
    CHECK(s.pose.rotX == doctest::Approx(3.5));
    CHECK(r.evaluate(14.0, a).opacity == doctest::Approx(1));
    CHECK(r.expired(19.0));
    r.fadeOutFrom(14.0, 1.0);
    CHECK(r.evaluate(14.5, a).opacity == doctest::Approx(.5));

    auto pd = compile("PDur=\"1\"\n");
    SolidRun p(pd, env, "P", MeshKind::Cube, 0.0, 1.0, 30.0, fn);
    CHECK(p.endTime() == doctest::Approx(3));  // PDur wins over the default hold
}

TEST_CASE("solids: commands, slideshow settings") {
    auto c = parseCommand("switch:object:Cube Orbit");
    REQUIRE(c);
    CHECK((c->type == CommandType::Switch && c->layer == Layer::Object && c->text == "Cube Orbit" && c->quick));
    c = parseCommand("kill:object");
    REQUIRE(c);
    CHECK((c->type == CommandType::Kill && c->layer == Layer::Object));
    c = parseCommand("life:object:20");
    REQUIRE(c);
    CHECK((c->type == CommandType::SetSpawnDuration && c->value == 20));
    CHECK_FALSE(parseCommand("morph:object:2:Cube Orbit"));
    CHECK_FALSE(parseCommand("kill:object:images"));
    // interval 0 (none from the slideshow) is saved for spawn layers.
    CHECK(slideshowSaveEntries(Command::interval(Layer::Object, 0)).string("Object.Slideshow.Interval.Duration", "?") == "0");
    CHECK(slideshowSaveEntries(Command::spawnDuration(Layer::Object, 25)).string("Object.Duration", "?") == "25");
}

TEST_CASE("solids: settings (reforce-only, never imported from G-Force)") {
    Settings p;
    CHECK(p.object.slideshowInterval == 0);  // not in the default slideshow
    CHECK(p.object.duration > 0);
    CHECK(p.objectMaxLive == 1);
    PrefsTable t;
    t.set("Object.Slideshow.Interval.Duration", "45", false);
    t.set("Object.Duration", "12", false);
    t.set("reforce.Object.MaxLive", "2", false);
    t.set("Solid.Duration", "99", false);  // the layers before Objects: no longer read
    t.set("reforce.Solid.Dir", "/tmp/solids", true);
    t.set("reforce.ContentDir", "/tmp/content", true);
    p.apply(t);
    CHECK(p.object.slideshowInterval == 45);
    CHECK(p.object.duration == 12);
    CHECK(p.objectMaxLive == 2);
    CHECK(p.solidDir == "/tmp/solids");
    CHECK(p.contentDir == "/tmp/content");
    for (const auto& k : importedGForceKeys()) CHECK(k.rfind("Solid.", 0) != 0);
}

TEST_CASE("solids: themes start them, content themes merge") {
    ThemeLibrary lib;
    lib.addTheme("ThemeName = \"Night\"\nSprite.CategoryExpr = \"+Space\"\n");
    ThemeLibrary more;
    more.addTheme("ThemeName = \"Solids\"\nObject.Slideshow.Interval.Duration = 30\nObject.CategoryExpr = \"+Beat\"\n");
    more.addTheme("ThemeName = \"Old\"\nSolid.Slideshow.Interval.Duration = 40\n");  // no longer read
    more.addTheme("ThemeName = \"night\"\nObject.Slideshow.Interval.Duration = 5\n");  // name taken: ignored
    more.addCategory("Object", "CategoryTitle = \"Beat\"\nInclude.List = \"\nDodecahedron Beat\n\"\n");
    lib.merge(more);
    REQUIRE(lib.themes().size() == 3);
    CHECK_FALSE(lib.find("Night")->objectInterval);
    REQUIRE(lib.find("Solids")->objectInterval);
    CHECK(*lib.find("Solids")->objectInterval == 30);
    CHECK_FALSE(lib.find("Old")->objectInterval);
    CHECK(lib.find("Solids")->categoryExpr.count("object"));
    CHECK(lib.category("Object", "Beat"));
    // No CategoryExpr: every object.
    CHECK(lib.filter(lib.find("Old"), "Object", {"A", "B"}).size() == 2);
}

TEST_CASE("solids: reforce.Solid.Dir folder listing") {
    const fs::path dir = fs::temp_directory_path() / "reforce_solid_dir_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "Mine");
    std::ofstream(dir / "Mine" / "Spin.txt") << "Mesh=\"cube\"\n";
    std::ofstream(dir / "_helper.txt") << "\n";
    std::ofstream(dir / "notes.md") << "\n";
    const auto e = listSceneFolder(dir, SceneType::Solid, "Extra/");
    REQUIRE(e.size() == 1);
    CHECK(e[0].id == "Extra/Mine/Spin");
    CHECK(e[0].name == "Spin");
    fs::remove_all(dir);
}

TEST_CASE("content: every committed solid compiles cleanly and evaluates finitely") {
    const auto solids = listScenes(kContent, SceneType::Solid);
    REQUIRE(solids.size() >= 5);
    reforce::scene::Environment env;
    for (const auto& e : solids) {
        CAPTURE(e.id);
        auto cfg = compile(readAll(e.path));
        for (const auto& d : cfg->diags.items()) {
            CAPTURE(d.message);
            CHECK(false);  // no warnings or errors at all
        }
        CHECK(solidMesh(*cfg).has_value());
        CHECK((cfg->find("META") && !cfg->find("META")->text.empty()));  // says what it is
        SolidRun r(cfg, env, e.name, *solidMesh(*cfg), 5.0, 1.0, 30.0, nullptr);
        SpriteAudio a;
        int closeRun = 0;  // consecutive samples right in front of the camera (z = 2.75)
        for (double t = 5.0; t < 40.0; t += 0.37) {
            a.beat = std::fmod(t, 0.5);
            a.sync.on = t > 20 ? 1 : 0;
            a.sync.beat = t * 2.0;
            const SolidSpec s = r.evaluate(t, a);
            for (double v : {s.pose.x, s.pose.y, s.pose.z, s.pose.scale, s.pose.rotX, s.pose.rotY, s.pose.rotZ})
                CHECK(std::isfinite(v));
            CHECK(s.pose.scale > 0.1);
            // In front of the camera (z = 2.75), or flying past it (Prism Flyby): a solid may cross the
            // camera plane, but never hangs just in front of it, where it would fill the frame.
            closeRun = (s.pose.z >= 2.0 && s.pose.z < 3.0) ? closeRun + 1 : 0;
            CHECK(closeRun <= 1);
        }
    }
    // The content's theme turns solids on.
    const ThemeLibrary themes = ThemeLibrary::load(kContent);
    const Theme* t = themes.find("Objects: Solids");
    REQUIRE(t);
    REQUIRE(t->objectInterval);
    CHECK(*t->objectInterval > 0);
}

TEST_CASE("content: Dodecahedron Beat snaps a fifth of a turn on each beat while locked") {
    auto cfg = compile(readAll(kContent / "Packages/reforce.Solid.package/Dodecahedron Beat.txt"));
    reforce::scene::Environment env;
    SolidRun r(cfg, env, "Beat", MeshKind::Dodecahedron, 0.0, 1.0, 30.0, nullptr);
    SpriteAudio a;
    a.sync.on = 1;
    auto rotAt = [&](double t, double beat) {
        a.sync.beat = beat;
        return r.evaluate(t, a).pose.rotY - 0.25 * t;  // without the slow drift
    };
    const double step = 2.0 * M_PI / 5.0;
    CHECK(rotAt(1.0, 12.0) == doctest::Approx(12 * step).epsilon(1e-4));       // on the beat
    CHECK(rotAt(1.1, 12.5) == doctest::Approx(12 * step + step / 32).epsilon(1e-4));  // eased: (1/2)^5
    CHECK(rotAt(1.2, 12.999) == doctest::Approx(13 * step).epsilon(1e-2));    // arrives as the next beat lands
    a.sync.on = 0;
    a.sync.beat = 12.5;
    CHECK(r.evaluate(1.3, a).pose.rotY == doctest::Approx(0.25 * 1.3));  // not locked: the smooth turn only
}

TEST_CASE("content: (Beat Clock) steps through the bar and flashes on each beat; never in a slideshow") {
    CHECK(reforce::engine::excludedFromSlideshow("(Beat Clock)"));
    auto cfg = compile(readAll(kContent / "Packages/reforce.Solid.package/(Beat Clock).txt"));
    reforce::scene::Environment env;
    SolidRun r(cfg, env, "(Beat Clock)", MeshKind::Cube, 0.0, 1.0, 30.0, nullptr);
    SpriteAudio a;
    a.sync.on = 1;
    auto at = [&](double t, double beat) {
        a.sync.beat = beat;
        return r.evaluate(t, a);
    };
    // Beat in the bar -> x: -.6, -.2, .2, .6 (beat 8 = a bar line).
    CHECK(at(1.0, 8.0).pose.x == doctest::Approx(-0.6));
    CHECK(at(1.1, 9.0).pose.x == doctest::Approx(-0.2));
    CHECK(at(1.2, 10.5).pose.x == doctest::Approx(0.2));
    CHECK(at(1.3, 11.9).pose.x == doctest::Approx(0.6));
    // Flash: big and bright on the beat, back to rest after a quarter beat.
    const SolidSpec on = at(1.4, 12.0), later = at(1.5, 12.3);
    CHECK(on.fill == doctest::Approx(1.0));
    CHECK(on.pose.scale == doctest::Approx(0.3));
    CHECK(later.fill == doctest::Approx(0.25));
    CHECK(later.pose.scale == doctest::Approx(0.16));
    // Not locked: small, dim, centered.
    a.sync.on = 0;
    const SolidSpec off = at(1.6, 12.0);
    CHECK(off.pose.x == doctest::Approx(0.0));
    CHECK(off.pose.scale == doctest::Approx(0.12));
    CHECK(off.pen == doctest::Approx(0.3));
}
