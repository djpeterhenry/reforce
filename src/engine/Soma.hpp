#pragma once
// reforce extension (docs/3d-design.md "Soma cube"): the puzzle behind the `soma` solid. Pure C++; the solver and the
// player keep fixed-size storage made once at load, so a frame (and a new round's solve) allocates nothing and can
// run on the render thread.
//
// The Soma cube: seven polycubes, 27 unit cubes in all (piece order and names as in the literature):
//   0 V  the L-tricube (3 cubes)            3 Z  the S / Z tetracube
//   1 L  the L tetracube                    4 A  the left screw   (non-planar; the mirror image of B)
//   2 T  the T tetracube                    5 B  the right screw
//                                           6 P  the branch (tripod)
// Pieces turn only by the 24 proper rotations: mirror images are different pieces (A and B swap under a reflection).
//
// Figures are sets of 27 cells (x right, y up, z towards the viewer), solved by exact cover (Dancing Links /
// Algorithm X): 7 piece columns + 27 cell columns, one row per placement of a piece (each distinct orientation x
// each translation inside the figure). The cube has 11,520 solutions with the cube fixed in space, 480 up to its 24
// rotations and 240 up to rotations and reflections (Conway / Guy's "240 essentially different" solutions;
// tests/test_engine_soma.cpp counts all three).
//
// A round (frameAt) assembles one figure on the beat: the previous round's figure stands complete for `hold` beats,
// bursts apart (one beat) into a ring of hovering, tumbling pieces around it, and then the pieces fly
// in one at a time in a supported, bottom-up order (each drops in from above, or slides in from the side when
// something above blocks it), turning into place on the way and locking on their beat: piece k of 7 locks at beat
// len - (6 - k) * each, so the last one lands on the round's end (the next bar / phrase line).

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace reforce::engine::soma {

inline constexpr int kPieces = 7;
inline constexpr int kMaxPieceCells = 4;
inline constexpr int kCells = 27;  // cubes in the set, and cells in every figure
inline constexpr int kGrid = 8;    // figures fit in kGrid^3 (cells 0..kGrid-1)
inline constexpr int kRotations = 24;
inline constexpr int kScrewA = 4, kScrewB = 5;  // the chiral pair

struct Cell {
    int8_t x = 0, y = 0, z = 0;
    bool operator==(const Cell&) const = default;
};

struct Piece {
    const char* name = "";
    int count = 0;  // 3 or 4
    std::array<Cell, kMaxPieceCells> cells{};
};
const std::array<Piece, kPieces>& pieces();

// How cube `cell` of `piece` draws the four borders of each of its faces (engine::somaFaceAxes order: -u, +u, -v,
// +v), 2 bits each: 2 = the piece's outer edge (a bevel and a clear groove: between pieces), 1 = a seam to the next
// cube of the same piece on a flat stretch (faint), 0 = nothing (a concave fold, or a face hidden inside the piece).
// Face d's byte is bits 8d..8d+7 of word 0 (d < 3) or 8(d-3).. of word 1.
std::array<uint32_t, 2> edgeCodes(int piece, int cell);

// The 24 proper rotations as integer matrices, row-major (r[row * 3 + col]); index 0 is the identity.
using Rot = std::array<int8_t, 9>;
const std::array<Rot, kRotations>& rotations();
Cell rotate(const Rot& r, Cell c);

struct Figure {
    const char* name = "";
    int count = 0;
    std::array<Cell, kCells> cells{};
    Cell size;  // bounding box (cells start at 0)
};
int figureCount();
const Figure& figure(int index);         // clamped to a valid index
int findFigure(std::string_view name);   // case-insensitive; -1 when unknown
// A Figs key: "all", or figure names separated by spaces / commas, in round order ("cube pyramid steps"). Unknown
// names are skipped (and returned in `unknown`); nothing valid = just the cube.
std::vector<int> parseFigures(std::string_view text, std::string* unknown = nullptr);

// A piece placed in a figure: figure cell = rotations()[rot] * piece cell + offset.
struct Placement {
    int8_t rot = 0;
    Cell offset;
};
using Solution = std::array<Placement, kPieces>;
// The figure cells covered by `piece` under `p` (pieces()[piece].count of them).
void placedCells(int piece, const Placement& p, Cell* out);
// The solution as a kGrid^3 label grid (piece index per cell, -1 empty), index (x * kGrid + y) * kGrid + z.
std::array<int8_t, kGrid * kGrid * kGrid> labels(const Solution& s);

// Exact cover over one figure. Fixed storage (~0.6 MB), made once; solve() allocates nothing.
class Solver {
public:
    Solver();
    ~Solver();
    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;

    // The first solution with the placements tried in an order shuffled by `seed` (so seeds give different
    // solutions). False when the figure has none, or none was found within `budget` placements tried.
    bool solve(const Figure& fig, uint64_t seed, Solution& out, long budget = 4'000'000);
    // Every solution in a fixed order; `fn` returns false to stop. Returns the number found (tests).
    long long enumerate(const Figure& fig, bool (*fn)(const Solution&, void*), void* ctx);
    // The same with the placements tried in an order shuffled by `seed` (when `shuffle`), and at most `budget`
    // placements tried (0: no limit).
    long long enumerate(const Figure& fig, bool (*fn)(const Solution&, void*), void* ctx, uint64_t seed, bool shuffle,
                        long budget);
    // Placements (rows) of the last figure built: tests and the log.
    int rows() const;

private:
    struct Dlx;
    std::unique_ptr<Dlx> d_;
};

// How a piece comes into its place: dropped from above, or slid in horizontally along one axis.
enum class Approach : int8_t { Above = 0, FromMinusX, FromPlusX, FromMinusZ, FromPlusZ };

// One round's figure: the solution and the assembly.
struct Plan {
    int figure = 0;
    Solution solution{};
    std::array<int8_t, kPieces> order{0, 1, 2, 3, 4, 5, 6};  // order[k] = the piece placed k-th
    std::array<Approach, kPieces> approach{};                 // per piece
    std::array<float, 3> center{};                            // the figure's bounding-box center (cell units)
    bool resting = true;  // every piece rests on the floor or on a piece placed before it (else: touches one)
    float ring = 0;       // the exploded ring's radius (ringRadius of the player's figures)
    int8_t burst = 0;     // how this figure bursts (a mode under which no two pieces meet; -1: none found)
};
// A bottom-up assembly order for `s` in `fig` (ties broken by `seed`): each piece, when placed, rests on the floor
// or on pieces already placed (a cell directly below), and has a clear path in (nothing placed above it; else
// nothing placed beside it along one horizontal direction). Fills plan.order / approach / resting; false when no
// such order exists.
bool assemble(const Figure& fig, const Solution& s, uint64_t seed, Plan& plan);
// A plan for `figure`: solutions from `seed` (and the seeds after it, if needed) until one assembles resting, every
// piece dropped in from above, with a burst under which no two pieces meet (else the best found). `ring`: the
// exploded ring's radius (0: ringRadius of this figure alone).
bool makePlan(Solver& solver, int figure, uint64_t seed, Plan& out, double ring = 0);
// Clearances (docs/3d-design.md "Soma cube", "No two pieces meet"): a piece's bounding radius about its centroid;
// the ring radius that keeps the hovering pieces clear of each other and of any of `figures`; the burst mode of a
// plan (sets plan.burst; false when no mode keeps the pieces apart).
double pieceRadius(int piece);
double ringRadius(const std::vector<int>& figures);
bool chooseBurst(Plan& plan);

// The round's beats (scene keys Len, Hold, Each).
struct Timing {
    double len = 32;   // beats per round: the last piece locks on `len`
    double hold = 8;   // beats the finished figure stands before it bursts
    double each = 2;   // beats between locks
    double burst = 1;  // beats the burst takes
};
// Clamped to sane values: len 8..1024, each .25..(room for 7 pieces after the hold and the burst), hold shortened
// when the build needs the room. Junk (NaN, inf) falls back to the defaults.
Timing clampTiming(Timing t);
double lockBeat(const Timing& t, int k);    // when the k-th piece (0..6) locks
double flightBeats(const Timing& t);        // how long a piece flies in

// What a piece is doing (tests, the log): in its place, bursting out, hovering in the ring, flying in (the arc,
// turning into place), or on its straight final approach.
enum class PieceState : int8_t { Placed, Burst, Hover, Flight, Approach };
struct PieceFrame {
    std::array<float, 9> rot{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major; world = rot * piece cell + pos
    std::array<float, 3> pos{};                          // cell units, around the stage center
    float flash = 0;  // 1 when it locks (and the whole figure on completion), decaying
    bool placed = false;  // in its place in the figure being shown (the previous one while it holds)
    PieceState state = PieceState::Placed;
};
enum class Phase { Hold, Burst, Hover, Build };
// How the figure is seen (the scene's RotY, then RotX). Unused since the ring moved into the figure's own space
// (2026-11-24, so every clearance can be checked ahead); kept so callers need not change.
struct View {
    double yaw = 0, tilt = 0;
};
struct Frame {
    std::array<PieceFrame, kPieces> pieces{};
    Phase phase = Phase::Hold;
    int figure = 0;    // the figure shown complete (Hold) or being built
    int placed = 0;    // pieces locked into it
    float thump = 0;   // 1 at each lock, decaying (the figure dips)
    double turn = 0;   // clamped
};
// The pieces at `turn` beats into the round that builds `cur`, coming from `prev` (the previous round's figure).
// `explode` (scene key Expl, e.g. SYNC_DROP, 0..1) opens the figure up along its pieces' checked burst paths (about a
// cell at 1);
// `tension` (Tens, e.g. SYNC_BUILD) makes the hovering pieces tremble and pull in. A pure function of its inputs.
Frame frameAt(const Plan& prev, const Plan& cur, double turn, const Timing& timing, double explode = 0,
              double tension = 0, const View& view = {});

// Plans per round, cached (the current and previous round's; a new round solves once, ~0.1-1 ms). Round r builds
// figures[r mod n]; its solution comes from a hash of r. Construct at load (allocates the solver).
class Player {
public:
    explicit Player(std::vector<int> figures);
    Plan plan(int64_t round);
    // The frame for Seed / Turn as the scene gives them (junk-safe: NaN = round 0 / complete).
    Frame frame(double seed, double turn, const Timing& timing, double explode = 0, double tension = 0,
                const View& view = {});
    const std::vector<int>& figures() const { return figs_; }
    int solves() const { return solves_; }

private:
    std::unique_ptr<Solver> solver_;
    std::vector<int> figs_;
    struct Entry {
        int64_t round = 0;
        bool valid = false;
        uint64_t used = 0;
        Plan plan;
    };
    std::array<Entry, 4> cache_{};
    double ring_ = 0;
    uint64_t clock_ = 0;
    int solves_ = 0;
};

}  // namespace reforce::engine::soma
