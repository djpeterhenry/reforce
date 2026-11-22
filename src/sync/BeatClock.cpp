#include "sync/BeatClock.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::sync {

const char* beatSourceName(BeatSource s) {
    switch (s) {
        case BeatSource::Link: return "link";
        case BeatSource::Audio: return "audio";
        default: return "none";
    }
}

void BeatClock::updateAudio(double dt, const BeatClockInput& in) {
    constexpr double kTwoPi = 6.283185307179586;
    if (in.audioTempo > 0) {
        aBpm_ = in.audioTempo;
        sinceTempo_ = 0;
        aStarted_ = true;
    } else {
        sinceTempo_ += dt;
    }
    if (!aStarted_ || sinceTempo_ > cfg_.holdSeconds) {
        aConfident_ = false;
        zRe_ = zIm_ = zW_ = 0;
        offGrid_ = 0;
        zK_ = 0;
        trim_ = 0;
        prevPulse_ = in.audioBeat;
        return;
    }
    if (!cfg_.steady || std::abs(aBpm_ / std::max(1e-9, trimBpm_) - 1.0) > 0.02) trim_ = 0;  // a new tempo
    trimBpm_ = aBpm_;
    if (cfg_.useFilter && in.filterPhase >= 0 &&
        updateFromPhase(dt, in, in.filterPhase, in.filterConf, cfg_.filterOn, cfg_.filterOff, cfg_.filterOffsetMs))
        return;
    if (cfg_.useKickPhase && in.kickPhase >= 0 &&
        updateFromPhase(dt, in, in.kickPhase, in.kickPhaseLock, cfg_.kickLockOn, cfg_.kickLockOff, cfg_.kickPhaseOffsetMs))
        return;
    double rescueD = 0.0;
    bool rescueHave = false;
    bool fromOnsets = false, fromFold = false;  // rescueD's sign: onset average (+ = we are early) vs a phase (+ = we are ahead)
    if (cfg_.rescueFromOnsets) {
        const double keep = std::exp(-dt / std::max(1e-3, cfg_.rescueMemorySeconds));
        rRe_ *= keep, rIm_ *= keep, rW_ *= keep;
    }
    if (cfg_.useFold && in.foldPhase >= 0 && in.foldConf >= cfg_.foldMinConf) {
        const double ours = aBeat_ + dt * aBpm_ * (1.0 + trim_) / 60.0;  // our count at the end of this step
        rescueD = (ours - std::floor(ours)) - in.foldPhase;
        rescueD -= std::round(rescueD);
        rescueHave = fromFold = true;
    } else if (cfg_.rescueFromOnsets) {
        if (rW_ > 1e-3 && std::hypot(rRe_, rIm_) > 0.2 * rW_) {
            rescueD = std::atan2(rIm_, rRe_) / 6.283185307179586;  // + = onsets come after our beats: we are early
            rescueHave = fromOnsets = true;
        }
    } else if (in.kickPhase >= 0 && in.kickPhaseLock >= cfg_.offbeatMinLock) {
        const double ours = aBeat_ - (cfg_.kickPhaseOffsetMs + cfg_.audioOffsetMs) / 1000.0 * aBpm_ / 60.0;
        rescueD = (ours - std::floor(ours)) - in.kickPhase;
        rescueD -= std::round(rescueD);
        rescueHave = true;
    }
    if (cfg_.steady && cfg_.offbeatRescue && aConfident_ && (in.audioKick || (fromFold && cfg_.foldWithoutKick)) &&
        rescueHave && shift_ == 0.0) {
        const double d = fromOnsets ? -rescueD : rescueD;  // d: how far we are ahead
        offbeatEv_ = std::abs(d) > (fromFold ? cfg_.foldPhaseOff : cfg_.offbeatPhase) ? offbeatEv_ + dt
                                                                                     : std::max(0.0, offbeatEv_ - dt);
        if (offbeatEv_ >= cfg_.offbeatSeconds) {
            shift_ = -d;  // slide onto the kicks; the onset average starts over
            ++rescues_;
            offbeatEv_ = 0;
            zRe_ = zIm_ = zW_ = zK_ = 0;
            rRe_ = rIm_ = rW_ = 0;
            offGrid_ = 0;
        }
    }
    // Steady and locked with no kick (a breakdown): run on at the held tempo, no corrections.
    const bool freewheel = cfg_.steady && aConfident_ && !in.audioKick;
    const double perSecond = aBpm_ * (1.0 + trim_) / 60.0;
    // Forget old onsets.
    const double keep = std::exp(-dt / cfg_.memorySeconds);
    zRe_ *= keep;
    zIm_ *= keep;
    zW_ *= keep;
    zK_ *= keep;
    // Onset: the BEAT pulse jumps up (1 at the onset, then exp(-age / decay)).
    const double pulse = in.audioBeat;
    if (!freewheel && pulse > 0.5 && pulse > prevPulse_ + 0.2) {
        const double age = -cfg_.pulseDecaySeconds * std::log(std::clamp(pulse, 1e-3, 1.0));
        // Our count when the onset happened in the music (detection latency: audioOffsetMs).
        // `age` is measured from the end of this step's audio (now), so start from the count at the end of this
        // step too (aBeat_ is still the previous step's: using it put every beat one step early).
        const double at = aBeat_ + dt * perSecond - (age + cfg_.audioOffsetMs / 1000.0) * perSecond;
        const double w = std::clamp(in.audioStrength, 0.25, 8.0);
        double w2 = w * w;  // strong onsets (kicks) dominate weak ones (hats, off-beat bass)
        bool use = true;
        if (cfg_.rescueFromOnsets) {  // ungated, on-beat weighted
            const double pb = std::clamp(kickLike(in), 0.0, 1.0);
            const double rw = std::pow(pb, cfg_.rescuePower) * w;
            rRe_ += rw * std::cos(kTwoPi * at), rIm_ += rw * std::sin(kTwoPi * at), rW_ += rw;
        }
        if (cfg_.steady) {
            const double share = std::clamp(kickLike(in), 0.0, 1.0);
            const double f = cfg_.lowFloor + (1.0 - cfg_.lowFloor) * std::min(1.0, share / cfg_.kickShare);
            w2 *= f * f;
            if (aConfident_) {
                const bool kick = share >= cfg_.kickShare;
                if (std::abs(at - std::round(at)) >= cfg_.gatePhase) {
                    use = false;
                    if (kick && ++offGrid_ >= cfg_.escapeKicks) {  // the grid moved: start over
                        aConfident_ = false;
                        zRe_ = zIm_ = zW_ = zK_ = 0;
                        offGrid_ = 0;
                    }
                } else if (kick) {
                    offGrid_ = 0;
                    // Learn the exact tempo from kicks only: each one's offset from our beat, one beat's worth.
                    const double ph = at - std::round(at);  // + = the kick came after our beat: we run fast
                    if (std::abs(ph) < cfg_.trimPhase)  // fine-tuning only, not while (re)locking
                        trim_ = std::clamp(trim_ - ph * (1.0 / perSecond) / std::max(1e-3, cfg_.trimSeconds),
                                       -cfg_.maxTrim, cfg_.maxTrim);
                }
            }
        }
        if (use) {
            zRe_ += w2 * std::cos(kTwoPi * at);
            zIm_ += w2 * std::sin(kTwoPi * at);
            zW_ += w2;
            if (kickLike(in) >= cfg_.kickShare) zK_ += w2;
        }
    }
    prevPulse_ = pulse;
    const double len = std::hypot(zRe_, zIm_);
    const double lock = zW_ > 1e-9 ? len / zW_ : 0.0;
    // Where onsets fall on the count's beat circle (-0.5..0.5; 0 = on whole beats); slew the count to cancel it.
    double step = dt * perSecond;
    if (len > 1e-9 && !freewheel) {
        const double phase = std::atan2(zIm_, zRe_) / kTwoPi;  // + = onsets come after our beats: we are early
        double slew = cfg_.slewSeconds;
        if (cfg_.steady && aConfident_ && zW_ > 1e-9)
            slew = std::min(cfg_.maxSlewSeconds, slew / std::max(1e-3, zK_ / zW_));
        double c = -phase * std::min(1.0, dt / slew);
        c = std::clamp(c, -0.5 * step, 0.5 * step);  // never jump, never run backwards
        step += c;
        // Shifting the count by c moves every remembered onset's phase by c too (as if the count had always been so).
        const double cr = std::cos(kTwoPi * c), ci = std::sin(kTwoPi * c);
        const double re = zRe_ * cr - zIm_ * ci;
        zIm_ = zRe_ * ci + zIm_ * cr;
        zRe_ = re;
    }
    if (shift_ != 0.0) {  // off-beat rescue slide, at most half a step per step (never backwards)
        const double c = std::clamp(shift_, -0.5 * step, 0.5 * step);
        step += c;
        shift_ -= c;
        if (std::abs(shift_) < 1e-9) shift_ = 0.0;
    }
    aBeat_ += step;
    const bool enough = zW_ >= cfg_.minWeight;
    if (aConfident_ && cfg_.steady) {
        // Locked: stays so while the tempo is held (the early return above) unless onsets clearly disagree.
        if (enough && lock < cfg_.lockOff) aConfident_ = false;
    } else if (aConfident_) aConfident_ = enough && lock >= cfg_.lockOff;
    else aConfident_ = enough && lock >= cfg_.lockOn;
    lock_ = lock;
}

double BeatClock::kickLike(const BeatClockInput& in) const {
    if (cfg_.useBeatness && in.audioBeatness >= 0) {
        if (cfg_.beatnessKickMix > 0 && in.audioKickness >= 0)
            return std::pow(in.audioBeatness, 1.0 - cfg_.beatnessKickMix) *
                   std::pow(std::max(in.audioKickness, 1e-3), cfg_.beatnessKickMix);
        return in.audioBeatness;
    }
    if (cfg_.useKickness && in.audioKickness >= 0) return in.audioKickness;
    return in.audioLowShare;
}

bool BeatClock::updateFromPhase(double dt, const BeatClockInput& in, double phase, double kpl, double lockOn,
                                double lockOff, double offsetMs) {
    const double perSecond = aBpm_ * (1.0 + trim_) / 60.0;
    const bool usable = kpl >= (aConfident_ ? lockOff : lockOn) && (in.audioKick || !cfg_.steady);
    const bool freewheel = cfg_.steady && aConfident_ && !in.audioKick;
    if (!usable && !freewheel) {
        if (aConfident_ && cfg_.steady && in.audioKick && kpl < lockOff) aConfident_ = false;
        return false;  // the onset path
    }
    double step = dt * perSecond;
    if (usable) {
        // Our count at the end of this step, as a beat fraction, against the kick band's (+ = we are ahead).
        const double ours = aBeat_ + step - (offsetMs + cfg_.audioOffsetMs) / 1000.0 * perSecond;
        double err = (ours - std::floor(ours)) - phase;
        err -= std::round(err);
        if (cfg_.steady && aConfident_ && std::abs(err) < cfg_.trimPhase)
            trim_ = std::clamp(trim_ - err * dt / std::max(1e-3, cfg_.trimSeconds), -cfg_.maxTrim, cfg_.maxTrim);
        double c = -err * std::min(1.0, dt / cfg_.slewSeconds);
        c = std::clamp(c, -0.5 * step, 0.5 * step);
        step += c;
        aConfident_ = true;
        lock_ = kpl;
    }
    aBeat_ += step;
    prevPulse_ = in.audioBeat;
    return true;
}

const BeatClockState& BeatClock::update(double dt, const BeatClockInput& in) {
    dt = std::max(0.0, dt);
    const std::string& src = cfg_.source;
    const bool useLink = (src == "auto" || src == "link") && in.linkOn && in.linkBpm > 0;
    const bool audioAllowed = src == "auto" || src == "audio";
    if (audioAllowed) updateAudio(dt, in);  // keep the PLL warm even while Link leads
    BeatClockState s;
    if (useLink) {
        s.source = BeatSource::Link;
        s.confident = true;
        s.bpm = in.linkBpm;
        s.beat = in.linkBeat;
        s.bars = true;
        s.quantum = std::max(1.0, in.linkQuantum);
        s.lock = 1.0;
    } else if (audioAllowed && aStarted_ && sinceTempo_ <= cfg_.holdSeconds) {
        s.source = BeatSource::Audio;
        s.confident = aConfident_;
        s.bpm = aBpm_ * (1.0 + trim_);
        s.beat = aBeat_ + cfg_.outputOffsetMs / 1000.0 * s.bpm / 60.0;
        s.lock = lock_;
    }
    state_ = s;
    return state_;
}

}  // namespace reforce::sync
