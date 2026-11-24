// reforce extension: the rain object's simulation. See Rain.hpp and docs/3d-design.md "Rain on glass".
#include "engine/Rain.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace reforce::engine::rain {

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

// The GPU's puddle noise: a 32-bit lattice hash (lowbias32), smoothstep-interpolated value noise.
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

}  // namespace

std::optional<Mode> parseMode(std::string_view s) {
    std::string t;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"') t += char(std::tolower(static_cast<unsigned char>(c)));
    if (t.empty() || t == "window" || t == "0") return Mode::Window;
    if (t == "floor" || t == "1") return Mode::Floor;
    return std::nullopt;
}

const char* modeName(Mode m) { return m == Mode::Floor ? "floor" : "window"; }

double volumeOf(double r, double degrees) {
    const double th = std::clamp(degrees, 5.0, 90.0) * kPi / 180.0;
    const double c = std::cos(th), s = std::sin(th);
    return kPi * r * r * r * (2 + c) * (1 - c) * (1 - c) / (3 * s * s * s);
}

double radiusOf(double volume, double degrees) {
    const double unit = volumeOf(1.0, degrees);
    return volume > 0 ? std::cbrt(volume / unit) : 0.0;
}

double puddle(double x, double y, double wet, std::uint32_t seed) {
    wet = clampFinite(wet, 0.0, 1.0, 0.0);
    if (wet <= 0.001) return 0.0;
    if (wet >= 0.999) return 1.0;
    const double n = 0.65 * valueNoise(x * 2.2, y * 2.2, seed) + 0.35 * valueNoise(x * 5.1 + 17.0, y * 5.1 + 3.0, seed);
    const double thr = 0.5 + (0.5 - wet) * 0.6;
    const double e0 = thr - 0.025, e1 = thr + 0.025;
    const double t = std::clamp((n - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

WaveStep waveStep(double dt, double speed, double decay, int rows) {
    WaveStep w;
    dt = clampFinite(dt, 0.0, 0.25, 0.0);
    speed = clampFinite(speed, 0.0, 4.0, 0.3);
    decay = clampFinite(decay, 0.02, 60.0, 1.5);
    rows = std::clamp(rows, 8, 8192);
    const double cells = speed * double(rows) * dt;  // cells travelled this step
    // 8 c2 + 64 b + 16 nu <= 4 at the largest c: 8 (.28^2) (1 + 8 * .5) + 16 (.012) = 3.33.
    constexpr double kMaxC = 0.28;
    w.substeps = std::clamp(int(std::ceil(cells / kMaxC)), 1, kMaxWaveSubsteps);
    const double c = std::min(kMaxC, cells / double(w.substeps));
    w.c2 = c * c;
    w.b = 0.5 * w.c2;                   // capillary dispersion (cells^2): short ripples run ahead, rings come in trains
    w.nu = 0.012 * (c / kMaxC);         // viscosity: grid-scale chatter dies, the rings live
    w.damp = dt > 0 ? std::exp(-dt / double(w.substeps) / decay) : 1.0;
    return w;
}

WaveGrid::WaveGrid(int cols, int rows)
    : cols_(std::max(4, cols)), rows_(std::max(4, rows)),
      cur_(std::size_t(cols_ * rows_), 0.0), prev_(cur_), next_(cur_), lap_(cur_) {}

void WaveGrid::substep(const WaveStep& w, const std::vector<Impulse>& impulses, const std::vector<float>* wetMask) {
    auto idx = [&](int x, int y) {
        return std::size_t(std::clamp(y, 0, rows_ - 1) * cols_ + std::clamp(x, 0, cols_ - 1));
    };
    // The Laplacian of the current heights (clamped edges), then of the velocity term.
    for (int y = 0; y < rows_; ++y)
        for (int x = 0; x < cols_; ++x)
            lap_[idx(x, y)] = cur_[idx(x + 1, y)] + cur_[idx(x - 1, y)] + cur_[idx(x, y + 1)] + cur_[idx(x, y - 1)] -
                              4.0 * cur_[idx(x, y)];
    for (int y = 0; y < rows_; ++y)
        for (int x = 0; x < cols_; ++x) {
            const std::size_t i = idx(x, y);
            const double l2 = lap_[idx(x + 1, y)] + lap_[idx(x - 1, y)] + lap_[idx(x, y + 1)] + lap_[idx(x, y - 1)] -
                              4.0 * lap_[i];
            const double v = cur_[i] - prev_[i];
            const double lv = (cur_[idx(x + 1, y)] - prev_[idx(x + 1, y)]) + (cur_[idx(x - 1, y)] - prev_[idx(x - 1, y)]) +
                              (cur_[idx(x, y + 1)] - prev_[idx(x, y + 1)]) + (cur_[idx(x, y - 1)] - prev_[idx(x, y - 1)]) -
                              4.0 * v;
            double n = (2.0 * cur_[i] - prev_[i] + w.c2 * lap_[i] - w.b * l2 + w.nu * lv) * w.damp;
            // The dry glass between puddles holds the water at rest, so ripples bounce off a puddle's edge.
            if (wetMask && !wetMask->empty()) n *= double((*wetMask)[i]);
            for (const Impulse& p : impulses) {
                const double dx = (x + 0.5) - p.x * rows_, dy = (y + 0.5) - p.y * rows_;
                const double rr = std::max(1.0, p.radius * rows_);
                const double q2 = (dx * dx + dy * dy) / (rr * rr);
                n -= p.amp * rows_ * (1.0 - q2) * std::exp(-q2);  // a crater and its rim: no net push
            }
            next_[i] = n;
        }
    std::swap(prev_, cur_);
    std::swap(cur_, next_);
}

double WaveGrid::maxAbs() const {
    double m = 0;
    for (double v : cur_) m = std::max(m, std::isfinite(v) ? std::fabs(v) : 1e300);
    return m;
}

// ---------------------------------------------------------------- Sim

Sim::Sim(const Settings& s, double aspect, double startTime) : settings_(s) {
    aspect_ = clampFinite(aspect, 0.2, 8.0, 16.0 / 9.0);
    start_ = std::isfinite(startTime) ? startTime : 0.0;
    contact_ = clampFinite(s.contact, 20.0, 85.0, 62.0);
    settings_.contact = contact_;
    settings_.slide = clampFinite(s.slide, 2.0, 200.0, 17.0);
    settings_.maxDrops = std::clamp(s.maxDrops, 16, 4000);
    drops_.reserve(std::size_t(settings_.maxDrops) + 2 * kMaxPerLine * kMaxLines + 64);
    order_.reserve(drops_.capacity());
    impulses_.reserve(kMaxImpulses);
    wipes_.reserve(kMaxWipes);
    lines_.reserve(256);
    wetRows_ = 96;
    wetCols_ = std::max(8, int(std::lround(96.0 * aspect_)));
    wet_.assign(std::size_t(wetCols_ * wetRows_), -1.0f);  // never wet
}

double Sim::rand01(std::uint64_t& h) const {
    h = mix64(h);
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}

double Sim::volume() const {
    double v = 0;
    for (const Drop& d : drops_) v += volumeOf(d.r, contact_);
    return v;
}

void Sim::addDrop(double x, double y, double r, bool impact, std::uint64_t h) {
    if (!(r > 0)) return;
    const double A = aspect_;
    const double v0 = volumeOf(r, contact_);
    Drop d;
    d.id = nextId_++;
    d.r = r;
    d.x = std::clamp(x, r, std::max(r, A - r));
    d.y = std::clamp(y, r, std::max(r, 1.0 - r));
    d.wx = d.x;
    d.wy = d.y;
    d.slideAt = settings_.slide * kPx * (0.82 + 0.36 * rand01(h));
    d.age = impact ? 0.0 : 1.0;
    d.pinCell = std::int32_t(std::floor(d.y * 160.0));
    if (impact) tally_.spawned += v0;
    if (settings_.mode == Mode::Floor) {
        const std::uint32_t seed32 = std::uint32_t(settings_.seed ^ (settings_.seed >> 32));
        const bool inPuddle = puddle(d.x, d.y, controls_.wet, seed32) > 0.5;
        if (!impact) {
            if (inPuddle) {
                tally_.pooled += v0;
                if (int(impulses_.size()) < kMaxImpulses) impulses_.push_back({d.x, d.y, std::max(1.5 * kPx, r), 0.8 * r});
                return;
            }
            drops_.push_back(d);
            return;
        }
        // A strike: a crown of droplets thrown out (their volume taken from the drop), then the drop itself into the
        // puddle (an impulse) or onto the dry glass (a bead).
        double rest = v0;
        const int crown = r > 3.0 * kPx ? 5 + int(rand01(h) * 6.0) : 2;
        const double phase = rand01(h) * 2 * kPi;
        for (int i = 0; i < crown; ++i) {
            const double rc = r * (0.11 + 0.1 * rand01(h));
            const double vc = volumeOf(rc, contact_);
            if (vc > 0.3 * rest) break;
            const double a = phase + 2 * kPi * (i + 0.3 * rand01(h)) / crown;
            const double T = 0.08 + 0.14 * rand01(h);
            const double L = r * (inPuddle ? 2.5 + 4.5 * rand01(h) : 1.5 + 2.5 * rand01(h));
            Drop c;
            c.id = nextId_++;
            c.r = rc;
            c.x = d.x + std::cos(a) * r * 0.8;
            c.y = d.y + std::sin(a) * r * 0.8;
            c.vx = std::cos(a) * L / T;
            c.vy = std::sin(a) * L / T;
            c.flight = T;
            c.age = 1.0;
            c.wx = c.x;
            c.wy = c.y;
            drops_.push_back(c);
            rest -= vc;
        }
        if (inPuddle) {
            tally_.pooled += rest;
            if (int(impulses_.size()) < kMaxImpulses)
                impulses_.push_back({d.x, d.y, std::max(1.5 * kPx, 0.9 * r), std::min(0.8 * r, 9.0 * kPx)});
            return;
        }
        d.r = radiusOf(rest, contact_);
        drops_.push_back(d);
        return;
    }
    // Window: a few micro droplets splashed around a fresh drop (their volume from it).
    if (impact && r > 4.0 * kPx) {
        double rest = v0;
        const int n = 1 + int(rand01(h) * 4.0);
        for (int i = 0; i < n; ++i) {
            const double rr = r * (0.1 + 0.12 * rand01(h));
            const double vr = volumeOf(rr, contact_);
            if (vr > 0.06 * v0) continue;
            const double a = rand01(h) * 2 * kPi, dist = r * (1.5 + 2.2 * rand01(h));
            Drop m;
            m.id = nextId_++;
            m.r = rr;
            m.x = std::clamp(d.x + std::cos(a) * dist, rr, A - rr);
            m.y = std::clamp(d.y + std::sin(a) * dist, rr, 1.0 - rr);
            m.wx = m.x;
            m.wy = m.y;
            m.slideAt = d.slideAt;
            m.age = 0.5;
            m.pinCell = std::int32_t(std::floor(m.y * 160.0));
            drops_.push_back(m);
            rest -= vr;
        }
        d.r = radiusOf(rest, contact_);
    }
    drops_.push_back(d);
}

void Sim::place(double x, double y, double r) {
    if (!std::isfinite(x) || !std::isfinite(y) || !(r > 0) || !std::isfinite(r)) return;
    r = std::min(r, 0.25);
    tally_.spawned += volumeOf(r, contact_);
    addDrop(x, y, r, false, mix64(settings_.seed ^ nextId_));  // (floor: into a puddle, addDrop pools it)
}

void Sim::spawnLine(std::int64_t k, double beatAt, const Clock& clock, const Controls& c) {
    ++tally_.lines;
    if (lines_.size() >= 256) lines_.erase(lines_.begin(), lines_.begin() + 128);
    lines_.push_back(k);
    std::uint64_t h = mix64(settings_.seed * 0x9e3779b97f4a7c15ull ^ std::uint64_t(k) * 0xd6e8feb86659fd93ull);
    const double build = clampFinite(clock.build, 0.0, 1.0, 0.0);
    const double drop = clampFinite(clock.drop, 0.0, 1.0, 0.0);
    const double rMean = clampFinite(c.size, 0.5, 60.0, 11.0) * kPx;
    const auto beatLine = ((k % 4) + 4) % 4 == 0;
    // Bar and phrase lines: the clock's bar / phrase at this line (linear from the step's values).
    bool barLine = false, phraseLine = false;
    if (beatLine) {
        const double bar = clock.bar + (double(k) / 4.0 - beatAt) / 4.0;
        barLine = std::fabs(bar - std::round(bar)) < 0.02;
        if (barLine) {
            const double perPhrase = clock.phrase > 1e-9 ? std::clamp(std::round(clock.bar / clock.phrase), 1.0, 64.0) : 16.0;
            const double phrase = clock.phrase + (std::round(bar) - clock.bar) / perPhrase;
            phraseLine = std::fabs(phrase - std::round(phrase)) < 0.02;
        }
    }
    auto count = [&](double expected) {
        expected = clampFinite(expected, 0.0, double(kMaxPerLine), 0.0);
        const double f = std::floor(expected);
        return int(f) + (rand01(h) < expected - f ? 1 : 0);
    };
    auto size = [&](double scale) {
        // Lognormal around the mean (Box-Muller).
        const double u1 = std::max(1e-12, rand01(h)), u2 = rand01(h);
        const double g = std::sqrt(-2.0 * std::log(u1)) * std::cos(2 * kPi * u2);
        return std::clamp(rMean * scale * std::exp(0.45 * g), 0.6 * kPx, 80.0 * kPx);
    };
    const double A = aspect_;
    int spawned = 0;
    auto place = [&](double r, bool impact) {
        if (spawned >= kMaxPerLine) return;
        ++spawned;
        ++tally_.drops;
        const double x = r + rand01(h) * std::max(0.0, A - 2 * r);
        const double y = r + rand01(h) * std::max(0.0, 1.0 - 2 * r);
        addDrop(x, y, r, impact, h);
        h = mix64(h);
    };
    if (beatLine) {
        const int n = count(clampFinite(c.density, 0.0, 40.0, 4.0) * (1.0 - 0.55 * build) * (1.0 + 5.0 * drop));
        for (int i = 0; i < n; ++i) place(size(1.0), true);
        const int clusters = count(clampFinite(c.cluster, 0.0, 8.0, 1.0) * (barLine ? 1.0 + drop : 0.0) + (drop > 0.3 ? 1.0 : 0.0));
        for (int i = 0; i < clusters && spawned < kMaxPerLine; ++i) {
            // One big drop and a ring of satellites.
            const double r = size(1.9);
            const double x = r + rand01(h) * std::max(0.0, A - 2 * r);
            const double y = r + rand01(h) * std::max(0.0, 0.8 - 2 * r);
            ++spawned;
            ++tally_.drops;
            addDrop(x, y, r, true, h);
            const int sat = 3 + int(rand01(h) * 5.0);
            for (int j = 0; j < sat && spawned < kMaxPerLine; ++j) {
                const double a = rand01(h) * 2 * kPi, dist = r * (2.4 + 3.0 * rand01(h));
                ++spawned;
                ++tally_.drops;
                addDrop(x + std::cos(a) * dist, y + std::sin(a) * dist, size(0.45), true, h);
            }
        }
        if (phraseLine && settings_.mode == Mode::Window && settings_.wipe) {
            cascade_ = 0.5;  // a beat at 120 BPM: big drops let go together
            ++tally_.cascades;
        }
    } else {
        const int n = count(clampFinite(c.drizzle, 0.0, 40.0, 1.0) * (1.0 + 3.0 * build) * (1.0 + 2.0 * drop));
        for (int i = 0; i < n; ++i) place(size(0.42), true);
    }
}

void Sim::slide(Drop& d, double h, Drop& residual) {
    const double thr = d.slideAt * (cascade_ > 0 ? 0.4 : 1.0);
    const double ratio = thr / std::max(d.r, 1e-9);
    std::uint64_t rh = mix64(settings_.seed ^ (std::uint64_t(d.id) << 20));
    if (d.pinned > 0) {
        d.pinned -= h;
        d.vy *= std::exp(-h / 0.012);
        d.vx *= std::exp(-h / 0.012);
    } else {
        const double drive = 2.2 * std::max(0.0, 1.0 - ratio * ratio) + 0.05;
        d.vy += drive * h;
        d.vy -= d.vy * 2.0 * h;  // drag
        d.vy = std::clamp(d.vy, 0.0, 1.1 * std::sqrt(std::max(1.0, 1.0 / std::max(ratio, 0.2))));
    }
    // Stick-slip: each 1/160 frame height of glass may hold the drop for a moment (small drops more often).
    const auto cell = std::int32_t(std::floor(d.y * 160.0));
    if (cell != d.pinCell) {
        d.pinCell = cell;
        std::uint64_t ch = mix64(rh ^ std::uint64_t(std::uint32_t(cell)) * 0x2545f4914f6cdd1dull);
        const double p = std::clamp(0.08 * ratio * ratio, 0.01, 0.35);
        if (rand01(ch) < p) d.pinned = 0.025 + 0.22 * rand01(ch);
    }
    // Sideways: a meander of its own plus a pull towards wet glass below (earlier trails).
    const double s = d.y * 7.0 + double(d.id % 997) * 0.37;
    const auto i0 = std::int32_t(std::floor(s));
    std::uint64_t a = mix64(rh ^ std::uint64_t(std::uint32_t(i0))), b = mix64(rh ^ std::uint64_t(std::uint32_t(i0 + 1)));
    const double f = s - std::floor(s), fs = f * f * (3 - 2 * f);
    const double meander = (rand01(a) * 2 - 1) * (1 - fs) + (rand01(b) * 2 - 1) * fs;
    const double now = double(done_) * kSubstep;
    auto wetAt = [&](double x, double y) {
        const int cx = std::clamp(int(x / aspect_ * wetCols_), 0, wetCols_ - 1);
        const int cy = std::clamp(int(y * wetRows_), 0, wetRows_ - 1);
        const double stamp = double(wet_[std::size_t(cy * wetCols_ + cx)]);
        return stamp < 0 ? 0.0 : std::exp(-(now - stamp) / 10.0);  // a trail dries over ~10 s
    };
    const double look = std::max(d.r * 2.5, 2.0 / wetRows_);
    const double wl = wetAt(d.x - look, d.y + look), wr = wetAt(d.x + look, d.y + look);
    const double steer = (wr - wl) / (wr + wl + 0.2);
    const double target = d.vy * (0.42 * meander + 0.5 * steer);
    d.vx += (target - d.vx) * (1 - std::exp(-h / 0.06));
    const double ox = d.x, oy = d.y;
    d.x += d.vx * h;
    d.y += d.vy * h;
    if (d.x < d.r || d.x > aspect_ - d.r) {
        d.x = std::clamp(d.x, d.r, std::max(d.r, aspect_ - d.r));
        d.vx = 0;
    }
    const double dist = std::hypot(d.x - ox, d.y - oy);
    // The glass it ran over is wet.
    {
        const int cx = std::clamp(int(d.x / aspect_ * wetCols_), 0, wetCols_ - 1);
        const int cy = std::clamp(int(d.y * wetRows_), 0, wetRows_ - 1);
        wet_[std::size_t(cy * wetCols_ + cx)] = float(now);
    }
    if (dist <= 0) return;
    // A film stays behind (about 6 % of the volume per frame height run) and every so often a residual droplet.
    double v = volumeOf(d.r, contact_);
    const double film = std::min(0.5 * v, 0.06 * v * dist);
    v -= film;
    tally_.film += film;
    d.trailLeft -= dist;
    if (d.trailLeft <= 0) {
        d.trailLeft = d.r * (0.9 + 3.0 * rand01(rh));
        const double rr = d.r * (0.14 + 0.16 * rand01(rh));
        const double vr = volumeOf(rr, contact_);
        if (vr < 0.25 * v) {
            Drop& m = residual;
            m.id = nextId_++;
            m.r = rr;
            m.x = std::clamp(d.x + (rand01(rh) - 0.5) * d.r, rr, aspect_ - rr);
            m.y = std::max(rr, d.y - d.r * d.stretch * (1.0 + 0.4 * rand01(rh)) - rr);
            m.wx = m.x;
            m.wy = m.y;
            m.age = 0.3;
            m.slideAt = d.slideAt;
            m.pinCell = std::int32_t(std::floor(m.y * 160.0));
            v -= vr;
            ++tally_.residuals;
        }
    }
    d.r = radiusOf(v, contact_);
}

void Sim::mergeAll() {
    const std::size_t n = drops_.size();
    order_.resize(n);
    for (std::size_t i = 0; i < n; ++i) order_[i] = std::uint32_t(i);
    auto extent = [&](const Drop& d) { return d.r * (1.0 + 0.5 * (d.stretch - 1.0)); };
    std::sort(order_.begin(), order_.end(), [&](std::uint32_t a, std::uint32_t b) {
        const double xa = drops_[a].x - extent(drops_[a]), xb = drops_[b].x - extent(drops_[b]);
        return xa != xb ? xa < xb : drops_[a].id < drops_[b].id;
    });
    bool any = false;
    for (std::size_t oi = 0; oi < n; ++oi) {
        Drop& a = drops_[order_[oi]];
        if (!(a.r > 0) || a.flight > 0) continue;
        for (std::size_t oj = oi + 1; oj < n; ++oj) {
            Drop& b = drops_[order_[oj]];
            if (b.x - extent(b) > a.x + extent(a) + 1e-12) break;
            if (!(b.r > 0) || b.flight > 0) continue;
            const double ea = extent(a), eb = extent(b);
            const double dx = b.x - a.x, dy = b.y - a.y;
            if (dx * dx + dy * dy >= (ea + eb) * (ea + eb)) continue;
            // Merge into the bigger one: volume conserved, volume-weighted center and momentum, a wobble along the
            // line between them.
            Drop& big = a.r >= b.r ? a : b;
            Drop& small = a.r >= b.r ? b : a;
            const double vb = volumeOf(big.r, contact_), vs = volumeOf(small.r, contact_), vt = vb + vs;
            const double ax = std::atan2(small.y - big.y, small.x - big.x);
            big.x = (big.x * vb + small.x * vs) / vt;
            big.y = (big.y * vb + small.y * vs) / vt;
            big.vx = (big.vx * vb + small.vx * vs) / vt;
            big.vy = (big.vy * vb + small.vy * vs) / vt;
            big.r = radiusOf(vt, contact_);
            big.x = std::clamp(big.x, big.r, std::max(big.r, aspect_ - big.r));
            if (settings_.mode == Mode::Window) big.y = std::max(big.y, big.r);
            else big.y = std::clamp(big.y, big.r, std::max(big.r, 1.0 - big.r));
            const double amp = 0.5 * std::cbrt(vs / vt);
            if (amp >= big.wobble * std::exp(-big.wobbleT / 0.2)) {
                big.wobble = amp;
                big.wobbleAxis = ax;
                big.wobbleT = 0;
            }
            big.sliding = big.sliding || small.sliding;
            if (small.sliding && !big.sliding) big.trailLeft = small.trailLeft;
            small.r = 0;  // gone
            ++tally_.merges;
            any = true;
            if (&small == &a) break;
        }
    }
    if (any) drops_.erase(std::remove_if(drops_.begin(), drops_.end(), [](const Drop& d) { return !(d.r > 0); }),
                          drops_.end());
}

void Sim::bound() {
    const std::size_t cap = std::size_t(settings_.maxDrops);
    if (drops_.size() <= cap) return;
    // The smallest evaporate (ties: the oldest id first).
    std::sort(drops_.begin(), drops_.end(), [](const Drop& a, const Drop& b) { return a.r != b.r ? a.r > b.r : a.id > b.id; });
    for (std::size_t i = cap; i < drops_.size(); ++i) tally_.evaporated += volumeOf(drops_[i].r, contact_);
    drops_.resize(cap);
    std::sort(drops_.begin(), drops_.end(), [](const Drop& a, const Drop& b) { return a.id < b.id; });
}

void Sim::substep(const Controls& c) {
    const double h = kSubstep;
    const bool window = settings_.mode == Mode::Window;
    const std::uint32_t seed32 = std::uint32_t(settings_.seed ^ (settings_.seed >> 32));
    for (std::size_t i = 0; i < drops_.size(); ++i) {
        Drop& d = drops_[i];
        d.age += h;
        d.wobbleT += h;
        if (d.flight > 0) {  // floor: a crown droplet in the air
            d.x += d.vx * h;
            d.y += d.vy * h;
            d.flight -= h;
            if (d.x < 0 || d.y < 0 || d.x > aspect_ || d.y > 1.0) {
                tally_.exited += volumeOf(d.r, contact_);
                d.r = 0;
                continue;
            }
            if (d.flight <= 0) {
                d.flight = 0;
                d.vx = d.vy = 0;
                d.age = 0.6;
                d.x = std::clamp(d.x, d.r, std::max(d.r, aspect_ - d.r));
                d.y = std::clamp(d.y, d.r, std::max(d.r, 1.0 - d.r));
                if (puddle(d.x, d.y, c.wet, seed32) > 0.5) {
                    tally_.pooled += volumeOf(d.r, contact_);
                    if (int(impulses_.size()) < kMaxImpulses)
                        impulses_.push_back({d.x, d.y, std::max(1.2 * kPx, 1.4 * d.r), 1.2 * d.r});
                    d.r = 0;
                }
            }
            continue;
        }
        if (window) {
            const double thr = d.slideAt * (cascade_ > 0 ? 0.4 : 1.0);
            if (!d.sliding && d.r > thr) {
                d.sliding = true;
                d.pinned = 0;
                d.trailLeft = d.r * (0.8 + 1.5 * double(d.id % 7) / 7.0);
                ++tally_.slides;
            } else if (d.sliding && d.r < 0.72 * thr && cascade_ <= 0) {
                d.sliding = false;
                d.vx = d.vy = 0;
            }
            Drop residual;
            if (d.sliding) slide(d, h, residual);
            if (residual.r > 0) drops_.push_back(residual);  // (d may move with the vector)
            Drop& e = drops_[i];
            const double target = 1.0 + std::min(0.75, e.vy * 2.6);
            e.stretch += (target - e.stretch) * (1 - std::exp(-h / 0.05));
            if (e.y - e.r * e.stretch > 1.0) {  // off the bottom
                tally_.exited += volumeOf(e.r, contact_);
                tally_.exitMinY = std::min(tally_.exitMinY, e.y);
                e.r = 0;
                continue;
            }
            // Tiny residuals dry up slowly.
            if (e.r < 1.3 * kPx) {
                const double nr = e.r - 0.25 * kPx * h;
                const double keep = nr > 0.45 * kPx ? nr : 0.0;
                tally_.evaporated += volumeOf(e.r, contact_) - volumeOf(keep, contact_);
                e.r = keep;
            }
        } else {
            // Floor beads dry slowly; a puddle reaching one takes it in.
            const double nr = d.r - 0.12 * kPx * h;
            const double keep = nr > 0.45 * kPx ? nr : 0.0;
            tally_.evaporated += volumeOf(d.r, contact_) - volumeOf(keep, contact_);
            d.r = keep;
            if (d.r > 0 && puddle(d.x, d.y, c.wet, seed32) > 0.6) {
                tally_.pooled += volumeOf(d.r, contact_);
                d.r = 0;
            }
        }
    }
    drops_.erase(std::remove_if(drops_.begin(), drops_.end(), [](const Drop& d) { return !(d.r > 0); }), drops_.end());
    mergeAll();
    if (cascade_ > 0) cascade_ -= h;
}

void Sim::step(double t, const Controls& c, const Clock& clock) {
    impulses_.clear();
    wipes_.clear();
    lastDt_ = 0;
    controls_ = c;
    controls_.wet = clampFinite(c.wet, 0.0, 1.0, 0.65);
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
    const double beatNow = clk.beat;
    const auto target = std::int64_t(std::floor(age / kSubstep));
    const auto lineNow = std::int64_t(std::floor(beatNow * 4.0));
    if (first_) {
        first_ = false;
        done_ = target;
        lastBeat_ = beatNow;
        nextLine_ = lineNow + 1;
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
    for (Drop& d : drops_) {
        d.wx = d.x;
        d.wy = d.y;
    }
    const double b0 = std::fabs(beatNow - lastBeat_) < 64.0 ? lastBeat_ : beatNow;
    for (std::int64_t i = 1; i <= n; ++i) {
        // Without a clock the beat is a function of the substep alone (the same at any frame rate).
        const double b = on ? b0 + (beatNow - b0) * double(i) / double(n) : 2.0 * double(done_ + 1) * kSubstep;
        const auto upTo = std::int64_t(std::floor(b * 4.0));
        while (nextLine_ <= upTo) {
            spawnLine(nextLine_, b, clk, controls_);
            ++nextLine_;
        }
        substep(controls_);
        ++done_;
    }
    bound();
    lastBeat_ = beatNow;
    lastDt_ = double(n) * kSubstep;
    // This step's wipes (each running drop's path).
    if (settings_.mode == Mode::Window) {
        for (const Drop& d : drops_) {
            if (!d.sliding || int(wipes_.size()) >= kMaxWipes) continue;
            if (std::hypot(d.x - d.wx, d.y - d.wy) < 1e-7 && d.pinned <= 0) continue;
            wipes_.push_back({d.wx, d.wy, d.x, d.y, d.r * 0.85});
        }
    }
}

}  // namespace reforce::engine::rain
