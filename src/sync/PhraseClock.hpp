#pragma once
// reforce extension (PLAN 5O.1): phrases on top of the beat clock (sync/BeatClock.hpp).
//
// Dance music is built in phrases of 8, 16 or 32 bars; big changes (a drop, a breakdown, a new layer of drums) land
// on phrase lines. The phrase clock places the beat clock's running count in phrases of `bars` bars:
//
//   phrase = (beat - origin) / (bars * quantum)      running phrase count; wrap(phrase) = position in the phrase
//   bar    = (beat - origin) / quantum               running bar count, aligned to the phrase
//
// The origin (where a phrase starts) comes from, in order of trust:
//   deck    startAt(): the song Peter just brought in started here (sync/DeckStarts, PLAN 5O.8). Auto never moves
//           it; the buttons below still do, until the next song.
//   manual  startNow(): "a phrase starts now" (snapped to the nearest bar line with Link, else the nearest beat),
//           nudge(): +-1 bar.
//   auto    energy changes at beat lines: after every beat, the boundary two beats back is scored by how much the
//           loudness of the 2 beats after it differs from the 4 beats before (|log ratio|, from minScore up). Scores
//           are added to a histogram over the phrase's beats that forgets over memoryPhrases phrases; its peak, once
//           clear (>= minPeak and >= peakRatio x the next best), is the origin. A manual start votes too, so
//           auto keeps it unless the music disagrees.
//   none    the beat clock's own zero (Link: the session timeline; rekordbox's bar lines, but phrase lines only
//           by luck).
// Single-threaded (the render thread, once per sim step), pure (unit-tested without audio).

#include <string>
#include <vector>

#include "sync/BeatClock.hpp"

namespace reforce::sync {

struct PhraseConfig {
    int bars = 16;               // reforce.Phrase.Bars: bars per phrase (1..64)
    bool autoAlign = true;       // reforce.Phrase.AutoAlign
    double memoryPhrases = 4.0;  // histogram time constant, phrases
    double minScore = 0.25;      // |log loudness ratio| below this is not a change
    double minPeak = 1.0;        // histogram peak needed before auto trusts it
    double peakRatio = 1.5;      // ... and this many times the next best (not its neighbours)
    double switchRatio = 1.25;   // auto moves an existing origin only when the new peak beats it by this much
    double manualVote = 3.0;     // startNow() adds this at the new origin
};

struct PhraseState {
    bool on = false;          // the beat clock has a source
    int bars = 16;
    double quantum = 4.0;
    double origin = 0.0;      // beats, 0 <= origin < bars * quantum
    double phrase = 0.0;      // running phrase count (0 while off)
    double bar = 0.0;         // running bar count aligned to the phrase
    int barInPhrase = 0;      // 0 .. bars-1
    std::string how = "none"; // none | auto | manual | deck
    int deck = -1;            // how == deck: rekordbox deck 0..3 whose start set it
    bool aligned = false;     // origin from auto (clear peak) or manual
    double confidence = 0.0;  // auto: peak / (peak + next best); manual: 1
};

class PhraseClock {
public:
    explicit PhraseClock(PhraseConfig cfg = {}) : cfg_(cfg) {}
    void setConfig(const PhraseConfig& cfg);
    const PhraseConfig& config() const { return cfg_; }

    // One sim step: the beat clock's state after its update, and the loudness now (any non-negative scale).
    const PhraseState& update(const BeatClockState& clock, double loudness);
    const PhraseState& state() const { return state_; }

    void startNow();        // a phrase starts at the current beat (snapped)
    void nudge(int bars);   // move the origin by whole bars (+ = the phrase starts later)
    // A phrase starts at `beat` of the beat clock (snapped to the nearest bar line with Link, else beat): a song brought in on rekordbox
    // `deck` (sync/DeckStarts). Kept until the next one, a button or a source switch.
    void startAt(double beat, int deck);
    void reset();           // forget the origin and the histogram
    // Outside evidence that a phrase starts at `beat` (a drop: sync/DropDetector), added to the histogram.
    void hint(double beat, double weight);

    const std::vector<double>& histogram() const { return hist_; }  // per beat of the phrase (diagnostics, tests)

private:
    int length() const;     // beats per phrase
    void vote(long long beatIndex, double score);
    void pickAuto();
    void place();           // state_ from the clock beat and origin

    PhraseConfig cfg_;
    PhraseState state_;
    BeatSource source_ = BeatSource::None;
    double beat_ = 0.0, quantum_ = 4.0;
    bool bars_ = false;
    double origin_ = 0.0;
    std::string how_ = "none";
    int deck_ = -1;
    double confidence_ = 0.0;
    std::vector<double> hist_;
    // per-beat loudness
    long long curBeat_ = 0;
    bool haveBeat_ = false;
    double sum_ = 0.0;
    int count_ = 0;
    std::vector<double> recent_;          // loudness of the last completed beats, oldest first
    long long recentLast_ = 0;            // beat index of recent_.back()
};

}  // namespace reforce::sync
