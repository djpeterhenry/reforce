#pragma once
// reforce extension (PLAN 5J.4): a movie sprite's playhead locked to the music's beat.
//
// A movie with BPM / BeatStart / BeatsPerBar (MediaOptions.hpp, found by tools/dance_sync) plays so that its own
// beats land on the beat clock's beats (sync/BeatClock.hpp: Link, or the audio PLL):
//   - Octave: the movie is played at r = 1/2, 1 or 2 of its beats per music beat, whichever puts the playback
//     rate (music BPM * r / movie BPM) closest to 1; it only switches when another is clearly better. A rate
//     outside [minRate, maxRate] even then: not synced.
//   - Speed: each step advances the media time by the clock's beat delta * r * 60 / movie BPM, so tempo changes
//     (Link or the DJ riding the pitch) follow at once and smoothly.
//   - Phase: the movie's beat position is pulled toward the music's (bar position with Link's bars, else beat),
//     by a slew limited to +-maxSlew of the step, so corrections never look like a jump.
//   - Start: the first synced step jumps forward (within one bar) so the movie starts in phase.
// Without a confident clock (or a BPM) the movie plays at its own rate, from wherever it is. Media time is
// unwrapped and only moves forward (looping is the decoder's job; a section that is a whole number of bars
// keeps the phase across loops: video::SourceSpec::loopUnit).

namespace reforce::engine {

struct ClipBeats {
    double bpm = 0;         // 0 = unknown: never synced
    double beatStart = 0;   // seconds of a downbeat
    int beatsPerBar = 4;
};

struct BeatSample {         // from sync::BeatClockState (engine doesn't depend on sync)
    bool confident = false;
    int source = 0;         // changes = a different beat count
    double bpm = 0, beat = 0;
    bool bars = false;
};

struct BeatSyncConfig {
    double minRate = 0.5, maxRate = 2.0;
    double slewSeconds = 0.5;   // phase error time constant
    double maxSlew = 0.25;      // correction <= this share of the step
    double switchMargin = 0.15; // octave switch needs |log rate| this much better (natural log)
};

class MovieBeatSync {
public:
    MovieBeatSync(const ClipBeats& clip, double startMedia, double nativeRate, BeatSyncConfig cfg = {});
    // Advances by dt (seconds) and returns the unwrapped media time to show. `enabled` = beat sync on.
    double step(double dt, const BeatSample& clock, bool enabled);

    double mediaTime() const { return u_; }
    bool synced() const { return synced_; }
    double rate() const { return rate_; }   // media seconds per second in the last step
    double ratio() const { return r_; }     // movie beats per music beat (octave)
    double phaseError() const { return err_; }  // movie beats, last synced step

private:
    double pickRatio(double musicBpm) const;

    ClipBeats clip_;
    BeatSyncConfig cfg_;
    double native_ = 1.0;
    double u_ = 0, rate_ = 0, r_ = 1.0, err_ = 0;
    double lastBeat_ = 0;
    int lastSource_ = -1;
    bool synced_ = false, everSynced_ = false;
};

}  // namespace reforce::engine
