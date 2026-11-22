// Drop detector (PLAN 5O.5): breakdowns, build, drops from the bass on the beat clock.
#include <doctest/doctest.h>

#include <cmath>

#include "expr/Rng.hpp"
#include "sync/DropDetector.hpp"

using namespace reforce::sync;

namespace {

BeatClockState clockAt(double beat) {
    BeatClockState s;
    s.source = BeatSource::Link;
    s.confident = true;
    s.bpm = 128;
    s.beat = beat;
    s.bars = true;
    return s;
}

// Bass per step: a kick on every beat at `level`, plus noise.
double bassAt(double beat, double level, reforce::expr::Pcg32& rng) {
    const double kick = std::exp(-6.0 * (beat - std::floor(beat)));
    return level * (0.6 + 0.4 * kick) + 0.03 * rng.uniform01();
}

}  // namespace

TEST_CASE("drop: steady music never drops; a breakdown builds, and the bass coming back drops at once") {
    DropDetector d;
    reforce::expr::Pcg32 rng(3);
    const double dt = 1.0 / 60.0, perStep = 128.0 / 60.0 * dt;
    double beat = 0;
    auto run = [&](double beats, double level) {
        int fired = 0;
        const double end = beat + beats;
        for (; beat < end; beat += perStep) {
            d.update(dt, clockAt(beat), bassAt(beat, level, rng));
            fired += d.dropped() ? 1 : 0;
        }
        return fired;
    };
    CHECK(run(64, 1.0) == 0);
    CHECK_FALSE(d.state().breakdown);
    CHECK(d.state().usual == doctest::Approx(0.67).epsilon(0.1));
    // A two-beat dip is not a breakdown.
    CHECK(run(2, 0.1) == 0);
    CHECK(run(16, 1.0) == 0);
    CHECK_FALSE(d.state().breakdown);
    // A breakdown: 16 quiet beats; the build rises.
    CHECK(run(8, 0.1) == 0);
    CHECK(d.state().breakdown);
    const double b1 = d.state().build;
    CHECK(run(8, 0.1) == 0);
    CHECK(d.state().build > b1);
    CHECK(d.state().build < 1.0);
    // The drop: within a beat of the bass coming back.
    const double dropAt = std::ceil(beat);
    run(dropAt - beat, 0.1);
    int fired = 0;
    double firedAt = -1;
    for (; beat < dropAt + 1; beat += perStep) {
        d.update(dt, clockAt(beat), bassAt(beat, 1.0, rng));
        if (d.dropped()) {
            ++fired;
            firedAt = beat;
        }
    }
    CHECK(fired == 1);
    CHECK(firedAt - dropAt < 1.0);  // the half-beat average needs most of a beat to rise
    CHECK(d.state().drops == 1);
    CHECK(d.state().drop > 0.5);
    CHECK_FALSE(d.state().breakdown);
    // The pulse dies away over about a bar.
    run(8, 1.0);
    CHECK(d.state().drop < 0.2);
    // Silence (the end of the track) breaks down but never drops by itself.
    CHECK(run(32, 0.0) == 0);
    CHECK(d.state().breakdown);
}

TEST_CASE("drop: works without a beat clock (120 BPM)") {
    DropDetector d;
    reforce::expr::Pcg32 rng(4);
    BeatClockState none;
    const double dt = 1.0 / 60.0;
    int fired = 0;
    double t = 0;
    auto run = [&](double seconds, double level) {
        for (const double end = t + seconds; t < end; t += dt) {
            d.update(dt, none, bassAt(t * 2.0, level, rng));
            fired += d.dropped() ? 1 : 0;
        }
    };
    run(30, 1.0);
    run(8, 0.05);
    CHECK(d.state().breakdown);
    run(2, 1.0);
    CHECK(fired == 1);
}
