#pragma once
// reforce extension (PLAN 5L.4, docs/3d-design.md): one live 3D solid from a scene file in reforce.Solid.package.
//
// Like a sprite (engine/Sprites.hpp): the config is evaluated per frame (scene::SolidInstance: B vars, then the pose
// and shading keys), the lifetime is a fade-in / hold / fade-out envelope (FADER; PBar or PDur, else Object.Duration), and
// Pen / EPen / Flag pick the sprite layering and blend of its composite. The GPU side is render/SolidPipeline.
//
// Scene keys (docs/scene-format.md "Solids"):
//   Mesh   literal: tetrahedron | cube | octahedron | dodecahedron | icosahedron | sphere | torus | prism | rubik |
//          fifteen | labyrinth | knight | soma | model | rain | fractal | jigsaw | hanoi | bubbles | shatter
//          (default icosahedron)
//   Text   0 = the current WaveShape on the faces (default), 1 = the previous frame, 2 = flat
//   Pers   1 = perspective (default), 0 = orthographic
//   Flag   sprite blend flags (engine::sprite::Flag; 0 = keyed copy)
//   PDur   seconds at full strength (else Object.Duration)
//   per frame: X Y Z (sprite convention, +-1 = the frame edges at z = 0; +z towards the viewer), Scal (circumradius
//   in frame half-heights, default .5), RotX RotY RotZ (radians, applied X, then Y, then Z), Fill (.15), Shad (.6),
//   Edge (.8), Pen, EPen (no Pen and no EPen: Pen 1).
//   ColorKey (load, default 1): 0 = the solid stays opaque in the Syphon alpha and over the camera backdrop (it is
//   not keyed out where its colors are dark).
//   Over (load, default 0): 1 = Pen / EPen draw the solid opaque over the frame, its coverage as alpha (exact
//   intensities, dark parts included), instead of the sprite blend that Flag picks.
//   fractal (docs/3d-design.md "Fractal zoom"): Frac, Path (literals), ZBar Puls Drop DrpB Hovr Stpb Res Detl Stps
//   (load), Zoom Orbt LkX LkY Roll Fov Fog Glow (per frame).
//   rubik / fifteen: Moves (load, 30; rubik 1..32, fifteen 1..64), Turn and Seed per frame (engine/Rubik.hpp,
//   engine/Fifteen.hpp frameAt; no Turn = solved); fifteen: Shuf per frame (1).
//   rain (engine/Rain.hpp): Rain (literal window | floor), Slid, Bead, Wipe, Max, Seed at load; per frame Dens, Size,
//   Drz, Clus, Wet, Fog, Refg, Fros, Dpth, Spec, Rim, Spd, Dcay.
//   bubbles (engine/Bubbles.hpp): Src (literal wand | edge | air), Max, Clst, Seed at load; per frame Blow, Size, Pop,
//   Life, Air, Rise, Film, Iris, Swrl, Drn, Bend, Spec, Rim, Dpth, Tint.
//   knight: Sym, Brd (load), Orbt, Folw, Flsh, Hovr per frame; Turn in hops 0..64, Seed (engine/Knight.hpp).
//   labyrinth (engine/Labyrinth.hpp): Grid, Beats, Traps (load: cells per side 4..12, 8; beats per run 8..128, 32;
//   trap holes 0..6, 3), Turn (beats into the run; the ball drops into the goal at Turn = Beats) and Seed (the run:
//   a new maze) per frame, Floor per frame (the board's opacity, .55: the visuals show through).
//   soma (engine/Soma.hpp): Figs (literal: "all" or figure names in round order; "cube"), Len / Hold / Each (load,
//   beats: 32 / 8 / 2), Turn (beats into the round; no Turn = complete) and Seed (the round) per frame, Expl and Tens
//   per frame (0).
//   shatter (engine/Shatter.hpp, docs/3d-design.md "Shatter"): Imp, Cycl, Hit, Spk, Rng, Fork, Crk, Size, Arrv, Seed
//   (load); Brst, Grav, Tilt, Dpth, Spec, Line, Sprk, Gls, Thck per frame.
//   jigsaw (engine/Jigsaw.hpp, docs/3d-design.md "Jigsaw"): Pcs, Ordr, Cycl, Brk, Gap (load); Turn (beats into the
//   cycle; no Turn = whole), Seed, Kick, Shak, Dim, Fros per frame.
//   hanoi (engine/Hanoi.hpp, docs/3d-design.md "Tower of Hanoi"): Disc, Beats, Each, Rset (load); Turn (beats into
//   the run; no Turn = solved), Seed (the run), Lift and Flip (latched per move), Hovr, Fros, Tint, Bend, Dpth, Spec
//   per frame.

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "engine/Bubbles.hpp"
#include "engine/CameraMoves.hpp"
#include "engine/Labyrinth.hpp"
#include "engine/Jigsaw.hpp"
#include "engine/FractalZoom.hpp"
#include "engine/Hanoi.hpp"
#include "engine/Meshes.hpp"
#include "engine/Particles.hpp"
#include "engine/Soma.hpp"
#include "engine/Rain.hpp"
#include "engine/Shatter.hpp"
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
    // labyrinth: this frame (the maze, the ball, the board's tilt) and the board's opacity (Floor)
    labyrinth::Frame labyrinth;
    double floor = 0.55;
    double tiltGain = 1;  // Tilt: the board's drawn tilt x this (1 = as the physics tilts it)
    double frost = 12, tint = 0.35, bend = 1;
    // hanoi: Mono >= 0: every part (discs, pegs, base) one glass color, the ColorMap's at that intensity; -1 (default):
    // each disc its own color, the pegs and base nearly clear
    double mono = -1;
    // knight (engine/Knight.hpp, docs/3d-design.md "Knight's tour"): Sym, Brd (load); Orbt, Folw, Flsh, Hovr (frame).
    // Turn = hops into the tour (0..64; absent: 64, the whole web), Seed picks the tour.
    bool symmetric = true;
    int board = 1;
    double orbit = 0, follow = 0.15, flash = 0, hover = 0;
    // mesh == Soma (engine/Soma.hpp): the pieces this frame.
    soma::Frame soma;
    // mesh == Jigsaw: the puzzle's settings (load) and this frame's position in the cycle; the empty table's look
    // (Gap preset, Dim: NaN = the preset's) and blur (frost, here 10 by default).
    jigsaw::Config jigsaw;
    jigsaw::Input jigsawIn;
    int gap = 0;
    double dim = std::nan("");
    // mesh == Model (docs/model-design.md): the shot this frame and the look.
    ModelShot shot;
    double spin = 0, bank = 0;     // the model's turn about its up axis (radians, Spin x age) and roll (Bank)
    double glow = 1, bloom = 0.6, exposure = 1;
    std::array<double, 3> light{-0.45, 0.6, 0.65};  // towards the key light, camera space (x right, y up, z back)
    // mesh == Rain (docs/3d-design.md "Rain on glass"): the simulation, stepped to this frame by evaluate(), its look,
    // and whether this was its first step (the GPU fields start afresh).
    std::shared_ptr<const rain::Sim> rain;
    rain::Look rainLook;
    bool rainReset = false;
    // mesh == Bubbles (docs/3d-design.md "Bubbles"): the simulation, stepped to this frame by evaluate(), its look.
    std::shared_ptr<const bubbles::Sim> bubbles;
    bubbles::Look bubbleLook;
    // mesh == Shatter (docs/3d-design.md "Shatter"): the pane, stepped to this frame by evaluate(), and its look.
    std::shared_ptr<const shatter::Player> shatter;
    shatter::Look shatterLook;
    // mesh == Fractal (docs/3d-design.md "Fractal zoom"): the camera at this frame's zoom (glow: Glow x the beat
    // pulse; fill / shade: the fractal's defaults .06 / .85 unless the scene sets them).
    FractalCamera fractalCam;
    double zoom = 0;  // 0..1 through the step
    double fog = 3;
    // mesh == Hanoi (docs/3d-design.md "Tower of Hanoi"): this frame's discs, and the glass's look (frost, tint and
    // bend above; depth: Dpth, frame heights from the glass to the visuals; spec: Spec, the highlight).
    hanoi::Frame hanoi;
    double depth = 0.5, spec = 1;
};

// Load-time settings of a fractal solid (Mesh="fractal").
struct FractalSettings {
    FractalKind kind = FractalKind::Sierpinski;
    const FractalPath* path = nullptr;  // null: an unknown Frac or Path (`error` says which)
    FractalTiming timing;               // ZBar, Puls, Drop, DrpB, Hovr, Stpb
    double res = 0.5;                   // Res: render scale .25 .. 1
    double detail = 2.5;                // Detl: the smallest pieces' size in pixels
    int steps = 120;                    // Stps: raymarching steps per pixel
    std::string error;
};

// Load-time settings of a model solid (Mesh="model"): its File, Look, axes and camera move.
struct ModelSettings {
    std::string file;          // File (relative to reforce.Model.Dir, or absolute)
    int look = 0;              // Look: 0 = into the visuals (luminance), 1 = color over them
    std::array<float, 3> front{0, 0, -1}, up{0, 1, 0};  // Front / Up ("+x", "-z", ...; default -z / +y)
    std::string move = "orbit";  // Move: a camera preset's name (content/Camera, <model folder>/Camera), or "free"
    bool freeCamera = false;     // Move = free: CamX..AimZ are the camera
    bool ownPath = false;        // the scene has its own keyframes (K1, ...): they win over the preset
    CameraPath path;             // the keyframes played (the scene's own, or the preset's via setCameraPath)
    double moveSeconds = 24;   // MDur, or MBar converted by the caller (setMoveSeconds)
    double moveBars = std::nan("");  // MBar, NaN when absent
    bool loop = true;          // Loop: 1 = repeat the move, 0 = hold its last frame
    bool trails = false;       // look B with an explicit Pen: a luminance copy also goes into the feedback
    std::string error;         // unknown Front / Up, bad keyframes (logged; defaults used)
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
    // After setHold(holdForBars(...)): keep ending on the live clock's lines (engine::BarHold, as organics).
    void followBars(double unitBars) {
        barHold_.unit = std::isfinite(unitBars) && unitBars > 0 ? std::min(unitBars, 4096.0) : 0.0;
    }
    void setHold(double seconds) {
        if (std::isfinite(seconds) && seconds >= 0) env_.hold = std::min(seconds, 24.0 * 3600.0);
    }

    SolidSpec evaluate(double t, const SpriteAudio& audio = {});

    // Soma solids (mesh() == Soma): the figures (Figs) and the round's beats (Len, Hold, Each); null otherwise.
    const soma::Player* soma() const { return soma_.get(); }
    const soma::Timing& somaTiming() const { return somaTiming_; }
    const std::string& somaUnknown() const { return somaUnknown_; }  // Figs names not found (for the log)

    // Model solids (mesh() == Model).
    const ModelSettings& model() const { return model_; }
    // Fractal solids (mesh() == Fractal).
    const FractalSettings& fractal() const { return fractal_; }
    bool hasDuration() const { return std::isfinite(inst_->duration()) || std::isfinite(inst_->bars()); }
    // The Move preset's keyframes (the caller finds and loads it); the scene's own Ease / Step / Rand / Push win.
    void setCameraPath(CameraPath p);
    void setMoveSeconds(double s) {
        if (std::isfinite(s) && s > 0.1) model_.moveSeconds = s;
    }

    // Rain, bubble and shatter solids (mesh() == Rain, Bubbles, Shatter): the frame's aspect
    // (width / height) they live in; set before the first evaluate (default 16:9).
    void setAspect(double aspect) {
        if (std::isfinite(aspect) && aspect > 0.1 && aspect < 10 && !rain_ && !bubbles_ && !shatter_)
            aspect_ = aspect;
    }
    const bubbles::Settings& bubbleSettings() const { return bubbleSettings_; }
    const bubbles::Sim* bubbleSim() const { return bubbles_.get(); }
    const rain::Settings& rainSettings() const { return rainSettings_; }
    // Tower of Hanoi solids (mesh() == Hanoi): the solve (null otherwise).
    const hanoi::Plan* hanoi() const { return hanoi_.get(); }
    const rain::Sim* rainSim() const { return rain_.get(); }
    // Shatter solids (mesh() == Shatter): the load-time config and seed; the player (null before the first evaluate).
    const shatter::Config& shatterConfig() const { return shatterConfig_; }
    std::uint64_t shatterSeed() const { return shatterSeed_; }
    const shatter::Player* shatterPlayer() const { return shatter_.get(); }

private:
    std::shared_ptr<const scene::CompiledConfig> cfg_;
    std::unique_ptr<scene::SolidInstance> inst_;
    std::string name_;
    MeshKind mesh_;
    double start_ = 0;
    FadeEnvelope env_;
    BarHold barHold_;  // followBars
    ModelSettings model_;
    rain::Settings rainSettings_;
    std::shared_ptr<rain::Sim> rain_;
    bubbles::Settings bubbleSettings_;
    std::shared_ptr<bubbles::Sim> bubbles_;
    double aspect_ = 16.0 / 9.0;
    shatter::Config shatterConfig_;
    std::uint64_t shatterSeed_ = 0;
    std::shared_ptr<shatter::Player> shatter_;
    // Labyrinth solids: the planned runs (made at the start, so frames allocate nothing) and the load-time keys.
    std::unique_ptr<labyrinth::Player> labyrinth_;
    int grid_ = 8, beats_ = 32, traps_ = 3;
    bool catchUpLogged_ = false;
    // Hanoi solids: the solve (made at the start) and the style of the move in flight, latched when it starts.
    std::unique_ptr<hanoi::Plan> hanoi_;
    hanoi::Style hanoiStyle_;
    int64_t hanoiRun_ = 0;
    int hanoiMove_ = -3;
    std::unique_ptr<soma::Player> soma_;
    soma::Timing somaTiming_;
    std::string somaUnknown_;
    FractalSettings fractal_;
    FractalClock fractalClock_;
    double lastT_ = std::nan("");
};

}  // namespace reforce::engine
