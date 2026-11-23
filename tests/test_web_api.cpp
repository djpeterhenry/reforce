// Web API JSON layer (M5b): JSON reader/writer, state/library serialization,
// command parsing and validation. No engine, no sockets.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "web/Api.hpp"
#include "web/Json.hpp"

using namespace reforce;
using engine::CommandType;
using engine::Layer;
namespace fs = std::filesystem;

TEST_CASE("json: parse objects, arrays, escapes, numbers; reject garbage") {
    auto v = web::json::parse(R"( {"a": [1, -2.5e1, true, null], "s": "x\"y\\é😀", "o": {}} )");
    REQUIRE(v);
    REQUIRE(v->get("a"));
    CHECK(v->get("a")->array.size() == 4);
    CHECK(v->get("a")->array[1].num == doctest::Approx(-25));
    CHECK(v->get("a")->array[2].b);
    CHECK(v->get("s")->str == "x\"y\\\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(v->get("o")->isObject());
    CHECK(!v->get("missing"));
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\":1} x", "\"\x01\"", "01x", "tru", "{'a':1}"})
        CHECK_MESSAGE(!web::json::parse(bad), bad);
    std::string deep(100, '[');
    deep += std::string(100, ']');
    std::string err;
    CHECK(!web::json::parse(deep, &err));
    CHECK(err.find("deep") != std::string::npos);
}

TEST_CASE("json: writer escapes and round-trips") {
    web::json::Writer w;
    w.beginObject().field("name", "a\"b\n\x01").field("n", 1.5).field("i", 3).field("b", false);
    w.key("arr").stringArray({"x", "y"});
    w.field("nan", std::nan("")).endObject();
    CHECK(w.str() == R"({"name":"a\"b\n\u0001","n":1.5,"i":3,"b":false,"arr":["x","y"],"nan":null})");
    auto v = web::json::parse(w.str());
    REQUIRE(v);
    CHECK(v->get("name")->str == "a\"b\n\x01");
}

TEST_CASE("state JSON: layers, slideshow summary, stable form omits volatile fields") {
    engine::EngineState s;
    s.simTime = 12.5;
    s.simSteps = 750;
    s.displayFps = 59.9;
    s.theme = "In the Club";
    s.themes = {"Fractal Flow", "In the Club"};
    engine::LayerState ff;
    ff.layer = Layer::FlowField;
    ff.current = "HAL/Crop Spiral";
    ff.target = "Next One";
    ff.transitioning = true;
    ff.progress = 0.25;
    ff.held = true;
    ff.interval = 20;
    ff.transitionDuration = 10;
    ff.nextSwitchIn = 3;
    ff.blendMode = "gforce";
    ff.blendModes = {"gforce", "polar", "crossfade"};
    engine::LayerState ws;
    ws.layer = Layer::WaveShape;
    ws.slideshowEnabled = false;
    s.layers = {ws, ff};
    s.recentLog = {"1.00 hello"};

    auto v = web::json::parse(web::stateToJson(s));
    REQUIRE(v);
    CHECK(v->get("fps")->num == doctest::Approx(59.9));
    CHECK(v->get("theme")->str == "In the Club");
    CHECK(v->get("slideshow")->get("anyOn")->b);
    CHECK(!v->get("slideshow")->get("allOn")->b);
    CHECK(v->get("slideshow")->get("anyHeld")->b);
    const auto& layers = v->get("layers")->array;
    REQUIRE(layers.size() == 2);
    CHECK(layers[1].get("key")->str == "flowfield");
    CHECK(layers[1].get("name")->str == "FlowField");
    CHECK(layers[1].get("current")->str == "HAL/Crop Spiral");
    CHECK(layers[1].get("progress")->num == doctest::Approx(0.25));
    CHECK(layers[1].get("blendModes")->array.size() == 3);
    CHECK(layers[1].get("nextSwitchIn")->num == doctest::Approx(3));
    CHECK(v->get("log")->array[0].str == "1.00 hello");

    // Stable form: same for a later tick with nothing else changed.
    engine::EngineState later = s;
    later.simTime = 99;
    later.simSteps = 9999;
    later.displayFps = 30;
    later.layers[1].nextSwitchIn = 1;
    CHECK(web::stateToJson(s, true) == web::stateToJson(later, true));
    // Transition progress moves every tick: it rides the live event, not the stable form.
    later.layers[1].progress = 0.5;
    CHECK(web::stateToJson(s, true) == web::stateToJson(later, true));
    later.layers[1].transitioning = false;
    CHECK(web::stateToJson(s, true) != web::stateToJson(later, true));
    CHECK(!web::json::parse(web::stateToJson(s, true))->get("simTime"));
    CHECK(!web::json::parse(web::stateToJson(s, true))->get("layers")->array[1].get("progress"));

    // Live event: the fast-changing part only.
    auto live = web::json::parse(web::liveToJson(s));
    REQUIRE(live);
    CHECK(live->get("simTime"));
    CHECK(!live->get("theme"));
    CHECK(!live->get("log"));
    const auto& ll = live->get("layers")->array;
    REQUIRE(ll.size() == 2);
    CHECK(ll[1].get("key")->str == "flowfield");
    CHECK(ll[1].get("progress")->num == doctest::Approx(0.25));
    CHECK(ll[1].get("nextSwitchIn")->num == doctest::Approx(3));
    CHECK(!ll[1].get("current"));
}

TEST_CASE("command JSON: every action maps to the engine command") {
    auto one = [](const char* body) {
        auto r = web::parseCommandJson(body);
        INFO(body << " -> " << r.error);
        REQUIRE(r.ok());
        REQUIRE(r.commands.size() == 1);
        return r.commands[0];
    };
    auto c = one(R"({"layer":"flowfield","action":"switch","config":"HAL/Crop Spiral"})");
    CHECK(c.type == CommandType::Switch);
    CHECK(c.layer == Layer::FlowField);
    CHECK(c.target == engine::SwitchTarget::Name);
    CHECK(c.text == "HAL/Crop Spiral");
    CHECK(c.quick);
    CHECK(!one(R"({"layer":"ws","action":"switch","config":"X","quick":false})").quick);
    c = one(R"({"layer":"waveshape","action":"next"})");
    CHECK((c.type == CommandType::Switch && c.target == engine::SwitchTarget::Next && c.layer == Layer::WaveShape));
    CHECK(one(R"({"layer":"colormap","action":"prev"})").target == engine::SwitchTarget::Previous);
    c = one(R"({"layer":"all","action":"hold"})");
    CHECK((c.type == CommandType::Hold && c.allLayers && c.flag));
    c = one(R"({"layer":"colormap","action":"hold","on":false})");
    CHECK((c.type == CommandType::Hold && !c.allLayers && !c.flag && c.layer == Layer::ColorMap));
    c = one(R"({"layer":"all","action":"unhold"})");
    CHECK((c.type == CommandType::Hold && c.allLayers && !c.flag));
    CHECK(one(R"({"layer":"ff","action":"togglehold"})").type == CommandType::ToggleHold);
    c = one(R"({"layer":"all","action":"slideshow","on":false})");
    CHECK((c.type == CommandType::SetSlideshowEnabled && c.allLayers && !c.flag));
    c = one(R"({"layer":"flowfield","action":"interval","seconds":12.5})");
    CHECK((c.type == CommandType::SetSlideshowInterval && c.value == 12.5));
    c = one(R"({"layer":"sprite","action":"interval","seconds":0})");
    CHECK((c.type == CommandType::SetSlideshowInterval && c.value == 0));  // 0 = off for spawners
    c = one(R"({"layer":"sprite","action":"movieinterval","seconds":300})");
    CHECK((c.type == CommandType::SetMovieInterval && c.value == 300));
    c = one(R"({"action":"spritemix","target":"movie","value":0.75})");
    CHECK((c.type == CommandType::SetSpriteMix && c.text == "movie" && c.value == 0.75 && !c.flag));
    c = one(R"({"action":"spritemix","target":"sprite","value":0})");
    CHECK((c.type == CommandType::SetSpriteMix && c.text == "sprite" && c.value == 0));
    c = one(R"({"layer":"particle","action":"life","seconds":40})");
    CHECK((c.type == CommandType::SetSpawnDuration && c.layer == Layer::Particle && c.value == 40));
    c = one(R"({"layer":"flowfield","action":"duration","seconds":0})");
    CHECK((c.type == CommandType::SetTransitionDuration && c.value == 0));
    c = one(R"({"layer":"colormap","action":"blend","mode":"oklab"})");
    CHECK((c.type == CommandType::SetBlendMode && c.text == "oklab"));
    c = one(R"x({"action":"theme","theme":"(Standard)"})x");
    CHECK((c.type == CommandType::SetTheme && c.text == "(Standard)"));
    c = one(R"({"layer":"waveshape","action":"fader","weight":0.3})");
    CHECK((c.type == CommandType::SetCrossfader && c.value == doctest::Approx(0.3)));
    CHECK(one(R"({"layer":"waveshape","action":"release"})").type == CommandType::ReleaseCrossfader);
    CHECK(one(R"({"layer":"sprite","action":"next"})").layer == Layer::Sprite);
    // Text form goes through engine::parseCommand.
    c = one(R"({"command":"switch:flowfield:Gold/Lust"})");
    CHECK((c.type == CommandType::Switch && c.text == "Gold/Lust" && c.layer == Layer::FlowField));
    // Arrays.
    auto r = web::parseCommandJson(R"([{"layer":"ws","action":"next"},{"layer":"cm","action":"next"}])");
    REQUIRE(r.ok());
    CHECK(r.commands.size() == 2);
}

TEST_CASE("command JSON: validation errors") {
    auto err = [](const char* body, int status = 400) {
        auto r = web::parseCommandJson(body);
        INFO(body);
        CHECK(!r.ok());
        CHECK(r.status == status);
        CHECK(r.commands.empty());
        return r.error;
    };
    err("not json");
    err("42");
    err("[]");
    err(R"({"layer":"ff"})");
    err(R"({"action":"next"})");
    err(R"({"action":"next","layer":"nope"})");
    err(R"({"action":"next","layer":"all"})");               // only hold/slideshow take "all"
    err(R"({"action":"switch","layer":"ff"})");
    err(R"({"action":"switch","layer":"ff","config":""})");
    err(R"({"action":"switch","layer":"ff","config":7})");
    err(R"({"action":"interval","layer":"ff","seconds":"5"})");
    err(R"({"action":"interval","layer":"ff","seconds":0})");
    err(R"({"action":"life","layer":"ff","seconds":30})");
    err(R"({"action":"movieinterval","layer":"particle","seconds":30})");
    err(R"({"action":"life","layer":"sprite","seconds":0})");
    err(R"({"action":"interval","layer":"ff","seconds":1e9})");
    err(R"({"action":"duration","layer":"ff","seconds":-1})");
    err(R"({"action":"fader","layer":"ff","weight":1.5})");
    err(R"({"action":"blend","layer":"ff","mode":"oklab"})");  // ColorMap mode on a FlowField
    err(R"({"action":"blend","layer":"union","mode":"gforce"})");
    err(R"({"action":"slideshow","layer":"ff"})");
    err(R"({"action":"hold","layer":"ff","on":"yes"})");
    err(R"({"action":"explode","layer":"ff"})");
    err(R"({"command":"bogus:ff"})");
    err(R"([{"layer":"ws","action":"next"},{"layer":"ws","action":"nope"}])");  // all or nothing
    std::string big = "[";
    for (int i = 0; i < 17; ++i) big += std::string(i ? "," : "") + R"({"layer":"ws","action":"next"})";
    err((big + "]").c_str());
}

namespace {

struct TempScenes {
    fs::path root = fs::temp_directory_path() / "reforce_web_api_scenes";
    TempScenes() {
        fs::remove_all(root);
        const fs::path ff = root / "Packages/G-Force.FlowField.package";
        const fs::path cm = root / "Packages/G-Force.ColorMap.package";
        fs::create_directories(ff / "HAL");
        fs::create_directories(cm);
        fs::create_directories(root / "Packages/G-Force.WaveShape.package");
        fs::create_directories(root / "Categories/FlowField");
        fs::create_directories(root / "Themes");
        for (const fs::path& p : {ff / "HAL/Crop Spiral.txt", ff / "Twirl.txt", ff / "(Hidden).txt", cm / "Fire.txt"})
            std::ofstream(p) << "x";
        std::ofstream(root / "Categories/FlowField/Spin.txt")
            << "CategoryTitle = \"Spin\"\nInclude.List = \"\nTwirl\nCrop Spiral\n\"\n";
        std::ofstream(root / "Categories/FlowField/Calm.txt") << "CategoryTitle = \"Calm\"\nInclude.Pattern = \"^\\(.*\"\n";
        std::ofstream(root / "Themes/Spinning.txt") << "ThemeName = \"Spinning\"\nFlowField.CategoryExpr = \"+Spin\"\n";
    }
    ~TempScenes() { fs::remove_all(root); }
};

}  // namespace

TEST_CASE("library: configs per type with categories, parenthesized") {
    TempScenes scenes;
    const web::Library lib = web::buildLibrary(scenes.root);
    CHECK(lib.themes == std::vector<std::string>{"g-force: Spinning"});
    const web::LibraryLayer* ff = lib.find(Layer::FlowField);
    REQUIRE(ff);
    CHECK(ff->categories == std::vector<std::string>{"Calm", "Spin"});
    REQUIRE(ff->configs.size() == 3);
    const auto& hidden = ff->configs[0];
    CHECK(hidden.id == "(Hidden)");
    CHECK(hidden.parenthesized);
    CHECK(hidden.categories == std::vector<std::string>{"Calm"});
    CHECK(ff->configs[1].id == "HAL/Crop Spiral");
    CHECK(ff->configs[1].categories == std::vector<std::string>{"Spin"});

    auto v = web::json::parse(web::libraryToJson(lib));
    REQUIRE(v);
    const auto& layers = v->get("layers")->array;
    REQUIRE(layers.size() == 8);  // ... particle, sprite, object (solids and organics), dancer
    CHECK(layers[0].get("key")->str == "flowfield");
    CHECK(layers[6].get("key")->str == "object");
    CHECK(layers[7].get("key")->str == "dancer");
    CHECK(layers[0].get("blendModes")->array.size() == 3);
    CHECK(layers[0].get("configs")->array[0].get("parenthesized")->b);
    CHECK(!layers[0].get("configs")->array[1].get("parenthesized"));

    // Validation against the library: unknown configs/themes are 404.
    CHECK(web::parseCommandJson(R"({"layer":"ff","action":"switch","config":"Twirl"})", &lib).ok());
    CHECK(web::parseCommandJson(R"({"layer":"ff","action":"switch","config":"crop spiral"})", &lib).ok());
    CHECK(web::parseCommandJson(R"({"layer":"ff","action":"switch","config":"hal/crop spiral.txt"})", &lib).ok());
    auto r = web::parseCommandJson(R"({"layer":"ff","action":"switch","config":"Nope"})", &lib);
    CHECK(r.status == 404);
    CHECK(web::parseCommandJson(R"({"action":"theme","theme":"spinning"})", &lib).ok());  // bare G-Force title
    CHECK(web::parseCommandJson(R"({"action":"theme","theme":"g-force: Spinning"})", &lib).ok());
    CHECK(web::parseCommandJson(R"x({"action":"theme","theme":"(Standard)"})x", &lib).ok());
    CHECK(web::parseCommandJson(R"({"action":"theme","theme":"Other"})", &lib).status == 404);
    // A type with no configs in the library is not checked (e.g. Particles before M6).
    CHECK(web::parseCommandJson(R"({"layer":"particle","action":"switch","config":"Any"})", &lib).ok());
}

TEST_CASE("library: solids from reforce's content/ and reforce.Solid.Dir; content themes join") {
    TempScenes scenes;
    const fs::path content = fs::temp_directory_path() / "reforce_web_api_content";
    const fs::path extra = fs::temp_directory_path() / "reforce_web_api_solids";
    fs::remove_all(content);
    fs::remove_all(extra);
    fs::create_directories(content / "Packages/reforce.Solid.package");
    fs::create_directories(content / "Themes");
    fs::create_directories(extra);
    std::ofstream(content / "Packages/reforce.Solid.package/Spin.txt") << "Mesh=\"cube\"\nRotY=\".5 * t\"\n";
    // Beat-synced through a B var (SYNC_BEAT); a commented-out mention does not count.
    std::ofstream(content / "Packages/reforce.Solid.package/Stomp.txt")
        << "Mesh=\"cube\"\n// TEMPO\nB0=\"wrap(SYNC_BEAT)\"\nScal=\".5 + .1 * B0\"\n";
    std::ofstream(content / "Themes/Solids.txt") << "ThemeName = \"Solids\"\nObject.Slideshow.Interval.Duration = 30\n";
    std::ofstream(extra / "Mine.txt") << "Mesh=\"torus\"\n";
    const web::Library lib = web::buildLibrary(scenes.root, {}, {}, content, extra);
    CHECK(lib.themes == std::vector<std::string>{"Solids", "g-force: Spinning"});  // reforce's own first
    const web::LibraryLayer* solids = lib.find(Layer::Object);
    REQUIRE(solids);
    REQUIRE(solids->configs.size() == 3);
    CHECK(solids->configs[0].id == "Spin");
    CHECK(!solids->configs[0].beat);
    CHECK(solids->configs[1].id == "Stomp");
    CHECK(solids->configs[1].beat);
    CHECK(solids->configs[2].id == "Extra/Mine");
    CHECK(!solids->configs[2].beat);
    {
        auto v = web::json::parse(web::libraryToJson(lib));
        REQUIRE(v);
        const auto& cfgs = v->get("layers")->array.at(6).get("configs")->array;
        REQUIRE(cfgs.size() == 3);
        CHECK(!cfgs[0].get("beat"));
        REQUIRE(cfgs[1].get("beat"));
        CHECK(cfgs[1].get("beat")->b);
    }
    CHECK(web::parseCommandJson(R"({"layer":"object","action":"switch","config":"spin"})", &lib).ok());
    CHECK(web::parseCommandJson(R"({"layer":"object","action":"switch","config":"Nope"})", &lib).status == 404);
    CHECK(web::parseCommandJson(R"({"layer":"object","action":"interval","seconds":0})", &lib).ok());
    CHECK(web::parseCommandJson(R"({"layer":"object","action":"life","seconds":20})", &lib).ok());
    fs::remove_all(content);
    fs::remove_all(extra);
}

TEST_CASE("library: organics come in groups (categories), each with the theme that plays just that group") {
    const fs::path content = fs::temp_directory_path() / "reforce_web_api_organics";
    fs::remove_all(content);
    fs::create_directories(content / "Packages/reforce.Organic.package");
    fs::create_directories(content / "Categories/Object");
    fs::create_directories(content / "Themes");
    for (const char* n : {"Oak", "Fern", "Pendulum Wave", "Loner"})
        std::ofstream(content / "Packages/reforce.Organic.package" / (std::string(n) + ".txt")) << "Kind=\"tree\"\n";
    std::ofstream(content / "Categories/Object/1 Trees.txt") << "CategoryTitle = \"Trees\"\nInclude.List = \"\nOak\nFern\n\"\n";
    std::ofstream(content / "Categories/Object/2 Pendulums.txt")
        << "CategoryTitle = \"Pendulums\"\nInclude.List = \"\nPendulum Wave\n\"\n";
    std::ofstream(content / "Themes/Organics_1 Trees.txt")
        << "ThemeName = \"Objects: Trees\"\nObject.CategoryExpr = \"+Trees\"\n";
    const web::Library lib = web::buildLibrary({}, {}, {}, content);
    const web::LibraryLayer* org = lib.find(Layer::Object);
    REQUIRE(org);
    REQUIRE(org->groups.size() == 2);
    CHECK(org->groups[0].name == "Trees");
    CHECK(org->groups[0].theme == "Objects: Trees");
    CHECK(org->groups[1].name == "Pendulums");
    CHECK(org->groups[1].theme.empty());  // no theme for it
    auto v = web::json::parse(web::libraryToJson(lib));
    REQUIRE(v);
    const web::json::Value* layer = nullptr;
    for (const auto& l : v->get("layers")->array)
        if (l.get("key")->str == "object") layer = &l;
    REQUIRE(layer);
    REQUIRE(layer->get("groups"));
    const auto& g = layer->get("groups")->array;
    REQUIRE(g.size() == 2);
    CHECK(g[0].get("name")->str == "Trees");
    CHECK(g[0].get("theme")->str == "Objects: Trees");
    for (const auto& c : layer->get("configs")->array) {
        const std::string id = c.get("id")->str;
        const auto* cats = c.get("categories");
        if (id == "Oak" || id == "Fern") {
            REQUIRE(cats);
            CHECK(cats->array.at(0).str == "Trees");
        } else if (id == "Loner") {
            CHECK(!cats);  // in no group: listed under "Other"
        }
    }
    // Other layers carry no groups.
    for (const auto& l : v->get("layers")->array)
        if (l.get("key")->str != "object") CHECK(!l.get("groups"));
    fs::remove_all(content);
}

TEST_CASE("library: empty scenes dir") {
    const web::Library lib = web::buildLibrary({});
    CHECK(lib.layers.empty());
    CHECK(web::json::parse(web::libraryToJson(lib)));
}

TEST_CASE("state JSON: perf stats (Phase 5D) only when present, never in the stable form") {
    engine::EngineState s;
    auto v = web::json::parse(web::stateToJson(s));
    REQUIRE(v);
    CHECK(!v->get("perf"));
    s.perf.valid = true;
    s.perf.frames = 600;
    s.perf.hitches = 2;
    s.perf.workP99 = 4.5;
    s.perf.budgetMs = 16.67;
    s.perf.histEdges = {1, 2};
    s.perf.hist = {10, 20, 30};
    s.perf.lastHitch = "frame 9";
    v = web::json::parse(web::stateToJson(s));
    REQUIRE(v);
    const auto* p = v->get("perf");
    REQUIRE(p);
    CHECK(p->get("frames")->num == doctest::Approx(600));
    CHECK(p->get("hitches")->num == doctest::Approx(2));
    CHECK(p->get("p99")->num == doctest::Approx(4.5));
    CHECK(p->get("hist")->array.size() == 3);
    CHECK(p->get("lastHitch")->str == "frame 9");
    CHECK(!web::json::parse(web::stateToJson(s, true))->get("perf"));
}

TEST_CASE("video commands (Phase 5F): text form, JSON, validation, state and sources JSON") {
    // Text form (engine::parseCommand).
    auto c = engine::parseCommand("video:layer:file:/tmp/a:b.mov");
    REQUIRE(c);
    CHECK(c->type == CommandType::VideoStart);
    CHECK(c->videoTarget == engine::VideoTarget::Layer);
    CHECK(c->text == "file:/tmp/a:b.mov");
    c = engine::parseCommand("video:sprite:camera");
    REQUIRE(c);
    CHECK(c->videoTarget == engine::VideoTarget::Sprite);
    CHECK(c->text == "camera");
    c = engine::parseCommand("videostop:layer");
    REQUIRE(c);
    CHECK(c->type == CommandType::VideoStop);
    c = engine::parseCommand("videohold");
    REQUIRE(c);
    CHECK(c->type == CommandType::VideoHold);
    {
        auto r = web::parseCommandJson(R"({"action":"videohold"})");
        REQUIRE(r.commands.size() == 1);
        CHECK(r.commands[0].type == CommandType::VideoHold);
    }
    c = engine::parseCommand("videoset:keycolor:#00ff00");
    REQUIRE(c);
    CHECK(c->type == CommandType::VideoSet);
    CHECK(c->text == "keycolor");
    CHECK(c->arg == "#00ff00");
    std::string err;
    CHECK_FALSE(engine::parseCommand("video:window:x", &err));
    CHECK_FALSE(engine::parseCommand("video:layer", &err));
    CHECK(engine::describe(*engine::parseCommand("video:layer:syphon:Arena - Comp")) ==
          "video layer play \"syphon:Arena - Comp\"");

    // JSON.
    auto r = web::parseCommandJson(R"({"action":"video","target":"sprite","source":"syphon:Arena"})");
    REQUIRE(r.ok());
    CHECK(r.commands[0].type == CommandType::VideoStart);
    CHECK(r.commands[0].videoTarget == engine::VideoTarget::Sprite);
    r = web::parseCommandJson(R"({"action":"video","source":"Clip"})");  // target defaults to the layer
    REQUIRE(r.ok());
    CHECK(r.commands[0].videoTarget == engine::VideoTarget::Layer);
    r = web::parseCommandJson(R"([{"action":"videoset","key":"opacity","value":0.5},
                                  {"action":"videoset","key":"syphon","value":false},
                                  {"action":"videostop","target":"layer"}])");
    REQUIRE(r.ok());
    CHECK(r.commands[0].arg == "0.5");
    CHECK(r.commands[1].arg == "off");
    CHECK(r.commands[2].type == CommandType::VideoStop);
    r = web::parseCommandJson(R"({"command":"videoset:blend:screen"})");
    CHECK(r.ok());
    // Validation.
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"opacity","value":3})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"bogus","value":1})").ok());
    // Keys the Visualizer handles (panel toggles): beat sync, full-colour movie sprites (PLAN 5K).
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"beatsync","value":true})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"spritecolour","value":"over"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"spritecolour","value":"off"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"spritecolour","value":"under"})").ok());
    // Syphon alpha (docs/syphon-alpha.md).
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"syphonalpha","value":true})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"keysimilarity","value":80})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"keysmoothness","value":1000})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"syphonalpha","value":"maybe"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"windowvisuals","value":true})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"windowvisuals","value":"maybe"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"keysimilarity","value":0})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"keysmoothness","value":1001})").ok());
    // Camera backdrop (docs/backdrop.md).
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"backdrop","value":"camera:Logitech BRIO"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"backdrop","value":"off"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"backdropmirror","value":true})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"backdropfit","value":"contain"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"backdropopacity","value":0.5})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"backdrop","value":""})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"backdropmirror","value":"maybe"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"backdropfit","value":"tile"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"backdropopacity","value":1.5})").ok());
    // Body tracking (docs/body.md).
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"bodytrack","value":true})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"bodyburn","value":"outline"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"videoset","key":"bodytrails","value":0.7})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"bodyburn","value":"glow"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"bodyfront","value":2})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"bodynothing","value":1})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"videoset","key":"beatsync","value":"maybe"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"command":"videoset:blend:overlay"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"video","target":"window","source":"x"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"video","target":"layer"})").ok());

    // State JSON: the video object; volatile fields only in the full form.
    engine::EngineState s;
    s.video.layerActive = true;
    s.video.layerSource = "Clip";
    s.video.decodeMsAvg = 1.5;
    s.video.sprites = {"Live"};
    const std::string full = web::stateToJson(s), stable = web::stateToJson(s, true);
    CHECK(full.find("\"video\":{\"active\":true,\"source\":\"Clip\"") != std::string::npos);
    CHECK(full.find("\"decodeMsAvg\":1.5") != std::string::npos);
    CHECK(stable.find("decodeMsAvg") == std::string::npos);
    CHECK(stable.find("\"sprites\":[\"Live\"]") != std::string::npos);

    // Sources JSON.
    web::VideoSourceList v;
    v.files.push_back({"Sprites/Movies/Clip", "Clip", "/x/Clip.mov"});
    v.cameras.push_back({"cam-1", "FaceTime"});
    v.syphon.push_back({"Comp", "Arena", "Arena - Comp", false});
    v.syphon.push_back({"reforce", "reforce", "reforce - reforce", true});
    const std::string j = web::videoSourcesToJson(v);
    CHECK(j.find("\"source\":\"file:/x/Clip.mov\"") != std::string::npos);
    CHECK(j.find("\"source\":\"camera:cam-1\"") != std::string::npos);
    CHECK(j.find("\"source\":\"syphon:Arena - Comp\"") != std::string::npos);
    CHECK(j.find("\"self\":true") != std::string::npos);
    CHECK(j.find("\"cameraPermission\":\"ask\"") != std::string::npos);
    // Permission states for the Video card (3 = prompt up, 4 = no usage key in the bundle).
    const char* const names[] = {"ask", "granted", "denied", "waiting", "unavailable"};
    for (int k = 0; k <= 4; ++k) {
        v.cameraPermission = k;
        CHECK(web::videoSourcesToJson(v).find(std::string("\"cameraPermission\":\"") + names[k] + "\"") !=
              std::string::npos);
    }
    s.video.layerWaiting = "waiting for camera permission";
    CHECK(web::stateToJson(s).find("\"waiting\":\"waiting for camera permission\"") != std::string::npos);
}

TEST_CASE("web api: look commands (JSON and text) and look state") {
    auto r = web::parseCommandJson(R"({"action":"look","key":"thinlines","value":"gforce"})");
    REQUIRE(r.ok());
    CHECK(r.commands[0].type == CommandType::Look);
    CHECK(r.commands[0].text == "thinlines");
    CHECK(r.commands[0].arg == "gforce");
    r = web::parseCommandJson(R"({"action":"look","key":"reforce.FlowRounding","value":"nearest"})");
    REQUIRE(r.ok());
    CHECK(r.commands[0].text == "flowrounding");
    r = web::parseCommandJson(R"({"action":"look","key":"aa","value":false})");
    REQUIRE(r.ok());
    CHECK(r.commands[0].arg == "off");
    r = web::parseCommandJson(R"([{"action":"look","op":"ab"},{"action":"look","op":"preset","value":"right"},)"
                              R"({"action":"look","op":"reset"},{"action":"look","op":"save","key":"intensity"}])");
    REQUIRE(r.ok());
    REQUIRE(r.commands.size() == 4);
    CHECK(r.commands[0].text == "ab");
    CHECK((r.commands[1].text == "preset" && r.commands[1].arg == "right"));
    CHECK((r.commands[2].text == "reset" && r.commands[2].arg.empty()));
    CHECK((r.commands[3].text == "save" && r.commands[3].arg == "intensity"));
    r = web::parseCommandJson(R"({"command":"look:intensity:hq"})");
    REQUIRE(r.ok());
    CHECK(r.commands[0].arg == "hq");
    // Rejected
    CHECK(web::parseCommandJson(R"({"action":"look","key":"intensity","value":"32bit"})").status == 400);
    CHECK(!web::parseCommandJson(R"({"action":"look","key":"bogus","value":"x"})").ok());
    CHECK(!web::parseCommandJson(R"({"action":"look","op":"preset","value":"nope"})").ok());
    CHECK(!web::parseCommandJson(R"({"action":"look","op":"dance"})").ok());
    CHECK(!web::parseCommandJson(R"({"action":"look"})").ok());

    // State JSON: current / saved / slots / differs.
    engine::EngineState s;
    CHECK(web::stateToJson(s).find("\"look\"") == std::string::npos);  // not published (offline)
    const auto start = engine::lookFromSettings(engine::Settings{});
    engine::LookControl lc(start, start);
    unsigned m = 0;
    lc.set(engine::LookKey::Intensity, "hq", &m);
    lc.toggleAB();
    lc.toggleAB();  // slot B = done right
    s.look = lc.state();
    s.look.fieldPrecision = "double";
    auto doc = web::json::parse(web::stateToJson(s, true));
    REQUIRE(doc);
    const web::json::Value* look = doc->get("look");
    REQUIRE(look);
    CHECK(look->get("current")->get("intensity")->str == "hq");
    CHECK(look->get("current")->get("thinlines")->str == "smooth");
    CHECK(look->get("saved")->get("intensity")->str == "gforce");
    CHECK(look->get("slot")->str == "B");
    CHECK(look->get("slotA")->get("thinlines")->str == "gforce");
    CHECK(look->get("fieldPrecision")->str == "double");
    std::vector<std::string> differs;
    for (const auto& v : look->get("differs")->array) differs.push_back(v.str);
    CHECK(differs == std::vector<std::string>{"intensity", "flowrounding", "linescale"});
}

TEST_CASE("library: the extra sprite folder (reforce.ExternalSpritesDir) joins the Sprite list") {
    TempScenes scenes;
    const fs::path ext = fs::temp_directory_path() / "reforce_web_api_external";
    fs::remove_all(ext);
    fs::create_directories(ext / "images");
    fs::create_directories(ext / "movies");
    std::ofstream(ext / "images/  dj peter henry.png") << "x";
    std::ofstream(ext / "movies/ clip.mov") << "x";
    std::ofstream(ext / "movies/ clip.reforce.txt") << "Loop = 1";  // options file: not a sprite
    const web::Library lib = web::buildLibrary(scenes.root, "Sprite: clip", ext);
    const web::LibraryLayer* sp = lib.find(engine::Layer::Sprite);
    REQUIRE(sp);
    REQUIRE(sp->configs.size() == 1);  // the movie is on the skip list
    CHECK(sp->configs[0].id == "External/images/  dj peter henry");
    CHECK(sp->configs[0].name == "  dj peter henry");
    // A favourite button's command passes validation against this library.
    const auto r = web::parseCommandJson(R"({"layer":"sprite","action":"switch","config":"External/images/  dj peter henry"})",
                                         &lib);
    CHECK(r.ok());
    fs::remove_all(ext);
}

TEST_CASE("favourites: parse and join") {
    const auto f = web::parseFavorites("sprite:External/images/  dj peter henry;\nWaveShape:Corona; x:y; colormap:");
    REQUIRE(f.size() == 2);
    CHECK(f[0].id == "External/images/  dj peter henry");
    CHECK(f[1].layer == "waveshape");
    CHECK(web::joinFavorites(f) == "sprite:External/images/  dj peter henry;waveshape:Corona");
    const auto o = web::parseFavorites("solid:Rubik Cube;object:Oak");  // solid: is no layer any more
    CHECK(web::joinFavorites(o) == "object:Oak");
}

TEST_CASE("rhythm (PLAN 5O): beatset commands, their settings, and the rhythm state") {
    auto r = web::parseCommandJson(R"({"action":"beatset","key":"quantize","value":"phrase"})");
    REQUIRE(r.ok());
    REQUIRE(r.commands.size() == 1);
    CHECK(r.commands[0].type == CommandType::BeatSet);
    CHECK(r.commands[0].text == "quantize");
    CHECK(r.commands[0].arg == "phrase");
    auto save = engine::slideshowSaveEntries(r.commands[0]);
    CHECK(save.get("reforce.Link.Quantize") == std::optional<std::string>("phrase"));
    r = web::parseCommandJson(R"({"action":"beatset","key":"kickzoom","value":0.012})");
    REQUIRE(r.ok());
    CHECK(engine::slideshowSaveEntries(r.commands[0]).get("reforce.Kick.Zoom") == std::optional<std::string>("0.012"));
    r = web::parseCommandJson(R"({"action":"beatset","key":"phraseauto","value":false})");
    REQUIRE(r.ok());
    CHECK(engine::slideshowSaveEntries(r.commands[0]).get("reforce.Phrase.AutoAlign") == std::optional<std::string>("0"));
    r = web::parseCommandJson(R"({"action":"beatset","key":"phrasenow"})");
    REQUIRE(r.ok());
    CHECK(engine::slideshowSaveEntries(r.commands[0]).toJson() == engine::PrefsTable{}.toJson());  // an action
    CHECK_FALSE(web::parseCommandJson(R"({"action":"beatset","key":"quantize","value":"sometimes"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"beatset","key":"kickzoom","value":5})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"beatset","key":"bogus"})").ok());
    // Text form too.
    CHECK(engine::parseCommand("beatset:phrasenudge:-1")->arg == "-1");
    CHECK_FALSE(engine::parseCommand("beatset:phrasebars:0"));

    engine::EngineState s;
    s.rhythm.phraseOn = true;
    s.rhythm.bars = 8;
    s.rhythm.barInPhrase = 5;
    s.rhythm.how = "auto";
    s.rhythm.quantize = "phrase";
    s.rhythm.kickZoom = 0.01;
    s.rhythm.breakdown = true;
    s.rhythm.build = 0.4;
    auto v = web::json::parse(web::stateToJson(s));
    REQUIRE(v);
    const auto* rh = v->get("rhythm");
    REQUIRE(rh);
    CHECK(rh->get("bars")->num == 8);
    CHECK(rh->get("barInPhrase")->num == 5);
    CHECK(rh->get("how")->str == "auto");
    CHECK(rh->get("quantize")->str == "phrase");
    CHECK(rh->get("kickZoom")->num == doctest::Approx(0.01));
    CHECK(rh->get("breakdown")->b);
    auto live = web::json::parse(web::liveToJson(s));
    REQUIRE(live);
    CHECK(live->get("rhythm")->get("build")->num == doctest::Approx(0.4));
    // The live part is left out of the stable state (a bar change is not a state change).
    engine::EngineState later = s;
    later.rhythm.barInPhrase = 6;
    CHECK(web::stateToJson(s, true) == web::stateToJson(later, true));
}

TEST_CASE("dancers: commands and the Dancers card's settings") {
    using reforce::engine::Layer;
    auto sw = web::parseCommandJson(R"({"layer":"dancer","action":"switch","config":"Ruby"})");
    REQUIRE(sw.ok());
    REQUIRE(sw.commands.size() == 1);
    CHECK(sw.commands[0].layer == Layer::Dancer);
    CHECK(web::parseCommandJson(R"({"command":"kill:dancer"})").ok());
    CHECK(web::parseCommandJson(R"({"layer":"dancer","action":"life","seconds":60})").ok());
    CHECK(web::parseCommandJson(R"({"layer":"dancer","action":"life","seconds":0})").ok());  // forever (∞)
    CHECK_FALSE(web::parseCommandJson(R"({"layer":"object","action":"life","seconds":0})").ok());
    CHECK(web::parseCommandJson(R"({"layer":"dancer","action":"interval","seconds":0})").ok());
    CHECK(web::parseCommandJson(R"({"action":"beatset","key":"dancefloor","value":"on"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"beatset","key":"dancefloor","value":"off"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"beatset","key":"dancelight","value":"0.5"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"beatset","key":"dancefloor","value":"wild"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"beatset","key":"dancelight","value":"2"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"beatset","key":"dancehuman","value":"1.5"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"beatset","key":"dancefloorlook","value":"mirror"})").ok());  // gone
    CHECK(web::parseCommandJson(R"({"action":"beatset","key":"dancemax","value":"12"})").ok());
    CHECK(web::parseCommandJson(R"({"action":"beatset","key":"dancemax","value":"32"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"beatset","key":"dancemax","value":"33"})").ok());
    CHECK_FALSE(web::parseCommandJson(R"({"action":"beatset","key":"dancehuman","value":"3"})").ok());
}
