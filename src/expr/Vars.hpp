#pragma once
// Built-in variables (scene-format §3.4, §3.5, §5) and user-variable names.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace reforce::expr {

enum class Var : std::uint8_t {
    T,            // time in seconds (transition progress for Transition.Function)
    S,            // WaveShape step parameter: scalar per step, row vector in A/B tiers
    I,            // ColorMap intensity 0..1
    X, Y,         // FlowField destination coords (x is a row vector per evaluated row)
    R, Theta,     // FlowField polar coords
    ID,           // WaveShape instance 0..NUM-1
    NUM,          // WaveShape instance count
    NUM_S_STEPS,  // resolved Stps
    BASS, LEVEL, ENERGY,
    END_TIME, START_TIME, DT,
    X_EXTENTS, Y_EXTENTS, WIDTH, HEIGHT,
    PI,
    PATH_ID,      // index k of the X#/Y# path being evaluated (Pen/LWdt)
    FADER,        // sprite/particle fade envelope 0..1
    NUM_FFT_BINS, NUM_SAMPLE_BINS,
    XPOS, YPOS,   // sprite built-in bouncing position
    // reforce extensions (not in G-Force; docs/scene-format.md §3.5): band levels, onset pulse, tempo (BPM)
    MID, TREBLE, BEAT, TEMPO,
    // reforce extensions: Ableton Link session (rekordbox etc.; docs/scene-format.md §3.5)
    LINK_ON, LINK_BPM, LINK_BEAT, LINK_PHASE,
    // reforce extensions: the beat clock (Link when a session runs, else the audio phase lock; PLAN 5L.4)
    SYNC_ON, SYNC_BPM, SYNC_BEAT, SYNC_LOCK,
    // reforce extensions: phrases on the beat clock (sync/PhraseClock.hpp; PLAN 5O.1)
    SYNC_BAR, SYNC_PHRASE,
    // reforce extensions: breakdowns and drops (sync/DropDetector.hpp; PLAN 5O.5)
    SYNC_BUILD, SYNC_DROP,
    Count
};

inline constexpr int kVarCount = int(Var::Count);
static_assert(kVarCount <= 64, "varBit uses a 64-bit mask");

const char* varName(Var v);  // canonical upper-case spelling
// Case-insensitive lookup.
std::optional<Var> lookupVar(std::string_view name);

inline constexpr std::uint64_t varBit(Var v) { return std::uint64_t(1) << unsigned(v); }

// User variable families A#, B#, C#, D# (case-insensitive). Index limit keeps
// slot arrays bounded; the corpus maximum is 31.
inline constexpr int kMaxUserVarIndex = 1023;
enum Family : int { FamA = 0, FamB = 1, FamC = 2, FamD = 3 };
bool parseUserVar(std::string_view name, int& family, int& index);

std::string toUpper(std::string_view s);
std::string toLower(std::string_view s);

}  // namespace reforce::expr
