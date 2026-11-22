#include "app/Describe.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "expr/Builtins.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

namespace reforce::app {
namespace {

using scene::Tier;

struct Flags {
    bool random = false, time = false, audio = false;
    std::set<std::string> builtins;
    void merge(const Flags& o) {
        random |= o.random;
        time |= o.time;
        audio |= o.audio;
        builtins.insert(o.builtins.begin(), o.builtins.end());
    }
};

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += char(c);
    }
    return out + "\"";
}

const char* tierGroup(Tier t) {
    switch (t) {
        case Tier::Pre:
        case Tier::Load:
        case Tier::PostLoad: return "load";
        case Tier::Frame:
        case Tier::FrameOutput: return "frame";
        case Tier::Step:
        case Tier::StepOutput: return "step";
        case Tier::Pixel:
        case Tier::PixelOutput: return "pixel";
        case Tier::Ignored: break;
    }
    return nullptr;
}

bool isLoadTier(Tier t) { return t == Tier::Pre || t == Tier::Load || t == Tier::PostLoad; }

// Flags of key i: its own use plus (transitively) that of every key and user var it reads. Randomness
// from load-tier keys (A vars etc.) is fixed once per instance, so it does not make a later-tier key
// "random" (a FlowField whose D vars read a random A var is random-at-load, not per-pixel noise); the
// load tier group reports it.
Flags keyFlags(const scene::CompiledConfig& cfg, int i, std::vector<int>& state, std::vector<Flags>& memo) {
    if (state[std::size_t(i)] == 2) return memo[std::size_t(i)];
    if (state[std::size_t(i)] == 1) return {};  // cycle (self-referencing B/C vars): own flags only
    state[std::size_t(i)] = 1;
    const auto& k = cfg.keys[std::size_t(i)];
    Flags f;
    const auto& p = k.prog;
    f.random = p.usesRandom;
    f.audio = p.usesAudio || p.usesVar(expr::Var::BASS) || p.usesVar(expr::Var::LEVEL) ||
              p.usesVar(expr::Var::ENERGY);
    f.time = p.usesVar(expr::Var::T) || p.usesVar(expr::Var::DT);
    for (const auto& in : p.code)
        if (in.op == expr::Op::Call || in.op == expr::Op::CallS1 || in.op == expr::Op::CallS2)
            f.builtins.insert(expr::builtinAt(in.arg).name);
    auto mergeRef = [&](int r) {
        if (r < 0 || r >= int(cfg.keys.size()) || r == i) return;
        Flags g = keyFlags(cfg, r, state, memo);
        if (isLoadTier(cfg.keys[std::size_t(r)].tier) && !isLoadTier(k.tier)) g.random = false;
        f.merge(g);
    };
    for (int r : p.keyRefs) mergeRef(r);
    for (const auto& [fam, idx] : p.userRefs) {
        if (fam < 0 || fam > 3 || idx < 0 || idx >= cfg.familySize(fam)) continue;
        mergeRef(cfg.families[std::size_t(fam)][std::size_t(idx)]);
    }
    state[std::size_t(i)] = 2;
    memo[std::size_t(i)] = f;
    return f;
}

}  // namespace

int describeConfigs(const std::vector<std::string>& paths) {
    int rc = 0;
    for (const auto& path : paths) {
        auto type = scene::sceneTypeFromPath(path);
        auto file = scene::loadSceneFile(path);
        if (!type || !file) {
            std::printf("{\"path\": %s, \"error\": %s}\n", jsonString(path).c_str(),
                        jsonString(!file ? "cannot read" : "unknown scene type (not under a G-Force.*.package)").c_str());
            rc = 1;
            continue;
        }
        const scene::CompiledConfig cfg = scene::compileConfig(*file, *type);
        std::vector<int> state(cfg.keys.size(), 0);
        std::vector<Flags> memo(cfg.keys.size());
        Flags all;
        std::map<std::string, Flags> groups;  // tier group -> flags (with dependencies)
        for (int i = 0; i < int(cfg.keys.size()); ++i) {
            const char* g = tierGroup(cfg.keys[std::size_t(i)].tier);
            if (!g || !cfg.keys[std::size_t(i)].prog.defined) continue;
            const Flags f = keyFlags(cfg, i, state, memo);
            groups[g].merge(f);
            all.merge(f);
        }
        std::string out = "{\"path\": " + jsonString(path) + ", \"type\": " +
                          jsonString(std::string(scene::sceneTypeName(*type))) +
                          ", \"errors\": " + std::to_string(cfg.diags.errorCount()) +
                          ", \"warnings\": " + std::to_string(cfg.diags.warningCount());
        auto flagsJson = [](const Flags& f) {
            return std::string("{\"random\": ") + (f.random ? "true" : "false") + ", \"time\": " +
                   (f.time ? "true" : "false") + ", \"audio\": " + (f.audio ? "true" : "false") + "}";
        };
        out += ", \"all\": " + flagsJson(all) + ", \"tiers\": {";
        bool first = true;
        for (const auto& [g, f] : groups) {
            out += (first ? "" : ", ") + jsonString(g) + ": " + flagsJson(f);
            first = false;
        }
        out += "}, \"builtins\": [";
        first = true;
        for (const auto& b : all.builtins) {
            out += (first ? "" : ", ") + jsonString(b);
            first = false;
        }
        out += "]}";
        std::printf("%s\n", out.c_str());
    }
    std::fflush(stdout);
    return rc;
}

}  // namespace reforce::app
