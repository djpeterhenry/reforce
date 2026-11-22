#pragma once
// A scene file compiled for one scene type: every key compiled to bytecode and
// assigned to an evaluation tier (scene-format §4, §5; architecture §4.3).

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "expr/Compiler.hpp"
#include "expr/Diagnostics.hpp"
#include "expr/Operators.hpp"
#include "expr/Program.hpp"
#include "expr/Vm.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

namespace reforce::scene {

enum class Tier {
    Ignored,      // not evaluated (Meta, Vers, keys of a family the type does not use)
    Pre,          // once, before A: Aspc, NUM, Stps, PDur, ConB, named constants (dependency order)
    Load,         // A# (ascending index)
    PostLoad,     // once, after A: CCal, Sped, named constants that read A vars
    Frame,        // B# (ascending index), per frame (x per ID for WaveShapes)
    FrameOutput,  // Sprite X, Y, Pen, EPen; Solid X, Y, Z, Scal, RotX/Y/Z, Fill, Shad, Edge, Pen, EPen
    Step,         // C#: per s step (WaveShape), per i (ColorMap), per row (FlowField)
    StepOutput,   // WaveShape X#/Y#/Pen/LWdt, ColorMap H/S/V/R/G/B
    Pixel,        // D#: per pixel (FlowField, Union)
    PixelOutput,  // srcX/srcY/srcR/srcT, ALFA
};

const char* tierName(Tier t);

struct CompiledKey {
    std::string key;     // normalized upper-case
    std::string rawKey;
    std::string text;    // the value as written (literal keys: Meta, a Solid's Mesh)
    int line = 0;
    Tier tier = Tier::Ignored;
    int family = -1;     // A..D family for user vars
    int index = -1;
    expr::Program prog;  // prog.defined == false for empty values
};

struct CompileOptions {
    expr::LanguageOptions lang;
};

class CompiledConfig {
public:
    SceneType type = SceneType::WaveShape;
    std::string path;
    expr::Diagnostics diags;           // loader + compiler diagnostics
    std::vector<CompiledKey> keys;     // one per effective (winning) entry; key slot == index
    std::map<std::string, int> byName; // normalized key -> index
    std::array<std::vector<int>, 4> families;  // family -> (var index -> key index or -1)
    std::vector<int> preOrder;         // Pre tier evaluation order (Aspc first, then dependencies)
    std::vector<int> postLoadOrder;
    bool particleOnly = false;
    bool particleAlso = false;

    const CompiledKey* find(std::string_view key) const;
    int indexOf(std::string_view key) const;
    bool defines(std::string_view key) const;  // present with a non-empty value
    int familySize(int family) const { return int(families[std::size_t(family)].size()); }
    // Sizes the context's slot arrays for this config.
    void prepare(expr::EvalContext& ctx) const;
};

// Built-in variables a scene type provides natively (others still resolve,
// with a warning, and read their binding, normally 0).
bool isNativeVar(SceneType type, expr::Var v);

CompiledConfig compileConfig(const SceneFile& file, SceneType type, const CompileOptions& opts = {});

// True when a defined key reads the beat or tempo: BEAT, TEMPO, LINK_* or SYNC_* (the control panel's
// "Beat-synced" filter for solids).
bool usesBeat(const CompiledConfig& cfg);

// Scene type from a path below a Packages directory, e.g.
// ".../G-Force.WaveShape.package/Particles/x.txt" -> Particle.
std::optional<SceneType> sceneTypeFromPath(std::string_view path);

}  // namespace reforce::scene
