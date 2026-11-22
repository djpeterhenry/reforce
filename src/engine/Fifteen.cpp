#include "engine/Fifteen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace reforce::engine::fifteen {
namespace {

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Neighbours of `cell` into out; returns how many.
int neighbours(int cell, int* out) {
    const int r = cell / kSize, c = cell % kSize;
    int n = 0;
    if (r > 0) out[n++] = cell - kSize;
    if (r < kSize - 1) out[n++] = cell + kSize;
    if (c > 0) out[n++] = cell - 1;
    if (c < kSize - 1) out[n++] = cell + 1;
    return n;
}

void slideBlank(Board& b, int from, int to) {  // the tile at `to` moves into the blank at `from`
    b.tile[std::size_t(from)] = b.tile[std::size_t(to)];
    b.tile[std::size_t(to)] = 0;
}

}  // namespace

Board Board::solved() {
    Board b;
    for (int i = 0; i < kTiles; ++i) b.tile[std::size_t(i)] = int8_t(i + 1);
    b.tile[kCells - 1] = 0;
    return b;
}

bool Board::isSolved() const {
    for (int i = 0; i < kTiles; ++i)
        if (tile[std::size_t(i)] != i + 1) return false;
    return true;
}

int Board::blank() const {
    for (int i = 0; i < kCells; ++i)
        if (tile[std::size_t(i)] == 0) return i;
    return -1;
}

int Board::scrambledness() const {
    int d = 0;
    for (int i = 0; i < kCells; ++i) {
        const int t = tile[std::size_t(i)];
        if (t == 0) continue;
        const int home = t - 1;
        d += std::abs(i / kSize - home / kSize) + std::abs(i % kSize - home % kSize);
    }
    return d;
}

int scramble(uint64_t seed, int count, std::array<int8_t, kMaxMoves + 1>& path) {
    count = std::clamp(count, 0, kMaxMoves);
    uint64_t s = seed * 0xD1342543DE82EF95ull + 0x51ull;
    int best = -1;
    std::array<int8_t, kMaxMoves + 1> cand{};
    for (int k = 0; k < kCandidates; ++k) {
        Board b = Board::solved();
        cand[0] = int8_t(kCells - 1);
        int prev = -1;
        for (int m = 1; m <= count; ++m) {
            const int at = cand[std::size_t(m - 1)];
            int nb[4];
            const int n = neighbours(at, nb);
            int opts[4], o = 0;
            for (int i = 0; i < n; ++i)
                if (nb[i] != prev) opts[o++] = nb[i];
            const int to = opts[splitmix(s) % uint64_t(o)];
            slideBlank(b, at, to);
            cand[std::size_t(m)] = int8_t(to);
            prev = at;
        }
        const int score = b.scrambledness();
        if (score > best) {
            best = score;
            path = cand;
        }
    }
    if (best < 0) path[0] = int8_t(kCells - 1);
    return count;
}

Frame frameAt(uint64_t seed, int moves, double turn, double shuffle) {
    Frame f;
    std::array<int8_t, kMaxMoves + 1> path{};
    const int n = scramble(seed, moves, path);
    f.moves = n;
    Board b = Board::solved();
    for (int m = 1; m <= n; ++m) slideBlank(b, path[std::size_t(m - 1)], path[std::size_t(m)]);
    if (!std::isfinite(turn)) turn = 0.0;
    turn = std::clamp(turn, 0.0, double(n));
    int k = int(std::floor(turn));
    double frac = turn - k;
    if (k >= n) {
        k = n;
        frac = 0.0;
    }
    // Solve step j: the blank goes from path[n - j] back to path[n - j - 1].
    for (int j = 0; j < k; ++j) slideBlank(b, path[std::size_t(n - j)], path[std::size_t(n - j - 1)]);
    f.board = b;
    f.index = k;
    int movingCell = -1, target = -1;
    if (k < n && frac > 0.0) {
        f.sliding = true;
        target = path[std::size_t(n - k)];       // the blank now
        movingCell = path[std::size_t(n - k - 1)];  // the tile sliding into it
    }
    double sh = std::isfinite(shuffle) ? std::clamp(shuffle, 0.0, 1.0) : 1.0;
    const double e = sh * sh * (3 - 2 * sh);
    const float hop = float(0.9 * std::sin(std::acos(-1.0) * sh));
    int t = 0;
    for (int cell = 0; cell < kCells; ++cell) {
        const int num = b.tile[std::size_t(cell)];
        if (num == 0) continue;
        Tile& tile = f.tiles[std::size_t(t++)];
        tile.number = num;
        float x = cellX(cell), y = cellY(cell);
        if (cell == movingCell) {
            x += float((cellX(target) - x) * frac);
            y += float((cellY(target) - y) * frac);
        }
        if (sh < 1.0) {
            const int home = num - 1;
            x = float(cellX(home) + (x - cellX(home)) * e);
            y = float(cellY(home) + (y - cellY(home)) * e);
            tile.z = hop * (0.6f + 0.4f * float(num % 3) / 2.0f);  // staggered so tiles pass over each other
        }
        tile.x = x;
        tile.y = y;
    }
    return f;
}

}  // namespace reforce::engine::fifteen
