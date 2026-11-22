#pragma once
// reforce extension (docs/3d-design.md "Rubik's cube"): the puzzle behind the `rubik` solid. Pure C++, no
// allocations (fixed arrays), so it can run on the render thread every frame.
//
// The cube is 26 cubies at integer positions -1..1 on each axis. Each cubie carries six sticker slots, one per
// direction (kDir: +x, -x, +y, -y, +z, -z); a slot holds the colour of the sticker facing that way, or -1 (no
// sticker: plastic). Colour c is the face the sticker sits on when solved, numbered like the directions, so the
// opposite faces are the colour pairs (0, 1), (2, 3), (4, 5).
//
// A move turns one outer layer (axis 0..2 = x, y, z; layer -1 or +1) by `turns` quarter turns, right-handed about
// the axis (the same sense as engine::solidModel's rotations: +1 takes +y to +z about x).
//
// One cycle (frameAt): `moves` random moves from the seed scramble the solved cube, then the same moves are undone
// in reverse order. `turn` is the position in that sequence of 2 * moves turns: 0 = solved, moves = fully
// scrambled, 2 * moves = solved again; the fraction is how far the current turn has gone. The state is a pure
// function of (seed, moves, turn), so any jump of the clock just shows the right state.

#include <array>
#include <cstdint>

namespace reforce::engine::rubik {

inline constexpr int kCubies = 26;
inline constexpr int kMaxMoves = 32;  // per half cycle (scramble), so at most 64 turns per cycle

struct Move {
    int8_t axis = 0;   // 0 x, 1 y, 2 z
    int8_t layer = 1;  // -1 or +1
    int8_t turns = 1;  // 1, -1 or 2 quarter turns
};
inline Move inverse(Move m) { return {m.axis, m.layer, int8_t(-m.turns)}; }

struct Cubie {
    std::array<int8_t, 3> pos{};   // -1..1
    std::array<int8_t, 6> face{};  // colour per direction (kDir order), -1 = none
};

struct State {
    std::array<Cubie, kCubies> cubies{};
    static State solved();
    void apply(const Move& m);
    bool isSolved() const;
    // Colour of the sticker at (cubie position, direction); -1 when none (inside) or no cubie there.
    int colourAt(int x, int y, int z, int dir) const;
};

// `count` (clamped 0..kMaxMoves) random moves from `seed` into `out`; returns the count. Two moves in a row never
// share an axis (so no move cancels or merges with the one before). Quarter turns both ways and half turns.
int scramble(uint64_t seed, int count, Move* out);

// The cube at `turn` of a cycle (see above): `state` before the current turn, and the turn in progress.
struct Frame {
    State state;
    bool turning = false;
    Move move;           // the turn in progress (turning)
    double angle = 0.0;  // radians so far, signed (move.turns * pi / 2 * fraction)
    int moves = 0;       // scramble length (clamped)
    int index = 0;       // whole turns done, 0..2 * moves
};
Frame frameAt(uint64_t seed, int moves, double turn);

}  // namespace reforce::engine::rubik
