// Hotkey self-test: key events through the same routing the NSView uses
// (platform::routeWindowKey) and App::onKey's mapping (app::hotkeyCommands).
#include <doctest/doctest.h>

#include <map>

#include "app/Hotkeys.hpp"
#include "engine/Blend.hpp"

using namespace reforce;
using engine::Command;
using engine::CommandType;
using engine::Layer;
using engine::SwitchTarget;
using platform::KeyEvent;
using platform::WindowKey;

namespace {

struct Mods {
    bool shift = false, command = false, control = false, option = false;
};

KeyEvent key(const std::string& chars, Mods m = {}, int keyCode = -1) {
    KeyEvent k;
    k.chars = chars;
    k.shift = m.shift;
    k.command = m.command;
    k.control = m.control;
    k.option = m.option;
    k.keyCode = keyCode;
    return k;
}

// What the window does with a key press: keyDown path (performKeyEquivalent first,
// like AppKit), then the App's commands. Blend modes come from `modes`.
struct Result {
    WindowKey route = WindowKey::Unhandled;
    std::vector<Command> commands;
};

Result press(const KeyEvent& k, bool fullscreen = false, std::map<Layer, int> modes = {}) {
    Result r;
    r.route = platform::routeWindowKey(k, fullscreen, /*keyEquivalent=*/true);
    if (r.route == WindowKey::Unhandled) r.route = platform::routeWindowKey(k, fullscreen, false);
    if (r.route == WindowKey::App)
        r.commands = app::hotkeyCommands(k, [&](Layer l) { return modes.count(l) ? modes[l] : 0; });
    return r;
}

std::string one(const Result& r) {
    REQUIRE(r.route == WindowKey::App);
    REQUIRE(r.commands.size() == 1);
    return engine::describe(r.commands[0]);
}

}  // namespace

TEST_CASE("hotkeys: W/C/F switch layers (Shift: previous), quick") {
    const std::pair<const char*, Layer> keys[] = {{"w", Layer::WaveShape}, {"c", Layer::ColorMap}, {"f", Layer::FlowField}};
    for (const auto& [ch, layer] : keys) {
        CAPTURE(ch);
        Result r = press(key(ch));
        REQUIRE(r.route == WindowKey::App);
        REQUIRE(r.commands.size() == 1);
        CHECK(r.commands[0].type == CommandType::Switch);
        CHECK(r.commands[0].layer == layer);
        CHECK(r.commands[0].target == SwitchTarget::Next);
        CHECK(r.commands[0].quick);
        r = press(key(ch, {.shift = true}));
        REQUIRE(r.commands.size() == 1);
        CHECK(r.commands[0].layer == layer);
        CHECK(r.commands[0].target == SwitchTarget::Previous);
    }
    // AppKit reports Shift+W as "w" with Shift (charactersIgnoringModifiers, lower-cased).
    CHECK(one(press(key("w", {.shift = true}))) == engine::describe(Command::previous(Layer::WaveShape)));
}

TEST_CASE("hotkeys: U, H, P/S and their Shift variants") {
    Result u = press(key("u"));
    REQUIRE(u.commands.size() == 1);
    CHECK(u.commands[0].layer == Layer::Union);
    CHECK(u.commands[0].target == SwitchTarget::Next);

    Result h = press(key("h"));
    REQUIRE(h.commands.size() == 1);
    CHECK(h.commands[0].type == CommandType::ToggleHold);
    CHECK(h.commands[0].allLayers);

    for (const auto& [ch, layer] : {std::pair{"p", Layer::Particle}, std::pair{"s", Layer::Sprite}}) {
        CAPTURE(ch);
        Result start = press(key(ch));
        REQUIRE(start.commands.size() == 1);
        CHECK(start.commands[0].type == CommandType::Switch);
        CHECK(start.commands[0].layer == layer);
        CHECK(start.commands[0].quick);  // manual start: Transition.Duration.Quick fade-in
        Result kill = press(key(ch, {.shift = true}));
        REQUIRE(kill.commands.size() == 1);
        CHECK(kill.commands[0].type == CommandType::Kill);
        CHECK(kill.commands[0].layer == layer);
    }
}

TEST_CASE("hotkeys: 1/2/3 cycle the blend mode from the current one") {
    const std::pair<const char*, Layer> keys[] = {{"1", Layer::WaveShape}, {"2", Layer::ColorMap}, {"3", Layer::FlowField}};
    for (const auto& [ch, layer] : keys) {
        CAPTURE(ch);
        const auto& names = engine::blendModeNames(layer);
        REQUIRE(names.size() >= 2);
        for (int cur = 0; cur < int(names.size()); ++cur) {
            Result r = press(key(ch), false, {{layer, cur}});
            REQUIRE(r.commands.size() == 1);
            CHECK(r.commands[0].type == CommandType::SetBlendMode);
            CHECK(r.commands[0].layer == layer);
            CHECK(r.commands[0].text == names[std::size_t(cur + 1) % names.size()]);
        }
    }
}

TEST_CASE("hotkeys: Cmd+Ctrl+F toggles fullscreen, Esc leaves it, chords are not scene keys") {
    Result fs = press(key("f", {.command = true, .control = true}));
    CHECK(fs.route == WindowKey::ToggleFullscreen);
    CHECK(fs.commands.empty());
    CHECK(press(key("f", {.command = true, .control = true}), true).route == WindowKey::ToggleFullscreen);

    CHECK(press(key("\x1b", {}, platform::kKeyCodeEscape), true).route == WindowKey::ExitFullscreen);
    CHECK(press(key("\x1b", {}, platform::kKeyCodeEscape), false).route == WindowKey::Consumed);

    // Cmd+F, Ctrl+F, Option+F, Cmd+Q: not ours (menu / responder chain), no commands.
    for (Mods m : {Mods{.command = true}, Mods{.control = true}, Mods{.option = true}}) {
        Result r = press(key("f", m));
        CHECK(r.commands.empty());
    }
    Result q = press(key("q", {.command = true}));
    CHECK(q.commands.empty());
    CHECK(app::hotkeyCommands(key("q", {.command = true}), {}).empty());

    // Unmapped keys and empty characters (dead keys).
    CHECK(press(key("x")).commands.empty());
    CHECK(press(key("")).route == WindowKey::Unhandled);
}

TEST_CASE("hotkeys: B flips the Look A/B slots") {
    Result b = press(key("b"));
    REQUIRE(b.commands.size() == 1);
    CHECK(b.commands[0].type == CommandType::Look);
    CHECK(b.commands[0].text == "ab");
}

TEST_CASE("hotkeys: [ and ] nudge the sprite Mix (Shift: movie sprites)") {
    CHECK(one(press(key("]"))) == "image sprite mix +0.10");
    CHECK(one(press(key("["))) == "image sprite mix -0.10");
    CHECK(one(press(key("}", {true}))) == "movie sprite mix +0.10");
    CHECK(one(press(key("{", {true}))) == "movie sprite mix -0.10");
}
