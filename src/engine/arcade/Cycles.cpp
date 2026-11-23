// Arcade organic `cycles` (PLAN 5Q.3, docs/arcade-design.md "Light cycles"): cycles riding a grid on the beat,
// leaving walls behind them, seen from above. The game is CyclesSim (Cycles.hpp); the body draws it: a faint grid
// pulsing on the beats, the arena's border, every wall as a few long lines (straight runs merged) fading towards
// its retracting tail, the heads (a dart of our own design) over them, and the sparks of the walls dissolving at the
// end of a round.
#include "engine/arcade/Cycles.hpp"

#include <climits>
#include <cstdio>

#include "engine/arcade/Arcade.hpp"

namespace reforce::engine::arcade {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr int kDx[4] = {1, 0, -1, 0};
constexpr int kDy[4] = {0, 1, 0, -1};
constexpr int kNoMirror = INT_MIN;
constexpr int kMaxSparks = 2400;     // from the walls at the end of a round
constexpr int kMaxSparksAll = 4000;  // with crashes on top
constexpr int kRoomCap = 48;      // flood fill (turn points): enough room to keep riding
constexpr int kMinRoom = 8;       // fewer turn points than this behind a path is a pocket
constexpr int kDepth = 6;         // turn lines the AI looks ahead
constexpr int kTurnRoom = 16;     // a turn by choice only into at least this much room
constexpr int kOpenLook = 24;     // how far ahead the AI looks for open ground

}  // namespace

// ------------------------------------------------------------------ the game

CyclesSim::CyclesSim(const CyclesConfig& c, std::uint64_t seed) : cfg_(c), rng_(seed ^ 0x6379636c65735eedULL) {
    cfg_.num = std::clamp(cfg_.num, 1, kMaxCycles);
    cfg_.cols = std::clamp(cfg_.cols, 8, 400);
    cfg_.rows = std::clamp(cfg_.rows, 6, 300);
    cfg_.spd = std::clamp(cfg_.spd, 1, 8);
    cfg_.turn = cfg_.turn <= 1 ? 1 : cfg_.turn <= 2 ? 2 : 4;
    cfg_.tail = std::clamp(finiteOr(cfg_.tail, 4), 0.25, 64.0);
    cfg_.reset = std::clamp(finiteOr(cfg_.reset, 16), 0.0, 1024.0);
    cfg_.mirror = std::clamp(finiteOr(cfg_.mirror, 1), 0.0, 1.0);
    const std::size_t n = std::size_t(cfg_.cols) * std::size_t(cfg_.rows);
    owner_.assign(n, -1);
    laid_.assign(n, 0);
    resOwner_.assign(n, -1);
    resUntil_.assign(n, -1e300);
    softOwner_.assign(n, -1);
    softAt_.assign(n, -1e300);
    stamp_.assign(n, 0);
    queue_.reserve(n);
    cycles_.resize(std::size_t(cfg_.num));
    // Levels spread over .62 .. 1 (dimmer reads as black in many ColorMaps), partners far from their leaders so a mirrored pair still reads as two colors.
    const int pairs = cfg_.num / 2;
    for (int i = 0; i < cfg_.num; ++i) {
        const int slot = i < 2 * pairs ? (i % 2 == 0 ? i / 2 : pairs + i / 2) : i;
        cycles_[std::size_t(i)].level = cfg_.num == 1 ? 1.0 : 0.62 + 0.38 * double(slot) / double(cfg_.num - 1);
    }
}

int CyclesSim::wallCells() const {
    int n = 0;
    for (int o : owner_) n += o >= 0 ? 1 : 0;
    return n;
}

bool CyclesSim::blocked(int x, int y, int self, double t) const {
    if (!inside(x, y)) return true;
    const std::size_t i = idx(x, y);
    return owner_[i] >= 0 || (resOwner_[i] >= 0 && resOwner_[i] != self && resUntil_[i] > t) ||
           (useSoft_ && softOwner_[i] >= 0 && softOwner_[i] != self && softAt_[i] == t);
}

void CyclesSim::advance(double beat, double bar, double drop, double build) {
    if (!std::isfinite(beat) || std::abs(beat) > 1e12) return;
    if (!std::isfinite(bar)) bar = beat / 4.0;
    drop = std::clamp(finiteOr(drop, 0), 0.0, 1.0);
    build = std::clamp(finiteOr(build, 0), 0.0, 1.0);
    const auto barAt = [&](double b) { return bar - (beat - b) / 4.0; };
    const long now = long(std::floor(beat));

    // A drop: twice the speed from the next beat line for a bar, the walls flash now.
    if (drop >= 0.5 && lastDrop_ < 0.5) {
        boostFrom_ = now + 1;
        boostUntil_ = now + 5;
        dropBeat_ = beat;
    }
    lastDrop_ = drop;

    // First step, the clock jumping back, or too far ahead to replay: a fresh round on this beat line.
    const double lastT = started_ ? double(beat_) + double(k_) / double(rate_) - 1.0 / double(rate_) : 0;
    if (!started_ || beat < lastT - 1e-6 || now - beat_ > kMaxCatchUpBeats) {
        if (started_) ++stats_.skips;
        beat_ = now;
        k_ = 0;
        if (cfg_.reset > 0) lastPhrase_ = std::floor(barAt(double(now)) / cfg_.reset + 1e-6);
        startRound(double(now), started_);
        started_ = true;
    }

    for (int guard = 0; guard < (kMaxCatchUpBeats + 2) * kMaxRate; ++guard) {
        const double tb = double(beat_) + (k_ == 0 ? 0.0 : double(k_) / double(rate_));
        if (tb > beat + 1e-9) break;
        if (k_ == 0) beginBeat(beat_, barAt(double(beat_)), build);
        const bool turnLine = (k_ * turnDiv_) % rate_ == 0;
        int gap = 1;
        while (k_ + gap < rate_ && ((k_ + gap) * turnDiv_) % rate_ != 0) ++gap;
        tick(tb, turnLine, gap);
        if (onTick) onTick(*this, tb, turnLine);
        if (++k_ >= rate_) {
            k_ = 0;
            ++beat_;
        }
    }
    prune(beat);
}

void CyclesSim::beginBeat(long b, double barAtB, double build) {
    if (cfg_.reset > 0) {
        const double p = std::floor(barAtB / cfg_.reset + 1e-6);
        if (p != lastPhrase_) {
            startRound(double(b), true);
            lastPhrase_ = p;
        }
    }
    barLine_ = std::abs(barAtB - std::round(barAtB)) < 0.125;
    // Crashed cycles rez in on a bar line once they have been out Out bars.
    if (barLine_)
        for (int i = 0; i < cfg_.num; ++i) {
            const Cycle& c = cycles_[std::size_t(i)];
            if (!c.alive && double(b) >= c.outSince + 4.0 * cfg_.out - 1e-6) respawn(i, double(b));
        }
    // Through a breakdown: twice the speed and turns on twice as many lines; a drop doubles the speed again.
    const bool breakdown = build >= 0.5;
    int r = cfg_.spd * (breakdown ? 2 : 1);
    if (boosted(b)) {
        r *= 2;
        ++stats_.boostBeats;
    }
    rate_ = std::clamp(r, 1, kMaxRate);
    turnDiv_ = std::min(4, cfg_.turn * (breakdown ? 2 : 1));
}

void CyclesSim::startRound(double beat, bool sparks) {
    // The old walls dissolve into sparks flying out from where they stood (a crash's sparks fly on).
    prune(beat);
    if (sparks) {
        std::size_t total = 0;
        for (const auto& c : cycles_) total += c.trail.size();
        const std::size_t stride = std::max<std::size_t>(1, (total + kMaxSparks - 1) / kMaxSparks);
        const int per = total * 2 <= std::size_t(kMaxSparks) ? 2 : 1;
        std::size_t k = 0;
        for (const auto& c : cycles_)
            for (const auto& p : c.trail) {
                if (k++ % stride) continue;
                for (int j = 0; j < per && sparks_.size() < std::size_t(kMaxSparksAll); ++j) {
                    const double a = 2 * kPi * rng_.uniform01(), v = 1.0 + 5.0 * rng_.uniform01();
                    sparks_.push_back({double(p.x), double(p.y), v * std::cos(a), v * std::sin(a),
                                       1.5 + 2.0 * rng_.uniform01(), c.level, beat});
                }
            }
    }
    std::fill(owner_.begin(), owner_.end(), -1);
    std::fill(resOwner_.begin(), resOwner_.end(), -1);
    roundBeat_ = beat;
    ++stats_.rounds;

    // A formation: each pair placed as mirror images (left-right, up-down or about the center), the leader's slot
    // in fractions of the arena.
    struct Slot {
        double fx, fy;
        int dir;
    };
    static const Slot kSides[4] = {{0.04, 0.5, 0}, {0.04, 0.22, 0}, {0.04, 0.78, 0}, {0.18, 0.06, 1}};
    static const Slot kPinwheel[4] = {{0.06, 0.08, 0}, {0.94, 0.08, 1}, {0.28, 0.3, 1}, {0.72, 0.3, 2}};
    static const Slot kBurst[4] = {{0.49, 0.5, 2}, {0.49, 0.56, 1}, {0.49, 0.44, 3}, {0.4, 0.62, 2}};
    static const Slot kColumns[4] = {{0.5, 0.04, 1}, {0.28, 0.04, 1}, {0.72, 0.04, 1}, {0.06, 0.2, 0}};
    static const Slot* const kForms[4] = {kSides, kPinwheel, kBurst, kColumns};
    static const int kFormMirror[4] = {0, 2, 0, 1};
    const int form = int(rng_.next() % 4u);
    const int mirror = kFormMirror[form];
    const int C = cfg_.cols, R = cfg_.rows;
    auto place = [&](Cycle& c, int x, int y, int dir, int who) {
        // Next free cell if taken (tiny grids).
        for (int n = 0; n < C * R && owner_[idx(x, y)] >= 0; ++n) {
            x = (x + 1) % C;
            if (x == 0) y = (y + 1) % R;
        }
        c.trail.clear();
        c.trail.push_back({x, y, beat});
        c.dir = dir;
        c.turned = c.reckless = false;
        c.alive = true;
        c.spawnBeat = beat;
        c.lastRel = 0;
        c.lastTurn = -1e300;
        owner_[idx(x, y)] = who;
        laid_[idx(x, y)] = beat;
    };
    order_.clear();
    const int pairs = cfg_.num / 2;
    for (int j = 0; j < pairs; ++j) {
        const Slot& s = kForms[form][j];
        const int x = int(std::lround(s.fx * (C - 1))), y = int(std::lround(s.fy * (R - 1)));
        Cycle& a = cycles_[std::size_t(2 * j)];
        Cycle& b = cycles_[std::size_t(2 * j + 1)];
        place(a, x, y, s.dir, 2 * j);
        int mx = x, my = y, md = s.dir;
        if (mirror != 1) mx = C - 1 - x;
        if (mirror != 0) my = R - 1 - y;
        if (mirror == 0 && (md & 1) == 0) md ^= 2;
        if (mirror == 1 && (md & 1) == 1) md ^= 2;
        if (mirror == 2) md = (md + 2) & 3;
        place(b, mx, my, md, 2 * j + 1);
        const bool mirrored = rng_.uniform01() < cfg_.mirror;
        a.partner = mirrored ? 2 * j + 1 : -1;
        b.leader = mirrored ? 2 * j : -1;
        a.leader = b.partner = -1;
        a.mirror = b.mirror = mirror;
        order_.push_back(2 * j);
    }
    for (int j = 0; j < pairs; ++j) order_.push_back(2 * j + 1);
    if (cfg_.num % 2) {
        // The odd one out: from the middle of an edge the formation leaves free.
        Cycle& c = cycles_.back();
        c.partner = c.leader = -1;
        const int dir = int(rng_.next() % 4u);
        const int x = dir == 0 ? 1 : dir == 2 ? C - 2 : C / 2, y = dir == 1 ? 1 : dir == 3 ? R - 2 : R / 2;
        place(c, x, y, dir, cfg_.num - 1);
        order_.push_back(cfg_.num - 1);
    }
}

void CyclesSim::retract(double t) {
    const double u = t - tailBeats();
    for (int i = 0; i < cfg_.num; ++i) {
        auto& tr = cycles_[std::size_t(i)].trail;
        while (tr.size() >= 2 && tr[1].t <= u) {
            const std::size_t c = idx(tr[0].x, tr[0].y);
            if (owner_[c] == i && laid_[c] == tr[0].t) owner_[c] = -1;
            tr.pop_front();
        }
    }
}

int CyclesSim::floodRoom(int x, int y, int self, double t, int span, int cap) {
    // Turn points reachable from (x, y), up to cap. A cycle turns only every `span` cells, so it moves between points
    // `span` apart; a way is open when all its cells are free (stamped cells, the planned path, count as walls). A
    // one-cell gap between two walls is no room at all for a cycle that rides two cells per turn line.
    queue_.clear();
    queue_.push_back(int(idx(x, y)));
    stamp_[idx(x, y)] = stampNo_;
    int head = 0, n = 0;
    while (head < int(queue_.size()) && n < cap) {
        const int c = queue_[std::size_t(head++)];
        const int cx = c % cfg_.cols, cy = c / cfg_.cols;
        ++n;
        for (int d = 0; d < 4; ++d) {
            int nx = cx, ny = cy;
            bool open = true;
            for (int s = 1; s <= span && open; ++s) {
                nx += kDx[d];
                ny += kDy[d];
                open = !blocked(nx, ny, self, t) && stamp_[idx(nx, ny)] != stampNo_;
            }
            if (!open) continue;
            stamp_[idx(nx, ny)] = stampNo_;
            queue_.push_back(int(idx(nx, ny)));
        }
    }
    return n;
}

int CyclesSim::lookAhead(int x, int y, int d, int depth, int span, double when, int self, double t) {
    // Depth-first over the next `depth` turn lines: the most of them some way survives. A wall counts where it will
    // still stand when the head gets there; the cells already on this way (trace_) count as walls.
    if (depth <= 0) return 0;
    const double step = 1.0 / double(rate_), gone = tailBeats() + 2.0 * step;
    int best = 0;
    for (int r2 : {0, 1, -1}) {
        const int d2 = (d + r2 + 4) & 3;
        const std::size_t mark = trace_.size();
        bool ok = true;
        int cx = x, cy = y;
        for (int s = 1; s <= span && ok; ++s) {
            cx += kDx[d2];
            cy += kDy[d2];
            if (!inside(cx, cy)) {
                ok = false;
                break;
            }
            const std::size_t q = idx(cx, cy);
            const double at = when + s * step;
            ok = !(owner_[q] >= 0 && laid_[q] + gone > at) &&
                 !(resOwner_[q] >= 0 && resOwner_[q] != self && resUntil_[q] > t) &&
                 !(useSoft_ && softOwner_[q] >= 0 && softOwner_[q] != self && softAt_[q] == t) &&
                 std::find(trace_.begin(), trace_.end(), int(q)) == trace_.end();
            trace_.push_back(int(q));
        }
        if (ok) best = std::max(best, 1 + lookAhead(cx, cy, d2, depth - 1, span, when + span * step, self, t));
        trace_.resize(mark);
        if (best >= depth) break;
    }
    return best;
}

int CyclesSim::decide(int i, double t, bool turnLine, int gap, int mirrorRel, bool& voluntary, bool& reckless) {
    voluntary = false;
    reckless = false;
    if (!turnLine) return 0;
    const Cycle& c = cycles_[std::size_t(i)];
    const Point& at = c.trail.back();
    struct Opt {
        int rel, run = 0, area = 0, open = 0, exits = 0, cross = 0, depth = 0;
        bool safe = false, inBounds = false;
    };
    Opt opts[3];
    const int span = std::max(1, rate_ / std::max(1, turnDiv_));  // ticks from one turn line to the next
    const auto newStamp = [&] {
        if (++stampNo_ == INT_MAX) {
            std::fill(stamp_.begin(), stamp_.end(), 0);
            stampNo_ = 1;
        }
    };
    // Stamps the n cells from (x, y) along d (as walls for what follows); false if one is blocked or stamped.
    const auto path = [&](int x, int y, int d, int n) {
        for (int s = 1; s <= n; ++s) {
            x += kDx[d];
            y += kDy[d];
            if (blocked(x, y, i, t) || stamp_[idx(x, y)] == stampNo_) return false;
            stamp_[idx(x, y)] = stampNo_;
        }
        return true;
    };
    const auto evaluate = [&](Opt& o) {
        const int d = (c.dir + o.rel + 4) & 3;
        // The path to the next turn line, and how far the way is open (to prefer open ground over hugging a wall).
        for (int s = 1, x = at.x, y = at.y; s <= kOpenLook; ++s) {
            x += kDx[d];
            y += kDy[d];
            if (blocked(x, y, i, t)) break;
            if (s <= gap) o.run = s;
            o.open = s;
        }
        o.safe = o.run >= gap;
        const int ex = at.x + kDx[d] * gap, ey = at.y + kDy[d] * gap;
        o.inBounds = inside(ex, ey);
        if (!o.safe) {
            // For when it is boxed in: how far it gets before it hits something, and how it would fare beyond.
            if (!o.inBounds) return;
            for (int s = 1; s <= gap; ++s) o.cross += blocked(at.x + kDx[d] * s, at.y + kDy[d] * s, i, t);
            trace_.clear();
            o.depth = 1 + lookAhead(ex, ey, d, kDepth - 1, span, t + gap / double(rate_), i, t);
            newStamp();
            o.area = floodRoom(ex, ey, i, t, span, kRoomCap);
            return;
        }
        // Two moves ahead: each way on from the end of the path to the turn line after, and the room behind that
        // (both paths count as walls, so the fill never walks back along them). The best of them is the room.
        for (int r2 = -1; r2 <= 1; ++r2) {
            const int d2 = (d + r2 + 4) & 3;
            newStamp();
            path(at.x, at.y, d, gap);
            if (!path(ex, ey, d2, span)) continue;
            ++o.exits;
            o.area = std::max(o.area, floodRoom(ex + kDx[d2] * span, ey + kDy[d2] * span, i, t, span, kRoomCap));
        }
        if (o.exits == 0) {
            newStamp();
            path(at.x, at.y, d, gap);
            o.area = floodRoom(ex, ey, i, t, span, kRoomCap);
        }
        // Further: how many turn lines it can keep riding from there (walls retracting meanwhile), up to kDepth.
        trace_.clear();
        for (int s = 1; s <= gap; ++s) trace_.push_back(int(idx(at.x + kDx[d] * s, at.y + kDy[d] * s)));
        o.depth = 1 + lookAhead(ex, ey, d, kDepth - 1, span, t + gap / double(rate_), i, t);
    };
    // First keeping off the ways on of the cycles that already chose this tick (so as not to cut across them), then,
    // if that leaves nothing good, without.
    auto good = [&](const Opt& o) { return o.safe && o.depth >= kDepth && o.area >= kMinRoom; };
    for (int pass = 0; pass < 2; ++pass) {
        useSoft_ = pass == 0;
        for (int k = 0; k < 3; ++k) {
            opts[k] = Opt{k == 0 ? 0 : k == 1 ? 1 : -1};
            evaluate(opts[k]);
        }
        if (good(opts[0]) || good(opts[1]) || good(opts[2])) break;
    }
    useSoft_ = false;
    auto opt = [&](int rel) -> const Opt& { return opts[rel == 0 ? 0 : rel == 1 ? 1 : 2]; };
    if (mirrorRel != kNoMirror && good(opt(mirrorRel))) {
        voluntary = mirrorRel != 0;
        return mirrorRel;
    }
    // Running out of room straight on: towards the most room, before it closes.
    if (good(opts[0]) && opts[0].area < kRoomCap / 2) {
        const int k = opts[1].area >= opts[2].area ? 1 : 2;
        if (good(opts[k]) && 2 * opts[k].area > 3 * opts[0].area) return opts[k].rel;
    }
    if (good(opts[0])) {
        // A turn by choice: likeliest on a bar line, now and then on a beat, rarely in between; and early rather than
        // late when a wall is coming up.
        double p = k_ == 0 ? (barLine_ ? 0.5 : 0.12) : 0.05;
        if (opts[0].open < gap + 6) p = std::max(p, k_ == 0 ? 0.6 : 0.3);
        if (rng_.uniform01() < p) {
            // Towards open ground: weighted by the open run and the room.
            double w[2] = {0, 0};
            for (int k = 0; k < 2; ++k) {
                const Opt& o = opts[k + 1];
                // Not the same way as the last turn soon after it: that U-turn alongside its own wall is how a cycle
                // winds itself into a spiral it cannot leave.
                const bool uTurn = o.rel == c.lastRel && t - c.lastTurn < 2.0;
                if (good(o) && o.area >= kTurnRoom && !uTurn) w[k] = double(o.area) * double(1 + o.open * o.open);
            }
            if (w[0] + w[1] > 0) {
                voluntary = true;
                return rng_.uniform01() * (w[0] + w[1]) < w[0] ? 1 : -1;
            }
        }
        return 0;
    }
    // Must turn (or ride on into a pocket): the safe way it can ride on longest, with the most room.
    int best = -1;
    const auto score = [&](const Opt& o) { return o.depth * (1 << 16) + o.area * 64 + o.open; };
    for (int k = 0; k < 3; ++k)
        if (opts[k].safe && (best < 0 || score(opts[k]) > score(opts[best]) ||
                             (score(opts[k]) == score(opts[best]) && rng_.uniform01() < 0.5)))
            best = k;
    // Boxed in: the way it rides longest before it hits something (a wall may retract meanwhile, or the next turn
    // line show a way out), staying in the arena.
    if (best < 0) {
        reckless = true;
        const auto boxed = [&](const Opt& o) {
            return (o.inBounds ? 1 << 26 : 0) + o.run * (1 << 20) + o.depth * (1 << 14) + std::min(o.area, 255) * 32 +
                   std::max(0, 31 - o.cross);
        };
        for (int k = 0; k < 3; ++k)
            if (best < 0 || boxed(opts[k]) > boxed(opts[best])) best = k;
    }
    return opts[best].rel;
}

void CyclesSim::tick(double t, bool turnLine, int gap) {
    ++stats_.ticks;
    retract(t);
    const double step = 1.0 / double(rate_);
    int rels[kMaxCycles] = {};
    int preX[kMaxCycles], preY[kMaxCycles], preDir[kMaxCycles];
    for (int i = 0; i < cfg_.num; ++i) {
        const auto& c = cycles_[std::size_t(i)];
        preX[i] = c.alive ? c.trail.back().x : -9;
        preY[i] = c.alive ? c.trail.back().y : -9;
        preDir[i] = c.dir;
    }
    for (int i : order_) {
        Cycle& c = cycles_[std::size_t(i)];
        if (!c.alive) continue;
        // A mirrored partner copies its leader's move while the two are still exact mirror images.
        int mirrorRel = kNoMirror;
        if (c.leader >= 0 && cycles_[std::size_t(c.leader)].alive) {
            const int L = c.leader, m = c.mirror;
            const int mx = m != 1 ? cfg_.cols - 1 - preX[L] : preX[L], my = m != 0 ? cfg_.rows - 1 - preY[L] : preY[L];
            int md = preDir[L];
            if (m == 0 && (md & 1) == 0) md ^= 2;
            if (m == 1 && (md & 1) == 1) md ^= 2;
            if (m == 2) md = (md + 2) & 3;
            if (mx == preX[i] && my == preY[i] && md == c.dir) mirrorRel = m == 2 ? rels[L] : -rels[L];
        }
        bool voluntary = false, reckless = false;
        int rel = decide(i, t, turnLine, gap, mirrorRel, voluntary, reckless);
        if (turnLine) c.reckless = reckless;
        const Point at = c.trail.back();
        int d = (c.dir + rel + 4) & 3;
        if (!inside(at.x + kDx[d], at.y + kDy[d])) {
            // Never leave the arena: the AI plans for this, but a tiny grid may leave no other way.
            for (int r : {1, -1, 2}) {
                const int e = (c.dir + r + 4) & 3;
                if (inside(at.x + kDx[e], at.y + kDy[e])) {
                    rel = r;
                    d = e;
                    break;
                }
            }
            if (!turnLine) ++stats_.offLineTurns;
        }
        rels[i] = rel;
        c.turned = rel != 0;
        if (c.turned) {
            c.lastRel = rel;
            c.lastTurn = t;
            ++stats_.turns;
            if (k_ == 0 && barLine_) ++stats_.barTurns;
        }
        c.dir = d;
        const int nx = at.x + kDx[d], ny = at.y + kDy[d];
        if (turnLine)
            for (int s = 1, x = at.x, y = at.y; s <= gap; ++s) {
                x += kDx[d];
                y += kDy[d];
                if (!inside(x, y)) break;
                const std::size_t q = idx(x, y);
                if (resOwner_[q] < 0 || resUntil_[q] <= t || resOwner_[q] == i) {
                    resOwner_[q] = i;
                    resUntil_[q] = t + gap * step + 1e-9;
                }
            }
        if (turnLine) {
            // Its ways on from the end of that path, for the cycles still to choose this tick to keep off.
            const int span = std::max(1, rate_ / std::max(1, turnDiv_));
            const int ex = at.x + kDx[d] * gap, ey = at.y + kDy[d] * gap;
            for (int r2 = -1; r2 <= 1; ++r2) {
                const int d2 = (d + r2 + 4) & 3;
                for (int s = 1, x = ex, y = ey; s <= span; ++s) {
                    x += kDx[d2];
                    y += kDy[d2];
                    if (!inside(x, y)) break;
                    softOwner_[idx(x, y)] = i;
                    softAt_[idx(x, y)] = t;
                }
            }
        }
        const std::size_t q = idx(nx, ny);
        if (owner_[q] >= 0) {
            // It hits a wall (or a head: then both crash). Only a boxed-in cycle rides into one, or one whose way a
            // boxed-in cycle cut across.
            const int o = owner_[q];
            Cycle* other = o != i ? &cycles_[std::size_t(o)] : nullptr;
            const bool headOn = other && other->alive && !other->trail.empty() && other->trail.back().x == nx &&
                                other->trail.back().y == ny;
            const int cause = c.reckless ? 0 : other && other->reckless ? 1 : 2;
            if (headOn) {
                ++stats_.headOns;
                crash(o, t, other->dir, other->reckless ? 0 : c.reckless ? 1 : 2);
            }
            crash(i, t, d, cause);
            continue;
        }
        owner_[q] = i;
        laid_[q] = t + step;
        c.trail.push_back({nx, ny, t + step});
    }
}

void CyclesSim::crash(int i, double t, int dir, int cause) {
    Cycle& c = cycles_[std::size_t(i)];
    if (!c.alive || c.trail.empty()) return;
    // Already on its way into the next cell this tick: it never got there.
    if (c.trail.size() >= 2 && c.trail.back().t > t + 1e-9) {
        const Point& p = c.trail.back();
        const std::size_t q = idx(p.x, p.y);
        if (owner_[q] == i && laid_[q] == p.t) owner_[q] = -1;
        c.trail.pop_back();
    }
    const Point h = c.trail.back();
    const double bx = h.x + 0.45 * kDx[dir], by = h.y + 0.45 * kDy[dir];
    ++stats_.crashes;
    (cause == 0 ? stats_.boxedCrashes : cause == 1 ? stats_.rammed : stats_.unexplained)++;
    const Crash k{i, bx, by, t, cause == 0};
    blasts_.push_back(k);
    crashLog_.push_back(k);
    if (crashLog_.size() > 64) crashLog_.erase(crashLog_.begin());
    // The explosion: sparks all round, and short wall fragments spinning out.
    for (int j = 0; j < 36 && sparks_.size() < std::size_t(kMaxSparksAll); ++j) {
        const double a = 2 * kPi * rng_.uniform01(), v = 3.0 + 9.0 * rng_.uniform01();
        sparks_.push_back({bx, by, v * std::cos(a), v * std::sin(a), 0.8 + 1.4 * rng_.uniform01(),
                           std::min(1.0, c.level + 0.2), t});
    }
    for (int j = 0; j < 10 && sparks_.size() < std::size_t(kMaxSparksAll); ++j) {
        const double a = 2 * kPi * (j + rng_.uniform01()) / 10.0, v = 2.0 + 5.0 * rng_.uniform01();
        Spark f{bx, by, v * std::cos(a), v * std::sin(a), 1.2 + 1.0 * rng_.uniform01(), c.level, t};
        f.len = 0.5 + 0.8 * rng_.uniform01();
        f.ang = 2 * kPi * rng_.uniform01();
        f.spin = (rng_.uniform01() < 0.5 ? -1 : 1) * (4.0 + 8.0 * rng_.uniform01());
        sparks_.push_back(f);
    }
    // Its wall dissolves from the crash back to the tail, each cell a spark as it goes.
    Ghost g{std::vector<Point>(c.trail.begin(), c.trail.end()), t, c.level};
    const std::size_t n = g.pts.size();
    for (std::size_t j = 0; j < n; ++j) {
        const Point& p = g.pts[j];
        const std::size_t q = idx(p.x, p.y);
        if (owner_[q] == i && laid_[q] == p.t) owner_[q] = -1;
        if (sparks_.size() >= std::size_t(kMaxSparksAll)) continue;
        const double a = 2 * kPi * rng_.uniform01(), v = 0.5 + 2.0 * rng_.uniform01();
        const double when = t + kDissolveBeats * (n > 1 ? double(n - 1 - j) / double(n - 1) : 0.0);
        sparks_.push_back({double(p.x), double(p.y), v * std::cos(a), v * std::sin(a), 1.0 + rng_.uniform01(),
                           c.level, when});
    }
    ghosts_.push_back(std::move(g));
    for (auto& r : resOwner_)
        if (r == i) r = -1;
    c.trail.clear();
    c.alive = false;
    c.reckless = false;
    c.outSince = t;
}

bool CyclesSim::respawn(int i, double t) {
    // Rezzing in: a free cell one in from the border, heading in, with a clear run ahead.
    Cycle& c = cycles_[std::size_t(i)];
    const int C = cfg_.cols, R = cfg_.rows;
    for (int tries = 0; tries < 48; ++tries) {
        const int edge = int(rng_.next() % 4u);
        const double f = rng_.uniform01();
        int x = 1 + int(f * (C - 2.0001)), y = 1 + int(f * (R - 2.0001));
        if (edge == 0) x = 1;
        if (edge == 1) y = 1;
        if (edge == 2) x = C - 2;
        if (edge == 3) y = R - 2;
        const int dir = edge;  // left edge: heading right (0); bottom: up (1); right: left (2); top: down (3)
        bool ok = !blocked(x, y, i, t);
        for (int s = 1, px = x, py = y; s <= 6 && ok; ++s) {
            px += kDx[dir];
            py += kDy[dir];
            ok = !blocked(px, py, i, t);
        }
        if (!ok) continue;
        c.trail.clear();
        c.trail.push_back({x, y, t});
        c.dir = dir;
        c.alive = true;
        c.turned = c.reckless = false;
        c.lastRel = 0;
        c.lastTurn = -1e300;
        c.spawnBeat = t;
        owner_[idx(x, y)] = i;
        laid_[idx(x, y)] = t;
        ++stats_.respawns;
        return true;
    }
    return false;  // no room: the next bar line
}

void CyclesSim::prune(double beat) {
    sparks_.erase(std::remove_if(sparks_.begin(), sparks_.end(),
                                 [&](const Spark& k) { return beat > k.t0 + k.life || beat < k.t0 - 64; }),
                  sparks_.end());
    ghosts_.erase(std::remove_if(ghosts_.begin(), ghosts_.end(),
                                 [&](const Ghost& g) { return beat > g.t0 + kDissolveBeats || beat < g.t0 - 64; }),
                  ghosts_.end());
    blasts_.erase(std::remove_if(blasts_.begin(), blasts_.end(),
                                 [&](const Crash& k) { return beat > k.t + 2.0 || beat < k.t - 64; }),
                  blasts_.end());
}

void CyclesSim::head(int i, double beat, double& x, double& y, double& angle) const {
    const auto& tr = cycles_[std::size_t(i)].trail;
    x = y = angle = 0;
    if (tr.empty()) return;
    const Point& b = tr.back();
    x = b.x;
    y = b.y;
    angle = std::atan2(kDy[cycles_[std::size_t(i)].dir], kDx[cycles_[std::size_t(i)].dir]);
    if (tr.size() < 2) return;
    const Point& a = tr[tr.size() - 2];
    const double f = b.t > a.t ? std::clamp((beat - a.t) / (b.t - a.t), 0.0, 1.0) : 1.0;
    x = a.x + (b.x - a.x) * f;
    y = a.y + (b.y - a.y) * f;
    angle = std::atan2(double(b.y - a.y), double(b.x - a.x));
    if (tr.size() >= 3 && f < 0.4) {
        // Swing round over the first part of the move after a turn.
        const Point& p = tr[tr.size() - 3];
        const double a0 = std::atan2(double(a.y - p.y), double(a.x - p.x));
        double da = angle - a0;
        while (da > kPi) da -= 2 * kPi;
        while (da < -kPi) da += 2 * kPi;
        const double u = f / 0.4, e = u * u * (3 - 2 * u);
        angle = a0 + da * (0.35 + 0.65 * e);
    }
}

// ------------------------------------------------------------------ the organic

namespace {

class Cycles : public OrganicBody {
public:
    Cycles(scene::OrganicInstance& in, std::uint64_t seed) : sim_(config(in), seed) {
        grid_ = keyNum(in, "GRID", 0, 0, 1);
        border_ = keyNum(in, "BRDR", 0.5, 0, 1);
        wdth_ = keyNum(in, "WDTH", 3, 0, 50);
        fit(16.0 / 9.0);
    }

    // The arena fitted to the frame (half width `halfW` tree units; 3.36 x 1.88 at 16:9).
    void fit(double halfW) {
        const auto& c = sim_.config();
        fitW_ = halfW;
        cell_ = std::min(3.36 * halfW / (16.0 / 9.0) / c.cols, 1.88 / c.rows);
        x0_ = -0.5 * cell_ * (c.cols - 1);
        y0_ = -0.5 * cell_ * (c.rows - 1);
    }
    double fitW_ = 0;

    static CyclesConfig config(scene::OrganicInstance& in) {
        CyclesConfig c;
        c.num = keyCount(in, "NUM", 4, 1, CyclesSim::kMaxCycles);
        c.cols = keyCount(in, "COLS", 48, 8, 400);
        c.rows = keyCount(in, "ROWS", 27, 6, 300);
        c.spd = keyCount(in, "SPD", 2, 1, 8);
        c.turn = keyCount(in, "TURN", 1, 1, 4);
        c.tail = keyNum(in, "TAIL", 4, 0.25, 64);
        c.reset = keyNum(in, "RSD", 16, 0, 1024);
        c.mirror = keyNum(in, "MIRR", 1, 0, 1);
        c.out = keyNum(in, "OUT", 2, 0.25, 64);
        return c;
    }

    void step(const OrganicStep& s, OrganicCanvas& c) override {
        if (const double hw = std::clamp(c.halfWidth(), 0.25, 8.0); hw != fitW_) fit(hw);  // the frame's aspect
        sim_.advance(s.beat, s.bar, dropOf(s), buildOf(s));
        const double beat = s.beat, pen = s.pen;
        const auto& cfg = sim_.config();
        const double frac = beat - std::floor(beat);
        // Walls flash on a drop, fading over a bar.
        const double sinceDrop = beat - sim_.dropBeat();
        const double flash = sinceDrop >= 0 && sinceDrop < 4 ? std::exp(-1.2 * sinceDrop) : 0.0;
        const double half = 0.5 * cell_;
        const double left = x0_ - half, right = x0_ + cell_ * (cfg.cols - 1) + half;
        const double bottom = y0_ - half, top = y0_ + cell_ * (cfg.rows - 1) + half;

        // The floor: grid lines pulsing on the beat, then the arena's border.
        if (grid_ > 0) {
            const double g = pen * grid_ * (0.55 + 0.45 * std::exp(-5.0 * frac));
            const double w = std::max(0.4, 0.3 * wdth_);
            for (int k = 1; k < cfg.cols; ++k) c.line(left + k * cell_, bottom, left + k * cell_, top, w, g);
            for (int k = 1; k < cfg.rows; ++k) c.line(left, bottom + k * cell_, right, bottom + k * cell_, w, g);
        }
        if (border_ > 0) {
            const double b = std::min(1.0, pen * border_ * (0.8 + 0.2 * std::exp(-5.0 * frac)) + pen * flash * 0.5);
            const double w = std::max(0.5, 0.6 * wdth_);
            c.line(left, bottom, right, bottom, w, b);
            c.line(right, bottom, right, top, w, b);
            c.line(right, top, left, top, w, b);
            c.line(left, top, left, bottom, w, b);
        }

        // Sparks (walls dissolving at the end of a round or after a crash) and an explosion's spinning fragments.
        for (const auto& k : sim_.sparks()) {
            const double a = beat - k.t0;
            if (a < 0 || a >= k.life) continue;
            const double e = (1.0 - std::exp(-2.0 * a)) / 2.0, f = 1.0 - a / k.life;
            const double x = k.x + k.vx * e, y = k.y + k.vy * e;
            if (k.len > 0) {
                const double ang = k.ang + k.spin * e, hx = 0.5 * k.len * std::cos(ang), hy = 0.5 * k.len * std::sin(ang);
                c.line(gx(x - hx), gy(y - hy), gx(x + hx), gy(y + hy), wdth_, pen * k.level * f);
            } else {
                c.dotUnits(gx(x), gy(y), cell_ * (0.18 + 0.2 * f), pen * k.level * f * f);
            }
        }
        // A crashed cycle's wall, dissolving from the crash back to its tail.
        for (const auto& g : sim_.ghosts()) {
            const std::size_t n = g.pts.size();
            const double a = beat - g.t0;
            if (a < 0 || n < 2) continue;
            const double p = pen * g.level * 0.92 * (1.0 - 0.5 * a / CyclesSim::kDissolveBeats);
            std::size_t last = n;  // the points still standing: 0 .. last - 1
            while (last > 0 && g.t0 + CyclesSim::kDissolveBeats * double(n - last) / double(n - 1) <= beat) --last;
            for (std::size_t j = 0, k = 0; j + 1 < last; j = k) {
                k = j + 1;  // merge a straight run
                while (k + 1 < last && g.pts[k + 1].x - g.pts[k].x == g.pts[j + 1].x - g.pts[j].x &&
                       g.pts[k + 1].y - g.pts[k].y == g.pts[j + 1].y - g.pts[j].y)
                    ++k;
                c.line(gx(g.pts[j].x), gy(g.pts[j].y), gx(g.pts[k].x), gy(g.pts[k].y), wdth_, p);
            }
        }
        // Explosions: a white-hot flash and a ring racing out.
        for (const auto& k : sim_.blasts()) {
            const double a = beat - k.t;
            if (a < 0) continue;
            const double cx = gx(k.x), cy = gy(k.y);
            if (a < 0.3) c.dotUnits(cx, cy, cell_ * 2.4 * (1.0 - a / 0.3), pen);
            if (a < 0.75) {
                const double r = cell_ * (0.5 + 6.0 * a), p = pen * std::pow(1.0 - a / 0.75, 1.5);
                for (int q = 0; q < 12; ++q) {
                    const double a0 = 2 * kPi * q / 12.0, a1 = 2 * kPi * (q + 1) / 12.0;
                    c.line(cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1),
                           wdth_ * 0.8, p);
                }
            }
        }

        // The walls: from the retracting tail to the head, only the corners as vertices.
        const double u = beat - sim_.tailBeats();
        const auto wallPen = [&](double level, double t) {
            const double age = std::clamp((beat - t) / sim_.tailBeats(), 0.0, 1.0);
            const double p = level * (0.92 - 0.42 * age * std::sqrt(age));
            return pen * (p + (1.0 - p) * 0.85 * flash);
        };
        for (int i = 0; i < cfg.num; ++i) {
            const auto& cy = sim_.cycles()[std::size_t(i)];
            const auto& tr = cy.trail;
            if (!cy.alive || tr.size() < 2) continue;
            double hx, hy, ha;
            sim_.head(i, beat, hx, hy, ha);
            // Vertices (cell units) with their times.
            struct V {
                double x, y, t;
            };
            V verts[2048];
            int n = 0;
            std::size_t j = 0;
            while (j + 1 < tr.size() && tr[j + 1].t <= u) ++j;
            if (tr[j].t < u && j + 1 < tr.size()) {
                const auto &a = tr[j], &b = tr[j + 1];
                const double f = (u - a.t) / (b.t - a.t);
                verts[n++] = {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, u};
            } else {
                verts[n++] = {double(tr[j].x), double(tr[j].y), tr[j].t};
            }
            for (std::size_t k = j + 1; k + 1 < tr.size() && tr[k].t < beat; ++k) {
                // Keep a point only where the wall turns.
                const auto &p = tr[k - 1], &q = tr[k], &r = tr[k + 1];
                const bool corner = (q.x - p.x) != (r.x - q.x) || (q.y - p.y) != (r.y - q.y);
                if (corner && n < 2046) verts[n++] = {double(q.x), double(q.y), q.t};
            }
            verts[n++] = {hx, hy, beat};
            const double w = wdth_;
            for (int k = 0; k + 1 < n; ++k)
                c.line(gx(verts[k].x), gy(verts[k].y), gx(verts[k + 1].x), gy(verts[k + 1].y), w,
                       wallPen(cy.level, verts[k].t), wallPen(cy.level, verts[k + 1].t));
        }

        // The heads over everything: a dart pointing along the motion, a hot core, a shrinking diamond when it starts
        // (a round, or rezzing in after a crash).
        c.newPath();
        for (int i = 0; i < cfg.num; ++i) {
            const auto& cy = sim_.cycles()[std::size_t(i)];
            if (!cy.alive) continue;
            double hx, hy, ha;
            sim_.head(i, beat, hx, hy, ha);
            const double hp = pen * std::min(1.0, cy.level + 0.1);
            drawDart(c, gx(hx), gy(hy), ha, hp);
            c.dotUnits(gx(hx), gy(hy), cell_ * 0.32, std::min(1.0, hp * 1.25));
            const double since = beat - cy.spawnBeat;
            if (since >= 0 && since < 1) {
                const double r = cell_ * (0.7 + 3.5 * (1 - since) * (1 - since));
                const double p = pen * cy.level * (1 - since);
                const double cx = gx(hx), cyy = gy(hy);
                c.line(cx + r, cyy, cx, cyy + r, wdth_ * 0.6, p);
                c.line(cx, cyy + r, cx - r, cyy, wdth_ * 0.6, p);
                c.line(cx - r, cyy, cx, cyy - r, wdth_ * 0.6, p);
                c.line(cx, cyy - r, cx + r, cyy, wdth_ * 0.6, p);
            }
        }
    }

    std::string summary() const override {
        const auto& c = sim_.config();
        char b[128];
        std::snprintf(b, sizeof b, "%d cycles on %d x %d, %d cells per beat, walls %g bars", c.num, c.cols, c.rows,
                      c.spd, c.tail);
        return b;
    }

private:
    double gx(double x) const { return x0_ + cell_ * x; }
    double gy(double y) const { return y0_ + cell_ * y; }
    void drawDart(OrganicCanvas& c, double x, double y, double a, double pen) const {
        // An arrowhead with swept fins and a notched tail, about a cell long.
        static const double px[6] = {0.62, 0.02, -0.46, -0.22, -0.46, 0.02};
        static const double py[6] = {0.0, 0.3, 0.38, 0.0, -0.38, -0.3};
        const double ca = std::cos(a) * cell_ * 1.4, sa = std::sin(a) * cell_ * 1.4;
        double xs[6], ys[6];
        for (int k = 0; k < 6; ++k) {
            xs[k] = x + ca * px[k] - sa * py[k];
            ys[k] = y + sa * px[k] + ca * py[k];
        }
        c.fillPolygon(xs, ys, 6, pen);
    }

    CyclesSim sim_;
    double grid_, border_, wdth_;
    double cell_ = 0.07, x0_ = 0, y0_ = 0;
};

}  // namespace

std::unique_ptr<OrganicBody> makeCycles(scene::OrganicInstance& inst, std::uint64_t seed) {
    return std::make_unique<Cycles>(inst, seed);
}

}  // namespace reforce::engine::arcade
