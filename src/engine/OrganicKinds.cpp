#include "engine/OrganicKinds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "engine/Dragon.hpp"
#include "engine/arcade/Arcade.hpp"

#include "expr/Rng.hpp"

namespace reforce::engine {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr int kMaxCatchUp = 8;  // generations a step may advance (automaton, life)

double finiteOr(double v, double fb) { return std::isfinite(v) ? v : fb; }
double num(const scene::OrganicInstance& in, const char* key, double fb, double lo, double hi) {
    return std::clamp(finiteOr(in.loaded(key, fb), fb), lo, hi);
}
int count(const scene::OrganicInstance& in, const char* key, double fb, int lo, int hi) {
    return int(std::lround(num(in, key, fb, lo, hi)));
}

// ------------------------------------------------------------------ phyllotaxis
class Phyllotaxis : public OrganicBody {
public:
    Phyllotaxis(scene::OrganicInstance& in)
        : num_(count(in, "NUM", 800, 1, 5000)), rate_(num(in, "RATE", 4, 0.01, 64)), spc_(num(in, "SPC", 0.032, 0, 1)),
          dotw_(num(in, "DOTW", 6, 0, 200)), inst_(in) {}
    void step(const OrganicStep& s, OrganicCanvas& c) override {
        if (beat0_ < -1e300 || s.beat < beat0_) beat0_ = s.beat;
        const double seeds = std::min(double(num_), std::max(0.0, s.beat - beat0_) * rate_);
        const double ang = finiteOr(inst_.frameKey("ANG", 137.50776405), 137.50776405) * kPi / 180.0;
        const int n = int(std::ceil(seeds));
        for (int i = 0; i < n; ++i) {
            const double grown = std::min(1.0, seeds - i);  // the newest seed swells in
            const double ageBeats = (seeds - i) / rate_;
            const double r = spc_ * std::sqrt(double(i)), a = double(i) * ang;
            const double size = dotw_ * grown * (1.0 + 1.2 * std::exp(-3.0 * ageBeats));
            c.dot(r * std::cos(a), r * std::sin(a), size, s.pen);
        }
        seeds_ = seeds;
    }
    std::string summary() const override {
        char b[64];
        std::snprintf(b, sizeof b, "%d seeds at %g per beat", num_, rate_);
        return b;
    }

private:
    int num_;
    double rate_, spc_, dotw_;
    scene::OrganicInstance& inst_;
    double beat0_ = -1e308, seeds_ = 0;
};

// ------------------------------------------------------------------ harmonograph
class Harmonograph : public OrganicBody {
public:
    Harmonograph(scene::OrganicInstance& in)
        : stps_(count(in, "STPS", 600, 8, 8000)), len_(num(in, "LEN", 1, 0.01, 64)), wdth_(num(in, "WDTH", 2, 0, 200)),
          inst_(in) {}
    void step(const OrganicStep& s, OrganicCanvas& c) override {
        const double fx = finiteOr(inst_.frameKey("FX", 3), 3), fy = finiteOr(inst_.frameKey("FY", 2), 2);
        const double phx = finiteOr(inst_.frameKey("PHX", 0), 0), phy = finiteOr(inst_.frameKey("PHY", kPi / 2), kPi / 2);
        const double ax = finiteOr(inst_.frameKey("AX", 1.2), 1.2), ay = finiteOr(inst_.frameKey("AY", 0.8), 0.8);
        const double damp = std::max(0.0, finiteOr(inst_.frameKey("DAMP", 0), 0));
        double px = 0, py = 0, pp = 0;
        for (int k = 0; k <= stps_; ++k) {
            const double u = double(k) / double(stps_);  // 0 = tail, 1 = head
            const double back = len_ * (1.0 - u);        // bars behind the head
            const double b = s.bar - back;
            const double e = std::exp(-damp * back);
            const double x = ax * e * std::sin(2 * kPi * fx * b + phx), y = ay * e * std::sin(2 * kPi * fy * b + phy);
            const double pen = s.pen * std::pow(u, 0.7);
            if (k > 0) c.line(px, py, x, y, wdth_, pp, pen);
            px = x;
            py = y;
            pp = pen;
        }
    }
    std::string summary() const override {
        char b[64];
        std::snprintf(b, sizeof b, "%d steps over %g bar(s)", stps_, len_);
        return b;
    }

private:
    int stps_;
    double len_, wdth_;
    scene::OrganicInstance& inst_;
};

// Grid layout shared by the cellular automata: cols x rows cells, centered, at most 3.3 x 1.85 tree units.
struct Grid {
    int cols = 1, rows = 1;
    double cell = 0.05, x0 = 0, y0 = 0;
    Grid(int c, int r) : cols(c), rows(r) {
        cell = std::min(3.3 / cols, 1.85 / rows);
        x0 = -0.5 * cell * (cols - 1);
        y0 = 0.5 * cell * (rows - 1);  // row 0 at the top
    }
    double x(int c) const { return x0 + cell * c; }
    double y(double r) const { return y0 - cell * r; }
};

// ------------------------------------------------------------------ 1D automaton
class Automaton : public OrganicBody {
public:
    Automaton(scene::OrganicInstance& in, std::uint64_t seed)
        : cols_(count(in, "COLS", 101, 3, 400)), rows_(count(in, "ROWS", 56, 2, 300)),
          rule_(count(in, "RULE", 90, 0, 255)), rate_(num(in, "RATE", 2, 0.01, 64)),
          random_(num(in, "SEED", 0, 0, 1) >= 0.5), dotw_(num(in, "CELL", 0.8, 0.05, 1.5)), rng_(seed) {
        rowsData_.push_back(firstRow());
    }
    void step(const OrganicStep& s, OrganicCanvas& c) override {
        if (beat0_ < -1e300 || s.beat < beat0_) beat0_ = s.beat;
        const double gens = std::max(0.0, s.beat - beat0_) * rate_;
        int todo = std::min(kMaxCatchUp, int(std::floor(gens)) - gen_);
        while (todo-- > 0) {
            rowsData_.push_back(nextRow(rowsData_.back()));
            if (int(rowsData_.size()) > rows_ + 1) rowsData_.erase(rowsData_.begin());
            ++gen_;
        }
        const Grid g(cols_, rows_);
        const double frac = std::clamp(gens - gen_, 0.0, 1.0);  // scroll smoothly between generations
        const int n = int(rowsData_.size());
        for (int j = 0; j < n; ++j) {
            const double row = double(rows_ - n + j) + (n > rows_ ? 1.0 - frac : 0.0);  // newest at the bottom
            if (row < -0.5) continue;
            const double pen = s.pen * (0.3 + 0.7 * double(j + 1) / double(n));
            for (int i = 0; i < cols_; ++i)
                if (rowsData_[std::size_t(j)][std::size_t(i)]) c.dotUnits(g.x(i), g.y(row), g.cell * dotw_, pen, true);
        }
    }
    std::string summary() const override {
        char b[64];
        std::snprintf(b, sizeof b, "rule %d, %d cells, %g generations per beat", rule_, cols_, rate_);
        return b;
    }

private:
    std::vector<char> firstRow() {
        std::vector<char> r(std::size_t(cols_), 0);
        if (random_)
            for (auto& v : r) v = rng_.uniform01() < 0.5 ? 1 : 0;
        else
            r[std::size_t(cols_ / 2)] = 1;
        return r;
    }
    std::vector<char> nextRow(const std::vector<char>& p) {
        std::vector<char> r(p.size(), 0);
        int alive = 0;
        for (int i = 0; i < cols_; ++i) {
            const int l = p[std::size_t((i + cols_ - 1) % cols_)], m = p[std::size_t(i)], rr = p[std::size_t((i + 1) % cols_)];
            r[std::size_t(i)] = char((rule_ >> (l << 2 | m << 1 | rr)) & 1);
            alive += r[std::size_t(i)];
        }
        if (alive == 0 || alive == cols_) return firstRow();  // died out or filled up: start over
        return r;
    }
    int cols_, rows_, rule_;
    double rate_;
    bool random_;
    double dotw_;
    expr::Pcg32 rng_;
    std::vector<std::vector<char>> rowsData_;
    double beat0_ = -1e308;
    int gen_ = 0;
};

// ------------------------------------------------------------------ Game of Life
class Life : public OrganicBody {
public:
    Life(scene::OrganicInstance& in, std::uint64_t seed)
        : cols_(count(in, "COLS", 64, 4, 300)), rows_(count(in, "ROWS", 36, 4, 200)), rate_(num(in, "RATE", 1, 0.01, 64)),
          dens_(num(in, "DENS", 0.3, 0.01, 0.9)), reseedBars_(num(in, "RSD", 16, 0, 1024)),
          dotw_(num(in, "CELL", 0.8, 0.05, 1.5)), rng_(seed) {
        seedGrid();
    }
    void step(const OrganicStep& s, OrganicCanvas& c) override {
        if (beat0_ < -1e300 || s.beat < beat0_) beat0_ = s.beat;
        // Reseed on the reseed line (a phrase, with the phrase clock) and when it has died down or stopped.
        if (reseedBars_ > 0) {
            const double k = std::floor(s.bar / reseedBars_);
            if (lastBlock_ > -1e300 && k != lastBlock_) seedGrid();
            lastBlock_ = k;
        }
        const double gens = std::max(0.0, s.beat - beat0_) * rate_;
        int todo = std::min(kMaxCatchUp, int(std::floor(gens)) - gen_);
        while (todo-- > 0) {
            advance();
            ++gen_;
        }
        const Grid g(cols_, rows_);
        for (int r = 0; r < rows_; ++r)
            for (int q = 0; q < cols_; ++q) {
                const std::size_t i = std::size_t(r * cols_ + q);
                if (!cells_[i]) continue;
                const double pen = s.pen * (age_[i] <= 1 ? 1.0 : 0.65);  // newborns brightest
                c.dotUnits(g.x(q), g.y(r), g.cell * dotw_, pen, true);
            }
    }
    std::string summary() const override {
        char b[64];
        std::snprintf(b, sizeof b, "%d x %d cells, %g generations per beat", cols_, rows_, rate_);
        return b;
    }

private:
    void seedGrid() {
        cells_.assign(std::size_t(cols_ * rows_), 0);
        age_.assign(cells_.size(), 0);
        for (std::size_t i = 0; i < cells_.size(); ++i) cells_[i] = rng_.uniform01() < dens_ ? 1 : 0;
        h1_ = h2_ = 0;
    }
    void advance() {
        std::vector<char> next(cells_.size(), 0);
        int alive = 0;
        std::uint64_t h = 1469598103934665603ULL;
        for (int r = 0; r < rows_; ++r)
            for (int q = 0; q < cols_; ++q) {
                int n = 0;
                for (int dr = -1; dr <= 1; ++dr)
                    for (int dq = -1; dq <= 1; ++dq)
                        if (dr || dq) n += cells_[std::size_t(((r + dr + rows_) % rows_) * cols_ + (q + dq + cols_) % cols_)];
                const std::size_t i = std::size_t(r * cols_ + q);
                const char v = (n == 3 || (n == 2 && cells_[i])) ? 1 : 0;
                next[i] = v;
                age_[i] = v ? (cells_[i] ? std::min(age_[i] + 1, 1000) : 1) : 0;
                alive += v;
                h = (h ^ std::uint64_t(v)) * 1099511628211ULL;
            }
        cells_.swap(next);
        // Died down, or settled into a still life / blinker: start again.
        if (alive < int(cells_.size()) / 50 || h == h1_ || h == h2_) seedGrid();
        else {
            h2_ = h1_;
            h1_ = h;
        }
    }
    int cols_, rows_;
    double rate_, dens_, reseedBars_, dotw_;
    expr::Pcg32 rng_;
    std::vector<char> cells_;
    std::vector<int> age_;
    std::uint64_t h1_ = 0, h2_ = 0;
    double beat0_ = -1e308, lastBlock_ = -1e308;
    int gen_ = 0;
};

// ------------------------------------------------------------------ L-system
class LSystem : public OrganicBody {
public:
    LSystem(scene::OrganicInstance& in)
        : gens_(count(in, "GENS", 5, 0, 12)), rate_(num(in, "RATE", 2, 0.25, 64)), wdth_(num(in, "WDTH", 1.5, 0, 200)),
          inst_(in) {
        std::string axiom = in.literal("AXM");
        if (axiom.empty()) axiom = "X";
        std::map<char, std::string> rules;
        for (const char* k : {"R1", "R2", "R3", "R4"}) {
            const std::string r = in.literal(k);
            const auto eq = r.find('=');
            if (eq == 1 && r.size() >= 2) rules[r[0]] = r.substr(2);
        }
        if (rules.empty()) {  // default: a fern
            rules['X'] = "F+[[X]-X]-F[-FX]+X";
            rules['F'] = "FF";
        }
        strings_.push_back(axiom);
        for (int g = 0; g < gens_; ++g) {
            std::string next;
            for (char ch : strings_.back()) {
                auto it = rules.find(ch);
                next += it == rules.end() ? std::string(1, ch) : it->second;
                if (next.size() > 400000) break;
            }
            if (next.size() > 400000) break;
            strings_.push_back(std::move(next));
        }
    }
    void step(const OrganicStep& s, OrganicCanvas& c) override {
        if (bar0_ < -1e300 || s.bar < bar0_) bar0_ = s.bar;
        const double since = std::max(0.0, s.bar - bar0_);
        const int gen = std::min(int(strings_.size()) - 1, int(std::floor(since / rate_)));
        const double trace = std::clamp(since - gen * rate_, 0.0, 1.0);  // traced over the generation's first bar
        const double ang = finiteOr(inst_.frameKey("ANG", 25.0), 25.0) * kPi / 180.0;
        // Turtle path (up from the origin), then fitted into the frame, root at the bottom.
        segs_.clear();
        struct T {
            double x, y, a;
        };
        std::vector<T> stack;
        T t{0, 0, kPi / 2};
        double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        for (char ch : strings_[std::size_t(gen)]) {
            switch (ch) {
                case 'F':
                case 'G': {
                    const double nx = t.x + std::cos(t.a), ny = t.y + std::sin(t.a);
                    if (segs_.size() < 60000) segs_.push_back({t.x, t.y, nx, ny});
                    t.x = nx;
                    t.y = ny;
                    x0 = std::min(x0, nx); x1 = std::max(x1, nx);
                    y0 = std::min(y0, ny); y1 = std::max(y1, ny);
                    break;
                }
                case 'f':
                    t.x += std::cos(t.a);
                    t.y += std::sin(t.a);
                    break;
                case '+': t.a += ang; break;
                case '-': t.a -= ang; break;
                case '|': t.a += kPi; break;
                case '[': stack.push_back(t); break;
                case ']':
                    if (!stack.empty()) {
                        t = stack.back();
                        stack.pop_back();
                    }
                    break;
                default: break;
            }
        }
        if (segs_.empty()) return;
        const double w = std::max(1e-9, x1 - x0), h = std::max(1e-9, y1 - y0);
        const double k = std::min(3.2 / w, 1.85 / h);
        const double ox = -0.5 * (x0 + x1) * k, oy = -0.95 - y0 * k;
        const double shown = trace * double(segs_.size());
        const std::size_t full = std::size_t(shown);
        for (std::size_t i = 0; i < segs_.size() && i <= full; ++i) {
            const Seg& q = segs_[i];
            const double f = i < full ? 1.0 : shown - double(full);
            if (f <= 0) break;
            c.line(ox + q.x0 * k, oy + q.y0 * k, ox + (q.x0 + f * (q.x1 - q.x0)) * k, oy + (q.y0 + f * (q.y1 - q.y0)) * k,
                   wdth_, s.pen);
        }
    }
    std::string summary() const override {
        char b[80];
        std::snprintf(b, sizeof b, "%zu generation(s), one per %g bar(s), %zu symbols at the end", strings_.size() - 1,
                      rate_, strings_.back().size());
        return b;
    }

private:
    struct Seg {
        double x0, y0, x1, y1;
    };
    int gens_;
    double rate_, wdth_;
    scene::OrganicInstance& inst_;
    std::vector<std::string> strings_;
    std::vector<Seg> segs_;
    double bar0_ = -1e308;
};

// ------------------------------------------------------------------ Clifford attractor
class Attractor : public OrganicBody {
public:
    Attractor(scene::OrganicInstance& in)
        : num_(count(in, "NUM", 6000, 100, 30000)), dotw_(num(in, "DOTW", 1.5, 0, 50)),
          size_(num(in, "SPRD", 0.42, 0.01, 4)), inst_(in) {}
    void step(const OrganicStep& s, OrganicCanvas& c) override {
        double p[4] = {finiteOr(inst_.frameKey("PA", -1.4), -1.4), finiteOr(inst_.frameKey("PB", 1.6), 1.6),
                       finiteOr(inst_.frameKey("PC", 1.0), 1.0), finiteOr(inst_.frameKey("PD", 0.7), 0.7)};
        // Some parameters are not chaotic, or have a short cycle next to the strange attractor: the orbit falls onto a
        // fixed point or a cycle and the 9000 dots land on a handful of pixels, so the organic seems to blink off
        // (Clifford Attractor spent 13 % of its first path there). Then try again from a fresh start (which side it
        // lands on depends on where the orbit starts), and if it still collapses, draw the last full cloud again.
        if (!iterate(p)) {
            x_ = 0.1;
            y_ = 0.1;
            if (!iterate(p) && !last_.empty()) pts_ = last_;
        }
        if (cells_ >= kMinCells) last_ = pts_;
        for (std::size_t i = 0; i < pts_.size(); i += 2) c.dot(pts_[i] * size_, pts_[i + 1] * size_, dotw_, s.pen);
    }
    // Distinct cells (1/50 of a unit) the drawn points cover; fewer than this is a collapsed orbit.
    static constexpr int kMinCells = 800;
    std::string summary() const override {
        char b[48];
        std::snprintf(b, sizeof b, "%d points per frame", num_);
        return b;
    }

private:
    // Runs the map for this frame's points into pts_; false when they cover fewer than kMinCells cells.
    bool iterate(const double* p) {
        const double a = p[0], b = p[1], cc = p[2], d = p[3];
        pts_.clear();
        std::fill(seen_.begin(), seen_.end(), 0);
        cells_ = 0;
        for (int i = 0; i < num_ + 10; ++i) {
            const double nx = std::sin(a * y_) + cc * std::cos(a * x_);
            const double ny = std::sin(b * x_) + d * std::cos(b * y_);
            x_ = std::isfinite(nx) ? nx : 0.1;
            y_ = std::isfinite(ny) ? ny : 0.1;
            if (i < 10) continue;
            pts_.push_back(x_);
            pts_.push_back(y_);
            // |x|, |y| <= 1 + |c| or 1 + |d|; cells over +-6 cover any sane parameters (outside: not counted).
            const int gx = int(std::floor((x_ + 6.0) * 50.0)), gy = int(std::floor((y_ + 6.0) * 50.0));
            if (gx < 0 || gy < 0 || gx >= kGrid || gy >= kGrid) continue;
            const std::size_t k = std::size_t(gy) * kGrid + std::size_t(gx);
            if (!seen_[k]) {
                seen_[k] = 1;
                ++cells_;
            }
        }
        return cells_ >= kMinCells;
    }
    static constexpr int kGrid = 600;
    int num_;
    double dotw_, size_;
    scene::OrganicInstance& inst_;
    double x_ = 0.1, y_ = 0.1;
    std::vector<double> pts_;
    std::vector<unsigned char> seen_ = std::vector<unsigned char>(std::size_t(kGrid) * kGrid, 0);
    int cells_ = 0;
    std::vector<double> last_;  // the last frame's points that covered a real cloud
};

}  // namespace

// ------------------------------------------------------------------ canvas

OrganicCanvas::OrganicCanvas(const FrameMapping& map, const LineStyle& style, std::vector<LineSegment>& out, double x,
                             double y, double scale, double rot)
    : map_(map), style_(style), out_(out) {
    x_ = finiteOr(x, 0);
    y_ = finiteOr(y, 0);
    scale = std::clamp(finiteOr(scale, 1), -100.0, 100.0);
    rot = finiteOr(rot, 0);
    rc_ = std::cos(rot) * scale;
    rs_ = std::sin(rot) * scale;
    absScale_ = std::abs(scale);
    limit_ = 64.0 * std::max(map.width, map.height);
    path_ = float(out.size());
}

bool OrganicCanvas::toPx(double x, double y, float& ox, float& oy) const {
    const double qx = map_.toPixelX(x_ + rc_ * x - rs_ * y), qy = map_.toPixelY(y_ + rs_ * x + rc_ * y);
    ox = float(qx);
    oy = float(qy);
    return std::isfinite(qx) && std::isfinite(qy) && std::abs(qx) < limit_ && std::abs(qy) < limit_;
}

float OrganicCanvas::radius(double width) const {
    return float(0.5 * lineWidthPixels(width * absScale_, style_, map_.width, map_.height));
}

void OrganicCanvas::line(double x0, double y0, double x1, double y1, double width, double pen0, double pen1) {
    pen0 = std::clamp(finiteOr(pen0, 0), 0.0, 1.0);
    pen1 = std::clamp(finiteOr(pen1, 0), 0.0, 1.0);
    if (pen0 <= 0 && pen1 <= 0) return;
    float ax, ay, bx, by;
    if (!toPx(x0, y0, ax, ay) || !toPx(x1, y1, bx, by)) return;
    out_.push_back({ax, ay, bx, by, float(pen0), float(pen1), radius(width), path_});
}

void OrganicCanvas::dot(double x, double y, double width, double pen) {
    pen = std::clamp(finiteOr(pen, 0), 0.0, 1.0);
    if (pen <= 0 || !(width > 0)) return;
    float ax, ay;
    if (!toPx(x, y, ax, ay)) return;
    out_.push_back({ax, ay, ax, ay, float(pen), float(pen), radius(width), path_});
}

void OrganicCanvas::dotUnits(double x, double y, double diameter, double pen, bool hard) {
    pen = std::clamp(finiteOr(pen, 0), 0.0, 1.0);
    if (pen <= 0 || !(diameter > 0)) return;
    float ax, ay;
    if (!toPx(x, y, ax, ay)) return;
    const float r = float(std::max(0.5, 0.25 * diameter * map_.height * absScale_));  // tree unit = H / 2 px
    out_.push_back({ax, ay, ax, ay, float(pen), float(pen), hard ? -r : r, path_});
}

double OrganicCanvas::pixelsPerUnit() const { return 0.5 * map_.height * absScale_; }

void OrganicCanvas::fillPolygon(const double* xs, const double* ys, int n, double pen) {
    pen = std::clamp(finiteOr(pen, 0), 0.0, 1.0);
    if (pen <= 0 || n < 3 || n > 4096) return;
    // Into frame pixels (Rot and Scal applied), then scanlines there.
    float px[4096], py[4096];
    float y0 = 1e30f, y1 = -1e30f;
    for (int i = 0; i < n; ++i) {
        if (!toPx(xs[i], ys[i], px[i], py[i])) return;
        y0 = std::min(y0, py[i]);
        y1 = std::max(y1, py[i]);
    }
    const float pf = float(pen);
    // Hard-edged strokes (negative radius; a soft rim's in-between intensities take unrelated ColorMap colors),
    // laid on the pixel grid so the edges are clean: one stroke per pair of pixel rows (radius 1 on the line between
    // them covers both rows' centers and nothing else), each ending on the first and last pixel center inside the
    // span, whose round cap then reaches no further pixel (square corners). Pixel centers are at k + .5.
    const auto spans = [&](float y, float rad) {
        float hits[64];
        int k = 0;
        for (int i = 0, j = n - 1; i < n && k < 64; j = i++) {
            const float ya = py[j], yb = py[i];
            if ((ya <= y && yb > y) || (yb <= y && ya > y)) hits[k++] = px[j] + (y - ya) / (yb - ya) * (px[i] - px[j]);
        }
        std::sort(hits, hits + k);
        for (int i = 0; i + 1 < k; i += 2) {
            const float first = std::ceil(hits[i] - 0.5f) + 0.5f, last = std::floor(hits[i + 1] - 0.5f) + 0.5f;
            if (first <= last) {
                out_.push_back({first, y, last, y, pf, pf, -rad, path_});
            } else if (hits[i + 1] - hits[i] > 0.25f) {  // narrower than a pixel: the nearest pixel, not nothing
                const float m = std::floor(0.5f * (hits[i] + hits[i + 1])) + 0.5f;
                out_.push_back({m, y, m, y, pf, pf, -rad, path_});
            }
        }
    };
    const int rFirst = int(std::ceil(y0 - 0.5f)), rLast = int(std::floor(y1 - 0.5f));
    if (rLast < rFirst) {  // thinner than a pixel row: the row nearest its middle
        if (y1 - y0 > 0.25f) spans(std::floor(0.5f * (y0 + y1)) + 0.5f, 0.5f);
        return;
    }
    if (rLast - rFirst > 8000) return;  // absurdly large
    for (int r = rFirst; r <= rLast; r += 2) {
        if (r + 1 <= rLast) spans(float(r) + 1.0f, 1.0f);  // rows r and r + 1
        else spans(float(r) + 0.5f, 0.5f);                // the last row alone
    }
}

void OrganicCanvas::rect(double cx, double cy, double w, double h, double pen) {
    const double hw = 0.5 * std::abs(w), hh = 0.5 * std::abs(h);
    const double xs[4] = {cx - hw, cx + hw, cx + hw, cx - hw};
    const double ys[4] = {cy - hh, cy - hh, cy + hh, cy + hh};
    fillPolygon(xs, ys, 4, pen);
}

void OrganicCanvas::bitmap(const char* const* rows, int nRows, double cx, double cy, double px, double pen,
                           bool mirror) {
    if (nRows <= 0 || !(px > 0)) return;
    int cols = 0;
    for (int r = 0; r < nRows; ++r) cols = std::max(cols, int(std::strlen(rows[r])));
    const double left = cx - 0.5 * cols * px, top = cy + 0.5 * nRows * px;
    auto lit = [](char ch) { return ch != '.' && ch != ' ' && ch != 0; };
    for (int r = 0; r < nRows; ++r) {
        const char* row = rows[r];
        const int len = int(std::strlen(row));
        int q = 0;
        while (q < len) {
            if (!lit(row[q])) {
                ++q;
                continue;
            }
            int e = q;
            while (e < len && lit(row[e])) ++e;
            // Columns q..e-1, flipped when mirrored.
            const double a = mirror ? cols - e : q, b = mirror ? cols - q : e;
            rect(left + 0.5 * (a + b) * px, top - (r + 0.5) * px, (b - a) * px, px, pen);
            q = e;
        }
    }
}

// ------------------------------------------------------------------ factory

const std::vector<std::string>& organicBodyKinds() {
    static const std::vector<std::string> k = {"phyllotaxis", "harmonograph", "automaton", "life", "lsystem",
                                               "attractor", "dragon", "invaders", "hopper", "cycles", "rocks", "bricks",
                                               "chase"};
    return k;
}

std::unique_ptr<OrganicBody> makeOrganicBody(const std::string& kind, scene::OrganicInstance& inst,
                                             std::uint64_t seed) {
    if (kind == "phyllotaxis") return std::make_unique<Phyllotaxis>(inst);
    if (kind == "harmonograph") return std::make_unique<Harmonograph>(inst);
    if (kind == "automaton") return std::make_unique<Automaton>(inst, seed);
    if (kind == "life") return std::make_unique<Life>(inst, seed);
    if (kind == "lsystem") return std::make_unique<LSystem>(inst);
    if (kind == "attractor") return std::make_unique<Attractor>(inst);
    if (kind == "dragon") return dragon::makeDragon(inst, seed);
    if (kind == "invaders") return arcade::makeInvaders(inst, seed);
    if (kind == "hopper") return arcade::makeHopper(inst, seed);
    if (kind == "cycles") return arcade::makeCycles(inst, seed);
    if (kind == "rocks") return arcade::makeRocks(inst, seed);
    if (kind == "bricks") return arcade::makeBricks(inst, seed);
    if (kind == "chase") return arcade::makeChase(inst, seed);
    return nullptr;
}

}  // namespace reforce::engine
