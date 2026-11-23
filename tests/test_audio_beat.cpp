#include <doctest/doctest.h>

#include "audio/Analyzer.hpp"
#include "audio/BeatTracker.hpp"
#include "sync/BeatClock.hpp"

#include <cmath>
#include <vector>

using namespace reforce::audio;

namespace {

constexpr double kSr = 44100.0;

// The probes/audio/kick120.wav kick (tools/audio_signals/gen_signals.py kick()): a sine gliding
// 150 -> 55 Hz (tau 30 ms), amplitude decay tau 150 ms, 2 ms attack, 0.45 s long, -3 dBFS peak.
std::vector<float> kickHit() {
    std::vector<float> out(size_t(0.45 * kSr));
    const double a = std::pow(10.0, -3.0 / 20.0);
    double ph = 0.0;
    for (size_t i = 0; i < out.size(); ++i) {
        const double t = double(i) / kSr;
        ph += 2.0 * M_PI * (55.0 + 95.0 * std::exp(-t / 0.03)) / kSr;
        out[i] = float(a * std::exp(-t / 0.15) * std::min(1.0, t / 0.002) * std::sin(ph));
    }
    return out;
}

// `seconds` of audio: silence until `start`, then one kick every 60/bpm s.
std::vector<float> kickTrack(double seconds, double start, double bpm) {
    std::vector<float> x(size_t(seconds * kSr), 0.f);
    const std::vector<float> hit = kickHit();
    for (double t = start; t < seconds; t += 60.0 / bpm) {
        const size_t i0 = size_t(std::lround(t * kSr));
        for (size_t i = 0; i < hit.size() && i0 + i < x.size(); ++i) x[i0 + i] += hit[i];
    }
    return x;
}

// A house-style arrangement on one 124 BPM grid (beat k at start + k * 60 / bpm):
//   [0, kickEnd): four-on-the-floor kick; [kickEnd, breakEnd): breakdown, no kick: chord stabs on random
//   sixteenths (35 %) and short noise hits on random eighths (fixed seed); [breakEnd, seconds): the kick again
//   (bpm2 > 0: at that tempo, from breakEnd).
std::vector<float> houseBreakdown(double seconds, double bpm, double kickEnd, double breakEnd, double bpm2 = 0.0) {
    std::vector<float> x(size_t(seconds * kSr), 0.f);
    const std::vector<float> hit = kickHit();
    auto add = [&](double t, const std::vector<float>& v) {
        const size_t i0 = size_t(std::lround(t * kSr));
        for (size_t i = 0; i < v.size() && i0 + i < x.size(); ++i) x[i0 + i] += v[i];
    };
    std::vector<float> stab(size_t(0.25 * kSr)), noise(size_t(0.04 * kSr));
    uint32_t rng = 12345u;
    auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return double(rng >> 8) / double(1u << 24); };
    for (size_t i = 0; i < stab.size(); ++i) {
        const double t = double(i) / kSr;
        double v = 0.0;
        for (const double f : {262.0, 330.0, 392.0, 523.0}) v += std::sin(2.0 * M_PI * f * t);
        stab[i] = float(0.12 * v * std::exp(-t / 0.08) * std::min(1.0, t / 0.003));
    }
    for (size_t i = 0; i < noise.size(); ++i)
        noise[i] = float(0.25 * (2.0 * rnd() - 1.0) * std::exp(-double(i) / kSr / 0.01));
    const double beat = 60.0 / bpm;
    for (double t = 0.0; t < kickEnd; t += beat) add(t, hit);
    for (double t = kickEnd; t < breakEnd; t += beat / 4.0) {
        if (rnd() < 0.35) add(t, stab);
        if (std::fmod(t - kickEnd, beat / 2.0) < 1e-6 && rnd() < 0.5) add(t, noise);
    }
    // Same tempo: the kick comes back on the same grid; a new tempo starts its grid at breakEnd.
    const double beat2 = 60.0 / (bpm2 > 0.0 ? bpm2 : bpm);
    const double back = bpm2 > 0.0 ? breakEnd : std::ceil(breakEnd / beat - 1e-9) * beat;
    for (double t = back; t < seconds; t += beat2) add(t, hit);
    return x;
}

// Like houseBreakdown, but the breakdown is a steady dotted-eighth arpeggio (one note every 3/4 beat, a 4-note
// cycle; reads as 4/3 x the tempo, folded to 2/3: 124 -> ~83) plus a soft pad, with the kick back on the grid.
std::vector<float> dottedBreakdown(double seconds, double bpm, double kickEnd, double breakEnd) {
    std::vector<float> x(size_t(seconds * kSr), 0.f);
    const std::vector<float> hit = kickHit();
    auto add = [&](double t, const std::vector<float>& v) {
        const size_t i0 = size_t(std::lround(t * kSr));
        for (size_t i = 0; i < v.size() && i0 + i < x.size(); ++i) x[i0 + i] += v[i];
    };
    std::vector<std::vector<float>> notes;
    for (const double f : {220.0, 277.2, 329.6, 440.0}) {
        std::vector<float> n(size_t(0.3 * kSr));
        for (size_t i = 0; i < n.size(); ++i) {
            const double t = double(i) / kSr;
            n[i] = float(0.3 * (std::sin(2.0 * M_PI * f * t) + 0.4 * std::sin(4.0 * M_PI * f * t)) *
                         std::exp(-t / 0.12) * std::min(1.0, t / 0.004));
        }
        notes.push_back(std::move(n));
    }
    const double beat = 60.0 / bpm;
    for (double t = 0.0; t < kickEnd; t += beat) add(t, hit);
    int k = 0;
    for (double t = kickEnd; t < breakEnd; t += 0.75 * beat) add(t, notes[size_t(k++ % 4)]);
    for (size_t i = size_t(kickEnd * kSr); i < size_t(breakEnd * kSr) && i < x.size(); ++i)
        x[i] += float(0.05 * std::sin(2.0 * M_PI * 110.0 * double(i) / kSr));  // pad
    for (double t = std::ceil(breakEnd / beat - 1e-9) * beat; t < seconds; t += beat) add(t, hit);
    return x;
}

// Tempo every 0.25 s while the audio is pushed in 60 Hz blocks: {time, tempo}.
std::vector<std::pair<double, double>> tempoTrace(BeatTracker& bt, const std::vector<float>& x) {
    std::vector<std::pair<double, double>> out;
    double next = 0.25;
    for (size_t i = 0; i < x.size(); i += 735) {
        bt.push(x.data() + i, std::min<size_t>(735, x.size() - i));
        if (bt.streamSeconds() >= next) {
            out.emplace_back(bt.streamSeconds(), bt.tempo());
            next += 0.25;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("BeatTracker: kick120 -> one onset per kick, aligned, TEMPO ~120") {
    BeatTracker bt({}, kSr);
    const double start = 2.0;
    const std::vector<float> x = kickTrack(18.0, start, 120.0);
    std::vector<double> onsets;
    uint64_t seen = 0;
    const size_t block = 735;  // one 60 Hz frame
    for (size_t i = 0; i < x.size(); i += block) {
        bt.push(x.data() + i, std::min(block, x.size() - i));
        if (bt.onsetCount() != seen) {
            seen = bt.onsetCount();
            onsets.push_back(bt.lastOnsetSeconds());
        }
    }
    // 32 kicks between 2 s and 18 s.
    CHECK(onsets.size() == 32);
    for (size_t k = 0; k < onsets.size(); ++k) {
        const double expect = start + 0.5 * double(k);
        CHECK(std::abs(onsets[k] - expect) < 0.03);
    }
    CHECK(bt.tempo() == doctest::Approx(120.0).epsilon(0.02));
    CHECK(bt.confidence() > 0.5);
}

TEST_CASE("BeatTracker: BEAT pulse is 1 at the kick and decays") {
    BeatTracker bt({}, kSr);
    const std::vector<float> x = kickTrack(4.0, 1.0, 120.0);
    const size_t at = size_t(1.05 * kSr);  // 50 ms after the first kick
    bt.push(x.data(), at);
    CHECK(bt.beat() > 0.6);
    bt.push(x.data() + at, size_t(0.4 * kSr));  // 0.45 s after the kick, just before the next
    CHECK(bt.beat() < 0.05);
}

TEST_CASE("BeatTracker: silence and a steady tone give no beats and TEMPO 0") {
    BeatTracker bt({}, kSr);
    std::vector<float> x(size_t(10 * kSr), 0.f);
    bt.push(x.data(), x.size());
    CHECK(bt.onsetCount() == 0);
    CHECK(bt.tempo() == 0.0);
    for (size_t i = 0; i < x.size(); ++i) x[i] = float(0.5 * std::sin(2.0 * M_PI * 440.0 * double(i) / kSr));
    BeatTracker b2({}, kSr);
    b2.push(x.data(), x.size());
    CHECK(b2.onsetCount() <= 1);  // the tone's start may count once
    CHECK(b2.tempo() == 0.0);
}

TEST_CASE("BeatTracker: other tempi (90, 140 BPM)") {
    for (double bpm : {90.0, 140.0}) {
        BeatTracker bt({}, kSr);
        const std::vector<float> x = kickTrack(16.0, 1.0, bpm);
        bt.push(x.data(), x.size());
        CHECK(bt.tempo() == doctest::Approx(bpm).epsilon(0.03));
    }
}

TEST_CASE("BeatTracker: TEMPO is folded into 80-160 BPM") {
    // {kick tempo, expected TEMPO}: half time doubles, fast tempi halve, 80-160 stays.
    const double cases[][2] = {{62.0, 124.0}, {174.0, 87.0}, {165.0, 82.5}, {150.0, 150.0}, {84.0, 84.0}};
    for (const auto& c : cases) {
        CAPTURE(c[0]);
        BeatTracker bt({}, kSr);
        const std::vector<float> x = kickTrack(16.0, 1.0, c[0]);
        bt.push(x.data(), x.size());
        CHECK(bt.tempo() == doctest::Approx(c[1]).epsilon(0.03));
    }
    BeatConfig off;
    off.foldMinBpm = 0.0;  // no folding: the raw pick (60-200), an octave of the kick (a bare kick is either)
    BeatTracker bt(off, kSr);
    const std::vector<float> x = kickTrack(16.0, 1.0, 62.0);
    bt.push(x.data(), x.size());
    const double oct = std::log2(bt.tempo() / 62.0);
    CHECK(std::abs(oct - std::round(oct)) < 0.04);
}

TEST_CASE("BeatTracker: TEMPO falls back to 0 after the music stops") {
    BeatTracker bt({}, kSr);
    std::vector<float> x = kickTrack(12.0, 1.0, 120.0);
    bt.push(x.data(), x.size());
    CHECK(bt.tempo() > 100.0);
    std::vector<float> z(size_t(14 * kSr), 0.f);
    bt.push(z.data(), z.size());
    CHECK(bt.tempo() == 0.0);
    CHECK(bt.beat() == 0.0);
}

TEST_CASE("Analyzer: BEAT / TEMPO / MID / TREBLE on kick120") {
    Analyzer an({}, kSr);
    const std::vector<float> x = kickTrack(14.0, 2.0, 120.0);
    const size_t block = 735;
    double maxBeat = 0.0, maxMid = 0.0, maxTreble = 0.0;
    double beatAtKick = 0.0, beatBeforeKick = 1.0;
    for (size_t i = 0; i + block <= x.size(); i += block) {
        an.pushSamples(x.data() + i, block);
        const AudioFrame& f = an.analyze(double(block) / kSr);
        const double t = double(i + block) / kSr;
        if (t > 10.0) {
            maxBeat = std::max(maxBeat, double(f.beat));
            maxMid = std::max(maxMid, double(f.mid));
            maxTreble = std::max(maxTreble, double(f.treble));
            const double phase = std::fmod(t - 2.0, 0.5);
            if (phase < 0.04) beatAtKick = std::max(beatAtKick, double(f.beat));
            if (phase > 0.45) beatBeforeKick = std::min(beatBeforeKick, double(f.beat));
        }
    }
    CHECK(maxBeat > 0.8);
    CHECK(beatAtKick > 0.8);
    CHECK(beatBeforeKick < 0.05);
    CHECK(an.frame().tempo == doctest::Approx(120.0).epsilon(0.02));
    // A kick is all bass: TREBLE stays near 0, MID small.
    CHECK(maxTreble < 0.3);
}

TEST_CASE("BeatTracker: steady tempo holds through a breakdown") {
    // 16 s of kick at 124, a 32 s breakdown (random stabs and noise, no kick), the kick again.
    const std::vector<float> x = houseBreakdown(64.0, 124.0, 16.0, 48.0);
    for (bool steady : {false, true}) {
        CAPTURE(steady);
        BeatConfig c;
        c.steady = steady;
        BeatTracker bt(c, kSr);
        double lo = 1e9, hi = 0;
        for (const auto& [t, bpm] : tempoTrace(bt, x))
            if (t >= 10.0) {
                lo = std::min(lo, bpm);
                hi = std::max(hi, bpm);
            }
        if (steady) {
            CHECK(lo > 124.0 * 0.98);
            CHECK(hi < 124.0 * 1.02);
        } else {
            CHECK(lo < 124.0 * 0.9);  // the old behavior jumps (to ~82 and ~99 here): what Peter saw
        }
    }
}

TEST_CASE("BeatTracker: steady tempo follows real tempo changes") {
    struct Case { double to, readBy; };  // seconds after the change by which TEMPO reads `to` (+-1.5 %)
    for (const Case c : {Case{128.0, 8.0}, Case{100.0, 14.0}}) {
        CAPTURE(c.to);
        const std::vector<float> x = houseBreakdown(60.0, 124.0, 30.0, 30.0, c.to);  // change at 30 s
        BeatTracker bt({}, kSr);
        double firstOk = -1.0;
        bool stayed = true;
        for (const auto& [t, bpm] : tempoTrace(bt, x)) {
            const bool ok = std::abs(bpm / c.to - 1.0) < 0.015;
            if (t > 30.0 && ok && firstOk < 0) firstOk = t;
            if (firstOk >= 0 && !ok) stayed = false;
            if (c.to == 100.0 && t > 12.0 && t < 34.0) CHECK(std::abs(bpm / 124.0 - 1.0) < 0.02);  // no early switch
        }
        CHECK(firstOk > 30.0);
        CHECK(firstOk - 30.0 <= c.readBy);
        CHECK(stayed);
        MESSAGE(c.to << " BPM read after " << firstOk - 30.0 << " s");
    }
}

TEST_CASE("BeatTracker: steady tempo drops after silence, holds without a beat") {
    BeatTracker bt({}, kSr);
    std::vector<float> x = kickTrack(12.0, 1.0, 120.0);
    bt.push(x.data(), x.size());
    REQUIRE(bt.tempo() == doctest::Approx(120.0).epsilon(0.02));
    // A steady pad (sound, no onsets) for 30 s: the tempo stays.
    std::vector<float> pad(size_t(30 * kSr));
    for (size_t i = 0; i < pad.size(); ++i) pad[i] = float(0.1 * std::sin(2.0 * M_PI * 220.0 * double(i) / kSr));
    bt.push(pad.data(), pad.size());
    CHECK(bt.tempo() == doctest::Approx(120.0).epsilon(0.02));
    // Silence: gone within a few seconds.
    std::vector<float> z(size_t(5 * kSr), 0.f);
    bt.push(z.data(), z.size());
    CHECK(bt.tempo() == 0.0);
}

TEST_CASE("BeatClock + BeatTracker: the beat lock survives a breakdown, the drop lands on the beat") {
    // Kick at 124 on the grid t = k * 60/124 until 40 s, breakdown to 72 s, kick again on the same grid.
    const double bpm = 124.0, spb = 60.0 / bpm;
    const std::vector<float> x = houseBreakdown(92.0, bpm, 40.0, 72.0);
    for (bool steady : {false, true}) {
        CAPTURE(steady);
        BeatConfig bc;
        bc.steady = steady;
        BeatTracker bt(bc, kSr);
        reforce::sync::BeatClockConfig cc;
        cc.steady = steady;
        cc.source = "audio";
        reforce::sync::BeatClock clock(cc);
        const size_t block = 735;
        const double dt = double(block) / kSr;
        bool lostInBreakdown = false;
        double worstDrop = 0.0;   // largest beat-phase error of the clock after the drop (from 76 s)
        double worstBreak = 0.0;  // ... during the breakdown
        double offsetBeats = 0.0;  // clock count minus grid count, measured before the breakdown
        bool haveOffset = false;
        for (size_t i = 0; i < x.size(); i += block) {
            bt.push(x.data() + i, std::min(block, x.size() - i));
            reforce::sync::BeatClockInput in;
            in.audioTempo = bt.tempo();
            in.audioBeat = bt.beat();
            in.audioStrength = bt.lastOnsetStrength();
            in.audioLowShare = bt.lastOnsetLowShare();
            in.audioKick = bt.kickPresent();
            in.audioKickness = bt.lastOnsetKickness();
            in.audioBeatness = bt.lastOnsetBeatness();
            in.kickPhase = bt.kickPhase();
            in.kickPhaseLock = bt.kickPhaseLock();
            in.foldPhase = bt.foldPhase();
            in.foldConf = bt.foldConfidence();
            in.filterPhase = bt.filterPhase();
            in.filterConf = bt.filterConfidence();
            const auto& st = clock.update(dt, in);
            const double t = bt.streamSeconds();
            // Phase of the clock against the music's grid (offset fixed at 36 s, while locked on the kick).
            const double grid = t / spb;
            if (t >= 36.0 && !haveOffset && st.confident) {
                offsetBeats = st.beat - grid;
                haveOffset = true;
            }
            if (t >= 44.0 && t < 72.0 && !st.confident) lostInBreakdown = true;
            if (t >= 40.0 && t < 72.0 && haveOffset) {
                const double e = st.beat - grid - offsetBeats;
                worstBreak = std::max(worstBreak, std::abs(e - std::round(e)));
            }
            if (t >= 76.0 && haveOffset) {
                const double e = st.beat - grid - offsetBeats;
                worstDrop = std::max(worstDrop, std::abs(e - std::round(e)));
            }
        }
        MESSAGE("steady " << steady << ": lost lock in breakdown " << lostInBreakdown << ", phase error in it "
                          << worstBreak << ", after the drop " << worstDrop << " beat");
        if (steady) {
            CHECK(haveOffset);
            CHECK(!lostInBreakdown);
            CHECK(worstBreak < 0.06);
            CHECK(worstDrop < 0.06);
        } else {
            CHECK(lostInBreakdown);  // the old behavior
        }
    }
}


TEST_CASE("BeatClock + BeatTracker (steady): a kick back on a shifted grid is re-acquired") {
    // The kick returns at 48 s exactly, 0.2 beat off the old 124 BPM grid (a new track, not beat-matched).
    const double bpm = 124.0, spb = 60.0 / bpm;
    const std::vector<float> x = houseBreakdown(76.0, bpm, 16.0, 48.0, bpm);
    BeatTracker bt({}, kSr);
    reforce::sync::BeatClockConfig cc;
    cc.source = "audio";
    reforce::sync::BeatClock clock(cc);
    const size_t block = 735;
    double offsetOld = 0.0, worst = 0.0;
    bool haveOld = false;
    for (size_t i = 0; i < x.size(); i += block) {
        bt.push(x.data() + i, std::min(block, x.size() - i));
        reforce::sync::BeatClockInput in;
        in.audioTempo = bt.tempo();
        in.audioBeat = bt.beat();
        in.audioStrength = bt.lastOnsetStrength();
        in.audioLowShare = bt.lastOnsetLowShare();
        in.audioKick = bt.kickPresent();
        in.audioKickness = bt.lastOnsetKickness();
        in.audioBeatness = bt.lastOnsetBeatness();
        in.kickPhase = bt.kickPhase();
        in.kickPhaseLock = bt.kickPhaseLock();
        in.foldPhase = bt.foldPhase();
        in.foldConf = bt.foldConfidence();
        in.filterPhase = bt.filterPhase();
        in.filterConf = bt.filterConfidence();
        const auto& st = clock.update(double(block) / kSr, in);
        const double t = bt.streamSeconds();
        if (t >= 14.0 && !haveOld && st.confident) {
            offsetOld = st.beat - t / spb;
            haveOld = true;
        }
        if (t >= 62.0) {  // against the new grid: kicks at 48 + k * spb
            // Locked, the clock sits at the same offset from the kicks as it did on the old grid.
            const double e = st.beat - (t - 48.0) / spb - offsetOld;
            worst = std::max(worst, std::abs(e - std::round(e)));
            CHECK(st.confident);
        }
    }
    REQUIRE(haveOld);
    // The old grid's phase at the new kicks is 48/spb mod 1 ~ 0.2 beat off; the clock must have moved to the new one.
    const double shift = 48.0 / spb - std::floor(48.0 / spb);
    MESSAGE("grid shift " << shift << " beat; clock phase error vs the new grid from 62 s: " << worst);
    CHECK(worst < 0.06);
}

TEST_CASE("BeatTracker (steady): a long breakdown with a dotted-eighth arpeggio keeps the tempo") {
    // 20 s kick at 125, 90 s breakdown (arpeggio every 3/4 beat: a strong ~83 / ~167 BPM periodicity), kick again.
    const std::vector<float> x = dottedBreakdown(130.0, 125.0, 20.0, 110.0);
    BeatTracker bt({}, kSr);
    double lo = 1e9, hi = 0;
    std::string trace;
    for (const auto& [t, bpm] : tempoTrace(bt, x)) {
        if (t < 12.0) continue;
        lo = std::min(lo, bpm);
        hi = std::max(hi, bpm);
        if (std::fmod(t, 5.0) < 0.25) trace += std::to_string(int(std::lround(bpm))) + " ";
    }
    MESSAGE("tempo every 5 s: " << trace);
    CHECK(lo > 125.0 * 0.98);
    CHECK(hi < 125.0 * 1.02);
}

TEST_CASE("BeatClock + BeatTracker (steady): the dotted-eighth breakdown keeps the lock and the phase") {
    // 60 s of kick (the clock learns the exact tempo), 90 s dotted-eighth breakdown, the kick again.
    const double bpm = 125.0, spb = 60.0 / bpm;
    const std::vector<float> x = dottedBreakdown(170.0, bpm, 60.0, 150.0);
    BeatTracker bt({}, kSr);
    reforce::sync::BeatClockConfig cc;
    cc.source = "audio";
    reforce::sync::BeatClock clock(cc);
    const size_t block = 735;
    double offset = 0.0, worstBreak = 0.0, worstDrop = 0.0;
    bool have = false, lost = false;
    for (size_t i = 0; i < x.size(); i += block) {
        bt.push(x.data() + i, std::min(block, x.size() - i));
        reforce::sync::BeatClockInput in;
        in.audioTempo = bt.tempo();
        in.audioBeat = bt.beat();
        in.audioStrength = bt.lastOnsetStrength();
        in.audioLowShare = bt.lastOnsetLowShare();
        in.audioKick = bt.kickPresent();
        in.audioKickness = bt.lastOnsetKickness();
        in.audioBeatness = bt.lastOnsetBeatness();
        in.kickPhase = bt.kickPhase();
        in.kickPhaseLock = bt.kickPhaseLock();
        in.foldPhase = bt.foldPhase();
        in.foldConf = bt.foldConfidence();
        in.filterPhase = bt.filterPhase();
        in.filterConf = bt.filterConfidence();
        const auto& st = clock.update(double(block) / kSr, in);
        const double t = bt.streamSeconds();
        if (t >= 50.0 && !have && st.confident) {
            offset = st.beat - t / spb;
            have = true;
        }
        if (!have) continue;
        const double e = st.beat - t / spb - offset, d = std::abs(e - std::round(e));
        if (t >= 60.0 && t < 150.0) {
            worstBreak = std::max(worstBreak, d);
            if (!st.confident) lost = true;
        }
        if (t >= 154.0) worstDrop = std::max(worstDrop, d);
    }
    MESSAGE("phase error in the breakdown " << worstBreak << ", after the drop " << worstDrop << " beat");
    REQUIRE(have);
    CHECK(!lost);
    CHECK(worstBreak < 0.08);
    CHECK(worstDrop < 0.06);
}

TEST_CASE("BeatTracker (steady): a new track after a breakdown still switches tempo") {
    // 124 kick, 30 s breakdown (no switch while it lasts), then a 100 BPM kick: read within ~14 s.
    const std::vector<float> x = houseBreakdown(80.0, 124.0, 16.0, 46.0, 100.0);
    BeatTracker bt({}, kSr);
    double firstOk = -1.0;
    for (const auto& [t, bpm] : tempoTrace(bt, x)) {
        if (t > 12.0 && t < 46.0) CHECK(std::abs(bpm / 124.0 - 1.0) < 0.02);
        if (t > 46.0 && firstOk < 0 && std::abs(bpm / 100.0 - 1.0) < 0.015) firstOk = t;
    }
    MESSAGE("100 BPM read " << firstOk - 46.0 << " s after the new kick");
    CHECK(firstOk > 46.0);
    CHECK(firstOk - 46.0 < 16.0);
}


TEST_CASE("BeatTracker: the beat fold puts the beat on the kick, not on the off-beat hats") {
    // 124 BPM: kick on the beats, loud noise hats on the off-beats.
    const double bpm = 124.0, beat = 60.0 / bpm, seconds = 30.0;
    std::vector<float> x = kickTrack(seconds, 0.0, bpm);
    uint32_t rng = 777u;
    for (double t = beat / 2; t < seconds; t += beat) {
        const size_t i0 = size_t(std::lround(t * kSr));
        for (size_t i = 0; i < size_t(0.05 * kSr) && i0 + i < x.size(); ++i) {
            rng = rng * 1664525u + 1013904223u;
            x[i0 + i] += float(0.5 * (double(rng >> 8) / double(1u << 24) * 2.0 - 1.0) * std::exp(-double(i) / kSr / 0.012));
        }
    }
    BeatTracker bt({}, kSr);
    bt.push(x.data(), x.size());
    REQUIRE(bt.tempo() == doctest::Approx(bpm).epsilon(0.02));
    REQUIRE(bt.foldPhase() >= 0.0);
    const double expect = bt.streamSeconds() / beat;
    double d = bt.foldPhase() - (expect - std::floor(expect));
    d -= std::round(d);
    CHECK(std::abs(d) < 0.12);
    CHECK(bt.foldConfidence() > 0.6);
}

TEST_CASE("BeatClock + BeatTracker (steady): a pitch ride 124 -> 128 BPM keeps the beat") {
    // Kicks at 124 for 30 s, then the tempo glides to 128 over 30 s (a DJ riding the pitch), then 128 for 20 s.
    auto bpmAt = [](double t) { return t < 30.0 ? 124.0 : t < 60.0 ? 124.0 + 4.0 * (t - 30.0) / 30.0 : 128.0; };
    const double seconds = 80.0;
    std::vector<float> x(size_t(seconds * kSr), 0.f);
    const std::vector<float> hit = kickHit();
    std::vector<double> beats;  // kick times
    for (double t = 0.0, beat = 0.0; t < seconds; t += 1.0 / kSr) {
        if (beat >= double(beats.size())) {
            beats.push_back(t);
            const size_t i0 = size_t(std::lround(t * kSr));
            for (size_t i = 0; i < hit.size() && i0 + i < x.size(); ++i) x[i0 + i] += hit[i];
        }
        beat += bpmAt(t) / 60.0 / kSr;
    }
    auto gridBeat = [&](double t) {  // the music's beat count at t (linear between kicks)
        size_t k = 0;
        while (k + 1 < beats.size() && beats[k + 1] <= t) ++k;
        return k + 1 < beats.size() ? double(k) + (t - beats[k]) / (beats[k + 1] - beats[k]) : double(k);
    };
    BeatConfig bc;
    bc.steady = true;
    BeatTracker bt(bc, kSr);
    reforce::sync::BeatClockConfig cc;
    cc.steady = true;
    cc.source = "audio";
    reforce::sync::BeatClock clock(cc);
    const size_t block = 735;
    const double dt = double(block) / kSr;
    double offset = 0.0, worstRide = 0.0, worstAfter = 0.0;
    bool haveOffset = false;
    for (size_t i = 0; i + block <= x.size(); i += block) {
        bt.push(x.data() + i, block);
        reforce::sync::BeatClockInput in;
        in.audioTempo = bt.tempo();
        in.audioBeat = bt.beat();
        in.audioStrength = bt.lastOnsetStrength();
        in.audioLowShare = bt.lastOnsetLowShare();
        in.audioKick = bt.kickPresent();
        in.audioKickness = bt.lastOnsetKickness();
        in.audioBeatness = bt.lastOnsetBeatness();
        in.foldPhase = bt.foldPhase();
        in.foldConf = bt.foldConfidence();
        in.filterPhase = bt.filterPhase();
        in.filterConf = bt.filterConfidence();
        const auto& st = clock.update(dt, in);
        const double t = bt.streamSeconds();
        if (t >= 25.0 && !haveOffset && st.confident) offset = st.beat - gridBeat(t), haveOffset = true;
        if (!haveOffset) continue;
        double e = st.beat - gridBeat(t) - offset;
        e = std::abs(e - std::round(e));
        if (t >= 30.0 && t < 60.0) worstRide = std::max(worstRide, e);
        if (t >= 70.0 && t < 78.0) worstAfter = std::max(worstAfter, e);  // (the grid ends with the last kick)
    }
    MESSAGE("phase error during the ride " << worstRide << ", after it " << worstAfter << " beat; TEMPO " << bt.tempo());
    CHECK(haveOffset);
    CHECK(clock.state().confident);
    CHECK(bt.tempo() == doctest::Approx(128.0).epsilon(0.01));
    CHECK(worstRide < 0.15);
    CHECK(worstAfter < 0.06);
}
