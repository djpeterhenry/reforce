// Arcade organic `hopper` (PLAN 5Q.2, docs/arcade-design.md): a round sprout-headed creature hops one cube per beat
// over an isometric pyramid of cubes; every cycle starts all dark and each landing steps a top towards bright. Its
// route is planned so the last top turns bright exactly on the cycle line (Cyc bars, like the Rubik cube solved on
// the 8-bar line); then the board flashes and goes dark again for the next cycle. Balls drop onto the apex on bar lines and bounce down a row per
// beat, routed around the player. A drop sends the player round on a spinning disc to the apex; a breakdown lifts the
// hops and makes the tops shimmer. Line art into the intensity buffer, so it trails and takes the ColorMap.
#include "engine/arcade/Hopper.hpp"

#include <cstdio>

#include "engine/arcade/Arcade.hpp"
#include "expr/Rng.hpp"

namespace reforce::engine::arcade {

namespace hopper {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kCrouch = 0.2;      // share of a beat spent on the cube after a landing, before the next take-off
constexpr long kWalkBudget = 3000;  // search nodes per attempt (a 7-row board takes a few hundred); short
constexpr int kWalkAttempts = 12;   // attempts with fresh seeded orders beat long backtracking (heavy-tailed)

int neighbors(int cube, int rows, int out[4]) {
    const int r = rowOf(cube), c = colOf(cube);
    int n = 0;
    if (r > 0 && c > 0) out[n++] = cubeIndex(r - 1, c - 1);  // up-left
    if (r > 0 && c < r) out[n++] = cubeIndex(r - 1, c);      // up-right
    if (r + 1 < rows) {
        out[n++] = cubeIndex(r + 1, c);      // down-left
        out[n++] = cubeIndex(r + 1, c + 1);  // down-right
    }
    return n;
}

// Landings 1..hops alternate between even and odd rows (every hop changes the row by one), so from a start on row
// parity p each parity gets a fixed share; both must cover their cubes `steps` times (levels - 1: dark to bright).
bool feasible(int rows, int hops, int steps) {
    const int m = steps * cubeCount(rows);
    int even = 0;
    for (int r = 0; r < rows; r += 2) even += steps * (r + 1);
    if (hops < m) return false;
    for (int p = 0; p < 2; ++p) {
        int onEven = 0;
        for (int j = 1; j <= hops; ++j) onEven += ((p + j) & 1) == 0;
        if (onEven < even || hops - onEven < m - even) return false;
    }
    return true;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }
double smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

}  // namespace

int rowOf(int cube) {
    int r = 0;
    while (cubeIndex(r + 1, 0) <= cube) ++r;
    return r;
}
int colOf(int cube) { return cube - cubeIndex(rowOf(cube), 0); }
bool adjacent(int a, int b) {
    const int ra = rowOf(a), rb = rowOf(b), ca = colOf(a), cb = colOf(b);
    if (rb == ra + 1) return cb == ca || cb == ca + 1;
    if (ra == rb + 1) return ca == cb || ca == cb + 1;
    return false;
}
double topX(int cube) { return colOf(cube) - 0.5 * rowOf(cube); }
double topY(int cube) { return -0.75 * rowOf(cube); }

int cycleBarsFor(int rows, int cycBars, int levels) {
    rows = std::clamp(rows, kMinRows, kMaxRows);
    cycBars = std::clamp(cycBars, 1, 64);
    const int steps = std::clamp(levels, 2, 3) - 1;
    while (cycBars < 64 && !feasible(rows, 4 * cycBars, steps)) cycBars = std::min(64, cycBars * 2);
    return cycBars;
}

Game::Game(const Config& c)
    : rows_(std::clamp(c.rows, kMinRows, kMaxRows)), cubes_(cubeCount(rows_)),
      hops_(4 * cycleBarsFor(rows_, c.cycBars, c.levels)), levels_(std::clamp(c.levels, 2, 3)),
      ballsPerBar_(std::clamp(finiteOr(c.ballsPerBar, 0), 0.0, 4.0)), offbeat_(c.offbeat), seed_(c.seed) {
    // Hop distances (breadth-first from every cube): the route search's "can it still get there" test.
    dist_.assign(std::size_t(cubes_ * cubes_), 255);
    std::vector<int> queue(static_cast<std::size_t>(cubes_));
    for (int s = 0; s < cubes_; ++s) {
        unsigned char* d = &dist_[std::size_t(s * cubes_)];
        int head = 0, tail = 0;
        d[s] = 0;
        queue[std::size_t(tail++)] = s;
        while (head < tail) {
            const int a = queue[std::size_t(head++)];
            int nb[4];
            const int n = neighbors(a, rows_, nb);
            for (int i = 0; i < n; ++i)
                if (d[nb[i]] == 255) {
                    d[nb[i]] = (unsigned char)(d[a] + 1);
                    queue[std::size_t(tail++)] = nb[i];
                }
        }
    }
}

std::uint64_t Game::seedFor(long long cycle, std::uint64_t salt) const {
    return seed_ ^ (std::uint64_t(cycle) * 0x9e3779b97f4a7c15ULL) ^ (salt * 0xc2b2ae3d27d4eb4fULL);
}

namespace {

// Depth-first route search with Warnsdorff ordering (cubes still to brighten with the fewest such neighbors first,
// seeded ties) over the steps each cube still needs (`need`), and four cuts: every cube still to brighten within reach,
// enough landings left on each row parity for its steps, the cycle's last landing (on a fixed parity) still has a step
// to take, and that step is not on the only way into a corner that needs as many visits.
struct Walker {
    const int rows, cubes, hops, from, par0;
    const unsigned char* dist;
    std::vector<int>& pos;
    expr::Pcg32& rng;
    unsigned char need[64] = {};
    int sum[2] = {0, 0};   // steps still needed per row parity
    int open[2] = {0, 0};  // cubes still needing a step per row parity
    long budget = 0;

    // Landings j in [a, hops] on rows of parity cls.
    int lands(int a, int cls) const {
        const int n = hops - a + 1;
        if (n <= 0) return 0;
        const int first = (par0 + a - from) & 1;
        return cls == first ? (n + 1) / 2 : n / 2;
    }
    bool dfs(int i) {
        if (i == hops) return true;
        if (--budget < 0) return false;
        int nb[4];
        const int n = neighbors(pos[std::size_t(i)], rows, nb);
        int key[4];
        for (int k = 0; k < n; ++k) {
            const int q = nb[k];
            if (need[q] > 0) {
                int nn[4], fresh = 0;
                const int m = neighbors(q, rows, nn);
                for (int t = 0; t < m; ++t) fresh += need[nn[t]] > 0;
                key[k] = fresh * 8 + int(rng.next() % 12);
            } else {
                key[k] = 1000 + int(rng.next() % 64);
            }
        }
        int order[4] = {0, 1, 2, 3};
        std::sort(order, order + n, [&](int a, int b) { return key[a] < key[b]; });
        const int lastCls = (par0 + hops - from) & 1;
        for (int oi = 0; oi < n; ++oi) {
            const int q = nb[order[oi]];
            const bool fresh = need[q] > 0;
            const int cls = rowOf(q) & 1;
            int s2[2] = {sum[0], sum[1]}, o2[2] = {open[0], open[1]};
            if (fresh) {
                --s2[cls];
                if (need[q] == 1) --o2[cls];
                --need[q];
            }
            const int left = s2[0] + s2[1];
            bool ok;
            if (i + 1 == hops) {
                ok = fresh && left == 0;
            } else {
                const int rem = hops - (i + 1);
                ok = left >= 1 && left <= rem && s2[0] <= lands(i + 2, 0) && s2[1] <= lands(i + 2, 1) &&
                     o2[lastCls] >= 1;
                for (int x = 0; x < cubes && ok; ++x)
                    if (need[x] > 0 && dist[q * cubes + x] > rem) ok = false;
                // A bottom corner has one neighbor, landed on before each visit: while the corner needs as many
                // visits as that neighbor has steps left, the neighbor's last step cannot be the cycle's last.
                if (ok && ((rows - 1) & 1) != lastCls) {
                    int forced = 0;
                    for (int side = 0; side < 2; ++side) {
                        const int corner = cubeIndex(rows - 1, side ? rows - 1 : 0);
                        const int nbr = cubeIndex(rows - 2, side ? rows - 2 : 0);
                        forced += need[corner] > 0 && need[nbr] > 0 && need[nbr] <= need[corner];
                    }
                    ok = o2[lastCls] - forced >= 1;
                }
            }
            if (ok) {
                const int ss[2] = {sum[0], sum[1]}, so[2] = {open[0], open[1]};
                sum[0] = s2[0], sum[1] = s2[1], open[0] = o2[0], open[1] = o2[1];
                pos[std::size_t(i + 1)] = q;
                if (dfs(i + 1)) return true;
                sum[0] = ss[0], sum[1] = ss[1], open[0] = so[0], open[1] = so[1];
            }
            if (fresh) ++need[q];
        }
        return false;
    }
};

}  // namespace

bool Game::walk(std::vector<int>& pos, int from, const std::vector<int>& need, std::uint64_t rngSeed) const {
    if (from < 0 || from >= hops_ || int(pos.size()) != hops_ + 1 || pos[std::size_t(from)] < 0 ||
        int(need.size()) != cubes_)
        return false;
    expr::Pcg32 rng(rngSeed);
    for (int attempt = 0; attempt < kWalkAttempts; ++attempt) {
        Walker w{rows_, cubes_, hops_, from, rowOf(pos[std::size_t(from)]) & 1, dist_.data(), pos, rng};
        for (int x = 0; x < cubes_; ++x) {
            const int k = std::clamp(need[std::size_t(x)], 0, 2);
            w.need[x] = (unsigned char)k;
            w.sum[rowOf(x) & 1] += k;
            w.open[rowOf(x) & 1] += k > 0;
        }
        if (w.sum[0] + w.sum[1] == 0) return false;
        w.budget = kWalkBudget;
        if (w.dfs(from)) return true;
    }
    return false;
}

bool Game::completes(const std::vector<int>& pos) const {
    const int steps = levels_ - 1;
    std::vector<int> count(std::size_t(cubes_), 0);
    int lastDone = 0;
    for (int k = 1; k <= hops_; ++k) {
        const int q = pos[std::size_t(k)];
        if (q >= 0 && ++count[std::size_t(q)] == steps) lastDone = k;
    }
    for (int x = 0; x < cubes_; ++x)
        if (count[std::size_t(x)] < steps) return false;
    return lastDone == hops_;  // the landing on the line is some cube's last step, so every other came before
}

void Game::computeFlips() {
    // Each cube's first levels - 1 landings step it towards bright; later ones change nothing.
    const int steps = levels_ - 1;
    plan_.stepAt.assign(std::size_t(cubes_ * steps), hops_ + 1);
    plan_.doneAt.assign(std::size_t(cubes_), hops_ + 1);
    std::vector<int> count(std::size_t(cubes_), 0);
    for (int k = 1; k <= hops_; ++k) {
        const int q = plan_.pos[std::size_t(k)];
        if (q < 0 || count[std::size_t(q)] >= steps) continue;
        plan_.stepAt[std::size_t(q * steps + count[std::size_t(q)])] = k;
        if (++count[std::size_t(q)] == steps) plan_.doneAt[std::size_t(q)] = k;
    }
}

bool Game::placeBall(Ball& b, std::uint64_t rngSeed) const {
    expr::Pcg32 rng(rngSeed);
    const auto& pos = plan_.pos;
    auto at = [&](int t) { return t >= 0 && t <= hops_ ? pos[std::size_t(t)] : -2; };
    // On the beat a ball meets the player when both land on one cube, or when they swap cubes over a beat; on the
    // off-beat when it lands on a cube the player has just left or is about to land on.
    auto clear = [&](int j, int cube, int prev) {
        const int t = b.t0 + j;
        if (offbeat_) return at(t) != cube && at(t + 1) != cube;
        return at(t) != cube && !(j > 0 && at(t - 1) == cube && at(t) == prev);
    };
    int cols[kMaxRows + 1];
    // Depth-first over the left / right bounces (at most 2^9 paths), seeded order.
    struct Frame {
        int col, tried;
        bool rightFirst;
    };
    Frame st[kMaxRows];
    int j = 0;
    if (!clear(0, 0, -1)) return false;
    cols[0] = 0;
    st[0] = {0, 0, (rng.next() & 1) != 0};
    while (j >= 0) {
        if (j == rows_ - 1) {
            for (int r = 0; r < rows_; ++r) b.col[r] = cols[r];
            b.col[rows_] = cols[rows_ - 1] + int(rng.next() & 1);
            return true;
        }
        Frame& f = st[j];
        if (f.tried >= 2) {
            --j;
            continue;
        }
        const int dc = (f.tried == 0) == f.rightFirst ? 1 : 0;
        ++f.tried;
        const int c = cols[j] + dc;
        if (!clear(j + 1, cubeIndex(j + 1, c), cubeIndex(j, cols[j]))) continue;
        cols[j + 1] = c;
        ++j;
        st[j] = {c, 0, (rng.next() & 1) != 0};
    }
    return false;
}

void Game::placeBalls(std::uint64_t rngSeed, double after) {
    const double off = offbeat_ ? 0.5 : 0.0;
    if (after < -1e299) {
        plan_.balls.clear();
        if (ballsPerBar_ <= 0) return;
        // Ball n falls onto the apex at beat ceil(4 n / Ball): on the bar lines at 1 per bar. Not on the cycle line
        // (it would pop out of nowhere: its fall starts in the previous cycle), and only while it can bounce down and
        // fall off before the next line.
        for (int n = 1; n < 256; ++n) {
            const int t0 = int(std::ceil(4.0 * n / ballsPerBar_ - 1e-9));
            if (t0 + rows_ + 1 > hops_) break;
            if (!plan_.balls.empty() && plan_.balls.back().t0 == t0) continue;
            Ball b;
            b.t0 = t0;
            b.alive = placeBall(b, rngSeed + std::uint64_t(n) * 7919);
            plan_.balls.push_back(b);
        }
        return;
    }
    for (std::size_t i = 0; i < plan_.balls.size(); ++i) {
        Ball& b = plan_.balls[i];
        if (b.t0 + off - 1 > after) b.alive = placeBall(b, rngSeed + i * 104729);
    }
}

void Game::newCycle(long long cycle) {
    int start = 0;  // the apex
    if (havePlan_) {
        if (cycle == plan_.cycle + 1) {
            start = plan_.pos[std::size_t(hops_)];
        } else {
            // The clock jumped: carry on from where the player stood.
            for (int k = landing(); k >= 0; --k)
                if (plan_.pos[std::size_t(k)] >= 0) {
                    start = plan_.pos[std::size_t(k)];
                    break;
                }
        }
    }
    plan_ = Plan{};
    plan_.cycle = cycle;
    plan_.hops = hops_;
    plan_.pos.assign(std::size_t(hops_ + 1), start);
    plan_.searched = walk(plan_.pos, 0, std::vector<int>(std::size_t(cubes_), levels_ - 1), seedFor(cycle, 1));
    if (!plan_.searched) {
        // Never seen in the tests; keep it on the pyramid (down and up again); the line resets the board anyway.
        for (int k = 1; k <= hops_; ++k) {
            int nb[4];
            const int n = neighbors(plan_.pos[std::size_t(k - 1)], rows_, nb);
            plan_.pos[std::size_t(k)] = nb[k % n];
        }
    }
    computeFlips();
    placeBalls(seedFor(cycle, 2), -1e300);
    havePlan_ = true;
    ++plansMade_;
}

void Game::onDrop(double hc) {
    if (hc - plan_.dropAt < 4 || (plan_.rideTo >= 0 && hc < plan_.rideTo)) return;  // one at a time
    plan_.dropAt = hc;
    // The drop bursts every ball on the board.
    for (auto& b : plan_.balls) {
        const BallPose p = ballPose(b, 0.5);
        if (p.visible && p.burst < 0) b.popAt = hc;
    }
    // Ride: the disc picks the player up now and sets it down two landings on, on the apex if a route from there
    // still turns every remaining top by the line, else back on the planned route (searching again when the landing
    // it misses would have turned a top).
    const int k = std::clamp(int(std::floor(hc + 1e-9)), 0, hops_ - 1);
    const int to = k + 2;
    if (to < hops_) {
        // The steps each cube still needs after landing k (the landing at k + 1 is lost to the ride).
        std::vector<int> need(std::size_t(cubes_), levels_ - 1);
        for (int j = 1; j <= k; ++j)
            if (const int q = plan_.pos[std::size_t(j)]; q >= 0) need[std::size_t(q)] = std::max(0, need[std::size_t(q)] - 1);
        const int planned = plan_.pos[std::size_t(to)];
        for (int pass = 0; pass < 2; ++pass) {
            const int target = pass == 0 ? 0 : planned;
            if (target < 0 || (pass == 1 && planned == 0)) continue;
            std::vector<int> pos = plan_.pos;
            pos[std::size_t(k + 1)] = -1;
            pos[std::size_t(to)] = target;
            bool ok = target == planned && completes(pos);
            if (!ok) {
                std::vector<int> left = need;
                left[std::size_t(target)] = std::max(0, left[std::size_t(target)] - 1);  // landed on at `to`
                ok = walk(pos, to, left, seedFor(plan_.cycle, 3) + std::uint64_t(k));
                pos[std::size_t(k + 1)] = -1;
            }
            if (!ok) continue;
            const Pose p = pose(0.55);
            plan_.pos = pos;
            plan_.rideFrom = hc;
            plan_.rideTo = to;
            plan_.rideX = p.x;
            plan_.rideY = p.y;
            plan_.spinHop = -1;
            computeFlips();
            placeBalls(seedFor(plan_.cycle, 4) + std::uint64_t(k), hc);
            ++plansMade_;
            return;
        }
    }
    // No route left: the next hop is a high spinning jump instead (on the planned route).
    if (k + 1 < hops_) plan_.spinHop = k + 1;
}

void Game::advance(double h, bool drop) {
    h = finiteOr(h, 0);
    h = std::clamp(h, -1e15, 1e15);
    const auto cycle = (long long)std::floor(h / hops_);
    if (!havePlan_ || cycle != plan_.cycle) newCycle(cycle);  // (reads where the player stood: the old hc_)
    hc_ = std::clamp(h - double(cycle) * hops_, 0.0, double(hops_) - 1e-9);
    if (drop) onDrop(hc_);
}

int Game::landing() const { return std::clamp(int(std::floor(hc_ + 1e-9)), 0, hops_ - 1); }

bool Game::bright(int cube) const { return state(cube) == levels_ - 1; }

int Game::state(int cube) const {
    const int steps = levels_ - 1;
    if (cube < 0 || cube >= cubes_ || plan_.stepAt.size() != std::size_t(cubes_ * steps)) return 0;
    const int k = landing();
    int s = 0;
    while (s < steps && plan_.stepAt[std::size_t(cube * steps + s)] <= k) ++s;
    return s;
}

Pose Game::pose(double hop) const {
    Pose p;
    if (!havePlan_) return p;
    const double hc = hc_;
    if (plan_.rideTo >= 0 && hc >= plan_.rideFrom && hc < plan_.rideTo) {
        // Out over the nearer edge, round and up, then over the apex; the last fifth is a hop off the disc.
        const double v = (hc - plan_.rideFrom) / (plan_.rideTo - plan_.rideFrom);
        const double side = plan_.rideX >= 0 ? 1.0 : -1.0;
        const int target = std::max(0, plan_.pos[std::size_t(plan_.rideTo)]);
        const double tx = topX(target), ty = topY(target);
        const double p0x = plan_.rideX, p0y = plan_.rideY;
        const double c1x = p0x + side * 1.8, c1y = p0y + 0.6;
        const double c2x = side * (0.5 * rows_ + 1.0), c2y = std::max(2.0, ty + 2.5);
        const double p3x = tx, p3y = ty + 1.0;
        auto bez = [&](double w, double& x, double& y) {
            const double a = 1 - w;
            x = a * a * a * p0x + 3 * a * a * w * c1x + 3 * a * w * w * c2x + w * w * w * p3x;
            y = a * a * a * p0y + 3 * a * a * w * c1y + 3 * a * w * w * c2y + w * w * w * p3y;
        };
        constexpr double kOff = 0.8;
        p.onDisc = true;
        p.discSpin = 7 * kPi * (hc - plan_.rideFrom);
        if (v < kOff) {
            const double w = smooth(v / kOff);
            bez(w, p.x, p.y);
            double nx, ny;
            bez(std::min(1.0, w + 0.02), nx, ny);
            p.facing = nx >= p.x ? 1 : -1;
            p.discX = p.x;
            p.discY = p.y;
            p.discScale = std::min(1.0, v / 0.12);
            p.ground = p.y;
        } else {
            const double e = (v - kOff) / (1 - kOff);
            p.x = lerp(p3x, tx, e);
            p.ground = lerp(p3y, ty, e);
            p.air = 4 * 0.5 * e * (1 - e);
            p.y = p.ground + p.air;
            p.facing = 1;
            p.discX = p3x;
            p.discY = p3y + 1.5 * e * e;
            p.discScale = 1 - e;
            p.squash = 1 + 0.12 * std::sin(kPi * e);
        }
        return p;
    }
    const int k = landing();
    const double u = std::clamp(hc - k, 0.0, 1.0);
    int a = plan_.pos[std::size_t(k)], b = plan_.pos[std::size_t(k + 1)];
    if (a < 0) a = b >= 0 ? b : 0;
    if (b < 0) b = a;
    const double xa = topX(a), ya = topY(a), xb = topX(b), yb = topY(b);
    p.facing = xb < xa ? -1 : 1;
    p.back = rowOf(b) < rowOf(a);
    const bool spin = k == plan_.spinHop;
    const double hgt = hop * (spin ? 3.2 : 1.0);
    if (u < kCrouch) {
        p.x = xa;
        p.y = p.ground = ya;
        p.squash = 1 - 0.28 * std::sin(kPi * u / kCrouch);
    } else {
        const double e = (u - kCrouch) / (1 - kCrouch);
        p.x = lerp(xa, xb, e);
        p.ground = lerp(ya, yb, e);
        p.air = 4 * hgt * e * (1 - e);
        p.y = p.ground + p.air;
        p.squash = 1 + 0.14 * std::sin(kPi * e);
        if (spin) p.spin = -2 * kPi * p.facing * smooth(e);
    }
    return p;
}

BallPose Game::ballPose(const Ball& b, double hop) const {
    BallPose p;
    if (!b.alive) return p;
    const double hc = std::min(hc_, b.popAt);
    if (hc_ >= b.popAt) {
        p.burst = (hc_ - b.popAt) / 0.75;
        if (p.burst >= 1) return p;
    }
    const double tau = hc - (b.t0 + (offbeat_ ? 0.5 : 0.0));
    const int R = rows_;
    if (tau < -1 || tau > R + 0.7) return p;
    p.visible = true;
    if (tau < 0) {
        p.x = 0;
        p.y = 3.5 * (1 - (tau + 1) * (tau + 1));  // falling in from above, onto the apex at tau = 0
        p.squash = 1.1;
        return p;
    }
    const int j = std::min(int(std::floor(tau)), R - 1);
    const double e = tau - j;  // past 1 on the last row: off the bottom and falling away
    const double xa = b.col[j] - 0.5 * j, ya = -0.75 * j;
    const double xb = b.col[j + 1] - 0.5 * (j + 1), yb = -0.75 * (j + 1);
    p.x = lerp(xa, xb, e);
    p.y = lerp(ya, yb, e) + 4 * hop * e * (1 - e);
    p.squash = e < 0.1 ? 1 - 0.3 * std::sin(kPi * e / 0.1) : 1.0;
    return p;
}

}  // namespace hopper

namespace {

using hopper::Game;
using hopper::Pose;
constexpr double kPi = 3.141592653589793;

// Maps cube units into tree units (the pyramid's place and size) and fills polygons there.
struct Board {
    OrganicCanvas& c;
    double w, ox, oy;
    void poly(const double* xs, const double* ys, int n, double pen) const {
        double tx[128], ty[128];
        n = std::min(n, 128);
        for (int i = 0; i < n; ++i) {
            tx[i] = ox + w * xs[i];
            ty[i] = oy + w * ys[i];
        }
        c.fillPolygon(tx, ty, n, pen);
    }
};

// A convex polygon moved in by g on every side (the gaps between faces).
void inset(double* xs, double* ys, int n, double g) {
    double area = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) area += xs[j] * ys[i] - xs[i] * ys[j];
    const double s = area >= 0 ? 1.0 : -1.0;
    double px[8], py[8], dx[8], dy[8];
    for (int i = 0; i < n; ++i) {
        const int k = (i + 1) % n;
        const double ex = xs[k] - xs[i], ey = ys[k] - ys[i], l = std::max(1e-12, std::hypot(ex, ey));
        dx[i] = ex / l;
        dy[i] = ey / l;
        px[i] = xs[i] - s * dy[i] * g;  // the inward normal of a counter-clockwise edge is (-dy, dx)
        py[i] = ys[i] + s * dx[i] * g;
    }
    for (int i = 0; i < n; ++i) {
        const int a = (i + n - 1) % n;  // edge a ends at vertex i, edge i starts there
        const double cr = dx[a] * dy[i] - dy[a] * dx[i];
        if (std::abs(cr) < 1e-12) continue;
        const double t = ((px[i] - px[a]) * dy[i] - (py[i] - py[a]) * dx[i]) / cr;
        xs[i] = px[a] + t * dx[a];
        ys[i] = py[a] + t * dy[a];
    }
}

// The creature's local frame: units of its body radius, origin between its feet, facing right (mirrored for left),
// squashed about the feet and spun about the body's center; then scaled into cube units at the pose.
struct Critter {
    const Board& b;
    double fx, fy, r, sx, sy, spin;
    int facing;
    void map(double lx, double ly, double& x, double& y) const {
        lx *= facing;
        double qx = lx * sx, qy = ly * sy;
        if (spin != 0) {
            const double cy = 1.05 * sy, cs = std::cos(spin), sn = std::sin(spin);
            const double dx = qx, dy = qy - cy;
            qx = cs * dx - sn * dy;
            qy = cy + sn * dx + cs * dy;
        }
        x = fx + r * qx;
        y = fy + r * qy;
    }
    void ellipse(double cx, double cy, double rx, double ry, double rot, double pen, int n = 20) const {
        double xs[32], ys[32];
        n = std::min(n, 32);
        const double cs = std::cos(rot), sn = std::sin(rot);
        for (int i = 0; i < n; ++i) {
            const double a = 2 * kPi * i / n, ex = rx * std::cos(a), ey = ry * std::sin(a);
            map(cx + cs * ex - sn * ey, cy + sn * ex + cs * ey, xs[i], ys[i]);
        }
        b.poly(xs, ys, n, pen);
    }
    void quad(const double* lx, const double* ly, double pen) const {
        double xs[4], ys[4];
        for (int i = 0; i < 4; ++i) map(lx[i], ly[i], xs[i], ys[i]);
        b.poly(xs, ys, 4, pen);
    }
};

class Hopper : public OrganicBody {
public:
    Hopper(scene::OrganicInstance& in, std::uint64_t seed)
        : inst_(in), game_(config(in, seed)), size_(keyNum(in, "SIZE", 0, 0, 2)) {}

    void step(const OrganicStep& s, OrganicCanvas& c) override {
        // The hop clock: the beat clock's beats, shifted by whole beats onto the phrase-aligned bars, so landings
        // are on the beat lines and cycles start on bar lines.
        const double beat = finiteOr(s.beat, 0), bar = finiteOr(s.bar, beat / 4);
        const double h = beat - std::round(std::clamp(beat - 4 * bar, -1e15, 1e15));
        const double drop = dropOf(s), build = buildOf(s);
        game_.advance(h, drop > 0.5 && lastDrop_ <= 0.5);
        lastDrop_ = drop;
        const double hc = game_.cycleBeat();
        const auto& plan = game_.plan();
        const int R = game_.rows(), k = game_.landing();

        const double pen = s.pen;
        auto fpen = [&](const char* key, double fb) { return std::clamp(frameNum(inst_, key, fb), 0.0, 1.0) * pen; };
        const double edge = fpen("EDGE", 0.04), sideL = fpen("SIDL", 0.22), sideR = fpen("SIDR", 0.4);
        const double topLo = fpen("TLO", 0.6), topHi = fpen("THI", 1.0);
        const double body = fpen("CPEN", 0.9), ballPen = fpen("BPEN", 0.72);
        const double hop = std::clamp(frameNum(inst_, "HOP", 0.55), 0.0, 4.0) * (1 + 1.6 * build);

        // Fit: R rows plus headroom over the apex for the hops, in 1.78 x 3.3 tree units at 16:9 (the width follows the
        // frame's aspect).
        const double head = 1.4;
        const double fitW = 3.3 * std::clamp(c.halfWidth(), 0.25, 8.0) / (16.0 / 9.0);
        const double w = size_ > 0 ? size_ : std::min(1.78 / (0.75 * R + head), fitW / R);
        const Board bd{c, w, 0.0, w * (0.75 * R - head) / 2};
        const double gap = std::max(0.016, 1.2 / std::max(1e-9, c.pixelsPerUnit() * w));

        // The pyramid, one path: the silhouette in the edge color, every face inset on it (max-combined, so the
        // gaps stay dark).
        {
            double xs[4 * kMaxSil], ys[4 * kMaxSil];
            int n = 0;
            for (int r = 0; r < R; ++r) {  // the right edge, down
                xs[n] = 0.5 * r + 0.5, ys[n++] = -0.75 * r;
                xs[n] = 0.5 * r + 0.5, ys[n++] = -0.75 * r - 0.5;
            }
            for (int col = R - 1; col >= 0; --col) {  // the bottom zigzag, right to left
                const double x = col - 0.5 * (R - 1), y = -0.75 * (R - 1);
                xs[n] = x, ys[n++] = y - 0.75;
                xs[n] = x - 0.5, ys[n++] = y - 0.5;
            }
            for (int r = R - 1; r >= 0; --r) {  // the left edge, up
                xs[n] = -0.5 * r - 0.5, ys[n++] = -0.75 * r - 0.5;
                xs[n] = -0.5 * r - 0.5, ys[n++] = -0.75 * r;
            }
            xs[n] = 0, ys[n++] = 0.25;
            bd.poly(xs, ys, n, edge);
        }
        // The cycle line: the finished board (all bright) flashes twice in the first half beat (the sides light up
        // on the 16ths), then the tops go dark again in a quick cascade down the rows, before the first landing.
        const bool flash = hc < 0.5 && (int(std::floor(hc * 8)) & 1) == 0;
        const int landed = plan.pos[std::size_t(k)];
        for (int i = 0; i < game_.cubes(); ++i) {
            const int r = hopper::rowOf(i), col = hopper::colOf(i);
            const double x = hopper::topX(i), y = hopper::topY(i);
            double top = lerp(topLo, topHi, double(game_.state(i)) / (game_.levels() - 1));
            double l = sideL, rr = sideR;
            if (build > 0)  // shimmer: a glint running over the tops, only ever brighter (a darker top looks unturned)
                top = lerp(top, pen, 0.45 * build * std::pow(0.5 + 0.5 * std::sin(2 * kPi * (hc + 0.17 * r - 0.31 * col)), 4));
            if (i == landed && hc - k < 1) top = lerp(top, pen, 0.6 * std::exp(-10 * (hc - k)));  // landing pop
            if (hc < 1) {
                const double down = 0.5 + 0.35 * r / std::max(1, R - 1);  // this row's tops go dark here
                top = lerp(topHi, top, smooth((hc - down) / 0.1));
                if (flash) {
                    l = lerp(l, pen, 0.6);
                    rr = lerp(rr, pen, 0.6);
                }
            }
            const double since = hc - plan.dropAt - 0.05 * r;  // the drop: a flash down the rows
            if (since >= 0 && since < 3) {
                const double f = std::exp(-2.5 * since);
                top = lerp(top, pen, f);
                l = lerp(l, 0.8 * pen, f);
                rr = lerp(rr, 0.8 * pen, f);
            }
            double tx[4] = {x, x + 0.5, x, x - 0.5}, ty[4] = {y + 0.25, y, y - 0.25, y};
            inset(tx, ty, 4, gap);
            bd.poly(tx, ty, 4, std::clamp(top, 0.0, 1.0));
            double lx[4] = {x - 0.5, x, x, x - 0.5}, ly[4] = {y, y - 0.25, y - 0.75, y - 0.5};
            inset(lx, ly, 4, gap);
            bd.poly(lx, ly, 4, l);
            double rx[4] = {x + 0.5, x, x, x + 0.5}, ry[4] = {y, y - 0.25, y - 0.75, y - 0.5};
            inset(rx, ry, 4, gap);
            bd.poly(rx, ry, 4, rr);
        }

        // Balls: a dark rim, the ball, a highlight; a burst is a ring of sparks.
        for (const auto& b : plan.balls) {
            const auto p = game_.ballPose(b, 0.75 * hop);
            if (p.burst >= 0) {
                if (p.burst >= 1) continue;
                c.newPath();
                const Critter cr{bd, p.x, p.y, 0.15, 1, 1, 0, 1};
                for (int i = 0; i < 8; ++i) {
                    const double a = 2 * kPi * i / 8 + 0.4, d = 0.8 + 3.5 * p.burst;
                    cr.ellipse(d * std::cos(a), 1 + d * std::sin(a), 0.45 * (1 - p.burst), 0.45 * (1 - p.burst), 0,
                               pen * (1 - 0.5 * p.burst), 10);
                }
                continue;
            }
            if (!p.visible) continue;
            const Critter cr{bd, p.x, p.y, 0.15, 1 / std::sqrt(p.squash), p.squash, 0, 1};
            c.newPath();
            cr.ellipse(0, 1, 1.25, 1.25, 0, edge, 22);
            c.newPath();
            cr.ellipse(0, 1, 1, 1, 0, ballPen, 22);
            c.newPath();
            cr.ellipse(-0.35, 1.35, 0.3, 0.22, 0.6, pen, 12);
        }

        drawCritter(bd, game_.pose(hop), edge, body, pen, hc);
    }

    std::string summary() const override {
        char b[128];
        std::snprintf(b, sizeof b, "%d rows (%d cubes), %d states, solved every %d bars, %g ball(s) per bar%s",
                      game_.rows(), game_.cubes(), game_.levels(), game_.cycleBars(), ballsPerBar_,
                      offbeat_ ? " on the off-beat" : "");
        return b;
    }

private:
    static constexpr int kMaxSil = 3 * hopper::kMaxRows + 2;

    hopper::Config config(scene::OrganicInstance& in, std::uint64_t seed) {
        hopper::Config c;
        c.rows = keyCount(in, "ROWS", 7, hopper::kMinRows, hopper::kMaxRows);
        c.cycBars = keyCount(in, "CYC", 8, 1, 64);
        c.levels = keyCount(in, "LEVELS", 2, 2, 3);
        c.ballsPerBar = ballsPerBar_ = keyNum(in, "BALL", 1, 0, 4);
        c.offbeat = offbeat_ = keyNum(in, "OFFB", 0, 0, 1) >= 0.5;
        c.seed = seed;
        return c;
    }

    static double lerp(double a, double b, double t) { return a + (b - a) * t; }
    static double smooth(double t) {
        t = std::clamp(t, 0.0, 1.0);
        return t * t * (3 - 2 * t);
    }

    // The creature: a round body on two feet with a two-leaf sprout, eyes towards where it hops (none from behind),
    // a dark rim so it reads over any top, and a shadow on the cubes.
    void drawCritter(const Board& bd, const Pose& p, double edge, double body, double pen, double hc) const {
        OrganicCanvas& c = bd.c;
        const double r = 0.23;  // body radius in cube units
        if (!p.onDisc) {
            c.newPath();
            const double k = 1 - 0.5 * std::min(1.0, p.air / 1.2);
            const Critter sh{bd, p.x, p.ground, r, 1, 1, 0, 1};
            sh.ellipse(0, 0, 1.3 * k, 0.55 * k, 0, edge, 18);
        } else if (p.discScale > 0) {
            // The disc: a flat spinning plate under the feet, studs going round its rim.
            const double s = p.discScale;
            const Critter dc{bd, p.discX, p.discY - 0.04, r, 1, 1, 0, 1};
            c.newPath();
            dc.ellipse(0, 0, 2.6 * s + 0.25, 0.8 * s + 0.2, 0, edge, 26);
            c.newPath();
            dc.ellipse(0, 0, 2.6 * s, 0.8 * s, 0, pen * 0.95, 26);
            c.newPath();
            for (int i = 0; i < 6; ++i) {
                const double a = p.discSpin + 2 * kPi * i / 6;
                dc.ellipse(2.0 * s * std::cos(a), 0.55 * s * std::sin(a), 0.28 * s, 0.14 * s, 0, edge, 10);
            }
        }
        const double sy = p.squash, sx = 1 / std::sqrt(std::max(0.3, sy));
        const Critter cr{bd, p.x, p.y, r, sx, sy, p.spin, p.facing};
        const double sway = 0.25 * std::sin(2 * kPi * hc);
        const double fe = p.back ? 0 : 0.18;  // feet step towards the facing side when seen from the front
        // Outlines (one path, dark), then the fills over them.
        c.newPath();
        cr.ellipse(-0.42 + fe, 0.14, 0.5, 0.33, 0, edge, 14);
        cr.ellipse(0.42 + fe, 0.14, 0.5, 0.33, 0, edge, 14);
        cr.ellipse(0, 1.05, 1.17, 1.13, 0, edge, 28);
        const double sxo[4] = {-0.02, 0.18, 0.36, 0.16}, syo[4] = {1.9, 1.9, 2.45, 2.45};
        cr.quad(sxo, syo, edge);
        cr.ellipse(-0.05, 2.55, 0.47, 0.27, 0.5 + sway, edge, 14);
        cr.ellipse(0.62, 2.5, 0.44, 0.25, -0.45 + sway, edge, 14);
        c.newPath();
        cr.ellipse(-0.42 + fe, 0.14, 0.36, 0.2, 0, 0.6 * body, 14);
        cr.ellipse(0.42 + fe, 0.14, 0.36, 0.2, 0, 0.6 * body, 14);
        cr.ellipse(0, 1.05, 1.0, 0.96, 0, body, 28);
        const double sxi[4] = {0.05, 0.12, 0.3, 0.22}, syi[4] = {1.9, 1.9, 2.45, 2.45};
        cr.quad(sxi, syi, 0.75 * body);
        cr.ellipse(-0.05, 2.55, 0.33, 0.14, 0.5 + sway, 0.8 * pen, 14);
        cr.ellipse(0.62, 2.5, 0.3, 0.12, -0.45 + sway, 0.8 * pen, 14);
        if (p.back) return;
        // Face (three-quarter view towards +x): the near eye bigger, pupils looking ahead, a small mouth.
        c.newPath();
        cr.ellipse(0.02, 1.25, 0.27, 0.35, 0, pen, 16);
        cr.ellipse(0.6, 1.22, 0.22, 0.31, 0, pen, 16);
        c.newPath();
        cr.ellipse(0.11, 1.19, 0.13, 0.17, 0, edge, 12);
        cr.ellipse(0.68, 1.17, 0.11, 0.15, 0, edge, 12);
        cr.ellipse(0.42, 0.66, 0.15, 0.07, 0, edge, 10);
    }

    scene::OrganicInstance& inst_;
    double ballsPerBar_ = 1;
    bool offbeat_ = false;
    Game game_;
    double size_;
    double lastDrop_ = 0;
};

}  // namespace

std::unique_ptr<OrganicBody> makeHopper(scene::OrganicInstance& inst, std::uint64_t seed) {
    return std::make_unique<Hopper>(inst, seed);
}

}  // namespace reforce::engine::arcade
