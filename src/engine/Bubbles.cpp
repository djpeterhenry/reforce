// reforce extension: the bubbles object's simulation. See Bubbles.hpp and docs/3d-design.md "Bubbles".
#include "engine/Bubbles.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace reforce::engine::bubbles {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kPx = 1.0 / 1080.0;  // one pixel at 1080p, in frame heights

std::uint64_t mix64(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

double clampFinite(double v, double lo, double hi, double fallback) {
    if (!std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

std::uint32_t lowbias32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
double lattice(std::int32_t ix, std::int32_t iy, std::uint32_t seed) {
    const std::uint32_t h = lowbias32(std::uint32_t(ix) * 0x8da6b343u ^ std::uint32_t(iy) * 0xd8163841u ^ seed * 0xcb1ab31fu);
    return double(h) / 4294967296.0;
}
double valueNoise(double x, double y, std::uint32_t seed) {
    const double fx = std::floor(x), fy = std::floor(y);
    const auto ix = std::int32_t(fx), iy = std::int32_t(fy);
    double u = x - fx, v = y - fy;
    u = u * u * (3 - 2 * u);
    v = v * v * (3 - 2 * v);
    const double a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
    const double c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}
// One octave that changes over time: a crossfade from one seeded noise layer to the next every `period` seconds.
double drifting(double x, double y, double t, double period, std::uint32_t seed) {
    const double s = t / period, i = std::floor(s);
    double f = s - i;
    f = f * f * (3 - 2 * f);
    const auto layer = std::uint32_t(std::int32_t(std::fmod(i, 1e6)));
    const double a = valueNoise(x, y, seed + layer * 0x9e3779b9u);
    const double b = valueNoise(x, y, seed + (layer + 1u) * 0x9e3779b9u);
    return a + (b - a) * f;
}

// The n = 2 shape mode's frequency (Hz) of a bubble of drawn radius r (frame heights): bigger bubbles wobble slower
// (as r^-1.5); n = 3 rings 2.24 times faster.
double modeHz(double r) {
    const double rpx = std::max(4.0, r / kPx);
    return std::clamp(2.6 * std::pow(55.0 / rpx, 1.5), 0.7, 9.0);
}

}  // namespace

std::optional<Source> parseSource(std::string_view s) {
    std::string t;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"') t += char(std::tolower(static_cast<unsigned char>(c)));
    if (t.empty() || t == "wand" || t == "0") return Source::Wand;
    if (t == "edge" || t == "1") return Source::Edge;
    if (t == "air" || t == "2") return Source::Air;
    return std::nullopt;
}

const char* sourceName(Source s) {
    switch (s) {
        case Source::Edge: return "edge";
        case Source::Air: return "air";
        case Source::Wand: break;
    }
    return "wand";
}

void Sim::airAt(double x, double y, double t, std::uint32_t seed, double& vx, double& vy) {
    // psi = two drifting octaves; v = (d psi / dy, -d psi / dx) (y down): divergence free.
    auto psi = [&](double px, double py) {
        return (0.62 / 1.3) * drifting(px * 1.3 + 3.1, py * 1.3 + 7.7, t, 7.0, seed ^ 0x51ed270bu) +
               (0.30 / 3.1) * drifting(px * 3.1 + 11.3, py * 3.1 + 1.9, t, 3.7, seed ^ 0x2c1b3c6du);
    };
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(t)) {
        vx = vy = 0;
        return;
    }
    constexpr double e = 1e-3;
    vx = (psi(x, y + e) - psi(x, y - e)) / (2 * e);
    vy = -(psi(x + e, y) - psi(x - e, y)) / (2 * e);
}

double Sim::drawnRadius(const Bubble& b) {
    const double g = std::clamp(b.grow, 0.0, 1.0);
    const double e = 1.0 - (1.0 - g) * (1.0 - g);  // ease out: fast at first, settling
    return b.r * (0.12 + 0.88 * e);
}

Sim::Sim(const Settings& s, double aspect, double startTime) : settings_(s) {
    aspect_ = clampFinite(aspect, 0.2, 8.0, 16.0 / 9.0);
    start_ = std::isfinite(startTime) ? startTime : 0.0;
    settings_.maxBubbles = std::clamp(s.maxBubbles, 1, kMaxBubbles);
    settings_.cluster = clampFinite(s.cluster, 0.0, 1.0, 0.5);
    bubbles_.reserve(kMaxBubbles + kMaxPerLine);
    droplets_.reserve(kMaxDroplets);
    pops_.reserve(256);
    lines_.reserve(256);
    pick_.reserve(kMaxBubbles + kMaxPerLine);
    seed32_ = std::uint32_t(settings_.seed ^ (settings_.seed >> 32));
}

double Sim::rand01(std::uint64_t& h) const {
    h = mix64(h);
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}

Bubble* Sim::find(std::uint32_t id) {
    if (id == 0) return nullptr;
    for (Bubble& b : bubbles_)
        if (b.id == id) return &b;
    return nullptr;
}

void Sim::unbond(Bubble& a, std::uint32_t other) {
    for (auto& b : a.bond)
        if (b == other && other != 0) {
            b = 0;
            ++tally_.unbonds;
        }
}

void Sim::wand(std::int64_t k, double& x, double& y, double& aim) const {
    // Each bar's puff from a new seeded spot anywhere in the frame (Peter 2026-11-26: more random than one corner),
    // aimed towards a random point well inside the frame (so the puff stays in view), swaying a little over the bar.
    const std::int64_t bar = k >= 0 ? k / 16 : -((-k + 15) / 16);
    std::uint64_t h = mix64(settings_.seed * 0x2545f4914f6cdd1dull ^ 0x77616e64ull ^ (std::uint64_t(bar) * 0x9e3779b97f4a7c15ull));
    const double A = aspect_;
    x = (0.08 + 0.84 * rand01(h)) * A;
    y = 0.12 + 0.8 * rand01(h);
    double tx = (0.2 + 0.6 * rand01(h)) * A, ty = 0.15 + 0.6 * rand01(h);
    if (std::hypot(tx - x, ty - y) < 0.2) {  // too close to aim by: a random way, away from the nearer edge
        tx = x + (x < 0.5 * A ? 1.0 : -1.0) * (0.3 + 0.4 * rand01(h));
        ty = y + (rand01(h) - 0.7) * 0.6;
    }
    const double sway = 2 * kPi * double(((k % 16) + 16) % 16) / 16.0;
    aim = std::atan2(ty - y, tx - x) + 0.2 * std::sin(sway) + (rand01(h) - 0.5) * 0.5;
}

std::uint32_t Sim::place(double x, double y, double r) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(r) || !(r > 0)) return 0;
    if (int(bubbles_.size()) >= kMaxBubbles) return 0;
    Bubble b;
    b.id = nextId_++;
    b.x = std::clamp(x, 0.0, aspect_);
    b.y = std::clamp(y, 0.0, 1.0);
    b.r = std::clamp(r, 2.0 * kPx, 0.45);
    b.grow = 1;
    b.age = 1;
    b.film = double(b.id % 97) * 1.37;
    b.bornLine = nextLine_ - 1;
    bubbles_.push_back(b);
    ++tally_.blown;
    return b.id;
}

void Sim::planPop(std::uint32_t id) {
    Bubble* b = find(id);
    if (!b || b->pop >= 0) return;
    // The next beat line after the last visited line.
    const std::int64_t last = nextLine_ - 1;
    std::int64_t k = last + 1;
    while (((k % 4) + 4) % 4 != 0) ++k;
    b->popLine = k;
    b->planLine = last;
}

void Sim::blow(std::int64_t k, int n, bool burst, const Controls& c, std::uint64_t& h) {
    int alive = 0;
    for (const Bubble& b : bubbles_) alive += b.pop < 0;
    const int limit = burst ? std::min(kMaxBubbles, settings_.maxBubbles + std::max(2, settings_.maxBubbles / 2))
                            : settings_.maxBubbles;
    const double rMean = clampFinite(c.size, 4.0, 400.0, 55.0) * kPx * (1.0 + 0.6 * calm_);
    const double A = aspect_;
    for (int i = 0; i < n && i < kMaxPerLine; ++i) {
        if (alive >= limit || int(bubbles_.size()) >= kMaxBubbles) break;
        // Lognormal sizes around the mean (Box-Muller).
        const double u1 = std::max(1e-12, rand01(h)), u2 = rand01(h);
        const double g = std::sqrt(-2.0 * std::log(u1)) * std::cos(2 * kPi * u2);
        const double r = std::clamp(rMean * std::exp(0.3 * g), 5.0 * kPx, 420.0 * kPx);
        Bubble b;
        b.id = nextId_++;
        b.r = r;
        b.film = rand01(h) * 100.0;
        b.bornLine = k;
        switch (settings_.source) {
            case Source::Wand: {
                double wx, wy, aim;
                wand(k, wx, wy, aim);
                aim += (rand01(h) - 0.5) * (burst ? 1.8 : 0.6);
                const double sp = burst ? 0.7 + 0.6 * rand01(h) : 0.45 + 0.35 * rand01(h);
                b.x = wx + std::cos(aim) * 0.3 * r;
                b.y = wy + std::sin(aim) * 0.3 * r;
                b.vx = std::cos(aim) * sp;
                b.vy = std::sin(aim) * sp;
                b.grow = 0;
                // Stretched along the way it is blown, then wobbling round.
                b.mode[0] = 0.07 * std::cos(2 * aim);
                b.mode[1] = 0.07 * std::sin(2 * aim);
                break;
            }
            case Source::Edge: {
                b.x = r + rand01(h) * std::max(0.0, A - 2 * r);
                b.y = 1.0 + 1.05 * r;
                b.vx = (rand01(h) - 0.5) * 0.06;
                b.vy = burst ? -(0.25 + 0.3 * rand01(h)) : -(0.07 + 0.08 * rand01(h));
                b.grow = 1;
                break;
            }
            case Source::Air: {
                // In place, inflating: of a few seeded spots the one farthest from the others.
                double best = -1e9;
                for (int tries = 0; tries < 6; ++tries) {
                    const double x = r + rand01(h) * std::max(0.0, A - 2 * r);
                    const double y = r + rand01(h) * std::max(0.0, 0.92 - 2 * r);
                    double clear = 1e9;
                    for (const Bubble& o : bubbles_) clear = std::min(clear, std::hypot(o.x - x, o.y - y) - o.r - r);
                    if (clear > best) {
                        best = clear;
                        b.x = x;
                        b.y = y;
                    }
                }
                if (burst) {
                    const double a = rand01(h) * 2 * kPi;
                    b.vx = std::cos(a) * 0.3;
                    b.vy = std::sin(a) * 0.3;
                }
                b.grow = 0;
                break;
            }
        }
        b.mode[2] = 0.03 * (rand01(h) - 0.5);
        b.mode[3] = 0.03 * (rand01(h) - 0.5);
        b.age = 0;
        bubbles_.push_back(b);
        ++alive;
        ++tally_.blown;
    }
}

void Sim::startPop(Bubble& b, std::int64_t line, double beat, bool planned, std::uint64_t h) {
    if (b.pop >= 0) return;
    b.pop = 0;
    b.doom = planned ? 1.0 : b.doom;
    b.popLine = 0;
    // The film tears somewhere on the face turned to us (a little more often higher up, where it is thinnest), so the
    // hole opens in the face and grows out over the sphere rather than sweeping down from the top edge (seeded).
    const double az = 2 * kPi * rand01(h), cz = 0.55 + 0.45 * rand01(h), sz = std::sqrt(1 - cz * cz);
    double tx = sz * std::cos(az), ty = sz * std::sin(az) - 0.15, tz = cz;
    const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
    b.tear[0] = tx / len;
    b.tear[1] = ty / len;
    b.tear[2] = tz / len;
    // The rim breaks up into a spray of droplets as it runs round the shell: each appears when the rim reaches its spot.
    const double rd = drawnRadius(b);
    const int n = std::clamp(int(12 + rd / kPx / 3.0), 12, 60);
    for (int i = 0; i < n && int(droplets_.size()) < kMaxDroplets; ++i) {
        const double z = 2 * rand01(h) - 1, phi = 2 * kPi * rand01(h), s = std::sqrt(std::max(0.0, 1 - z * z));
        const double ux = s * std::cos(phi), uy = s * std::sin(phi), uz = z;
        const double cosA = std::clamp(ux * b.tear[0] + uy * b.tear[1] + uz * b.tear[2], -1.0, 1.0);
        const double delay = kPopSeconds * std::acos(cosA) / kPi;
        // Flung along the rim's travel (away from the tear), projected.
        double dx = ux - b.tear[0] * cosA, dy = uy - b.tear[1] * cosA;
        const double dl = std::hypot(dx, dy);
        if (dl > 1e-6) {
            dx /= dl;
            dy /= dl;
        }
        const double sp = 0.2 + 0.6 * rand01(h);
        Droplet d;
        d.x = b.x + ux * rd;
        d.y = b.y + uy * rd;
        d.vx = b.vx + dx * sp + ux * 0.3;
        d.vy = b.vy + dy * sp + uy * 0.3;
        d.r = (0.7 + 1.6 * rand01(h) * rand01(h)) * kPx * std::clamp(rd / (55.0 * kPx), 0.6, 1.8);
        d.age = -delay;
        d.life = 0.3 + 0.5 * rand01(h);
        droplets_.push_back(d);
    }
    for (auto& id : b.bond) {
        if (Bubble* o = find(id)) unbond(*o, b.id);
        id = 0;
    }
    ++tally_.popped;
    if (((line % 4) + 4) % 4 != 0) ++tally_.offBeatPops;
    if (pops_.size() >= 256) pops_.erase(pops_.begin(), pops_.begin() + 128);
    pops_.push_back({line, beat, b.id, planned});
}

void Sim::visitLine(std::int64_t k, double beatAt, const Clock& clock, const Controls& c, double /*frac*/) {
    ++tally_.lines;
    if (lines_.size() >= 256) lines_.erase(lines_.begin(), lines_.begin() + 128);
    lines_.push_back(k);
    std::uint64_t h = mix64(settings_.seed * 0x9e3779b97f4a7c15ull ^ std::uint64_t(k) * 0xd6e8feb86659fd93ull);
    const double build = clampFinite(clock.build, 0.0, 1.0, 0.0);
    const double drop = clampFinite(clock.drop, 0.0, 1.0, 0.0);
    const bool beatLine = ((k % 4) + 4) % 4 == 0;
    bool barLine = false;
    if (beatLine) {
        const double bar = clock.bar + (double(k) / 4.0 - beatAt) / 4.0;
        barLine = std::fabs(bar - std::round(bar)) < 0.02;
    }
    auto count = [&](double expected, double hi) {
        expected = clampFinite(expected, 0.0, hi, 0.0);
        const double f = std::floor(expected);
        return int(f) + (rand01(h) < expected - f ? 1 : 0);
    };
    if (drop > 0.5 && lastDrop_ <= 0.5) dropArmed_ = true;
    lastDrop_ = drop;
    // A bar's puff: Blow bubbles (more as a build rises), released over the bar's first 16ths.
    if (barLine) queue_ = std::min(64, queue_ + count(clampFinite(c.blow, 0.0, 64.0, 6.0) * (1.0 + 2.0 * tremble_), 64.0));
    if (queue_ > 0) {
        const int n = std::min(queue_, 1 + queue_ / 6);
        blow(k, n, false, c, h);
        queue_ -= n;
    }
    if (!beatLine) return;
    // 1. The pops planned for this line (and any overdue). A plan far ahead (the clock jumped back) is re-planned.
    for (Bubble& b : bubbles_) {
        if (b.pop >= 0 || b.popLine == 0) continue;
        if (b.popLine <= k) {
            startPop(b, k, curBeat_, true, mix64(h ^ b.id));
        } else if (b.popLine > k + 8) {
            b.popLine = k + 4;
            b.planLine = k;
        }
    }
    // 2. A drop: many pop at once (no warning) and a burst blows in.
    if (dropArmed_) {
        dropArmed_ = false;
        ++tally_.bursts;
        pick_.clear();
        for (std::size_t i = 0; i < bubbles_.size(); ++i)
            if (bubbles_[i].pop < 0 && bubbles_[i].grow >= 1) pick_.push_back(std::uint32_t(i));
        const int n = int(std::ceil(0.6 * double(pick_.size())));
        for (int i = 0; i < n && !pick_.empty(); ++i) {
            const std::size_t j = std::size_t(rand01(h) * double(pick_.size())) % pick_.size();
            Bubble& b = bubbles_[pick_[j]];
            startPop(b, k, curBeat_, false, mix64(h ^ b.id));
            ++tally_.dropPops;
            pick_[j] = pick_.back();
            pick_.pop_back();
        }
        blow(k, count(2.0 * clampFinite(c.blow, 0.0, 64.0, 6.0) + 4.0, double(kMaxPerLine)), true, c, h);
    }
    // 3. Plan the next beat's pops: Pop of them (fewer while a build holds its breath), every bubble past its Life,
    //    and the extras over Max (a burst may overfill it), the oldest first with a seeded jitter.
    pick_.clear();
    int alive = 0, planned = 0;
    for (std::size_t i = 0; i < bubbles_.size(); ++i) {
        const Bubble& b = bubbles_[i];
        if (b.pop >= 0) continue;
        ++alive;
        if (b.popLine != 0) {
            ++planned;
            continue;
        }
        if (b.grow >= 1) pick_.push_back(std::uint32_t(i));
    }
    const double life = clampFinite(c.life, 1.0, 4096.0, 32.0);
    int want = count(clampFinite(c.pop, 0.0, 16.0, 1.0) * (1.0 - 0.6 * build), 16.0);
    want = std::max(want, alive - planned - settings_.maxBubbles);
    std::uint64_t jh = h;
    auto score = [&](const Bubble& b) {
        std::uint64_t q = mix64(jh ^ b.id);
        const double beats = double(k - b.bornLine) / 4.0;
        return beats * (0.6 + 0.8 * rand01(q));
    };
    for (std::uint32_t i : pick_) {
        Bubble& b = bubbles_[i];
        if (double(k - b.bornLine) / 4.0 >= life) {
            b.popLine = k + 4;
            b.planLine = k;
        }
    }
    std::sort(pick_.begin(), pick_.end(), [&](std::uint32_t a, std::uint32_t b) {
        const double sa = score(bubbles_[a]), sb = score(bubbles_[b]);
        return sa != sb ? sa > sb : bubbles_[a].id < bubbles_[b].id;
    });
    for (std::uint32_t i : pick_) {
        if (want <= 0) break;
        Bubble& b = bubbles_[i];
        if (b.popLine == 0) {
            b.popLine = k + 4;
            b.planLine = k;
        }
        --want;
    }
}

void Sim::substep(const Controls& c) {
    const double h = kSubstep;
    const double t = double(done_) * kSubstep;
    const double air = 0.15 * clampFinite(c.air, 0.0, 8.0, 1.0) * (1.0 - 0.6 * calm_);
    const double rise = 0.04 * clampFinite(c.rise, -4.0, 8.0, 1.0) * (1.0 - 0.5 * calm_);
    // Wind: a breeze whose heading and strength wander slowly (seeded), so the drift is not always up.
    double wx = 0, wy = 0;
    if (const double wind = clampFinite(c.wind, 0.0, 4.0, 0.0); wind > 0) {
        const double s = double(seed32_ % 1000u) * 0.0137;
        const double head = s * 7.0 + 2.1 * std::sin(t * 0.043 + s) + 1.3 * std::sin(t * 0.027 + 2.0 * s);
        const double gust = 0.65 + 0.35 * std::sin(t * 0.11 + 3.0 * s);
        wx = 0.07 * wind * gust * std::cos(head);
        wy = 0.07 * wind * gust * std::sin(head);
    }
    const double A = aspect_;
    const bool edge = settings_.source == Source::Edge;
    // How far each planned pop is (its film drains black from the top over the beat before it).
    const double lineF = curBeat_ * 4.0;
    for (Bubble& b : bubbles_) {
        if (b.pop >= 0) continue;
        if (b.popLine == 0) {
            b.doom = 0;
            continue;
        }
        const double span = std::max(1.0, double(b.popLine - b.planLine));
        b.doom = clampFinite((lineF - double(b.planLine)) / span, 0.0, 1.0, 0.0);
    }
    for (Bubble& b : bubbles_) {
        b.age += h;
        if (b.grow < 1) b.grow = std::min(1.0, b.grow + h / kInflateSeconds);
        b.x += b.vx * h;
        b.y += b.vy * h;
        if (b.pop >= 0) {
            b.pop += h;
            continue;
        }
        const double rd = drawnRadius(b);
        double ax, ay;
        airAt(b.x, b.y, t, seed32_, ax, ay);
        const double size = std::clamp(b.r / (55.0 * kPx), 0.2, 8.0);
        const double tx = ax * air + wx, ty = ay * air + wy - rise * (0.8 + 0.2 * std::min(size, 2.0));
        const double tau = std::clamp(1.0 * std::sqrt(size), 0.3, 3.0);
        const double k = 1.0 - std::exp(-h / tau);
        b.vx += (tx - b.vx) * k;
        b.vy += (ty - b.vy) * k;
        // Soft walls at the sides and the bottom (the edge source's bubbles come up from below it); the top is open.
        if (b.x < rd) b.vx += (rd - b.x) * 8.0 * h;
        if (b.x > A - rd) b.vx -= (b.x - (A - rd)) * 8.0 * h;
        if (!edge && b.y > 1.0 - rd) b.vy -= (b.y - (1.0 - rd)) * 8.0 * h;
        // Shape modes: damped oscillators kicked by the air, a build's tremble and a coming pop.
        const double f2 = modeHz(rd);
        const double base = 0.012 * clampFinite(c.air, 0.0, 8.0, 1.0) + 0.05 * tremble_ + 0.025 * b.doom;
        for (int m = 0; m < 4; ++m) {
            const double f = m < 2 ? f2 : std::min(14.0, f2 * 2.24);
            const double w = 2 * kPi * f, zeta = 0.12;
            const double rate = m < 2 ? 1.7 : 2.9;
            const double drive = base * (m < 2 ? 1.0 : 0.6) *
                                 (2.0 * valueNoise(t * rate * (1.0 + 2.0 * tremble_), double(b.id % 4093) * 3.7 + m * 11.0,
                                                   seed32_ ^ 0x6d6f6465u) -
                                  1.0);
            b.modeV[std::size_t(m)] += h * (-w * w * (b.mode[std::size_t(m)] - drive) - 2 * zeta * w * b.modeV[std::size_t(m)]);
            b.mode[std::size_t(m)] = std::clamp(b.mode[std::size_t(m)] + h * b.modeV[std::size_t(m)], -0.12, 0.12);
        }
    }
    // Contacts: bonded pairs settle where their films meet at 120 degrees; others bounce softly (or stick, seeded).
    const std::size_t n = bubbles_.size();
    for (std::size_t i = 0; i < n; ++i) {
        Bubble& a = bubbles_[i];
        if (a.pop >= 0) continue;
        const double ra = drawnRadius(a);
        for (std::size_t j = i + 1; j < n; ++j) {
            Bubble& b = bubbles_[j];
            if (b.pop >= 0) continue;
            const double rb = drawnRadius(b);
            double dx = b.x - a.x, dy = b.y - a.y;
            const double d2 = dx * dx + dy * dy;
            const double reach = ra + rb;
            if (d2 >= reach * reach * 1.44) continue;
            double d = std::sqrt(d2);
            if (d < 1e-9) {
                dx = 1;
                dy = 0;
                d = 1e-9;
            } else {
                dx /= d;
                dy /= d;
            }
            const double ma = ra * ra, mb = rb * rb, wa = mb / (ma + mb), wb = ma / (ma + mb);
            const double dv = (b.vx - a.vx) * dx + (b.vy - a.vy) * dy;
            bool bonded = false;
            for (auto id : a.bond) bonded = bonded || id == b.id;
            if (bonded) {
                const double target = std::sqrt(std::max(ra * ra + rb * rb - ra * rb, 1e-12));
                const double err = d - target;
                if (d > 1.05 * (ra + rb)) {  // torn apart by the air
                    unbond(a, b.id);
                    unbond(b, a.id);
                    continue;
                }
                const double imp = (40.0 * err + 6.0 * dv) * h;
                a.vx += dx * imp * wa;
                a.vy += dy * imp * wa;
                b.vx -= dx * imp * wb;
                b.vy -= dy * imp * wb;
                continue;
            }
            if (d >= reach) continue;
            // Stick? Once per pair (seeded), only when they meet gently and both have room for another wall.
            if (a.grow >= 1 && b.grow >= 1 && a.bonds() < kMaxBonds && b.bonds() < kMaxBonds && std::fabs(dv) < 0.2) {
                std::uint64_t ph = mix64(settings_.seed ^ (std::uint64_t(std::min(a.id, b.id)) << 32 | std::max(a.id, b.id)));
                if (rand01(ph) < settings_.cluster) {
                    for (auto& s : a.bond)
                        if (s == 0) {
                            s = b.id;
                            break;
                        }
                    for (auto& s : b.bond)
                        if (s == 0) {
                            s = a.id;
                            break;
                        }
                    ++tally_.bonds;
                    continue;
                }
            }
            // A soft bounce: push apart by the overlap, lose most of the approach.
            const double o = reach - d;
            double imp = o * 30.0 * h;
            if (dv < 0) imp += -dv * 0.6;
            a.vx -= dx * imp * wa;
            a.vy -= dy * imp * wa;
            b.vx += dx * imp * wb;
            b.vy += dy * imp * wb;
            // Both squash along the contact (n = 2 mode).
            const double phi = std::atan2(dy, dx);
            // (A force towards a squash of a quarter of the overlap: the mode's spring, so it holds while they touch and rings
            // when they part.)
            for (Bubble* p : {&a, &b}) {
                const double rp = p == &a ? ra : rb;
                const double w = 2 * kPi * modeHz(rp);
                const double sq = 0.25 * std::min(0.2, o / std::max(1e-6, rp));
                p->modeV[0] -= h * w * w * sq * std::cos(2 * phi);
                p->modeV[1] -= h * w * w * sq * std::sin(2 * phi);
            }
        }
    }
    // Droplets: fly, fall, fade.
    for (Droplet& d : droplets_) {
        d.age += h;
        if (d.age <= 0) continue;
        d.vy += 1.4 * h;
        const double drag = std::exp(-1.5 * h);
        d.vx *= drag;
        d.vy *= drag;
        d.x += d.vx * h;
        d.y += d.vy * h;
    }
    droplets_.erase(std::remove_if(droplets_.begin(), droplets_.end(),
                                   [&](const Droplet& d) {
                                       return d.age > d.life || d.y > 1.05 || d.x < -0.05 || d.x > A + 0.05 ||
                                              !std::isfinite(d.x) || !std::isfinite(d.y);
                                   }),
                    droplets_.end());
    // Gone: popped, or drifted out of the frame.
    bool any = false;
    for (Bubble& b : bubbles_) {
        const double rd = drawnRadius(b);
        const bool out = b.y + rd < -0.01 || b.x + rd < -0.02 || b.x - rd > A + 0.02 ||
                         b.y - rd > (edge ? 1.0 + 3.0 * b.r : 1.02) || !std::isfinite(b.x) || !std::isfinite(b.y);
        if (b.pop >= kPopSeconds || out) {
            if (out && b.pop < 0) ++tally_.exited;
            b.r = 0;
            any = true;
        }
    }
    if (any) {
        bubbles_.erase(std::remove_if(bubbles_.begin(), bubbles_.end(), [](const Bubble& b) { return !(b.r > 0); }),
                       bubbles_.end());
        for (Bubble& b : bubbles_)
            for (auto& id : b.bond)
                if (id != 0) {
                    const Bubble* o = find(id);
                    if (!o || o->pop >= 0) {
                        id = 0;
                        ++tally_.unbonds;
                    }
                }
    }
}

void Sim::step(double t, const Controls& c, const Clock& clock) {
    lastDt_ = 0;
    controls_.blow = clampFinite(c.blow, 0.0, 64.0, 6.0);
    controls_.size = clampFinite(c.size, 4.0, 400.0, 55.0);
    controls_.pop = clampFinite(c.pop, 0.0, 16.0, 1.0);
    controls_.life = clampFinite(c.life, 1.0, 4096.0, 32.0);
    controls_.air = clampFinite(c.air, 0.0, 8.0, 1.0);
    controls_.rise = clampFinite(c.rise, -4.0, 8.0, 1.0);
    controls_.wind = clampFinite(c.wind, 0.0, 4.0, 0.0);
    // The beat: the clock while it is on (junk clamped), else 120 BPM from the start.
    const double age = std::isfinite(t) ? std::clamp(t - start_, 0.0, 1e8) : simTime();
    const bool on = clock.on && std::isfinite(clock.beat) && std::fabs(clock.beat) < 1e9;
    Clock clk = clock;
    if (!on) {
        clk.beat = 2.0 * age;
        clk.bar = clk.beat / 4.0;
        clk.phrase = clk.bar / 16.0;
    } else {
        clk.bar = clampFinite(clock.bar, -1e9, 1e9, clock.beat / 4.0);
        clk.phrase = clampFinite(clock.phrase, -1e9, 1e9, clk.bar / 16.0);
    }
    clk.build = clampFinite(clock.build, 0.0, 1.0, 0.0);
    clk.drop = clampFinite(clock.drop, 0.0, 1.0, 0.0);
    // A build: more bubbles, trembling (rising with it); a breakdown's start: slow and big.
    tremble_ = clk.build * clk.build;
    calm_ = std::min(1.0, 4.0 * clk.build) * (1.0 - clk.build * clk.build);
    const double beatNow = clk.beat;
    const auto target = std::int64_t(std::floor(age / kSubstep));
    const auto lineNow = std::int64_t(std::floor(beatNow * 4.0));
    if (first_) {
        first_ = false;
        done_ = target;
        lastBeat_ = beatNow;
        nextLine_ = lineNow + 1;
        lastDrop_ = clk.drop;
    }
    // Lines: bounded catch-up; a clock that jumped back (or far ahead) re-bases.
    if (lineNow < nextLine_ - 8) nextLine_ = lineNow + 1;
    if (lineNow - nextLine_ + 1 > kMaxLines) nextLine_ = lineNow - kMaxLines + 1;
    std::int64_t n = target - done_;
    if (n < 0) {
        done_ = target;
        n = 0;
    }
    if (n > kMaxCatchUp) {
        done_ = target - kMaxCatchUp;
        n = kMaxCatchUp;
    }
    const double b0 = std::fabs(beatNow - lastBeat_) < 64.0 ? lastBeat_ : beatNow;
    for (std::int64_t i = 1; i <= n; ++i) {
        // Without a clock the beat is a function of the substep alone (the same at any frame rate).
        const double b = on ? b0 + (beatNow - b0) * double(i) / double(n) : 2.0 * double(done_ + 1) * kSubstep;
        curBeat_ = b;
        const auto upTo = std::int64_t(std::floor(b * 4.0));
        while (nextLine_ <= upTo) {
            visitLine(nextLine_, b, clk, controls_, 0.0);
            ++nextLine_;
        }
        substep(controls_);
        ++done_;
    }
    lastBeat_ = beatNow;
    lastDt_ = double(n) * kSubstep;
}

}  // namespace reforce::engine::bubbles
