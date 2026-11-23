#pragma once
// CPU reference evaluation of compiled configs (architecture §4.3). These are
// the APIs the engine uses: no rendering, just numbers.
//
//   WaveShapeInstance   one frame -> point arrays per (ID, path)
//   ColorMapInstance    256-entry RGB palette
//   FlowFieldInstance   source coordinates per row (or point / full grid);
//                       FlowField Union ALFA masks via the same row machinery
//   SpriteInstance      per-frame X, Y, Pen, EPen
//   SolidInstance       per-frame pose and shading of a 3D solid (reforce extension, PLAN 5L.4)
//   OrganicInstance     procedural growth (trees): per-point keys at the start, per-frame keys (PLAN 5N)
//
// Each instance owns its EvalContext (slot arrays + RNG), so A vars are
// evaluated once per instance and B/C vars persist between evaluations.

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "expr/Vm.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/WorkerPool.hpp"

namespace reforce::scene {

struct Environment {
    double width = 1920.0;          // WIDTH (pixels)
    double height = 1080.0;         // HEIGHT
    int numFftBins = 185;           // NUM_FFT_BINS [VH 3.1]
    int numSampleBins = 200;        // NUM_SAMPLE_BINS (pref Audio.PCM.NumBins; real default unknown)
    double particleDuration = 10.0; // default PDur (pref PREF_PARTICLE_DURATION; value unknown)
    std::uint64_t seed = 1;         // RNG seed for the instance (deterministic mode)
    double loadTime = 0.0;          // `t` while the Pre/A tiers run
    bool sInclusive = true;         // s_k = k/(N-1) (probe-confirmed) or k/N
    int maxInstances = 4096;        // clamp for NUM
    int maxSteps = 16384;           // clamp for NUM_S_STEPS
    bool flowFloat32 = false;       // FlowFields evaluate in float32 (reforce.FlowPrecision)
};

// Shared machinery: Pre -> A -> PostLoad tiers and key evaluation.
class ConfigInstance {
public:
    ConfigInstance(const CompiledConfig& cfg, const Environment& env);
    virtual ~ConfigInstance() = default;

    expr::EvalContext& context() { return ctx_; }
    const CompiledConfig& config() const { return *cfg_; }
    double xExtent() const { return ctx_.get(expr::Var::X_EXTENTS); }
    double yExtent() const { return ctx_.get(expr::Var::Y_EXTENTS); }
    bool aspect() const { return aspc_; }

protected:
    // Evaluates key `i`, stores the result in its key slot (and user-var slot
    // for A-D keys), and returns it.
    const expr::Value& evalKey(int i);
    // Evaluates family `fam` in ascending index order. With laneCount > 1,
    // matrix results as wide as the lane count are reduced to column `lane`.
    void evalFamily(int fam, int lane = 0, int laneCount = 1);
    // Scalar value of an optional key (default when absent or empty).
    double evalScalar(int keyIndex, double fallback, int lane = 0, int laneCount = 1);
    void runLoadTiers();  // Pre, A, PostLoad
    // Per-step tier with VectorC state (probe 2026-11): each C var holds one
    // value per step that persists across frames. Before evaluating step k,
    // every C slot is loaded with step k's value from the previous pass; the
    // family then runs in ascending order and the results are saved back.
    void evalStepFamily(int fam, int step, int stepCount);
    // The same on a given context (lanes of a parallel evaluation). The step
    // store must already be sized (sizeStepStore); distinct steps touch
    // distinct elements, so lanes may run different steps concurrently.
    void sizeStepStore(int fam, int stepCount);
    void evalStepFamilyIn(expr::EvalContext& c, int fam, int step, int stepCount);
    const expr::Value& evalKeyIn(expr::EvalContext& c, int i) const;
    double evalScalarIn(expr::EvalContext& c, int keyIndex, double fallback, int lane, int laneCount) const;

    std::function<void()> on_pre_done_;  // hook between the Pre and A tiers
    std::vector<std::vector<expr::Value>> stepStore_;  // [C index][step]
    // Keys evaluated lane-sliced in per-step tiers (Phase 5D; empty = none):
    // an elementwise program whose matrix inputs are all 1 x stepCount rows
    // computes only column `step` (in doubles) instead of the whole row and
    // then extracting it. Same arithmetic per element, so the same bits; a
    // key is marked only where nothing can observe the row itself.
    std::vector<char> slice_;
    bool runSliced(expr::EvalContext& c, int key, int lane, int laneCount, double& out) const;

    const CompiledConfig* cfg_;
    Environment env_;
    expr::EvalContext ctx_;
    bool aspc_ = false;
    // FlowFields set this before runLoadTiers: with Aspc=1 their pixel grid has
    // equal x and y steps, so the long axis spans (long-1)/(short-1) (5B.4 #36).
    bool pixelGridExtents_ = false;
};

// reforce extension (PLAN 5L.4): the beat clock (sync/BeatClock.hpp) as SYNC_* variables, for every per-frame
// scene type. All 0 while the clock has no source.
struct SyncInput {
    double on = 0.0;    // SYNC_ON: 1 while the clock is confident (Link: always; audio: phase locked)
    double bpm = 0.0;   // SYNC_BPM
    double beat = 0.0;  // SYNC_BEAT: running beat count (whole numbers on the beats)
    double lock = 0.0;  // SYNC_LOCK: audio phase concentration 0..1 (Link: 1)
    double bar = 0.0;     // SYNC_BAR: running bar count, bar lines on whole numbers, aligned to the phrase
    double phrase = 0.0;  // SYNC_PHRASE: running phrase count; wrap(SYNC_PHRASE) = position in the phrase
    double build = 0.0;   // SYNC_BUILD: 0..1 through a breakdown (tension), 0 otherwise
    double drop = 0.0;    // SYNC_DROP: 1 at a drop, decaying over a bar
};
void setSyncVars(expr::EvalContext& ctx, const SyncInput& s);

// ----------------------------------------------------------------- WaveShape

struct WaveShapeFrameInput {
    double t = 0.0;
    expr::AudioInput audio;
    double bass = 0.0, level = 0.0, energy = 0.0;
    double mid = 0.0, treble = 0.0, beat = 0.0, tempo = 0.0;  // reforce extensions
    double linkOn = 0.0, linkBpm = 0.0, linkBeat = 0.0, linkPhase = 0.0;  // Ableton Link (LINK_*)
    SyncInput sync;          // SYNC_*
    double startTime = 0.0;  // particles
    double endTime = 1e30;   // >= 1e30 for regular WaveShapes (spec §5.2)
    double dt = -1.0;        // particles: age / life (probe #43); < 0: derived from start/end (0 for WaveShapes)
    double fader = 1.0;
};

struct WavePath {
    int id = 0;          // instance ID
    int pathIndex = 0;   // k of X#k/Y#k
    std::vector<float> x, y, pen, width;  // NUM_S_STEPS points
};

struct WaveShapeFrame {
    std::vector<WavePath> paths;
    bool connected = false;  // ConB
};

class WaveShapeInstance : public ConfigInstance {
public:
    WaveShapeInstance(const CompiledConfig& cfg, const Environment& env);

    // Evaluates B (per ID), then per s step C, X#/Y#, Pen, LWdt.
    void evaluateFrame(const WaveShapeFrameInput& in, WaveShapeFrame& out);

    // Phase 5D: with a pool, the s steps are evaluated in parallel when the
    // config allows it (parallelMode() != Serial) and the frame is big enough.
    // Results are bit-identical to the serial evaluation: steps only share
    // state the step tier cannot observe in a different order (see
    // analyzeParallel in Evaluate.cpp); configs whose step tier uses the RNG,
    // print, or reads a key before the step writes it stay serial.
    enum class Parallel {
        Serial,  // step tier must run in order
        PerId,   // steps parallel, one dispatch per ID (B reads step-tier state)
        Frame,   // B for every ID first, then all steps in one dispatch
    };
    void setPool(WorkerPool* pool) { pool_ = pool; }
    Parallel parallelMode() const { return parallel_; }
    // Lane slicing of per-step keys (on by default; off = whole-row evaluation, for tests).
    void setSlicing(bool on) {
        if (on) analyzeSlicing();
        else slice_.clear();
    }
    // Minimum program runs per frame (IDs x steps x step keys) worth a dispatch
    // (also used once a serial frame has taken > 0.25 ms). 0: always.
    void setMinParallelWork(long n) { minParallelWork_ = n; }

    int numSteps() const { return numSteps_; }
    int numInstances() const { return numInstances_; }
    const std::vector<int>& pathIndices() const { return pathIdx_; }
    bool connected() const { return connected_; }
    double duration() const { return duration_; }  // PDur (particles)
    double stepParam(int k) const;

private:
    int numSteps_ = 1;
    int numInstances_ = 1;
    bool connected_ = false;
    double duration_ = 0.0;
    std::vector<int> pathIdx_;               // path numbers k, ascending
    std::vector<std::pair<int, int>> pathKeys_;  // (X key index, Y key index), -1 if absent
    int pen_ = -1, lwdt_ = -1;
    bool penPerPath_ = false, widthPerPath_ = false;
    expr::Value sVector_;

    // Parallel evaluation (Phase 5D).
    void analyzeParallel();
    void analyzeSlicing();
    double outputIn(expr::EvalContext& c, int keyIndex, double fallback, int k, int N) const;
    void evalStep(expr::EvalContext& c, WaveShapeFrame& out, int id, int k);
    void evaluateParallel(WaveShapeFrame& out);
    void saveFinal(const expr::EvalContext& lane);
    void restoreFinal();
    Parallel parallel_ = Parallel::Serial;
    WorkerPool* pool_ = nullptr;
    long minParallelWork_ = 6000;
    bool heavy_ = false;                      // a serial frame was slow: parallel from now on
    long stepRuns_ = 1;                       // program runs per step
    std::vector<int> stepKeys_;               // keys written by a step
    std::vector<int> bKeys_;                  // B family keys (ascending index)
    // One per pool lane. Matrices are deep copies owned by the lane, so lanes
    // never share a reference count (shared_ptr contention made heavy shapes
    // slower in parallel than serially).
    struct Lane {
        expr::EvalContext ctx;
        std::vector<std::vector<expr::Value>> bSnap;  // Frame mode: copy of bSnap_
        std::uint64_t stamp = 0;                      // dispatch last synced for
    };
    std::vector<Lane> lanes_;
    std::uint64_t dispatch_ = 0;
    void syncLane(Lane& lane, bool created, bool withSnapshots) const;
    std::vector<std::vector<expr::Value>> bSnap_;  // Frame mode: per ID, B slots then B key values
    std::vector<expr::Value> finalKeys_, finalC_;  // state after the last step (for the next B / frame)
};

// ----------------------------------------------------------------- ColorMap

struct Rgb {
    float r = 0, g = 0, b = 0;
};

class ColorMapInstance : public ConfigInstance {
public:
    static constexpr int kEntries = 256;
    ColorMapInstance(const CompiledConfig& cfg, const Environment& env);

    // Evaluates B once, then C and H/S/V (or R/G/B) for i = k/255, k = 0..255.
    void evaluate(double t, std::array<Rgb, kEntries>& out);
    double recomputeInterval() const { return ccal_; }  // CCal seconds
    bool usesHsv() const { return hsv_; }

private:
    bool hsv_ = true;
    double ccal_ = 0.09;  // factory value is "a little less than 0.1" [CW]
    int h_ = -1, s_ = -1, v_ = -1, r_ = -1, g_ = -1, b_ = -1;
};

Rgb hsvToRgb(double h, double s, double v);

// ----------------------------------------------------------------- FlowField

class FlowFieldInstance : public ConfigInstance {
public:
    FlowFieldInstance(const CompiledConfig& cfg, const Environment& env);

    // FlowField: evaluates one scan row at height y for destination xs,
    // writing source coordinates. C vars run once for the row (scalar rnd),
    // D vars and src keys as row vectors (per-pixel rnd) [NM][FL].
    void evaluateRow(double y, std::span<const double> xs, double* srcX, double* srcY);
    // FlowField Union: ALFA for one row (unclamped).
    void evaluateAlphaRow(double y, std::span<const double> xs, double* alpha);

    std::array<double, 2> evaluatePoint(double x, double y);
    // Full grid, w x h destination pixels spanning +-extents (+y up, row 0 at
    // the top). Output interleaved (srcX, srcY), row-major.
    void evaluateGrid(int w, int h, std::vector<float>& srcXY);
    // Destination x coordinates of a row of `w` pixels (edge pixels on +-X_EXTENTS).
    std::vector<double> rowXs(int w) const;
    double rowY(int row, int h) const;

    bool polar() const { return polar_; }

private:
    void bindRow(double y, std::span<const double> xs);
    bool polar_ = false;
    int srcX_ = -1, srcY_ = -1, srcR_ = -1, srcT_ = -1, alfa_ = -1;
};

// ----------------------------------------------------------------- Sprite

struct SpriteFrame {
    double x = 0, y = 0, pen = 1, epen = 0;
    bool hasPen = true;  // Pen defined (else "background-only": only EPen draws)
    bool hasEPen = false;  // EPen defined
};

struct SpriteFrameInput {
    double t = 0.0, dt = 0.0, fader = 1.0;
    double xpos = 0.0, ypos = 0.0;          // built-in bouncing position
    double startTime = 0.0, endTime = 1e30;
    double bass = 0.0, level = 0.0, energy = 0.0;
    double mid = 0.0, treble = 0.0, beat = 0.0, tempo = 0.0;  // reforce extensions
    double linkOn = 0.0, linkBpm = 0.0, linkBeat = 0.0, linkPhase = 0.0;  // Ableton Link (LINK_*)
    SyncInput sync;                                                        // SYNC_*
};

class SpriteInstance : public ConfigInstance {
public:
    SpriteInstance(const CompiledConfig& cfg, const Environment& env);
    SpriteFrame evaluateFrame(double t, double dt, double fader = 1.0);
    SpriteFrame evaluateFrame(const SpriteFrameInput& in);

    bool hasPen() const { return hasPen_; }
    // Load-time keys (NaN / nullopt-like defaults when absent).
    double sped() const { return sped_; }          // 0 when absent
    bool hasFlags() const { return hasFlag_; }
    unsigned flags() const { return flags_; }      // Flag (0 when absent)
    bool invt() const { return invt_; }            // Invt != 0
    double duration() const { return duration_; }  // PDur, NaN when absent

private:
    bool hasPen_ = false, hasEPen_ = false, hasFlag_ = false, invt_ = false;
    unsigned flags_ = 0;
    double sped_ = 0.0, duration_ = 0.0;
    int x_ = -1, y_ = -1, pen_ = -1, epen_ = -1;
};

// ----------------------------------------------------------------- Solid
// reforce extension (PLAN 5L.4, docs/3d-design.md). Load-time keys: Mesh (a literal name), Text, Flag, PDur, Pers,
// ColorKey, Over, Moves. Per frame: B vars (persist across frames), then X, Y, Z, Scal, RotX, RotY, RotZ, Fill, Shad, Edge,
// Pen, EPen, Turn, Seed, Shuf (Turn / Seed / Moves: the rubik and fifteen meshes, engine/Rubik.hpp,
// engine/Fifteen.hpp; Shuf: fifteen).

struct SolidFrame {
    double x = 0, y = 0, z = 0;           // sprite convention: +-1 = the frame edges at z = 0
    double scale = 0.5;                   // circumradius in frame half-heights
    double rotX = 0, rotY = 0, rotZ = 0;  // radians
    double fill = 0.15, shade = 0.6, edge = 0.8;
    double pen = 1, epen = 0;
    bool hasPen = true, hasEPen = false;
    double turn = 0, seed = 0;  // rubik / fifteen: position in the cycle (turns), scramble seed
    double shuffle = 1;         // fifteen: Shuf, the leap into the scramble (0 = solved places, 1 = scrambled)
    double frost = 12, tint = 0.35, bend = 1;  // fifteen with Glas = 1: Fros (blur px at 1080p), Tint, Bend
};

struct SolidFrameInput {
    double t = 0.0, dt = 0.0, fader = 1.0;
    double startTime = 0.0, endTime = 1e30;
    double bass = 0.0, level = 0.0, energy = 0.0;
    double mid = 0.0, treble = 0.0, beat = 0.0, tempo = 0.0;
    double linkOn = 0.0, linkBpm = 0.0, linkBeat = 0.0, linkPhase = 0.0;
    SyncInput sync;
};

class SolidInstance : public ConfigInstance {
public:
    SolidInstance(const CompiledConfig& cfg, const Environment& env);
    SolidFrame evaluateFrame(const SolidFrameInput& in);

    const std::string& mesh() const { return mesh_; }  // Mesh as written ("" when absent)
    int text() const { return text_; }                 // Text: 0 WaveShape (default), 1 frame, 2 flat
    bool hasFlags() const { return hasFlag_; }
    unsigned flags() const { return flags_; }          // Flag (0 when absent)
    double duration() const { return duration_; }      // PDur, NaN when absent
    double bars() const { return bars_; }              // PBar (bars at full strength), NaN when absent
    bool perspective() const { return pers_; }         // Pers != 0 (default 1)
    bool colorKey() const { return colorKey_; }        // ColorKey != 0 (default 1; 0 = opaque in the Syphon alpha)
    bool glass() const { return glass_; }              // Glas != 0: fifteen drawn as glass over the frame
    bool over() const { return over_; }                // Over != 0: drawn opaque over (coverage as alpha), not by Flag
    int moves() const { return moves_; }               // Moves (scramble length; default 30, 1..64)
    bool hasTurn() const { return turn_ >= 0; }
    // Model solids (Mesh="model", docs/model-design.md): a literal key's text, trimmed ("" when absent); a load-time
    // key's value (fallback when absent); a per-frame key's value this frame (call after evaluateFrame).
    std::string literal(const char* key) const;
    double loaded(const char* key, double fallback) const;
    bool defines(const char* key) const { return cfg_->defines(key); }
    double frameKey(const char* key, double fallback) { return evalScalar(cfg_->indexOf(key), fallback); }

private:
    std::string mesh_;
    int text_ = 0, moves_ = 30;
    bool hasFlag_ = false, pers_ = true, hasPen_ = true, hasEPen_ = false, colorKey_ = true, over_ = false, glass_ = false;
    int turn_ = -1, seed_ = -1, shuf_ = -1, fros_ = -1, tint_ = -1, bend_ = -1;
    unsigned flags_ = 0;
    double duration_ = 0.0;
    double bars_ = std::nan("");
    int x_ = -1, y_ = -1, z_ = -1, scal_ = -1, rx_ = -1, ry_ = -1, rz_ = -1;
    int fill_ = -1, shad_ = -1, edge_ = -1, pen_ = -1, epen_ = -1;
};

// ----------------------------------------------------------------- Organic
// reforce extension (PLAN 5N, docs/organics-design.md). Load-time keys: Kind (a literal name), PDur and the
// algorithm's numbers (Pts, Seg, ...: loaded()). Per point / per root, once (with `s`): the C vars, then PX PY or
// RtX RtY RtA (evaluatePoints). Per frame: B vars, then Grow, TrpX, TrpY, Sway, X, Y, Scal, Rot, Pen, Bud (trees: with `s`
// = beats since the current growth began; Vib, Puls, PAll read through frameKey).

struct OrganicFrame {
    double grow = 12, trpX = 0, trpY = 0, sway = 0.15;   // trees
    double x = 0, y = 0, scale = 1, rot = 0;
    double pen = 1, bud = 0;
    double amp = std::nan(""), strg = std::nan(""), phas = std::nan("");  // pendulums (NaN = the kind's default)
};

using OrganicFrameInput = SolidFrameInput;

class OrganicInstance : public ConfigInstance {
public:
    OrganicInstance(const CompiledConfig& cfg, const Environment& env);
    // `s`: when finite, the value of `s` in the B vars and frame keys (trees: beats since the current growth began).
    OrganicFrame evaluateFrame(const OrganicFrameInput& in, double s = std::nan(""));

    // For k = 0..count-1 with s = k / (count - 1) (0 for one): the C vars in ascending order, then each of
    // `keys` (key name, value when absent), appending to out[i] for keys[i].
    struct PointKey {
        const char* key;
        double fallback;
    };
    void evaluatePoints(int count, std::span<const PointKey> keys, std::vector<std::vector<double>>& out);
    // A Pre-tier key's value, `fallback` when absent or empty.
    double loaded(const char* key, double fallback) const;
    // Evaluates the A vars again (trees: at every strike after the first, so a strike can pick its own place).
    void reloadA() {
        ctx_.randomLanes = 1;
        evalFamily(expr::FamA);
    }
    bool defines(const char* key) const { return cfg_->defines(key); }
    // A per-frame key's value this frame (call after evaluateFrame), `fallback` when absent.
    double frameKey(const char* key, double fallback) { return evalScalar(cfg_->indexOf(key), fallback); }
    // A literal key's text, trimmed ("" when absent): L-system axiom and rules.
    std::string literal(const char* key) const;
    const std::string& kind() const { return kind_; }  // Kind as written, trimmed ("" when absent)
    double duration() const { return duration_; }      // PDur, NaN when absent
    double bars() const { return bars_; }              // PBar (bars at full strength), NaN when absent

private:
    std::string kind_;
    double duration_ = 0.0;
    double bars_ = std::nan("");
    int grow_ = -1, trpX_ = -1, trpY_ = -1, sway_ = -1, x_ = -1, y_ = -1, scal_ = -1, rot_ = -1, pen_ = -1,
        bud_ = -1, amp_ = -1, strg_ = -1, phas_ = -1;
};

}  // namespace reforce::scene
