#pragma once
// reforce extension (docs/3d-design.md "Knight's tour"): the knight's tour behind the `knight` solid. Pure C++; the
// tour search runs once per tour (engine::knight::TourCache), the frame is a pure function of (tour, turn) without
// allocations.
//
// Board: 8 x 8 squares numbered row by row, square = row * 8 + col; row 0 is the first rank (nearest the viewer at the
// rest pose), col 0 the a-file. Square centers in board units (1 per square): x = col - 3.5, y = row - 3.5.
//
// A tour is closed: 64 distinct squares, every step a knight move, and the last square a knight move from the first,
// so the 64th hop returns the knight to where it started. Every tour starts on `home` (g1 by default), so tour after
// tour continues without a jump. Symmetric tours are invariant under the half turn of the board (square s and 63 - s
// are 32 hops apart); quarter-turn symmetric closed tours don't exist on 8 x 8.
//
// One tour per cycle: `turn` 0..64 in hops. Hop k (1..64) lands exactly on turn k; between landings the knight
// settles, turns towards the next square, crouches and flies in an arc (the last half of each hop).

#include <array>
#include <cstdint>

#include "engine/Meshes.hpp"

namespace reforce::engine::knight {

inline constexpr int kSide = 8;
inline constexpr int kSquares = kSide * kSide;
inline constexpr int kHome = 6;  // g1

using Tour = std::array<int8_t, kSquares>;

inline float squareX(int s) { return float(s % kSide) - 3.5f; }
inline float squareY(int s) { return float(s / kSide) - 3.5f; }

bool isKnightMove(int a, int b);
// 64 distinct squares, each step a knight move, last -> first a knight move.
bool isClosedTour(const Tour& t);
// Invariant under the board's half turn (t[i + 32] = 63 - t[i]).
bool isSymmetric(const Tour& t);

// A closed tour from `seed`, starting on `home` (0..63). symmetric: half-turn symmetric. Deterministic, bounded work
// (randomized Warnsdorff tries / a budgeted depth-first search over half tours); falls back to a fixed tour.
Tour makeTour(uint64_t seed, bool symmetric, int home = kHome);

// The last few tours made, so a tour is searched for once (not every frame).
class TourCache {
public:
    const Tour& get(uint64_t seed, bool symmetric, int home = kHome);

private:
    struct Entry {
        uint64_t seed = 0;
        int flags = -1;  // symmetric | home << 1; -1 = empty
        Tour tour{};
    };
    std::array<Entry, 4> entries_{};
    int next_ = 0;
};

// Heading of hop k (1..64: from t[k - 1] to t[k mod 64]), radians, 0 = +x (towards the h-file), counter-clockwise.
float hopHeading(const Tour& t, int k);

struct Frame {
    // The knight: board units (z up from the board), heading (radians about z, 0 = facing +x), squash (vertical
    // scale; the width keeps the volume), pitch (nose up > 0, radians).
    float x = 0, y = 0, z = 0, heading = 0, squash = 1, pitch = 0;
    int landed = 0;    // hops landed, 0..64 (segments 1..landed of the web are drawn)
    float grow = 0;    // the hop in flight: horizontal progress 0..1 along segment landed + 1 (0 at rest)
    float since = 0;   // hops since the last landing, 0..1 (landing ripple, settle)
    int square = 0;    // the square the knight stands on (or took off from)
    int next = 0;      // the square it flies to next
};
// `prev`: the tour before (its closing heading is where the knight faces at turn 0); may equal `tour`.
// hover 0..1 lifts the knight above the board between hops (a build). Clamped, NaN-safe.
Frame frameAt(const Tour& tour, const Tour& prev, double turn, double hover = 0);

// The knight piece (MeshKind::Knight): a stylized chess knight modeled in code (a lathed base and collar, a horse head
// from an extruded, rounded side profile with ears, a ridged mane, carved eyes, nostrils and mouth), polygonized from
// its distance field (surface nets) with smooth normals. Z up, facing +x, base on z = 0, height about 1.05, base radius
// .375; uv = (0, 0), edge = 1.
Mesh makePiece(double cell = 0.0125);
inline constexpr float kPieceHeight = 1.05f;

// Flat parts of the board and the web, in the plane z = 0 (normal +z, counter-clockwise from above):
// the board's 64 squares (1 x 1 around their centers; uv 0..1 across the square, edge = 1 on light squares, 0 on dark;
// a1 is dark); a unit quad x 0..1, y -.5...5 (a web segment, scaled and turned); a disc of radius 1 (inner = 0) or a
// ring from `inner` to 1 (uv.x = the radius).
Mesh makeBoard();
Mesh makeSegmentQuad();
Mesh makeDisc(int segments, double inner);

}  // namespace reforce::engine::knight
