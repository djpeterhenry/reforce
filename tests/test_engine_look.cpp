// engine/Look.hpp: live rendering-quality choices, A/B slots, save entries,
// and the look:... command text.
#include <doctest/doctest.h>

#include "engine/Commands.hpp"
#include "engine/Look.hpp"

using namespace reforce::engine;

TEST_CASE("look: keys, values and presets") {
    CHECK(parseLookKey("thinlines") == LookKey::ThinLines);
    CHECK(parseLookKey("reforce.Intensity") == LookKey::Intensity);
    CHECK(parseLookKey("WaveShape.AntiAlias") == LookKey::Antialias);
    CHECK(!parseLookKey("bogus"));
    CHECK(normalizeLookValue(LookKey::Antialias, "1") == std::string("on"));
    CHECK(normalizeLookValue(LookKey::Antialias, "false") == std::string("off"));
    CHECK(normalizeLookValue(LookKey::FlowRounding, "Fixed256") == std::string("fixed256"));
    CHECK(!normalizeLookValue(LookKey::Intensity, "32bit"));
    const LookValues g = *lookPreset("gforce"), r = *lookPreset("right");
    CHECK(g[LookKey::ThinLines] == "gforce");
    CHECK(g[LookKey::Intensity] == "gforce");
    CHECK(g[LookKey::FlowRounding] == "fixed16");
    CHECK(r[LookKey::ThinLines] == "smooth");
    CHECK(r[LookKey::Intensity] == "hq");
    CHECK(r[LookKey::FlowRounding] == "nearest");
    CHECK(g[LookKey::FlowPrecision] == r[LookKey::FlowPrecision]);  // flipping never forces a field rebuild
    CHECK(!lookPreset("nope"));
    // Defaults from settings: reforce's current defaults.
    const LookValues d = lookFromSettings(Settings{});
    CHECK(d[LookKey::ThinLines] == "smooth");
    CHECK(d[LookKey::Intensity] == "gforce");
    CHECK(d[LookKey::FlowRounding] == "fixed16");
    CHECK(d[LookKey::FlowPrecision] == "double");
    CHECK(d[LookKey::Antialias] == "on");
}

TEST_CASE("look: control set / A-B / reset / saved") {
    const LookValues start = lookFromSettings(Settings{});
    LookControl lc(start, start);
    unsigned m = 0;
    CHECK(lc.set(LookKey::Intensity, "hq", &m).empty());
    CHECK(m == 1u << int(LookKey::Intensity));
    CHECK(!lc.set(LookKey::Intensity, "bad", &m).empty());
    CHECK(lc.current()[LookKey::Intensity] == "hq");
    // First toggle: to A (G-Force exact) unless already there.
    m = lc.toggleAB();
    CHECK(lc.state().slot == 'A');
    CHECK(lc.current() == *lookPreset("gforce"));
    CHECK((m & (1u << int(LookKey::ThinLines))) != 0);
    m = lc.toggleAB();
    CHECK(lc.state().slot == 'B');
    CHECK(lc.current() == *lookPreset("right"));
    // Edits while on B change slot B; A stays.
    lc.set(LookKey::FlowRounding, "floor", &m);
    lc.toggleAB();
    CHECK(lc.current() == *lookPreset("gforce"));
    lc.toggleAB();
    CHECK(lc.current()[LookKey::FlowRounding] == "floor");
    // Reset to saved (one key, then all).
    lc.resetToSaved(LookKey::ThinLines);
    CHECK(lc.current()[LookKey::ThinLines] == start[LookKey::ThinLines]);
    lc.resetToSaved();
    CHECK(lc.current() == start);
    lc.set(LookKey::Antialias, "off", &m);
    lc.markSaved(LookKey::Antialias, "off");
    CHECK(lc.state().saved[LookKey::Antialias] == "off");
    lc.resetToSaved();
    CHECK(lc.current()[LookKey::Antialias] == "off");
}

TEST_CASE("look: save entries for settings.json") {
    LookValues v = *lookPreset("right");
    v[LookKey::Antialias] = "off";
    const PrefsTable all = lookSaveEntries(v);
    CHECK(all.size() == std::size_t(kLookKeys));
    CHECK(all.string("reforce.Intensity", "") == "hq");
    CHECK(all.entry("reforce.Intensity")->quoted);
    CHECK(all.number("WaveShape.AntiAlias", 1) == 0);
    CHECK(!all.entry("WaveShape.AntiAlias")->quoted);
    const PrefsTable one = lookSaveEntries(v, LookKey::ThinLines);
    CHECK(one.size() == 1);
    CHECK(one.string("reforce.ThinLines", "") == "smooth");
    // Round trip through Settings.
    Settings s;
    s.apply(all);
    CHECK(lookFromSettings(s) == v);
}

TEST_CASE("look: command text") {
    auto c = parseCommand("look:intensity:hq");
    REQUIRE(c);
    CHECK(c->type == CommandType::Look);
    CHECK(c->text == "intensity");
    CHECK(c->arg == "hq");
    c = parseCommand("look:reforce.ThinLines:gforce");
    REQUIRE(c);
    CHECK(c->text == "thinlines");
    CHECK(parseCommand("look:ab"));
    CHECK(parseCommand("look:preset:right"));
    CHECK(parseCommand("look:reset"));
    CHECK(parseCommand("look:reset:aa"));
    CHECK(parseCommand("look:save:flowrounding"));
    CHECK(parseCommand("look:saved:intensity=hq"));
    std::string err;
    CHECK(!parseCommand("look:intensity:32bit", &err));
    CHECK(err.find("gforce | hq") != std::string::npos);
    CHECK(!parseCommand("look:bogus:1"));
    CHECK(!parseCommand("look:preset:nope"));
    CHECK(!parseCommand("look"));
    CHECK(describe(*parseCommand("look:aa:off")) == "look aa = off");
}

TEST_CASE("look: linescale (reforce.LineScale) is a live key; gforce in A, frame in B") {
    CHECK(parseLookKey("linescale") == LookKey::LineScale);
    CHECK(parseLookKey("reforce.LineScale") == LookKey::LineScale);
    CHECK(normalizeLookValue(LookKey::LineScale, "Frame") == std::string("frame"));
    CHECK(!normalizeLookValue(LookKey::LineScale, "half"));
    CHECK((*lookPreset("gforce"))[LookKey::LineScale] == "gforce");
    CHECK((*lookPreset("right"))[LookKey::LineScale] == "frame");
    CHECK(lookInfo(LookKey::LineScale).live);
    Settings s;
    CHECK(lookFromSettings(s)[LookKey::LineScale] == "gforce");
    s.lineScale = "frame";
    CHECK(lookFromSettings(s)[LookKey::LineScale] == "frame");
    const PrefsTable t = lookSaveEntries(*lookPreset("right"), LookKey::LineScale);
    CHECK(t.size() == 1);
    CHECK(t.get("reforce.LineScale") == std::string("frame"));
}

TEST_CASE("look: sprites (reforce.SpriteLayering) is a live key; gforce in both presets") {
    CHECK(parseLookKey("sprites") == LookKey::SpriteLayering);
    CHECK(parseLookKey("reforce.SpriteLayering") == LookKey::SpriteLayering);
    CHECK(normalizeLookValue(LookKey::SpriteLayering, "Trails") == std::string("trails"));
    CHECK(!normalizeLookValue(LookKey::SpriteLayering, "overlay"));
    CHECK((*lookPreset("gforce"))[LookKey::SpriteLayering] == "gforce");
    CHECK((*lookPreset("right"))[LookKey::SpriteLayering] == "gforce");
    CHECK(lookInfo(LookKey::SpriteLayering).live);
    Settings s;
    CHECK(lookFromSettings(s)[LookKey::SpriteLayering] == "gforce");
    s.spriteLayering = "melt";
    CHECK(lookFromSettings(s)[LookKey::SpriteLayering] == "melt");
    s.spriteLayering = "bogus";
    CHECK(lookFromSettings(s)[LookKey::SpriteLayering] == "gforce");
}
