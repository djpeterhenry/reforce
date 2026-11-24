#include "engine/Labyrinth.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::engine::labyrinth {
namespace {

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

constexpr int kCandidates = 6;
constexpr double kStraight = 0.55;  // the backtracker keeps its direction this often when it can
constexpr double kNaturalTilt = 3.5 / kRollGain;  // a comfortable roll (3.5 cells / beat^2 through the board's lag)
constexpr double kPlanTilt = kMaxTilt * 0.65;     // the steepest a plan may ask for (margin for the search)
// The player (Player::simulate): its reaction time and centering across (rad / beat).
constexpr double kReaction = 0.04, kCenter = 2.5;
constexpr double kLevelTime = 0.2;  // a braking ball this close to stopping (beats): the hand levels the board
constexpr int kDx[4] = {1, 0, -1, 0};
constexpr int kDy[4] = {0, -1, 0, 1};  // rows: North = row - 1

double smooth01(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3 - 2 * x);
}

int opposite(int d) { return (d + 2) & 3; }

Maze carve(uint64_t seed, int n) {
    Maze m;
    m.n = n;
    const int cells = n * n;
    std::array<uint8_t, kMaxCells> seen{};
    std::array<int16_t, kMaxCells> stack{};
    std::array<int8_t, kMaxCells> came{};  // direction the backtracker entered the cell by (-1: the first)
    uint64_t s = seed;
    int top = 0;
    const int first = int(splitmix(s) % uint64_t(cells));
    stack[0] = int16_t(first);
    seen[std::size_t(first)] = 1;
    came[std::size_t(first)] = -1;
    top = 1;
    while (top > 0) {
        const int cur = stack[std::size_t(top - 1)];
        int opts[4], o = 0;
        for (int d = 0; d < 4; ++d) {
            const int nb = m.neighbor(cur, d);
            if (nb >= 0 && !seen[std::size_t(nb)]) opts[o++] = d;
        }
        if (o == 0) {
            --top;
            continue;
        }
        int d = opts[splitmix(s) % uint64_t(o)];
        const int straight = came[std::size_t(cur)];
        if (straight >= 0) {
            for (int i = 0; i < o; ++i)
                if (opts[i] == straight && double(splitmix(s) >> 11) * 0x1.0p-53 < kStraight) d = straight;
        }
        const int nb = m.neighbor(cur, d);
        m.open[std::size_t(cur)] |= uint8_t(1u << d);
        m.open[std::size_t(nb)] |= uint8_t(1u << opposite(d));
        seen[std::size_t(nb)] = 1;
        came[std::size_t(nb)] = int8_t(d);
        stack[std::size_t(top++)] = int16_t(nb);
    }
    return m;
}

// Breadth-first distances and parents over the maze's openings.
void bfs(const Maze& m, int from, std::array<int16_t, kMaxCells>& dist, std::array<int16_t, kMaxCells>& parent) {
    const int cells = m.n * m.n;
    for (int i = 0; i < cells; ++i) {
        dist[std::size_t(i)] = -1;
        parent[std::size_t(i)] = -1;
    }
    std::array<int16_t, kMaxCells> q{};
    int head = 0, tail = 0;
    q[std::size_t(tail++)] = int16_t(from);
    dist[std::size_t(from)] = 0;
    while (head < tail) {
        const int c = q[std::size_t(head++)];
        for (int d = 0; d < 4; ++d) {
            if (m.wall(c, d)) continue;
            const int nb = m.neighbor(c, d);
            if (nb < 0 || dist[std::size_t(nb)] >= 0) continue;
            dist[std::size_t(nb)] = int16_t(dist[std::size_t(c)] + 1);
            parent[std::size_t(nb)] = int16_t(c);
            q[std::size_t(tail++)] = int16_t(nb);
        }
    }
}

int dirBetween(const Maze& m, int a, int b) {
    for (int d = 0; d < 4; ++d)
        if (m.neighbor(a, d) == b) return d;
    return 0;
}

int openings(const Maze& m, int c) { return __builtin_popcount(unsigned(m.open[std::size_t(c)])); }

struct Durations {
    int min = 1, natural = 1;
};
}  // namespace


double brakeShare(Stop stop, double D, double d) {
    if (stop != Stop::Wall || !(D > 0) || !(d > 0)) return 0.0;
    // Idealized: +a for half the time, then -beta a: v_end / (D / d) = 4 (1 - beta) / (3 - beta). Keep v_end at
    // about kClackSpeed.
    const double q = kClackSpeed * d / (4 * D);
    return q >= 1.0 / 3.0 ? 0.0 : std::clamp((1 - 3 * q) / (1 - q), 0.0, 0.85);
}

double tiltShape(Stop stop, double D, double d, double tau) {
    d = std::max(d, 0.25);
    const double ramp = smooth01((tau + kLead) / (kLead + kRise));
    auto sm = [&](double from, double width) { return smooth01((tau - from * d) / (width * d)); };
    switch (stop) {
        case Stop::Wall: {
            const double beta = brakeShare(stop, D, d);
            // the lean back into the wall ends a quarter beat before the clack: the board takes that long to follow
            const double lean = smooth01((tau - (d - 0.45)) / 0.25);
            return ramp * (1 - (1 + beta) * sm(0.35, 0.2) + (beta + kLean) * lean);
        }
        case Stop::Brake:  // early enough that the ball is at rest well before the beat; then the hand holds it
            return ramp * (1 - 2 * sm(0.3, 0.2));
        case Stop::Goal: return ramp * (1 - sm(0.35, 0.55));
    }
    return 0.0;
}

double tiltGuess(Stop stop, double D, double d) {
    // The distance the ball covers for a unit tilt command through the board's response (no rate limit, no friction):
    // it scales with the tilt.
    const double h = 1.0 / 200;
    double x = 0, v = 0, tilt = 0, rate = 0;
    for (double tau = -kLead; tau < d + 2; tau += h) {
        if (stop == Stop::Brake && tau > 0.5 * d && v <= 0) break;
        if (stop != Stop::Brake && tau >= d) break;
        rate += (kBoardOmega * kBoardOmega * (tiltShape(stop, D, d, tau) - tilt) - 2 * kBoardDamping * kBoardOmega * rate) * h;
        tilt += rate * h;
        v += kRollGain * tilt * h;
        x += v * h;
    }
    return x > 1e-9 ? D / x : 1e9;
}

namespace {

Durations computeDurations(Stop stop, int cells) {
    const double D = stop == Stop::Wall ? cells + kFree : double(cells);
    Durations r;
    r.min = stop == Stop::Brake ? 2 : 1;  // speeding up and stopping in one beat is more than the board can follow
    while (r.min < 32 && tiltGuess(stop, D, r.min) > kPlanTilt) ++r.min;
    r.natural = r.min;
    while (r.natural < 32 && tiltGuess(stop, D, r.natural) > kNaturalTilt) ++r.natural;
    return r;
}

// A segment's durations depend only on its stop and length: a table made once (tiltGuess simulates; choosing a
// goal asks for thousands of these, ~60 ms per route before the table).
Durations durationsFor(Stop stop, int cells) {
    using Table = std::array<std::array<Durations, kMaxGrid + 1>, 3>;
    static const Table table = [] {
        Table t{};
        for (int s = 0; s < 3; ++s)
            for (int c = 1; c <= kMaxGrid; ++c) t[std::size_t(s)][std::size_t(c)] = computeDurations(Stop(s), c);
        return t;
    }();
    return table[std::size_t(stop)][std::size_t(std::clamp(cells, 1, kMaxGrid))];
}

// Cuts the path into straight segments (no timing yet). Returns the count.
int cutSegments(const Maze& m, const int16_t* path, int len, std::array<Segment, kMaxCells>& out) {
    int count = 0;
    int i = 0;
    while (i + 1 < len) {
        const int d = dirBetween(m, path[i], path[i + 1]);
        int j = i + 1;
        while (j + 1 < len && dirBetween(m, path[j], path[j + 1]) == d) ++j;
        Segment s;
        s.from = path[i];
        s.to = path[j];
        s.dir = d;
        s.cells = j - i;
        s.stop = j + 1 >= len ? Stop::Goal : m.wall(path[j], d) ? Stop::Wall : Stop::Brake;
        out[std::size_t(count++)] = s;
        i = j;
    }
    return count;
}

struct GoalScore {
    int fill = -1, deadEnd = 0, length = 0;
    bool better(const GoalScore& o) const {
        if (fill != o.fill) return fill > o.fill;
        if (deadEnd != o.deadEnd) return deadEnd > o.deadEnd;
        return length > o.length;
    }
};

}  // namespace

int Maze::neighbor(int cell, int d) const {
    const int c = cell % n + kDx[d], r = cell / n + kDy[d];
    if (c < 0 || r < 0 || c >= n || r >= n) return -1;
    return r * n + c;
}

bool Maze::isPerfect() const {
    if (n < 1) return false;
    const int cells = n * n;
    int edges = 0;
    for (int c = 0; c < cells; ++c)
        for (int d = 0; d < 4; ++d) {
            if (wall(c, d)) continue;
            const int nb = neighbor(c, d);
            if (nb < 0 || wall(nb, opposite(d))) return false;  // open off the board, or one-sided
            ++edges;
        }
    if (edges != 2 * (cells - 1)) return false;
    std::array<int16_t, kMaxCells> dist{}, parent{};
    bfs(*this, 0, dist, parent);
    for (int c = 0; c < cells; ++c)
        if (dist[std::size_t(c)] < 0) return false;
    return true;
}

Route makeRoute(uint64_t seed, int grid, int beats, int traps) {
    grid = std::clamp(grid, kMinGrid, kMaxGrid);
    beats = std::clamp(beats, kMinBeats, kMaxBeats);
    traps = std::clamp(traps, 0, kMaxTraps);
    Route best;
    best.beats = beats;
    best.interlude = beats >= 16 ? 2 : 1;
    const int avail = beats - best.interlude;
    GoalScore bestScore;
    uint64_t s = seed * 0xD1342543DE82EF95ull + 0x4C4142ull;
    std::array<int16_t, kMaxCells> dist{}, parent{}, path{};
    std::array<Segment, kMaxCells> segs{};
    const int cells = grid * grid;
    for (int k = 0; k < kCandidates; ++k) {
        const Maze m = carve(splitmix(s), grid);
        // Start: one end of a longest path (the farthest cell from any cell is such an end, a dead end).
        bfs(m, int(splitmix(s) % uint64_t(cells)), dist, parent);
        int start = 0;
        for (int c = 0; c < cells; ++c)
            if (dist[std::size_t(c)] > dist[std::size_t(start)]) start = c;
        bfs(m, start, dist, parent);
        for (int g = 0; g < cells; ++g) {
            if (g == start) continue;
            int len = 0;
            for (int c = g; c >= 0; c = parent[std::size_t(c)]) path[std::size_t(len++)] = int16_t(c);
            std::reverse(path.begin(), path.begin() + len);
            const int n = cutSegments(m, path.data(), len, segs);
            int sumMin = 0, sumNat = 0;
            for (int i = 0; i < n; ++i) {
                const Durations d = durationsFor(segs[std::size_t(i)].stop, segs[std::size_t(i)].cells);
                sumMin += d.min;
                sumNat += d.natural;
            }
            if (sumMin > avail) continue;
            GoalScore sc{std::min(sumNat, avail), openings(m, g) == 1 ? 1 : 0, len};
            if (!sc.better(bestScore)) continue;
            bestScore = sc;
            best.maze = m;
            best.maze.start = start;
            best.maze.goal = g;
            best.maze.pathLength = len;
            std::copy(path.begin(), path.begin() + len, best.maze.path.begin());
        }
    }
    Maze& m = best.maze;
    // Trap holes: dead ends off the route, not next to it, the start or the goal (decoration only).
    {
        std::array<uint8_t, kMaxCells> near{};
        for (int i = 0; i < m.pathLength; ++i) near[std::size_t(m.path[std::size_t(i)])] = 1;
        for (int c : {m.start, m.goal})
            for (int d = 0; d < 4; ++d)
                if (const int nb = m.neighbor(c, d); nb >= 0) near[std::size_t(nb)] = 1;
        std::array<int16_t, kMaxCells> cand{};
        int nc = 0;
        for (int c = 0; c < cells; ++c)
            if (!near[std::size_t(c)] && openings(m, c) == 1) cand[std::size_t(nc++)] = int16_t(c);
        m.trapCount = 0;
        while (m.trapCount < traps && nc > 0) {
            const int i = int(splitmix(s) % uint64_t(nc));
            const int c = cand[std::size_t(i)];
            cand[std::size_t(i)] = cand[std::size_t(--nc)];
            bool clear = true;  // not next to another trap
            for (int t = 0; t < m.trapCount; ++t) {
                const int o = m.traps[std::size_t(t)];
                if (std::abs(o % grid - c % grid) + std::abs(o / grid - c / grid) <= 1) clear = false;
            }
            if (clear) m.traps[std::size_t(m.trapCount++)] = int16_t(c);
        }
    }
    // Timing.
    best.count = cutSegments(m, m.path.data(), m.pathLength, best.segments);
    const int S = best.count;
    std::array<int, kMaxCells> d{}, nat{};
    int left = avail;
    for (int i = 0; i < S; ++i) {
        const Durations du = durationsFor(best.segments[std::size_t(i)].stop, best.segments[std::size_t(i)].cells);
        d[std::size_t(i)] = du.min;
        nat[std::size_t(i)] = du.natural;
        left -= du.min;
    }
    while (left > 0) {
        int pick = -1;
        for (int i = 0; i < S; ++i)
            if (nat[std::size_t(i)] > d[std::size_t(i)] &&
                (pick < 0 || nat[std::size_t(i)] - d[std::size_t(i)] > nat[std::size_t(pick)] - d[std::size_t(pick)]))
                pick = i;
        if (pick < 0) break;
        ++d[std::size_t(pick)];
        --left;
    }
    int at = best.interlude + (S <= 1 ? left : 0);
    const int gaps = S - 1;
    for (int i = 0; i < S; ++i) {
        Segment& sg = best.segments[std::size_t(i)];
        sg.start = at;
        sg.end = at + d[std::size_t(i)];
        const int pause = i < gaps ? (i + 1) * left / gaps - i * left / gaps : 0;
        sg.next = sg.end + pause;
        at = sg.next;
    }
    return best;
}

// ---------------------------------------------------------------- physics

namespace {

struct Contact {
    double clearance = 1e9;  // distance from the ball's surface to the nearest wall (< 0: overlapping)
    double nx = 0, ny = 0;   // away from that wall
};

int cellAt(const Maze& m, double x, double y) {
    const int c = std::clamp(int(std::floor(x + 0.5 * m.n)), 0, m.n - 1);
    const int r = std::clamp(int(std::floor(0.5 * m.n - y)), 0, m.n - 1);
    return r * m.n + c;
}

// The walls near (x, y): the walled sides of the 3 x 3 cells around it, as boxes (the outer border included).
Contact contactAt(const Maze& m, double x, double y) {
    Contact best;
    const int c0 = std::clamp(int(std::floor(x + 0.5 * m.n)), 0, m.n - 1);
    const int r0 = std::clamp(int(std::floor(0.5 * m.n - y)), 0, m.n - 1);
    for (int r = std::max(0, r0 - 1); r <= std::min(m.n - 1, r0 + 1); ++r)
        for (int c = std::max(0, c0 - 1); c <= std::min(m.n - 1, c0 + 1); ++c) {
            const int cell = r * m.n + c;
            const double cx = m.cellX(cell), cy = m.cellY(cell);
            for (int d = 0; d < 4; ++d) {
                if (!m.wall(cell, d)) continue;
                const bool vertical = d == East || d == West;
                const double bx = cx + (d == East ? 0.5 : d == West ? -0.5 : 0.0);
                const double by = cy + (d == North ? 0.5 : d == South ? -0.5 : 0.0);
                const double hx = vertical ? kWall / 2 : 0.5 + kWall / 2;
                const double hy = vertical ? 0.5 + kWall / 2 : kWall / 2;
                const double dx = x - bx, dy = y - by;
                const double qx = std::fabs(dx) - hx, qy = std::fabs(dy) - hy;
                if (qx - kBall >= best.clearance || qy - kBall >= best.clearance) continue;  // farther than the best
                double dist, nx, ny;
                if (qx > 0 || qy > 0) {
                    const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
                    dist = std::sqrt(ox * ox + oy * oy);
                    nx = dist > 0 ? ox / dist * (dx < 0 ? -1 : 1) : 0;
                    ny = dist > 0 ? oy / dist * (dy < 0 ? -1 : 1) : 0;
                } else {
                    dist = std::max(qx, qy);
                    nx = qx > qy ? (dx < 0 ? -1 : 1) : 0;
                    ny = qx > qy ? 0 : (dy < 0 ? -1 : 1);
                }
                const double cl = dist - kBall;
                if (cl < best.clearance) best = {cl, nx, ny};
            }
        }
    return best;
}

// Inside its cell's free square the ball cannot touch any wall.
bool clearOfWalls(const Maze& m, double x, double y) {
    const int cell = cellAt(m, x, y);
    return std::fabs(x - m.cellX(cell)) < kFree - 1e-9 && std::fabs(y - m.cellY(cell)) < kFree - 1e-9;
}

double clearance(const Maze& m, double x, double y) { return clearOfWalls(m, x, y) ? 1.0 : contactAt(m, x, y).clearance; }

void pushOut(const Maze& m, Player::Ball& b, double* penetration) {
    for (int i = 0; i < 4; ++i) {
        if (clearOfWalls(m, b.x, b.y)) return;
        const Contact c = contactAt(m, b.x, b.y);
        if (c.clearance >= 0) return;
        if (penetration) *penetration = std::max(*penetration, -c.clearance);
        b.x += c.nx * (-c.clearance + 1e-9);
        b.y += c.ny * (-c.clearance + 1e-9);
        const double vn = b.vx * c.nx + b.vy * c.ny;
        if (vn < 0) {
            b.vx -= vn * c.nx;
            b.vy -= vn * c.ny;
        }
    }
}

}  // namespace

void Player::step(const Maze& m, Ball& b, double cx, double cy, double dt, double t, Hit* hit, double* penetration) {
    auto sane = [](double v, double lim) { return std::isfinite(v) ? std::clamp(v, -lim, lim) : 0.0; };
    cx = sane(cx, kMaxTilt);
    cy = sane(cy, kMaxTilt);
    if (!(dt > 0) || !std::isfinite(dt)) return;
    dt = std::min(dt, 0.05);
    const double half = 0.5 * m.n - kWall / 2 - kBall;
    if (!std::isfinite(b.x) || !std::isfinite(b.y)) b.x = b.y = 0;
    b.x = std::clamp(b.x, -half, half);
    b.y = std::clamp(b.y, -half, half);
    b.vx = sane(b.vx, kMaxSpeed);
    b.vy = sane(b.vy, kMaxSpeed);
    b.tx = sane(b.tx, kMaxTilt);
    b.ty = sane(b.ty, kMaxTilt);
    if (!std::isfinite(b.z) || !std::isfinite(b.vz)) b.z = b.vz = 0;
    // The board: a wrist's second-order response to the command, its tilt rate limited.
    b.rx = sane(b.rx, kTiltRate);
    b.ry = sane(b.ry, kTiltRate);
    auto wrist = [&](double& tilt, double& rate, double cmd) {
        const double acc = kBoardOmega * kBoardOmega * (cmd - tilt) - 2 * kBoardDamping * kBoardOmega * rate;
        rate = std::clamp(rate + acc * dt, -kTiltRate, kTiltRate);
        const double next = tilt + rate * dt;
        tilt = std::clamp(next, -kMaxTilt, kMaxTilt);
        if (tilt != next) rate = 0;
    };
    wrist(b.tx, b.rx, cx);
    wrist(b.ty, b.ry, cy);

    double ax = kRollGain * b.tx, ay = kRollGain * b.ty;
    // The goal hole: over it the ball loses the floor, rests on the rim (pushed inwards) and drops in.
    const double hx = m.cellX(m.goal), hy = m.cellY(m.goal);
    double dx = b.x - hx, dy = b.y - hy;
    double rho = std::sqrt(dx * dx + dy * dy);
    if (!b.falling && rho < kHole) b.falling = true;
    if (b.falling && rho >= kHole && b.z > -kBall) {  // rolled across the hole without dropping: back on the floor
        b.falling = false;
        b.z = b.vz = 0;
    }
    bool inHole = false;
    if (b.falling) {
        b.vz -= kGravity * dt;
        b.z += b.vz * dt;
        const double s = (kHole - rho) / kBall;  // how far over the rim (the rim's horizontal offset / radius)
        if (b.z > -kBall && s < 1 && rho > 1e-9) {
            const double rest = kBall * std::sqrt(std::max(0.0, 1 - s * s)) - kBall;
            if (b.z <= rest) {  // resting on the rim: its push is inwards
                b.z = rest;
                b.vz = std::max(b.vz, 0.0);
                const double in = kGravity * s * std::sqrt(std::max(0.0, 1 - s * s));
                ax -= in * dx / rho;
                ay -= in * dy / rho;
            }
        }
        if (b.z <= -kBall) {  // in the hole: the walls of the cup hold it
            inHole = true;
            ax = ay = 0;
            if (b.z < -2.5 * kBall) {
                b.z = -2.5 * kBall;
                b.vz = 0;
            }
        }
    }
    b.vx += ax * dt;
    b.vy += ay * dt;
    double speed = std::sqrt(b.vx * b.vx + b.vy * b.vy);
    if (!b.falling && speed > 0) {
        const double k = std::max(0.0, speed - kRolling * kGravity * dt) / speed;
        b.vx *= k;
        b.vy *= k;
        speed *= k;
    }
    if (speed > kMaxSpeed) {
        b.vx *= kMaxSpeed / speed;
        b.vy *= kMaxSpeed / speed;
        speed = kMaxSpeed;
    }
    if (inHole) {
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        dx = b.x - hx;
        dy = b.y - hy;
        rho = std::sqrt(dx * dx + dy * dy);
        const double lim = kHole - kBall;
        if (rho > lim && rho > 1e-12) {
            b.x = hx + dx / rho * lim;
            b.y = hy + dy / rho * lim;
            const double vn = (b.vx * dx + b.vy * dy) / rho;
            if (vn > 0) {
                b.vx -= (1 + kRestitution) * vn * dx / rho;
                b.vy -= (1 + kRestitution) * vn * dy / rho;
            }
        }
        b.vx *= std::exp(-6 * dt);
        b.vy *= std::exp(-6 * dt);
        return;
    }

    // Move with collisions: sub-steps of at most kSubStep cells, the time of impact bisected.
    const int subs = std::clamp(int(std::ceil(speed * dt / kSubStep)), 1, 64);
    const double h = dt / subs;
    bool hitDone = false;
    for (int i = 0; i < subs; ++i) {
        double remaining = 1.0;
        for (int bounce = 0; bounce < 4 && remaining > 1e-12; ++bounce) {
            const double mx = b.vx * h * remaining, my = b.vy * h * remaining;
            if (clearance(m, b.x + mx, b.y + my) >= 0) {
                b.x += mx;
                b.y += my;
                remaining = 0;
                break;
            }
            if (clearance(m, b.x, b.y) < 0) {  // overlapping (numerically): resolve, then go on
                pushOut(m, b, penetration);
                continue;
            }
            if (const Contact rest = contactAt(m, b.x, b.y); rest.clearance < 1e-7) {
                // Resting against a wall and pressed into it: no bounce, slide along it.
                const double vn = b.vx * rest.nx + b.vy * rest.ny;
                if (vn < 0 && -vn < kRestingSpeed) {
                    b.vx -= vn * rest.nx;
                    b.vy -= vn * rest.ny;
                    continue;
                }
            }
            double lo = 0, hi = 1;
            for (int it = 0; it < 24; ++it) {
                const double mid = 0.5 * (lo + hi);
                if (clearance(m, b.x + mx * mid, b.y + my * mid) >= 0) lo = mid;
                else hi = mid;
            }
            b.x += mx * lo;
            b.y += my * lo;
            const Contact c = contactAt(m, b.x + mx * (hi - lo), b.y + my * (hi - lo));
            const double vn = b.vx * c.nx + b.vy * c.ny;
            if (vn < 0) {
                const double e = -vn < kRestingSpeed ? 0.0 : kRestitution;
                b.vx -= (1 + e) * vn * c.nx;
                b.vy -= (1 + e) * vn * c.ny;
                // Friction along the wall: at most kWallFriction x the normal impulse (Coulomb), so a glancing hit
                // keeps sliding and a head-on one loses little.
                const double tx = -c.ny, ty = c.nx;
                const double vt = b.vx * tx + b.vy * ty;
                const double jt = std::min(std::fabs(vt), kWallFriction * (1 + e) * -vn);
                b.vx -= (vt > 0 ? jt : -jt) * tx;
                b.vy -= (vt > 0 ? jt : -jt) * ty;
                if (hit && !hitDone && -vn >= kRestingSpeed) {
                    hit->time = t + h * (i + (1 - remaining) + lo * remaining);
                    hit->speed = float(-vn);
                    hit->nx = float(c.nx);
                    hit->ny = float(c.ny);
                    hitDone = true;
                }
            }
            remaining *= 1 - lo;
        }
        pushOut(m, b, penetration);
    }
}

// ---------------------------------------------------------------- player

Player::Player(int maxBeats) : maxBeats_(std::clamp(maxBeats, kMinBeats, kMaxBeats)) {
    samples_.assign(std::size_t(maxBeats_ * kStepsPerBeat + 2) * 5, 0.0f);
}

void Player::reset(uint64_t seed, int grid, int beats, int traps) {
    seed_ = seed;
    grid_ = grid;
    beats_ = beats;
    traps_ = traps;
    have_ = true;
    route_ = makeRoute(seed, grid, beats, traps);
    planned_ = 0;
    state_ = Ball{};
    state_.x = route_.maze.cellX(route_.maze.start);
    state_.y = route_.maze.cellY(route_.maze.start);
    hitCount_ = 0;
    maxPen_ = 0;
    illegal_ = 0;
    maxWander_ = wanderSum_ = maxTiltRate_ = 0;
    wanderN_ = offRoute_ = 0;
    onRoute_.fill(0);
    for (int i = 0; i < route_.maze.pathLength; ++i) onRoute_[std::size_t(route_.maze.path[std::size_t(i)])] = 1;
    // Sample 0: the ball at rest on the start when the first segment begins.
    samples_[0] = float(state_.x);
    samples_[1] = float(state_.y);
    samples_[2] = samples_[3] = samples_[4] = 0.0f;
}

double Player::simulate(int k, double param, bool record) {
    Segment& sg = route_.segments[std::size_t(k)];
    const Maze& m = route_.maze;
    Ball b = state_;
    const int axis = sg.dir == East || sg.dir == West ? 0 : 1;
    const double sign = sg.dir == East || sg.dir == North ? 1.0 : -1.0;
    auto along = [&](const Ball& q) { return sign * (axis == 0 ? q.x : q.y); };
    auto alongV = [&](const Ball& q) { return sign * (axis == 0 ? q.vx : q.vy); };
    auto across = [&](const Ball& q) { return axis == 0 ? q.y : q.x; };
    auto acrossV = [&](const Ball& q) { return axis == 0 ? q.vy : q.vx; };
    const double toCoord = axis == 0 ? m.cellX(sg.to) : m.cellY(sg.to);
    const double uEnd = sign * toCoord + (sg.stop == Stop::Wall ? kFree : 0.0);
    const double D = uEnd - along(b);
    const double d = double(sg.end - sg.start);
    const double theta = std::clamp(param, -kMaxTilt, kMaxTilt);  // the tilt the player commits to (searched)
    const double pc = axis == 0 ? m.cellY(sg.from) : m.cellX(sg.from);
    double pressSign = 0;  // after a clack: towards the wall the ball hit (on this segment's across axis)
    if (k > 0 && route_.segments[std::size_t(k - 1)].stop == Stop::Wall) {
        const int pd = route_.segments[std::size_t(k - 1)].dir;
        pressSign = pd == East || pd == North ? 1.0 : -1.0;
    }
    const double hx = m.cellX(m.goal), hy = m.cellY(m.goal);
    const int s0 = route_.segments[0].start;
    // Recording runs to the next segment's start (its samples and the state there), and on until the event if
    // that comes later; searching stops at the event (a brake: at the ball's first rest).
    const double tEnd = record ? std::max(double(sg.next), double(sg.end) + 1.5) : double(sg.end) + 1.5;
    const int steps = int(std::lround((tEnd - sg.start) * kStepsPerBeat));
    const int keep = (sg.next - sg.start) * kStepsPerBeat;  // recorded steps
    const double dt = 1.0 / kStepsPerBeat;
    double event = -1, restErr = 0;
    bool leveling = false;  // brake: the ball is nearly stopped, the hand levels the board

    // The player's hand for this segment (seeded: a pure function of the run and the segment): how much it cares
    // about the ball's side drift, and a slow tremor.
    uint64_t hs = seed_ * 0x9E3779B97F4A7C15ull + uint64_t(k + 1) * 0xD1B54A32D192ED03ull + uint64_t(route_.maze.n);
    auto rnd = [&]() { return double(splitmix(hs) >> 11) * 0x1.0p-53; };
    const double crossGain = 0.75 + 0.5 * rnd();
    const double f1 = 0.5 + 0.6 * rnd(), f2 = 1.2 + 1.0 * rnd(), p1 = 6.2832 * rnd(), p2 = 6.2832 * rnd();
    const double tremor = 0.004 + 0.005 * rnd();
    // Ahead of the next segment's beat the hand already starts tilting that way (the board needs time to follow):
    // its first guess of that tilt, on this segment's across axis (the next segment runs across this one).
    const bool hasNext = k + 1 < route_.count;
    Stop nStop = Stop::Wall;
    double nSign = 0, nD = 1, nd = 1, nTheta = 0;
    if (hasNext) {
        const Segment& nx = route_.segments[std::size_t(k + 1)];
        nStop = nx.stop;
        nSign = nx.dir == East || nx.dir == North ? 1.0 : -1.0;
        nD = nx.cells + (nx.stop == Stop::Wall ? kFree : 0.0);
        nd = double(std::max(1, nx.end - nx.start));
        nTheta = std::min(tiltGuess(nStop, nD, nd), kMaxTilt);
    }
    // What the player sees, kReaction late: along x, v; across x, v.
    std::array<std::array<double, 4>, 64> seen{};
    const int lag = std::clamp(int(std::lround(kReaction * kStepsPerBeat)), 0, 63);
    for (int i = 0; i < steps; ++i) {
        const double t = sg.start + double(i) / kStepsPerBeat;
        const double tau = t - sg.start;
        seen[std::size_t(i & 63)] = {along(b), alongV(b), across(b), acrossV(b)};
        const auto& o = seen[std::size_t((i >= lag ? i - lag : 0) & 63)];
        double ca, cp;
        if (sg.stop == Stop::Wall && event >= 0) {
            // After the clack: still leaning into the wall, easing off (the ball rebounds and ticks back).
            const double lean = std::max(0.05, kLean * theta);
            ca = lean * (0.55 + 0.45 * std::exp(-(t - event) / 0.6));
        } else if (sg.stop == Stop::Goal && b.falling) {
            ca = 0;  // let go
        } else if (sg.stop == Stop::Brake && event >= 0) {
            ca = (-2 * kCenter * o[1] - kCenter * kCenter * (o[0] - uEnd)) / kRollGain;  // at rest: holding it there
            ca = std::clamp(ca, -0.04, 0.04);
        } else if (sg.stop == Stop::Brake &&
                   (leveling || (tau > 0.5 * d && o[1] > 0 && o[1] < kLevelTime * kRollGain * theta))) {
            // Braking: once the ball would stop within the board's response time, the hand levels the board, so
            // the ball comes to rest on a level board (and stays).
            leveling = true;
            ca = 0;
        } else {
            ca = theta * tiltShape(sg.stop, D, d, tau);  // the planned tilt: the wrist's open-loop move
        }
        // Across: a loose hand. It leaves small drifts alone, corrects bigger ones late and imperfectly; just after
        // a clack it first keeps the ball against that wall.
        // Lining up for the goal the hand is careful: no dead band, firmer, so the ball rolls over the hole's middle.
        const bool aim = sg.stop == Stop::Goal;
        const double hold = pressSign != 0 && !aim ? 1.0 - smooth01(tau / 0.5) : 0.0;
        const double off = o[2] - (pc + pressSign * kFree * hold);
        const double band = aim ? 0.0 : 0.035;
        const double dead = off > 0 ? std::max(0.0, off - band) : std::min(0.0, off + band);
        const double g = aim ? 1.0 : crossGain;
        cp = (pressSign * 1.2 * hold - g * (2 * kCenter * o[3] + kCenter * kCenter * dead)) / kRollGain;
        cp += tremor * (0.6 * std::sin(6.2832 * f1 * t + p1) + 0.4 * std::sin(6.2832 * f2 * t + p2));
        const double lim = aim ? 0.1 : 0.07;
        cp = std::clamp(cp, -lim - 0.05 * hold, lim + 0.05 * hold);
        if (hasNext && t > sg.next - kLead) {  // tilting towards the next segment, ahead of its beat
            const double w8 = smooth01((t - (sg.next - kLead)) / kLead);
            cp = cp * (1 - w8) + nSign * nTheta * tiltShape(nStop, nD, nd, t - sg.next);
        }
        if (sg.stop == Stop::Goal && b.falling) cp = 0;
        ca = std::clamp(ca, -kMaxTilt, kMaxTilt);
        const double cx = axis == 0 ? sign * ca : cp;
        const double cy = axis == 0 ? cp : sign * ca;
        const Ball prev = b;
        Hit h;
        h.time = -1;
        double pen = 0;
        step(m, b, cx, cy, dt, t, &h, &pen);
        if (record && i < keep) {
            maxPen_ = std::max(maxPen_, pen);
            const int a = cellAt(m, prev.x, prev.y), c = cellAt(m, b.x, b.y);
            if (a != c) {
                bool ok = false;
                for (int d = 0; d < 4; ++d) ok = ok || (m.neighbor(a, d) == c && !m.wall(a, d));
                if (!ok) ++illegal_;
            }
            if (h.time >= 0 && hitCount_ < kMaxHits) hits_[std::size_t(hitCount_++)] = h;
            if (!onRoute_[std::size_t(c)]) ++offRoute_;
            maxTiltRate_ = std::max(maxTiltRate_, std::max(std::fabs(b.tx - prev.tx), std::fabs(b.ty - prev.ty)) / dt);
            if (tau > 0.3 && event < 0 && !b.falling && cellAt(m, b.x, b.y) != sg.to) {  // rolling down a corridor
                const double wd = std::fabs(across(b) - pc);
                maxWander_ = std::max(maxWander_, wd);
                wanderSum_ += wd;
                ++wanderN_;
            }
            const std::size_t j = std::size_t((sg.start - s0) * kStepsPerBeat + i + 1) * 5;
            if (j + 4 < samples_.size()) {
                samples_[j] = float(b.x);
                samples_[j + 1] = float(b.y);
                samples_[j + 2] = float(b.z);
                samples_[j + 3] = float(b.tx);
                samples_[j + 4] = float(b.ty);
            }
            if (sg.stop == Stop::Brake && i + 1 == (sg.end - sg.start) * kStepsPerBeat) {
                const double ex = b.x - m.cellX(sg.to), ey = b.y - m.cellY(sg.to);
                sg.endError = std::sqrt(ex * ex + ey * ey);
            }
        }
        if (event < 0) {
            if (sg.stop == Stop::Wall && h.time >= 0 && h.nx * kDx[sg.dir] - h.ny * kDy[sg.dir] < -0.7 &&
                cellAt(m, b.x, b.y) == sg.to)
                event = h.time;
            else if (sg.stop == Stop::Brake && tau > 0.3 * d && (alongV(b) <= 0 || (leveling && alongV(b) < 0.02))) {
                event = t + dt;  // first at rest
                restErr = along(b) - uEnd;
            } else if (sg.stop == Stop::Goal) {
                // The drop: the ball's center passes half its radius inside the rim (sinking, past the point of no
                // return; a distance, so the search sees it move smoothly).
                const double r0 = std::hypot(prev.x - hx, prev.y - hy), r1 = std::hypot(b.x - hx, b.y - hy);
                const double rd = kHole - 0.5 * kBall;
                if (r0 > rd && r1 <= rd) event = t + dt * (r0 - rd) / (r0 - r1);
            }
        }
        if (record && i + 1 == keep) state_ = b;
        if (event >= 0 && (!record || i + 1 >= keep)) break;
    }
    if (record) {
        sg.event = sg.stop == Stop::Brake ? double(sg.end) : event;
        sg.param = param;
    }
    if (sg.stop == Stop::Brake) return event < 0 ? along(b) - uEnd + 1.0 : restErr;  // where it came to rest
    return event < 0 ? tEnd + 1.0 : event;
}

void Player::planSegment(int k) {
    // Search the tilt the player commits to so that the segment's event lands: a clack or the drop on its beat line
    // (f = event time - beat), a braked ball at rest on its cell (f = where it rests - the cell's center). Secant
    // steps, falling back to bisection once the root is bracketed.
    Segment& sg = route_.segments[std::size_t(k)];
    const Maze& m = route_.maze;
    const int axis = sg.dir == East || sg.dir == West ? 0 : 1;
    const double sign = sg.dir == East || sg.dir == North ? 1.0 : -1.0;
    const double u = sign * (axis == 0 ? state_.x : state_.y);
    const double D = sign * (axis == 0 ? m.cellX(sg.to) : m.cellY(sg.to)) + (sg.stop == Stop::Wall ? kFree : 0.0) - u;
    const double d = double(sg.end - sg.start);
    const bool brake = sg.stop == Stop::Brake;
    const double tol = brake ? 2e-3 : kTimingTolerance * 0.25;
    auto f = [&](double th) { return simulate(k, th, false) - (brake ? 0.0 : double(sg.end)); };
    double x0 = std::clamp(tiltGuess(sg.stop, std::max(D, 0.05), d), 0.002, kMaxTilt), f0 = f(x0);
    double x1 = std::clamp(x0 * 1.1, 0.002, kMaxTilt), f1 = f(x1);
    double best = std::fabs(f0) <= std::fabs(f1) ? x0 : x1, bestErr = std::min(std::fabs(f0), std::fabs(f1));
    double lo = 0, hi = 0, flo = 0;
    bool bracket = false;
    auto noteBracket = [&](double a, double fa, double b2, double fb) {
        if ((fa < 0) != (fb < 0)) {
            lo = a;
            hi = b2;
            flo = fa;
            bracket = true;
        }
    };
    noteBracket(x0, f0, x1, f1);
    for (int it = 0; it < 24 && bestErr > tol; ++it) {
        double x2 = f1 != f0 ? x1 - f1 * (x1 - x0) / (f1 - f0) : x1 * 1.2;
        if (bracket && !(x2 > std::min(lo, hi) && x2 < std::max(lo, hi))) x2 = 0.5 * (lo + hi);
        if (!std::isfinite(x2)) break;
        x2 = std::clamp(x2, 0.002, kMaxTilt);
        const double f2 = f(x2);
        if (std::fabs(f2) < bestErr) {
            bestErr = std::fabs(f2);
            best = x2;
        }
        if (bracket) {
            if ((f2 < 0) == (flo < 0)) {
                lo = x2;
                flo = f2;
            } else {
                hi = x2;
            }
        } else {
            noteBracket(x1, f1, x2, f2);
        }
        x0 = x1;
        f0 = f1;
        x1 = x2;
        f1 = f2;
    }
    simulate(k, best, true);
    ++planned_;
}

void Player::planAll() {
    while (have_ && planned_ < route_.count) planSegment(planned_);
}

void Player::interludeFrame(double turn) {
    Frame& f = frame_;
    const Maze& m = route_.maze;
    f.rise = float(smooth01((turn - 0.5) / 0.5));
    f.sink = float(1.0 - smooth01(turn / 0.5));
    f.holes = float(smooth01((turn - 0.5) / 0.5));
    f.oldHoles = float(1.0 - smooth01((turn - 0.25) / 0.5));
    f.tiltX = f.tiltY = 0;
    f.segment = -1;
    if (turn < 1.0) {
        // The last run's ball, still dropping into its goal.
        f.ballX = float(f.previous.n > 0 ? f.previous.cellX(f.previous.goal) : m.cellX(m.start));
        f.ballY = float(f.previous.n > 0 ? f.previous.cellY(f.previous.goal) : m.cellY(m.start));
        f.ballZ = float(std::max(-3.0 * kBall, -kBall - 1.5 * turn - 0.5 * kGravity * turn * turn));
        return;
    }
    // Popping up out of the start: thrown up from below the floor, landing, one small bounce.
    f.ballX = float(m.cellX(m.start));
    f.ballY = float(m.cellY(m.start));
    const double tau = turn - 1.0;
    const double z0 = -2.2 * kBall, v0 = std::sqrt(2 * kGravity * (0.3 - z0));
    const double land = (v0 + std::sqrt(v0 * v0 + 2 * kGravity * z0)) / kGravity;
    double z = 0;
    if (tau < land) {
        z = z0 + v0 * tau - 0.5 * kGravity * tau * tau;
    } else {
        const double v1 = kRestitution * (kGravity * land - v0);  // up again, slower
        const double t1 = tau - land;
        if (t1 < 2 * v1 / kGravity) z = v1 * t1 - 0.5 * kGravity * t1 * t1;
    }
    f.ballZ = float(z);
}

const Frame& Player::at(uint64_t seed, int grid, int beats, double turn, int traps) {
    grid = std::clamp(grid, kMinGrid, kMaxGrid);
    beats = std::clamp(beats, kMinBeats, maxBeats_);
    traps = std::clamp(traps, 0, kMaxTraps);
    if (!have_ || seed != seed_ || grid != grid_ || beats != beats_ || traps != traps_) {
        if (have_ && seed == seed_ + 1 && grid == grid_ && beats == beats_ && traps == traps_) {
            previous_ = route_.maze;  // the run just played is the next one's previous: no second maze to make
            previousSeed_ = seed_;
            havePrevious_ = true;
        }
        reset(seed, grid, beats, traps);
    }
    frame_.catchingUp = false;
    if (!std::isfinite(turn)) turn = 0;
    turn = std::clamp(turn, 0.0, double(beats));
    Frame& f = frame_;
    f.maze = route_.maze;
    f.turn = turn;
    f.flash = 0;
    const int s0 = route_.count > 0 ? route_.segments[0].start : beats;
    if (turn < 1.0) {
        if (!havePrevious_ || previousSeed_ != seed - 1 || previous_.n != grid) {
            previous_ = makeRoute(seed - 1, grid, beats, traps).maze;
            previousSeed_ = seed - 1;
            havePrevious_ = true;
        }
        f.previous = previous_;
    } else {
        f.previous.n = 0;
    }
    if (turn < s0 || route_.count == 0) {
        interludeFrame(turn);
        return f;
    }
    f.rise = 1;
    f.sink = 0;
    f.holes = 1;
    f.oldHoles = 0;
    int k = 0;
    while (k + 1 < route_.count && route_.segments[std::size_t(k + 1)].start <= turn) ++k;
    // At most segmentsPerFrame_ segments planned per call (each ~.1-.5 ms): in play a frame needs at most one; after a
    // jump (an object started mid-run) the plan catches up over a few frames, the board level and the ball resting
    // where the plan has got to meanwhile.
    for (int n = 0; planned_ <= k && (segmentsPerFrame_ <= 0 || n < segmentsPerFrame_); ++n) planSegment(planned_);
    f.segment = k;
    f.catchingUp = planned_ <= k;
    if (f.catchingUp) {
        f.ballX = float(state_.x);
        f.ballY = float(state_.y);
        f.ballZ = 0;
        f.tiltX = f.tiltY = 0;
        return f;
    }
    const Segment& last = route_.segments[std::size_t(planned_ - 1)];
    const double maxIdx = double(last.next - s0) * kStepsPerBeat;
    const double x = std::min((turn - s0) * kStepsPerBeat, maxIdx);
    const int i0 = int(std::floor(x));
    const int i1 = std::min(i0 + 1, int(maxIdx));
    const float a = float(x - i0);
    auto s = [&](int i, int c) { return samples_[std::size_t(i) * 5 + std::size_t(c)]; };
    f.ballX = s(i0, 0) + (s(i1, 0) - s(i0, 0)) * a;
    f.ballY = s(i0, 1) + (s(i1, 1) - s(i0, 1)) * a;
    f.ballZ = s(i0, 2) + (s(i1, 2) - s(i0, 2)) * a;
    f.tiltX = s(i0, 3) + (s(i1, 3) - s(i0, 3)) * a;
    f.tiltY = s(i0, 4) + (s(i1, 4) - s(i0, 4)) * a;
    for (int i = hitCount_ - 1; i >= 0; --i) {
        const Hit& h = hits_[std::size_t(i)];
        if (h.time > turn) continue;
        if (turn - h.time > 1.0) break;
        f.flash = std::max(f.flash, float(std::min(1.0, h.speed / 4.0) * std::exp(-(turn - h.time) / 0.15)));
    }
    return f;
}

}  // namespace reforce::engine::labyrinth
