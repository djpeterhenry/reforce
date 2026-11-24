#pragma once
// reforce extension (docs/3d-design.md "Jigsaw"): a jigsaw of the live frame, solved on the phrase line. Pure C++, no
// allocations after construction, so it runs on the render thread every frame.
//
// The cut (engine/JigsawShape.inc, shared with the shaders): `cols` x `rows` pieces fitted to the frame's aspect,
// classic tabs and blanks with a hand-cut irregularity, a new cut per seed. Every piece always shows the frame at its
// home place (render/JigsawPipeline), so a scattered piece is a moving patch of picture out of place.
//
// One cycle of `cycle` beats (Turn 0..cycle; Seed = the cycle's number):
//   - beats 0..brk: whole. The picture completed on the line (beat 0); its seams flash and fade out. This part still
//     shows the previous seed's cut (the puzzle that completed).
//   - beat brk: it breaks apart. Every piece lifts and bursts to a resting spot over another piece's slot (rotated,
//     overlapping, layered), in about a beat.
//   - then the pieces fly home in solve order (Order), each lifting, gliding on an arc, turning upright and dropping
//     to snap in exactly on a beat, the first on brk + 2, the last on the next line (beat `cycle`). More pieces than
//     beats: the first beats take one more each (a rush of easy pieces); fewer: pauses spread evenly.
//   - pieces in the pile rest over slots still empty; when a piece is about to land on a slot, whatever lies on it
//     hops aside to another empty slot (so the assembled part is always clear, and the pile shrinks into the gaps).
// A pure function of (seed, config, turn, kick, shake): any jump of the beat clock shows the right state.

#include <array>
#include <cstdint>

namespace reforce::engine::jigsaw {

inline constexpr int kMaxCols = 16;
inline constexpr int kMaxRows = 12;
inline constexpr int kMaxPieces = 160;
inline constexpr int kMaxSpots = 8;  // resting spots per piece and cycle (the burst's, then hops)

struct Grid {
    int cols = 8, rows = 4;
    float cw = 1.0f;     // cell width in board units (a cell is 1 high)
    uint32_t seed = 0;   // the cut
    int count() const { return cols * rows; }
    float width() const { return float(cols) * cw; }
};
// About `pieces` pieces (clamped 2..kMaxPieces) in a grid whose cells are as square as the frame's aspect (W / H)
// allows. `seed`: the cut.
Grid fitGrid(int pieces, double aspect, uint64_t seed);
uint32_t cutSeed(uint64_t seed);

// The cut (JigsawShape.inc). Board units, y down.
struct Point { float x = 0, y = 0; };
Point corner(const Grid& g, int i, int j);  // i 0..cols, j 0..rows
// The edge's field: < 0 on its "-" side (above a horizontal edge, left of a vertical one), > 0 on the "+" side.
float edgeField(const Grid& g, bool horizontal, int i, int j, Point p);
// < 0 inside piece (col, row), > 0 outside; about the distance to its outline.
float pieceField(const Grid& g, int col, int row, Point p);
struct Owner {
    int index = -1;  // the piece at p (row * cols + col), -1 outside the frame
    float g = 1e9f;  // its field (< 0)
    int index2 = -1; // the nearest other piece
    float g2 = 1e9f;
};
Owner owner(const Grid& g, Point p);
Point homeCenter(const Grid& g, int index);  // the mean of its four corners
// Which side the knob of an interior edge sticks out to: +1 into the "+" piece (below / right), -1 into the "-" one;
// 0 for a border edge.
int knobSide(const Grid& g, bool horizontal, int i, int j);

enum class Order { Edges = 0, Corner = 1, Rows = 2, Random = 3 };

struct Config {
    int pieces = 32;           // Pcs
    double aspect = 16.0 / 9.0;
    Order order = Order::Edges;  // Ordr
    double cycle = 32;         // Cycl: beats per cycle (one phrase of 8 bars)
    double brk = 4;            // Brk: the beat it breaks apart (0 = on the line itself)
};
Config clampConfig(Config c);

// Times in beats of the cycle.
inline constexpr double kBurstBeats = 1.0;   // breaking apart
inline constexpr double kFlightBeats = 0.85; // a piece's flight home (ends on its beat)
inline constexpr double kHopBeats = 0.4;     // a pile piece making room

struct Spot {
    float t0 = 0, t1 = 0;          // leaves the previous spot .. arrives
    float x = 0, y = 0, angle = 0; // center (board units), rotation (radians)
    float z = 0;                   // resting height (layering in the pile)
};

struct Plan {
    Grid grid;
    Config config;
    uint64_t seed = 0;
    int count = 0;
    std::array<int16_t, kMaxPieces> sequence{};  // pieces in landing order
    std::array<float, kMaxPieces> land{};        // landing beat per piece
    std::array<std::array<Spot, kMaxSpots>, kMaxPieces> spots{};
    std::array<uint8_t, kMaxPieces> spotCount{};
    std::array<float, kMaxPieces> rnd{};         // per piece 0..1 (kick and shake variation)
};
void makePlan(Plan& out, uint64_t seed, const Config& c);
// The landing beat of the k-th piece of n, with landings on whole beats first .. last (the last on `last`).
double landingBeat(int k, int n, int first, int last);

struct Piece {
    float x = 0, y = 0;      // where its home center is now (board units)
    float angle = 0;         // rotation about it (radians)
    float z = 0;             // lift (board units; 0 = on the board)
    float tiltX = 0, tiltY = 0;  // small tilts while flying (radians, about the x / y axes)
    float flash = 0;         // the snap's flash on its seams, 0..1
    bool landed = true;      // home and flat: drawn by the board pass
};

struct Frame {
    Grid grid;
    int count = 0;
    std::array<Piece, kMaxPieces> pieces{};
    std::array<int16_t, kMaxPieces> order{};  // the pieces that are not landed, bottom to top
    int loose = 0;                            // how many
    float seam = 1;   // the cut's visibility on the landed part (fades out after completion)
    float glow = 0;   // completion flash over all seams
    bool whole = false;  // nothing to draw: every piece home, no seams, no flash
};

struct Input {
    double turn = 0;   // beats into the cycle, 0..cycle (NaN / absent: whole)
    double kick = 0;   // Kick 0..1: the picture pops apart (a drop)
    double shake = 0;  // Shak 0..1: the pile trembles (a build)
};

// Keeps the current and the previous cycle's plans (made again only when the seed or config changes).
class Solver {
public:
    const Frame& frame(uint64_t seed, const Config& c, const Input& in);

private:
    Plan cur_, prev_;
    bool haveCur_ = false, havePrev_ = false;
    Frame frame_;
};

// Without the cache (tests).
void frameAt(const Plan& cur, const Plan& prev, const Input& in, Frame& out);

}  // namespace reforce::engine::jigsaw
