// Arcade organic `bricks` (PLAN 5Q.6, docs/arcade-design.md): a ball bouncing on the beat lines through a wall of
// bricks, the last ones breaking exactly on the cycle line (Cyc bars, a phrase), then a new wall drops in.
//
// Our own design, inspired by the paddle-and-ball brick games' mechanics. The cycle is planned volley by volley (Game,
// Bricks.hpp): at a constant speed, legs between paddle and brick hits of a whole number of ticks, so every hit is on
// a tick line (the angle, and side-wall bounces, make the lengths fit); per hit, how many bricks it breaks (the target
// and its nearest neighbours) so that the remainder lands on the final hits, which all end on the cycle line. The
// paddle glides to where the ball will land. Drawing is a pure function of the plan and the beat, so a step that
// jumps many beats costs at most a new plan.
#include "engine/arcade/Bricks.hpp"

#include <algorithm>
#include <optional>
#include <cmath>
#include <cstdio>
#include <limits>

#include "engine/arcade/Arcade.hpp"

namespace reforce::engine::arcade {

namespace bricks {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kEps = 1e-7;

std::uint64_t mix64(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
double unit(std::uint64_t h) { return double(h >> 11) * (1.0 / 9007199254740992.0); }

double smooth(double a) {
    a = std::clamp(a, 0.0, 1.0);
    return a * a * (3 - 2 * a);
}

}  // namespace

Params paramsFrom(const scene::OrganicInstance& in) {
    Params p;
    p.cols = keyCount(in, "COLS", p.cols, 1, 40);
    p.rows = keyCount(in, "ROWS", p.rows, 1, 16);
    p.cycBars = keyCount(in, "CYC", p.cycBars, 1, 64);
    // Ticks per beat from a set that divides every cycle into whole ticks (also x Bld).
    const double rate = keyNum(in, "RATE", p.rate, 0.25, 4);
    static constexpr double kRates[] = {0.25, 0.5, 1, 2, 3, 4};
    p.rate = 1;
    for (double r : kRates)
        if (std::abs(std::log(r / rate)) < std::abs(std::log(p.rate / rate))) p.rate = r;
    p.buildMul = keyCount(in, "BLD", p.buildMul, 1, 4);
    p.speed = keyNum(in, "SPD", p.speed, 0.2, 8);
    p.ball = keyNum(in, "BALL", p.ball, 0.01, 0.3);
    p.paddle = keyNum(in, "PAD", p.paddle, 0.1, 2);
    p.gap = keyNum(in, "GAP", p.gap, 0, 0.1);
    p.brickH = keyNum(in, "BRKH", p.brickH, 0.02, 0.3);
    p.halfW = keyNum(in, "FLDW", p.halfW, 0.3, 10);
    p.halfH = keyNum(in, "FLDH", p.halfH, 0.3, 5);
    p.multi = keyCount(in, "MULT", p.multi, 0, 6);
    p.frags = keyCount(in, "FRAG", p.frags, 0, 12);
    p.fragLife = keyNum(in, "FLIF", p.fragLife, 0.25, 8);
    p.layout = keyCount(in, "LAY", p.layout, -1, 7);
    p.wallPen = keyNum(in, "WALL", p.wallPen, 0, 1);
    return p;
}

Game::Game(const Params& p, std::uint64_t seed) : p_(p), seed_(seed) {
    const double W = p_.halfW, H = p_.halfH, r = 0.5 * p_.ball;
    p_.paddle = std::min(p_.paddle, 2 * W - 0.02);
    p_.ball = std::min(p_.ball, 0.25 * H);
    p_.gap = std::min(p_.gap, W / (p_.cols + 1));
    padH_ = 0.035;
    padY_ = -H + 0.1 + 0.5 * padH_;
    padContact_ = padY_ + 0.5 * padH_ + r;
    top_ = H - std::max(0.12, 3 * r);
    bw_ = std::max(0.005, (2 * W - (p_.cols + 1) * p_.gap) / p_.cols);
    // Rows fit above a clear band over the paddle; bricks get thinner rather than reach down into it.
    const double avail = top_ - (padContact_ + std::max(0.5, 0.45 * (top_ - padContact_)));
    bh_ = std::max(0.008, std::min(p_.brickH, avail / p_.rows - p_.gap));
    typical_ = (top_ - 0.5 * p_.rows * (bh_ + p_.gap)) - padContact_;
}

double Game::speedOf(const Seg& s) {
    return std::hypot(s.x1 - s.x0, s.y1 - s.y0) / std::max(1e-12, s.t1 - s.t0);
}

double Game::rnd() {
    rng_ = mix64(rng_);
    return unit(rng_);
}

double Game::snapUp(double t) const {
    const double g = tick();
    return ts_ + std::ceil((t - ts_) / g - 1e-6) * g;
}

// ------------------------------------------------------------------ the wall

void Game::layoutWall(long k) {
    const int C = p_.cols, R = p_.rows;
    const std::uint64_t h = mix64(seed_ ^ mix64(std::uint64_t(k) * 0x51ed27ULL + 17));
    int pattern = p_.layout >= 0 ? p_.layout : int(h % 8);
    const int scheme = int((h >> 8) % 4);
    const double phase = unit(mix64(h ^ 3)) * 6.283185307179586;
    auto has = [&](int c, int r) {
        const double mc = 0.5 * (C - 1), mr = 0.5 * (R - 1);
        const double dc = C > 1 ? std::abs(c - mc) / (0.5 * C) : 0, dr = R > 1 ? std::abs(r - mr) / (0.5 * R) : 0;
        switch (pattern) {
            case 1: return dc <= 1.0 - 0.75 * r / std::max(1, R) + 1e-9;  // narrowing downwards
            case 2: return ((c / 2) + r) % 2 == 0;                         // a chequer of double bricks
            case 3: return dc + dr <= 1.15;                                // a diamond
            case 4: return c % 3 != 1 || r == 0;                           // pillars hanging from a beam
            case 5: {                                                      // a wave of column heights
                const double hgt = R * (0.6 + 0.4 * std::sin(6.283185307179586 * 1.5 * c / C + phase));
                return r < std::max(1.0, hgt);
            }
            case 6: return dc > 0.22 || r == 0;  // two blocks under a beam
            case 7: return unit(mix64(h ^ std::uint64_t(r * 64 + c + 1))) > 0.2;  // worn
            default: return true;
        }
    };
    bricks_.clear();
    col_.clear();
    row_.clear();
    for (int pass = 0; pass < 2; ++pass) {
        for (int r = 0; r < R; ++r)
            for (int c = 0; c < C; ++c) {
                if (!has(c, r)) continue;
                // Rows at different intensities, so the ColorMap gives them different colours.
                const double f = R > 1 ? double(r) / (R - 1) : 0;
                double pen = 0.7;
                switch (scheme) {
                    case 0: pen = 0.95 - 0.45 * f; break;
                    case 1: pen = 0.5 + 0.45 * f; break;
                    case 2: pen = r % 2 ? 0.55 : 0.92; break;
                    default: pen = 0.6 + 0.35 * std::abs(std::sin(1.7 * r + phase)); break;
                }
                bricks_.push_back({-p_.halfW + p_.gap + 0.5 * bw_ + c * (bw_ + p_.gap),
                                   top_ - 0.5 * bh_ - r * (bh_ + p_.gap), bw_, bh_, float(pen), kInf});
                col_.push_back(c);
                row_.push_back(r);
            }
        if (int(bricks_.size()) >= std::max(1, C * R / 4)) break;
        bricks_.clear();  // too sparse at this size: the full wall
        col_.clear();
        row_.clear();
        pattern = 0;
    }
    claimBall_.assign(bricks_.size(), -1);
    // The brick the wall is eaten away towards: hits steer clear of it, so the last few left on the line are
    // neighbours (one burst there, not scattered ones), high in the wall.
    finale_ = 0;
    double best = -1;
    for (std::size_t b = 0; b < bricks_.size(); ++b) {
        const double v = unit(mix64(h ^ (b + 99))) + 0.6 * (1.0 - double(row_[b]) / std::max(1, R - 1));
        if (v > best) {
            best = v;
            finale_ = int(b);
        }
    }
}

double Game::fromFinale(int b) const {
    if (bricks_.empty()) return 0;
    const Brick &a = bricks_[std::size_t(b)], &f = bricks_[std::size_t(finale_)];
    return std::hypot((a.x - f.x) / (2 * p_.halfW), 2 * (a.y - f.y) / (2 * p_.halfW));
}

void Game::lowest(double t, std::vector<int>& low) const {
    low.assign(std::size_t(p_.cols), -1);
    for (std::size_t i = 0; i < bricks_.size(); ++i)  // top row first: the last one per column is the lowest
        if (bricks_[i].broken > t + kEps) low[std::size_t(col_[i])] = int(i);
}

// Whether the ball (centre path a -> b) passes below every brick present: within a column's span, the path stays under
// that column's lowest brick. (Bricks only ever hang above their column's lowest one.)
bool Game::clear(double ax, double ay, double bx, double by, const std::vector<int>& low) const {
    const double r = 0.5 * p_.ball;
    const double lo = std::min(ax, bx), hi = std::max(ax, bx);
    for (int c = 0; c < p_.cols; ++c) {
        const int b = low[std::size_t(c)];
        if (b < 0) continue;
        const double xc = -p_.halfW + p_.gap + 0.5 * bw_ + c * (bw_ + p_.gap);
        const double s0 = std::max(lo, xc - 0.5 * bw_ - r), s1 = std::min(hi, xc + 0.5 * bw_ + r);
        if (s0 > s1) continue;
        double ymax;
        if (hi - lo < 1e-9) ymax = std::max(ay, by);
        else {
            const double y0 = ay + (by - ay) * (s0 - ax) / (bx - ax), y1 = ay + (by - ay) * (s1 - ax) / (bx - ax);
            ymax = std::max(y0, y1);
        }
        if (ymax + 0.6 * r >= bricks_[std::size_t(b)].y - 0.5 * bricks_[std::size_t(b)].h) return false;
    }
    return true;
}

// ------------------------------------------------------------------ planning
//
// The ball flies at a constant speed (Spd units per beat, within kSpeedTol, changed only at the paddle). A leg
// between two on-beat hits (paddle and brick) is a whole number of ticks, so its path must be speed x ticks long:
// the planner picks the angle, and when a straight shot would be too short, a path off one or more side walls (those
// bounces land wherever they fall). The walls mirror the field, so such a path is a straight line in the unfolded
// plane: x beyond a wall continues in its mirror image, and fold() maps it back.

namespace {

constexpr double kSpeedTol = 0.03;  // the speed may differ this much from Spd (set at the paddle), to hit a brick
constexpr double kFlat = 3.5;       // at most this much sideways (unfolded) per unit up or down
constexpr double kHorizon = 6;      // beats planned ahead of the clock

}  // namespace

double Game::fold(double x) const {
    const double a = p_.halfW - 0.5 * p_.ball, P = 4 * a;
    double m = std::fmod(x + a, P);
    if (m < 0) m += P;
    return m <= 2 * a ? m - a : 3 * a - m;
}

int Game::trace(double ax, double ay, double X, double by, double* xs, double* ys, int maxPts) const {
    const double a = p_.halfW - 0.5 * p_.ball;
    int n = 0;
    xs[n] = fold(ax);
    ys[n++] = ay;
    // The wall lines x = (2k + 1) a crossed between ax and X, in order.
    if (X > ax) {
        for (double k = std::floor((ax - a) / (2 * a)) + 1; (2 * k + 1) * a < X - 1e-12; k += 1) {
            if (n >= maxPts - 1) return -1;
            const double xw = (2 * k + 1) * a;
            xs[n] = fold(xw);
            ys[n++] = ay + (by - ay) * (xw - ax) / (X - ax);
        }
    } else if (X < ax) {
        for (double k = std::ceil((ax - a) / (2 * a)) - 1; (2 * k + 1) * a > X + 1e-12; k -= 1) {
            if (n >= maxPts - 1) return -1;
            const double xw = (2 * k + 1) * a;
            xs[n] = fold(xw);
            ys[n++] = ay + (by - ay) * (xw - ax) / (X - ax);
        }
    }
    xs[n] = fold(X);
    ys[n++] = by;
    return n;
}

bool Game::pathClear(const double* xs, const double* ys, int n, const std::vector<int>& low) const {
    for (int k = 0; k + 1 < n; ++k)
        if (!clear(xs[k], ys[k], xs[k + 1], ys[k + 1], low)) return false;
    return true;
}

void Game::pushPath(int ball, double t0, double t1, const double* xs, const double* ys, int n, Hit end, int bricks,
                    int brick) {
    legBeats_ += t1 - t0;
    ++legs_;
    double total = 0;
    for (int k = 0; k + 1 < n; ++k) total += std::hypot(xs[k + 1] - xs[k], ys[k + 1] - ys[k]);
    double run = 0, ta = t0;
    for (int k = 0; k + 1 < n; ++k) {
        run += std::hypot(xs[k + 1] - xs[k], ys[k + 1] - ys[k]);
        const bool last = k + 2 == n;
        const double tb = last ? t1 : t0 + (t1 - t0) * run / std::max(1e-12, total);
        const Hit mid = std::abs(xs[k + 1]) >= p_.halfW - 0.5 * p_.ball - 1e-9 ? Hit::Wall : Hit::Ceiling;
        segs_.push_back({ta, tb, xs[k], ys[k], xs[k + 1], ys[k + 1], ball, last ? end : mid, last,
                         last ? bricks : 0, last ? brick : -1});
        ta = tb;
    }
}

// The best way up from the paddle point px (at beat tp) to a brick in nMin..nMax ticks at about the base speed: per
// column's lowest brick (present when the ball gets there), per duration and side, the unfolded sideways distance
// that makes the path speed x ticks long, nudged within `tol` of the speed to land under the brick.
void Game::bestUp(double px, double tp, int nMin, int nMax, const Relax& rx, const std::vector<int>& low,
                  bool wantFree, bool final, std::vector<UpPick>& out, int want) {
    out.clear();
    if (nMax < nMin) return;
    const double g = tick(), vb = p_.speed, r = 0.5 * p_.ball, a = p_.halfW - r;
    struct Cand {
        double score, X, ty, v;
        int target, n;
    };
    std::vector<Cand> cands;
    for (int c = 0; c < p_.cols; ++c) {
        const int k = low[std::size_t(c)];
        if (k < 0) continue;
        const Brick& br = bricks_[std::size_t(k)];
        const double lo = std::max(-a, br.x - 0.45 * br.w), hi = std::min(a, br.x + 0.45 * br.w);
        if (lo > hi) continue;
        const double ty = br.y - 0.5 * br.h - r, dy = ty - padContact_;
        if (dy <= 0) continue;
        const bool isFree = claimBall_[std::size_t(k)] < 0 || (final && br.broken >= te_ - kEps);
        // Only durations whose path is neither too short nor too flat at about the base speed.
        const double tolV = std::min(rx.tol, 0.9), steep = std::hypot(1.0, std::min(rx.flat, 50.0));
        const int n0 = std::max(nMin, int(std::ceil(dy * (1 - tolV) / (vb * g) - 1e-9)));
        const int n1 = std::min(nMax, int(std::floor(dy * steep * (1 + std::min(rx.tol, 10.0)) / (vb * g) + 1e-9)));
        const int stride = std::max(1, (n1 - n0 + 1) / 24);
        for (int n = std::max(1, n0); n <= n1; n += stride) {
            if (br.broken <= tp + n * g + kEps && !(final && br.broken >= te_ - kEps)) continue;  // gone by then
            const double L = vb * n * g;
            for (int s = -1; s <= 1; s += 2) {
                const double D0 = std::sqrt(std::max(0.0, L * L - dy * dy));
                double X = px + s * D0;
                const double x0 = fold(X);
                if (x0 < lo || x0 > hi) {
                    // Slide along the unfolded line to the brick (fold has slope +-1 there).
                    const double m = std::fmod(X + a, 4 * a), slope = (m < 0 ? m + 4 * a : m) <= 2 * a ? 1 : -1;
                    X += (std::clamp(x0, lo, hi) - x0) * slope;
                    if (std::abs(fold(X) - std::clamp(x0, lo, hi)) > 1e-9) continue;
                }
                const double D = std::abs(X - px), v = std::hypot(D, dy) / (n * g);
                const double dev = std::abs(v / vb - 1);
                if (dev > rx.tol || D > rx.flat * dy) continue;
                double score = rnd() - 25 * dev - (fast_ ? 0.4 * n * g : 0);
                // A breaking hit aims at a brick it breaks (knocking on one kept for later only when bending rules).
                if (wantFree && !isFree && rx.clear) continue;
                if (wantFree) score += isFree ? 2 : 0;
                score += (final ? -1.5 : 1.0) * fromFinale(k);
                cands.push_back({score, X, ty, v, k, n});
            }
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& p, const Cand& q) { return p.score > q.score; });
    double xs[kMaxPts], ys[kMaxPts];
    for (const Cand& cd : cands) {
        const int np = trace(px, padContact_, cd.X, cd.ty, xs, ys, kMaxPts);
        if (np < 2 || (rx.clear && !pathClear(xs, ys, np, low))) continue;
        out.push_back({true, cd.target, cd.X, cd.ty, cd.v, cd.n, cd.score});
        if (int(out.size()) >= want) break;
    }
}

// How many bricks the next hit of ball i breaks, one almost always (the classic feel; Peter 2026-11-22: many at once
// looks unnatural), the count made to work through time instead. One brick is kept for each ball's final hit on the
// line; the others are paced over the time before it: the volleys stretch (pace_, preferred by volleys()) when there
// are few bricks, and some hits only knock (break nothing) when even long volleys would leave bricks over; when the
// wall has more bricks than the time has hits at the quickest pace, a dithered share of the hits break two, spread
// evenly over the cycle (the share is recomputed at every hit, and the pace aims to be done a volley early, so it is
// never bunched at the end).
int Game::share(int i, bool final) {
    int rem = 0;
    for (int c : claimBall_) rem += c < 0 ? 1 : 0;
    int others = 0;
    for (std::size_t j = 0; j < st_.size(); ++j)
        if (!st_[j].done && int(j) != i) ++others;
    const int active = others + 1;
    pace_ = 0;
    // Ball 0's final hit takes what is left (one brick, paced so); extra balls end on the shield on the line.
    if (final) return i == 0 ? rem : 0;
    const int spare = rem - (st_[0].done ? 0 : 1);
    if (spare <= 0) return 0;
    const double fast = fastVolley(), slow = 2.4 * fast;
    // This ball's share of the spare bricks, and its time for them (before its final volley and one more).
    const double mine = spare / double(active);
    const double left = std::max(0.0, te_ - st_[std::size_t(i)].t - 3 * fast);
    const double possible = left / fast;
    if (mine > possible) {
        pace_ = fast;
        const int n = int(std::floor(mine / std::max(1.0, possible) + rnd()));
        return std::clamp(n, 1, spare);
    }
    pace_ = std::clamp(left / mine, fast, slow);
    if (left / mine > slow && rnd() > slow * mine / std::max(1e-9, left)) return 0;  // knock: too few bricks
    return 1;
}

// A quick volley in beats: down and up a typical height a little off vertical, in whole ticks (a few legs fit in one
// tick less, hence the .2).
double Game::fastVolley() const {
    const double g = tick();
    return 2 * g * std::max(1.0, std::ceil(std::hypot(typical_, 0.6) / (p_.speed * g) - 0.2));
}

// Commits ball i's hit of `target` at t1 breaking n bricks: the target (when free) and its nearest free neighbours,
// leaning upwards (a tunnel into the wall). n = 0: the ball just knocks on it. Returns the bricks broken.
int Game::claim(int i, int target, double t1, int n, bool final) {
    if (n <= 0 || target < 0) return 0;
    const bool own = claimBall_[std::size_t(target)] < 0;
    if (own) {
        claimBall_[std::size_t(target)] = i;
        bricks_[std::size_t(target)].broken = t1;
    }
    std::size_t take = 0;
    if (n - (own ? 1 : 0) > 0) {
        std::vector<std::pair<double, int>> near;
        for (std::size_t k = 0; k < bricks_.size(); ++k) {
            if (claimBall_[k] >= 0) continue;
            const int dc = std::abs(col_[k] - col_[std::size_t(target)]);
            const int dr = row_[std::size_t(target)] - row_[k];  // > 0: above the target
            near.push_back({2.5 * dc + (dr >= 0 ? 0.7 * dr : 1.5 * -dr) + 0.3 * rnd() +
                                (final ? 0.0 : 3.0 * std::max(0.0, 0.5 - fromFinale(int(k)))),
                            int(k)});
        }
        take = std::min(near.size(), std::size_t(n - (own ? 1 : 0)));
        std::partial_sort(near.begin(), near.begin() + std::ptrdiff_t(take), near.end());
        for (std::size_t k = 0; k < take; ++k) {
            claimBall_[std::size_t(near[k].second)] = i;
            bricks_[std::size_t(near[k].second)].broken = t1;
        }
    }
    return int(take) + (own ? 1 : 0);
}

int Game::legMin() const {
    // Ticks the steepest leg takes at most (paddle to the highest brick): every leg can be at least this short.
    return std::max(1, int(std::ceil((top_ - padContact_) / (p_.speed * tick()) - 1e-9)));
}

// The paddle (ball 0) meets the ball off-centre, on the side that sends it along the path's first piece.
void Game::paddleKey(double t, double px, double nx, double ny) {
    const double r = 0.5 * p_.ball, W = p_.halfW;
    const double lean = std::clamp((nx - px) / std::max(0.2, ny - padContact_) * 0.5, -1.0, 1.0);
    const double half = 0.5 * p_.paddle, reach = std::max(0.0, half - 0.5 * r);
    double centre = std::clamp(px - lean * 0.6 * half, -W + half, W - half);
    centre = std::clamp(centre, px - reach, px + reach);
    padKeys_.push_back({t, centre});
}

namespace {
// Looser and looser until something fits (the last: any speed, any path).
constexpr Game::Relax kRelax[] = {{kSpeedTol, kFlat, true}, {kSpeedTol, 6, true}, {0.05, 6, true},
                                  {0.05, 8, false}, {0.1, 8, false}, {1e9, 1e9, false}};
constexpr int kLevels = int(sizeof kRelax / sizeof kRelax[0]);
}  // namespace

// Ball i at the paddle (after a re-plan, or when there was nothing to aim at from up there): up to a brick, or
// through a gap to the ceiling and back.
void Game::decideUp(int i) {
    const BallState b = st_[std::size_t(i)];
    const double g = tick();
    const int R = std::max(1, int(std::floor((te_ - b.t) / g + 1e-6)));
    const int F = 2 * legMin();
    bool final = R < F + legMin();
    if (final && i > 0) {
        st_[std::size_t(i)].done = true;  // an extra ball near the line: the shield keeps it
        return;
    }
    std::vector<int> low;
    lowest(final ? te_ - 1e-4 : b.t, low);
    int n = share(i, final);
    UpPick up;
    std::vector<UpPick> ups;
    int level = 0;
    // Near the line, the up leg after which the volleys to the line still fit (tried on a copy, then undone).
    auto ahead = [&]() {
        if (final || R >= 5 * F) return;
        for (std::size_t k = 0; k < ups.size(); ++k) {
            const Snapshot sn = snapshot();
            commitUp(i, b.x, b.t, ups[k], n, false, R);
            const bool fits = fitsAhead(i, 1);
            restore(sn);
            if (fits) {
                std::swap(ups[0], ups[k]);
                return;
            }
        }
        if (sky(i, kRelax[0], R - F)) ups.clear();  // nothing fits after a hit: fill the time instead
    };
    for (const Relax& rx : kRelax) {
        if (final) bestUp(b.x, b.t, R, R, rx, low, n > 0, true, ups, 1);
        else bestUp(b.x, b.t, 1, R - F, rx, low, n > 0, false, ups, 12);
        if (ups.empty() && n > 0 && !final && rx.clear) {
            bestUp(b.x, b.t, 1, R - F, rx, low, false, false, ups, 12);
            if (!ups.empty()) n = 0;
        }
        if (!ups.empty() && rx.clear) {
            ahead();
            if (ups.empty()) {
                ++relaxed_[level];
                return;  // went to the sky
            }
        }
        if (ups.empty() && !final && rx.clear && sky(i, rx, R - F)) {
            ++relaxed_[level];
            return;
        }
        if (ups.empty() && i > 0 && !rx.clear) {
            st_[std::size_t(i)].done = true;  // an extra ball with nothing left within the rules: the shield keeps it
            ++relaxed_[level];
            return;
        }
        if (ups.empty() && !final && !rx.clear) {
            // Nothing before the line: the final hit now.
            std::vector<int> lowEnd;
            lowest(te_ - 1e-4, lowEnd);
            const int nf = share(i, true);
            bestUp(b.x, b.t, R, R, rx, lowEnd, nf > 0, true, ups, 1);
            if (!ups.empty()) {
                n = nf;
                final = true;
                low.swap(lowEnd);
            }
        }
        if (!ups.empty()) {
            up = ups[0];
            break;
        }
        ++level;
    }
    ++relaxed_[std::min(level, kLevels)];
    commitUp(i, b.x, b.t, up, n, final, R);
}

void Game::commitUp(int i, double px, double tp, const UpPick& up, int n, bool final, int R) {
    const double g = tick(), r = 0.5 * p_.ball;
    double xs[kMaxPts], ys[kMaxPts];
    if (!up.ok) {
        // Nothing to aim at (no brick left standing): straight up to the ceiling, on the line.
        xs[0] = px;
        ys[0] = padContact_;
        xs[1] = px;
        ys[1] = p_.halfH - r;
        if (i == 0) paddleKey(tp, px, px, ys[1]);
        pushPath(i, tp, final ? te_ : tp + R * g, xs, ys, 2, Hit::Ceiling, 0, -1);
        // (Last resort, never expected: whatever is left breaks on the line anyway.)
        if (i == 0)
            for (std::size_t k = 0; k < bricks_.size(); ++k)
                if (claimBall_[k] < 0) {
                    claimBall_[k] = 0;
                    bricks_[k].broken = te_;
                    ++orphans_;
                }
        st_[std::size_t(i)] = {segs_.back().t1, px, ys[1], false, true, p_.speed};
        return;
    }
    const double t1 = final ? te_ : tp + up.n * g;
    const int np = trace(px, padContact_, up.X, up.ty, xs, ys, kMaxPts);
    if (i == 0) paddleKey(tp, px, xs[1], ys[1]);
    const int broke = claim(i, up.target, t1, n, final);
    pushPath(i, tp, t1, xs, ys, np, Hit::Brick, broke, up.target);
    st_[std::size_t(i)] = {t1, xs[np - 1], ys[np - 1], false, final, up.v};
}

// Ball i at a brick (or dropping in at the start): its next volley, down to the paddle at its current speed and up
// to the next brick, the two chosen together so both hits land on tick lines. Candidates best first.
void Game::volleys(int i, const Relax& rx, int n, std::vector<Volley>& out, int perDown, bool forceFinal) {
    out.clear();
    const BallState b = st_[std::size_t(i)];
    const double g = tick();
    const int R = std::max(1, int(std::floor((te_ - b.t) / g + 1e-6)));
    const int F = 2 * legMin();
    const bool final = forceFinal || R < 2 * F;
    std::vector<int> low, none(std::size_t(p_.cols), -1), lowEnd;
    lowest(b.t, low);
    // On the line the new wall is still dropping in from above while the ball comes down: nothing in the way.
    const std::vector<int>& lowDown = b.t <= ts_ + kEps ? none : low;
    if (final) lowest(te_ - 1e-4, lowEnd);
    const double dy = std::max(1e-3, b.y - padContact_), v = b.v > 0 ? b.v : p_.speed;
    if (final && i > 0) {
        // An extra ball's last volley: down into the shield, which takes it (on a tick; on the line if it can). Any x
        // will do, so this always fits.
        double xs[kMaxPts], ys[kMaxPts];
        for (int d = std::max(1, int(std::ceil(dy / (v * g) - 1e-9))); d <= R; ++d) {
            const double L = v * d * g, D = std::sqrt(std::max(0.0, L * L - dy * dy));
            if (D > rx.flat * dy) break;
            for (int s = -1; s <= 1; s += 2) {
                const int np = trace(b.x, b.y, b.x + s * D, padContact_, xs, ys, kMaxPts);
                if (np < 2 || (rx.clear && !pathClear(xs, ys, np, lowDown))) continue;
                out.push_back({rnd() + (d == R ? 2 : 0), b.x + s * D, d, UpPick{}, true, true});
            }
        }
        std::sort(out.begin(), out.end(), [](const Volley& p, const Volley& q) { return p.score > q.score; });
        return;
    }
    const int dMin = std::max(1, int(std::ceil(dy / (v * g) - 1e-9)));
    const int dMax = std::max(dMin, int(std::floor(dy * std::hypot(1.0, std::min(rx.flat, 50.0)) / (v * g))));
    // Not the final volley: leave at least two ticks (decideVolley checks that the final one fits after it).
    const int dTop = std::min(dMax, final ? R - 1 : R - 3);
    double xs[kMaxPts], ys[kMaxPts];
    std::vector<UpPick> ups;
    const int stride = final ? 1 : std::max(1, (dTop - dMin + 1) / 8);
    for (int d = dMin; d <= dTop; d += stride)
        for (int s = -1; s <= 1; s += 2) {
            const double L = v * d * g, D = std::sqrt(std::max(0.0, L * L - dy * dy));
            if (D > rx.flat * dy) continue;
            const double X = b.x + s * D;
            const int np = trace(b.x, b.y, X, padContact_, xs, ys, kMaxPts);
            if (np < 2 || (rx.clear && !pathClear(xs, ys, np, lowDown))) continue;
            const double px = xs[np - 1], tp = b.t + d * g;
            if (final) bestUp(px, tp, R - d, R - d, rx, lowEnd, n > 0, true, ups, perDown);
            else bestUp(px, tp, 1, R - 2 - d, rx, low, n > 0, false, ups, perDown);
            for (const UpPick& up : ups)
                out.push_back({up.score + 0.5 * rnd() - (fast_ ? 0.3 * d * g : 0) -
                                   (pace_ > 0 && !final ? 2.0 * std::abs((d + up.n) * g - pace_) / pace_ : 0),
                               X, d, up, false, final});
        }
    std::sort(out.begin(), out.end(), [](const Volley& p, const Volley& q) { return p.score > q.score; });
}

void Game::commitVolley(int i, const Volley& vl, int n) {
    const BallState b = st_[std::size_t(i)];
    const double g = tick();
    const int R = std::max(1, int(std::floor((te_ - b.t) / g + 1e-6)));
    double xs[kMaxPts], ys[kMaxPts];
    const int np = trace(b.x, b.y, vl.X, padContact_, xs, ys, kMaxPts);
    const double tp = b.t + vl.d * g;
    pushPath(i, b.t, tp, xs, ys, np, i == 0 ? Hit::Paddle : Hit::Shield, 0, -1);
    if (vl.last) {
        st_[std::size_t(i)] = {tp, xs[np - 1], padContact_, true, true, b.v};
        return;
    }
    commitUp(i, xs[np - 1], tp, vl.up, n, vl.final, R - vl.d);
}

void Game::decideVolley(int i) {
    const BallState b = st_[std::size_t(i)];
    const double g = tick();
    const int R = std::max(1, int(std::floor((te_ - b.t) / g + 1e-6)));
    const int F = 2 * legMin();
    const bool final = R < 2 * F;
    int n = share(i, final);
    std::vector<Volley> cands, next;
    int level = 0;
    for (const Relax& rx : kRelax) {
        volleys(i, rx, n, cands, R < 5 * F ? 8 : 3);
        // No brick to break within the rules: knock on one instead (the pacing catches up later).
        if (cands.empty() && n > 0 && !final && rx.clear) {
            volleys(i, rx, 0, cands, R < 5 * F ? 8 : 3);
            if (!cands.empty()) n = 0;
        }
        if (cands.empty() && !final && rx.clear && level == 1 && downOnly(i, R - F - 1)) return;
        if (cands.empty() && i > 0 && level == 1) {
            // An extra ball with nothing left to do within the rules: down into the shield, which keeps it.
            volleys(i, rx, 0, cands, 8, true);
            if (!cands.empty()) {
                commitVolley(i, cands[0], 0);
                ++relaxed_[level];
                return;
            }
        }
        if (cands.empty()) {
            ++level;
            continue;
        }
        // Near the line, look ahead: take the best volley after which there is plenty of room, or the volleys up to
        // the line still fit at the base speed (tried on a copy of the plan, then undone); else the best one leaving
        // the room a final volley needs at worst.
        auto lookAhead = [&](const std::vector<Volley>& cs, int nb) {
            for (std::size_t k = 0; !final && k < cs.size() && k < 24; ++k) {
                if (R - (cs[k].d + cs[k].up.n) >= 4 * F) return k;
                const Snapshot sn = snapshot();
                commitVolley(i, cs[k], nb);
                const bool fits = fitsAhead(i, 1);
                restore(sn);
                if (fits) return k;
            }
            return cs.size();
        };
        std::size_t chosen = lookAhead(cands, n);
        if (chosen == cands.size() && n > 0 && !final && rx.clear) {
            // Knocking instead keeps a brick more standing for the final volleys to aim at.
            std::vector<Volley> knocks;
            volleys(i, rx, 0, knocks, R < 5 * F ? 8 : 3);
            const std::size_t k = lookAhead(knocks, 0);
            if (k < knocks.size()) {
                cands.swap(knocks);
                chosen = k;
                n = 0;
            }
        }
        if (chosen == cands.size()) {
            chosen = 0;
            int most = -1;
            for (std::size_t k = 0; !final && k < cands.size(); ++k) {
                const int left = R - (cands[k].d + cands[k].up.n);
                if (left >= F) {
                    chosen = k;
                    break;
                }
                if (left > most) {
                    most = left;
                    chosen = k;
                }
            }
        }
        commitVolley(i, cands[chosen], n);
        ++relaxed_[level];
        return;
    }
    // Nothing fits before the line: make this the final volley after all (at whatever level fits).
    if (!final) {
        const int nf = share(i, true);
        for (const Relax& rx : kRelax) {
            volleys(i, rx, nf, cands, 8, true);
            if (cands.empty()) continue;
            commitVolley(i, cands[0], nf);
            ++relaxed_[kLevels];
            return;
        }
    }
    // Nothing fits in time: down to the paddle at the ball's speed as soon as it can, then up as it can (decideUp).
    ++relaxed_[kLevels];
    const double dy = std::max(1e-3, b.y - padContact_), v = b.v > 0 ? b.v : p_.speed;
    const int d = std::max(1, std::min(R - 1, int(std::ceil(dy / (v * g) - 1e-9))));
    const double L = v * d * g, X = b.x + (rnd() < 0.5 ? -1 : 1) * std::sqrt(std::max(0.0, L * L - dy * dy));
    double xs[kMaxPts], ys[kMaxPts];
    const int np = trace(b.x, b.y, X, padContact_, xs, ys, kMaxPts);
    pushPath(i, b.t, b.t + d * g, xs, ys, np, i == 0 ? Hit::Paddle : Hit::Shield, 0, -1);
    st_[std::size_t(i)] = {b.t + d * g, xs[np - 1], padContact_, true, false, v};
}

// Nothing to aim at from up here: down to the paddle at the ball's speed in at most dTop ticks (clear of the bricks),
// to go on from there (decideUp: a brick, a knock, or up through a gap to the ceiling and back).
bool Game::downOnly(int i, int dTop) {
    const BallState b = st_[std::size_t(i)];
    const double g = tick(), dy = std::max(1e-3, b.y - padContact_), v = b.v > 0 ? b.v : p_.speed;
    std::vector<int> low;
    lowest(b.t, low);
    double xs[kMaxPts], ys[kMaxPts];
    for (int d = std::max(1, int(std::ceil(dy / (v * g) - 1e-9))); d <= dTop; ++d) {
        const double L = v * d * g, D = std::sqrt(std::max(0.0, L * L - dy * dy));
        if (D > kFlat * dy) break;
        const double s = rnd() < 0.5 ? -1 : 1;
        for (double side : {s, -s}) {
            const int np = trace(b.x, b.y, b.x + side * D, padContact_, xs, ys, kMaxPts);
            if (np < 2 || !pathClear(xs, ys, np, low)) continue;
            pushPath(i, b.t, b.t + d * g, xs, ys, np, i == 0 ? Hit::Paddle : Hit::Shield, 0, -1);
            st_[std::size_t(i)] = {b.t + d * g, xs[np - 1], padContact_, true, false, v};
            return true;
        }
    }
    return false;
}

// Ball i at the paddle with nothing to aim at: up through a gap in the wall to the ceiling and back down to the paddle
// (the ceiling bounce lands wherever it falls), in at most nTop ticks at the base speed, paced like a volley.
bool Game::sky(int i, const Relax& rx, int nTop) {
    const BallState b = st_[std::size_t(i)];
    const double g = tick(), v = p_.speed, r = 0.5 * p_.ball;
    const double top = p_.halfH - r, Vd = 2 * (top - padContact_);
    std::vector<int> low;
    lowest(b.t, low);
    double xs[kMaxPts], ys[kMaxPts], x2[kMaxPts], y2[kMaxPts];
    struct Opt {
        double score, X;
        int n;
    };
    std::vector<Opt> opts;
    for (int n = std::max(1, int(std::ceil(Vd / (v * g) - 1e-9))); n <= nTop; ++n) {
        const double L = v * n * g, D = std::sqrt(std::max(0.0, L * L - Vd * Vd));
        if (D > rx.flat * Vd) break;
        for (int s = -1; s <= 1; s += 2)
            opts.push_back({rnd() - (pace_ > 0 ? 2.0 * std::abs(n * g - pace_) / pace_ : 0), b.x + s * D, n});
    }
    std::sort(opts.begin(), opts.end(), [](const Opt& p, const Opt& q) { return p.score > q.score; });
    for (const Opt& o : opts) {
        const double Xm = 0.5 * (b.x + o.X);
        const int n1 = trace(b.x, padContact_, Xm, top, xs, ys, kMaxPts);
        const int n2 = trace(Xm, top, o.X, padContact_, x2, y2, kMaxPts);
        if (n1 < 2 || n2 < 2 || n1 + n2 - 1 > kMaxPts) continue;
        if (!pathClear(xs, ys, n1, low) || !pathClear(x2, y2, n2, low)) continue;
        for (int k = 1; k < n2; ++k) {
            xs[n1 - 1 + k] = x2[k];
            ys[n1 - 1 + k] = y2[k];
        }
        const int np = n1 + n2 - 1;
        if (i == 0) paddleKey(b.t, b.x, xs[1], ys[1]);
        pushPath(i, b.t, b.t + o.n * g, xs, ys, np, i == 0 ? Hit::Paddle : Hit::Shield, 0, -1);
        st_[std::size_t(i)] = {b.t + o.n * g, xs[np - 1], padContact_, true, false, v};
        return true;
    }
    return false;
}

// Whether ball i (at a brick) can still fly volleys at the base speed up to the line: the next one exists, and when
// it is not the final one and leaves little room, (depth) more after it.
bool Game::fitsAhead(int i, int depth) {
    const BallState b = st_[std::size_t(i)];
    if (b.done) return true;
    const double g = tick();
    const int R = std::max(1, int(std::floor((te_ - b.t) / g + 1e-6)));
    const int F = 2 * legMin();
    const bool final = R < 2 * F;
    const int n = share(i, final);
    std::vector<Volley> c;
    volleys(i, kRelax[1], n, c, final ? 1 : 4);
    if (c.empty()) return false;
    if (final || depth <= 0) return true;
    for (std::size_t k = 0; k < c.size() && k < 8; ++k) {
        if (R - (c[k].d + c[k].up.n) >= 4 * F) return true;
        const Snapshot sn = snapshot();
        commitVolley(i, c[k], n);
        const bool ok = fitsAhead(i, depth - 1);
        restore(sn);
        if (ok) return true;
    }
    return false;
}

Game::Snapshot Game::snapshot() const {
    Snapshot sn;
    sn.broken.reserve(bricks_.size());
    for (const Brick& br : bricks_) sn.broken.push_back(br.broken);
    sn.claim = claimBall_;
    sn.segs = segs_.size();
    sn.keys = padKeys_.size();
    sn.st = st_;
    sn.legBeats = legBeats_;
    sn.legs = legs_;
    sn.rng = rng_;
    return sn;
}

void Game::restore(const Snapshot& sn) {
    for (std::size_t k = 0; k < bricks_.size(); ++k) bricks_[k].broken = sn.broken[k];
    claimBall_ = sn.claim;
    segs_.resize(sn.segs);
    padKeys_.resize(sn.keys);
    st_ = sn.st;
    legBeats_ = sn.legBeats;
    legs_ = sn.legs;
    rng_ = sn.rng;
}

// Decides the balls' volleys in time order until every ball is planned past `until` (a few beats ahead of the
// clock, so the work is spread over the frames; the decisions come out the same however far ahead it plans).
void Game::plan(double until) {
    for (int guard = 0; guard < 200000; ++guard) {
        int i = -1;
        for (std::size_t j = 0; j < st_.size(); ++j)
            if (!st_[j].done && (i < 0 || st_[j].t < st_[std::size_t(i)].t - kEps)) i = int(j);
        if (i < 0 || st_[std::size_t(i)].t > until) return;
        const double t0 = st_[std::size_t(i)].t;
        if (t0 >= te_ - kEps) {
            st_[std::size_t(i)].done = true;
            continue;
        }
        if (st_[std::size_t(i)].atPaddle) decideUp(i);
        else decideVolley(i);
        if (st_[std::size_t(i)].t <= t0 + kEps) st_[std::size_t(i)].done = true;  // never stall
    }
}

void Game::newCycle(long k, bool carry, double build) {
    const double L = 4.0 * p_.cycBars;
    const double ts = double(k) * L;
    // Pieces of the walls before still flying.
    if (carry) {
        std::erase_if(prevFrags_, [&](const Frag& f) { return f.t + p_.fragLife <= ts; });
        for (std::size_t b = 0; b < bricks_.size(); ++b)
            if (bricks_[b].broken + p_.fragLife > ts && prevFrags_.size() < 4000)
                prevFrags_.push_back({bricks_[b].broken, bricks_[b].x, bricks_[b].y, bricks_[b].w, bricks_[b].h,
                                      bricks_[b].pen, std::uint32_t(cyc_ * 4099 + long(b))});
    } else {
        prevFrags_.clear();
    }
    // Where each ball ended (its final hit on the line).
    std::vector<BallState> carried;
    if (carry)
        for (int i = 0; i < balls_; ++i) {
            const Seg* last = nullptr;
            for (const Seg& s : segs_)
                if (s.ball == i) last = &s;
            if (last) carried.push_back({ts, last->x1, last->y1, last->end == Hit::Paddle || last->end == Hit::Shield,
                                         false, speedOf(*last)});
        }
    cyc_ = k;
    ts_ = ts;
    te_ = ts + L;
    fast_ = p_.buildMul > 1 && build > 0.35;
    balls_ = multiUntil_ > ts_ + kEps ? 1 + p_.multi : 1;
    rng_ = mix64(seed_ ^ mix64(std::uint64_t(k) + 0x6a09e667ULL));
    layoutWall(k);
    legBeats_ = 0;
    legs_ = 0;
    if (carried.empty()) {
        // A fresh start: the ball drops from under the middle of the wall.
        double bottom = top_;
        for (const Brick& br : bricks_) bottom = std::min(bottom, br.y - 0.5 * br.h);
        carried.push_back({ts_, (rnd() - 0.5) * p_.halfW, bottom - 0.5 * p_.ball - 0.05, false, false, p_.speed});
        padKeys_.clear();
        padKeys_.push_back({ts_, 0.0});
    } else if (!padKeys_.empty()) {
        // Keep the paddle's last position before the line.
        auto it = std::upper_bound(padKeys_.begin(), padKeys_.end(), ts_ + kEps,
                                   [](double t, const std::pair<double, double>& k) { return t < k.first; });
        const auto keep = it == padKeys_.begin() ? padKeys_.front() : *(it - 1);
        padKeys_.assign(1, keep);
    }
    st_.clear();
    for (int i = 0; i < balls_; ++i) st_.push_back(i < int(carried.size()) ? carried[std::size_t(i)] : carried[0]);
    segs_.clear();
    ++plans_;
}

void Game::replan(double u) {
    // Each ball keeps the leg it is on (and the bricks that leg's hit breaks); the rest is planned again.
    std::vector<double> keep(std::size_t(balls_), -1);
    st_.resize(std::size_t(balls_));
    for (int i = 0; i < balls_; ++i) {
        const Seg* resume = nullptr;
        for (const Seg& s : segs_)
            if (s.ball == i && s.legEnd && s.t1 > u + kEps) {
                resume = &s;
                break;
            }
        if (!resume)
            for (const Seg& s : segs_)
                if (s.ball == i) resume = &s;
        if (!resume) continue;  // a new ball
        keep[std::size_t(i)] = resume->t1;
        st_[std::size_t(i)] = {resume->t1, resume->x1, resume->y1,
                               resume->end == Hit::Paddle || resume->end == Hit::Shield, resume->t1 >= te_ - kEps,
                               speedOf(*resume)};
    }
    std::erase_if(segs_, [&](const Seg& s) {
        return s.ball >= balls_ || keep[std::size_t(s.ball)] < 0 || s.t1 > keep[std::size_t(s.ball)] + kEps;
    });
    for (std::size_t b = 0; b < bricks_.size(); ++b) {
        const int c = claimBall_[b];
        if (c >= 0 && (c >= balls_ || keep[std::size_t(c)] < 0 || bricks_[b].broken > keep[std::size_t(c)] + kEps)) {
            claimBall_[b] = -1;
            bricks_[b].broken = kInf;
        }
    }
    if (keep[0] >= 0)
        std::erase_if(padKeys_, [&](const std::pair<double, double>& k) { return k.first > keep[0] - kEps; });
    // New balls split off ball 0 at its next bounce.
    for (int i = 1; i < balls_; ++i)
        if (keep[std::size_t(i)] < 0) st_[std::size_t(i)] = st_[0];
    ++plans_;
}

void Game::update(double bar, double drop, double build) {
    if (!std::isfinite(bar) || std::abs(bar) > 1e9) return;
    drop = std::isfinite(drop) ? drop : 0;
    build = std::isfinite(build) ? build : 0;
    const double u = 4 * bar, L = 4.0 * p_.cycBars;
    const long k = long(std::floor(bar / p_.cycBars));
    if (!started_ || k != cyc_) {
        newCycle(k, started_ && k == cyc_ + 1, build);
        started_ = true;
    }
    bool again = false;
    // A drop splits the ball, until this cycle's line (or the next one when this one is about to end).
    if (dropArmed_ && drop > 0.6) {
        dropArmed_ = false;
        if (p_.multi > 0) {
            multiUntil_ = std::max(multiUntil_, te_ - u >= 8 ? te_ : te_ + L);
            if (balls_ == 1) {
                balls_ = 1 + p_.multi;
                again = true;
            }
        }
    } else if (drop < 0.25) {
        dropArmed_ = true;
    }
    // Through a breakdown the bounces come faster; back to normal on the next cycle line.
    if (!fast_ && p_.buildMul > 1 && build > 0.35) {
        fast_ = true;
        again = true;
    }
    if (again) replan(u);
    plan(u + kHorizon);
    u_ = u;
}

// ------------------------------------------------------------------ queries

double Game::paddleX(double u) const {
    if (padKeys_.empty()) return 0;
    auto it = std::upper_bound(padKeys_.begin(), padKeys_.end(), u,
                               [](double t, const std::pair<double, double>& k) { return t < k.first; });
    if (it == padKeys_.begin()) return padKeys_.front().second;
    if (it == padKeys_.end()) return padKeys_.back().second;
    const auto& a = *(it - 1);
    const auto& b = *it;
    // Glides over the middle of the interval, so it waits under the ball before each hit.
    const double s = (u - a.first) / std::max(1e-9, b.first - a.first);
    return a.second + (b.second - a.second) * smooth((s - 0.12) / 0.6);
}

bool Game::ballAt(int i, double u, double& x, double& y) const {
    for (const Seg& s : segs_)
        if (s.ball == i && u >= s.t0 - kEps && u <= s.t1 + kEps) {
            const double a = std::clamp((u - s.t0) / std::max(1e-9, s.t1 - s.t0), 0.0, 1.0);
            x = s.x0 + (s.x1 - s.x0) * a;
            y = s.y0 + (s.y1 - s.y0) * a;
            return true;
        }
    return false;
}

std::string Game::summary() const {
    char b[160];
    std::snprintf(b, sizeof b, "%d x %d bricks cleared every %d bars, bouncing on %g tick(s) per beat%s", p_.cols,
                  p_.rows, p_.cycBars, p_.rate, p_.multi ? ", multiball on drops" : "");
    return b;
}

// ------------------------------------------------------------------ drawing

void Game::drawFrags(OrganicCanvas& c, double pen, double x, double y, double w, double h, float bpen, double tb,
                     std::uint32_t id) const {
    const double tau = u_ - tb;
    if (tau < 0 || tau >= p_.fragLife || p_.frags <= 0) return;
    const double a = tau / p_.fragLife, fade = std::pow(1 - a, 1.5);
    const double pp = pen * std::min(1.0, bpen + 0.3 * (1 - a)) * fade;
    if (pp <= 0.01) return;
    const double s = 1 - 0.55 * a;
    for (int k = 0; k < p_.frags; ++k) {
        const std::uint64_t hk = mix64(seed_ ^ mix64(std::uint64_t(id) * 131 + std::uint64_t(k)));
        const double r1 = unit(hk), r2 = unit(mix64(hk + 1)), r3 = unit(mix64(hk + 2)), r4 = unit(mix64(hk + 3));
        // Pieces from a 3 x 2 grid over the brick, blown up and out, then falling (tree units per beat).
        const double ox = ((k % 3) - 1 + (r4 - 0.5) * 0.4) * w / 3, oy = ((k / 3) % 2 - 0.5) * h / 2;
        const double vx = 1.6 * ox / std::max(0.01, w) + (r1 - 0.5) * 0.9, vy = 0.3 + 0.8 * r2;
        const double px = x + ox + vx * tau, py = y + oy + vy * tau - 1.1 * tau * tau;
        if (py < -p_.halfH - 0.4) continue;
        const double hw = w / 6 * s, hh = h / 4 * s, th = (r3 - 0.5) * 7 * tau;
        const double cs = std::cos(th), sn = std::sin(th);
        const double xs[4] = {px + cs * -hw - sn * -hh, px + cs * hw - sn * -hh, px + cs * hw - sn * hh,
                              px + cs * -hw - sn * hh};
        const double ys[4] = {py + sn * -hw + cs * -hh, py + sn * hw + cs * -hh, py + sn * hw + cs * hh,
                              py + sn * -hw + cs * hh};
        c.fillPolygon(xs, ys, 4, pp);
    }
}

void Game::draw(OrganicCanvas& c, double pen) const {
    if (!started_) return;
    const double u = u_, W = p_.halfW, H = p_.halfH, r = 0.5 * p_.ball;
    const double since = std::max(0.0, u - ts_);
    // The walls, flashing on the cycle line.
    const double flash = std::exp(-2.5 * since);
    const double wp = pen * std::clamp(p_.wallPen + (1 - p_.wallPen) * flash, 0.0, 1.0);
    c.line(-W, -H, -W, H, 2, wp);
    c.line(W, -H, W, H, 2, wp);
    c.line(-W, H, W, H, 2, wp);
    // The wall drops in over the first beat (two ticks when they are shorter), bright as it lands.
    const double dropDur = std::min(1.0, 2 * tick());
    const double a = std::clamp(since / dropDur, 0.0, 1.0);
    const double off = std::pow(1 - a, 3) * (2 * H);
    const double fresh = std::exp(-3 * since);
    // A hit that breaks nothing (the ball knocking on a brick kept for later) flashes it.
    std::vector<float> knock(bricks_.size(), 0.f);
    for (const Seg& s : segs_)
        if (s.end == Hit::Brick && s.bricks == 0 && s.brick >= 0 && s.brick < int(knock.size()) && u >= s.t1 &&
            u < s.t1 + 1)
            knock[std::size_t(s.brick)] = std::max(knock[std::size_t(s.brick)], float(std::exp(-5 * (u - s.t1))));
    for (std::size_t k = 0; k < bricks_.size(); ++k) {
        const Brick& b = bricks_[k];
        const double lift = std::max(0.7 * fresh, double(knock[k]));
        if (b.broken > u) c.rect(b.x, b.y + off, b.w, b.h, pen * (b.pen + (1 - b.pen) * lift));
    }
    for (std::size_t b = 0; b < bricks_.size(); ++b) {
        const Brick& br = bricks_[b];
        if (br.broken <= u)
            drawFrags(c, pen, br.x, br.y, br.w, br.h, br.pen, br.broken, std::uint32_t(cyc_ * 4099 + long(b)));
    }
    for (const Frag& f : prevFrags_) drawFrags(c, pen, f.x, f.y, f.w, f.h, f.pen, f.t, f.id);
    // The paddle, brighter for a moment when it hits.
    double glow = 0;
    for (auto it = padKeys_.rbegin(); it != padKeys_.rend(); ++it)
        if (it->first <= u + kEps) {
            glow = std::exp(-6 * (u - it->first));
            break;
        }
    c.rect(paddleX(u), padY_, p_.paddle, padH_, pen * (0.8 + 0.2 * glow));
    // The balls; extra balls bounce off a shield along the bottom.
    bool shield = false;
    for (int i = 0; i < balls_; ++i) {
        double x, y;
        if (!ballAt(i, u, x, y)) continue;
        shield = shield || i > 0;
        c.dotUnits(x, y, p_.ball, pen);
    }
    if (shield) c.line(-W, padContact_ - r, W, padContact_ - r, 1.2, pen * (0.3 + 0.15 * std::cos(6.283185307 * u)));
}

}  // namespace bricks

namespace {

class Bricks : public OrganicBody {
public:
    Bricks(scene::OrganicInstance& in, std::uint64_t seed)
        : params_(bricks::paramsFrom(in)), fieldSet_(std::isfinite(in.loaded("FLDW", NAN))), seed_(seed) {
        game_.emplace(params_, seed_);
    }
    void step(const OrganicStep& s, OrganicCanvas& c) override {
        // Without FldW the playfield fits the frame's width (1.6 at 16:9, the frame's aspect otherwise); the game is
        // made again only if that differs from the 16:9 one it started with (once, on the first step).
        if (!fieldSet_ && !fitted_) {
            fitted_ = true;
            const double w = 0.9 * std::clamp(c.halfWidth(), 0.25, 8.0);
            if (std::abs(w - params_.halfW) > 1e-6) {
                params_.halfW = w;
                game_.emplace(params_, seed_);
            }
        }
        game_->update(s.bar, dropOf(s), buildOf(s));
        game_->draw(c, s.pen);
    }
    std::string summary() const override { return game_->summary(); }

private:
    bricks::Params params_;
    bool fieldSet_, fitted_ = false;
    std::uint64_t seed_;
    std::optional<bricks::Game> game_;
};

}  // namespace

std::unique_ptr<OrganicBody> makeBricks(scene::OrganicInstance& inst, std::uint64_t seed) {
    return std::make_unique<Bricks>(inst, seed);
}

}  // namespace reforce::engine::arcade
