#include "engine/Commands.hpp"
#include "engine/Dancers.hpp"

#include "engine/Body.hpp"
#include "engine/LightsSettings.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace reforce::engine {

Command Command::switchTo(Layer l, std::string name, bool quick) {
    Command c;
    c.type = CommandType::Switch;
    c.layer = l;
    c.target = SwitchTarget::Name;
    c.text = std::move(name);
    c.quick = quick;
    return c;
}
Command Command::morphTo(Layer l, std::string name, double seconds) {
    Command c = switchTo(l, std::move(name), true);
    c.seconds = seconds < 0 ? 0 : seconds;
    return c;
}
Command Command::next(Layer l, bool quick) {
    Command c;
    c.type = CommandType::Switch;
    c.layer = l;
    c.target = SwitchTarget::Next;
    c.quick = quick;
    return c;
}
Command Command::previous(Layer l, bool quick) {
    Command c = next(l, quick);
    c.target = SwitchTarget::Previous;
    return c;
}
Command Command::hold(Layer l, bool on) {
    Command c;
    c.type = CommandType::Hold;
    c.layer = l;
    c.flag = on;
    return c;
}
Command Command::holdAll(bool on) {
    Command c = hold(Layer::WaveShape, on);
    c.allLayers = true;
    return c;
}
Command Command::toggleHold(Layer l) {
    Command c;
    c.type = CommandType::ToggleHold;
    c.layer = l;
    return c;
}
Command Command::toggleHoldAll() {
    Command c = toggleHold(Layer::WaveShape);
    c.allLayers = true;
    return c;
}
Command Command::slideshow(Layer l, bool on) {
    Command c;
    c.type = CommandType::SetSlideshowEnabled;
    c.layer = l;
    c.flag = on;
    return c;
}
Command Command::slideshowAll(bool on) {
    Command c = slideshow(Layer::WaveShape, on);
    c.allLayers = true;
    return c;
}
Command Command::interval(Layer l, double seconds) {
    Command c;
    c.type = CommandType::SetSlideshowInterval;
    c.layer = l;
    c.value = seconds;
    return c;
}
Command Command::spawnDuration(Layer l, double seconds) {
    Command c;
    c.type = CommandType::SetSpawnDuration;
    c.layer = l;
    c.value = seconds;
    return c;
}
Command Command::movieInterval(double seconds) {
    Command c;
    c.type = CommandType::SetMovieInterval;
    c.layer = Layer::Sprite;
    c.value = seconds;
    return c;
}
Command Command::spriteMix(bool movie, double value, bool relative) {
    Command c;
    c.type = CommandType::SetSpriteMix;
    c.layer = Layer::Sprite;
    c.text = movie ? "movie" : "sprite";
    c.value = value;
    c.flag = relative;
    return c;
}
Command Command::transitionDuration(Layer l, double seconds) {
    Command c;
    c.type = CommandType::SetTransitionDuration;
    c.layer = l;
    c.value = seconds;
    return c;
}
Command Command::blendMode(Layer l, std::string mode) {
    Command c;
    c.type = CommandType::SetBlendMode;
    c.layer = l;
    c.text = std::move(mode);
    return c;
}
Command Command::theme(std::string name) {
    Command c;
    c.type = CommandType::SetTheme;
    c.text = std::move(name);
    return c;
}
Command Command::crossfader(Layer l, double weight) {
    Command c;
    c.type = CommandType::SetCrossfader;
    c.layer = l;
    c.value = weight;
    return c;
}
Command Command::releaseCrossfader(Layer l) {
    Command c;
    c.type = CommandType::ReleaseCrossfader;
    c.layer = l;
    return c;
}

Command Command::spriteHidden(std::string idList) {
    Command c;
    c.type = CommandType::SetSpriteHidden;
    c.layer = Layer::Sprite;
    c.text = std::move(idList);
    return c;
}

Command Command::kill(Layer l) {
    Command c;
    c.type = CommandType::Kill;
    c.layer = l;
    return c;
}

Command Command::videoStart(VideoTarget target, std::string source) {
    Command c;
    c.type = CommandType::VideoStart;
    c.layer = target == VideoTarget::Sprite ? Layer::Sprite : Layer::WaveShape;
    c.videoTarget = target;
    c.text = std::move(source);
    return c;
}
Command Command::videoStop(VideoTarget target) {
    Command c = videoStart(target, {});
    c.type = CommandType::VideoStop;
    return c;
}
Command Command::videoHold() {
    Command c = videoStart(VideoTarget::Sprite, {});
    c.type = CommandType::VideoHold;
    return c;
}
Command Command::videoSet(std::string key, std::string value) {
    Command c;
    c.type = CommandType::VideoSet;
    c.text = std::move(key);
    c.arg = std::move(value);
    return c;
}

Command Command::beatSet(std::string key, std::string value) {
    Command c;
    c.type = CommandType::BeatSet;
    for (char ch : key) c.text.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
    // Settings are case-insensitive; a dancer's name and the words of a speech bubble are not.
    if (c.text == "dancesay" || c.text == "dancetry" || c.text == "guestcall") c.arg = value;
    else
        for (char ch : value) c.arg.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
    return c;
}

std::string validateBeatSetting(const std::string& key, const std::string& value) {
    auto number = [&](double lo, double hi, double& out) {
        char* end = nullptr;
        out = std::strtod(value.c_str(), &end);
        return !value.empty() && end && !*end && std::isfinite(out) && out >= lo && out <= hi;
    };
    double v = 0;
    if (key == "phrasenow") return {};
    if (key == "phrasenudge") return number(-64, 64, v) ? std::string() : "phrasenudge: bars, -64..64";
    if (key == "phrasebars") return number(1, 64, v) ? std::string() : "phrasebars: 1..64";
    if (key == "phraseauto" || key == "dropswitch" || key == "phrasedecks" || key == "nowplaying")
        return value == "on" || value == "off" || value == "1" || value == "0" ? std::string() : key + ": on | off";
    if (key == "quantize" || key == "quantizemanual" || key == "quantizespawn")
        return value == "off" || value == "beat" || value == "bar" || value == "phrase"
                   ? std::string()
                   : key + ": off | beat | bar | phrase";
    if (key == "kickzoom") return number(-0.1, 0.1, v) ? std::string() : "kickzoom: -0.1..0.1";
    if (key == "kickrotate") return number(-10, 10, v) ? std::string() : "kickrotate: -10..10 degrees";
    if (key == "kickdecay") return number(20, 2000, v) ? std::string() : "kickdecay: 20..2000 ms";
    if (key == "nowplayingsize") return number(25, 300, v) ? std::string() : "nowplayingsize: 25..300 %";
    if (key == "nowplayingswitch") return number(0, 30, v) ? std::string() : "nowplayingswitch: 0..30 s";
    if (key == "nowplayingshadow" || key == "nowplayingshadowfrom")
        return number(0, 1, v) ? std::string() : key + ": 0..1";
    if (key == "nowplayingcorner")
        return value == "bl" || value == "br" || value == "tl" || value == "tr" ? std::string()
                                                                                 : "nowplayingcorner: bl | br | tl | tr";
    // Dancers (PLAN 5P): the Dancers card.
    if (key == "dancefloor")
        return value == "on" || value == "off" || value == "1" || value == "0" ? std::string() : key + ": on | off";
    if (key == "dancerleave") return number(0, 4294967295.0, v) ? std::string() : "dancerleave: a dancer's id";
    if (key == "guestsblocked") {
        for (char ch : value)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == ',')) return "guestsblocked: guest ids, comma separated, or none";
        return std::string();
    }
    if (key == "dancesay")
        return value.find('=') != std::string::npos ? std::string() : "dancesay: NAME=TEXT (a speech bubble over that dancer)";
    if (key == "dancehide")
        return value == "on" || value == "off" || value == "1" || value == "0" ? std::string() : key + ": on | off";
    if (key == "guests")
        return value == "on" || value == "off" || value == "1" || value == "0" ? std::string() : key + ": on | off";
    if (key == "guestminutes") return number(1, 240, v) ? std::string() : "guestminutes: 1..240";
    if (key == "dancerandom")
        return normalizeRandomStyles(value) ? std::string()
                                            : "dancerandom: all | none | a comma list of styles (disco, hiphop, house, "
                                              "pop, robot, rave, retro, chill)";
    if (key == "dancelight") return number(0, 1, v) ? std::string() : "dancelight: 0..1";
    if (key == "dancereflect") return number(0, 1, v) ? std::string() : "dancereflect: 0..1";
    if (key == "danceoutline") return number(0, 8, v) ? std::string() : "danceoutline: 0..8 px";
    if (key == "danceheight") return number(0.15, 0.9, v) ? std::string() : "danceheight: 0.15..0.9";
    if (key == "dancehuman") return number(0, 2, v) ? std::string() : "dancehuman: 0..2";
    if (key == "dancemocap") return number(0, 1, v) ? std::string() : "dancemocap: 0..1";
    if (key == "guestcall") return value.empty() ? "guestcall: a past guest's id or name" : std::string();
    if (key == "dancetry") return value.empty() ? "dancetry: a mocap move's id or label, or off" : std::string();
    if (key == "dancemax") return number(1, 32, v) ? std::string() : "dancemax: 1..32";
    return "unknown rhythm setting \"" + key + "\"";
}

Command Command::look(std::string keyOrOp, std::string value) {
    Command c;
    c.type = CommandType::Look;
    c.text = std::move(keyOrOp);
    c.arg = std::move(value);
    return c;
}

std::string validateLook(const Command& c) {
    const std::string& op = c.text;
    if (op == "ab") return {};
    if (op == "preset") return lookPreset(c.arg) ? std::string() : "look preset must be gforce or right, not \"" + c.arg + "\"";
    if (op == "reset" || op == "save") {
        if (c.arg.empty() || c.arg == "all" || parseLookKey(c.arg)) return {};
        return "look " + op + ": unknown key \"" + c.arg + "\"";
    }
    if (op == "saved") {
        const auto eq = c.arg.find('=');
        const auto k = eq == std::string::npos ? std::nullopt : parseLookKey(c.arg.substr(0, eq));
        if (!k || !normalizeLookValue(*k, c.arg.substr(eq + 1))) return "look saved needs KEY=VALUE";
        return {};
    }
    const auto k = parseLookKey(op);
    if (!k) return "unknown look key \"" + op + "\" (thinlines, intensity, flowrounding, flowprecision, aa, linescale, sprites)";
    if (!normalizeLookValue(*k, c.arg)) {
        std::string allowed;
        for (const char* x : lookInfo(*k).values) allowed += (allowed.empty() ? "" : " | ") + std::string(x);
        return std::string(lookInfo(*k).id) + ": \"" + c.arg + "\" is not one of " + allowed;
    }
    return {};
}

const char* videoTargetName(VideoTarget t) { return t == VideoTarget::Sprite ? "sprite" : "layer"; }

std::string describe(const Command& c) {
    const std::string layer = c.allLayers ? std::string("all") : std::string(layerName(c.layer));
    char buf[64];
    switch (c.type) {
        case CommandType::Switch:
            if (c.seconds >= 0) std::snprintf(buf, sizeof buf, " (%.2f s)", c.seconds);
            return layer + " switch " +
                   (c.target == SwitchTarget::Name ? "\"" + c.text + "\""
                                                   : (c.target == SwitchTarget::Next ? (c.arg == "movie" ? "next movie" : "next")
                                                                                     : "previous")) +
                   (c.seconds >= 0 ? std::string(buf) : (c.quick ? " (quick)" : ""));
        case CommandType::Hold: return layer + (c.flag ? " hold" : " unhold");
        case CommandType::ToggleHold: return layer + " toggle hold";
        case CommandType::SetSlideshowEnabled: return layer + " slideshow " + (c.flag ? "on" : "off");
        case CommandType::SetSlideshowInterval:
            std::snprintf(buf, sizeof buf, " interval %.2f s", c.value);
            return layer + buf;
        case CommandType::SetTransitionDuration:
            std::snprintf(buf, sizeof buf, " transition %.2f s", c.value);
            return layer + buf;
        case CommandType::SetSpawnDuration:
            if (c.layer == Layer::Dancer && c.value == 0) return layer + " duration forever";
            std::snprintf(buf, sizeof buf, " duration %.2f s", c.value);
            return layer + buf;
        case CommandType::SetMovieInterval:
            std::snprintf(buf, sizeof buf, "movie sprite interval %.2f s", c.value);
            return buf;
        case CommandType::SetSpriteMix:
            std::snprintf(buf, sizeof buf, c.flag ? " sprite mix %+.2f" : " sprite mix %.2f", c.value);
            return (c.text == "movie" ? "movie" : "image") + std::string(buf);
        case CommandType::SetBlendMode: return layer + " blend " + c.text;
        case CommandType::SetTheme: return "theme \"" + c.text + "\"";
        case CommandType::SetCrossfader:
            std::snprintf(buf, sizeof buf, " crossfader %.3f", c.value);
            return layer + buf;
        case CommandType::ReleaseCrossfader: return layer + " crossfader release";
        case CommandType::Kill: return layer + " kill" + (c.text.empty() ? "" : " " + c.text);
        case CommandType::VideoStart:
            return std::string("video ") + videoTargetName(c.videoTarget) + " play \"" + c.text + "\"";
        case CommandType::VideoStop: return std::string("video ") + videoTargetName(c.videoTarget) + " stop";
        case CommandType::VideoSet: return "video set " + c.text + " = " + c.arg;
        case CommandType::VideoHold: return "video sprites loop until stopped";
        case CommandType::SetSpriteHidden: return "hidden sprites \"" + c.text + "\"";
        case CommandType::BeatSet: return "rhythm " + c.text + (c.arg.empty() ? std::string() : " = " + c.arg);
        case CommandType::Look:
            if (c.text == "ab") return "look A/B toggle";
            return "look " + c.text + (c.arg.empty() ? std::string() : (c.text == "saved" ? " " : " = ") + c.arg);
    }
    return "?";
}

std::optional<Command> parseCommand(const std::string& text, std::string* error) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (int i = 0; i < 2; ++i) {  // action : layer : rest (the rest may contain ':')
        const auto colon = text.find(':', start);
        if (colon == std::string::npos) break;
        parts.push_back(text.substr(start, colon - start));
        start = colon + 1;
    }
    parts.push_back(text.substr(start));
    auto fail = [&](const std::string& m) -> std::optional<Command> {
        if (error) *error = m;
        return std::nullopt;
    };
    std::string action;
    for (char c : parts[0]) action.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (action == "video" || action == "videostop") {
        if (parts.size() < 2) return fail(action + " needs layer or sprite");
        std::string target;
        for (char c : parts[1]) target.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        VideoTarget t;
        if (target == "layer" || target == "color") t = VideoTarget::Layer;
        else if (target == "sprite") t = VideoTarget::Sprite;
        else return fail(action + ": target must be layer or sprite, not \"" + parts[1] + "\"");
        if (action == "videostop") return Command::videoStop(t);
        if (parts.size() < 3 || parts[2].empty()) return fail("video needs a source");
        return Command::videoStart(t, parts[2]);
    }
    if (action == "videohold") return Command::videoHold();
    if (action == "videoset") {
        if (parts.size() < 3 || parts[1].empty()) return fail("videoset needs KEY:VALUE");
        return Command::videoSet(parts[1], parts[2]);
    }
    if (action == "beatset") {
        if (parts.size() < 2 || parts[1].empty()) return fail("beatset needs KEY[:VALUE]");
        Command c = Command::beatSet(parts[1], parts.size() > 2 ? parts[2] : std::string());
        if (auto e = validateBeatSetting(c.text, c.arg); !e.empty()) return fail(e);
        return c;
    }
    if (action == "look") {
        if (parts.size() < 2 || parts[1].empty()) return fail("look needs KEY:VALUE, ab, preset:NAME, reset or save");
        std::string op;
        for (char c : parts[1]) op.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        Command c = Command::look(op, parts.size() > 2 ? parts[2] : std::string());
        if (auto k = parseLookKey(op)) c.text = lookInfo(*k).id;
        if (auto e = validateLook(c); !e.empty()) return fail(e);
        return c;
    }
    if (action == "spritemix") {
        std::string kind;
        if (parts.size() > 1)
            for (char c : parts[1]) kind.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        const bool movie = kind == "movie" || kind == "movies" || kind == "video";
        if (!movie && kind != "sprite" && kind != "image" && kind != "images")
            return fail("spritemix needs sprite or movie, then a value (spritemix:sprite:0.5)");
        const std::string v = parts.size() > 2 ? parts[2] : std::string();
        char* end = nullptr;
        const double w = std::strtod(v.c_str(), &end);
        if (v.empty() || end == v.c_str() || *end != '\0' || !std::isfinite(w)) return fail("spritemix: bad value \"" + v + "\"");
        const bool relative = v[0] == '+' || v[0] == '-';
        if (!relative && (w < 0 || w > 1)) return fail("spritemix: value must be 0..1 (or +/- a nudge)");
        return Command::spriteMix(movie, w, relative);
    }
    if (action == "theme") {
        if (parts.size() < 2) return fail("theme needs a name");
        std::string name = parts[1];
        if (parts.size() > 2) name += ":" + parts[2];
        return Command::theme(name);
    }
    if (parts.size() < 2) return fail("missing layer");
    const bool all = parts[1] == "all";
    const auto layer = parseLayer(parts[1]);
    if (!layer && !all) return fail("unknown layer \"" + parts[1] + "\"");
    const Layer l = layer.value_or(Layer::WaveShape);
    const std::string arg = parts.size() > 2 ? parts[2] : std::string();
    const double num = std::atof(arg.c_str());
    auto withAll = [&](Command c) {
        c.allLayers = all;
        return c;
    };
    if (action == "next") return Command::next(l);
    if (action == "prev" || action == "previous") return Command::previous(l);
    if (action == "kill") {
        Command c = Command::kill(l);
        if (l == Layer::Sprite && (arg == "images" || arg == "movies")) c.text = arg;  // one kind only
        else if (!arg.empty()) return fail("kill:sprite takes images or movies");
        return c;
    }
    if (action == "nextmovie") {  // the next slideshow movie sprite (movie shuffle)
        if (l != Layer::Sprite) return fail("nextmovie applies to sprite");
        Command c = Command::next(l);
        c.arg = "movie";
        return c;
    }
    // start = a scripted start (G-Force's StartParticle/StartSprite): like slide,
    // it uses the regular Transition.Duration, not .Quick (probe #19 fades).
    if (action == "switch" || action == "slide" || action == "start") {
        if (arg.empty()) return fail(action + " needs a name");
        return Command::switchTo(l, arg, action == "switch");
    }
    if (action == "morph") {
        const auto colon = arg.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= arg.size())
            return fail("morph needs SECS:NAME");
        const std::string secs = arg.substr(0, colon);
        char* end = nullptr;
        const double v = std::strtod(secs.c_str(), &end);
        if (end == secs.c_str() || *end != '\0' || !(v >= 0)) return fail("morph: bad seconds \"" + secs + "\"");
        if (l == Layer::Union || isSpawnLayer(l))
            return fail("morph applies to waveshape, colormap and flowfield");
        return Command::morphTo(l, arg.substr(colon + 1), v);
    }
    if (action == "hold") return withAll(Command::hold(l, true));
    if (action == "unhold") return withAll(Command::hold(l, false));
    if (action == "togglehold") return withAll(Command::toggleHold(l));
    if (action == "slideshow") return withAll(Command::slideshow(l, arg != "off" && arg != "0"));
    if (action == "interval") return Command::interval(l, num);
    if (action == "duration") return Command::transitionDuration(l, num);
    if (action == "life") {
        if (!isSpawnLayer(l)) return fail("life applies to particle, sprite, solid, organic and dancer");
        return Command::spawnDuration(l, num);
    }
    if (action == "movieinterval") {
        if (l != Layer::Sprite) return fail("movieinterval applies to sprite");
        return Command::movieInterval(num);
    }
    if (action == "blend") return Command::blendMode(l, arg);
    if (action == "fader") return Command::crossfader(l, num);
    if (action == "release") return Command::releaseCrossfader(l);
    return fail("unknown action \"" + action + "\"");
}

PrefsTable slideshowSaveEntries(const Command& c) {
    PrefsTable t;
    if (c.type == CommandType::BeatSet) {  // rhythm panel settings (actions like phrasenow are not saved)
        if (!validateBeatSetting(c.text, c.arg).empty()) return t;
        const std::string& v = c.arg;
        if (c.text == "phrasebars") t.set("reforce.Phrase.Bars", v, false);
        if (c.text == "phraseauto") t.set("reforce.Phrase.AutoAlign", v == "on" || v == "1" ? "1" : "0", false);
        if (c.text == "phrasedecks") t.set("reforce.Phrase.DeckStarts", v == "on" || v == "1" ? "1" : "0", false);
        if (c.text == "quantize") t.set("reforce.Link.Quantize", v, true);
        if (c.text == "quantizemanual") t.set("reforce.Link.QuantizeManual", v, true);
        if (c.text == "quantizespawn") t.set("reforce.Quantize.Spawn", v, true);
        if (c.text == "kickzoom") t.set("reforce.Kick.Zoom", v, false);
        if (c.text == "kickrotate") t.set("reforce.Kick.Rotate", v, false);
        if (c.text == "kickdecay") t.set("reforce.Kick.DecayMs", v, false);
        if (c.text == "dropswitch") t.set("reforce.Drop.Switch", v == "on" || v == "1" ? "1" : "0", false);
        if (c.text == "dancefloor") t.set("reforce.Dancer.Floor", v == "on" || v == "1" ? "1" : "0", false);
        if (c.text == "guests") t.set("reforce.Guests.Enabled", v == "on" || v == "1" ? "1" : "0", false);
        if (c.text == "guestminutes") t.set("reforce.Guests.Minutes", v, false);
        if (c.text == "guestsblocked") t.set("reforce.Guests.Blocked", v == "none" ? std::string() : v, true);
        if (c.text == "dancerandom") t.set("reforce.Dancer.RandomStyles", normalizeRandomStyles(v).value_or("all"), true);
        if (c.text == "dancelight") t.set("reforce.Dancer.Lighting", v, false);
        if (c.text == "dancereflect") t.set("reforce.Dancer.Reflection", v, false);
        if (c.text == "danceoutline") t.set("reforce.Dancer.Outline", v, false);
        if (c.text == "danceheight") t.set("reforce.Dancer.Height", v, false);
        if (c.text == "dancehuman") t.set("reforce.Dancer.Human", v, false);
        if (c.text == "dancemocap") t.set("reforce.Dancer.Mocap", v, false);
        if (c.text == "dancemax") t.set("reforce.Dancer.MaxLive", v, false);
        if (c.text == "nowplaying") t.set("reforce.NowPlaying.Show", v == "on" || v == "1" ? "1" : "0", false);
        if (c.text == "nowplayingsize") t.set("reforce.NowPlaying.Size", v, false);
        if (c.text == "nowplayingcorner") t.set("reforce.NowPlaying.Corner", v, true);
        if (c.text == "nowplayingswitch") t.set("reforce.NowPlaying.SwitchSeconds", v, false);
        if (c.text == "nowplayingshadow") t.set("reforce.NowPlaying.Shadow", v, false);
        if (c.text == "nowplayingshadowfrom") t.set("reforce.NowPlaying.ShadowFrom", v, false);
        return t;
    }
    if (c.type == CommandType::VideoSet) {  // panel toggles saved like the slideshow sliders
        std::string key, val;
        for (char ch : c.text) key.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
        for (char ch : c.arg) val.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
        const bool on = val == "1" || val == "on" || val == "true" || val == "yes";
        const bool off = val == "0" || val == "off" || val == "false" || val == "no";
        if (key == "beatsync" && (on || off)) t.set("reforce.Video.BeatSync", on ? "1" : "0", false);
        if (key == "beatoffset") {
            char* end = nullptr;
            const double ms = std::strtod(val.c_str(), &end);
            if (!val.empty() && !*end && ms >= -200.0 && ms <= 500.0) {
                char b[32];
                std::snprintf(b, sizeof b, "%g", ms);
                t.set("reforce.BeatClock.OffsetMs", b, false);
            }
        }
        if (key == "syphonalpha" && (on || off)) t.set("reforce.Syphon.Alpha", on ? "1" : "0", false);
        if (key == "windowvisuals" && (on || off)) t.set("reforce.Window.VisualsOnly", on ? "1" : "0", false);
        if (key.rfind("lights", 0) == 0)  // room lights (docs/lights.md)
            if (auto e = lightsSettingEntry(key.substr(6), c.arg)) t.set(e->first, e->second, false);
        if (key == "keysimilarity" || key == "keysmoothness") {
            char* end = nullptr;
            const double x = std::strtod(val.c_str(), &end);
            if (!val.empty() && !*end && x >= 1 && x <= 1000) {
                char b[32];
                std::snprintf(b, sizeof b, "%g", x);
                t.set(key == "keysimilarity" ? "reforce.Syphon.KeySimilarity" : "reforce.Syphon.KeySmoothness", b,
                      false);
            }
        }
        if (key.size() > 4 && key.rfind("body", 0) == 0) {  // body tracking (docs/body.md)
            const std::string name = bodySettingKey(key.substr(4));
            if (auto v = bodySettingValue(key.substr(4), c.arg); v && !name.empty())
                t.set(name, *v, key == "bodyquality" || key == "bodyburn");
        }
        if (key == "backdrop") t.set("reforce.Backdrop.Source", off ? std::string() : c.arg, true);
        if (key == "backdropmirror" && (on || off)) t.set("reforce.Backdrop.Mirror", on ? "1" : "0", false);
        if (key == "backdropfit" && (val == "cover" || val == "contain" || val == "stretch"))
            t.set("reforce.Backdrop.Fit", val, true);
        if (key == "backdropopacity") {
            char* end = nullptr;
            const double x = std::strtod(val.c_str(), &end);
            if (!val.empty() && !*end && x >= 0 && x <= 1) {
                char b[32];
                std::snprintf(b, sizeof b, "%g", x);
                t.set("reforce.Backdrop.Opacity", b, false);
            }
        }
        if (key == "spritecolor") {
            if (val == "over" || on) t.set("reforce.MovieSprite.Color", "over", true);
            else if (off) t.set("reforce.MovieSprite.Color", "off", true);
        }
        return t;
    }
    if (c.allLayers || !std::isfinite(c.value)) return t;
    const bool spawner = isSpawnLayer(c.layer);
    char v[32];
    std::snprintf(v, sizeof v, "%g", c.value);
    const std::string prefix = layerName(c.layer);
    switch (c.type) {
        case CommandType::SetSlideshowInterval:
            if (c.value > 0 || (spawner && c.value == 0)) t.set(prefix + ".Slideshow.Interval.Duration", v, false);
            break;
        case CommandType::SetTransitionDuration:
            if (c.value >= 0) t.set(prefix + ".Transition.Duration", v, false);
            break;
        case CommandType::SetSpawnDuration:
            // Dancers: 0 = forever (∞ on the card).
            if (spawner && (c.value > 0 || (c.value == 0 && c.layer == Layer::Dancer))) t.set(prefix + ".Duration", v, false);
            break;
        case CommandType::SetMovieInterval:
            if (c.value >= 0) t.set("reforce.MovieSprite.Slideshow.Interval.Duration", v, false);
            break;
        case CommandType::SetSpriteMix:
            if (!c.flag && c.value >= 0 && c.value <= 1)
                t.set(c.text == "movie" ? "reforce.MovieSprite.Mix" : "reforce.Sprite.Mix", v, false);
            break;
        default: break;
    }
    return t;
}

}  // namespace reforce::engine
