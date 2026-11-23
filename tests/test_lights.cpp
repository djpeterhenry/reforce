// Room lights (docs/lights.md): panel mapping, the Nanoleaf packet, state parsing, settings, and the connection
// against a fake Nanoleaf (HTTP + UDP on localhost).
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <httplib.h>

#include "engine/LightsSettings.hpp"
#include "lights/LightMap.hpp"
#include "lights/LightsOutput.hpp"

using namespace reforce;
using namespace reforce::lights;

namespace {

// A w x h BGRA grid filled by f(x, y) -> {r, g, b}.
template <class F>
std::vector<std::uint8_t> grid(int w, int h, F f) {
    std::vector<std::uint8_t> g(std::size_t(w * h * 4));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Rgb c = f(x, y);
            std::uint8_t* p = &g[std::size_t((y * w + x) * 4)];
            p[0] = c.b, p[1] = c.g, p[2] = c.r, p[3] = 255;
        }
    return g;
}

MapSettings plain() {
    MapSettings s;
    s.saturation = 1.0;
    s.release = 0;  // no smoothing
    return s;
}

const char* kStateJson = R"({"name":"Test Panels","model":"NL22","firmwareVersion":"5.3.2",
  "state":{"on":{"value":false},"brightness":{"value":100}},
  "effects":{"select":"Flames","effectsList":["Flames","Forest"]},
  "panelLayout":{"globalOrientation":{"value":59},"layout":{"numPanels":3,"sideLength":150,"positionData":[
    {"panelId":17,"x":0,"y":43,"o":180,"shapeType":0},
    {"panelId":300,"x":449,"y":43,"o":60,"shapeType":0},
    {"panelId":5,"x":899,"y":433,"o":0,"shapeType":0}]}}})";

}  // namespace

TEST_CASE("lights: the v2 packet is count, then id, RGBW and transition per panel, big-endian") {
    std::vector<std::uint8_t> out;
    encodeStreamV2({17, 300}, {Rgb{1, 2, 3}, Rgb{250, 0, 9}}, 1, out);
    const std::vector<std::uint8_t> want = {0, 2, 0, 17, 1, 2, 3, 0, 0, 1, 1, 44, 250, 0, 9, 0, 0, 1};
    CHECK(out == want);
}

TEST_CASE("lights: device state and layout parse") {
    std::string err;
    auto d = parseDeviceState(kStateJson, &err);
    REQUIRE_MESSAGE(d, err);
    CHECK(d->name == "Test Panels");
    CHECK(d->model == "NL22");
    CHECK_FALSE(d->on);
    CHECK(d->effect == "Flames");
    CHECK(d->effects.size() == 2);
    REQUIRE(d->layout.panels.size() == 3);
    CHECK(d->layout.panels[1].id == 300);
    CHECK(d->layout.sideLength == 150);
    CHECK_FALSE(parseDeviceState(R"({"name":"x"})"));
}

TEST_CASE("lights: each panel follows its own part of the picture; layout y points up") {
    LightMapper m;
    Layout l;
    l.panels = {{1, 0, 0}, {2, 1000, 0}, {3, 500, 400}};  // left low, right low, middle high
    m.setLayout(l);
    CHECK(m.u(0) < 0.2);
    CHECK(m.u(1) > 0.8);
    CHECK(m.v(2) < m.v(0));  // higher in the layout = nearer the top of the frame
    // Left third red, right third blue, top middle green.
    const int w = 120, h = 68;
    auto g = grid(w, h, [&](int x, int y) {
        if (x < w / 3) return Rgb{255, 0, 0};
        if (x >= 2 * w / 3) return Rgb{0, 0, 255};
        return y < h / 2 ? Rgb{0, 255, 0} : Rgb{0, 0, 0};
    });
    std::vector<Rgb> out;
    m.update(g.data(), w, h, w * 4, 0.05, plain(), out);
    REQUIRE(out.size() == 3);
    CHECK(out[0] == Rgb{255, 0, 0});
    CHECK(out[1] == Rgb{0, 0, 255});
    CHECK(out[2] == Rgb{0, 255, 0});
}

TEST_CASE("lights: thin bright lines on black still color the panel; black falls to the floor") {
    LightMapper m;
    Layout l;
    l.panels = {{1, 0, 0}};
    m.setLayout(l);
    const int w = 64, h = 36;
    auto lines = grid(w, h, [](int x, int y) { return x % 8 == 0 ? Rgb{255, 128, 0} : Rgb{0, 0, 0}; });
    std::vector<Rgb> out;
    m.update(lines.data(), w, h, w * 4, 0.05, plain(), out);
    CHECK(out[0].r > 80);              // an eighth of the area lit: well above an eighth of full
    CHECK(out[0].g > out[0].r / 3);    // the lines' orange, not gray
    CHECK(out[0].b == 0);
    auto black = grid(w, h, [](int, int) { return Rgb{0, 0, 0}; });
    MapSettings s = plain();
    s.floor = 0.2;
    m.update(black.data(), w, h, w * 4, 0.05, s, out);
    CHECK(out[0].r == 51);  // the floor, white when there is no color at all
}

TEST_CASE("lights: smoothing rises faster than it falls") {
    LightMapper m;
    Layout l;
    l.panels = {{1, 0, 0}};
    m.setLayout(l);
    const int w = 16, h = 9;
    auto white = grid(w, h, [](int, int) { return Rgb{255, 255, 255}; });
    auto black = grid(w, h, [](int, int) { return Rgb{0, 0, 0}; });
    MapSettings s = plain();
    s.release = 0.6;
    std::vector<Rgb> out;
    m.update(white.data(), w, h, w * 4, 0.05, s, out);
    const int up = out[0].r;  // one step up
    m.update(white.data(), w, h, w * 4, 1.0, s, out);  // settle at white
    m.update(black.data(), w, h, w * 4, 0.05, s, out);
    const int down = 255 - out[0].r;  // one step down
    CHECK(up > 2 * down);
}

TEST_CASE("lights: settings keys, ranges and settings.json entries") {
    engine::LightsSettings s;
    CHECK(engine::applyLightsSetting(s, "", "on").empty());
    CHECK(s.enabled);
    CHECK(engine::applyLightsSetting(s, "brightness", "1.5").empty());
    CHECK(s.brightness == 1.5);
    CHECK_FALSE(engine::applyLightsSetting(s, "brightness", "3").empty());
    CHECK_FALSE(engine::applyLightsSetting(s, "", "maybe").empty());
    CHECK_FALSE(engine::applyLightsSetting(s, "color", "1").empty());
    auto e = engine::lightsSettingEntry("saturation", "2");
    REQUIRE(e);
    CHECK(e->first == "reforce.Lights.Saturation");
    CHECK(e->second == "2");
    CHECK(engine::lightsSettingEntry("", "off")->first == "reforce.Lights.Enabled");
    CHECK_FALSE(engine::lightsSettingEntry("floor", "0.9"));
}

TEST_CASE("lights: device config round-trips; missing file = not paired") {
    const auto dir = std::filesystem::temp_directory_path() / ("reforce-lights-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto path = dir / "nanoleaf.json";
    std::string err;
    CHECK_FALSE(loadDeviceConfig(path, &err));
    CHECK(err.find("not paired") != std::string::npos);
    DeviceConfig c;
    c.host = "panels.local";
    c.token = "abc";
    c.udpPort = 5000;
    REQUIRE(saveDeviceConfig(path, c, &err));
    auto back = loadDeviceConfig(path, &err);
    REQUIRE_MESSAGE(back, err);
    CHECK(back->host == "panels.local");
    CHECK(back->token == "abc");
    CHECK(back->port == 16021);
    CHECK(back->udpPort == 5000);
    std::filesystem::remove_all(dir);
}

TEST_CASE("lights: streams to a fake Nanoleaf and puts it back when turned off") {
    // UDP receiver.
    const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(udp >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(udp, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    socklen_t al = sizeof a;
    ::getsockname(udp, reinterpret_cast<sockaddr*>(&a), &al);
    const int udpPort = ntohs(a.sin_port);
    timeval tv{0, 200000};
    ::setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    // HTTP: the device's API under /api/v1/tok.
    httplib::Server srv;
    std::mutex m;
    std::vector<std::string> puts;
    srv.Get("/api/v1/tok/", [](const httplib::Request&, httplib::Response& res) { res.set_content(kStateJson, "application/json"); });
    srv.Get("/api/v1/tok/state/on", [](const httplib::Request&, httplib::Response& res) { res.set_content(R"({"value":true})", "application/json"); });
    srv.Put(R"(/api/v1/tok/(state|effects))", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(m);
        puts.push_back(req.path.substr(12) + " " + req.body);
        res.status = 204;
    });
    const int httpPort = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(httpPort > 0);
    std::thread th([&] { srv.listen_after_bind(); });

    const auto dir = std::filesystem::temp_directory_path() / ("reforce-lights-fake-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    DeviceConfig c;
    c.host = "127.0.0.1";
    c.port = httpPort;
    c.token = "tok";
    c.udpPort = udpPort;
    REQUIRE(saveDeviceConfig(dir / "nanoleaf.json", c));

    {
        LightsOutput out(dir / "nanoleaf.json");
        MapSettings s = plain();
        out.setSettings(s);
        out.setEnabled(true);
        // Wait for streaming, feeding red frames.
        const int w = 32, h = 18;
        auto red = grid(w, h, [](int, int) { return Rgb{255, 0, 0}; });
        std::vector<std::uint8_t> pkt(1024);
        ssize_t got = -1;
        for (int i = 0; i < 40 && got <= 0; ++i) {
            out.submit(red.data(), w, h, w * 4);
            got = ::recv(udp, pkt.data(), pkt.size(), 0);
        }
        REQUIRE(got == 2 + 3 * 8);
        CHECK(pkt[1] == 3);
        CHECK((pkt[2] << 8 | pkt[3]) == 17);
        CHECK(pkt[4] == 255);  // red
        CHECK(pkt[5] == 0);
        CHECK((pkt[18] << 8 | pkt[19]) == 5);  // the third panel's id
        CHECK(out.status().state == "streaming");
        CHECK(out.status().panels == 3);
        // The beat pulse: a confident 120 BPM clock and full pulse. Each beat gets one instant (transition 0) packet at
        // full level, 500 ms apart; the packets between fade.
        s.pulse = 1.0;
        s.pulseLeadMs = 0;
        out.setSettings(s);
        out.setBeat(0.0, 120.0, true);  // the clock's report; the thread extrapolates from it
        std::vector<double> flashes;
        int lo = 255;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1700)) {
            out.submit(red.data(), w, h, w * 4);
            if (::recv(udp, pkt.data(), pkt.size(), 0) <= 0) continue;
            const double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (pkt[8] == 0 && pkt[9] == 0) {  // the first panel's transition: instant = a flash
                CHECK(pkt[4] == 255);
                flashes.push_back(at);
            } else {
                lo = std::min(lo, int(pkt[4]));
            }
        }
        CHECK(lo < 60);  // faded well down between beats
        REQUIRE(flashes.size() >= 3);
        for (std::size_t i = 1; i < flashes.size(); ++i)
            CHECK(flashes[i] - flashes[i - 1] == doctest::Approx(0.5).epsilon(0.05));
        CHECK(out.status().pulsing);
        CHECK(out.panels()[0].o == 180);  // the layout's orientation, for the preview
        CHECK(out.panels().size() == 3);
        {
            std::lock_guard<std::mutex> lk(m);
            REQUIRE(puts.size() >= 2);
            CHECK(puts[0] == R"(state {"on":{"value":true}})");
            CHECK(puts[1].find("extControl") != std::string::npos);
            CHECK(puts[1].find("v2") != std::string::npos);
        }
        out.setEnabled(false);
        for (int i = 0; i < 50 && out.status().state != "off"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(out.status().state == "off");
        std::lock_guard<std::mutex> lk(m);
        REQUIRE(puts.size() >= 4);
        CHECK(puts[puts.size() - 2] == R"(effects {"select":"Flames"})");  // the saved effect
        CHECK(puts.back() == R"(state {"on":{"value":false}})");          // and off, as it was
    }
    srv.stop();
    th.join();
    ::close(udp);
    std::filesystem::remove_all(dir);
}

TEST_CASE("lights: not paired is reported, not retried noisily") {
    const auto dir = std::filesystem::temp_directory_path() / ("reforce-lights-np-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    LightsOutput out(dir / "nanoleaf.json");
    out.setEnabled(true);
    for (int i = 0; i < 50 && out.status().state != "not paired"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(out.status().state == "not paired");
    out.shutdown();
    std::filesystem::remove_all(dir);
}

TEST_CASE("lights: the pulse is full on the beat and falls to 1 - amount") {
    CHECK(pulseFactor(0.0, 1.0) == doctest::Approx(1.0));
    CHECK(pulseFactor(3.0, 0.5) == doctest::Approx(1.0));          // any whole beat
    CHECK(pulseFactor(0.5, 1.0) < 0.15);                           // half a beat later: mostly faded
    CHECK(pulseFactor(0.99, 0.4) == doctest::Approx(0.6).epsilon(0.02));
    CHECK(pulseFactor(0.3, 0.0) == doctest::Approx(1.0));          // off
    CHECK(pulseFactor(-0.25, 1.0) == doctest::Approx(pulseFactor(0.75, 1.0)));
}

TEST_CASE("lights: pairing asks until the button is held, then saves the token") {
    httplib::Server srv;
    std::atomic<int> asks{0};
    srv.Post("/api/v1/new", [&](const httplib::Request&, httplib::Response& res) {
        if (++asks < 2) {
            res.status = 403;  // not in pairing mode yet
            return;
        }
        res.set_content(R"({"auth_token":"fresh"})", "application/json");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });
    const auto dir = std::filesystem::temp_directory_path() / ("reforce-lights-pair-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    DeviceConfig old;  // a re-pair: the paired host is asked again
    old.host = "127.0.0.1";
    old.port = port;
    old.token = "stale";
    REQUIRE(saveDeviceConfig(dir / "nanoleaf.json", old));
    {
        LightsOutput out(dir / "nanoleaf.json");
        out.startPairing();
        for (int i = 0; i < 300 && out.status().message.rfind("paired", 0) != 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(out.status().message == "paired with 127.0.0.1");
        CHECK(out.status().state == "off");
    }
    auto c = loadDeviceConfig(dir / "nanoleaf.json");
    REQUIRE(c);
    CHECK(c->token == "fresh");
    CHECK(asks.load() >= 2);
    srv.stop();
    th.join();
    std::filesystem::remove_all(dir);
}
