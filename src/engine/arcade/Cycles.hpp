#pragma once
// Light cycles (PLAN 5Q.3, docs/arcade-design.md "Light cycles"): the game itself, separate from its drawing so the
// tests can check the rules move by move. Cycles ride a Cols x Rows grid, one cell per tick, `Spd` ticks per beat, so
// every head sits exactly on a cell centre on every beat line; they turn 90 degrees only on turn lines (beats, or
// 8ths / 16ths with `Turn`), and every cell they ride through becomes wall until `Tail` bars later. The AI looks
// ahead and reserves its path, so a cycle crashes only when it is truly boxed in (or rammed by one that is): it
// explodes, its wall dissolves, and it rezzes in at the border on a bar line `Out` bars later (or with the next
// round).
//
// Time is the beat count only (plus SYNC_DROP / SYNC_BUILD sampled when a beat starts), so the same beats give the
// same game; a jump of more than kMaxCatchUpBeats beats starts a fresh round at the new beat instead of replaying.
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "expr/Rng.hpp"

namespace reforce::engine::arcade {

struct CyclesConfig {
    int num = 4;          // cycles, 1..8
    int cols = 48;        // grid
    int rows = 27;
    int spd = 2;          // cells (ticks) per beat, 1..8
    int turn = 1;         // turn lines per beat: 1 (beats), 2 (8ths), 4 (16ths)
    double tail = 4;      // bars a wall stands before it retracts
    double reset = 16;    // bars between rounds (phrase lines; 0 = never)
    double mirror = 1;    // 0..1: how often a pair rides as mirror images
    double out = 2;       // bars a crashed cycle is out before it rezzes in (on a bar line)
};

class CyclesSim {
public:
    static constexpr int kMaxCatchUpBeats = 8;
    static constexpr int kMaxCycles = 8;
    static constexpr int kMaxRate = 16;  // ticks per beat with a drop and a breakdown on top of Spd

    // dir: 0 = +x (right), 1 = +y (up), 2 = -x, 3 = -y; a left turn is dir + 1. Row 0 is the bottom.
    struct Point {
        int x, y;
        double t;  // beat at which the head is on this cell's centre
    };
    struct Cycle {
        std::deque<Point> trail;  // oldest first; back() = the cell the head is riding into (or sitting on); empty
                                  // while it is out
        bool alive = true;
        bool reckless = false;    // its last turn line left it no safe way
        double outSince = -1e300; // when it crashed
        double spawnBeat = -1e300;  // when it last started (a round, or rezzing in)
        int dir = 0;
        double level = 1;         // intensity: each cycle its own, so the ColorMap tells them apart
        int partner = -1;         // a leader's mirror partner (the partner has leader set instead)
        int leader = -1;
        int mirror = 0;           // the pair's symmetry: 0 left-right, 1 up-down, 2 about the centre
        bool turned = false;      // turned on the last tick
        int lastRel = 0;          // its last turn (1 left, -1 right) and when
        double lastTurn = -1e300;
    };
    struct Stats {
        long ticks = 0, turns = 0, barTurns = 0, offLineTurns = 0, rounds = 0, skips = 0, boostBeats = 0;
        long crashes = 0;      // cycles crashed: ...
        long boxedCrashes = 0; // ... boxed in themselves (no safe way at their last turn line)
        long rammed = 0;       // ... hit by a boxed-in one (or its wall), or in a head-on with it
        long unexplained = 0;  // ... neither: a bug (the tests expect none)
        long headOns = 0;      // two heads meeting (both crash)
        long respawns = 0;
    };
    // A crash: where (cell units, between the head and the wall it hit) and when.
    struct Crash {
        int cycle;
        double x, y, t;
        bool boxed;
    };
    const std::vector<Crash>& crashLog() const { return crashLog_; }  // the last 64
    // Called after every tick (tests): the tick's beat, whether it was on a turn line.
    std::function<void(const CyclesSim&, double t, bool turnLine)> onTick;

    CyclesSim(const CyclesConfig& cfg, std::uint64_t seed);

    // Advances to `beat`; `bar` is the bar position at that beat (phrase-aligned with the clock); drop / build are
    // SYNC_DROP / SYNC_BUILD (0..1).
    void advance(double beat, double bar, double drop, double build);

    const CyclesConfig& config() const { return cfg_; }
    const std::vector<Cycle>& cycles() const { return cycles_; }
    const Stats& stats() const { return stats_; }
    // The head's position in cell units at `beat` (between its last two trail points) and its heading in radians
    // (turning smoothly over the first part of a move after a turn).
    void head(int i, double beat, double& x, double& y, double& angle) const;
    // The beats a wall stands: Tail bars of 4 beats.
    double tailBeats() const { return 4.0 * cfg_.tail; }
    // Whether cell (x, y) is wall now (for tests), and the number of wall cells.
    bool wallAt(int x, int y) const { return inside(x, y) && owner_[idx(x, y)] >= 0; }
    int wallCells() const;
    // The beat of the last round start, the last drop (walls flash), and the rate of the current beat.
    double roundBeat() const { return roundBeat_; }
    double dropBeat() const { return dropBeat_; }
    int rate() const { return rate_; }
    bool boosted(long beat) const { return beat >= boostFrom_ && beat < boostUntil_; }

    // Sparks (from walls dissolving at the end of a round or after a crash) and spinning wall fragments (from an
    // explosion), in cell units from t0 on: position p + v (1 - e^-2a) / 2 at age a, fading over life.
    struct Spark {
        double x, y, vx, vy, life, level, t0;
        double len = 0, ang = 0, spin = 0;  // a fragment: a line len cells long, turning
    };
    const std::vector<Spark>& sparks() const { return sparks_; }
    // A crashed cycle's wall, dissolving from the crash back to its tail over kDissolveBeats: point i goes at
    // t0 + kDissolveBeats * (n - 1 - i) / (n - 1).
    static constexpr double kDissolveBeats = 1.0;
    struct Ghost {
        std::vector<Point> pts;
        double t0, level;
    };
    const std::vector<Ghost>& ghosts() const { return ghosts_; }
    // Explosions: the bright ring at a crash.
    const std::vector<Crash>& blasts() const { return blasts_; }

private:
    std::size_t idx(int x, int y) const { return std::size_t(y) * std::size_t(cfg_.cols) + std::size_t(x); }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < cfg_.cols && y < cfg_.rows; }
    bool blocked(int x, int y, int self, double t) const;
    void startRound(double beat, bool sparks);
    void beginBeat(long b, double barAtB, double build);
    void tick(double t, bool turnLine, int gap);
    int decide(int i, double t, bool turnLine, int gap, int mirrorRel, bool& voluntary, bool& reckless);
    int floodRoom(int x, int y, int self, double t, int span, int cap);
    int lookAhead(int x, int y, int d, int depth, int span, double when, int self, double t);
    void retract(double t);
    void crash(int i, double t, int dir, int cause);  // cause: 0 boxed in, 1 rammed, 2 neither
    bool respawn(int i, double t);
    void prune(double beat);

    CyclesConfig cfg_;
    expr::Pcg32 rng_;
    std::vector<Cycle> cycles_;
    std::vector<int> owner_;          // wall owner per cell, -1 = free
    std::vector<double> laid_;        // when the owner's head reached it
    std::vector<int> resOwner_;       // reserved for a cycle's committed path ...
    std::vector<double> resUntil_;    // ... until this beat
    std::vector<int> softOwner_;      // the ways on of a cycle that chose this tick ...
    std::vector<double> softAt_;      // ... (this tick's beat)
    bool useSoft_ = false;
    std::vector<int> stamp_;          // flood fill
    std::vector<int> order_;          // the order cycles move in a tick: leaders first
    std::vector<int> queue_;
    std::vector<int> trace_;          // look-ahead: the cells on the way being tried
    int stampNo_ = 0;
    Stats stats_;
    bool started_ = false;
    long beat_ = 0;    // the beat being ticked
    int k_ = 0;        // the next tick in it
    int rate_ = 2;     // ticks in this beat
    int turnDiv_ = 1;  // turn lines in this beat
    bool barLine_ = false;
    double lastPhrase_ = -1e300, roundBeat_ = 0, dropBeat_ = -1e300, lastDrop_ = 0;
    long boostFrom_ = 0, boostUntil_ = 0;
    std::vector<Spark> sparks_;
    std::vector<Ghost> ghosts_;
    std::vector<Crash> blasts_, crashLog_;
};

}  // namespace reforce::engine::arcade
