// Loader tests. All scene text here is written for these tests (no G-Force content).
#include <doctest/doctest.h>

#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

using namespace reforce::scene;

TEST_CASE("loader: basic quoted and unquoted values") {
    auto f = parseSceneText("Vers=100\nAspc=0\nA0=\"1 + 2\"\nPen=.8\nNum=-3\n");
    CHECK(f.diags.empty());
    REQUIRE(f.entries.size() == 5);
    CHECK(f.find("vers")->value == "100");
    CHECK(f.find("A0")->quoted);
    CHECK(f.find("a0")->value == "1 + 2");
    CHECK_FALSE(f.find("PEN")->quoted);
    CHECK(f.find("pen")->value == ".8");
    CHECK(f.find("NUM")->rawKey == "Num");
    CHECK(f.find("NUM")->value == "-3");
    CHECK(f.find("Pen")->line == 4);
}

TEST_CASE("loader: line endings CR, LF, CRLF and mixed") {
    auto f = parseSceneText("A0=1\r\nA1=2\rA2=3\nA3=\"x\r\n+1\"\r\rA4=5");
    CHECK(f.diags.empty());
    CHECK(f.find("A0")->line == 1);
    CHECK(f.find("A1")->line == 2);
    CHECK(f.find("A2")->line == 3);
    CHECK(f.find("A3")->line == 4);
    CHECK(f.find("A3")->endLine == 5);
    CHECK(f.find("A4")->line == 7);
    CHECK(f.find("A4")->value == "5");
}

TEST_CASE("loader: comments, including quotes inside comments") {
    auto f = parseSceneText(
        "/* header \"with quotes\"\n  spanning */ A0=\"1\" // note \"quoted\"\n"
        "B1=\" A1\"  // * ( 2 + t )\"\n"
        "C0=\"2\"/* no space */C1=\"3\"\n");
    CHECK(f.diags.empty());
    CHECK(f.find("A0")->line == 2);
    CHECK(f.find("B1")->value == " A1");
    CHECK(f.find("C0")->value == "2");
    CHECK(f.find("C1")->value == "3");
}

TEST_CASE("loader: comment markers inside a quoted value stay in the value") {
    auto f = parseSceneText("A0=\"1 // inline\n + 2\"\nA1=\"3 /* x */ + 4\"");
    CHECK(f.diags.empty());
    CHECK(f.find("A0")->value == "1 // inline\n + 2");
    CHECK(f.find("A1")->value == "3 /* x */ + 4");
    // The expression compiler skips them.
    auto cfg = compileConfig(f, SceneType::WaveShape);
    CHECK(cfg.diags.errorCount() == 0);
}

TEST_CASE("loader: commas, trailing commas, spaces around '='") {
    auto f = parseSceneText("Side=5, Cube=.7,\nNUM = \"3\",\nA0 =\"1\" ,\n  LWdt= 2\n");
    CHECK(f.diags.empty());
    CHECK(f.find("SIDE")->value == "5");
    CHECK(f.find("CUBE")->value == ".7");
    CHECK(f.find("NUM")->value == "3");
    CHECK(f.find("A0")->value == "1");
    CHECK(f.find("LWDT")->value == "2");
}

TEST_CASE("loader: several pairs on one line separated by whitespace") {
    auto f = parseSceneText("Vers=270 Aspc=1 ConB=1\tA0=\"t\"");
    CHECK(f.diags.empty());
    CHECK(f.entries.size() == 4);
    CHECK(f.find("ASPC")->value == "1");
}

TEST_CASE("loader: multi-line quoted values") {
    auto f = parseSceneText("B1=\"(A2 + 1) * { cos(B0)\n           , sin(B0)\n           }\"\nPen=1");
    CHECK(f.diags.empty());
    CHECK(f.find("B1")->line == 1);
    CHECK(f.find("B1")->endLine == 3);
    CHECK(f.find("PEN")->line == 4);
}

TEST_CASE("loader: empty values") {
    auto f = parseSceneText("A0=\"\"\nA1=\nA2=\"1\"");
    CHECK(f.find("A0")->value.empty());
    CHECK(f.find("A0")->quoted);
    CHECK(f.find("A1")->value.empty());
    CHECK(f.diags.countKind("empty-value") == 1);
    CHECK(f.diags.errorCount() == 0);
    auto cfg = compileConfig(f, SceneType::WaveShape);
    CHECK_FALSE(cfg.find("A0")->prog.defined);
    CHECK_FALSE(cfg.defines("A1"));
    CHECK(cfg.defines("A2"));
}

TEST_CASE("loader: quoted value on the line after '='") {
    auto f = parseSceneText("A0=\n  \"2\"\nA1=3");
    CHECK(f.find("A0")->value == "2");
    CHECK(f.find("A0")->line == 1);
    CHECK(f.find("A1")->line == 3);
    CHECK(f.diags.countKind("value-on-next-line") == 1);
}

TEST_CASE("loader: duplicate keys - last wins, case-insensitive") {
    auto f = parseSceneText("Pen=\"1\"\nA0=2\npen=\".5\"\n");
    CHECK(f.find("PEN")->value == ".5");
    CHECK(f.find("PEN")->rawKey == "pen");
    CHECK(f.diags.countKind("duplicate-key") == 1);
    CHECK(f.diags.errorCount() == 0);
    CHECK(f.entries.size() == 3);
    CHECK(f.effectiveEntries().size() == 2);
    CHECK(f.entries[0].overridden);
}

TEST_CASE("loader: stray text is a warning") {
    auto f = parseSceneText("Vers=100\nby someone at example dot org //\nw\nA0=1");
    CHECK(f.diags.countKind("stray-text") == 2);
    CHECK(f.diags.errorCount() == 0);
    CHECK(f.find("A0")->value == "1");
}

TEST_CASE("loader: non-UTF-8 bytes in comments") {
    std::string text = "/* caf\xE9 na\xEFve */\nA0=1 // \xA9 2001\n";
    auto f = parseSceneText(text);
    CHECK(f.nonUtf8);
    CHECK(f.diags.empty());
    CHECK(f.find("A0")->value == "1");
    auto utf = parseSceneText("// caf\xC3\xA9\nA0=1");
    CHECK_FALSE(utf.nonUtf8);
    auto bom = parseSceneText("\xEF\xBB\xBFVers=1");
    CHECK(bom.hadBom);
    CHECK(bom.find("VERS"));
}

TEST_CASE("loader: comment tags") {
    auto f = parseSceneText("// #waveshape-particle-only\n/* also #WaveShape-Particle-Also and #custom */\nA0=1");
    CHECK(f.particleOnly);
    CHECK(f.particleAlso);
    CHECK(f.tags == std::vector<std::string>{"waveshape-particle-only", "waveshape-particle-also", "custom"});
    auto g = parseSceneText("A0=\"1\" // see issue&#35 x#y");
    CHECK(g.tags.empty());
}

TEST_CASE("loader: unterminated constructs") {
    auto f = parseSceneText("A0=\"1 + \nA1=2");
    CHECK(f.diags.countKind("unterminated-string") == 1);
    CHECK(f.diags.errorCount() == 1);
    auto g = parseSceneText("A0=1 /* open");
    CHECK(g.diags.countKind("unterminated-comment") == 1);
    CHECK(g.diags.errorCount() == 0);
    auto h = parseSceneText("/* a /* b */ A0=1");
    CHECK(h.diags.countKind("nested-comment") == 1);
    CHECK(h.find("A0"));
}

TEST_CASE("scene types from paths") {
    CHECK(sceneTypeFromPath("scenes/Packages/G-Force.WaveShape.package/Particles/X/y.txt") == SceneType::Particle);
    CHECK(sceneTypeFromPath("scenes/Packages/G-Force.WaveShape.package/WaveShapes/y.txt") == SceneType::WaveShape);
    CHECK(sceneTypeFromPath("/a/G-Force.FlowField Union.package/u.txt") == SceneType::FlowFieldUnion);
    CHECK(sceneTypeFromPath("/a/G-Force.FlowField.package/HAL/u.txt") == SceneType::FlowField);
    CHECK(sceneTypeFromPath("/a/G-Force.ColorMap.package/c.txt") == SceneType::ColorMap);
    CHECK(sceneTypeFromPath("/a/G-Force.Sprite.package/s.txt") == SceneType::Sprite);
    CHECK_FALSE(sceneTypeFromPath("/a/Themes/t.txt").has_value());
}
