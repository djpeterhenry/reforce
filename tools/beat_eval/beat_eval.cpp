// reforce-beat-eval: scores the audio tempo tracker (audio/BeatTracker) and the beat clock (sync/BeatClock) against
// hand-made beat grids (Peter's DJ library: Ableton warp markers, via tools/beat_eval/beat_eval.py).
//
//   reforce-beat-eval [--jobs N] [--max-seconds S] [--warmup S] [--set key=value ...] MANIFEST.tsv
//
// MANIFEST.tsv, one song per line: id <TAB> audio path <TAB> sec:beat,sec:beat,... (warp markers, >= 2).
// Audio files are only read (ExtAudioFile). Output, one JSON object per song on stdout (in manifest order):
//   tempo: fractions of the scored time (after warmup) where TEMPO is right (+-2 %), 0, x1/2, x2, x2/3, x3/2, other
//   clock: locked fraction, and of the scored time: locked at the right tempo with |phase| < 0.1 beat ("onbeat"),
//          locked half a beat off ("offbeat"); mean signed phase in ms while on the beat (+ = the clock's beats come
//          before the grid, i.e. early); seconds to the first right lock.
// --set overrides tracker (beat.NAME) and clock (clock.NAME) parameters, for tuning; see kParams below.
// --mix [--blend-beats N]: the manifest holds pairs instead (A's three fields, B's three, ride 0/1): B is resampled to
//   A's tempo, enters on a 16-beat line of A's grid, blends over N beats (B fades in over the first half with its
//   lows cut, a one-beat bass swap at the middle, A fades out); with ride 1 the pitch then glides back to B's own
//   tempo over 32 s. Scored per part (A alone, blend halves, B after, the ride) against A's then B's grid.
//   Ride "nudge:BPM:SECONDS[:AT]" instead: AT s (default 30) after the blend the master tempo moves by BPM over
//   SECONDS (Peter's quick 1-3 BPM nudges), scored from the glide to 40 s after it. --oracle-tempo: B's true tempo
//   replaces TEMPO (the ceiling for better tempo tracking). --trace (one pair): u (s from the mix-in), part, ...

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/AudioFileSource.hpp"
#include "audio/BeatTracker.hpp"
#include "sync/BeatClock.hpp"

using namespace reforce;

namespace {

struct Song {
    std::string id, path;
    std::vector<std::pair<double, double>> markers;  // (sec, beat), sorted by sec
};

struct Grid {
    const std::vector<std::pair<double, double>>& m;
    size_t seg(double t) const {
        size_t i = 0;
        while (i + 2 < m.size() && t >= m[i + 1].first) ++i;
        return i;
    }
    double beatAt(double t) const {
        const size_t i = seg(t);
        const auto& [s0, b0] = m[i];
        const auto& [s1, b1] = m[i + 1];
        return b0 + (t - s0) * (b1 - b0) / (s1 - s0);
    }
    double bpmAt(double t) const {
        const size_t i = seg(t);
        return 60.0 * (m[i + 1].second - m[i].second) / (m[i + 1].first - m[i].first);
    }
};

struct Params {
    audio::BeatConfig beat;
    sync::BeatClockConfig clock;
    bool oracleTempo = false;  // --oracle-tempo (--mix): B's true tempo replaces TEMPO (what perfect tempo would give)
};

// Tunable parameters (--set NAME=VALUE).
bool setParam(Params& p, const std::string& name, double v) {
    struct D { const char* n; double audio::BeatConfig::* f; };
    static const D kBeat[] = {
        {"beat.foldMinBpm", &audio::BeatConfig::foldMinBpm}, {"beat.priorBpm", &audio::BeatConfig::priorBpm},
        {"beat.priorOctaves", &audio::BeatConfig::priorOctaves}, {"beat.minConfidence", &audio::BeatConfig::minConfidence},
        {"beat.steadyWindow", &audio::BeatConfig::steadyWindow}, {"beat.switchSeconds", &audio::BeatConfig::switchSeconds},
        {"beat.switchMinConfidence", &audio::BeatConfig::switchMinConfidence},
        {"beat.kickPresentRatio", &audio::BeatConfig::kickPresentRatio}, {"beat.kickHiHz", &audio::BeatConfig::kickHiHz},
        {"beat.kickLevelSeconds", &audio::BeatConfig::kickLevelSeconds},
        {"beat.kickRefSeconds", &audio::BeatConfig::kickRefSeconds}, {"beat.thresholdK", &audio::BeatConfig::thresholdK},
        {"beat.lowWeight", &audio::BeatConfig::lowWeight}, {"beat.midWeight", &audio::BeatConfig::midWeight},
        {"beat.tempoWindowSeconds", &audio::BeatConfig::tempoWindowSeconds},
        {"beat.tempoSmoothSeconds", &audio::BeatConfig::tempoSmoothSeconds},
        {"beat.lowHiHz", &audio::BeatConfig::lowHiHz}, {"beat.comb2", &audio::BeatConfig::comb2},
        {"beat.comb4", &audio::BeatConfig::comb4}, {"beat.comb8", &audio::BeatConfig::comb8}, {"beat.firstLockSeconds", &audio::BeatConfig::firstLockSeconds},
        {"beat.steadyHoldSeconds", &audio::BeatConfig::steadyHoldSeconds}, {"beat.midHiHz", &audio::BeatConfig::midHiHz},
    };
    struct C { const char* n; double sync::BeatClockConfig::* f; };
    static const C kClock[] = {
        {"clock.kickShare", &sync::BeatClockConfig::kickShare}, {"clock.gatePhase", &sync::BeatClockConfig::gatePhase},
        {"clock.lowFloor", &sync::BeatClockConfig::lowFloor}, {"clock.slewSeconds", &sync::BeatClockConfig::slewSeconds},
        {"clock.memorySeconds", &sync::BeatClockConfig::memorySeconds}, {"clock.lockOn", &sync::BeatClockConfig::lockOn},
        {"clock.lockOff", &sync::BeatClockConfig::lockOff}, {"clock.minWeight", &sync::BeatClockConfig::minWeight},
        {"clock.trimSeconds", &sync::BeatClockConfig::trimSeconds}, {"clock.maxTrim", &sync::BeatClockConfig::maxTrim},
        {"clock.trimPhase", &sync::BeatClockConfig::trimPhase},
        {"clock.maxSlewSeconds", &sync::BeatClockConfig::maxSlewSeconds},
        {"clock.audioOffsetMs", &sync::BeatClockConfig::audioOffsetMs},
    };
    for (const auto& d : kBeat)
        if (name == d.n) return (p.beat.*(d.f) = v, true);
    for (const auto& c : kClock)
        if (name == c.n) return (p.clock.*(c.f) = v, true);
    if (name == "steady") return (p.beat.steady = p.clock.steady = v != 0, true);
    if (name == "clock.escapeKicks") return (p.clock.escapeKicks = int(v), true);
    if (name == "clock.useKickness") return (p.clock.useKickness = v != 0, true);
    if (name == "clock.beatnessKickMix") return (p.clock.beatnessKickMix = v, true);
    if (name == "clock.useBeatness") return (p.clock.useBeatness = v != 0, true);
    if (name == "clock.useKickPhase") return (p.clock.useKickPhase = v != 0, true);
    if (name == "clock.offbeatRescue") return (p.clock.offbeatRescue = v != 0, true);
    if (name == "clock.offbeatPhase") return (p.clock.offbeatPhase = v, true);
    if (name == "clock.offbeatSeconds") return (p.clock.offbeatSeconds = v, true);
    if (name == "clock.rescueFromOnsets") return (p.clock.rescueFromOnsets = v != 0, true);
    if (name == "clock.rescueMemorySeconds") return (p.clock.rescueMemorySeconds = v, true);
    if (name == "clock.useFold") return (p.clock.useFold = v != 0, true);
    if (name == "clock.foldPhaseOff") return (p.clock.foldPhaseOff = v, true);
    if (name == "clock.foldWithoutKick") return (p.clock.foldWithoutKick = v != 0, true);
    if (name == "clock.useFilter") return (p.clock.useFilter = v != 0, true);
    if (name == "clock.filterOn") return (p.clock.filterOn = v, true);
    if (name == "clock.filterOff") return (p.clock.filterOff = v, true);
    if (name == "clock.filterOffsetMs") return (p.clock.filterOffsetMs = v, true);
    if (name == "beat.pfDiffusionMoving") return (p.beat.pfDiffusionMoving = v, true);
    if (name == "beat.pfMovingBand") return (p.beat.pfMovingBand = v, true);
    if (name == "beat.pfMovingSeconds") return (p.beat.pfMovingSeconds = v, true);
    if (name == "beat.pfFoldGainMoving") return (p.beat.pfFoldGainMoving = v, true);
    if (name == "beat.pfDiffusion") return (p.beat.pfDiffusion = v, true);
    if (name == "beat.pfTempoRows") return (p.beat.pfTempoRows = int(v), true);
    if (name == "beat.pfTempoStep") return (p.beat.pfTempoStep = v, true);
    if (name == "beat.pfTempoDiffusion") return (p.beat.pfTempoDiffusion = v, true);
    if (name == "beat.steadyDriftMinConfidence") return (p.beat.steadyDriftMinConfidence = v, true);
    if (name == "beat.pfTempoDiffusionSteady") return (p.beat.pfTempoDiffusionSteady = v, true);
    if (name == "beat.pfOpenBand") return (p.beat.pfOpenBand = v, true);
    if (name == "beat.pfOpenMinConfidence") return (p.beat.pfOpenMinConfidence = v, true);
    if (name == "beat.pfOpenHoldSeconds") return (p.beat.pfOpenHoldSeconds = v, true);
    if (name == "beat.kgRestartBeatness") return (p.beat.kgRestartBeatness = v, true);
    if (name == "beat.kgLostSeconds") return (p.beat.kgLostSeconds = v, true);
    if (name == "beat.kgSeconds") return (p.beat.kgSeconds = v, true);
    if (name == "beat.kgMinBeatness") return (p.beat.kgMinBeatness = v, true);
    if (name == "beat.kgGate") return (p.beat.kgGate = v, true);
    if (name == "beat.kgBand") return (p.beat.kgBand = v, true);
    if (name == "beat.kgSmoothSeconds") return (p.beat.kgSmoothSeconds = v, true);
    if (name == "beat.kgMinPoints") return (p.beat.kgMinPoints = int(v), true);
    if (name == "beat.kgMinBeats") return (p.beat.kgMinBeats = v, true);
    if (name == "beat.kgMaxResidual") return (p.beat.kgMaxResidual = v, true);
    if (name == "beat.pfTempoPrior") return (p.beat.pfTempoPrior = v, true);
    if (name == "beat.pfGain") return (p.beat.pfGain = v, true);
    if (name == "beat.pfWidth") return (p.beat.pfWidth = v, true);
    if (name == "beat.pfFloor") return (p.beat.pfFloor = v, true);
    if (name == "beat.pfFoldGain") return (p.beat.pfFoldGain = v, true);
    if (name == "beat.pfActSeconds") return (p.beat.pfActSeconds = v, true);
    if (name == "clock.foldMinConf") return (p.clock.foldMinConf = v, true);
    if (name == "beat.foldSeconds") return (p.beat.foldSeconds = v, true);
    if (name == "clock.rescuePower") return (p.clock.rescuePower = v, true);
    if (name == "clock.offbeatMinLock") return (p.clock.offbeatMinLock = v, true);
    if (name == "beat.combHalf") return (p.beat.combHalf = v, true);
    if (name == "beat.acfBeatness") return (p.beat.acfBeatness = v, true);
    if (name == "beat.steadyFastSeconds") return (p.beat.steadyFastSeconds = v, true);
    if (name == "beat.steadyFastBand") return (p.beat.steadyFastBand = v, true);
    if (name == "beat.useHalfModel") return (p.beat.useHalfModel = v != 0, true);
    if (name == "beat.combPeaks") return (p.beat.combPeaks = v != 0, true);
    if (name == "beat.refine4") return (p.beat.refine4 = v != 0, true);
    if (name == "beat.steadySmoothSeconds") return (p.beat.steadySmoothSeconds = v, true);
    if (name == "clock.kickLockOn") return (p.clock.kickLockOn = v, true);
    if (name == "clock.kickLockOff") return (p.clock.kickLockOff = v, true);
    if (name == "clock.kickPhaseOffsetMs") return (p.clock.kickPhaseOffsetMs = v, true);
    if (name == "beat.kickPhaseSeconds") return (p.beat.kickPhaseSeconds = v, true);
    if (name == "beat.kickPhasePower") return (p.beat.kickPhasePower = v, true);
    if (name == "beat.kickPeakSeconds") return (p.beat.kickPeakSeconds = v, true);
    if (name == "beat.minOnsets") return (p.beat.minOnsets = int(v), true);
    return false;
}

FILE* gTrace = nullptr;
FILE* gDump = nullptr;  // --dump-onsets FILE: one CSV row per onset (features + grid position), for training
FILE* gFolds = nullptr;  // --dump-folds FILE: binary records every second (BeatTracker::foldProfile), for training
std::mutex gDumpMutex;  // --trace FILE: per 0.5 s CSV of the songs (use with a one-song manifest)

std::string evalSong(const Song& s, int songIndex, const Params& prm, double maxSeconds, double warmup) {
    char out[1024];
    audio::AudioFileSource src;
    audio::FileOptions fo;
    fo.path = s.path;
    if (!src.open(fo)) {
        std::snprintf(out, sizeof out, "{\"id\":\"%s\",\"error\":\"open failed\"}", s.id.c_str());
        return out;
    }
    const double sr = src.sampleRate();
    const size_t block = size_t(std::lround(sr / 60.0));
    const double dt = double(block) / sr;
    audio::BeatTracker bt(prm.beat, sr);
    sync::BeatClock clock(prm.clock);
    const Grid g{s.markers};
    std::vector<float> buf(block);
    const double dur = std::min(maxSeconds, src.durationSeconds());
    long n = 0, ok = 0, zero = 0, half = 0, dbl = 0, two3 = 0, three2 = 0, other = 0;
    long locked = 0, onbeat = 0, offbeat = 0, fineTempo = 0, fineClock = 0;  // within 0.3 %
    long nKick = 0, onKick = 0, onNoKick = 0, okKick = 0, okNoKick = 0;  // split: a kick playing (kickPresent) or not
    double phaseSumMs = 0, firstLock = -1;
    long hist[10] = {};
    long onK[10] = {}, onO[10] = {};  // onsets vs the grid (tenths of a beat): kick-ness >= 0.5 / below
    uint64_t seenOnsets = 0;
    long ratioHist[48] = {};  // TEMPO / truth when not right: log2 in 1/24 octave bins over [-1, 1)
    std::vector<float> phases;  // locked at the right tempo: phase vs the grid (beats), for the song's own offset
    std::vector<std::pair<double, double>> drift;  // (t, phase) while locked and the clock's tempo is within 0.5 %
    long kpN = 0, kpOn = 0, kpOff = 0, kpHist[20] = {};  // kick phase vs the grid while TEMPO is right: within 0.1 beat / half off  // locked at the right tempo: phase vs the grid in tenths of a beat (0 = on the beat)
    double t = 0;
    while (t < dur) {
        const size_t got = src.advance(block);
        if (!got) break;
        const size_t r = src.ring().read(buf.data(), block);
        bt.push(buf.data(), r);
        t = bt.streamSeconds();
        sync::BeatClockInput in;
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
        const sync::BeatClockState& st = clock.update(dt, in);
        if (bt.onsetCount() != seenOnsets) {
            seenOnsets = bt.onsetCount();
            const double b = g.beatAt(bt.lastOnsetSeconds() - 0.010), f = b - std::floor(b);
            if (gDump && t >= warmup && std::abs(bt.tempo() / g.bpmAt(t) - 1.0) < 0.02) {
                char row[768];
                int len = std::snprintf(row, sizeof row, "%s,%.4f,%.4f,%.3f,%.3f,%d", s.id.c_str(), f, bt.lastOnsetKickness(),
                                        bt.lastOnsetStrength(), bt.lastOnsetSeconds(), int(bt.kickPresent()));
                for (double v : bt.lastOnsetBands()) len += std::snprintf(row + len, sizeof row - size_t(len), ",%.4f", v);
                for (double v : bt.lastOnsetBandsNext())
                    len += std::snprintf(row + len, sizeof row - size_t(len), ",%.4f", v);
                for (double v : bt.lastOnsetBandsHalf())
                    len += std::snprintf(row + len, sizeof row - size_t(len), ",%.4f", v);
                std::lock_guard<std::mutex> lk(gDumpMutex);
                std::fprintf(gDump, "%s\n", row);
            }
            (bt.lastOnsetKickness() >= 0.5 ? onK : onO)[int(std::floor(f * 10.0 + 0.5)) % 10]++;
        }
        if (gFolds && t >= warmup && std::fmod(t, 1.0) < dt && bt.tempo() > 0 &&
            std::abs(bt.tempo() / g.bpmAt(t) - 1.0) < 0.02) {
            // record: int32 song index, float t, kick, beat position on phi's circle, TEMPO / truth, then the profile
            struct { int32_t i; float t, kick, theta, ratio; } h{songIndex, float(t), float(bt.kickPresent()), 0.f,
                                                                 float(bt.tempo() / g.bpmAt(t))};
            const double gb = g.beatAt(t), th = bt.foldPhi() - (gb - std::floor(gb));
            h.theta = float(th - std::floor(th));
            const auto prof = bt.foldProfile();
            std::lock_guard<std::mutex> lk(gDumpMutex);
            std::fwrite(&h, sizeof h, 1, gFolds);
            std::fwrite(prof.data(), sizeof(float), prof.size(), gFolds);
        }
        if (gTrace && std::fmod(t, 0.5) < dt) {
            const double e0 = st.beat - g.beatAt(t);
            const auto a1 = bt.debugAcf(g.bpmAt(t)), a23 = bt.debugAcf(g.bpmAt(t) * 2.0 / 3.0),
                       a43 = bt.debugAcf(g.bpmAt(t) * 4.0 / 3.0);
            std::fprintf(gTrace, "%.2f,%.2f,%.2f,%.2f,%d,%.3f,%.2f,%d,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", t, g.bpmAt(t), bt.tempo(), st.bpm,
                         int(st.confident), e0 - std::round(e0), st.lock, int(bt.kickPresent()), bt.kickLevelRatio(),
                         bt.lastOnsetLowShare(), bt.kickPhase(), bt.kickPhaseLock(), a1[0], a1[1], a23[0], a23[1],
                         a43[0], a43[1]);
        }
        if (t < warmup) continue;
        ++n;
        const double truth = g.bpmAt(t), tempo = bt.tempo();
        const bool kickNow = bt.kickPresent();
        if (kickNow) ++nKick;
        if (tempo > 0 && std::abs(tempo / truth - 1.0) < 0.02) ++(kickNow ? okKick : okNoKick);
        auto near = [&](double r) { return std::abs(tempo / (truth * r) - 1.0) < 0.02; };
        if (near(1) && bt.kickPhase() >= 0 && bt.kickPresent()) {
            double e = bt.kickPhase() - (g.beatAt(t) - std::floor(g.beatAt(t))) - 0.015 * truth / 60.0;  // 15 ms
            e -= std::round(e);
            ++kpN;
            ++kpHist[int(std::floor((e + 0.5) * 20.0)) % 20];
            if (std::abs(e) < 0.1) ++kpOn;
            else if (std::abs(std::abs(e) - 0.5) < 0.1) ++kpOff;
        }
        if (tempo > 0 && std::abs(tempo / truth - 1.0) < 0.003) ++fineTempo;
        if (st.confident && std::abs(st.bpm / truth - 1.0) < 0.003) ++fineClock;
        if (tempo <= 0) ++zero;
        else if (near(1)) ++ok;
        else if (near(0.5)) ++half;
        else if (near(2)) ++dbl;
        else if (near(2.0 / 3.0)) ++two3;
        else if (near(1.5)) ++three2;
        else ++other;
        if (tempo > 0 && !near(1)) ++ratioHist[std::clamp(int(std::floor((std::log2(tempo / truth) + 1.0) * 24.0)), 0, 47)];
        if (st.source == sync::BeatSource::Audio && st.confident) {
            ++locked;
            if (std::abs(st.bpm / truth - 1.0) < 0.02) {
                const double e0 = st.beat - g.beatAt(t), e = e0 - std::round(e0);
                ++hist[int(std::floor((e + 1.05) * 10.0)) % 10];
                phases.push_back(float(e));
                if (std::abs(st.bpm / truth - 1.0) < 0.005 && std::fmod(t, 1.0) < dt) drift.emplace_back(t, e);
                if (std::abs(e) < 0.1) {
                    ++onbeat;
                    ++(kickNow ? onKick : onNoKick);
                    phaseSumMs += e * 60000.0 / truth;
                    if (firstLock < 0) firstLock = t;
                } else if (std::abs(std::abs(e) - 0.5) < 0.1) {
                    ++offbeat;
                }
            }
        }
    }
    const double N = n ? double(n) : 1.0;
    // Grid drift: unwrap the sampled phase and fit a line; the slope over the song = how far the grid slides
    // against the music (a two-marker grid on drifting music), with the fit's residual RMS.
    double driftBeats = 0, driftRms = 0;
    if (drift.size() >= 30) {
        std::vector<double> u(drift.size());
        u[0] = drift[0].second;
        for (size_t i = 1; i < drift.size(); ++i) {
            double d = drift[i].second - drift[i - 1].second;
            d -= std::round(d);
            u[i] = u[i - 1] + d;
        }
        double st = 0, su = 0, stt = 0, stu = 0;
        const double k = double(drift.size());
        for (size_t i = 0; i < drift.size(); ++i)
            st += drift[i].first, su += u[i], stt += drift[i].first * drift[i].first, stu += drift[i].first * u[i];
        const double slope = (k * stu - st * su) / std::max(1e-9, k * stt - st * st), icpt = (su - slope * st) / k;
        for (size_t i = 0; i < drift.size(); ++i) {
            const double r = u[i] - (icpt + slope * drift[i].first);
            driftRms += r * r;
        }
        driftRms = std::sqrt(driftRms / k);
        driftBeats = slope * (drift.back().first - drift.front().first);
    }
    // "aligned": on the beat after removing the song's own constant offset (its circular mean phase): grid / decoder
    // offsets (MP3 priming) and latency are the Beat offset's job, this measures holding the beat.
    double cre = 0, cim = 0;
    for (float e : phases) cre += std::cos(6.283185307179586 * e), cim += std::sin(6.283185307179586 * e);
    const double mean = phases.empty() ? 0.0 : std::atan2(cim, cre) / 6.283185307179586;
    long aligned = 0;
    for (float e : phases) {
        double d = double(e) - mean;
        d -= std::round(d);
        if (std::abs(d) < 0.1) ++aligned;
    }
    std::snprintf(out, sizeof out,
                  "{\"id\":\"%s\",\"seconds\":%.1f,\"bpm\":%.2f,\"tempo\":{\"ok\":%.4f,\"zero\":%.4f,\"half\":%.4f,"
                  "\"double\":%.4f,\"two_thirds\":%.4f,\"three_halves\":%.4f,\"other\":%.4f,\"fine\":%.4f,\"clock_fine\":%.4f},"
                  "\"split\":{\"kick\":%ld,\"nokick\":%ld,\"ok_kick\":%ld,\"ok_nokick\":%ld,\"on_kick\":%ld,\"on_nokick\":%ld},"
                  "\"clock\":{\"rescues\":%d,\"locked\":%.4f,\"onbeat\":%.4f,\"offbeat\":%.4f,\"phase_ms\":%.2f,\"first_lock\":%.2f,\"aligned\":%.4f,\"offset_ms\":%.1f,\"drift_beats\":%.3f,\"drift_rms\":%.3f,"
                  "\"phase_hist\":[%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld]},\"ratio_hist\":[%s],\"onsets_kick\":[%s],\"onsets_other\":[%s],\"kick_phase\":{\"n\":%ld,\"on\":%ld,\"off\":%ld,\"hist\":[%s]}}",
                  s.id.c_str(), t, g.bpmAt(warmup), ok / N, zero / N, half / N, dbl / N, two3 / N, three2 / N, other / N, fineTempo / N, fineClock / N,
                  nKick, n - nKick, okKick, okNoKick, onKick, onNoKick,
                  clock.rescues(), locked / N, onbeat / N, offbeat / N, onbeat ? phaseSumMs / double(onbeat) : 0.0, firstLock,
                  aligned / N, mean * 60000.0 / g.bpmAt(warmup), driftBeats, driftRms, hist[0],
                  hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9], [&] {
                      std::string h;
                      for (int k = 0; k < 48; ++k) h += (k ? "," : "") + std::to_string(ratioHist[k]);
                      return h;
                  }().c_str(), [&] {
                      std::string h;
                      for (int k = 0; k < 10; ++k) h += (k ? "," : "") + std::to_string(onK[k]);
                      return h;
                  }().c_str(), [&] {
                      std::string h;
                      for (int k = 0; k < 10; ++k) h += (k ? "," : "") + std::to_string(onO[k]);
                      return h;
                  }().c_str(), kpN, kpOn, kpOff, [&] {
                      std::string h;
                      for (int k = 0; k < 20; ++k) h += (k ? "," : "") + std::to_string(kpHist[k]);
                      return h;
                  }().c_str());
    return out;
}

// ---- DJ mix simulation (--mix): two gridded songs, beatmatched and blended as Peter mixes ----

struct MixPair {
    Song a, b;
    bool ride = false;  // after the blend, the pitch rides back to B's own tempo
    double nudgeAt = 30.0;  // nudge: the glide starts this long after the blend (s)
    double nudgeBpm = 0.0, nudgeSec = 0.0;  // or (nudgeSec > 0): 30 s after the blend, the master tempo moves by
                                            // nudgeBpm over nudgeSec (Peter's quick 1-3 BPM changes), then holds
};

// Whole file, mono, at its own rate (0 frames: open failed).
std::vector<float> decodeAll(const std::string& path, double& sr) {
    audio::AudioFileSource src;
    audio::FileOptions fo;
    fo.path = path;
    std::vector<float> out;
    sr = 0;
    if (!src.open(fo)) return out;
    sr = src.sampleRate();
    const size_t total = size_t(src.durationSeconds() * sr), block = 4096;
    out.reserve(total + block);
    std::vector<float> buf(block);
    while (out.size() < total) {
        const size_t got = src.advance(block);
        if (!got) break;
        const size_t r = src.ring().read(buf.data(), block);
        out.insert(out.end(), buf.begin(), buf.begin() + long(std::min(r, got)));
    }
    out.resize(std::min(out.size(), total));
    return out;
}

double secAtBeat(const std::vector<std::pair<double, double>>& m, double beat) {
    size_t i = 0;
    while (i + 2 < m.size() && beat >= m[i + 1].second) ++i;
    const auto& [s0, b0] = m[i];
    const auto& [s1, b1] = m[i + 1];
    return s0 + (beat - b0) * (s1 - s0) / (b1 - b0);
}

// 2nd-order Butterworth low-pass (RBJ biquad), for the bass swap: low = lp(x), the rest = x - low.
struct LowPass {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    LowPass(double fc, double sr) {
        const double w = 2 * M_PI * fc / sr, c = std::cos(w), al = std::sin(w) / std::sqrt(2.0), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0, b1 = (1 - c) / a0, b2 = b0, a1 = -2 * c / a0, a2 = (1 - al) / a0;
    }
    double operator()(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

double rmsOf(const std::vector<float>& x, size_t from, size_t to) {
    double s = 0;
    to = std::min(to, x.size());
    for (size_t i = from; i < to; ++i) s += double(x[i]) * x[i];
    return to > from ? std::sqrt(s / double(to - from)) : 0.0;
}

constexpr int kSegs = 6;
const char* kSegNames[kSegs] = {"a", "blend1", "blend2", "b30", "b90", "ride"};

std::string evalMix(const MixPair& p, const Params& prm, double blendBeats, double warmup) {
    char out[2048];
    double srA = 0, srB = 0;
    const std::vector<float> A = decodeAll(p.a.path, srA), Bv = decodeAll(p.b.path, srB);
    auto fail = [&](const char* why) {
        std::snprintf(out, sizeof out, "{\"a\":\"%s\",\"b\":\"%s\",\"error\":\"%s\"}", p.a.id.c_str(), p.b.id.c_str(), why);
        return std::string(out);
    };
    if (A.empty() || Bv.empty()) return fail("open failed");
    const Grid gA{p.a.markers}, gB{p.b.markers};
    const double durA = double(A.size()) / srA;
    // Mix in on a 16-beat line of A that leaves the blend + 8 beats before A ends; B from its first 16-beat line.
    const double m = 16.0 * std::floor((gA.beatAt(durA) - blendBeats - 8.0) / 16.0);
    const double tMix = secAtBeat(p.a.markers, m);
    if (tMix < warmup + 30.0) return fail("A too short");
    double kB = 0.0;
    while (secAtBeat(p.b.markers, kB) < 0.0) kB += 16.0;
    const double bStart = secAtBeat(p.b.markers, kB);
    const double TA = gA.bpmAt(tMix), TB = gB.bpmAt(bStart + 1.0), r0 = TA / TB;
    if (std::abs(r0 - 1.0) > 0.1) return fail("tempi too far apart");
    const double blendSec = blendBeats * 60.0 / TA;
    const double t0 = std::max(0.0, tMix - warmup - 60.0);  // render from here: warmup, then 60 s of A alone
    const bool nudge = p.nudgeSec > 0.0;
    const double rideAt = nudge ? blendSec + p.nudgeAt : blendSec + 16.0, rideLen = nudge ? p.nudgeSec : 32.0;  // (s after tMix)
    const double rEnd = nudge ? r0 * (1.0 + p.nudgeBpm / TA) : 1.0;  // B's speed after the ride / nudge
    const double after = nudge ? rideAt + rideLen + 40.0 : p.ride ? rideAt + rideLen + 30.0 : blendSec + 90.0;
    if (bStart + after * std::max(r0, rEnd) + 1.0 > double(Bv.size()) / srB) return fail("B too short");
    const size_t n = size_t((tMix - t0 + after) * srA);
    // Speed of B (its seconds per mix second) and its position.
    auto speed = [&](double u) {
        if ((!p.ride && !nudge) || u < rideAt) return r0;
        return r0 + (rEnd - r0) * std::min(1.0, (u - rideAt) / rideLen);
    };
    const size_t iMix = size_t((tMix - t0) * srA);
    // Equal loudness: A over its last minute before the mix, B over its first minute.
    const double rmsA = rmsOf(A, size_t((tMix - 60.0) * srA), size_t(tMix * srA));
    const double rmsB = rmsOf(Bv, size_t(bStart * srB), size_t((bStart + 60.0) * srB));
    if (rmsA < 1e-5 || rmsB < 1e-5) return fail("silent");
    const double gainB = rmsA / rmsB;
    std::vector<float> x(n, 0.f);
    std::vector<double> posB(n, -1.0);  // B's own seconds at each output sample (-1: not playing)
    LowPass lpA(150.0, srA), lpB(150.0, srA);
    double pb = bStart;
    const double swapLen = 60.0 / TA;  // the bass swap takes one beat
    for (size_t i = 0; i < n; ++i) {
        const double u = (double(i) - double(iMix)) / srA;  // seconds since the mix-in
        double v = 0.0;
        const size_t ia = size_t(t0 * srA) + i;
        const double a = ia < A.size() ? A[ia] : 0.0, aLow = lpA(a);
        double gA = 1.0, lowA = 1.0;
        if (u > blendSec / 2) gA = std::max(0.0, 1.0 - (u - blendSec / 2) / (blendSec / 2));
        if (u > blendSec / 2) lowA = std::max(0.0, 1.0 - (u - blendSec / 2) / swapLen);
        v += gA * ((a - aLow) + lowA * aLow);
        if (u >= 0.0) {
            const double q = pb * srB, fl = std::floor(q);
            const size_t j = size_t(fl);
            const double bs = j + 1 < Bv.size() ? (Bv[j] * (1.0 - (q - fl)) + Bv[j + 1] * (q - fl)) * gainB : 0.0;
            const double bLow = lpB(bs);
            const double gB = std::min(1.0, u / (blendSec / 2));
            const double lowB = u < blendSec / 2 ? 0.0 : std::min(1.0, (u - blendSec / 2) / swapLen);
            v += gB * ((bs - bLow) + lowB * bLow);
            posB[i] = pb;
            pb += speed(u) / srA;
        }
        x[i] = float(v);
    }
    // Truth: A's grid up to the end of the blend, then B's (aligned: B's beat kB = A's beat m).
    auto truth = [&](size_t i, double& bpm) {
        const double u = (double(i) - double(iMix)) / srA;
        if (u < blendSec) {
            bpm = gA.bpmAt(t0 + double(i) / srA);
            return gA.beatAt(t0 + double(i) / srA);
        }
        bpm = gB.bpmAt(posB[i]) * speed(u);
        return m + gB.beatAt(posB[i]) - kB;
    };
    audio::BeatTracker bt(prm.beat, srA);
    sync::BeatClock clock(prm.clock);
    const size_t block = size_t(std::lround(srA / 60.0));
    const double dt = double(block) / srA;
    long cnt[kSegs] = {}, ok[kSegs] = {}, on[kSegs] = {}, off[kSegs] = {}, lk[kSegs] = {}, cb[kSegs] = {};
    double lastOff = -1;  // last time (s since the blend's end) the clock was not on the beat
    for (size_t i = 0; i + block <= n; i += block) {
        if (prm.oracleTempo && (double(i) - double(iMix)) / srA > blendSec) {  // diagnostics: the true tempo, on B
            double tbo = 0;
            truth(i, tbo);
            bt.overrideTempo(tbo);
        }
        bt.push(x.data() + i, block);
        sync::BeatClockInput in;
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
        const sync::BeatClockState& st = clock.update(dt, in);
        const size_t end = i + block;
        const double t = double(end) / srA;
        if (t < warmup) continue;
        const double u = (double(end) - double(iMix)) / srA;
        const int seg = u < 0 ? 0 : u < blendSec / 2 ? 1 : u < blendSec ? 2
                      : (p.ride || nudge) && u >= rideAt ? 5 : u < blendSec + 30.0 ? 3 : 4;
        double tb = 0;
        const double tbeat = truth(end - 1, tb);
        ++cnt[seg];
        const bool tempoOk = bt.tempo() > 0 && std::abs(bt.tempo() / tb - 1.0) < 0.02;
        if (tempoOk) ++ok[seg];
        bool onb = false;
        if (st.source == sync::BeatSource::Audio && st.confident) {
            ++lk[seg];
            if (std::abs(st.bpm / tb - 1.0) < 0.02) {
                ++cb[seg];
                const double e0 = st.beat - tbeat, e = e0 - std::round(e0);
                if (std::abs(e) < 0.1) ++on[seg], onb = true;
                else if (std::abs(std::abs(e) - 0.5) < 0.1) ++off[seg];
            }
        }
        if (nudge ? u >= rideAt + rideLen && !onb : u >= blendSec && !onb)
            lastOff = u - (nudge ? rideAt + rideLen : blendSec);  // nudge: settle after the glide's end
        if (gTrace && std::fmod(t, 0.5) < dt) {
            const double e0 = st.beat - tbeat;
            double fe = bt.filterPhase() - (tbeat - std::floor(tbeat));  // the phase filter's own error (beats)
            fe = bt.filterPhase() >= 0 ? fe - std::round(fe) : -9.0;
            std::fprintf(gTrace, "%.2f,%d,%.2f,%.2f,%.2f,%d,%.3f,%.2f,%d,%.2f,%.3f\n", u, seg, tb, bt.tempo(), st.bpm,
                         int(st.confident), e0 - std::round(e0), bt.filterConfidence(), int(bt.kickPresent()),
                         bt.kickGridTempo(), fe);
        }
    }
    std::string segs;
    for (int s = 0; s < kSegs; ++s) {
        char b[200];
        std::snprintf(b, sizeof b, "%s\"%s\":{\"n\":%ld,\"ok\":%ld,\"on\":%ld,\"off\":%ld,\"locked\":%ld,\"clock_bpm_ok\":%ld}",
                      s ? "," : "", kSegNames[s], cnt[s], ok[s], on[s], off[s], lk[s], cb[s]);
        segs += b;
    }
    std::snprintf(out, sizeof out,
                  "{\"a\":\"%s\",\"b\":\"%s\",\"ride\":%d,\"tempo_a\":%.2f,\"tempo_b\":%.2f,\"blend_s\":%.1f,"
                  "\"settle_s\":%.2f,\"segs\":{%s}}",
                  p.a.id.c_str(), p.b.id.c_str(), int(p.ride), TA, TB, blendSec, lastOff, segs.c_str());
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        if (static_cast<unsigned char>(c) < 0x20) continue;
        o.push_back(c);
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    int jobs = int(std::max(1u, std::thread::hardware_concurrency() - 2));
    double maxSeconds = 600, warmup = 10;
    Params prm;
    std::string manifest;
    bool mix = false;  // --mix: the manifest holds pairs (A's three fields, B's three, ride 0/1), see evalMix
    double blendBeats = 64;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool v = i + 1 < argc;
        if (a == "--jobs" && v) jobs = std::max(1, std::atoi(argv[++i]));
        else if (a == "--max-seconds" && v) maxSeconds = std::atof(argv[++i]);
        else if (a == "--warmup" && v) warmup = std::atof(argv[++i]);
        else if (a == "--mix") mix = true;
        else if (a == "--blend-beats" && v) blendBeats = std::atof(argv[++i]);
        else if (a == "--dump-onsets" && v) {
            gDump = std::fopen(argv[++i], "w");
            if (gDump) std::fprintf(gDump, "id,phase,kickness,strength,t,kick,b0,b1,b2,b3,b4,b5,b6,b7,b8,n0,n1,n2,n3,n4,n5,n6,n7,n8,h0,h1,h2,h3,h4,h5,h6,h7,h8\n");
        } else if (a == "--dump-folds" && v) {
            gFolds = std::fopen(argv[++i], "wb");
        } else if (a == "--trace" && v) {
            gTrace = std::fopen(argv[++i], "w");
        } else if (a == "--oracle-tempo") prm.oracleTempo = true;
        else if (a == "--set" && v) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            if (eq == std::string::npos || !setParam(prm, kv.substr(0, eq), std::atof(kv.c_str() + eq + 1))) {
                std::fprintf(stderr, "unknown --set %s\n", kv.c_str());
                return 2;
            }
        } else manifest = a;
    }
    if (gTrace)
        std::fputs(mix ? "u,seg,truth_bpm,tempo,clock_bpm,locked,phase,filter_conf,kick,kick_grid_bpm,filter_phase\n"
                       : "t,truth_bpm,tempo,clock_bpm,locked,phase,lock,kick,kick_ratio,low_share,kick_phase,kick_phase_lock,acf_true,acf4_true,acf_23,acf4_23,acf_43,acf4_43\n",
                   gTrace);
    std::ifstream in(manifest);
    if (!in) {
        std::fprintf(stderr, "usage: reforce-beat-eval [--jobs N] [--max-seconds S] [--warmup S] [--set k=v] MANIFEST\n");
        return 2;
    }
    auto parseMarks = [](const std::string& marks, Song& s) {
        std::istringstream ms(marks);
        std::string m;
        while (std::getline(ms, m, ',')) {
            const auto c = m.find(':');
            if (c != std::string::npos) s.markers.emplace_back(std::atof(m.c_str()), std::atof(m.c_str() + c + 1));
        }
        s.id = jsonEscape(s.id);
        return s.markers.size() >= 2;
    };
    std::string line;
    if (mix) {
        std::vector<MixPair> pairs;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            MixPair mp;
            std::string ma, mb, ride;
            if (!std::getline(ls, mp.a.id, '\t') || !std::getline(ls, mp.a.path, '\t') || !std::getline(ls, ma, '\t') ||
                !std::getline(ls, mp.b.id, '\t') || !std::getline(ls, mp.b.path, '\t') || !std::getline(ls, mb, '\t') ||
                !std::getline(ls, ride))
                continue;
            if (!parseMarks(ma, mp.a) || !parseMarks(mb, mp.b)) continue;
            mp.ride = ride == "1";
            if (ride.rfind("nudge:", 0) == 0) {  // nudge:BPM:SECONDS[:AT]
                const auto c = ride.find(':', 6);
                mp.nudgeBpm = std::atof(ride.c_str() + 6);
                mp.nudgeSec = c == std::string::npos ? 0.0 : std::atof(ride.c_str() + c + 1);
                const auto c2 = c == std::string::npos ? c : ride.find(':', c + 1);
                if (c2 != std::string::npos) mp.nudgeAt = std::atof(ride.c_str() + c2 + 1);
            }
            pairs.push_back(std::move(mp));
        }
        std::vector<std::string> res(pairs.size());
        std::atomic<size_t> nx{0};
        std::vector<std::thread> workers;
        for (int j = 0; j < jobs; ++j)
            workers.emplace_back([&] {
                for (size_t i; (i = nx.fetch_add(1)) < pairs.size();) res[i] = evalMix(pairs[i], prm, blendBeats, warmup);
            });
        for (auto& th : workers) th.join();
        for (const auto& r : res) std::printf("%s\n", r.c_str());
        return 0;
    }
    std::vector<Song> songs;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        Song s;
        std::string marks;
        if (!std::getline(ls, s.id, '\t') || !std::getline(ls, s.path, '\t') || !std::getline(ls, marks)) continue;
        std::istringstream ms(marks);
        std::string m;
        while (std::getline(ms, m, ',')) {
            const auto c = m.find(':');
            if (c != std::string::npos) s.markers.emplace_back(std::atof(m.c_str()), std::atof(m.c_str() + c + 1));
        }
        if (s.markers.size() < 2) continue;
        s.id = jsonEscape(s.id);
        songs.push_back(std::move(s));
    }
    std::vector<std::string> results(songs.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int j = 0; j < jobs; ++j)
        pool.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < songs.size();) results[i] = evalSong(songs[i], int(i), prm, maxSeconds, warmup);
        });
    for (auto& th : pool) th.join();
    if (gDump) std::fclose(gDump);
    if (gFolds) std::fclose(gFolds);
    for (const auto& r : results) std::printf("%s\n", r.c_str());
    return 0;
}
