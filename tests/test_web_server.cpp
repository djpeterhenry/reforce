// Web server integration (M5b): a real cpp-httplib server on an ephemeral
// loopback port with a fake state provider and a bare CommandQueue (no engine).

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <httplib.h>

#include "web/Json.hpp"
#include "web/WebServer.hpp"

using namespace reforce;
using engine::Layer;

namespace {

struct FakeEngine {
    std::mutex m;
    engine::EngineState s;
    FakeEngine() {
        s.theme = "(Standard)";
        s.displayFps = 60;
        for (Layer l : {Layer::WaveShape, Layer::ColorMap, Layer::FlowField, Layer::Union}) {
            engine::LayerState ls;
            ls.layer = l;
            ls.current = std::string("cur-") + engine::layerName(l);
            s.layers.push_back(ls);
        }
    }
    engine::EngineState snapshot() {
        std::lock_guard<std::mutex> lock(m);
        return s;
    }
};

std::shared_ptr<web::Library> fakeLibrary() {
    auto lib = std::make_shared<web::Library>();
    web::LibraryLayer ff;
    ff.layer = Layer::FlowField;
    ff.categories = {"Spin"};
    ff.configs.push_back({"HAL/Crop Spiral", "Crop Spiral", false, {"Spin"}, {}});
    ff.configs.push_back({"(Hidden)", "(Hidden)", true, {}, {}});
    lib->layers.push_back(ff);
    lib->themes = {"Club"};
    return lib;
}

web::WebServerOptions testOptions(std::string host = "127.0.0.1", std::string token = {}) {
    web::WebServerOptions o;
    o.host = std::move(host);
    o.port = 0;
    o.token = std::move(token);
    o.ssePollSeconds = 0.02;
    return o;
}

}  // namespace

TEST_CASE("web server: state, library, UI, command -> CommandQueue") {
    FakeEngine eng;
    engine::CommandQueue queue;
    web::WebServer server(testOptions(), queue, [&] { return eng.snapshot(); }, fakeLibrary());
    std::string err;
    REQUIRE_MESSAGE(server.start(&err), err);
    REQUIRE(server.port() > 0);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);

    auto res = cli.Get("/api/state");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->get_header_value("Content-Type") == "application/json");
    auto st = web::json::parse(res->body);
    REQUIRE(st);
    CHECK(st->get("layers")->array.size() == 4);
    CHECK(st->get("layers")->array[2].get("current")->str == "cur-FlowField");
    CHECK(st->get("fps")->num == 60);

    res = cli.Get("/api/library");
    REQUIRE(res);
    CHECK(res->status == 200);
    auto lib = web::json::parse(res->body);
    REQUIRE(lib);
    CHECK(lib->get("layers")->array[0].get("configs")->array[0].get("id")->str == "HAL/Crop Spiral");

    res = cli.Get("/");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->get_header_value("Content-Type").rfind("text/html", 0) == 0);
    CHECK(res->body.find("<!doctype html>") != std::string::npos);
    for (const auto& name : web::uiAssetNames()) {
        auto a = cli.Get("/" + name);
        REQUIRE(a);
        CHECK_MESSAGE(a->status == 200, name);
    }
    res = cli.Get("/nope.js");
    REQUIRE(res);
    CHECK(res->status == 404);

    // Commands land in the queue (nothing else touched).
    res = cli.Post("/api/command", R"({"layer":"flowfield","action":"switch","config":"HAL/Crop Spiral"})",
                   "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    res = cli.Post("/api/command", R"([{"layer":"all","action":"hold"},{"layer":"cm","action":"blend","mode":"oklab"}])",
                   "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    auto cmds = queue.drain();
    REQUIRE(cmds.size() == 3);
    CHECK(cmds[0].type == engine::CommandType::Switch);
    CHECK(cmds[0].layer == Layer::FlowField);
    CHECK(cmds[0].text == "HAL/Crop Spiral");
    CHECK((cmds[1].type == engine::CommandType::Hold && cmds[1].allLayers));
    CHECK(cmds[2].text == "oklab");

    // Rejected: bad JSON, unknown config (404), wrong content type (415). Nothing queued.
    res = cli.Post("/api/command", "{nope", "application/json");
    REQUIRE(res);
    CHECK(res->status == 400);
    res = cli.Post("/api/command", R"({"layer":"ff","action":"switch","config":"Missing"})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 404);
    CHECK(web::json::parse(res->body)->get("error")->str.find("not found") != std::string::npos);
    res = cli.Post("/api/command", R"({"layer":"ff","action":"next"})", "text/plain");
    REQUIRE(res);
    CHECK(res->status == 415);
    CHECK(queue.drain().empty());

    // DNS-rebinding guard: a loopback server refuses foreign Host headers.
    res = cli.Get("/api/state", httplib::Headers{{"Host", "evil.example:8411"}});
    REQUIRE(res);
    CHECK(res->status == 403);
    res = cli.Get("/api/state", httplib::Headers{{"Host", "localhost:8411"}});
    REQUIRE(res);
    CHECK(res->status == 200);

    server.stop();
    CHECK(!server.running());
}

TEST_CASE("web server: SSE pushes state on change") {
    FakeEngine eng;
    engine::CommandQueue queue;
    web::WebServer server(testOptions(), queue, [&] { return eng.snapshot(); }, nullptr);
    REQUIRE(server.start());
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(5);

    std::string buf;
    std::vector<std::string> events;
    std::atomic<bool> changed{false};
    auto t0 = std::chrono::steady_clock::now();
    auto res = cli.Get("/api/events", [&](const char* data, size_t n) {
        buf.append(data, n);
        std::size_t pos;
        while ((pos = buf.find("\n\n")) != std::string::npos) {
            const std::string ev = buf.substr(0, pos);
            buf.erase(0, pos + 2);
            if (ev.rfind("data: ", 0) == 0) events.push_back(ev.substr(6));
        }
        if (events.size() == 1 && !changed) {
            std::lock_guard<std::mutex> lock(eng.m);
            eng.s.layers[0].current = "changed";
            changed = true;
        }
        const bool timeout = std::chrono::steady_clock::now() - t0 > std::chrono::seconds(4);
        return events.size() < 2 && !timeout;  // false cancels the stream
    });
    REQUIRE(events.size() >= 2);
    auto first = web::json::parse(events[0]);
    auto second = web::json::parse(events[1]);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->get("layers")->array[0].get("current")->str == "cur-WaveShape");
    CHECK(second->get("layers")->array[0].get("current")->str == "changed");
    // Pushed on change, well before the 2 s refresh.
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500));
    server.stop();
}

TEST_CASE("web server: LAN mode requires the token on /api routes") {
    FakeEngine eng;
    engine::CommandQueue queue;
    const std::string token = web::generateToken();
    CHECK(token.size() == 32);
    CHECK(token != web::generateToken());
    // 0.0.0.0 like --ui-lan, reached over loopback here.
    web::WebServer server(testOptions("0.0.0.0", token), queue, [&] { return eng.snapshot(); }, fakeLibrary());
    REQUIRE(server.start());
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(5);

    auto res = cli.Get("/api/state");
    REQUIRE(res);
    CHECK(res->status == 401);
    res = cli.Get("/api/state?token=wrong");
    REQUIRE(res);
    CHECK(res->status == 401);
    res = cli.Get("/api/library?token=" + token);
    REQUIRE(res);
    CHECK(res->status == 200);
    res = cli.Get("/api/state", httplib::Headers{{"X-Reforce-Token", token}});
    REQUIRE(res);
    CHECK(res->status == 200);
    res = cli.Get("/api/state", httplib::Headers{{"Authorization", "Bearer " + token}});
    REQUIRE(res);
    CHECK(res->status == 200);
    // Non-loopback Host is fine in LAN mode (the token protects it).
    res = cli.Get("/api/state?token=" + token, httplib::Headers{{"Host", "192.168.1.20:8411"}});
    REQUIRE(res);
    CHECK(res->status == 200);

    res = cli.Post("/api/command", R"({"layer":"ws","action":"next"})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 401);
    CHECK(queue.drain().empty());
    res = cli.Post("/api/command?token=" + token, R"({"layer":"ws","action":"next"})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(queue.drain().size() == 1);
    res = cli.Get("/api/events");
    REQUIRE(res);
    CHECK(res->status == 401);

    // The page itself loads without the token (it reads ?token= from its URL).
    res = cli.Get("/");
    REQUIRE(res);
    CHECK(res->status == 200);
    server.stop();
}

TEST_CASE("web server: port in use is reported, not fatal") {
    FakeEngine eng;
    engine::CommandQueue queue;
    web::WebServer a(testOptions(), queue, [&] { return eng.snapshot(); }, nullptr);
    REQUIRE(a.start());
    web::WebServerOptions o = testOptions();
    o.port = a.port();
    web::WebServer b(o, queue, [&] { return eng.snapshot(); }, nullptr);
    std::string err;
    CHECK(!b.start(&err));
    CHECK(err.find("bind") != std::string::npos);
}

TEST_CASE("web server: Look save writes through the saver and queues look:saved") {
    FakeEngine eng;
    eng.s.look = engine::LookControl(engine::lookFromSettings(engine::Settings{}),
                                     engine::lookFromSettings(engine::Settings{})).state();
    engine::CommandQueue queue;
    web::WebServer server(testOptions(), queue, [&] { return eng.snapshot(); }, fakeLibrary());
    std::vector<engine::PrefsTable> saved;
    std::string failWith;
    server.setLookSaver([&](const engine::PrefsTable& t) {
        saved.push_back(t);
        return failWith;
    });
    std::string err;
    REQUIRE_MESSAGE(server.start(&err), err);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(5);

    // A change and its save in one request: the save sees the change.
    auto res = cli.Post("/api/command",
                        R"([{"action":"look","key":"intensity","value":"hq"},{"action":"look","op":"save","key":"intensity"}])",
                        "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].size() == 1);
    CHECK(saved[0].string("reforce.Intensity", "") == "hq");
    auto cmds = queue.drain();
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0].text == "intensity");
    CHECK((cmds[1].text == "saved" && cmds[1].arg == "intensity=hq"));

    // Save all: every key, as the engine's state has them.
    res = cli.Post("/api/command", R"({"action":"look","op":"save"})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    REQUIRE(saved.size() == 2);
    CHECK(saved[1].size() == std::size_t(engine::kLookKeys));
    CHECK(saved[1].number("WaveShape.AntiAlias", 0) == 1);
    CHECK(queue.drain().size() == std::size_t(engine::kLookKeys));

    // Saver error: 500, nothing queued.
    failWith = "disk full";
    res = cli.Post("/api/command", R"({"action":"look","op":"save","key":"aa"})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 500);
    CHECK(web::json::parse(res->body)->get("error")->str.find("disk full") != std::string::npos);
    CHECK(queue.drain().empty());
}

TEST_CASE("web server: slideshow settings from the panel are saved and applied") {
    FakeEngine eng;
    engine::CommandQueue queue;
    web::WebServer server(testOptions(), queue, [&] { return eng.snapshot(); }, fakeLibrary());
    std::vector<engine::PrefsTable> saved;
    std::string failWith;
    server.setLookSaver([&](const engine::PrefsTable& t) {
        saved.push_back(t);
        return failWith;
    });
    std::string err;
    REQUIRE_MESSAGE(server.start(&err), err);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(5);
    auto res = cli.Post("/api/command",
                        R"([{"layer":"sprite","action":"movieinterval","seconds":300},{"layer":"particle","action":"life","seconds":40}])",
                        "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].number("reforce.MovieSprite.Slideshow.Interval.Duration", -1) == 300);
    CHECK(saved[0].number("Particle.Duration", -1) == 40);
    CHECK(queue.drain().size() == 2);
    // Not saveable: nothing written.
    res = cli.Post("/api/command", R"({"layer":"sprite","action":"next"})", "application/json");
    REQUIRE(res);
    CHECK(saved.size() == 1);
    queue.drain();
    // A failed save still applies the change and says so.
    failWith = "disk full";
    res = cli.Post("/api/command", R"({"layer":"wave","action":"interval","seconds":30})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    INFO(res->body);
    const auto body = web::json::parse(res->body);
    REQUIRE(body);
    const auto* se = body->get("saveError");
    REQUIRE(se);
    CHECK(se->str == "disk full");
    CHECK(queue.drain().size() == 1);
}

TEST_CASE("web server: Look save without a saver answers 501") {
    FakeEngine eng;
    engine::CommandQueue queue;
    web::WebServer server(testOptions(), queue, [&] { return eng.snapshot(); }, fakeLibrary());
    std::string err;
    REQUIRE_MESSAGE(server.start(&err), err);
    httplib::Client cli("127.0.0.1", server.port());
    auto res = cli.Post("/api/command", R"({"action":"look","op":"save"})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 501);
    res = cli.Post("/api/command", R"({"command":"look:ab"})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(queue.drain().size() == 1);
}

TEST_CASE("web server: favorites (GET/POST /api/favorites, saved through the saver)") {
    FakeEngine eng;
    engine::CommandQueue queue;
    web::WebServer server(testOptions(), queue, [&] { return eng.snapshot(); }, fakeLibrary());
    std::vector<std::string> saved;
    server.setFavorites("sprite:  dj peter henry; bogus:x; flowfield:HAL/Crop Spiral; sprite:  dj peter henry",
                        [&](const std::string& t) {
                            saved.push_back(t);
                            return std::string();
                        });
    std::string err;
    REQUIRE_MESSAGE(server.start(&err), err);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(5);

    auto res = cli.Get("/api/favorites");
    REQUIRE(res);
    auto doc = web::json::parse(res->body);
    REQUIRE(doc);
    CHECK(doc->get("canSave")->b);
    const auto& favs = doc->get("favorites")->array;
    REQUIRE(favs.size() == 2);  // unknown layer and the duplicate dropped
    CHECK(favs[0].get("layer")->str == "sprite");
    CHECK(favs[0].get("id")->str == "  dj peter henry");  // leading spaces kept

    res = cli.Post("/api/favorites", R"({"layer":"waveshape","id":"WaveShapes/Corona","on":true})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    REQUIRE(saved.size() == 1);
    CHECK(saved[0] == "sprite:  dj peter henry;flowfield:HAL/Crop Spiral;waveshape:WaveShapes/Corona");
    res = cli.Post("/api/favorites", R"({"layer":"sprite","id":"  dj peter henry","on":false})", "application/json");
    REQUIRE(res);
    CHECK(saved.back() == "flowfield:HAL/Crop Spiral;waveshape:WaveShapes/Corona");
    res = cli.Post("/api/favorites", R"({"layer":"nope","id":"x","on":true})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 400);
    res = cli.Post("/api/favorites", R"({"layer":"sprite","id":"a;b","on":true})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 400);
    CHECK(saved.size() == 2);
    CHECK(queue.drain().empty());  // favorites never queue engine commands
    server.stop();
}

TEST_CASE("web server: hidden sprites (GET/POST /api/sprites/hidden) and sprite images") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("reforce-hidden-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        std::ofstream(dir / "Rose.jpg", std::ios::binary) << "JPEGDATA";
    }
    auto lib = fakeLibrary();
    web::LibraryLayer sp;
    sp.layer = Layer::Sprite;
    web::LibraryEntry rose{"Abstract/Rose", "Rose", false, {}, {}};
    rose.path = dir / "Rose.jpg";
    web::LibraryEntry movie{"External/movies/ dance", " dance", false, {}, {}};
    movie.movie = true;
    movie.path = dir / "dance.mov";
    sp.configs = {rose, movie};
    lib->layers.push_back(sp);

    FakeEngine eng;
    engine::CommandQueue queue;
    web::WebServer server(testOptions(), queue, [&] { return eng.snapshot(); }, lib);
    std::vector<std::string> saved;
    server.setSpriteHidden("Space/Comet", [&](const std::string& t) {
        saved.push_back(t);
        return std::string();
    });
    std::string err;
    REQUIRE_MESSAGE(server.start(&err), err);
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(5);

    auto res = cli.Get("/api/sprites/hidden");
    REQUIRE(res);
    auto doc = web::json::parse(res->body);
    REQUIRE(doc);
    CHECK(doc->get("canSave")->b);
    REQUIRE(doc->get("hidden")->array.size() == 1);

    res = cli.Post("/api/sprites/hidden", R"({"ids":["Abstract/Rose","Abstract/Iris"],"on":true})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    REQUIRE(saved.size() == 1);
    CHECK(saved[0] == "Space/Comet;Abstract/Rose;Abstract/Iris");
    auto cmds = queue.drain();
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].type == engine::CommandType::SetSpriteHidden);
    CHECK(cmds[0].text == saved[0]);

    res = cli.Post("/api/sprites/hidden", R"({"ids":["abstract/rose","Space/Comet"],"on":false})", "application/json");
    REQUIRE(res);
    CHECK(saved.back() == "Abstract/Iris");
    doc = web::json::parse(res->body);
    REQUIRE(doc);
    REQUIRE(doc->get("hidden")->array.size() == 1);
    CHECK(queue.drain().size() == 1);

    for (const char* bad : {R"({"ids":[],"on":true})", R"({"ids":["a;b"],"on":true})", R"({"ids":["x"]})",
                            R"({"ids":"x","on":true})"}) {
        res = cli.Post("/api/sprites/hidden", bad, "application/json");
        REQUIRE(res);
        CHECK(res->status == 400);
    }
    CHECK(saved.size() == 2);

    // Images: library ids only (any case), never movies or paths.
    res = cli.Get("/api/sprites/image?id=abstract%2Frose");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body == "JPEGDATA");
    CHECK(res->get_header_value("Content-Type") == "image/jpeg");
    CHECK(res->get_header_value("Cache-Control") == "private, max-age=600");
    for (const char* bad : {"/api/sprites/image?id=External%2Fmovies%2F%20dance", "/api/sprites/image?id=..%2F..%2Fetc%2Fpasswd",
                            "/api/sprites/image"}) {
        res = cli.Get(bad);
        REQUIRE(res);
        CHECK(res->status == 404);
    }
    server.stop();
    fs::remove_all(dir);
}
