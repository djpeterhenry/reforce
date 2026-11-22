// The dancer preview for the web (a guest sign-up page, docs/guests.md): reforce's own dancer code (engine/DancerRig, DanceMoves,
// Dancers) compiled to WebAssembly, so the page shows exactly the character reforce will put on the dance floor,
// dancing. dancer_gl.js draws the parts with a WebGL2 port of render/DancerPipeline's body shader.
//
//   dancer_moves(bytes, n)       the web sample of mocap moves (tools/aist_import/web_moves.py: 3 basic moves and a
//                                routine per style); returns how many, or -1. Call dancer_set again afterwards.
//   dancer_set(keys, seed, t)    a character from dancer file keys ("HairStyle=bob\nTop=#ff0000\n...")
//   dancer_frame(t, bpm)         step to time t (seconds) at bpm; returns the part count (0: no dancer)
//   dancer_parts()               the parts: kDancerInstFloats floats each (engine/DancerInstances.hpp)
//   dancer_head_top()            world height of the top of the head, hair or hat (framing)
//
// Build: tools/dancer_web/build.sh (Emscripten).

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/DanceClips.hpp"
#include "engine/DancerInstances.hpp"
#include "engine/Dancers.hpp"

using namespace reforce::engine;

namespace {
std::unique_ptr<DancerRun> run;
std::vector<float> parts;
dance::BodyOut body;
double lastT = -1;
float headTop = 1;

// The preview cycles through its style's mocap moves (Peter, 2026-11-21: "just cycle through what's there"): each
// basic move for its 4 bars, then the routine, then again. Each move starts on a bar line from its own bar 1.
struct Cycle {
    std::vector<int> moves;  // moveLibrary() indices: basics first, then routines
    std::size_t at = 0;
    double nextBar = -1;     // the bar line where the move after this one starts (-1: not started)
} cycle;

void planCycle(dance::Style style) {
    cycle = Cycle{};
    std::vector<int> basics, routines;
    for (int i = 0; i < dance::clipCount(); ++i) {
        const dance::ClipInfo& c = dance::clipInfo(i);
        if (dance::clipGenreStyle(c.genre) == style) (c.routine ? routines : basics).push_back(c.move);
    }
    cycle.moves = basics;
    cycle.moves.insert(cycle.moves.end(), routines.begin(), routines.end());
}

double barsOf(int move) { return dance::moveLibrary()[std::size_t(move)].beats / 4.0; }

// Before the step at running bar `bar`: queue the next move in the last bar of the current one (a tried move takes
// over on the next bar line).
void stepCycle(double bar) {
    if (cycle.moves.empty() || !run) return;
    if (cycle.nextBar < 0) {
        run->choreographer().tryMove(cycle.moves[0]);
        cycle.at = 0;
        cycle.nextBar = std::floor(bar) + 1 + barsOf(cycle.moves[0]);
        return;
    }
    if (std::floor(bar) + 1 >= cycle.nextBar) {
        cycle.at = (cycle.at + 1) % cycle.moves.size();
        const int move = cycle.moves[cycle.at];
        run->choreographer().tryMove(move);
        cycle.nextBar += barsOf(move);
    }
}
}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int dancer_set(const char* keys, unsigned seed, double t) {
    std::map<std::string, std::string> kv;
    std::string text = keys ? keys : "";
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(at, nl - at);
        at = nl + 1;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        for (char& c : k) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        kv[k] = v;
    }
    DancerSpec spec = dancerSpecFromKeys(
        [&](const char* key) {
            auto it = kv.find(key);
            return it == kv.end() ? std::string() : it->second;
        },
        "preview");
    spec.random = false;
    // Started a few seconds ago: no entrance hop each time a setting changes.
    run = std::make_unique<DancerRun>(std::move(spec), seed ? seed : 1u, t - 3.0, (t - 3.0) * 2.0, 0.0);
    run->setSlot(0, 0);
    run->choreographer().setMocap(1);  // mocap moves only, as on stream
    planCycle(run->spec().style);
    lastT = -1;
    return int(run->spec().warnings.size());
}

EMSCRIPTEN_KEEPALIVE int dancer_moves(const unsigned char* bytes, int n) {
    if (!bytes || n <= 0) return -1;
    const std::string err = dance::loadClipsFromMemory(bytes, std::size_t(n), "web moves");
    return err.empty() ? dance::clipCount() : -1;
}

EMSCRIPTEN_KEEPALIVE int dancer_frame(double t, double bpm) {
    if (!run) return 0;
    const double dt = lastT < 0 ? 1.0 / 60.0 : std::clamp(t - lastT, 0.0, 0.1);
    lastT = t;
    dance::MusicState m;
    m.beat = t * bpm / 60.0;
    m.bar = m.beat / 4;
    m.phrase = m.bar / 16;
    m.bass = 0.5;
    m.locked = true;
    run->setSlot(0, 0);
    stepCycle(m.bar);
    run->step(t, dt, m, 1.0f);
    body = dance::BodyOut{};
    run->build(body);
    headTop = body.headTop;
    parts.resize(body.parts.size() * kDancerInstFloats);
    for (std::size_t i = 0; i < body.parts.size(); ++i) packDancerPart(body.parts[i], body, parts.data() + i * kDancerInstFloats);
    return int(body.parts.size());
}

EMSCRIPTEN_KEEPALIVE const float* dancer_parts() { return parts.data(); }
EMSCRIPTEN_KEEPALIVE float dancer_head_top() { return headTop; }
EMSCRIPTEN_KEEPALIVE int dancer_floats() { return kDancerInstFloats; }

}  // extern "C"
