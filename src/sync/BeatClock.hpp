#pragma once
// reforce extension (PLAN 5J.4): one running beat count for beat-synced movies, from the best available source.
//
//   Link   an Ableton Link session (>= 1 peer): its beat count, tempo and bar phase, exactly.
//   Audio  the analyzer's BEAT pulse and TEMPO (audio/BeatTracker.hpp), which have a tempo but no phase. The
//          clock adds it: the beat count advances at TEMPO, and every onset is placed on the count's beat
//          circle (onset time from the pulse: BEAT = exp(-age / decay)) and added to a decaying average,
//          weighted by how strongly it stood out (kicks over off-beat hats or bass). The count is slewed toward
//          the average's direction, so the strongest cluster of onsets lands on whole beats. The average's length
//          (0..1: how consistently onsets fall at one phase) is the lock; with equally strong on- and off-beat
//          onsets it stays low and the clock says it is not confident rather than guess. Through breakdowns the
//          count freewheels at the last tempo until holdSeconds pass. No bar phase: beats only.
//   None   neither is usable (no Link, no audio tempo).
//
// Source preference (reforce.BeatClock.Source): auto = Link when it has peers, else audio; link; audio; off.
// The beat count only moves forward within a source; switching sources starts a different count (users of the
// clock compare beat deltas and phase, so they cope). Single-threaded (the render thread, once per sim step).

#include <string>

namespace reforce::sync {

enum class BeatSource { None, Link, Audio };
const char* beatSourceName(BeatSource s);

struct BeatClockState {
    BeatSource source = BeatSource::None;
    bool confident = false;  // usable for sync (Link: always; audio: locked)
    double bpm = 0.0;
    double beat = 0.0;       // running beat count (monotonic within a source)
    bool bars = false;       // beat 0, quantum, 2*quantum, ... are downbeats (Link)
    double quantum = 4.0;
    double lock = 0.0;       // audio: phase concentration 0..1 (Link: 1)
};

struct BeatClockInput {
    bool linkOn = false;     // Link session with >= 1 peer
    double linkBpm = 0.0, linkBeat = 0.0, linkQuantum = 4.0;
    double audioTempo = 0.0; // TEMPO (0 = unsure)
    double audioBeat = 0.0;  // BEAT pulse 0..1
    double audioStrength = 1.0;  // latest onset's strength (AudioFrame::beatStrength)
    double audioLowShare = 1.0;  // latest onset's kick-band share 0..1 (AudioFrame::beatLowShare)
    double audioKickness = -1;   // latest onset's kick-ness 0..1 (BeatTracker::lastOnsetKickness); < 0: unknown
    double audioBeatness = -1;   // latest onset's on-beat probability (BeatTracker::lastOnsetBeatness); < 0: unknown
    double kickPhase = -1;       // beat fraction of the kick band now, 0 = on a kick (BeatTracker::kickPhase); < 0: none
    double kickPhaseLock = 0;    // its concentration 0..1 (BeatTracker::kickPhaseLock)
    double foldPhase = -1;       // the learnt beat's fraction now (BeatTracker::foldPhase); < 0: none
    double foldConf = 0;         // its confidence 0..1 (BeatTracker::foldConfidence)
    double filterPhase = -1;     // the phase filter's beat fraction now (BeatTracker::filterPhase); < 0: none
    double filterConf = 0;       // its confidence 0..1 (BeatTracker::filterConfidence)
    bool audioKick = true;       // a kick is playing (BeatTracker::kickPresent; AudioFrame::kickPresent)
};

struct BeatClockConfig {
    std::string source = "auto";      // auto | link | audio | off
    double audioOffsetMs = 10.0;      // + = the audio clock's beats earlier: the detector's own delay (BeatTracker
                                      //   onset timestamps are ~10 ms after the attack, tools/beat_eval)
    double outputOffsetMs = 0.0;      // + = the audio source's reported count runs this much ahead (latency nudge;
                                      //   the phase loop itself is unaffected). Link: the app samples ahead instead
    double pulseDecaySeconds = 0.12;  // BeatConfig::pulseDecaySeconds
    double memorySeconds = 4.0;       // onset phase average decays with this time constant
    double slewSeconds = 0.6;         // phase correction time constant
    double lockOn = 0.5, lockOff = 0.3;  // hysteresis for "confident"
    double minWeight = 3.0;           // decayed onset weight needed before trusting the phase
    double holdSeconds = 4.0;         // freewheel without a tempo for this long
    // Steady tempo (reforce.BeatClock.SteadyTempo, with audio/BeatTracker's steady mode): once locked, the audio
    // clock stays locked while the tracker keeps a tempo (breakdowns), and only onsets near our beats
    // (|phase| < gatePhase) correct the phase, so off-beat stabs and hats cannot drag it. Kick-like onsets: kick-band
    // share >= kickShare (BeatTracker::lastOnsetLowShare: ~0.4 for a kick, 0.05-0.3 for chord stabs and hats).
    // Onsets are weighted (lowFloor + (1 - lowFloor) * min(1, share / kickShare))^2. Re-acquire: escapeKicks
    // kick-like onsets in a row off our beats (a shifted grid) drop the lock and start over.
    bool steady = true;
    double gatePhase = 0.25;
    double kickShare = 0.7;   // with audioBeatness (half-beat model, default); 0.5 suits audioKickness, 0.35 audioLowShare
    bool useBeatness = true; 
    double beatnessKickMix = 0.0;  // 0..1: blend the learnt score with kick-ness (geometric), for kick-less riffs  // judge onsets by audioBeatness (learnt) instead of kick-ness / share
    bool useKickness = true;  // judge onsets by audioKickness (when known) instead of audioLowShare: on Peter's
                              //   library (tools/beat_eval, held-out songs) on the beat 42.2 -> 45.4 % of the time
    double lowFloor = 0.3;
    int escapeKicks = 8;
    // Steady, while locked: the phase is corrected with slewSeconds / (kick share of the remembered onset weight),
    // at most maxSlewSeconds, so a breakdown's stabs and hats barely move it while the kick is away (music without
    // a kick still gets slow drift correction).
    double maxSlewSeconds = 20.0;
    // Steady, locked, no kick (a breakdown; audioKick false): no phase correction at all, the count runs on at the
    // held tempo. To make that exact, while locked each kick near our beat (< trimPhase) also trims the tempo
    // (second-order loop: integration time trimSeconds, at most maxTrim); other onsets never touch it.
    double trimSeconds = 8.0;
    double maxTrim = 0.001;   // relative (tools/beat_eval: larger trims made the clock's tempo worse on real music)
    double trimPhase = 0.08;  // only kicks this close to our beat (beats) trim
    // Kick phase (PLAN 5J.12): steer the count to the kick band's phase (BeatTracker::kickPhase) whenever it is
    // concentrated enough (kickLockOn to lock, kickLockOff to keep); onsets are used only without it.
    bool useKickPhase = false;
    double kickLockOn = 0.4, kickLockOff = 0.25;
    // Off-beat rescue (steady, locked, kick playing): when our beat and the kick phase disagree by more than
    // offbeatPhase (beats) for offbeatSeconds, the count slides over by that difference (onto the kicks).
    bool offbeatRescue = true;
    double offbeatPhase = 0.25, offbeatSeconds = 2.0;
    double offbeatMinLock = 0.4;
    // Rescue from onsets (instead of the kick phase): an ungated average of where the onsets land on our beat,
    // weighted by on-beat probability^rescuePower (and strength), decaying over rescueMemorySeconds. Its angle is
    // the "real beat" for the rescue above.
    bool rescueFromOnsets = true;  // tools/beat_eval: on the beat 71.1 -> 74.2 %, off-beat 5.7 -> 3.6 % (tuning)
    double rescueMemorySeconds = 16.0, rescuePower = 2.0;  // the kick phase must be at least this concentrated to count
    // Rescue from the beat fold (PLAN 5J.12, before the two above): the tracker's learnt "which half is the beat"
    // (foldPhase) is the real beat whenever its confidence is >= foldMinConf.
    // The fold is precise enough to also fix smaller misses: slide when we are > foldPhaseOff (beats) from it.
    // tools/beat_eval (held out): on the beat 74.3 -> 80.9 %, off-beat 6.3 -> 3.7 %; slides 0.5 -> 1.2 per minute.
    bool useFold = true;
    double foldMinConf = 0.6, foldPhaseOff = 0.10;
    bool foldWithoutKick = false;  // also in breakdowns (no kick)
    // Phase filter (PLAN 5J.12, research idea 1): steer the count to BeatTracker::filterPhase while its confidence
    // is >= filterOn to lock / filterOff to keep (as the kick phase above); onsets otherwise. tools/beat_eval: on
    // the beat 88.6 -> 91.9 % (recent 2,000), 81.2 -> 85.8 % (held out); off-beat 2.6 -> 2.3 / 3.7 -> 3.2 %.
    bool useFilter = true;
    double filterOn = 0.5, filterOff = 0.3, filterOffsetMs = 5.0;
    double kickPhaseOffsetMs = 5.0;  // (synthetic kicks: 5 ms) + = our beats earlier than the kick band's phase (its timing vs the attack)
};

class BeatClock {
public:
    explicit BeatClock(BeatClockConfig cfg = {}) : cfg_(std::move(cfg)) {}
    void setConfig(const BeatClockConfig& cfg) { cfg_ = cfg; }
    const BeatClockConfig& config() const { return cfg_; }
    // One simulation step of dt seconds.
    const BeatClockState& update(double dt, const BeatClockInput& in);
    const BeatClockState& state() const { return state_; }
    int rescues() const { return rescues_; }  // off-beat rescue slides so far (diagnostics)

private:
    void updateAudio(double dt, const BeatClockInput& in);
    bool updateFromPhase(double dt, const BeatClockInput& in, double phase, double lock, double lockOn, double lockOff,
                         double offsetMs);  // steer to a phase input; false: not usable, use onsets
    double kickLike(const BeatClockInput& in) const;  // the onset's kick / on-beat score per config  // true: handled this step

    BeatClockConfig cfg_;
    BeatClockState state_;
    // audio phase loop
    double aBeat_ = 0.0, aBpm_ = 0.0;
    double zRe_ = 0.0, zIm_ = 0.0, zW_ = 0.0;  // decayed sum of weight * e^(i 2pi phase) and of weights
    double prevPulse_ = 0.0, sinceTempo_ = 1e9;
    double lock_ = 0.0;
    int rescues_ = 0;
    int offGrid_ = 0;  // steady: kick-like onsets off our beats in a row
    double zK_ = 0.0;  // steady: the kick-like part of zW_
    double trim_ = 0.0, trimBpm_ = 0.0;
    double offbeatEv_ = 0.0, shift_ = 0.0;
    double rRe_ = 0.0, rIm_ = 0.0, rW_ = 0.0;  // rescue-from-onsets average  // off-beat rescue: evidence (s), pending slide (beats)  // steady: relative tempo trim, and the TEMPO it was learnt for
    bool aConfident_ = false, aStarted_ = false;
};

}  // namespace reforce::sync
