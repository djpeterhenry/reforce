#pragma once
// reforce extension (PLAN 5O.8): phrase lines from how Peter plays his library.
//
// Peter starts almost every song (hot cue 1 and others) on the first beat of a phrase of at least 8 bars. So the
// moment a song becomes audible, its last start point is a phrase line, and it stays one until the next song comes
// in. Inputs:
//   rekordbox, through rkbx_link's "reforce" output (sync/DeckFeed): each deck's position in the track, its running
//     beat on the track's beatgrid, and its tempo, ~120 times a second.
//   the DJ controller's MIDI (sync/DeckFeed, mapped by sync/ControllerMap): channel faders, the crossfader and its assign switches, hot cue pads.
//
// Per deck:
//   start     the deck begins to play from standing still (Play, Cue, a hot cue): the start point is the beat where
//             the playback run began, rounded to the grid. Also a jump while playing that follows a hot cue pad
//             press on that deck (a hot cue mid-song); other jumps (loops, beat jumps) are not starts.
//   elapsed   beats since the start point, counted as play time x tempo, so loops and beat jumps after the start
//             do not change it (the phrase keeps going through them).
//   audible   channel fader >= faderUp (and the crossfader does not cut the deck). A fader never seen yet (a controller
//             sends a fader only when it moves) counts as down (Peter 2026-11-18).
// A start becomes the phrase line (a "commit") when it happens on an audible deck, or later when that deck becomes
// audible while still playing from it. A start made with the fader down and replaced by another start before the
// fader comes up is never used. Each start commits at most once.
//
// Pure and single-threaded (the render thread); times are seconds. `now` is the local receive time (MIDI and
// rkbx frames on one clock); frame intervals use the sender's own clock.

#include <array>
#include <string>
#include <vector>

namespace reforce::sync {

struct DeckFrame {
    int deck = 0;              // 0..3 (rekordbox deck 1..4 = DDJ-1000 mixer channel 1..4)
    double position = 0;       // seconds into the track
    double gridBeat = 0;       // running beat on the beatgrid (0 = the first grid beat); NaN without a grid
    double bpm = 0;            // current tempo (pitched)
    double originalBpm = 0;    // the grid's tempo here
};

struct DeckStartsConfig {
    double faderUp = 1.0 / 3.0;   // reforce.Phrase.FaderUp: channel fader position that makes a deck audible
    double hotCueWindow = 0.5;    // s between a hot cue press and the jump it causes
    double playConfirm = 0.15;    // s of steady playback before a start counts (not a jog nudge)
    double stopSeconds = 0.12;    // position unchanged this long: stopped (rekordbox updates in audio buffers)
    double stillTol = 0.0005;     // s of position: not moving
    double jumpTol = 0.06;        // s off the expected advance between two frames: a jump
    double crossfaderCut = 0.05;  // the crossfader within this of the far end cuts a deck assigned to the other side
};

struct DeckCommit {
    int deck = 0;
    double elapsed = 0;   // beats since the phrase line, as of local time `at`
    double at = 0;
    double bpm = 0;       // to carry `elapsed` on from `at`
    std::string why;      // "start" | "hot cue" | "fader"
};

struct DeckView {
    bool seen = false;       // frames with a beatgrid
    bool playing = false;
    bool audible = false;
    double fader = -1;       // 0..1, < 0: not seen yet
    bool hasStart = false;   // a start point since it began playing
    bool committed = false;  // ... and it is (was) the phrase line
    double elapsed = 0;      // beats since the start point
};

class DeckStarts {
public:
    static constexpr int kDecks = 4;
    explicit DeckStarts(DeckStartsConfig cfg = {}) : cfg_(cfg) {}
    void setConfig(const DeckStartsConfig& cfg) { cfg_ = cfg; }
    const DeckStartsConfig& config() const { return cfg_; }

    // One deck of an rkbx datagram: `senderT` is the datagram's own time stamp.
    void frame(double now, double senderT, const DeckFrame& f);
    void fader(double now, int deck, double value);   // 0..1
    void crossfader(double now, double value);        // 0 = side A, 1 = side B
    void assign(double now, int deck, int side);      // -1 A, 0 thru, +1 B
    void hotCue(double now, int deck);                // a hot cue pad pressed (any of the 8)

    std::vector<DeckCommit> take();                   // commits since the last call, oldest first
    const DeckView& deck(int i) const { return view_[std::size_t(clampDeck(i))]; }
    int lastCommitDeck() const { return lastCommit_; }
    // How loud the deck is in the mix, 0..1: 0 unless playing; the channel fader (0 until seen) times the
    // crossfader's gain for the deck's side (linear to the middle, full beyond it; thru = 1). For now playing.
    double level(int i) const;
    bool fadersSeen() const;                          // any channel fader seen yet (the controller's MIDI is there)
    std::string summary() const;                      // "1 ▶ 2 ■ ..." for the panel / log

private:
    enum class Mode { Unknown, Stopped, Playing };
    struct Deck {
        bool have = false;
        double lastT = 0, lastNow = 0, lastPos = 0, lastBeat = 0, bpm = 0, rate = 1;
        Mode mode = Mode::Unknown;
        double lastMoveT = 0;                         // sender time the position last moved
        double runT = 0, runPos = 0, runBeat = 0;     // the current jump-free playback run began here
        bool hasStart = false, committed = false;
        double elapsed = 0;
        double lastPad = -1e9;
        bool pendingJump = false;                     // a jump while playing, waiting for its hot cue press
        double pendingNow = 0, pendingElapsed = 0;
        double fader = -1;
        int side = 0;
    };
    static int clampDeck(int i) { return i < 0 ? 0 : i >= kDecks ? kDecks - 1 : i; }
    bool audible(const Deck& d) const;
    void newStart(int i, double elapsed, const char* why);
    void commit(int i, double now, const char* why);
    void refresh(int i);

    DeckStartsConfig cfg_;
    std::array<Deck, kDecks> d_{};
    std::array<DeckView, kDecks> view_{};
    double crossfader_ = -1;
    std::vector<DeckCommit> out_;
    int lastCommit_ = -1;
};

}  // namespace reforce::sync
