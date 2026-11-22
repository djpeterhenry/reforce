#pragma once
// reforce extension (PLAN 5P, docs/dancers-design.md): dance moves, dance styles and the choreographer. Pure C++.
//
// A move is a function of the beat phase b (0 .. beats) that writes into a DancePose. Moves come in three parts:
//   - Legs:  footwork (feet, pelvis, body turn and travel)
//   - Arms:  arm patterns (hands, chest and head accents)
//   - Full:  signature moves that drive the whole body (spin, floss, moonwalk...)
// A "routine" for a few bars is either a Legs + Arms combination or a Full move, plus a Variation (mirrored,
// half / double time, a move-specific variant, amplitude). Every move is tagged with the dance styles it belongs to
// and its energy (chill / groove / hype), so a style has dozens of legs x arms combinations x variants.
//
// Styles change the feel too: hip-hop grooves down on the beat, house bounces up, pop snaps to the 8ths, jazz is
// looser. The styles are the dance genres of the mocap moves (docs/dancers-design.md "Styles are the dance genres"):
// a dancer's style is what they dance, and their look is generated to go with it.
//
// The choreographer (one per dancer, or one shared by the crew in together / canon / mirror mode) picks a new
// routine on bar lines every 1-4 bars (always on phrase lines), blends half a beat between routines, follows the
// music (a breakdown -> chill, a build -> hype, a drop -> a jump and then hype), and avoids repeats. Deterministic:
// seeded, and driven only by the beat / bar / music inputs.

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "engine/DancerRig.hpp"

namespace reforce::engine::dance {

// Peter, 2026-11-21: the AIST++ genres, middle and LA hip-hop combined, waack called disco and krump stomp; street
// jazz is jazz and ballet jazz ballet (merged at first, split the same day: the two are as different as any styles).
enum class Style : int { House, Pop, Lock, Break, Disco, Stomp, HipHop, Jazz, Ballet, Count };
constexpr int kStyleCount = int(Style::Count);
const char* styleName(Style s);   // "house", "pop", "lock", "break", "disco", "stomp", "hiphop", "jazz", "ballet"
const char* styleLabel(Style s);  // for people: "hip-hop" for hiphop, else the name
// The names above, "hip-hop", the dances' own names (popping, locking, breaking, waacking, krump, ...) and the
// styles before 2026-11-21 (robot -> pop, rave -> house, retro -> lock, chill -> jazz), so old scene files, settings
// and stream guests still work.
std::optional<Style> parseStyle(std::string_view s);

enum Energy : std::uint8_t { Chill = 1, Groove = 2, Hype = 4 };
enum class Part : std::uint8_t { Legs, Arms, Full };

struct MoveCtx {
    float amp = 1;      // amplitude (energy x the music)
    float sharp = 1;    // 0 loose .. 1 normal .. 2 snappy
    int variant = 0;    // 0..3, move-specific
    float shoulder = 0.15f;  // skeleton shoulder half-width (hand targets are placed relative to it)
    float foot = 0.088f;     // rest stance half-width
    const Skeleton* sk = nullptr;  // the dancer's skeleton (mocap moves size themselves to it; null: the default)
};

using MoveFn = void (*)(float b, const MoveCtx& c, DancePose& p);

struct MoveDef {
    const char* name;
    Part part;
    int beats;            // length of one cycle (1, 2, 4, 8)
    std::uint32_t styles; // bit per Style
    std::uint8_t energy;  // Energy bits
    MoveFn fn;            // null for a mocap move
    int clip = -1;        // >= 0: a mocap move (engine/DanceClips), played by clipPose
};
// The built-in moves, then any mocap moves (DanceClips loadClips).
const std::vector<MoveDef>& moveLibrary();
// Plays move `m` at beat phase b into p (a procedural move's function, or a mocap clip for c.sk).
void runMove(const MoveDef& m, float b, const MoveCtx& c, DancePose& p);
// Replaces the mocap moves at the end of the library (DanceClips); returns the index of the first. Not locked: call
// before dancers run.
int setClipMoves(const std::vector<MoveDef>& moves);
int findMove(std::string_view name);  // index into moveLibrary(), -1 when unknown
constexpr std::uint32_t styleBit(Style s) { return 1u << unsigned(s); }

// How many distinct routines a style offers (legs x arms + full moves, before variants; procedural moves only), for
// tests and docs.
int routineCount(Style s);

// Per-style feel.
struct StyleFeel {
    float sharp;     // default sharpness
    float groove;    // pelvis drop on the beat (+ down groove, - up bounce)
    float halfTime;  // probability of a half-time routine
    float fullOdds;  // probability a routine is a signature (Full) move rather than legs + arms
};
StyleFeel styleFeel(Style s);

// One routine: what plays for a few bars.
struct Routine {
    int legs = -1, arms = -1, full = -1;  // move indices (full >= 0: legs / arms unused)
    int variant = 0;
    bool mirror = false;
    float speed = 1;   // 0.5 half time, 1, 2 double time
    float amp = 1;
    double start = 0;  // beat where it starts (its phase 0)
    double end = 0;    // beat where the next one takes over (exclusive)
    double created = -1e9;  // beat when it was scheduled (a drop's jump cuts in: no fade-in before then)
    // Mocap moves: phase 0 is at `origin`: a routine's own start (on an 8-bar point of the phrase), a basic move's
    // 4-bar line in the phrase grid (it may join mid-cycle). Otherwise phase 0 is `start`.
    bool aligned = false;
    double origin = 0;
};

// Music inputs for one step.
struct MusicState {
    double beat = 0;   // running beat count (SYNC_BEAT, or a free-running clock)
    double bar = 0;    // running bar count (SYNC_BAR)
    double phrase = 0; // running phrase count (SYNC_PHRASE)
    double build = 0;  // 0..1 through a breakdown
    double drop = 0;   // 1 at a drop, decaying
    double bass = 0;   // 0..~1 (smoothed by the caller)
    bool locked = true;
};

struct ChoreoOptions {
    Style style = Style::Pop;
    std::uint32_t seed = 1;
    float sharp = -1;     // < 0: the style's
    float energy = 1;     // amplitude multiplier (Enrg)
    std::vector<int> likes;  // move indices this character prefers (weighted x4); empty = none
    std::uint32_t avoidStyles = 0;  // unused for now
    // Share of routines that are mocap moves of the dancer's style (reforce.Dancer.Mocap; 0 = none, 1 = only those,
    // while the style has some).
    float mocap = 0;
};

class Choreographer {
public:
    explicit Choreographer(const ChoreoOptions& o);

    // Advances to `g` (call once per step, beats non-decreasing apart from small jitter) and returns the pose at
    // beat g.beat - delay (canon: a lagged copy of the same routines). `mirror` mirrors the result.
    DancePose pose(const MusicState& g, const Skeleton& sk, double delay = 0, bool mirror = false);

    // The routine playing at `beat` (after pose() has advanced past it).
    const Routine* routineAt(double beat) const;
    std::string describe(double beat) const;  // "twoStep + raiseRoof (mirror, x2)" — for the panel and logs
    const ChoreoOptions& options() const { return opts_; }
    void setMocap(float share) { opts_.mocap = share; }
    // Try a move (the panel's move browser): from the next bar line on, only `move` (an index into moveLibrary(), a
    // Full move), looped; -1 goes back to the choreography. Drops and the hello / goodbye waves still play.
    void tryMove(int move);
    int triedMove() const { return tryMove_; }
    // Forces the next routine to be a goodbye wave (exit).
    void farewell(double beat);
    void hello(double beat);

private:
    void advance(const MusicState& g);
    // A mocap routine `r` (r.full set, r.start / r.end the plan): its origin and end. A routine (16 bars or an 8-bar
    // half, started on an 8-bar point) plays whole from its bar 1; a basic move (4 bars) runs on the phrase's 4-bar
    // grid, finishes its cycle and changes on the phrase line at the latest. Tried: from its bar 1, until stopped.
    void alignClip(Routine& r, const MusicState& g, bool tried) const;
    // Beats from the phrase line at or before `beat` (0 .. 64), from the music's running phrase count.
    static double phrasePos(double beat, const MusicState& g);
    Routine pick(double start, double end, const MusicState& g, bool dropHit);
    DancePose evaluate(const Routine& r, double beat, const Skeleton& sk) const;

    ChoreoOptions opts_;
    StyleFeel feel_;
    std::deque<Routine> routines_;  // past, current and the next one (scheduled a beat ahead), oldest first
    static constexpr double kBlend = 1.0;  // half the crossfade between routines, beats (centred on the change)
    std::uint32_t rng_;
    std::uint32_t counter_ = 0;
    double lastDropBeat_ = -1e9;
    int lastLegs_ = -1, lastArms_ = -1, lastFull_ = -1;
    double lastPhrase_ = -1;
    int tryMove_ = -1;
    bool tryChanged_ = false;
};

// Social mode (PLAN 5P.6): what a dancer does while watching someone else dance (a partner's call, the soloist in a
// cypher): claps, points at them, raises the roof, nods with arms crossed, hands on hips... over light footwork. The
// reaction changes every 2 bars (seeded per dancer) and keeps the style's feel.
DancePose reactionPose(double beat, std::uint32_t seed, Style style, const Skeleton& sk);
std::string reactionName(double beat, std::uint32_t seed);
// Watching someone with the mocap moves (Peter, 2026-11-21: 100 % AIST): a calm basic move of the style (chill
// energy where there are such), a new one every cycle (4 bars, seeded per dancer), its phase on the beat count. The
// stage turns the dancer towards whoever they watch. `mocap` false (or no mocap moves of the style): reactionPose.
DancePose watchPose(double beat, std::uint32_t seed, Style style, const Skeleton& sk, bool mocap);
std::string watchName(double beat, std::uint32_t seed, Style style, bool mocap);

// Face accents for a pose at a beat: blinks (seeded) and a mouth that opens on hits.
void faceAccents(DancePose& p, double beat, double t, std::uint32_t seed, float energy);

}  // namespace reforce::engine::dance
