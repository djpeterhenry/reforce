#include "engine/Rubik.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::engine::rubik {
namespace {

// Direction index <-> (axis, sign): 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z.
int dirIndex(int axis, int sign) { return axis * 2 + (sign > 0 ? 0 : 1); }
int dirAxis(int d) { return d / 2; }
int dirSign(int d) { return d % 2 == 0 ? 1 : -1; }

// One right-handed quarter turn about `axis` of an integer vector: (i, j) -> (-j, i) in the plane (i, j) =
// (axis + 1, axis + 2) mod 3 (as engine::solidModel's rotation: +y -> +z about x, +z -> +x about y, +x -> +y about z).
void quarter(int axis, std::array<int, 3>& v) {
    const int i = (axis + 1) % 3, j = (axis + 2) % 3;
    const int vi = v[std::size_t(i)], vj = v[std::size_t(j)];
    v[std::size_t(i)] = -vj;
    v[std::size_t(j)] = vi;
}

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace

State State::solved() {
    State s;
    int n = 0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z) {
                if (x == 0 && y == 0 && z == 0) continue;
                Cubie& c = s.cubies[std::size_t(n++)];
                c.pos = {int8_t(x), int8_t(y), int8_t(z)};
                c.face.fill(-1);
                const int p[3] = {x, y, z};
                for (int a = 0; a < 3; ++a)
                    if (p[a] != 0) c.face[std::size_t(dirIndex(a, p[a]))] = int8_t(dirIndex(a, p[a]));
            }
    return s;
}

void State::apply(const Move& m) {
    const int q = ((m.turns % 4) + 4) % 4;  // -1 -> 3 quarter turns
    if (q == 0 || m.axis < 0 || m.axis > 2) return;
    for (Cubie& c : cubies) {
        if (c.pos[std::size_t(m.axis)] != m.layer) continue;
        std::array<int, 3> p{c.pos[0], c.pos[1], c.pos[2]};
        std::array<int8_t, 6> f;
        f.fill(-1);
        for (int d = 0; d < 6; ++d) {
            if (c.face[std::size_t(d)] < 0) continue;
            std::array<int, 3> v{0, 0, 0};
            v[std::size_t(dirAxis(d))] = dirSign(d);
            for (int k = 0; k < q; ++k) quarter(m.axis, v);
            int nd = 0;
            for (int a = 0; a < 3; ++a)
                if (v[std::size_t(a)] != 0) nd = dirIndex(a, v[std::size_t(a)]);
            f[std::size_t(nd)] = c.face[std::size_t(d)];
        }
        for (int k = 0; k < q; ++k) quarter(m.axis, p);
        c.pos = {int8_t(p[0]), int8_t(p[1]), int8_t(p[2])};
        c.face = f;
    }
}

bool State::isSolved() const {
    // Every sticker shows the color of the face it is on (the whole cube may not be rotated: only face turns).
    for (const Cubie& c : cubies)
        for (int d = 0; d < 6; ++d)
            if (c.face[std::size_t(d)] >= 0 && c.face[std::size_t(d)] != d) return false;
    return true;
}

int State::colorAt(int x, int y, int z, int dir) const {
    for (const Cubie& c : cubies)
        if (c.pos[0] == x && c.pos[1] == y && c.pos[2] == z) return dir >= 0 && dir < 6 ? c.face[std::size_t(dir)] : -1;
    return -1;
}

int scramble(uint64_t seed, int count, Move* out) {
    count = std::clamp(count, 0, kMaxMoves);
    uint64_t s = seed * 0x2545F4914F6CDD1Dull + 0x1234567ull;
    int prevAxis = -1;
    static constexpr int8_t kTurns[5] = {1, -1, 1, -1, 2};
    for (int k = 0; k < count; ++k) {
        int axis = int(splitmix(s) % 3);
        if (axis == prevAxis) axis = (axis + 1 + int(splitmix(s) % 2)) % 3;
        Move m;
        m.axis = int8_t(axis);
        m.layer = int8_t(splitmix(s) % 2 ? 1 : -1);
        m.turns = kTurns[splitmix(s) % 5];
        out[k] = m;
        prevAxis = axis;
    }
    return count;
}

Frame frameAt(uint64_t seed, int moves, double turn) {
    Frame f;
    std::array<Move, kMaxMoves> sc{};
    f.moves = scramble(seed, moves, sc.data());
    f.state = State::solved();
    const int total = 2 * f.moves;
    if (!std::isfinite(turn)) turn = 0.0;
    turn = std::clamp(turn, 0.0, double(total));
    int k = int(std::floor(turn));
    double frac = turn - k;
    if (k >= total) {
        k = total;
        frac = 0.0;
    }
    auto step = [&](int j) { return j < f.moves ? sc[std::size_t(j)] : inverse(sc[std::size_t(total - 1 - j)]); };
    for (int j = 0; j < k; ++j) f.state.apply(step(j));
    f.index = k;
    if (k < total && frac > 0.0) {
        f.turning = true;
        f.move = step(k);
        f.angle = f.move.turns * (std::acos(-1.0) / 2.0) * frac;
    }
    return f;
}

}  // namespace reforce::engine::rubik
