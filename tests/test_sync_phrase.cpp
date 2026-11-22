// Phrase clock (PLAN 5O.1): phrase position from the beat clock, auto alignment from loudness changes, manual start.
#include <doctest/doctest.h>

#include <cmath>

#include "expr/Rng.hpp"
#include "sync/PhraseClock.hpp"

using namespace reforce::sync;

namespace {

// A track at `bpm` on the Link timeline whose sections (loud / quiet / medium) change every `bars` bars, starting at
// beat `firstChange`. Small per-step noise and a kick on every beat.
struct Track {
    double bpm = 128, firstChange = 24;
    int bars = 16;
    reforce::expr::Pcg32 rng{5};
    double loudness(double beat) {
        const double L = bars * 4.0;
        const long long section = (long long)std::floor((beat - firstChange) / L);
        const double base = (section % 3 == 0) ? 1.0 : (section % 3 == 1) ? 0.3 : 0.6;
        const double kick = std::exp(-8.0 * (beat - std::floor(beat)));
        return base * (0.8 + 0.2 * kick) + 0.05 * rng.uniform01();
    }
};

BeatClockState linkAt(double beat, double bpm = 128) {
    BeatClockState s;
    s.source = BeatSource::Link;
    s.confident = true;
    s.bpm = bpm;
    s.beat = beat;
    s.bars = true;
    s.quantum = 4;
    s.lock = 1;
    return s;
}

// Runs from beat b0 to b1 at 60 steps per second.
void run(PhraseClock& pc, Track& tr, double b0, double b1, BeatSource src = BeatSource::Link) {
    const double perStep = tr.bpm / 60.0 / 60.0;
    for (double b = b0; b < b1; b += perStep) {
        BeatClockState s = linkAt(b, tr.bpm);
        if (src == BeatSource::Audio) {
            s.source = BeatSource::Audio;
            s.bars = false;
        }
        pc.update(s, tr.loudness(b));
    }
}

}  // namespace

TEST_CASE("phrase: position from the beat clock; off without a source") {
    PhraseClock pc;
    BeatClockState none;
    CHECK_FALSE(pc.update(none, 0.5).on);
    const PhraseState& s = pc.update(linkAt(100.0), 0.5);
    CHECK(s.on);
    CHECK(s.bars == 16);
    CHECK(s.how == "none");
    CHECK(s.phrase == doctest::Approx(100.0 / 64.0));
    CHECK(s.bar == doctest::Approx(25.0));
    CHECK(s.barInPhrase == 9);
}

TEST_CASE("phrase: auto alignment finds the section changes, a few phrases in") {
    PhraseClock pc;
    Track tr;  // changes at beat 24 + 64k (bar 6 of Link's phrases)
    run(pc, tr, 0.0, 64.0 * 5);
    const PhraseState& s = pc.state();
    CHECK(s.how == "auto");
    CHECK(s.aligned);
    CHECK(s.origin == doctest::Approx(24.0));
    CHECK(s.confidence > 0.6);
    // A phrase line at every change.
    pc.update(linkAt(24.0 + 64.0 * 6 + 0.01), 1.0);
    CHECK(pc.state().barInPhrase == 0);
    CHECK(pc.state().phrase - std::floor(pc.state().phrase) < 0.01);

    // 8-bar phrases: the same changes every 16 bars land on every other phrase line, still aligned.
    PhraseConfig c = pc.config();
    c.bars = 8;
    pc.setConfig(c);
    run(pc, tr, 64.0 * 6, 64.0 * 11);
    CHECK(pc.state().how == "auto");
    CHECK(std::fmod(pc.state().origin, 32.0) == doctest::Approx(24.0));
}

TEST_CASE("phrase: audio clock (no bar lines) aligns to the beat") {
    PhraseClock pc;
    Track tr;
    tr.firstChange = 13;  // not on a bar of the running count
    run(pc, tr, 0.0, 64.0 * 6, BeatSource::Audio);
    CHECK(pc.state().how == "auto");
    CHECK(pc.state().origin == doctest::Approx(13.0));
}

TEST_CASE("phrase: manual start and nudges; auto keeps a manual origin it agrees with; a new source resets") {
    PhraseClock pc;
    Track tr;
    pc.update(linkAt(41.3), 0.5);
    pc.startNow();  // nearest bar line: 40
    CHECK(pc.state().how == "manual");
    CHECK(pc.state().origin == doctest::Approx(40.0));
    pc.nudge(1);
    CHECK(pc.state().origin == doctest::Approx(44.0));
    pc.nudge(-12);  // wraps within the phrase
    CHECK(pc.state().origin == doctest::Approx(60.0));

    // Without changes in the music nothing overrides the manual origin.
    PhraseClock quiet;
    quiet.update(linkAt(8.2), 0.5);
    quiet.startNow();
    for (double b = 8.2; b < 64 * 6; b += 128.0 / 3600.0) quiet.update(linkAt(b), 0.5);
    CHECK(quiet.state().how == "manual");
    CHECK(quiet.state().origin == doctest::Approx(8.0));

    // Music that keeps changing elsewhere wins over a manual vote in time.
    PhraseClock wrong;
    wrong.update(linkAt(0.0), 0.5);
    wrong.startNow();  // origin 0, but the changes are at 24
    run(wrong, tr, 0.0, 64.0 * 8);
    CHECK(wrong.state().how == "auto");
    CHECK(wrong.state().origin == doctest::Approx(24.0));

    // Auto off: only manual or none.
    PhraseClock off;
    PhraseConfig c;
    c.autoAlign = false;
    off.setConfig(c);
    run(off, tr, 0.0, 64.0 * 5);
    CHECK(off.state().how == "none");

    // A source switch forgets everything.
    BeatClockState audio = linkAt(5.0);
    audio.source = BeatSource::Audio;
    audio.bars = false;
    wrong.update(audio, 0.5);
    CHECK(wrong.state().how == "none");
    CHECK(wrong.state().origin == 0.0);
}
