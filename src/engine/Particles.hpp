#pragma once
// Particles (scene-format §5.2): WaveShape-format configs that live for a
// while on top of the current WaveShape. Several may be alive at once.
//
// Lifetime (probe #19, measured on G-Force 5.9.7):
//   age in [0, D)            fade-in:  envelope = fn(age / D)
//   age in [D, D + hold)     full pen: envelope = 1
//   age in [D + hold, 2D + hold) fade-out: envelope = 1 - fn(tau / D), tau = age - D - hold
// D = Particle.Transition.Duration (6 s, +- Deviation), fn = Particle.Transition.Function
// (default 1-(1-t)^1.5, so the fade-out is (1 - tau/D)^1.5 as measured), hold = PDur
// from the config, else Particle.Duration (25 s, +- Duration.Deviation 0.2).
// The envelope multiplies Pen and is exposed to expressions as FADER.
// START_TIME = spawn time; dt = age / (2D + hold), the whole visible life (probe #43). END_TIME is NOT the end:
// G-Force sets END_TIME = START_TIME + 5 + (D + hold) / 2 (probe #43, runs 33-36: 13 lives from 5 to 34 s, fades
// 2/4/6 s; deterministic, independent of the other duration prefs), so dt != (t - START_TIME) / (END_TIME -
// START_TIME) despite the comment in the stock "Spinners" config.
//
// Particle segments are appended after the main WaveShape's, so they draw on
// top with the same overwrite semantics (probe #15).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/Blend.hpp"
#include "engine/LineGeometry.hpp"
#include "engine/Scheduler.hpp"
#include "engine/Transition.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

// Fade-in / hold / fade-out envelope (probe #19). Also used for sprites.
struct FadeEnvelope {
    double fade = 6.0;  // seconds, fade-in and fade-out each
    double hold = 25.0; // seconds at full pen between the fades
    std::shared_ptr<const TransitionFunction> fn;  // null = linear
    // Early fade-out (a newer background sprite, a kill): from age `earlyOut`
    // the envelope is 1 - fn(earlyX + (age - earlyOut) / outFade), where
    // outFade = earlyFade when >= 0, else `fade`; with earlyLinear the curve is
    // the identity (a straight line from the current value, probe #44).
    double earlyOut = -1.0, earlyX = 0.0, earlyFade = -1.0;
    bool earlyLinear = false;

    double total() const;
    // Envelope at `age` seconds after the start (0 outside the lifetime).
    double operator()(double age) const;
    // Duration of a full early fade-out.
    double outFade() const;
    // Starts the fade-out at `age` from the current value (continuous). A full
    // fade-out lasts `outFade` seconds (< 0: the instance's own fade; 0: cut).
    // An early fade-out already running is only ever shortened. linear: see earlyLinear.
    void fadeOutAt(double age, double outFade = -1.0, bool linear = false);
    // Past full strength at `age`: fading out (on its own or early), or over.
    bool fadingOut(double age) const {
        return earlyOut >= 0.0 ? age >= earlyOut : age >= fade + hold;
    }
};

// Seconds at full strength for a lifetime given in bars (PBar, solids and organics). With the beat clock (sync.on,
// sync.bpm, sync.bar) the fade-out starts on the bar line nearest to `bars` bars after the fade-in, or on the nearest
// phrase line (every `phraseBars` bars of SYNC_BAR) when `bars` is a whole number of phrases; the hold never drops
// below half of `bars`. Without the clock: `bars` at 120 BPM. A bar is `beatsPerBar` beats (Link's quantum). The tempo is the one at the start (a later tempo change
// does not stretch it). NaN for bars that are not finite and > 0.
double holdForBars(double bars, double fade, const scene::SyncInput& sync, int phraseBars, double beatsPerBar = 4.0);
// The lines holdForBars ends on, in bars: the phrase length when `bars` is a whole number of phrases, else 1.
double holdLineBars(double bars, int phraseBars);

// After holdForBars: full strength keeps ending on a line of the live clock (Peter 2026-11-23: the dragons, then the
// solids, "always phrase aligned"). The time left is counted in bars as the clock runs (so a tempo change moves the
// end with it), a jump of the clock is not counted, and every step the end snaps to the nearest line, `unit` bars
// apart, of the clock as it is now (a deck start, a phrase re-alignment, the lock arriving). Without the beat clock the
// bodies' fallback: 2 beats a second of age, 4 beats a bar. Organics (OrganicRun) and solids (SolidRun) use it.
struct BarHold {
    double unit = 0;  // line spacing in bars (holdLineBars); 0 = off
    double left = std::nan(""), last = std::nan("");
    void follow(FadeEnvelope& env, double age, const scene::SyncInput& sync);
};

// Slideshow spawn timer: the interval counts from one spawn to the next
// (particles overlap; GUESS for G-Force, whose docs say "when G-Force rolls
// some dice"). Arms on the first poll; returns true when a spawn is due and
// re-arms from `now`.
bool pollSpawn(LayerSlideshow& slideshow, double now, SchedRng& rng);

// One live particle.
class ParticleInstance {
public:
    // `defaultHold` is used when the config has no PDur.
    ParticleInstance(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env,
                     std::string name, double startTime, double fade, double defaultHold,
                     std::shared_ptr<const TransitionFunction> fn);

    const std::string& name() const { return name_; }
    double startTime() const { return start_; }
    double endTime() const { return start_ + env_.total(); }  // when it is gone
    double scriptEndTime() const;                               // the END_TIME variable (probe #43)
    const FadeEnvelope& envelope() const { return env_; }
    double fader(double t) const { return env_(t - start_); }
    bool expired(double t) const { return t - start_ >= env_.total(); }
    // Starts the fade-out now over `outFade` seconds (< 0: its own fade).
    void fadeOutFrom(double t, double outFade = -1.0) { env_.fadeOutAt(t - start_, outFade); }
    const scene::WaveShapeInstance& instance() const { return *inst_; }

    // Evaluates one frame (START_TIME/END_TIME/dt/FADER set) into normalized
    // morph paths with Pen multiplied by the envelope.
    // With a pool, the steps of a heavy particle are evaluated in parallel
    // (bit-identical to serial, Phase 5D).
    void evaluate(const scene::WaveShapeFrameInput& in, MorphFrame& out, scene::WorkerPool* pool = nullptr);

private:
    std::shared_ptr<const scene::CompiledConfig> cfg_;
    std::unique_ptr<scene::WaveShapeInstance> inst_;
    std::string name_;
    double start_ = 0;
    FadeEnvelope env_;
    scene::WaveShapeFrame frame_;
};

// The live particles, in spawn order (later spawns draw on top).
class ParticleSystem {
public:
    static constexpr std::size_t kMaxAlive = 16;  // oldest dropped beyond this

    ParticleInstance& spawn(std::unique_ptr<ParticleInstance> p);
    void killAll() { alive_.clear(); }
    // Fades every live particle out over `fade` seconds from `t` (0 = remove now).
    void fadeOutAll(double t, double fade);
    // Drops expired particles, evaluates the rest and appends their segments.
    void step(const scene::WaveShapeFrameInput& in, const FrameMapping& map, const LineStyle& style,
              std::vector<LineSegment>& out);
    const std::vector<std::unique_ptr<ParticleInstance>>& alive() const { return alive_; }
    std::vector<std::string> names() const;
    void setPool(scene::WorkerPool* pool) { pool_ = pool; }

private:
    std::vector<std::unique_ptr<ParticleInstance>> alive_;
    scene::WorkerPool* pool_ = nullptr;
    MorphFrame frame_;
};

}  // namespace reforce::engine
