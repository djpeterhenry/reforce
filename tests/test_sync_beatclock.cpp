#include <doctest/doctest.h>

#include <cmath>
#include <utility>

#include "sync/BeatClock.hpp"

using reforce::sync::BeatClock;
using reforce::sync::BeatClockConfig;
using reforce::sync::BeatClockInput;
using reforce::sync::BeatSource;

namespace {

// The analyzer's BEAT pulse for onsets at the given times: exp(-age / 0.12) after the latest onset.
struct PulseTrain {
    double bpm = 120, firstBeat = 0.25, jitter = 0;
    bool offbeats = false;      // extra onsets halfway between beats (hats, off-beat bass)
    double beatStrength = 4.0, offStrength = 1.5;
    // latest onset at or before t: (time, strength); time < 0 = none yet
    std::pair<double, double> latest(double t) const {
        const double period = 60.0 / bpm;
        std::pair<double, double> out{-1.0, 0.0};
        for (int k = 0; k < 10000; ++k) {
            const double on = firstBeat + k * period + ((k % 3) - 1) * jitter;
            if (on > t) break;
            out = {on, beatStrength};
            if (offbeats && on + period / 2 <= t) out = {on + period / 2, offStrength};
        }
        return out;
    }
    void feed(double t, BeatClockInput& in) const {
        const auto [on, strength] = latest(t);
        in.audioBeat = on < 0 ? 0.0 : std::exp(-(t - on) / 0.12);
        in.audioStrength = strength;
    }
};

// Distance (beats) of the clock's beat count from the true beat grid at time t.
double phaseError(double clockBeat, double t, const PulseTrain& p) {
    const double trueBeat = (t - p.firstBeat) * p.bpm / 60.0;
    const double d = clockBeat - trueBeat;
    return d - std::round(d);
}

}  // namespace

TEST_CASE("beat clock: audio PLL locks onto a steady kick and holds phase") {
    BeatClock clock;
    PulseTrain p;
    const double dt = 1.0 / 60;
    BeatClockInput in;
    in.audioTempo = 120;
    double lastBeat = -1;
    bool monotonic = true;
    for (int i = 0; i < 60 * 20; ++i) {
        const double t = i * dt;
        p.feed(t, in);
        const auto& s = clock.update(dt, in);
        if (s.beat < lastBeat - 1e-12) monotonic = false;
        lastBeat = s.beat;
    }
    const auto& s = clock.state();
    CHECK(monotonic);
    CHECK(s.source == BeatSource::Audio);
    CHECK(s.confident);
    CHECK(s.bpm == doctest::Approx(120).epsilon(0.002));  // TEMPO plus the steady loop's learnt trim
    // within 1/60 s (one sim step) of the true grid: 0.033 beats at 120 BPM
    CHECK(std::abs(phaseError(s.beat, 20 * 60 * dt - dt, p)) < 0.05);
}

TEST_CASE("beat clock: off-beat onsets and jitter don't pull it off the beat") {
    BeatClock clock;
    PulseTrain p;
    p.bpm = 126;
    p.offbeats = true;
    p.jitter = 0.01;
    const double dt = 1.0 / 60;
    BeatClockInput in;
    in.audioTempo = 126;
    for (int i = 0; i < 60 * 30; ++i) {
        p.feed(i * dt, in);
        clock.update(dt, in);
    }
    CHECK(clock.state().confident);
    CHECK(std::abs(phaseError(clock.state().beat, (60 * 30 - 1) * dt, p)) < 0.08);
}

TEST_CASE("beat clock: equally strong on- and off-beat onsets: not confident (no guessing)") {
    BeatClock clock;
    PulseTrain p;
    p.offbeats = true;
    p.offStrength = p.beatStrength;
    const double dt = 1.0 / 60;
    BeatClockInput in;
    in.audioTempo = 120;
    for (int i = 0; i < 60 * 20; ++i) {
        p.feed(i * dt, in);
        clock.update(dt, in);
    }
    CHECK(clock.state().source == BeatSource::Audio);
    CHECK(!clock.state().confident);
}

TEST_CASE("beat clock: follows a tempo change, freewheels through silence, then gives up") {
    BeatClock clock;
    const double dt = 1.0 / 60;
    BeatClockInput in;
    PulseTrain a;  // 120 for 10 s
    in.audioTempo = 120;
    int i = 0;
    for (; i < 600; ++i) {
        a.feed(i * dt, in);
        clock.update(dt, in);
    }
    // the tracker reports 124 from now on; the kick moves to 124 BPM from t = 10 s (on a beat)
    PulseTrain b;
    b.bpm = 124;
    b.firstBeat = a.firstBeat + 20 * 0.5;  // beat 20 of the 120 grid = 10.25 s
    in.audioTempo = 124;
    for (; i < 600 + 900; ++i) {
        const double t = i * dt;
        if (t < b.firstBeat) a.feed(t, in);
        else b.feed(t, in);
        clock.update(dt, in);
    }
    CHECK(clock.state().confident);
    CHECK(std::abs(phaseError(clock.state().beat - 20, (i - 1) * dt, b)) < 0.08);
    // silence: no pulses, tempo gone -> freewheel for holdSeconds, then no source
    in.audioTempo = 0;
    in.audioBeat = 0;
    const double before = clock.state().beat;
    for (int k = 0; k < 60; ++k) clock.update(dt, in);
    CHECK(clock.state().source == BeatSource::Audio);
    CHECK(clock.state().beat == doctest::Approx(before + 124.0 / 60.0).epsilon(0.02));
    for (int k = 0; k < 60 * 5; ++k) clock.update(dt, in);
    CHECK(clock.state().source == BeatSource::None);
    CHECK(!clock.state().confident);
}

TEST_CASE("beat clock: Link wins in auto; source preference") {
    BeatClockInput in;
    in.linkOn = true;
    in.linkBpm = 128;
    in.linkBeat = 33.5;
    in.audioTempo = 120;
    BeatClock autoClock;
    auto s = autoClock.update(0.016, in);
    CHECK(s.source == BeatSource::Link);
    CHECK(s.confident);
    CHECK(s.bars);
    CHECK(s.beat == 33.5);

    BeatClockConfig cfg;
    cfg.source = "audio";
    BeatClock audioOnly(cfg);
    s = audioOnly.update(0.016, in);
    CHECK(s.source == BeatSource::Audio);
    CHECK(!s.confident);  // not locked yet

    cfg.source = "off";
    BeatClock off(cfg);
    CHECK(off.update(0.016, in).source == BeatSource::None);

    in.linkOn = false;
    cfg.source = "link";
    BeatClock linkOnly(cfg);
    CHECK(linkOnly.update(0.016, in).source == BeatSource::None);
}

TEST_CASE("beat clock: outputOffsetMs shifts the audio count earlier (Beat offset nudge)") {
    BeatClockConfig a, b;
    b.outputOffsetMs = 45;
    BeatClock plain(a), early(b);
    PulseTrain p;
    const double dt = 1.0 / 60;
    BeatClockInput in;
    in.audioTempo = 120;
    for (int i = 0; i < 60 * 12; ++i) {
        p.feed(i * dt, in);
        plain.update(dt, in);
        early.update(dt, in);
    }
    REQUIRE(early.state().confident);
    // 45 ms at the clock's tempo, in beats (the phase loop itself is the same in both).
    CHECK(early.state().beat - plain.state().beat == doctest::Approx(0.045 * early.state().bpm / 60.0).epsilon(1e-6));
}

TEST_CASE("beat clock (steady): a confident beat fold slides an off-beat lock onto the beat") {
    BeatClockConfig cfg;
    cfg.steady = true;
    for (const bool useFold : {false, true}) {
        cfg.useFold = useFold;
        BeatClock clock(cfg);
        PulseTrain p;  // the onsets the clock locks on: off-beat bass (low on-beat probability)
        p.bpm = 124;
        const double dt = 1.0 / 60;
        BeatClockInput in;
        in.audioTempo = 124;
        in.audioBeatness = 0.3;
        for (int i = 0; i < 60 * 20; ++i) {
            const double t = i * dt;
            p.feed(t, in);
            if (t >= 10.0) {  // from 10 s the fold says: the beat is half a beat after those onsets
                const double b = (t + dt - p.firstBeat) * p.bpm / 60.0 + 0.5;
                in.foldPhase = b - std::floor(b);
                in.foldConf = 0.9;
            }
            clock.update(dt, in);
        }
        CHECK(clock.state().confident);
        const double e = phaseError(clock.state().beat, (60 * 20 - 1) * dt, p);
        if (useFold) CHECK(std::abs(std::abs(e) - 0.5) < 0.08);
        else CHECK(std::abs(e) < 0.1);  // low on-beat probability: a looser lock
    }
}
