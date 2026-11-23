#pragma once
// reforce extension (docs/3d-design.md "15 puzzle"): the sliding puzzle behind the `fifteen` solid. Pure C++, no
// allocations, so it can run on the render thread every frame.
//
// A 4 x 4 board, cells numbered row by row from the top left (cell = row * 4 + col); solved = tiles 1..15 in order
// and the blank in the bottom right. A scramble is a random walk of the blank from `seed` (never straight back, never
// off the board); of kCandidates walks the one that leaves the tiles farthest from home (summed Manhattan distance)
// is kept. The solve is the walk played backwards: each step slides one tile into the blank.
//
// One cycle (frameAt): `turn` 0 = scrambled, `moves` = solved; the fraction is how far the current tile has slid.
// `shuffle` 0..1 moves the tiles from their solved places to the scrambled ones (1 = there; with a hop towards the
// viewer), for the leap into the scramble at the start of a cycle. A pure function of its arguments.

#include <array>
#include <cstdint>

namespace reforce::engine::fifteen {

inline constexpr int kSize = 4;
inline constexpr int kCells = kSize * kSize;
inline constexpr int kTiles = kCells - 1;
inline constexpr int kMaxMoves = 64;
inline constexpr int kCandidates = 24;

// Cell center in board units: x = col - 1.5 (left to right), y = 1.5 - row (top to bottom).
inline float cellX(int cell) { return float(cell % kSize) - 1.5f; }
inline float cellY(int cell) { return 1.5f - float(cell / kSize); }

struct Board {
    std::array<int8_t, kCells> tile{};  // tile number per cell, 0 = blank
    static Board solved();
    bool isSolved() const;
    int blank() const;
    int scrambledness() const;  // summed Manhattan distance of the tiles from their home cells
};

// The blank's path: path[0] = 15 (solved), path[1..count] its cells after each scramble move. Returns the count
// (clamped 0..kMaxMoves).
int scramble(uint64_t seed, int count, std::array<int8_t, kMaxMoves + 1>& path);

struct Tile {
    int number = 0;            // 1..15
    float x = 0, y = 0, z = 0; // board units (z: the shuffle's hop)
};
struct Frame {
    std::array<Tile, kTiles> tiles{};
    Board board;          // after the whole steps done
    bool sliding = false; // a tile is between cells
    int moves = 0;        // scramble length (clamped)
    int index = 0;        // whole solve steps done, 0..moves
};
Frame frameAt(uint64_t seed, int moves, double turn, double shuffle = 1.0);

}  // namespace reforce::engine::fifteen
