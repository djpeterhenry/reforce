// Arcade organic `rocks` (PLAN 5Q.5, docs/arcade-design.md): vector rocks drifting and slowly turning through a
// field that wraps at the frame's edges, a turret ship in the middle turning to aim and firing on the beat grid. Every
// shot connects: the ones arriving between the hit lines chip the rock (a flash, a few sparks), the one arriving on a
// hit line (each beat by default) splits it, large -> two medium -> two small -> a burst of its own outline. Fresh
// large rocks drift in from the edges on phrase lines, or on the next bar line once the field is clear. A drop
// (SYNC_DROP rising) sends a shockwave ring from the ship that shatters every rock it passes over a beat; through a
// breakdown (SYNC_BUILD) the fire rate doubles (8ths -> 16ths) and the rocks speed up. The ship never dies: it
// thrusts on the beat lines, recoils from its shots, dodges rocks that come close and springs back to the middle.
//
// The game runs on a tick grid of kTicks per beat of the clock (OrganicStep::beat), so it is the same at any frame
// rate and repeats with the seed; frames draw between ticks by extrapolating the motion. A step that jumps far ahead
// advances at most kMaxCatchUp ticks.
#include "engine/arcade/Rocks.hpp"

#include <array>
#include <cstdio>
#include <vector>

#include "engine/arcade/Arcade.hpp"
#include "expr/Rng.hpp"

namespace reforce::engine::arcade {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr int kTicks = kRocksTicksPerBeat;
constexpr double kDt = 1.0 / kTicks;      // beats per tick; velocities are in tree units per beat
constexpr int kMaxCatchUp = 4 * kTicks;   // ticks one step may advance
constexpr int kMaxRocks = 48;
constexpr int kMaxBits = 640;             // debris fragments + sparks
constexpr int kMaxShots = 32;
constexpr int kMaxVerts = 12;
// The field: the frame in tree units (half height 1, half width halfW_ from the canvas), wrapping a rock's width
// outside it so a rock is wholly off the frame when it jumps to the other side.
constexpr double kHalfH = 1.0;
// Intensities (x Pen), apart so the ColorMap tells the parts apart.
constexpr double kPenRock = 0.6, kPenFlash = 0.86, kPenShip = 0.92, kPenFlame = 0.74, kPenShot = 1.0;
constexpr double kPenDebris = 0.5, kPenSpark = 0.8, kPenRing = 0.7;

// Allowed shots per beat: each divides kTicks, so fire lines are whole ticks.
int fireStepFor(double perBeat) {
    static constexpr int kRates[] = {1, 2, 3, 4, 6, 8, 12, 16};
    int best = 2;
    for (int r : kRates)
        if (std::abs(r - perBeat) < std::abs(best - perBeat)) best = r;
    return kTicks / best;
}

double wrapAngle(double a) { return a - 2 * kPi * std::floor((a + kPi) / (2 * kPi)); }

void wrapInto(double& v, double h) {
    const double span = 2 * h;
    if (v > h) v -= span * std::ceil((v - h) / span);
    else if (v < -h) v += span * std::ceil((-h - v) / span);
}

struct Rock {
    double x = 0, y = 0, vx = 0, vy = 0, ang = 0, spin = 0, r = 0, flash = 0;
    int size = 0, n = 0;
    std::uint32_t id = 0;
    std::array<float, kMaxVerts> va{}, vr{};  // vertex angles and radii (x r)
    // Vertex k in the field at the rock's position + (ox, oy), turned by ang + da.
    void vertex(int k, double ox, double oy, double da, double& px, double& py) const {
        const double a = ang + da + va[std::size_t(k)], rr = r * vr[std::size_t(k)];
        px = x + ox + rr * std::cos(a);
        py = y + oy + rr * std::sin(a);
    }
};

// A piece of debris: a short turning line (an edge of the rock it came from) or a spark (a dot).
struct Bit {
    double x = 0, y = 0, vx = 0, vy = 0, ang = 0, spin = 0, len = 0;
    long long born = 0;
    int life = 0;  // ticks
    float pen = 0;
    bool spark = false;
};

struct Shot {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // from the nose to where it meets the rock's rim
    long long fired = 0, arrive = 0;
    std::uint32_t target = 0;  // 0: a miss (no rock to aim at), flies on and fades
    bool kill = false;         // arrives on a hit line: splits the rock
};

class Rocks : public OrganicBody {
public:
    Rocks(scene::OrganicInstance& in, std::uint64_t seed)
        : num_(keyCount(in, "NUM", 4, 1, 12)), hitsPerBeat_(keyNum(in, "HITS", 1, 0.125, 16)),
          spd_(keyNum(in, "SPD", 0.12, 0, 2)), wdth_(keyNum(in, "WDTH", 1.1, 0, 20)), size_(keyNum(in, "SIZE", 1, 0.2, 4)),
          waveBars_(keyNum(in, "WAVE", 16, 0, 1024)), glow_(keyNum(in, "GLOW", 0.35, 0, 1)), rng_(seed ^ 0x70c45ULL) {
        baseFire_ = fireStepFor(keyNum(in, "FIRE", 2, 0.5, 16));
        // A shot flies about half a beat, a whole number of fire steps, so it lands on the fire grid.
        flight_ = baseFire_ * std::max(1, int(std::ceil(0.45 * kTicks / baseFire_ - 1e-9)));
        // Hit lines on the fire grid.
        hitStep_ = std::max(1, int(std::lround(kTicks / hitsPerBeat_ / baseFire_))) * baseFire_;
        largeR_ = 0.2 * size_;
        margin_ = largeR_ * 1.2;
        stats_.fireStepTicks = baseFire_;
    }
    double halfW_ = 16.0 / 9.0;  // the field's half width: the frame's (OrganicCanvas::halfWidth), set each step

    void step(const OrganicStep& s, OrganicCanvas& c) override {
        if (!std::isfinite(s.beat)) return;
        halfW_ = std::clamp(c.halfWidth(), 0.25, 8.0);  // the frame's aspect (constant for a run)
        const double bt = std::clamp(s.beat, -1e12, 1e12) * kTicks;
        const long long now = (long long)std::floor(bt);
        // Per-step inputs, held through this step's ticks.
        const double build = buildOf(s);
        speedMul_ = 1.0 + 1.5 * build;
        fireStep_ = build > 0.35 ? std::max(1, baseFire_ / 2) : baseFire_;
        stats_.fireStepTicks = fireStep_;
        // Beats per second now (for the shots' streaks), from the step's own clock.
        if (s.dt > 1e-4 && s.dt < 0.5 && started_) {
            const double bps = (s.beat - lastBeat_) / s.dt;
            if (bps > 0.2 && bps < 20) beatsPerSec_ = bps;
        }
        lastBeat_ = s.beat;
        const double barOff = finiteOr(s.bar - s.beat / 4.0, 0);
        barOffset_ = std::abs(barOff) < 1e9 ? barOff : 0;
        if (!started_) {
            started_ = true;
            tick_ = now;
            lastBar_ = barIndex(tick_);
            lastPhrase_ = phraseIndex(lastBar_);
            ship_ = {};
            heading_ = kPi / 2;
            spawnWave(true);
        } else if (now < tick_ - kTicks) {
            // The clock jumped back: carry on from there, dropping shots timed for the old beat lines.
            tick_ = now;
            shots_.clear();
            targetLine_ = -1;
            lastBar_ = barIndex(tick_);
            lastPhrase_ = phraseIndex(lastBar_);
        }
        const double drop = dropOf(s);
        if (drop >= 0.6 && drop > prevDrop_ + 0.25) startShock(std::max(now, tick_));
        prevDrop_ = drop;

        if (now - tick_ > kMaxCatchUp) {
            // Jumped far ahead: skip what is too old to show. Shots in flight were timed for lines now skipped.
            tick_ = now - kMaxCatchUp;
            shots_.clear();
            targetLine_ = -1;
            lastBar_ = barIndex(tick_);
            lastPhrase_ = phraseIndex(lastBar_);
        }
        int n = 0;
        while (tick_ < now) {
            advance();
            ++n;
        }
        stats_.lastStepTicks = n;
        flameOnset_ = std::max(flameOnset_ * std::exp(-8.0 * std::max(0.0, s.dt)), std::min(1.0, 0.5 * onsetOf(s)));
        const double frac = std::clamp(bt - double(tick_), 0.0, 1.0);
        if (glow_ > 0) {
            haloPass_ = true;
            draw(s, c, frac);
            c.newPath();
        }
        haloPass_ = false;
        draw(s, c, frac);
        refreshCounts();
    }

    std::string summary() const override {
        char b[96];
        std::snprintf(b, sizeof b, "%d large rocks per wave, %d shots per beat, %g hits per beat", num_,
                      kTicks / baseFire_, double(kTicks) / hitStep_);
        return b;
    }
    const RocksStats& stats() const { return stats_; }

private:
    // ---- clock lines
    long long barIndex(long long tick) const {
        return (long long)std::floor(double(tick) / (4.0 * kTicks) + barOffset_ + 1e-9);
    }
    long long phraseIndex(long long bar) const {
        if (waveBars_ <= 0) return 0;
        return (long long)std::floor(double(bar) / waveBars_ + 1e-9);
    }
    static void keep(std::vector<double>& v, double x) {
        if (v.size() >= RocksStats::kKeep) v.erase(v.begin());
        v.push_back(x);
    }

    double uni(double a, double b) { return a + (b - a) * rng_.uniform01(); }

    // ---- rocks
    Rock makeRock(int size, double x, double y, double vx, double vy) {
        Rock k;
        k.id = ++nextId_;
        k.size = size;
        k.r = largeR_ * (size == 0 ? 1.0 : size == 1 ? 0.55 : 0.3);
        k.x = x;
        k.y = y;
        k.vx = vx;
        k.vy = vy;
        k.n = size == 0 ? 10 + int(rng_.next() % 3) : size == 1 ? 8 + int(rng_.next() % 2) : 7;
        const double step = 2 * kPi / k.n;
        for (int i = 0; i < k.n; ++i) {
            k.va[std::size_t(i)] = float(step * (i + uni(-0.28, 0.28)));
            // Mostly round with a dent or two: reads as a rock, not a star.
            k.vr[std::size_t(i)] = float(rng_.uniform01() < 0.18 ? uni(0.55, 0.7) : uni(0.8, 1.05));
        }
        k.ang = uni(0, 2 * kPi);
        k.spin = (rng_.uniform01() < 0.5 ? -1 : 1) * uni(0.06, 0.22) * (1 + 0.6 * size);
        return k;
    }
    void spawnWave(bool inside) {
        const int room = (kMaxRocks - int(rocks_.size())) / 7;  // a large rock splits into up to 7 rocks in all
        const int count = std::min(num_, std::max(0, room));
        if (count <= 0) return;
        for (int i = 0; i < count; ++i) {
            double x, y;
            if (inside) {
                // The first wave is already on screen, around the ship and clear of it.
                const double a = 2 * kPi * (i + uni(0.1, 0.9)) / count;
                const double rr = uni(0.65, 0.95);
                x = std::cos(a) * rr * (halfW_ - largeR_);
                y = std::sin(a) * rr * (kHalfH - largeR_);
            } else {
                // From just outside an edge, drifting in towards the middle.
                const double u = uni(-1, 1);
                switch (rng_.next() % 4) {
                    case 0: x = -halfW_ - margin_; y = u * kHalfH; break;
                    case 1: x = halfW_ + margin_; y = u * kHalfH; break;
                    case 2: x = u * halfW_; y = -kHalfH - margin_; break;
                    default: x = u * halfW_; y = kHalfH + margin_; break;
                }
            }
            const double tx = uni(-0.9, 0.9), ty = uni(-0.5, 0.5);
            double dx = tx - x, dy = ty - y;
            if (inside) {
                const double a = uni(0, 2 * kPi);
                dx = std::cos(a);
                dy = std::sin(a);
            }
            const double l = std::max(1e-9, std::hypot(dx, dy)), v = spd_ * uni(0.7, 1.3);
            rocks_.push_back(makeRock(0, x, y, dx / l * v, dy / l * v));
        }
        ++stats_.waves;
        keep(stats_.waveBeats, double(tick_) / kTicks);
    }
    Rock* find(std::uint32_t id) {
        for (auto& k : rocks_)
            if (k.id == id) return &k;
        return nullptr;
    }
    void removeRock(std::uint32_t id) {
        for (std::size_t i = 0; i < rocks_.size(); ++i)
            if (rocks_[i].id == id) {
                rocks_.erase(rocks_.begin() + long(i));
                return;
            }
    }

    // ---- debris
    void addBit(const Bit& b) {
        if (int(bits_.size()) < kMaxBits) {
            bits_.push_back(b);
            return;
        }
        // Full: replace the piece nearest its end.
        std::size_t worst = 0;
        long long least = 1LL << 62;
        for (std::size_t i = 0; i < bits_.size(); ++i) {
            const long long left = bits_[i].born + bits_[i].life - tick_;
            if (left < least) {
                least = left;
                worst = i;
            }
        }
        bits_[worst] = b;
    }
    // Edges of the rock's outline flying apart (every edge when `all`, else `count` of them), and sparks.
    void burst(const Rock& k, int count, bool all, int sparks, double push) {
        const int edges = all ? k.n : std::min(count, k.n);
        const int first = int(rng_.next() % std::uint32_t(k.n));
        for (int e = 0; e < edges; ++e) {
            const int i = all ? e : (first + e * 2) % k.n;  // every other edge: the rest go with the children
            double ax, ay, bx, by;
            k.vertex(i, 0, 0, 0, ax, ay);
            k.vertex((i + 1) % k.n, 0, 0, 0, bx, by);
            Bit b;
            b.x = 0.5 * (ax + bx);
            b.y = 0.5 * (ay + by);
            b.ang = std::atan2(by - ay, bx - ax);
            b.len = std::hypot(bx - ax, by - ay) * uni(0.7, 1.0);
            const double ox = b.x - k.x, oy = b.y - k.y, ol = std::max(1e-9, std::hypot(ox, oy));
            const double v = push * uni(0.6, 1.3);
            b.vx = k.vx * speedMul_ * 0.6 + ox / ol * v;
            b.vy = k.vy * speedMul_ * 0.6 + oy / ol * v;
            b.spin = uni(-4, 4);
            b.born = tick_;
            b.life = int(kTicks * uni(1.2, 2.2));
            b.pen = float(kPenDebris);
            addBit(b);
        }
        for (int i = 0; i < sparks; ++i) {
            Bit b;
            const double a = uni(0, 2 * kPi), v = push * uni(1.6, 3.6);
            b.x = k.x + std::cos(a) * k.r * 0.4;
            b.y = k.y + std::sin(a) * k.r * 0.4;
            b.vx = std::cos(a) * v;
            b.vy = std::sin(a) * v;
            b.born = tick_;
            b.life = int(kTicks * uni(0.5, 1.0));
            b.pen = float(kPenSpark);
            b.spark = true;
            addBit(b);
        }
    }
    void sparksAt(double x, double y, double dx, double dy, int count) {
        for (int i = 0; i < count; ++i) {
            Bit b;
            // Back towards the ship, fanned out: the shot's spray off the rim.
            const double a = std::atan2(-dy, -dx) + uni(-1.1, 1.1), v = uni(0.4, 1.0);
            b.x = x;
            b.y = y;
            b.vx = std::cos(a) * v;
            b.vy = std::sin(a) * v;
            b.born = tick_;
            b.life = int(kTicks * uni(0.3, 0.6));
            b.pen = float(kPenSpark);
            b.spark = true;
            addBit(b);
        }
    }

    // A shot splits rock `id` (dx, dy: the shot's direction): two smaller rocks parting across the shot, or a burst.
    void split(std::uint32_t id, double dx, double dy) {
        Rock* p = find(id);
        if (!p) return;
        const Rock k = *p;
        removeRock(id);
        ++stats_.hits;
        keep(stats_.hitBeats, double(tick_) / kTicks);
        if (stats_.hitSizes.size() >= RocksStats::kKeep) stats_.hitSizes.erase(stats_.hitSizes.begin());
        stats_.hitSizes.push_back(k.size);
        if (k.size >= 2 || int(rocks_.size()) + 2 > kMaxRocks) {
            burst(k, 0, true, 12, 0.35);
            return;
        }
        burst(k, k.size == 0 ? 5 : 4, false, 8, 0.3);
        const double nx = -dy, ny = dx;  // across the shot
        for (int side = -1; side <= 1; side += 2) {
            const double sv = spd_ * uni(0.7, 1.2) * (1.0 + 0.5 * k.size) + 0.05;
            Rock c = makeRock(k.size + 1, k.x + side * nx * k.r * 0.4, k.y + side * ny * k.r * 0.4,
                              k.vx * 0.7 + side * nx * sv + dx * 0.25 * sv, k.vy * 0.7 + side * ny * sv + dy * 0.25 * sv);
            c.flash = 1;
            rocks_.push_back(c);
        }
    }

    // ---- the drop: a ring from the ship shattering what it passes
    void startShock(long long at) {
        shockOn_ = true;
        shockStart_ = at;
        shockX_ = ship_.x;
        shockY_ = ship_.y;
        stats_.shockBeat = double(at) / kTicks;
        shots_.clear();
        targetLine_ = -1;
    }
    static constexpr double kShockR = 2.9;  // tree units: past the wrap margin's corners from anywhere near the middle
    double shockRadius(double ticks) const {
        const double u = std::clamp(ticks / kTicks, 0.0, 1.0);
        return kShockR * (1 - (1 - u) * (1 - u));
    }

    // ---- the ship
    struct Ship {
        double x = 0, y = 0, vx = 0, vy = 0;
    };
    double shipScale() const { return 0.12 * size_; }
    double noseLen() const { return 0.95 * shipScale(); }

    // The rock to split on hit line `line`: on the frame then, not on top of the ship; one of the nearest few.
    std::uint32_t chooseTarget(long long line) {
        const double ahead = double(line - tick_) / kTicks * speedMul_;
        struct C {
            double d;
            std::uint32_t id;
        };
        std::array<C, 3> best{};
        int nb = 0;
        for (const auto& k : rocks_) {
            const double px = k.x + k.vx * ahead, py = k.y + k.vy * ahead;
            if (std::abs(px) > halfW_ - 0.5 * k.r || std::abs(py) > kHalfH - 0.5 * k.r) continue;
            const double d = std::hypot(px - ship_.x, py - ship_.y);
            if (d < k.r + 1.2 * shipScale()) continue;
            // Small rocks a little sooner, so a field ends in bursts rather than with stragglers.
            const C cand{d * (1.0 - 0.12 * k.size), k.id};
            if (nb < 3) best[std::size_t(nb++)] = cand;
            else {
                int w = 0;
                for (int i = 1; i < 3; ++i)
                    if (best[std::size_t(i)].d > best[std::size_t(w)].d) w = i;
                if (cand.d < best[std::size_t(w)].d) best[std::size_t(w)] = cand;
            }
        }
        if (nb == 0) return 0;
        return best[std::size_t(rng_.next() % std::uint32_t(nb))].id;
    }
    void fire() {
        if (int(shots_.size()) >= kMaxShots) return;
        const long long arrive = tick_ + flight_;
        // The hit line this shot works towards: the first at or after its arrival.
        const long long m = ((arrive % hitStep_) + hitStep_) % hitStep_;
        const long long hl = m == 0 ? arrive : arrive + (hitStep_ - m);
        if (targetLine_ != hl) {
            target_ = chooseTarget(hl);
            targetLine_ = hl;
        }
        Shot sh;
        sh.fired = tick_;
        sh.arrive = arrive;
        const Rock* k = target_ ? find(target_) : nullptr;
        double dx, dy;
        if (k) {
            const double ahead = double(flight_) / kTicks * speedMul_;
            const double px = k->x + k->vx * ahead, py = k->y + k->vy * ahead;
            dx = px - ship_.x;
            dy = py - ship_.y;
            const double l = std::max(1e-9, std::hypot(dx, dy));
            dx /= l;
            dy /= l;
            sh.x1 = px - dx * k->r * 0.8;
            sh.y1 = py - dy * k->r * 0.8;
            sh.target = target_;
            sh.kill = arrive == hl;
        } else {
            dx = std::cos(heading_);
            dy = std::sin(heading_);
            sh.x1 = ship_.x + dx * (noseLen() + 0.9);
            sh.y1 = ship_.y + dy * (noseLen() + 0.9);
        }
        sh.x0 = ship_.x + dx * noseLen();
        sh.y0 = ship_.y + dy * noseLen();
        // The barrels swing onto the shot (the turn mostly happened already).
        heading_ = std::atan2(dy, dx);
        ship_.vx -= dx * 0.03;
        ship_.vy -= dy * 0.03;
        shots_.push_back(sh);
        ++stats_.fired;
        keep(stats_.fireBeats, double(tick_) / kTicks);
    }

    // ---- one tick
    void advance() {
        ++tick_;
        const double mv = kDt * speedMul_;
        for (auto& k : rocks_) {
            k.x += k.vx * mv;
            k.y += k.vy * mv;
            k.ang += k.spin * mv;
            wrapInto(k.x, halfW_ + margin_);
            wrapInto(k.y, kHalfH + margin_);
            k.flash *= 0.92;
        }
        for (std::size_t i = 0; i < bits_.size();) {
            Bit& b = bits_[i];
            if (tick_ - b.born >= b.life) {
                bits_[i] = bits_.back();
                bits_.pop_back();
                continue;
            }
            b.x += b.vx * kDt;
            b.y += b.vy * kDt;
            b.ang += b.spin * kDt;
            const double drag = 0.985;
            b.vx *= drag;
            b.vy *= drag;
            ++i;
        }

        // Shots arriving (late ones too, after a skipped stretch).
        for (std::size_t i = 0; i < shots_.size();) {
            Shot& sh = shots_[i];
            if (sh.target && tick_ >= sh.arrive) {
                const double dx = sh.x1 - sh.x0, dy = sh.y1 - sh.y0, l = std::max(1e-9, std::hypot(dx, dy));
                if (Rock* k = find(sh.target)) {
                    if (sh.kill) split(sh.target, dx / l, dy / l);
                    else {
                        ++stats_.chips;
                        k->flash = 1;
                        k->vx += dx / l * 0.015;
                        k->vy += dy / l * 0.015;
                        sparksAt(k->x - dx / l * k->r * 0.8, k->y - dy / l * k->r * 0.8, dx / l, dy / l, 3);
                    }
                }
                shots_.erase(shots_.begin() + long(i));
                continue;
            }
            if (!sh.target && tick_ >= sh.fired + 3 * (sh.arrive - sh.fired)) {
                shots_.erase(shots_.begin() + long(i));
                continue;
            }
            ++i;
        }

        // The shockwave.
        if (shockOn_) {
            const double rad = shockRadius(double(tick_ - shockStart_));
            for (std::size_t i = 0; i < rocks_.size();) {
                const Rock& k = rocks_[i];
                if (std::hypot(k.x - shockX_, k.y - shockY_) < rad + k.r) {
                    burst(k, 0, true, 5, 0.45);
                    ++stats_.shattered;
                    rocks_.erase(rocks_.begin() + long(i));
                    continue;
                }
                ++i;
            }
            if (tick_ - shockStart_ >= kTicks * 3 / 2) shockOn_ = false;
        }

        // Ship: a spring to the middle, dodging rocks that come close.
        {
            double ax = -1.4 * ship_.x - 2.2 * ship_.vx, ay = -1.4 * ship_.y - 2.2 * ship_.vy;
            const double keepOut = 2.2 * shipScale();
            for (const auto& k : rocks_) {
                const double dx = ship_.x - k.x, dy = ship_.y - k.y, d = std::hypot(dx, dy);
                const double reach = k.r + keepOut;
                if (d < reach && d > 1e-6) {
                    const double f = 6.0 * (reach - d) / reach;
                    ax += dx / d * f;
                    ay += dy / d * f;
                }
            }
            ship_.vx += ax * kDt;
            ship_.vy += ay * kDt;
            ship_.x = std::clamp(ship_.x + ship_.vx * kDt, -0.6 * halfW_, 0.6 * halfW_);
            ship_.y = std::clamp(ship_.y + ship_.vy * kDt, -0.6 * kHalfH, 0.6 * kHalfH);
        }
        // Turning towards the next rock to split (slowly round without one).
        {
            double want = heading_ + 0.5 * kDt;
            if (const Rock* k = target_ && targetLine_ >= tick_ ? find(target_) : nullptr) {
                const double ahead = double(targetLine_ - tick_) / kTicks * speedMul_;
                want = std::atan2(k->y + k->vy * ahead - ship_.y, k->x + k->vx * ahead - ship_.x);
            }
            heading_ = wrapAngle(heading_ + wrapAngle(want - heading_) * std::min(1.0, 16.0 * kDt));
        }
        flame_ *= std::exp(-7.0 * kDt);

        // Beat lines: a burst of thrust. Bar / phrase lines: fresh rocks.
        if (tick_ % kTicks == 0) {
            flame_ = 1;
            ship_.vx += std::cos(heading_) * 0.22;
            ship_.vy += std::sin(heading_) * 0.22;
        }
        const long long bar = barIndex(tick_);
        if (bar != lastBar_) {
            const long long phrase = phraseIndex(bar);
            if (waveBars_ > 0 && phrase != lastPhrase_) spawnWave(false);
            else if (rocks_.empty() && !shockOn_) spawnWave(false);
            lastBar_ = bar;
            lastPhrase_ = phrase;
        }
        // Fire lines (none while the ring is out).
        if (!shockOn_ && tick_ % fireStep_ == 0) fire();
    }

    // ---- drawing
    // A glowing vector line: a wide dim halo under a thin bright core. Within one path the stroke with the most
    // coverage wins, so a halo would cut into the cores drawn before it: the halos all go in a first pass, the cores
    // in a second path over them.
    void vline(OrganicCanvas& c, double x0, double y0, double x1, double y1, double w, double pen,
               double halo = 1) const {
        if (haloPass_) {
            if (glow_ * halo > 0) c.line(x0, y0, x1, y1, w * (1 + 2.2 * halo), pen * glow_ * halo);
        } else
            c.line(x0, y0, x1, y1, w, pen);
    }
    void dot(OrganicCanvas& c, double x, double y, double w, double pen) const {
        if (!haloPass_) c.dot(x, y, w, pen);
    }
    void draw(const OrganicStep& s, OrganicCanvas& c, double frac) {
        const double pen = s.pen, w = wdth_;
        if (w <= 0) return;
        const double ft = frac * kDt;  // beats past the tick
        const double tt = double(tick_) + frac;

        // Debris first: the oldest and dimmest.
        for (const auto& b : bits_) {
            const double age = (tt - double(b.born)) / b.life;
            if (age >= 1) continue;
            const double fade = (1 - age) * (1 - age);
            const double x = b.x + b.vx * ft, y = b.y + b.vy * ft;
            if (b.spark) {
                dot(c, x, y, w * 1.7, pen * b.pen * fade);
                continue;
            }
            const double a = b.ang + b.spin * ft, hx = 0.5 * b.len * std::cos(a), hy = 0.5 * b.len * std::sin(a);
            vline(c, x - hx, y - hy, x + hx, y + hy, w * 0.9, pen * b.pen * (0.35 + 0.65 * fade));
        }

        // Rocks, turned and drifted to the frame's moment.
        const double mv = ft * speedMul_;
        for (const auto& k : rocks_) {
            const double ox = k.vx * mv, oy = k.vy * mv, da = k.spin * mv;
            if (std::abs(k.x + ox) > halfW_ + k.r * 1.1 || std::abs(k.y + oy) > kHalfH + k.r * 1.1) continue;
            const double p = pen * (kPenRock + (kPenFlash - kPenRock) * k.flash);
            double px0, py0, pxa, pya;
            k.vertex(0, ox, oy, da, px0, py0);
            pxa = px0;
            pya = py0;
            for (int i = 1; i <= k.n; ++i) {
                double px, py;
                if (i == k.n) {
                    px = px0;
                    py = py0;
                } else
                    k.vertex(i, ox, oy, da, px, py);
                vline(c, pxa, pya, px, py, w, p);
                pxa = px;
                pya = py;
            }
        }

        // Shots: short bright dashes.
        for (const auto& sh : shots_) {
            const double span = double(sh.arrive - sh.fired), u = (tt - double(sh.fired)) / span;
            if (u < 0 || (sh.target && u >= 1)) continue;
            const double dx = sh.x1 - sh.x0, dy = sh.y1 - sh.y0, l = std::max(1e-9, std::hypot(dx, dy));
            const double hx = sh.x0 + dx * u, hy = sh.y0 + dy * u;
            // At least as long as this frame's travel, so the trail behind it is a streak, not a dotted line.
            const double perFrame = l / span * std::clamp(s.dt, 0.0, 0.1) * beatsPerSec_ * kTicks;
            const double dash = std::min(l * u, std::max(0.075 * size_, std::min(perFrame, 0.3 * size_)));
            const double fade = sh.target ? 1.0 : std::clamp(1.5 - 0.5 * u, 0.0, 1.0);
            vline(c, hx - dx / l * dash, hy - dy / l * dash, hx, hy, w * 1.5, pen * kPenShot * fade);
        }

        // The shockwave ring.
        if (shockOn_) {
            const double age = tt - double(shockStart_), rad = shockRadius(age);
            const double fade = std::clamp(1.0 - age / (kTicks * 1.5), 0.0, 1.0);
            constexpr int kSeg = 72;
            for (int i = 0; i < kSeg; ++i) {
                const double a0 = 2 * kPi * i / kSeg, a1 = 2 * kPi * (i + 1) / kSeg;
                vline(c, shockX_ + rad * std::cos(a0), shockY_ + rad * std::sin(a0), shockX_ + rad * std::cos(a1),
                      shockY_ + rad * std::sin(a1), w * 1.6, pen * kPenRing * fade);
            }
        }

        drawShip(s, c, ft);
    }

    // Our own ship: a stretched hexagonal hull, twin barrels forward, swept outriggers, a thrust flame aft.
    void drawShip(const OrganicStep& s, OrganicCanvas& c, double ft) const {
        const double sc = shipScale(), sx = ship_.x + ship_.vx * ft, sy = ship_.y + ship_.vy * ft;
        const double ca = std::cos(heading_), sa = std::sin(heading_);
        const double pen = s.pen * kPenShip, w = wdth_ * 1.1;
        auto P = [&](double u, double v, double& x, double& y) {
            x = sx + sc * (u * ca - v * sa);
            y = sy + sc * (u * sa + v * ca);
        };
        auto L = [&](double u0, double v0, double u1, double v1, double ww, double p) {
            double x0, y0, x1, y1;
            P(u0, v0, x0, y0);
            P(u1, v1, x1, y1);
            vline(c, x0, y0, x1, y1, ww, p, 0.5);  // a tighter halo: the ship's lines are close together
        };
        static constexpr double hull[6][2] = {{0.62, 0},     {0.15, 0.27},  {-0.38, 0.27},
                                              {-0.52, 0},    {-0.38, -0.27}, {0.15, -0.27}};
        for (int i = 0; i < 6; ++i) L(hull[i][0], hull[i][1], hull[(i + 1) % 6][0], hull[(i + 1) % 6][1], w, pen);
        for (int m = -1; m <= 1; m += 2) {
            L(0.25, m * 0.12, 1.0, m * 0.12, w, pen);  // twin barrels, out of the hull's front faces
            L(-0.02, m * 0.27, -0.42, m * 0.62, w, pen);  // swept fins
            L(-0.42, m * 0.62, -0.62, m * 0.62, w, pen);
            L(-0.62, m * 0.62, -0.38, m * 0.27, w, pen);
        }
        double cx, cy;
        P(0.1, 0, cx, cy);
        dot(c, cx, cy, w * 2.4, pen);  // the cockpit
        const double fl = std::max(flame_, flameOnset_);
        if (fl > 0.04) {
            // Flicker from the tick, so it repeats.
            const double flick = 0.8 + 0.2 * std::sin(double(tick_) * 2.7);
            const double len = 0.35 + 0.85 * fl * flick;
            const double fp = s.pen * kPenFlame * std::min(1.0, 0.3 + fl);
            L(-0.45, 0.13, -0.55 - len, 0, w, fp);
            L(-0.45, -0.13, -0.55 - len, 0, w, fp);
        }
    }

    void refreshCounts() {
        stats_.rocks = int(rocks_.size());
        stats_.large = stats_.medium = stats_.small = 0;
        for (const auto& k : rocks_) (k.size == 0 ? stats_.large : k.size == 1 ? stats_.medium : stats_.small)++;
        stats_.debris = int(bits_.size());
        stats_.shots = int(shots_.size());
    }

    int num_;
    double hitsPerBeat_, spd_, wdth_, size_, waveBars_, glow_;
    int baseFire_ = 24, fireStep_ = 24, flight_ = 24, hitStep_ = 48;
    double largeR_ = 0.2, margin_ = 0.24;
    expr::Pcg32 rng_;
    bool started_ = false;
    long long tick_ = 0, lastBar_ = 0, lastPhrase_ = 0;
    double barOffset_ = 0, speedMul_ = 1, prevDrop_ = 0, lastBeat_ = 0, beatsPerSec_ = 2;
    std::vector<Rock> rocks_;
    std::vector<Bit> bits_;
    std::vector<Shot> shots_;
    std::uint32_t nextId_ = 0, target_ = 0;
    long long targetLine_ = -1;
    Ship ship_;
    double heading_ = kPi / 2, flame_ = 0, flameOnset_ = 0;
    bool shockOn_ = false, haloPass_ = false;
    long long shockStart_ = 0;
    double shockX_ = 0, shockY_ = 0;
    RocksStats stats_;
};

}  // namespace

std::unique_ptr<OrganicBody> makeRocks(scene::OrganicInstance& inst, std::uint64_t seed) {
    return std::make_unique<Rocks>(inst, seed);
}

const RocksStats* rocksStats(const OrganicBody& body) {
    const auto* r = dynamic_cast<const Rocks*>(&body);
    return r ? &r->stats() : nullptr;
}

}  // namespace reforce::engine::arcade
