#include "sync/DeckStarts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace reforce::sync {

double DeckStarts::level(int i) const {
    const Deck& d = d_[std::size_t(clampDeck(i))];
    if (d.mode != Mode::Playing || d.fader <= 0) return 0;
    double g = d.fader;
    if (crossfader_ >= 0 && d.side != 0) g *= std::clamp(2.0 * (d.side < 0 ? 1.0 - crossfader_ : crossfader_), 0.0, 1.0);
    return g;
}

bool DeckStarts::fadersSeen() const {
    for (const Deck& d : d_)
        if (d.fader >= 0) return true;
    return false;
}

bool DeckStarts::audible(const Deck& d) const {
    if (d.fader < cfg_.faderUp) return false;  // includes a fader not seen yet (< 0): down
    if (crossfader_ >= 0) {
        if (d.side < 0 && crossfader_ >= 1.0 - cfg_.crossfaderCut) return false;
        if (d.side > 0 && crossfader_ <= cfg_.crossfaderCut) return false;
    }
    return true;
}

void DeckStarts::commit(int i, double now, const char* why) {
    Deck& d = d_[std::size_t(i)];
    DeckCommit c;
    c.deck = i;
    c.bpm = d.bpm;
    // As of the last frame; a commit from MIDI (fader) between frames is carried on by the caller from `at`.
    c.elapsed = d.elapsed;
    c.at = d.lastNow;
    c.why = why;
    (void)now;
    out_.push_back(c);
    d.committed = true;
    lastCommit_ = i;
}

void DeckStarts::newStart(int i, double elapsed, const char* why) {
    Deck& d = d_[std::size_t(i)];
    d.hasStart = true;
    d.committed = false;
    d.elapsed = elapsed;
    d.pendingJump = false;
    if (audible(d)) commit(i, d.lastNow, why);
}

void DeckStarts::frame(double now, double T, const DeckFrame& f) {
    if (f.deck < 0 || f.deck >= kDecks) return;
    const int i = f.deck;
    Deck& d = d_[std::size_t(i)];
    if (!std::isfinite(f.gridBeat) || !std::isfinite(f.position) || !std::isfinite(T)) {
        // No beatgrid (nothing loaded, or not analyzed): nothing to place a start on.
        const double fader = d.fader, pad = d.lastPad;
        const int side = d.side;
        d = Deck{};
        d.fader = fader;
        d.side = side;
        d.lastPad = pad;
        refresh(i);
        return;
    }
    const double rate = f.bpm > 0 && f.originalBpm > 0 ? f.bpm / f.originalBpm : 1.0;
    auto restartRun = [&] {
        d.runT = T;
        d.runPos = f.position;
        d.runBeat = f.gridBeat;
    };
    auto store = [&] {
        d.lastT = T;
        d.lastNow = now;
        d.lastPos = f.position;
        d.lastBeat = f.gridBeat;
        d.bpm = f.bpm;
        d.rate = rate;
    };
    if (!d.have || T < d.lastT || T - d.lastT > 0.5) {
        // First frame, the sender restarted, or a gap: we do not know what happened in between.
        d.have = true;
        d.mode = Mode::Unknown;
        d.hasStart = d.committed = d.pendingJump = false;
        d.lastMoveT = T;
        restartRun();
        store();
        refresh(i);
        return;
    }
    const double dt = T - d.lastT;
    if (dt <= 0) return;  // duplicate
    const double dp = f.position - d.lastPos;
    const bool moved = std::abs(dp) > cfg_.stillTol;
    const double sinceMove = T - d.lastMoveT;  // before this frame
    if (moved) d.lastMoveT = T;

    if (d.mode == Mode::Playing) {
        const double beats = dt * (f.bpm > 0 ? f.bpm : d.bpm) / 60.0;
        if (d.hasStart) d.elapsed += beats;
        if (d.pendingJump) {
            d.pendingElapsed += beats;
            if (now - d.pendingNow > cfg_.hotCueWindow) d.pendingJump = false;
        }
        if (std::abs(dp - dt * rate) > cfg_.jumpTol) {
            // A jump while playing: a hot cue (a start) or a loop / beat jump (not one).
            restartRun();
            const double frac = f.gridBeat - std::round(f.gridBeat);
            store();
            if (now - d.lastPad <= cfg_.hotCueWindow) {
                d.lastPad = -1e9;
                newStart(i, frac, "hot cue");
            } else {
                d.pendingJump = true;
                d.pendingNow = now;
                d.pendingElapsed = frac;
            }
            refresh(i);
            return;
        }
        if (T - d.lastMoveT >= cfg_.stopSeconds) {
            d.mode = Mode::Stopped;
            d.hasStart = d.committed = d.pendingJump = false;
            restartRun();
        }
        store();
        refresh(i);
        return;
    }

    // Stopped or unknown: watch for a playback run long and steady enough to be a start.
    if (std::abs(dp) > cfg_.jumpTol) {
        restartRun();  // a jump (hot cue, load, search): playback would start from here
    } else if (!moved && sinceMove >= 0.06) {
        restartRun();  // at rest (longer than an audio buffer): the run starts at the latest still frame
    }
    const double span = T - d.runT;
    const double adv = f.position - d.runPos;
    const double want = span * rate;
    if (moved && span >= cfg_.playConfirm && std::abs(adv - want) <= 0.25 * want + 0.03) {
        const bool fromStop = d.mode == Mode::Stopped;
        d.mode = Mode::Playing;
        store();
        if (fromStop) newStart(i, f.gridBeat - std::round(d.runBeat), "start");
        refresh(i);
        return;
    }
    if (d.mode == Mode::Unknown && !moved && sinceMove + dt >= cfg_.stopSeconds) d.mode = Mode::Stopped;
    store();
    refresh(i);
}

void DeckStarts::fader(double now, int deck, double value) {
    if (deck < 0 || deck >= kDecks || !std::isfinite(value)) return;
    Deck& d = d_[std::size_t(deck)];
    const bool before = audible(d);
    d.fader = std::fmax(0.0, std::fmin(1.0, value));
    const bool after = audible(d);
    if (!before && after && d.mode == Mode::Playing && d.hasStart && !d.committed) commit(deck, now, "fader");
    refresh(deck);
}

void DeckStarts::crossfader(double now, double value) {
    if (!std::isfinite(value)) return;
    std::array<bool, kDecks> before{};
    for (int i = 0; i < kDecks; ++i) before[std::size_t(i)] = audible(d_[std::size_t(i)]);
    crossfader_ = std::fmax(0.0, std::fmin(1.0, value));
    for (int i = 0; i < kDecks; ++i) {
        Deck& d = d_[std::size_t(i)];
        if (!before[std::size_t(i)] && audible(d) && d.mode == Mode::Playing && d.hasStart && !d.committed)
            commit(i, now, "fader");
        refresh(i);
    }
}

void DeckStarts::assign(double now, int deck, int side) {
    if (deck < 0 || deck >= kDecks) return;
    Deck& d = d_[std::size_t(deck)];
    const bool before = audible(d);
    d.side = side < 0 ? -1 : side > 0 ? 1 : 0;
    if (!before && audible(d) && d.mode == Mode::Playing && d.hasStart && !d.committed) commit(deck, now, "fader");
    refresh(deck);
}

void DeckStarts::hotCue(double now, int deck) {
    if (deck < 0 || deck >= kDecks) return;
    Deck& d = d_[std::size_t(deck)];
    d.lastPad = now;
    if (d.pendingJump && now - d.pendingNow <= cfg_.hotCueWindow) {
        // The jump came in first (the pad's MIDI normally arrives before rekordbox moves).
        d.lastPad = -1e9;
        newStart(deck, d.pendingElapsed, "hot cue");
    }
    refresh(deck);
}

std::vector<DeckCommit> DeckStarts::take() {
    std::vector<DeckCommit> r;
    r.swap(out_);
    return r;
}

void DeckStarts::refresh(int i) {
    const Deck& d = d_[std::size_t(i)];
    DeckView& v = view_[std::size_t(i)];
    v.seen = d.have;
    v.playing = d.mode == Mode::Playing;
    v.audible = audible(d);
    v.fader = d.fader;
    v.hasStart = d.hasStart;
    v.committed = d.committed;
    v.elapsed = d.elapsed;
}

std::string DeckStarts::summary() const {
    std::string s;
    for (int i = 0; i < kDecks; ++i) {
        const DeckView& v = view_[std::size_t(i)];
        if (!v.seen) continue;
        char b[96];
        std::snprintf(b, sizeof b, "%s%d %s%s%s", s.empty() ? "" : " · ", i + 1, v.playing ? "▶" : "■",
                      v.audible ? " up" : v.fader < 0 ? " down?" : " down",
                      v.hasStart ? (i == lastCommit_ && v.committed ? " ★" : v.committed ? " ✓" : " (start)") : "");
        s += b;
    }
    return s;
}

}  // namespace reforce::sync
