// Arcade organic `invaders` (PLAN 5Q.1, docs/arcade-design.md): a formation of our own pixel aliens marching one step
// per beat, stepping down a row at the edge and reversing; a cannon below slides under a target and fires on 8th-note
// lines; hit aliens burst into their own pixels (debris that trails in the FlowField). A saucer crosses the top once
// per phrase and is shot down on a bar line; waves start on Wave-bar lines or after the formation is cleared; a drop
// (SYNC_DROP rising) explodes every alien within a beat. Nobody shoots back, nobody loses: a formation that reaches
// the cannon's row beams back up to the top.
//
// Everything happens on a 16th-note tick grid (4 ticks per beat) so it lands on the music: a step catches up the
// ticks since the last frame (at most kMaxCatchUp; a bigger jump skips ahead and starts a fresh wave). Moving things
// (shots, the saucer, debris, the cannon's slide) are drawn from closed-form positions in beat time, so the picture
// is the same at any frame rate. The state only advances on ticks and draws from the seeded RNG in tick order, so a
// run is deterministic for a given seed and clock.
#include <array>
#include <cstdio>
#include <vector>

#include "engine/arcade/Arcade.hpp"
#include "expr/Rng.hpp"

namespace reforce::engine::arcade {

namespace {

constexpr int kTicksPerBeat = 4;
constexpr int kTicksPerBar = 16;
constexpr int kPhraseBars = 16;           // the saucer comes once per phrase
constexpr std::int64_t kMaxCatchUp = 64;  // ticks a frame may advance (4 bars); a bigger jump skips ahead
constexpr std::size_t kMaxDebris = 2400;  // bounds the segments: one dot each
constexpr std::size_t kMaxShots = 32;
constexpr int kSweepMoves = 15;   // sideways moves per sweep; with the step down a sweep is 16 steps (4 bars at Rate 1)
constexpr int kEnterSteps = 8;    // a new wave descends in 8 8th-note steps (one bar)
constexpr int kBeamUpTicks = 4;   // a formation at the cannon's row beams back up over one beat
constexpr int kCascadeTicks = 4;  // a drop explodes the whole formation within one beat
constexpr double kXLim169 = 1.62;  // the play field's half width at 16:9 (inside its 1.78); scaled to the frame
constexpr double kTopY = 0.64;    // the formation's top row at the start of a wave
constexpr double kCannonY = -0.86;
constexpr double kSaucerY = 0.86;
constexpr double kShotSpeed = 7.0;  // tree units per beat (about a beat from the cannon to the saucer)

// ---- our own pixel art (11 x 8 aliens, two frames each), drawn with OrganicCanvas::bitmap

// Gazer: a one-eyed jelly whose tentacles ripple; the pupil looks the way the march goes (mirrored).
const char* const kGazer[2][8] = {
    {"...XXXXX...", ".XXXXXXXXX.", "XXX.....XXX", "XXX.XX..XXX", "XXX.....XXX", ".XXXXXXXXX.", ".X.X.X.X.X.",
     "X.X.X.X.X.X"},
    {"...XXXXX...", ".XXXXXXXXX.", "XXX.....XXX", "XXX.XX..XXX", "XXX.....XXX", ".XXXXXXXXX.", ".X.X.X.X.X.",
     ".X.X.X.X.X."},
};
// Moth: wings up, wings down.
const char* const kMoth[2][8] = {
    {"X...X.X...X", "XX...X...XX", "XXX.XXX.XXX", "XXXXXXXXXXX", ".XXXXXXXXX.", "..XX.X.XX..", ".XX..X..XX.",
     ".X.......X."},
    {"....X.X....", ".....X.....", "...XXXXX...", "XXXXXXXXXXX", "XXXXXXXXXXX", "XXX.XXX.XXX", "XX...X...XX",
     "X.........X"},
};
// Boxbot: a square robot with a grille mouth; antennae and legs shuffle.
const char* const kBoxbot[2][8] = {
    {"..X.....X..", ".XXXXXXXXX.", "XX..XXX..XX", "XXXXXXXXXXX", "XX.X.X.X.XX", ".XXXXXXXXX.", ".X.X...X.X.",
     "XX.......XX"},
    {".X.......X.", ".XXXXXXXXX.", "XX..XXX..XX", "XXXXXXXXXXX", "XX.X.X.X.XX", ".XXXXXXXXX.", "..XX...XX..",
     "..X.....X.."},
};
const char* const* const kSpecies[3][2] = {{kGazer[0], kGazer[1]}, {kMoth[0], kMoth[1]}, {kBoxbot[0], kBoxbot[1]}};
constexpr int kAlienW = 11, kAlienH = 8;

// Turret: a domed cannon on treads; the treads roll as it slides.
const char* const kTurret[2][7] = {
    {"......X......", ".....XXX.....", "....XX.XX....", "..XXXXXXXXX..", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
     "X.X.X.X.X.X.X"},
    {"......X......", ".....XXX.....", "....XX.XX....", "..XXXXXXXXX..", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
     ".X.X.X.X.X.X."},
};
constexpr int kTurretH = 7;

// Halo craft (the saucer): a hollow dome over a hull whose lights chase (row 4 is built per frame).
constexpr int kSaucerW = 15, kSaucerH = 7;
const char* const kSaucerRows[kSaucerH] = {".......X.......", "......XXX......", ".....XX.XX.....", ".XXXXXXXXXXXXX.",
                                           nullptr,           ".XXXXXXXXXXXXX.", "...X.......X..."};

// Pop: the flash where a shot lands.
const char* const kPop[7] = {"X...X...X", ".X..X..X.", "...X.X...", "XXX...XXX", "...X.X...", ".X..X..X.", "X...X...X"};

class Invaders : public OrganicBody {
public:
    Invaders(scene::OrganicInstance& in, std::uint64_t seed)
        : inst_(in), cols_(keyCount(in, "COLS", 11, 1, 24)), rows_(keyCount(in, "ROWS", 5, 1, 8)),
          rate_(keyNum(in, "RATE", 1, 0.25, 4)), fire_(keyNum(in, "FIRE", 2, 0, 4)),
          wave_(keyNum(in, "WAVE", 16, 0, 256)), aim_(keyNum(in, "AIM", 0.7, 0, 1)),
          saucerOn_(keyNum(in, "SAUC", 1, 0, 1) >= 0.5), rng_(seed) {
        if (wave_ > 0) wave_ = std::max(1.0, wave_);  // at least a bar: a wave needs its bar to descend
        pixWant_ = keyNum(in, "PIX", 0.0115, 0.002, 0.05);
        rowShade_ = keyNum(in, "RSHD", 0, 0, 0.45);
        fit(16.0 / 9.0);
        fireTicks_ = fire_ > 0 ? std::clamp(int(std::lround(kTicksPerBeat / fire_)), 1, 64) : 0;
        alive_.assign(std::size_t(cols_ * rows_), 0);
        doomed_.assign(alive_.size(), 0);
        blowAt_.assign(alive_.size(), -1);
    }

    // The march's sideways step: the formation crosses the field (scaled to the frame's half width) in a sweep.
    // The formation fits the field: at most 2.6 units wide at 16:9 (scaled to the frame) and .9 tall (16 x 14 pixel
    // cells).
    void fit(double halfW) {
        fitW_ = halfW;
        const double k = halfW / (16.0 / 9.0);
        xLim_ = kXLim169 * k;
        px_ = std::min({pixWant_, 2.6 * k / (16.0 * (cols_ - 1) + kAlienW), 0.9 / (14.0 * (rows_ - 1) + kAlienH)});
        colSp_ = 16 * px_;
        rowSp_ = 14 * px_;
        dy_ = 8 * px_;
        const double width = colSp_ * (cols_ - 1) + kAlienW * px_;
        dx_ = std::max(0.0, 2 * xLim_ - width) / kSweepMoves;
    }
    double fitW_ = 0, xLim_ = kXLim169, pixWant_ = 0.0115, rowShade_ = 0;

    void step(const OrganicStep& s, OrganicCanvas& c) override {
        if (const double hw = std::clamp(c.halfWidth(), 0.25, 8.0); hw != fitW_) fit(hw);  // the frame's aspect
        const double beat = std::clamp(finiteOr(s.beat, 0), -1e12, 1e12);
        const double bar = finiteOr(s.bar, beat / 4);
        barOff_ = bar - beat / 4;
        const std::int64_t now = std::int64_t(std::floor(beat * kTicksPerBeat));
        const double build = buildOf(s), drop = dropOf(s);
        speed_ = build >= 2.0 / 3.0 ? 4 : build >= 1.0 / 3.0 ? 2 : 1;

        if (!started_ || now < tick_ - kTicksPerBar) {  // the first frame, or the clock jumped back a bar or more
            start(now);
        } else {
            if (now - tick_ > kMaxCatchUp) start(now - kMaxCatchUp);  // a long jump: play only the last 4 bars
            // A drop (SYNC_DROP rising through .5) sets off the cascade from the next tick.
            if (drop >= 0.5 && lastDrop_ < 0.5) cascade(now + 1);
            while (tick_ < now) advance(++tick_);
        }
        lastDrop_ = drop;
        draw(beat, c, s.pen);
    }

    std::string summary() const override {
        // The log prints it at the start and the end: the tally of the run once it has played.
        char b[192];
        int n = std::snprintf(b, sizeof b, "%d x %d invaders, %g step(s) and %g shot(s) per beat", cols_, rows_, rate_,
                              fire_);
        if (started_ && n > 0 && n < int(sizeof b))
            std::snprintf(b + n, sizeof b - std::size_t(n),
                          "; waves %d, hits %d, saucers %d, beam-ups %d, drops %d, left %d", waveId_, hits_, saucers_,
                          beamUps_, dropsSeen_, live_);
        return b;
    }

private:
    enum class Phase { Enter, March, BeamUp, Cascade, Cleared };
    struct Shot {
        double x, y0, b0, v;   // from (x, y0) at beat b0, rising v units per beat
        std::int64_t hitTick;  // -1: a miss, flies off the top
        int target, wave;      // alien index (or -2 = the saucer), the wave it was aimed in
        double yEnd;
    };
    struct Bit {  // a piece of debris
        float x, y, vx, vy, b0, life, pen, size;
    };
    struct Pop {
        double x, y, b0, pen;
    };

    // ---- state machine on the tick grid

    double barAt(std::int64_t k) const { return double(k) / kTicksPerBar + barOff_; }

    void start(std::int64_t now) {
        started_ = true;
        tick_ = now;
        shots_.clear();
        debris_.clear();
        pops_.clear();
        saucerHit_ = -1;
        lastWaveIdx_ = wave_ > 0 ? std::int64_t(std::floor(barAt(now) / wave_)) : 0;
        lastPhrase_ = std::int64_t(std::floor(barAt(now) / kPhraseBars));
        newWave();
        schedulePhraseSaucer(lastPhrase_, now);
        camFromTick_ = camToTick_ = now;
        camFromX_ = camToX_ = 0;
    }

    void newWave() {
        ++waveId_;
        std::fill(alive_.begin(), alive_.end(), 1);
        std::fill(doomed_.begin(), doomed_.end(), 0);
        std::fill(blowAt_.begin(), blowAt_.end(), -1);
        live_ = cols_ * rows_;
        dir_ = rng_.uniform01() < 0.5 ? 1 : -1;
        ix_ = dir_ > 0 ? 0 : kSweepMoves;
        drops_ = 0;
        phase_ = Phase::Enter;
        enterLeft_ = kEnterSteps;
        lift_ = 0;
        planned_ = false;
    }

    // Remaining aliens dissolve upwards (on a wave line, before the next wave descends).
    void warpOut(std::int64_t k) {
        for (int i = 0; i < rows_ * cols_; ++i)
            if (alive_[std::size_t(i)]) burst(i, double(k) / kTicksPerBeat, 4, true);
    }

    void cascade(std::int64_t k) {
        if (phase_ == Phase::Cleared || live_ == 0) return;
        ++dropsSeen_;
        // Outwards from the alien nearest the cannon, all within kCascadeTicks.
        const double cx = camToX_, cy = kCannonY;
        auto dist = [&](int i) { return std::hypot(alienX(i % cols_) - cx, alienY(i / cols_) - cy); };
        double near = 1e300, far = 0;
        for (int i = 0; i < rows_ * cols_; ++i)
            if (alive_[std::size_t(i)]) {
                near = std::min(near, dist(i));
                far = std::max(far, dist(i));
            }
        for (int i = 0; i < rows_ * cols_; ++i) {
            if (!alive_[std::size_t(i)]) continue;
            const double u = far > near ? (dist(i) - near) / (far - near) : 0;
            blowAt_[std::size_t(i)] = k + std::lround(u * kCascadeTicks);
        }
        phase_ = Phase::Cascade;
    }

    void advance(std::int64_t k) {
        const double b = double(k) / kTicksPerBeat;
        const bool barLine = std::floor(barAt(k)) != std::floor(barAt(k - 1));

        // Phrase lines: the saucer's schedule. Wave lines: a fresh wave descends (what is left warps out).
        const std::int64_t phrase = std::int64_t(std::floor(barAt(k) / kPhraseBars));
        if (phrase != lastPhrase_) {
            lastPhrase_ = phrase;
            schedulePhraseSaucer(phrase, k);
        }
        if (wave_ > 0) {
            const std::int64_t w = std::int64_t(std::floor(barAt(k) / wave_));
            if (w != lastWaveIdx_) {
                lastWaveIdx_ = w;
                if (phase_ != Phase::Cascade) {
                    warpOut(k);
                    newWave();
                }
            }
        }

        // The drop cascade.
        if (phase_ == Phase::Cascade) {
            int pending = 0;
            for (int i = 0; i < rows_ * cols_; ++i) {
                if (!alive_[std::size_t(i)]) continue;
                if (blowAt_[std::size_t(i)] <= k) kill(i, b, 26);
                else ++pending;
            }
            if (pending == 0) phase_ = Phase::Cleared;
        }

        // Shots landing on this tick.
        for (auto& sh : shots_) {
            if (sh.hitTick != k) continue;
            if (sh.target == -2) {
                saucerBurst(b);
            } else if (sh.wave == waveId_ && sh.target >= 0 && alive_[std::size_t(sh.target)]) {
                kill(sh.target, b, 18);
            }
        }
        shots_.erase(std::remove_if(shots_.begin(), shots_.end(),
                                    [&](const Shot& sh) {
                                        return (sh.hitTick >= 0 && sh.hitTick <= k) ||
                                               (sh.hitTick < 0 && sh.y0 + sh.v * (b - sh.b0) > 1.2);
                                    }),
                     shots_.end());

        // The formation.
        switch (phase_) {
            case Phase::Enter:
                if (k % 2 == 0 && --enterLeft_ <= 0) phase_ = Phase::March;
                if (k % kTicksPerBeat == 0) frame_ ^= 1;
                break;
            case Phase::March:
                if (k % stepTicks() == 0) march();
                break;
            case Phase::BeamUp:
                if (--beamLeft_ <= 0) {
                    phase_ = Phase::March;
                    drops_ = 0;
                    lift_ = 0;
                } else {
                    lift_ = drops_ * dy_ * double(kBeamUpTicks - beamLeft_) / kBeamUpTicks;
                }
                break;
            case Phase::Cascade: break;
            case Phase::Cleared:
                if (barLine) newWave();
                break;
        }
        if (phase_ == Phase::March && live_ == 0 && shotsInFlight() == 0) phase_ = Phase::Cleared;

        // The cannon: the saucer shot first, then the regular 8ths.
        if (saucerHit_ >= 0) {
            const std::int64_t fireAt = saucerHit_ - saucerFlight_;
            if (k == fireAt - kTicksPerBeat) {
                moveCannon(k, saucerHitX_, fireAt);
                planned_ = false;
            }
            if (k == fireAt) fireAtSaucer(b);
        }
        const bool saucerBusy = saucerHit_ >= 0 && k >= saucerFireTick_ - kTicksPerBeat && k <= saucerFireTick_;
        if (fireTicks_ > 0 && k % fireTicks_ == 0 && !saucerBusy) {
            if (phase_ == Phase::March) {
                if (planned_ && camToTick_ <= k) fireRegular(k, b);
                plan(k);
            } else {
                planned_ = false;
            }
        }
        if (saucerHit_ >= 0 && k >= saucerHit_) saucerHit_ = -1;
    }

    int stepTicks() const {
        // Faster under a build (8ths, then 16ths); the last eighth of the formation hurries, like the classic.
        double mult = speed_;
        if (live_ > 0 && live_ * 8 <= cols_ * rows_) mult *= 2;
        return std::clamp(int(std::lround(kTicksPerBeat / (rate_ * mult))), 1, 64);
    }

    void march() {
        frame_ ^= 1;
        const int next = ix_ + dir_;
        if (next < 0 || next > kSweepMoves) {
            dir_ = -dir_;
            ++drops_;
            if (bottomEdge() <= kCannonY + 0.14) {  // at the cannon's row: no invasion, it beams back to the top
                phase_ = Phase::BeamUp;
                beamLeft_ = kBeamUpTicks;
                ++beamUps_;
                lift_ = 0;
            }
        } else {
            ix_ = next;
        }
    }

    // ---- geometry

    double formationX() const { return -xLim_ + 0.5 * (colSp_ * (cols_ - 1) + kAlienW * px_) + ix_ * dx_; }
    double alienX(int col) const { return formationX() + (col - 0.5 * (cols_ - 1)) * colSp_; }
    double topY() const {
        const double enter = phase_ == Phase::Enter ? enterLeft_ * (1.3 / kEnterSteps) : 0;
        return kTopY - drops_ * dy_ + lift_ + enter;
    }
    double alienY(int row) const { return topY() - row * rowSp_; }
    double bottomEdge() const {
        for (int r = rows_ - 1; r >= 0; --r)
            for (int q = 0; q < cols_; ++q)
                if (alive_[std::size_t(r * cols_ + q)]) return alienY(r) - 0.5 * kAlienH * px_;
        return 1;
    }
    int species(int row) const { return row == 0 ? 0 : row <= (rows_ - 1) / 2 ? 1 : 2; }

    // ---- the cannon

    double cannonX(double b) const {
        const double b0 = double(camFromTick_) / kTicksPerBeat, b1 = double(camToTick_) / kTicksPerBeat;
        if (b >= b1 || b1 <= b0) return camToX_;
        if (b <= b0) return camFromX_;
        const double u = (b - b0) / (b1 - b0);
        return camFromX_ + (camToX_ - camFromX_) * u * u * (3 - 2 * u);
    }
    // Slides from wherever it is at tick k (leaving then) to x, arriving at tick `arrive`.
    void moveCannon(std::int64_t k, double x, std::int64_t arrive) {
        const double from = cannonX(double(k) / kTicksPerBeat);
        camFromTick_ = k;
        camFromX_ = from;
        camToTick_ = std::max(arrive, k + 1);
        camToX_ = std::clamp(x, -xLim_, xLim_);
    }
    int shotsInFlight() const {
        int n = 0;
        for (const auto& sh : shots_) n += sh.target >= 0 && sh.wave == waveId_ ? 1 : 0;
        return n;
    }
    int lowestOpen(int col) const {
        for (int r = rows_ - 1; r >= 0; --r) {
            const std::size_t i = std::size_t(r * cols_ + col);
            if (alive_[i]) return doomed_[i] ? -1 : int(i);  // the lowest alien shields those above it
        }
        return -1;
    }
    // Where column `col` will be `ahead` ticks from k if the march goes on as now.
    double predictX(int col, std::int64_t k, std::int64_t ahead) const {
        int ix = ix_, dir = dir_;
        const int p = stepTicks();
        for (std::int64_t t = k + 1; t <= k + ahead && phase_ == Phase::March; ++t) {
            if (t % p) continue;
            const int next = ix + dir;
            if (next < 0 || next > kSweepMoves) dir = -dir;
            else ix = next;
        }
        return alienX(col) + (ix - ix_) * dx_;
    }
    // Picks the next target (the nearer of two open columns, so it does not zip across the field every shot) and
    // slides the cannon under it: it lingers after a shot for half the gap, then arrives on the next shot's tick.
    void plan(std::int64_t k) {
        std::array<int, 24> open{};
        int n = 0;
        for (int q = 0; q < cols_; ++q)
            if (lowestOpen(q) >= 0) open[std::size_t(n++)] = q;
        planned_ = n > 0;
        if (!planned_) return;
        const int a = open[std::size_t(std::min(n - 1, int(rng_.uniform01() * n)))];
        const int b = open[std::size_t(std::min(n - 1, int(rng_.uniform01() * n)))];
        planCol_ = std::abs(alienX(a) - camToX_) <= std::abs(alienX(b) - camToX_) ? a : b;
        planMiss_ = rng_.uniform01() >= aim_;
        const std::int64_t at = k + fireTicks_;
        const int target = lowestOpen(planCol_);
        const double y = alienY(target / cols_);
        const std::int64_t flight = flightTicks(y);
        double x = predictX(planCol_, k, fireTicks_ + flight);
        if (planMiss_) x += (rng_.uniform01() < 0.5 ? -0.5 : 0.5) * colSp_;  // through a gap
        moveCannon(k + fireTicks_ / 2, x, at);
    }
    std::int64_t flightTicks(double y) const {
        const double dist = std::max(0.0, y - (kCannonY + kTurretH * px_));
        return std::max<std::int64_t>(1, std::int64_t(std::ceil(dist / kShotSpeed * kTicksPerBeat)));
    }
    void fireRegular(std::int64_t k, double b) {
        if (shots_.size() >= kMaxShots) return;
        const double x = cannonX(b), y0 = kCannonY + kTurretH * px_ * 0.5;
        const int target = planMiss_ ? -1 : lowestOpen(planCol_);
        lastFire_ = b;
        if (target < 0) {
            shots_.push_back({x, y0, b, kShotSpeed, -1, -1, waveId_, 2});
            return;
        }
        const double yT = alienY(target / cols_) - 0.5 * kAlienH * px_;
        const std::int64_t flight = flightTicks(yT + 0.5 * kAlienH * px_);
        const double v = (yT - y0) / (double(flight) / kTicksPerBeat);
        doomed_[std::size_t(target)] = 1;
        shots_.push_back({x, y0, b, v, k + flight, target, waveId_, yT});
    }

    // ---- the saucer: crosses the top in the phrase's bars 10-12 and is shot down on bar 12's line

    void schedulePhraseSaucer(std::int64_t phrase, std::int64_t k) {
        if (!saucerOn_) return;
        const double hitBar = double(phrase * kPhraseBars + 12);
        const std::int64_t hit = std::int64_t(std::ceil((hitBar - barOff_) * kTicksPerBar - 1e-6));
        saucerFlight_ = flightTicks(kSaucerY);
        if (hit - saucerFlight_ - kTicksPerBeat <= k) return;  // too late in this phrase
        saucerHit_ = hit;
        saucerFireTick_ = hit - saucerFlight_;
        saucerHitX_ = (rng_.uniform01() * 2 - 1) * 1.0;
        saucerDir_ = rng_.uniform01() < 0.5 ? 1 : -1;
        saucerStart_ = double(hit - 2 * kTicksPerBar) / kTicksPerBeat;
    }
    double saucerX(double b) const {
        const double hitB = double(saucerHit_) / kTicksPerBeat;
        const double x0 = -saucerDir_ * 2.1;
        const double v = (saucerHitX_ - x0) / (hitB - saucerStart_);
        return saucerHitX_ + v * (b - hitB);
    }
    void fireAtSaucer(double b) {
        const double y0 = kCannonY + kTurretH * px_ * 0.5;
        const double yT = kSaucerY - 0.5 * kSaucerH * px_;
        const double v = (yT - y0) / (double(saucerFlight_) / kTicksPerBeat);
        shots_.push_back({saucerHitX_, y0, b, v, saucerHit_, -2, waveId_, yT});
        lastFire_ = b;
    }
    void saucerBurst(double b) {
        const double x = saucerHitX_, y = kSaucerY;
        // Its own pixels, then a ring of sparks.
        for (int i = 0; i < 70; ++i) {
            const double ox = (rng_.uniform01() - 0.5) * kSaucerW * px_, oy = (rng_.uniform01() - 0.5) * kSaucerH * px_;
            const double a = std::atan2(oy, ox), sp = 0.25 + 0.9 * rng_.uniform01();
            addBit(x + ox, y + oy, sp * std::cos(a), sp * std::sin(a) - 0.1, b, 2.5 + 2 * rng_.uniform01(), 1.0, px_);
        }
        for (int i = 0; i < 40; ++i) {
            const double a = 2 * 3.141592653589793 * (i + 0.5 * rng_.uniform01()) / 40;
            addBit(x, y, 1.1 * std::cos(a), 1.1 * std::sin(a), b, 2.0, 1.0, 0.8 * px_);
        }
        pops_.push_back({x, y, b, 1.0});
        saucerHit_ = -1;
        ++saucers_;
    }

    // ---- hits and debris

    void kill(int i, double b, int bits) {
        alive_[std::size_t(i)] = 0;
        doomed_[std::size_t(i)] = 0;
        --live_;
        ++hits_;
        burst(i, b, bits, false);
        pops_.push_back({alienX(i % cols_), alienY(i / cols_), b, speciesPen(species(i / cols_))});
    }
    // The alien falls apart into its own lit pixels, flying out from its middle (or drifting up when warping out).
    void burst(int i, double b, int bits, bool up) {
        const int row = i / cols_, sp = species(row);
        const char* const* art = kSpecies[sp][frame_];
        const double cx = alienX(i % cols_), cy = alienY(row);
        for (int n = 0; n < bits; ++n) {
            int r = 0, q = 0;
            for (int tries = 0; tries < 8; ++tries) {
                r = std::min(kAlienH - 1, int(rng_.uniform01() * kAlienH));
                q = std::min(kAlienW - 1, int(rng_.uniform01() * kAlienW));
                if (art[r][q] != '.') break;
            }
            const double ox = (q - 0.5 * (kAlienW - 1)) * px_, oy = (0.5 * (kAlienH - 1) - r) * px_;
            const double sp0 = up ? 0.0 : 0.35 + 0.8 * rng_.uniform01();
            const double len = std::max(1e-6, std::hypot(ox, oy));
            const double vx = up ? (rng_.uniform01() - 0.5) * 0.08 : sp0 * ox / len + (rng_.uniform01() - 0.5) * 0.2;
            const double vy = up ? 0.6 + 0.6 * rng_.uniform01() : sp0 * oy / len + 0.15;
            addBit(cx + ox, cy + oy, vx, vy, b, up ? 1.5 : 1.5 + 1.5 * rng_.uniform01(),
                   up ? 0.6 * speciesPen(sp) : speciesPen(sp), px_);
        }
    }
    void addBit(double x, double y, double vx, double vy, double b0, double life, double pen, double size) {
        if (debris_.size() >= kMaxDebris) debris_.erase(debris_.begin(), debris_.begin() + 64);
        debris_.push_back({float(x), float(y), float(vx), float(vy), float(b0), float(life), float(pen), float(size)});
    }

    // All rows at the same intensity by default (Peter 2026-11-22: dimmer lower rows vanished under some ColorMaps and
    // FlowFields); RShd darkens each species below the top one by that much (.12 was the first look).
    double speciesPen(int sp) const { return std::max(0.0, 1.0 - rowShade_ * sp); }

    // ---- drawing (closed-form in beat time)

    void draw(double beat, OrganicCanvas& c, double pen) {
        const double aln = std::clamp(frameNum(inst_, "ALN", 1.0), 0.0, 1.0) * pen;
        // The cannon in the aliens' color by default (Peter 2026-11-22: it was hard to see at .62).
        const double can = std::clamp(frameNum(inst_, "CANN", 1.0), 0.0, 1.0) * pen;
        const double sht = std::clamp(frameNum(inst_, "SHOT", 1.0), 0.0, 1.0) * pen;
        const double deb = std::clamp(frameNum(inst_, "DEBR", 1.0), 0.0, 1.0) * pen;

        // Debris first (drag slows it, a little gravity pulls it down), fading out.
        constexpr double kDrag = 1.3, kGravity = 0.25;
        debris_.erase(std::remove_if(debris_.begin(), debris_.end(),
                                     [&](const Bit& d) { return beat - d.b0 > d.life; }),
                      debris_.end());
        for (const Bit& d : debris_) {
            const double a = beat - d.b0;
            if (a < 0) continue;
            const double k = (1 - std::exp(-kDrag * a)) / kDrag;
            const double x = d.x + d.vx * k, y = d.y + d.vy * k - 0.5 * kGravity * a * a;
            const double f = 1 - a / d.life;
            c.dotUnits(x, y, d.size * (0.6 + 0.6 * f), deb * d.pen * f * std::sqrt(f));
        }

        // The formation (the gazers look the way it marches).
        if (phase_ != Phase::Cleared) {
            for (int r = 0; r < rows_; ++r) {
                const int sp = species(r);
                const double y = alienY(r);
                if (y - 0.5 * kAlienH * px_ > 1.05) continue;  // still above the frame while descending
                const double p = aln * speciesPen(sp);
                for (int q = 0; q < cols_; ++q)
                    if (alive_[std::size_t(r * cols_ + q)])
                        c.bitmap(kSpecies[sp][frame_], kAlienH, alienX(q), y, px_, p, sp == 0 && dir_ > 0);
            }
            if (phase_ == Phase::BeamUp)  // the beams pulling it back up
                for (int q = 0; q < cols_; ++q) {
                    for (int r = rows_ - 1; r >= 0; --r)
                        if (alive_[std::size_t(r * cols_ + q)]) {
                            const double y = alienY(r) + 0.5 * kAlienH * px_;
                            c.line(alienX(q), y, alienX(q), 1.05, 3.0, 0.35 * aln, 0);
                            break;
                        }
                }
        }

        // Pops where shots landed (half a beat).
        pops_.erase(std::remove_if(pops_.begin(), pops_.end(), [&](const Pop& p) { return beat - p.b0 > 0.5; }),
                    pops_.end());
        for (const Pop& p : pops_) {
            const double a = beat - p.b0;
            if (a < 0) continue;
            c.bitmap(kPop, 7, p.x, p.y, px_ * (1 + 1.5 * a), sht * p.pen * (1 - 2 * a));
        }

        // Shots: a bold bolt (2 pixels wide, 7 long) with a bigger bright head and a short fading tail, so they read
        // at a glance (Peter 2026-11-22: more visible).
        for (const Shot& sh : shots_) {
            const double a = beat - sh.b0;
            if (a < 0) continue;
            const double y = std::min(sh.yEnd, sh.y0 + sh.v * a);
            c.rect(sh.x, y - 9 * px_, 1.2 * px_, 5 * px_, 0.45 * sht);  // tail
            c.rect(sh.x, y - 3.5 * px_, 2 * px_, 7 * px_, sht);          // bolt
            c.rect(sh.x, y, 2.6 * px_, 2.6 * px_, sht);                  // head
        }

        // The saucer, lights chasing on 16ths.
        if (saucerHit_ >= 0) {
            const double hitB = double(saucerHit_) / kTicksPerBeat;
            if (beat >= saucerStart_ && beat < hitB) {
                char lights[kSaucerW + 1];
                const int phase = int(std::floor(beat * kTicksPerBeat)) % 3;
                for (int i = 0; i < kSaucerW; ++i) lights[i] = (i + phase) % 3 == 0 ? '.' : 'X';
                lights[kSaucerW] = 0;
                const char* rows[kSaucerH];
                for (int i = 0; i < kSaucerH; ++i) rows[i] = kSaucerRows[i] ? kSaucerRows[i] : lights;
                c.bitmap(rows, kSaucerH, saucerX(beat), kSaucerY, px_, pen);
            }
        }

        // The cannon, with a muzzle flash for a 16th after each shot.
        const double cx = cannonX(beat);
        const int tread = int(std::floor(cx / (2 * px_))) & 1;
        c.bitmap(kTurret[tread], kTurretH, cx, kCannonY, px_, can);
        const double since = beat - lastFire_;
        if (since >= 0 && since < 0.25)
            c.bitmap(kPop, 7, cx, kCannonY + (kTurretH * 0.5 + 2.5) * px_, 0.5 * px_, sht * (1 - 4 * since));
    }

    scene::OrganicInstance& inst_;
    int cols_, rows_;
    double rate_, fire_, wave_, aim_;
    bool saucerOn_;
    expr::Pcg32 rng_;
    double px_ = 0.01, colSp_ = 0.16, rowSp_ = 0.14, dx_ = 0.08, dy_ = 0.08;
    int fireTicks_ = 2;

    bool started_ = false;
    std::int64_t tick_ = 0, lastWaveIdx_ = 0, lastPhrase_ = 0;
    double barOff_ = 0, lastDrop_ = 0;
    int speed_ = 1;

    Phase phase_ = Phase::Enter;
    std::vector<char> alive_, doomed_;
    std::vector<std::int64_t> blowAt_;
    int live_ = 0, waveId_ = 0, ix_ = 0, dir_ = 1, drops_ = 0, frame_ = 0, enterLeft_ = 0, beamLeft_ = 0;
    double lift_ = 0;
    int hits_ = 0, saucers_ = 0, beamUps_ = 0, dropsSeen_ = 0;

    std::int64_t camFromTick_ = 0, camToTick_ = 0;
    double camFromX_ = 0, camToX_ = 0, lastFire_ = -1e9;
    bool planned_ = false, planMiss_ = false;
    int planCol_ = 0;

    std::int64_t saucerHit_ = -1, saucerFireTick_ = -1, saucerFlight_ = 4;
    double saucerHitX_ = 0, saucerStart_ = 0;
    int saucerDir_ = 1;

    std::vector<Shot> shots_;
    std::vector<Bit> debris_;
    std::vector<Pop> pops_;
};

}  // namespace

std::unique_ptr<OrganicBody> makeInvaders(scene::OrganicInstance& inst, std::uint64_t seed) {
    return std::make_unique<Invaders>(inst, seed);
}

}  // namespace reforce::engine::arcade
