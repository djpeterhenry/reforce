#pragma once
// reforce extension (PLAN 5L.4, docs/3d-design.md): one live 3D solid from a scene file in reforce.Solid.package.
//
// Like a sprite (engine/Sprites.hpp): the config is evaluated per frame (scene::SolidInstance: B vars, then the pose
// and shading keys), the lifetime is a fade-in / hold / fade-out envelope (FADER; PBar or PDur, else Object.Duration), and
// Pen / EPen / Flag pick the sprite layering and blend of its composite. The GPU side is render/SolidPipeline.
//
// Scene keys (docs/scene-format.md "Solids"):
//   Mesh   literal: tetrahedron | cube | octahedron | dodecahedron | icosahedron | sphere | torus | prism
//          (default icosahedron)
//   Text   0 = the current WaveShape on the faces (default), 1 = the previous frame, 2 = flat
//   Pers   1 = perspective (default), 0 = orthographic
//   Flag   sprite blend flags (engine::sprite::Flag; 0 = keyed copy)
//   PDur   seconds at full strength (else Object.Duration)
//   per frame: X Y Z (sprite convention, +-1 = the frame edges at z = 0; +z towards the viewer), Scal (circumradius
//   in frame half-heights, default .5), RotX RotY RotZ (radians, applied X, then Y, then Z), Fill (.15), Shad (.6),
//   Edge (.8), Pen, EPen (no Pen and no EPen: Pen 1).
//   ColorKey (load, default 1): 0 = the solid stays opaque in the Syphon alpha and over the camera backdrop (it is
//   not keyed out where its colours are dark).
//   Over (load, default 0): 1 = Pen / EPen draw the solid opaque over the frame, its coverage as alpha (exact
//   intensities, dark parts included), instead of the sprite blend that Flag picks.
//   rubik / fifteen: Moves (load, 30; rubik 1..32, fifteen 1..64), Turn and Seed per frame (engine/Rubik.hpp,
//   engine/Fifteen.hpp frameAt; no Turn = solved); fifteen: Shuf per frame (1).

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "engine/Meshes.hpp"
#include "engine/Particles.hpp"
#include "engine/Sprites.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

// One solid for one frame: what the solid pass and the sprite composite need.
struct SolidSpec {
    MeshKind mesh = MeshKind::Icosahedron;
    SolidPose pose;
    bool perspective = true;
    int text = 0;  // 0 WaveShape, 1 frame, 2 flat
    double fill = 0.15, shade = 0.6, edge = 0.8;
    double pen = 1, epen = 0;  // clamped 0..1
    bool hasPen = true, hasEPen = false;
    double opacity = 0;        // fade envelope (FADER)
    unsigned flags = 0;        // sprite blend flags
    bool colorKey = true;      // ColorKey = 0: opaque in the Syphon alpha / over the backdrop
    bool over = false;         // Over = 1: drawn opaque over the frame (coverage as alpha), Flag ignored
    // rubik mesh (engine/Rubik.hpp): Moves, Turn (0..2 Moves), Seed
    int moves = 30;
    double turn = 60, seed = 0;
    double shuffle = 1;  // fifteen: Shuf
    bool glass = false;  // fifteen: Glas = 1, drawn as glass over the colorized frame
    double frost = 12, tint = 0.35, bend = 1;
};

// Mesh name of a compiled solid config: its Mesh key, default icosahedron; nullopt for an unknown name.
std::optional<MeshKind> solidMesh(const scene::CompiledConfig& cfg, std::string* written = nullptr);

class SolidRun {
public:
    // `mesh` from solidMesh. `defaultHold`: seconds at full strength when the config has no PDur.
    SolidRun(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env, std::string name,
             MeshKind mesh, double startTime, double fade, double defaultHold,
             std::shared_ptr<const TransitionFunction> fn);

    const std::string& name() const { return name_; }
    MeshKind mesh() const { return mesh_; }
    int text() const { return inst_->text(); }
    double startTime() const { return start_; }
    double endTime() const { return start_ + env_.total(); }
    const FadeEnvelope& envelope() const { return env_; }
    bool expired(double t) const { return t - start_ >= env_.total(); }
    void fadeOutFrom(double t, double outFade = -1.0) { env_.fadeOutAt(t - start_, outFade); }
    // Past full strength (fading out or over) at `t`: no longer counts against MaxLive.
    bool fadingOut(double t) const { return env_.fadingOut(t - start_); }
    // PBar (NaN when absent) and an explicit hold in seconds (PBar through engine::holdForBars).
    double bars() const { return inst_->bars(); }
    void setHold(double seconds) {
        if (std::isfinite(seconds) && seconds >= 0) env_.hold = std::min(seconds, 24.0 * 3600.0);
    }

    SolidSpec evaluate(double t, const SpriteAudio& audio = {});

private:
    std::shared_ptr<const scene::CompiledConfig> cfg_;
    std::unique_ptr<scene::SolidInstance> inst_;
    std::string name_;
    MeshKind mesh_;
    double start_ = 0;
    FadeEnvelope env_;
};

}  // namespace reforce::engine
