#include "scene/SceneConfig.hpp"

#include <algorithm>
#include <functional>

#include "expr/Vars.hpp"

namespace reforce::scene {

using expr::Var;

const char* tierName(Tier t) {
    switch (t) {
        case Tier::Ignored: return "ignored";
        case Tier::Pre: return "pre";
        case Tier::Load: return "load(A)";
        case Tier::PostLoad: return "post-load";
        case Tier::Frame: return "frame(B)";
        case Tier::FrameOutput: return "frame-output";
        case Tier::Step: return "step(C)";
        case Tier::StepOutput: return "step-output";
        case Tier::Pixel: return "pixel(D)";
        case Tier::PixelOutput: return "pixel-output";
    }
    return "?";
}

const CompiledKey* CompiledConfig::find(std::string_view key) const {
    const int i = indexOf(key);
    return i < 0 ? nullptr : &keys[std::size_t(i)];
}

int CompiledConfig::indexOf(std::string_view key) const {
    auto it = byName.find(expr::toUpper(key));
    return it == byName.end() ? -1 : it->second;
}

bool CompiledConfig::defines(std::string_view key) const {
    const CompiledKey* k = find(key);
    return k && k->prog.defined;
}

void CompiledConfig::prepare(expr::EvalContext& ctx) const {
    for (int f = 0; f < 4; ++f) ctx.ensureSlots(f, familySize(f));
    ctx.ensureKeySlots(int(keys.size()));
}

bool usesBeat(const CompiledConfig& cfg) {
    static constexpr Var kBeatVars[] = {Var::BEAT,      Var::TEMPO,     Var::LINK_ON,  Var::LINK_BPM,
                                        Var::LINK_BEAT, Var::LINK_PHASE, Var::SYNC_ON, Var::SYNC_BPM,
                                        Var::SYNC_BEAT, Var::SYNC_LOCK, Var::SYNC_BAR, Var::SYNC_PHRASE, Var::SYNC_BUILD, Var::SYNC_DROP};
    return std::any_of(cfg.keys.begin(), cfg.keys.end(), [](const CompiledKey& k) {
        return k.prog.defined &&
               std::any_of(std::begin(kBeatVars), std::end(kBeatVars), [&](Var v) { return k.prog.usesVar(v); });
    });
}

bool isNativeVar(SceneType type, Var v) {
    switch (v) {
        case Var::PI:
        case Var::WIDTH:
        case Var::HEIGHT:
        case Var::X_EXTENTS:
        case Var::Y_EXTENTS:
        case Var::NUM_FFT_BINS:
        case Var::NUM_SAMPLE_BINS: return true;
        default: break;
    }
    switch (type) {
        case SceneType::WaveShape:
        case SceneType::Particle:
            switch (v) {
                case Var::S: case Var::T: case Var::ID: case Var::NUM: case Var::NUM_S_STEPS:
                case Var::BASS: case Var::LEVEL: case Var::ENERGY: case Var::END_TIME:
                case Var::START_TIME: case Var::DT: case Var::PATH_ID: case Var::FADER:
                case Var::MID: case Var::TREBLE: case Var::BEAT: case Var::TEMPO:
                case Var::LINK_ON: case Var::LINK_BPM: case Var::LINK_BEAT: case Var::LINK_PHASE:
                case Var::SYNC_ON: case Var::SYNC_BPM: case Var::SYNC_BEAT: case Var::SYNC_LOCK:
                case Var::SYNC_BAR: case Var::SYNC_PHRASE: case Var::SYNC_BUILD: case Var::SYNC_DROP: return true;
                default: return false;
            }
        case SceneType::ColorMap: return v == Var::I || v == Var::T;
        case SceneType::FlowField:
        case SceneType::FlowFieldUnion: return v == Var::X || v == Var::Y || v == Var::R || v == Var::Theta;
        case SceneType::Sprite:
            switch (v) {
                case Var::T: case Var::DT: case Var::XPOS: case Var::YPOS: case Var::FADER: case Var::BASS:
                case Var::LEVEL: case Var::ENERGY: case Var::END_TIME: case Var::START_TIME:
                case Var::MID: case Var::TREBLE: case Var::BEAT: case Var::TEMPO:
                case Var::LINK_ON: case Var::LINK_BPM: case Var::LINK_BEAT: case Var::LINK_PHASE:
                case Var::SYNC_ON: case Var::SYNC_BPM: case Var::SYNC_BEAT: case Var::SYNC_LOCK:
                case Var::SYNC_BAR: case Var::SYNC_PHRASE: case Var::SYNC_BUILD: case Var::SYNC_DROP: return true;
                default: return false;
            }
        case SceneType::Solid:
        case SceneType::Organic:  // plus `s` in the per-point / per-root keys
            switch (v) {
                case Var::T: case Var::DT: case Var::FADER: case Var::BASS: case Var::LEVEL: case Var::ENERGY:
                case Var::END_TIME: case Var::START_TIME:
                case Var::MID: case Var::TREBLE: case Var::BEAT: case Var::TEMPO:
                case Var::LINK_ON: case Var::LINK_BPM: case Var::LINK_BEAT: case Var::LINK_PHASE:
                case Var::SYNC_ON: case Var::SYNC_BPM: case Var::SYNC_BEAT: case Var::SYNC_LOCK:
                case Var::SYNC_BAR: case Var::SYNC_PHRASE: case Var::SYNC_BUILD: case Var::SYNC_DROP: return true;
                case Var::S: return type == SceneType::Organic;
                default: return false;
            }
        case SceneType::Dancer: return false;  // literal keys only (PLAN 5P)
    }
    return false;
}

namespace {
// Literal (text) keys of model solids (docs/model-design.md): the model file, the camera move, the front / up axes.
bool isSolidLiteral(const std::string& key) {
    if (key == "FILE" || key == "MOVE" || key == "FRONT" || key == "UP") return true;
    if (key == "FIGS") return true;  // soma: the figures, by name (engine/Soma.hpp)
    if (key == "RAIN") return true;  // rain: window | floor
    if (key == "SRC") return true;   // bubbles: wand | edge | air
    // Camera path keyframes and options (engine/CameraMoves.hpp): K1, K2, ..., Ease, Step, Rand, Push.
    if (key == "EASE" || key == "STEP" || key == "RAND" || key == "PUSH") return true;
    // Fractal solids (docs/3d-design.md "Fractal zoom"): the fractal and its zoom path.
    if (key == "FRAC" || key == "PATH") return true;
    return key.size() >= 2 && key[0] == 'K' &&
           key.find_first_not_of("0123456789", 1) == std::string::npos;
}
}  // namespace

namespace {

bool isPathKey(const std::string& k) {
    if (k.size() < 2 || (k[0] != 'X' && k[0] != 'Y')) return false;
    for (std::size_t i = 1; i < k.size(); ++i)
        if (k[i] < '0' || k[i] > '9') return false;
    return true;
}

bool isWave(SceneType t) { return t == SceneType::WaveShape || t == SceneType::Particle; }
bool isFlow(SceneType t) { return t == SceneType::FlowField || t == SceneType::FlowFieldUnion; }

// Tier of a key for a scene type (scene-format §5). `unusedFamily` is set for
// A-D families the type never evaluates.
Tier tierFor(SceneType type, const std::string& key, int& family, int& index, bool& unusedFamily) {
    unusedFamily = false;
    if (expr::parseUserVar(key, family, index)) {
        switch (family) {
            case expr::FamA: return Tier::Load;
            case expr::FamB:
                if (isFlow(type)) break;
                return Tier::Frame;
            case expr::FamC:
                if (type == SceneType::Sprite || type == SceneType::Solid) break;  // Organic: per point / root
                return Tier::Step;
            case expr::FamD:
                if (!isFlow(type)) break;
                return Tier::Pixel;
            default: break;
        }
        unusedFamily = true;
        return Tier::Ignored;
    }
    family = index = -1;
    if (key == "VERS" || key == "META") return Tier::Ignored;
    if (type == SceneType::Dancer) return Tier::Ignored;  // every key is a literal (colors, names, numbers)
    if (key == "ASPC") return Tier::Pre;
    if (isWave(type)) {
        if (key == "NUM" || key == "STPS" || key == "PDUR" || key == "CONB") return Tier::Pre;
        if (isPathKey(key) || key == "PEN" || key == "LWDT") return Tier::StepOutput;
    } else if (type == SceneType::ColorMap) {
        if (key == "H" || key == "S" || key == "V" || key == "R" || key == "G" || key == "B") return Tier::StepOutput;
        if (key == "CCAL") return Tier::PostLoad;
    } else if (type == SceneType::FlowField) {
        if (key == "SRCX" || key == "SRCY" || key == "SRCR" || key == "SRCT") return Tier::PixelOutput;
    } else if (type == SceneType::FlowFieldUnion) {
        if (key == "ALFA") return Tier::PixelOutput;
    } else if (type == SceneType::Sprite) {
        if (key == "X" || key == "Y" || key == "PEN" || key == "EPEN") return Tier::FrameOutput;
        if (key == "SPED") return Tier::PostLoad;
        if (key == "FLAG" || key == "INVT" || key == "PDUR" || key == "FIT") return Tier::Pre;
    } else if (type == SceneType::Solid) {
        if (key == "X" || key == "Y" || key == "Z" || key == "SCAL" || key == "ROTX" || key == "ROTY" ||
            key == "ROTZ" || key == "FILL" || key == "SHAD" || key == "EDGE" || key == "PEN" || key == "EPEN" ||
            key == "TURN" || key == "SEED" || key == "SHUF" ||
            key == "FROS" || key == "TINT" || key == "BEND" || key == "EXPL" || key == "TENS" || key == "MONO")
            return Tier::FrameOutput;
        // The knight's tour (docs/3d-design.md "Knight's tour"): Sym and Brd are Pre.
        if (key == "ORBT" || key == "FOLW" || key == "FLSH" || key == "HOVR") return Tier::FrameOutput;
        // The jigsaw (docs/3d-design.md "Jigsaw"): the table's brightness, a drop's kick, a build's shake, per frame.
        if (key == "DIM" || key == "KICK" || key == "SHAK") return Tier::FrameOutput;
        // Model solids (docs/model-design.md): the camera and the look, per frame.
        if (key == "DIST" || key == "HITE" || key == "SIDE" || key == "ROLL" || key == "FOV" ||
            key == "CUTB" || key == "CAMX" || key == "CAMY" || key == "CAMZ" || key == "AIMX" || key == "AIMY" ||
            key == "AIMZ" || key == "SPIN" || key == "BANK" || key == "GLOW" || key == "BLOM" ||
            key == "EXPO" || key == "LTX" || key == "LTY" || key == "LTZ")
            return Tier::FrameOutput;
        // Rain on glass (docs/3d-design.md "Rain on glass"): the rain and the look, per frame (Rain is a literal).
        if (key == "DENS" || key == "SIZE" || key == "DRZ" || key == "CLUS" || key == "WET" || key == "FOG" ||
            key == "REFG" || key == "DPTH" || key == "SPEC" || key == "RIM" || key == "SPD" || key == "DCAY" ||
            key == "PUDL" || key == "FLOW")
            return Tier::FrameOutput;
        // The Tower of Hanoi (docs/3d-design.md "Tower of Hanoi"): a drop's higher arc and flips (Hovr, Fros, Tint, Bend,
        // Dpth and Spec are above).
        if (key == "LIFT" || key == "FLIP") return Tier::FrameOutput;
        // Bubbles (docs/3d-design.md "Bubbles"): the bubbles and the film, per frame (Src is a literal; Max, Clst load).
        if (key == "BLOW" || key == "POP" || key == "LIFE" || key == "AIR" || key == "RISE" || key == "WIND" || key == "FILM" ||
            key == "IRIS" || key == "SWRL" || key == "DRN")
            return Tier::FrameOutput;
        // Shatter (docs/3d-design.md "Shatter"): the burst, gravity, tilt and the look, per frame (Dpth and Spec above).
        if (key == "BRST" || key == "GRAV" || key == "TILT" || key == "LINE" || key == "SPRK" || key == "GLS" ||
            key == "THCK")
            return Tier::FrameOutput;
        // Fractal solids (docs/3d-design.md "Fractal zoom"): the zoom position, the camera's sway, the fog.
        if (key == "ZOOM" || key == "ORBT" || key == "LKX" || key == "LKY" || key == "FOG") return Tier::FrameOutput;
        if (key == "MESH") return Tier::Ignored;  // a literal name (CompiledKey::text), not an expression
        if (isSolidLiteral(key)) return Tier::Ignored;  // model File / Move / Front / Up, camera keyframes
        if (key == "LOOK" || key == "MDUR" || key == "MBAR" || key == "LOOP") return Tier::Pre;
        if (key == "TEXT" || key == "FLAG" || key == "PDUR" || key == "PERS" || key == "OVER" || key == "COLORKEY" || key == "GLAS" ||
            key == "MOVES" || key == "LEN" || key == "HOLD" || key == "EACH" || key == "PCS" || key == "ORDR" ||
            key == "CYCL" || key == "BRK" || key == "GAP")
            return Tier::Pre;
    } else if (type == SceneType::Organic) {
        if (key == "GROW" || key == "TRPX" || key == "TRPY" || key == "SWAY" || key == "X" || key == "Y" ||
            key == "SCAL" || key == "ROT" || key == "PEN" || key == "BUD" || key == "AMP" || key == "STRG" ||
            key == "PHAS" || key == "ANG" || key == "FX" || key == "FY" || key == "PHX" || key == "PHY" ||
            key == "AX" || key == "AY" || key == "DAMP" || key == "PA" || key == "PB" || key == "PC" || key == "PD" ||
            key == "DX" || key == "DY" || key == "RAIN" || key == "GAIN" || key == "CRE" || key == "CIM" ||
            key == "BAND" || key == "VIB" || key == "PULS" || key == "PALL" || key == "TGT" || key == "GLOW" ||
            key == "LEAF" || key == "LFLT")
            return Tier::FrameOutput;
        // L-system axiom and rules: literal text ("F=F[+F]F"), not expressions (PLAN 5O.4).
        if (key == "AXM" || key == "R1" || key == "R2" || key == "R3" || key == "R4") return Tier::Ignored;
        // Per attraction point / per root, once at the start (with `s`).
        if (key == "PX" || key == "PY" || key == "RTX" || key == "RTY" || key == "RTA") return Tier::StepOutput;
        if (key == "KIND") return Tier::Ignored;  // a literal algorithm name, not an expression
    }
    return Tier::Pre;  // named constant ("Side=5"), readable by name from expressions
}

class SceneResolver : public expr::NameResolver {
public:
    SceneResolver(SceneType type, const std::map<std::string, int>& keys) : type_(type), keys_(keys) {}

    expr::NameRef resolve(std::string_view name) const override {
        expr::NameRef r;
        if (expr::parseUserVar(name, r.family, r.index)) {
            r.kind = expr::NameRef::Kind::UserVar;
            return r;
        }
        const auto var = expr::lookupVar(name);
        if (var && isNativeVar(type_, *var)) {
            r.kind = expr::NameRef::Kind::BuiltinVar;
            r.var = *var;
            return r;
        }
        auto it = keys_.find(expr::toUpper(name));
        if (it != keys_.end()) {
            r.kind = expr::NameRef::Kind::KeyRef;
            r.keySlot = it->second;
            return r;
        }
        if (var) {
            r.kind = expr::NameRef::Kind::BuiltinVar;
            r.var = *var;
            r.nonNative = true;
        }
        return r;
    }

private:
    SceneType type_;
    const std::map<std::string, int>& keys_;
};

}  // namespace

CompiledConfig compileConfig(const SceneFile& file, SceneType type, const CompileOptions& opts) {
    CompiledConfig cfg;
    cfg.type = type;
    cfg.path = file.path;
    cfg.diags = file.diags;
    cfg.particleOnly = file.particleOnly;
    cfg.particleAlso = file.particleAlso;

    // Pass 1: register every winning key so expressions can reference any key.
    // `NumS` is the old name of `Stps` (scene-format §5.1, [VH 2.6.0]); an explicit Stps wins.
    bool hasStps = false;
    for (const SceneEntry* e : file.effectiveEntries()) hasStps = hasStps || e->key == "STPS";
    for (const SceneEntry* e : file.effectiveEntries()) {
        CompiledKey k;
        k.key = e->key;
        if (isWave(type) && !hasStps && k.key == "NUMS") {
            k.key = "STPS";
            hasStps = true;
        }
        k.rawKey = e->rawKey;
        k.text = e->value;
        k.line = e->line;
        bool unusedFamily = false;
        k.tier = tierFor(type, k.key, k.family, k.index, unusedFamily);
        if (unusedFamily)
            cfg.diags.warn("key-unused-for-type",
                           k.rawKey + " is not evaluated by " + std::string(sceneTypeName(type)) + " configs", k.line,
                           k.rawKey);
        cfg.byName[k.key] = int(cfg.keys.size());
        if (k.key != e->key) cfg.byName[e->key] = int(cfg.keys.size());  // alias still readable by its own name
        cfg.keys.push_back(std::move(k));
    }

    // Pass 2: compile.
    const SceneResolver resolver(type, cfg.byName);
    std::array<int, 4> maxIndex = {-1, -1, -1, -1};
    const auto effective = file.effectiveEntries();
    for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
        CompiledKey& k = cfg.keys[i];
        const SceneEntry& e = *effective[i];
        if (k.key == "META") continue;  // metadata string, not an expression
        if (type == SceneType::Solid && (k.key == "MESH" || isSolidLiteral(k.key))) continue;  // literal names
        if (type == SceneType::Dancer) continue;  // literal keys (engine/Dancers.cpp dancerSpec)
        if (type == SceneType::Organic && (k.key == "KIND" || k.key == "AXM" || k.key == "R1" || k.key == "R2" ||
                                           k.key == "R3" || k.key == "R4"))
            continue;  // literal: algorithm name, L-system axiom / rules
        k.prog = expr::compileExpression(e.value, resolver, cfg.diags, opts.lang, e.line, e.rawKey);
        if (k.family >= 0 && k.tier != Tier::Ignored)
            maxIndex[std::size_t(k.family)] = std::max(maxIndex[std::size_t(k.family)], k.index);
        for (const auto& [fam, idx] : k.prog.userRefs) {
            maxIndex[std::size_t(fam)] = std::max(maxIndex[std::size_t(fam)], idx);
        }
    }
    for (int f = 0; f < 4; ++f) cfg.families[std::size_t(f)].assign(std::size_t(maxIndex[std::size_t(f)] + 1), -1);
    for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
        const CompiledKey& k = cfg.keys[i];
        if (k.family >= 0 && k.tier != Tier::Ignored) cfg.families[std::size_t(k.family)][std::size_t(k.index)] = int(i);
    }

    // Named constants / Sped that read user vars run after the A tier.
    for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
        CompiledKey& k = cfg.keys[i];
        if (k.tier == Tier::Pre && !k.prog.userRefs.empty() && k.key != "ASPC") {
            if (k.key == "NUM" || k.key == "STPS")
                cfg.diags.warn("load-order", k.rawKey + " reads user variables before they are evaluated (reads 0)",
                               k.line, k.rawKey);
            else
                k.tier = Tier::PostLoad;
        }
        if (k.tier == Tier::PostLoad) cfg.postLoadOrder.push_back(int(i));
    }

    // Pre tier: Aspc first, then a dependency (topological) order.
    const int numIdx = cfg.indexOf("NUM");
    const int stpsIdx = cfg.indexOf("STPS");
    std::vector<int> state(cfg.keys.size(), 0);  // 0 new, 1 visiting, 2 done
    std::function<void(int)> visit = [&](int i) {
        if (state[std::size_t(i)] == 2) return;
        if (state[std::size_t(i)] == 1) {
            cfg.diags.warn("dependency-cycle", "cyclic reference involving " + cfg.keys[std::size_t(i)].rawKey,
                           cfg.keys[std::size_t(i)].line, cfg.keys[std::size_t(i)].rawKey);
            return;
        }
        state[std::size_t(i)] = 1;
        const auto& p = cfg.keys[std::size_t(i)].prog;
        std::vector<int> deps(p.keyRefs.begin(), p.keyRefs.end());
        if (p.usesVar(Var::NUM) && numIdx >= 0) deps.push_back(numIdx);
        if (p.usesVar(Var::NUM_S_STEPS) && stpsIdx >= 0) deps.push_back(stpsIdx);
        for (int d : deps)
            if (d != i && cfg.keys[std::size_t(d)].tier == Tier::Pre) visit(d);
        state[std::size_t(i)] = 2;
        cfg.preOrder.push_back(i);
    };
    const int aspc = cfg.indexOf("ASPC");
    if (aspc >= 0 && cfg.keys[std::size_t(aspc)].tier == Tier::Pre) visit(aspc);
    for (std::size_t i = 0; i < cfg.keys.size(); ++i)
        if (cfg.keys[i].tier == Tier::Pre) visit(int(i));

    // Report tier violations that matter: Stps cannot see A vars [RC].
    return cfg;
}

std::optional<SceneType> sceneTypeFromPath(std::string_view path) {
    std::string p(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    const auto pos = p.rfind(".package/");
    if (pos == std::string::npos) return std::nullopt;
    const auto start = p.rfind('/', pos);
    const std::string pkg = p.substr(start == std::string::npos ? 0 : start + 1, pos + 8 - (start == std::string::npos ? 0 : start + 1));
    auto t = sceneTypeFromPackage(pkg);
    if (t && *t == SceneType::WaveShape && p.compare(pos + 9, 10, "Particles/") == 0) return SceneType::Particle;
    return t;
}

}  // namespace reforce::scene
