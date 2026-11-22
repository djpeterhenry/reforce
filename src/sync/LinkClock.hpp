#pragma once
// Ableton Link session clock (reforce extension, PLAN 5H). rekordbox (Performance
// mode, LINK on), Ableton Live, Traktor etc. share tempo and beat phase over the
// local network; reforce joins as a peer and reads, never sets, the tempo.
//
// Each frame the app samples the session at "now + offset" (the offset
// compensates display latency so a visual beat lands on the audible beat) and
// fills the LINK_* scene variables (AudioFrame::link*). With at least one peer,
// BEAT/TEMPO follow Link too (reforce.Link.DriveBeat).
//
// Following, not leading (reforce.Link.Follow, default on): when two Link
// sessions meet, the one that has existed longer wins and the other side adopts
// its tempo (Link's Sessions::handleSuccessfulMeasurement, 0.5 s tie margin). A
// reforce that enabled Link at launch would therefore always be the older
// session, and turning LINK on in rekordbox would pull rekordbox to reforce's
// 120 BPM. So reforce keeps Link off and only listens for other Link apps'
// discovery announcements (UDP multicast 224.76.78.75 / ff12::8080, port 20808).
// Once one has been heard for kFollowJoinDelay, it enables Link: its new
// session is younger by more than the margin, so reforce joins theirs. When
// the last peer leaves, it turns Link off and listens again.
//
// The Link library (GPL-2.0-or-later) is used only in LinkClock.cpp; the math
// helpers and LinkFollower below are header-only and unit-tested without a network.

#include <cmath>
#include <memory>

namespace reforce::sync {

struct LinkSnapshot {
    bool enabled = false;    // Link support on (announcing, or listening in follow mode)
    bool listening = false;  // follow mode, Link off: no other Link app heard yet
    bool joining = false;    // another Link app heard: waiting to join / for the session to form
    int peers = 0;           // other apps in the session
    bool on = false;       // enabled && peers > 0 (LINK_ON)
    double bpm = 0.0;      // session tempo (0 when !on)
    double beat = 0.0;     // running beat count at the sampled time (0 when !on)
    double phase = 0.0;    // 0..quantum, position in the bar (0 when !on)
    double quantum = 4.0;  // beats per bar
};

// BEAT while Link drives it: 1 on each Link beat, then exp(-age / decaySeconds)
// (the same pulse shape as the audio onset BEAT, docs/scene-format.md §3.5).
inline double linkPulse(double beat, double bpm, double decaySeconds = 0.12) {
    if (!(bpm > 0.0) || !std::isfinite(beat)) return 0.0;
    const double age = (beat - std::floor(beat)) * 60.0 / bpm;
    return std::exp(-age / decaySeconds);
}

// True when the beat count passed a multiple of `unit` (1 = every beat,
// quantum = every bar) going from prevBeat to beat. Backward jumps (a peer
// re-aligning the session) never count.
inline bool crossedBoundary(double prevBeat, double beat, double unit) {
    if (!(unit > 0.0) || !(beat > prevBeat)) return false;
    return std::floor(beat / unit) > std::floor(prevBeat / unit);
}

// Seconds for a length in beats at `bpm` (0 when bpm is unknown).
inline double beatsToSeconds(double beats, double bpm) { return bpm > 0.0 ? beats * 60.0 / bpm : 0.0; }

// Follow-mode decisions (pure, so they are testable without a network). Times
// in seconds on any monotonic clock.
inline constexpr double kFollowJoinDelay = 1.5;  // heard this long -> join (Link's tie margin is 0.5 s)
inline constexpr double kFollowHeardTtl = 5.0;   // announcements older than this mean the app is gone
inline constexpr double kFollowFormGrace = 5.0;  // after joining, wait this long for the first peer

struct LinkFollower {
    double firstHeard = -1.0;  // start of the current run of announcements (< 0 = none)
    double lastHeard = -1.0;
    double joinedAt = -1.0;    // Link enabled at (< 0 = Link off)
    bool hadPeers = false;     // the session had a peer since joining

    bool joined() const { return joinedAt >= 0.0; }
    bool heard(double now) const { return lastHeard >= 0.0 && now - lastHeard < kFollowHeardTtl; }

    // Another Link app's alive/response announcement (only counted while Link is off:
    // our own Link's announcements must not keep us joined).
    void onAnnouncement(double now) {
        if (joined()) return;
        if (!heard(now)) firstHeard = now;
        lastHeard = now;
    }

    // Returns +1 = enable Link now, -1 = disable Link now, 0 = no change.
    int update(double now, int peers) {
        if (!joined()) {
            // A steady stream (Link announces every ~250 ms), not one stray packet.
            if (heard(now) && now - lastHeard < 1.0 && now - firstHeard >= kFollowJoinDelay) {
                joinedAt = now;
                hadPeers = false;
                return +1;
            }
            return 0;
        }
        if (peers > 0) {
            hadPeers = true;
            return 0;
        }
        // The last peer left, or the session never formed.
        if (hadPeers || now - joinedAt >= kFollowFormGrace) {
            joinedAt = firstHeard = lastHeard = -1.0;
            hadPeers = false;
            return -1;
        }
        return 0;
    }
};

class LinkClock {
public:
    // follow = reforce.Link.Follow (see above); test peers use false (join at once, like a DJ app).
    explicit LinkClock(double quantum = 4.0, double initialBpm = 120.0, bool follow = false);
    ~LinkClock();
    LinkClock(const LinkClock&) = delete;
    LinkClock& operator=(const LinkClock&) = delete;

    void setEnabled(bool on);
    bool enabled() const;  // Link support on (in follow mode also while only listening)
    double quantum() const { return quantum_; }

    // Session state at Link's clock now + offsetSeconds. Realtime-safe
    // (captureAppSessionState), callable from the render thread.
    LinkSnapshot sample(double offsetSeconds) const;

    // Test peers / tools only: propose a tempo to the session.
    void proposeTempo(double bpm);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    double quantum_;
};

}  // namespace reforce::sync
