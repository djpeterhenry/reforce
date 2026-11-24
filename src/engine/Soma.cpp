#include "engine/Soma.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace reforce::engine::soma {
namespace {

constexpr double kPi = 3.14159265358979323846;

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
uint64_t hash2(uint64_t a, uint64_t b) {
    uint64_t s = a * 0x9E3779B97F4A7C15ull + b + 0x632BE59BD9B4E019ull;
    splitmix(s);
    return splitmix(s);
}
double unit(uint64_t h) { return double(h >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)

// ---- pieces, rotations, figures -------------------------------------------------------------------------------------

std::array<Piece, kPieces> makePieces() {
    auto p = [](const char* name, std::initializer_list<Cell> cells) {
        Piece r;
        r.name = name;
        r.count = int(cells.size());
        int i = 0;
        for (const Cell& c : cells) r.cells[std::size_t(i++)] = c;
        return r;
    };
    return {
        p("V", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}),
        p("L", {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}}),
        p("T", {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {1, 1, 0}}),
        p("Z", {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {2, 1, 0}}),
        // The screws: the V with a cube on top (+z) of one arm's end; A and B are mirror images.
        p("A", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 1, 1}}),
        p("B", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 0, 1}}),
        // The branch: the V with a cube on top of its corner.
        p("P", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}),
    };
}

std::array<Rot, kRotations> makeRotations() {
    std::array<Rot, kRotations> out{};
    int n = 0;
    out[std::size_t(n++)] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    static constexpr int kPerm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (const auto& perm : kPerm)
        for (int signs = 0; signs < 8; ++signs) {
            Rot r{};
            for (int row = 0; row < 3; ++row) r[std::size_t(row * 3 + perm[row])] = int8_t((signs >> row) & 1 ? -1 : 1);
            const int det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) +
                            r[2] * (r[3] * r[7] - r[4] * r[6]);
            if (det != 1 || r == out[0]) continue;
            out[std::size_t(n++)] = r;
        }
    return out;
}

// Layers bottom to top separated by '|', rows back (z = 0) to front by '/', '#' a cell.
Figure makeFigure(const char* name, const char* spec) {
    Figure f;
    f.name = name;
    int x = 0, y = 0, z = 0;
    int mx = 0, my = 0, mz = 0;
    for (const char* c = spec; *c; ++c) {
        if (*c == '|') {
            ++y;
            x = z = 0;
        } else if (*c == '/') {
            ++z;
            x = 0;
        } else {
            if (*c == '#' && f.count < kCells) {
                f.cells[std::size_t(f.count++)] = {int8_t(x), int8_t(y), int8_t(z)};
                mx = std::max(mx, x);
                my = std::max(my, y);
                mz = std::max(mz, z);
            }
            ++x;
        }
    }
    f.size = {int8_t(mx + 1), int8_t(my + 1), int8_t(mz + 1)};
    return f;
}

const std::array<Figure, 7>& figures() {
    static const std::array<Figure, 7> kFigures = {
        makeFigure("cube", "###/###/###|###/###/###|###/###/###"),
        // A stepped pyramid: 5 x 3, 3 x 3, then a ridge of 3.
        makeFigure("pyramid", "#####/#####/#####|.###./.###./.###.|..#../..#../..#.."),
        // Three steps up to the right, three deep (heights 2, 3, 4).
        makeFigure("steps", "###/###/###|###/###/###|.##/.##/.##|..#/..#/..#"),
        // A 5 x 3 floor with a rim one high.
        makeFigure("bathtub", "#####/#####/#####|#####/#...#/#####"),
        // A sofa: a 5 x 2 seat two high, the back and the arms on top.
        makeFigure("sofa", "#####/#####|#####/#####|#####/#...#"),
        // A wall 6 long, 2 deep, 2 high, with three merlons along the back.
        makeFigure("wall", "######/######|######/######|#.#.#./......"),
        // A tower: a 3 x 3 base, 2 x 2 up four floors, a 1 x 2 roof.
        makeFigure("tower", "###/###/###|##./##./...|##./##./...|##./##./...|##./##./...|#../#../..."),
    };
    return kFigures;
}

int cellIndex(const Cell& c) { return (int(c.x) * kGrid + int(c.y)) * kGrid + int(c.z); }
bool inGrid(int x, int y, int z) { return x >= 0 && y >= 0 && z >= 0 && x < kGrid && y < kGrid && z < kGrid; }

// ---- quaternions -------------------------------------------------------------------------------------------------
struct Q {
    double w = 1, x = 0, y = 0, z = 0;
};
Q qmul(const Q& a, const Q& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
Q qnorm(Q q) {
    const double l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!(l > 1e-12)) return {};
    return {q.w / l, q.x / l, q.y / l, q.z / l};
}
Q qaxis(double ax, double ay, double az, double angle) {
    const double l = std::sqrt(ax * ax + ay * ay + az * az);
    if (!(l > 1e-12)) return {};
    const double s = std::sin(angle * 0.5) / l;
    return {std::cos(angle * 0.5), ax * s, ay * s, az * s};
}
Q qfromRot(const Rot& r) {  // row-major integer rotation
    const double m00 = r[0], m01 = r[1], m02 = r[2], m10 = r[3], m11 = r[4], m12 = r[5], m20 = r[6], m21 = r[7],
                 m22 = r[8];
    const double tr = m00 + m11 + m22;
    Q q;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q = {0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        q = {(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
    }
    return qnorm(q);
}
Q qslerp(Q a, Q b, double t) {
    double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) {
        b = {-b.w, -b.x, -b.y, -b.z};
        d = -d;
    }
    if (d > 0.9995) return qnorm({a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
    const double th = std::acos(std::clamp(d, -1.0, 1.0));
    const double s = std::sin(th);
    const double ka = std::sin((1 - t) * th) / s, kb = std::sin(t * th) / s;
    return qnorm({a.w * ka + b.w * kb, a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb});
}
std::array<double, 9> qmat(const Q& q) {
    const double w = q.w, x = q.x, y = q.y, z = q.z;
    return {1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
            2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
            2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y)};
}

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
V3 mulm(const std::array<double, 9>& m, V3 v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

V3 centroid(int piece) {
    const Piece& p = pieces()[std::size_t(piece)];
    V3 c;
    for (int i = 0; i < p.count; ++i) {
        c.x += p.cells[std::size_t(i)].x;
        c.y += p.cells[std::size_t(i)].y;
        c.z += p.cells[std::size_t(i)].z;
    }
    return c * (1.0 / p.count);
}

double smooth(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3 - 2 * u);
}

}  // namespace

const std::array<Piece, kPieces>& pieces() {
    static const std::array<Piece, kPieces> k = makePieces();
    return k;
}

const std::array<Rot, kRotations>& rotations() {
    static const std::array<Rot, kRotations> k = makeRotations();
    return k;
}

Cell rotate(const Rot& r, Cell c) {
    return {int8_t(r[0] * c.x + r[1] * c.y + r[2] * c.z), int8_t(r[3] * c.x + r[4] * c.y + r[5] * c.z),
            int8_t(r[6] * c.x + r[7] * c.y + r[8] * c.z)};
}

std::array<uint32_t, 2> edgeCodes(int piece, int cell) {
    using Table = std::array<std::array<std::array<uint32_t, 2>, kMaxPieceCells>, kPieces>;
    static const Table kTable = [] {
        Table t{};
        for (int p = 0; p < kPieces; ++p) {
            const Piece& pc = pieces()[std::size_t(p)];
            auto has = [&](int x, int y, int z) {
                for (int i = 0; i < pc.count; ++i)
                    if (pc.cells[std::size_t(i)] == Cell{int8_t(x), int8_t(y), int8_t(z)}) return true;
                return false;
            };
            for (int i = 0; i < pc.count; ++i) {
                const Cell& c = pc.cells[std::size_t(i)];
                for (int d = 0; d < 6; ++d) {
                    int n[3] = {0, 0, 0};
                    n[d / 2] = d % 2 ? -1 : 1;
                    if (has(c.x + n[0], c.y + n[1], c.z + n[2])) continue;  // hidden inside the piece
                    // The face's in-plane axes, as engine::somaFaceAxes.
                    int ua = (d / 2 + 1) % 3, va = (d / 2 + 2) % 3;
                    if (d % 2) std::swap(ua, va);
                    uint32_t byte = 0;
                    for (int e = 0; e < 4; ++e) {
                        int m[3] = {c.x, c.y, c.z};
                        m[e < 2 ? ua : va] += e % 2 ? 1 : -1;
                        uint32_t code = 2;
                        if (has(m[0], m[1], m[2])) code = has(m[0] + n[0], m[1] + n[1], m[2] + n[2]) ? 0 : 1;
                        byte |= code << (2 * e);
                    }
                    t[std::size_t(p)][std::size_t(i)][std::size_t(d / 3)] |= byte << (8 * (d % 3));
                }
            }
        }
        return t;
    }();
    return kTable[std::size_t(std::clamp(piece, 0, kPieces - 1))][std::size_t(std::clamp(cell, 0, kMaxPieceCells - 1))];
}

int figureCount() { return int(figures().size()); }
const Figure& figure(int index) { return figures()[std::size_t(std::clamp(index, 0, figureCount() - 1))]; }

int findFigure(std::string_view name) {
    for (int i = 0; i < figureCount(); ++i) {
        const std::string_view n = figures()[std::size_t(i)].name;
        if (n.size() != name.size()) continue;
        bool same = true;
        for (std::size_t k = 0; k < n.size() && same; ++k)
            same = std::tolower(static_cast<unsigned char>(n[k])) == std::tolower(static_cast<unsigned char>(name[k]));
        if (same) return i;
    }
    return -1;
}

std::vector<int> parseFigures(std::string_view text, std::string* unknown) {
    std::vector<int> out;
    std::string word;
    auto flush = [&] {
        if (word.empty()) return;
        std::string low = word;
        for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (low == "all") {
            for (int i = 0; i < figureCount(); ++i) out.push_back(i);
        } else if (const int i = findFigure(word); i >= 0) {
            out.push_back(i);
        } else if (unknown) {
            if (!unknown->empty()) *unknown += ", ";
            *unknown += word;
        }
        word.clear();
    };
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ';' || c == '"') flush();
        else word.push_back(c);
    }
    flush();
    if (out.size() > 64) out.resize(64);
    if (out.empty()) out.push_back(0);
    return out;
}

void placedCells(int piece, const Placement& p, Cell* out) {
    const Piece& pc = pieces()[std::size_t(std::clamp(piece, 0, kPieces - 1))];
    const Rot& r = rotations()[std::size_t(std::clamp(int(p.rot), 0, kRotations - 1))];
    for (int i = 0; i < pc.count; ++i) {
        const Cell c = rotate(r, pc.cells[std::size_t(i)]);
        out[i] = {int8_t(c.x + p.offset.x), int8_t(c.y + p.offset.y), int8_t(c.z + p.offset.z)};
    }
}

std::array<int8_t, kGrid * kGrid * kGrid> labels(const Solution& s) {
    std::array<int8_t, kGrid * kGrid * kGrid> g;
    g.fill(-1);
    for (int p = 0; p < kPieces; ++p) {
        Cell c[kMaxPieceCells];
        placedCells(p, s[std::size_t(p)], c);
        for (int i = 0; i < pieces()[std::size_t(p)].count; ++i)
            if (inGrid(c[i].x, c[i].y, c[i].z)) g[std::size_t(cellIndex(c[i]))] = int8_t(p);
    }
    return g;
}

// ---- Dancing Links -------------------------------------------------------------------------------------------------

struct Solver::Dlx {
    static constexpr int kMaxRows = kPieces * kRotations * kCells;
    static constexpr int kMaxCols = kPieces + kCells;
    static constexpr int kMaxNodes = 1 + kMaxCols + kMaxRows * (kMaxPieceCells + 1);
    std::array<int, kMaxNodes> L{}, R{}, U{}, D{}, C{}, row{};
    std::array<int, kMaxCols + 1> S{};
    std::array<int8_t, kMaxRows> rowPiece{};
    std::array<Placement, kMaxRows> rowPlace{};
    std::array<int, kMaxRows> perm{};
    std::array<int8_t, kGrid * kGrid * kGrid> cellOf{};
    int nodes = 0, nrows = 0;
    const Figure* rowsFor = nullptr;  // the figure the rows were generated for (they depend on nothing else)
    std::array<int, kPieces> chosen{};
    long budget = 0, tried = 0;
    bool stop = false, firstOnly = true;
    long long found = 0;
    Solution first{};
    bool (*fn)(const Solution&, void*) = nullptr;
    void* ctx = nullptr;

    bool build(const Figure& fig, uint64_t seed, bool shuffle) {
        if (rowsFor != &fig) {
            if (!makeRows(fig)) return false;
            rowsFor = &fig;
        }
        link(seed, shuffle);
        return true;
    }

    bool makeRows(const Figure& fig) {
        rowsFor = nullptr;
        nrows = 0;
        cellOf.fill(-1);
        if (fig.count != kCells) return false;
        for (int i = 0; i < fig.count; ++i) {
            const Cell& c = fig.cells[std::size_t(i)];
            if (!inGrid(c.x, c.y, c.z)) return false;
            cellOf[std::size_t(cellIndex(c))] = int8_t(i);
        }
        // Rows: every distinct orientation of every piece, at every translation that stays inside the figure.
        for (int p = 0; p < kPieces; ++p) {
            const Piece& pc = pieces()[std::size_t(p)];
            std::array<std::array<Cell, kMaxPieceCells>, kRotations> seen{};
            int nseen = 0;
            for (int r = 0; r < kRotations; ++r) {
                std::array<Cell, kMaxPieceCells> rc{};
                for (int i = 0; i < pc.count; ++i) rc[std::size_t(i)] = rotate(rotations()[std::size_t(r)], pc.cells[std::size_t(i)]);
                // Normalized and sorted, to drop orientations that repeat (symmetric pieces).
                std::array<Cell, kMaxPieceCells> key{};
                int mx = 99, my = 99, mz = 99;
                for (int i = 0; i < pc.count; ++i) {
                    mx = std::min(mx, int(rc[std::size_t(i)].x));
                    my = std::min(my, int(rc[std::size_t(i)].y));
                    mz = std::min(mz, int(rc[std::size_t(i)].z));
                }
                for (int i = 0; i < pc.count; ++i)
                    key[std::size_t(i)] = {int8_t(rc[std::size_t(i)].x - mx), int8_t(rc[std::size_t(i)].y - my),
                                           int8_t(rc[std::size_t(i)].z - mz)};
                std::sort(key.begin(), key.begin() + pc.count,
                          [](const Cell& a, const Cell& b) { return cellIndex(a) < cellIndex(b); });
                bool dup = false;
                for (int s = 0; s < nseen && !dup; ++s) dup = seen[std::size_t(s)] == key;
                if (dup) continue;
                seen[std::size_t(nseen++)] = key;
                for (int a = 0; a < fig.count; ++a) {
                    const Cell& f = fig.cells[std::size_t(a)];
                    const Cell off{int8_t(f.x - rc[0].x), int8_t(f.y - rc[0].y), int8_t(f.z - rc[0].z)};
                    bool fits = true;
                    for (int i = 0; i < pc.count && fits; ++i) {
                        const int x = rc[std::size_t(i)].x + off.x, y = rc[std::size_t(i)].y + off.y,
                                  z = rc[std::size_t(i)].z + off.z;
                        fits = inGrid(x, y, z) && cellOf[std::size_t((x * kGrid + y) * kGrid + z)] >= 0;
                    }
                    if (!fits || nrows >= kMaxRows) continue;
                    rowPiece[std::size_t(nrows)] = int8_t(p);
                    rowPlace[std::size_t(nrows)] = {int8_t(r), off};
                    ++nrows;
                }
            }
        }
        return true;
    }

    // The links for the rows in an order shuffled by `seed` (or as generated).
    void link(uint64_t seed, bool shuffle) {
        for (int i = 0; i < nrows; ++i) perm[std::size_t(i)] = i;
        if (shuffle) {
            uint64_t s = seed ^ 0x5D0A5D0A5D0A5D0Aull;
            for (int i = nrows - 1; i > 0; --i) std::swap(perm[std::size_t(i)], perm[std::size_t(splitmix(s) % uint64_t(i + 1))]);
        }
        // Header + columns (1..7 pieces, 8..34 cells), then the rows in `perm` order.
        const int ncols = kMaxCols;
        for (int c = 0; c <= ncols; ++c) {
            L[std::size_t(c)] = c == 0 ? ncols : c - 1;
            R[std::size_t(c)] = c == ncols ? 0 : c + 1;
            U[std::size_t(c)] = D[std::size_t(c)] = C[std::size_t(c)] = c;
            S[std::size_t(c)] = 0;
            row[std::size_t(c)] = -1;
        }
        nodes = ncols + 1;
        for (int k = 0; k < nrows; ++k) {
            const int r = perm[std::size_t(k)];
            const int p = rowPiece[std::size_t(r)];
            Cell cells[kMaxPieceCells];
            placedCells(p, rowPlace[std::size_t(r)], cells);
            int cols[kMaxPieceCells + 1];
            int n = 0;
            cols[n++] = 1 + p;
            for (int i = 0; i < pieces()[std::size_t(p)].count; ++i) cols[n++] = 1 + kPieces + cellOf[std::size_t(cellIndex(cells[i]))];
            const int firstNode = nodes;
            for (int i = 0; i < n; ++i) {
                const int x = nodes++;
                const int c = cols[i];
                C[std::size_t(x)] = c;
                row[std::size_t(x)] = r;
                U[std::size_t(x)] = U[std::size_t(c)];
                D[std::size_t(x)] = c;
                D[std::size_t(U[std::size_t(c)])] = x;
                U[std::size_t(c)] = x;
                ++S[std::size_t(c)];
                L[std::size_t(x)] = i == 0 ? x : x - 1;
                R[std::size_t(x)] = firstNode;
                if (i > 0) R[std::size_t(x - 1)] = x;
                L[std::size_t(firstNode)] = x;
            }
        }
    }

    void cover(int c) {
        R[std::size_t(L[std::size_t(c)])] = R[std::size_t(c)];
        L[std::size_t(R[std::size_t(c)])] = L[std::size_t(c)];
        for (int i = D[std::size_t(c)]; i != c; i = D[std::size_t(i)])
            for (int j = R[std::size_t(i)]; j != i; j = R[std::size_t(j)]) {
                U[std::size_t(D[std::size_t(j)])] = U[std::size_t(j)];
                D[std::size_t(U[std::size_t(j)])] = D[std::size_t(j)];
                --S[std::size_t(C[std::size_t(j)])];
            }
    }
    void uncover(int c) {
        for (int i = U[std::size_t(c)]; i != c; i = U[std::size_t(i)])
            for (int j = L[std::size_t(i)]; j != i; j = L[std::size_t(j)]) {
                ++S[std::size_t(C[std::size_t(j)])];
                U[std::size_t(D[std::size_t(j)])] = j;
                D[std::size_t(U[std::size_t(j)])] = j;
            }
        R[std::size_t(L[std::size_t(c)])] = c;
        L[std::size_t(R[std::size_t(c)])] = c;
    }

    void search(int k) {
        if (R[0] == 0) {
            Solution s{};
            for (int i = 0; i < k; ++i) {
                const int r = chosen[std::size_t(i)];
                s[std::size_t(rowPiece[std::size_t(r)])] = rowPlace[std::size_t(r)];
            }
            ++found;
            if (found == 1) first = s;
            if (firstOnly || (fn && !fn(s, ctx))) stop = true;
            return;
        }
        int c = R[0], best = S[std::size_t(c)];
        for (int j = R[std::size_t(c)]; j != 0; j = R[std::size_t(j)])
            if (S[std::size_t(j)] < best) best = S[std::size_t(j)], c = j;
        if (best == 0) return;
        cover(c);
        for (int r = D[std::size_t(c)]; r != c && !stop; r = D[std::size_t(r)]) {
            if (budget > 0 && ++tried > budget) {
                stop = true;
                break;
            }
            chosen[std::size_t(k)] = row[std::size_t(r)];
            for (int j = R[std::size_t(r)]; j != r; j = R[std::size_t(j)]) cover(C[std::size_t(j)]);
            search(k + 1);
            for (int j = L[std::size_t(r)]; j != r; j = L[std::size_t(j)]) uncover(C[std::size_t(j)]);
        }
        uncover(c);
    }
};

Solver::Solver() : d_(std::make_unique<Dlx>()) {}
Solver::~Solver() = default;

int Solver::rows() const { return d_->nrows; }

bool Solver::solve(const Figure& fig, uint64_t seed, Solution& out, long budget) {
    if (!d_->build(fig, seed, true)) return false;
    d_->budget = std::max(1L, budget);
    d_->tried = 0;
    d_->stop = false;
    d_->firstOnly = true;
    d_->found = 0;
    d_->fn = nullptr;
    d_->search(0);
    if (d_->found == 0) return false;
    out = d_->first;
    return true;
}

long long Solver::enumerate(const Figure& fig, bool (*fn)(const Solution&, void*), void* ctx) {
    return enumerate(fig, fn, ctx, 0, false, 0);
}

long long Solver::enumerate(const Figure& fig, bool (*fn)(const Solution&, void*), void* ctx, uint64_t seed,
                            bool shuffle, long budget) {
    if (!d_->build(fig, seed, shuffle)) return 0;
    d_->budget = std::max(0L, budget);
    d_->tried = 0;
    d_->stop = false;
    d_->firstOnly = false;
    d_->found = 0;
    d_->fn = fn;
    d_->ctx = ctx;
    d_->search(0);
    return d_->found;
}

// ---- assembly ---------------------------------------------------------------------------------------------------

namespace {

struct Assembly {
    std::array<std::array<Cell, kMaxPieceCells>, kPieces> cells{};
    std::array<int, kPieces> count{};
    std::array<double, kPieces> rank{};  // bottom-up preference (lower first), with the seed's tie-break
    std::array<uint8_t, kGrid * kGrid * kGrid> occ{};
    std::array<int8_t, kPieces> order{};
    std::array<Approach, kPieces> approach{};
    bool strict = true;

    bool occupied(int x, int y, int z) const { return inGrid(x, y, z) && occ[std::size_t((x * kGrid + y) * kGrid + z)]; }

    bool supported(int p) const {
        bool touch = false;
        for (int i = 0; i < count[std::size_t(p)]; ++i) {
            const Cell& c = cells[std::size_t(p)][std::size_t(i)];
            if (c.y == 0 || occupied(c.x, c.y - 1, c.z)) return true;
            touch = touch || occupied(c.x + 1, c.y, c.z) || occupied(c.x - 1, c.y, c.z) || occupied(c.x, c.y + 1, c.z) ||
                    occupied(c.x, c.y, c.z + 1) || occupied(c.x, c.y, c.z - 1);
        }
        return !strict && touch;
    }
    // A clear way in: nothing placed above any cell (dropped from above), else nothing beside along one direction.
    bool way(int p, Approach& a) const {
        static constexpr int kDir[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        auto clear = [&](int dx, int dy, int dz) {
            for (int i = 0; i < count[std::size_t(p)]; ++i) {
                const Cell& c = cells[std::size_t(p)][std::size_t(i)];
                for (int k = 1; k < kGrid; ++k)
                    if (occupied(c.x + dx * k, c.y + dy * k, c.z + dz * k)) return false;
            }
            return true;
        };
        if (clear(0, 1, 0)) {
            a = Approach::Above;
            return true;
        }
        for (int d = 0; d < 4; ++d)
            if (clear(kDir[d][0], 0, kDir[d][1])) {
                a = Approach(d + 1);
                return true;
            }
        return false;
    }
    void mark(int p, uint8_t v) {
        for (int i = 0; i < count[std::size_t(p)]; ++i) occ[std::size_t(cellIndex(cells[std::size_t(p)][std::size_t(i)]))] = v;
    }
    bool dfs(int depth, int used) {
        if (depth == kPieces) return true;
        int cand[kPieces], n = 0;
        Approach ways[kPieces];
        for (int p = 0; p < kPieces; ++p) {
            if (used & (1 << p)) continue;
            Approach a{};
            if (!supported(p) || !way(p, a)) continue;
            ways[p] = a;
            cand[n++] = p;
        }
        std::sort(cand, cand + n, [&](int a, int b) { return rank[std::size_t(a)] < rank[std::size_t(b)]; });
        for (int i = 0; i < n; ++i) {
            const int p = cand[i];
            order[std::size_t(depth)] = int8_t(p);
            approach[std::size_t(p)] = ways[p];
            mark(p, 1);
            if (dfs(depth + 1, used | (1 << p))) return true;
            mark(p, 0);
        }
        return false;
    }
};

}  // namespace

bool assemble(const Figure& fig, const Solution& s, uint64_t seed, Plan& plan) {
    Assembly a;
    for (int p = 0; p < kPieces; ++p) {
        a.count[std::size_t(p)] = pieces()[std::size_t(p)].count;
        placedCells(p, s[std::size_t(p)], a.cells[std::size_t(p)].data());
        double miny = 99, meany = 0;
        for (int i = 0; i < a.count[std::size_t(p)]; ++i) {
            const Cell& c = a.cells[std::size_t(p)][std::size_t(i)];
            if (!inGrid(c.x, c.y, c.z)) return false;
            miny = std::min(miny, double(c.y));
            meany += c.y;
        }
        meany /= a.count[std::size_t(p)];
        a.rank[std::size_t(p)] = miny * 10 + meany * 2 + 3 * unit(hash2(seed, uint64_t(p)));
    }
    for (int pass = 0; pass < 2; ++pass) {
        a.strict = pass == 0;
        a.occ.fill(0);
        if (a.dfs(0, 0)) {
            plan.solution = s;
            plan.order = a.order;
            plan.approach = a.approach;
            plan.resting = a.strict;
            plan.center = {float(fig.size.x - 1) * 0.5f, float(fig.size.y - 1) * 0.5f, float(fig.size.z - 1) * 0.5f};
            return true;
        }
    }
    return false;
}

bool makePlan(Solver& solver, int figureIndex, uint64_t seed, Plan& out, double ring) {
    const Figure& fig = figure(figureIndex);
    const int fi = std::clamp(figureIndex, 0, figureCount() - 1);
    if (!(std::isfinite(ring) && ring > 0)) ring = ringRadius({fi});
    // The solutions of one search with the placements in an order shuffled by `seed` (cheaper than a new search
    // for each), until one assembles resting with every piece dropped in from above and its burst keeps the pieces
    // apart. The best kept so far: 3 = that; 2 = resting with a clear burst (some slide in); 1 = resting; 0 = any.
    struct Ctx {
        const Figure* fig;
        int fi;
        double ring;
        uint64_t seed;
        int seen = 0, bestScore = -1;
        Plan best{};
    } ctx{&fig, fi, ring, seed, 0, -1, Plan{}};
    solver.enumerate(fig, [](const Solution& s, void* v) {
        auto& c = *static_cast<Ctx*>(v);
        Plan p;
        p.figure = c.fi;
        p.ring = float(c.ring);
        const uint64_t sd = hash2(c.seed, uint64_t(c.seen));
        if (assemble(*c.fig, s, sd, p)) {
            bool above = true;
            for (Approach a : p.approach) above = above && a == Approach::Above;
            int score = p.resting ? 1 : 0;
            if (p.resting && above && chooseBurst(p)) score = 3;
            if (score > c.bestScore) c.best = p, c.bestScore = score;
        }
        return ++c.seen < 64 && c.bestScore < 3;
    }, &ctx, hash2(seed, 0x5EED), true, 4'000'000);
    Plan best = ctx.best;
    int bestScore = ctx.bestScore;
    if (bestScore == 1 && chooseBurst(best)) bestScore = 2;
    if (bestScore < 0) return false;
    out = best;
    return true;
}

// ---- timing and frames ----------------------------------------------------------------------------------------------

Timing clampTiming(Timing t) {
    const Timing d;
    auto ok = [](double v, double fb) { return std::isfinite(v) ? v : fb; };
    t.len = std::clamp(ok(t.len, d.len), 8.0, 1024.0);
    t.burst = std::clamp(ok(t.burst, d.burst), 0.25, 4.0);
    t.hold = std::clamp(ok(t.hold, d.hold), 0.0, t.len);
    t.each = ok(t.each, d.each);
    // The build (6 gaps + the first flight) must fit after the hold and the burst.
    const double room = t.len - t.burst;
    const double maxEach = (room - 0.5) / 7.5;
    t.each = std::clamp(t.each, 0.25, std::max(0.25, maxEach));
    const double buildStart = t.len - 6 * t.each - flightBeats(t);
    t.hold = std::clamp(t.hold, 0.0, std::max(0.0, buildStart - t.burst));
    return t;
}

double lockBeat(const Timing& t, int k) { return t.len - double(kPieces - 1 - std::clamp(k, 0, kPieces - 1)) * t.each; }
double flightBeats(const Timing& t) { return std::max(t.each, std::min(1.5, 1.5 * t.each)); }

namespace {

constexpr double kOmega = 0.45;      // tumbling, radians per beat (x .7..1.3)
constexpr double kRingSpin = 0.05;   // the ring turning about the figure's vertical, radians per beat
constexpr double kMargin = 0.3;      // clearance kept between the pieces' bounding spheres (cells)
constexpr double kBob = 0.15;        // hovering pieces bob up and down by this much
constexpr double kTremble = 0.07;    // ... and tremble by this much at full tension (Tens)
constexpr double kPull = 0.25;       // ... and the ring pulls in by this much at full tension
constexpr double kExplode = 1.0;     // Expl = 1 moves a placed piece out by about this much (cells)
constexpr double kExplodeU = 0.04;   // ... as this much of its (checked) burst path
// The crane path of a flight, as fractions of it: rise beside the ring (turning into place), cross over the top,
// then drop in (or come down beside the figure and slide in).
constexpr double kRiseEnd = 0.3, kCrossEnd = 0.55, kSideDropEnd = 0.75;

struct Pose {
    Q q;
    V3 c;  // the piece's centroid, stage coordinates
};

Pose placedPose(const Plan& plan, int p) {
    const Placement& pl = plan.solution[std::size_t(p)];
    const Rot& r = rotations()[std::size_t(std::clamp(int(pl.rot), 0, kRotations - 1))];
    const V3 cl = centroid(p);
    const V3 rc{r[0] * cl.x + r[1] * cl.y + r[2] * cl.z, r[3] * cl.x + r[4] * cl.y + r[5] * cl.z,
                r[6] * cl.x + r[7] * cl.y + r[8] * cl.z};
    Pose o;
    o.q = qfromRot(r);
    o.c = {rc.x + pl.offset.x - plan.center[0], rc.y + pl.offset.y - plan.center[1], rc.z + pl.offset.z - plan.center[2]};
    return o;
}

// Half the figure's horizontal diagonal (to its outer corners) and half its height (cells, from the stage center).
double reach(const Figure& f) { return std::hypot(0.5 * f.size.x, 0.5 * f.size.z); }
double halfHeight(int figureIndex) { return 0.5 * figure(figureIndex).size.y; }

V3 approachDir(Approach a) {
    switch (a) {
        case Approach::Above: return {0, 1, 0};
        case Approach::FromMinusX: return {-1, 0, 0};
        case Approach::FromPlusX: return {1, 0, 0};
        case Approach::FromMinusZ: return {0, 0, -1};
        case Approach::FromPlusZ: return {0, 0, 1};
    }
    return {0, 1, 0};
}

// The exploded view: a flat ring of hovering pieces around the figure, a little below it (in the figure's own
// space, so every clearance holds whatever the camera does; seen from above at 3/4 the ring frames the figure and
// the front pieces stay below it). Its radius keeps every piece's bounding sphere outside a cylinder around the
// figure (with room for Expl, Tens and the margin) and apart from its neighbors; each piece has a slot, in the order
// of the pieces' angles around the figure's vertical, so the burst sends each out on its own side.
struct Ring {
    std::array<double, kPieces> angle{};
    std::array<V3, kPieces> axis{};
    std::array<double, kPieces> omega{};
    double radius = 6, y = -2;
};

Ring ringFor(const Plan& prev) {
    Ring g;
    g.radius = std::isfinite(prev.ring) && prev.ring > 0 ? prev.ring : ringRadius({prev.figure});
    g.y = -halfHeight(prev.figure) - 0.5;
    std::array<double, kPieces> th{};
    std::array<int, kPieces> idx{};
    for (int p = 0; p < kPieces; ++p) {
        const V3 c = placedPose(prev, p).c;
        const uint64_t hh = hash2(uint64_t(prev.figure) * 977 + 13, uint64_t(p));
        th[std::size_t(p)] = std::hypot(c.x, c.z) > 0.3 ? std::atan2(c.z, c.x) : 2 * kPi * unit(hh);
        idx[std::size_t(p)] = p;
        const double u = 2 * unit(hash2(hh, 1)) - 1, phi = 2 * kPi * unit(hash2(hh, 2));
        const double sn = std::sqrt(std::max(0.0, 1 - u * u));
        g.axis[std::size_t(p)] = {sn * std::cos(phi), u, sn * std::sin(phi)};
        g.omega[std::size_t(p)] = kOmega * (0.7 + 0.6 * unit(hash2(hh, 3))) * (p % 2 ? 1 : -1);
    }
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return th[std::size_t(a)] < th[std::size_t(b)]; });
    // Evenly spaced slots, turned to sit as close as possible to where the pieces were (circular mean).
    double sx = 0, sy = 0;
    for (int k = 0; k < kPieces; ++k) {
        const double d = th[std::size_t(idx[std::size_t(k)])] - 2 * kPi * k / kPieces;
        sx += std::cos(d);
        sy += std::sin(d);
    }
    const double base = std::atan2(sy, sx);
    for (int k = 0; k < kPieces; ++k) g.angle[std::size_t(idx[std::size_t(k)])] = base + 2 * kPi * k / kPieces;
    return g;
}

// A piece's slot when the burst ends (its hovering starts there).
V3 slotAt(const Ring& g, int p) {
    const double a = g.angle[std::size_t(p)];
    return {g.radius * std::cos(a), g.y, g.radius * std::sin(a)};
}

// A hovering piece at beat t (t >= b1, the burst's end): its slot turning slowly, bobbing, tumbling about its own
// axis; Tens pulls the ring in a little and makes it tremble. All of it eases in from the end of the burst.
Pose hoverPose(const Ring& g, const Pose& from, int p, double t, double b1, double tension) {
    const double tau = std::max(0.0, t - b1);
    const double in = smooth(tau);  // the bob, the tremble and the pull ease in over the first beat
    const double a = g.angle[std::size_t(p)] + kRingSpin * tau;
    const double r = g.radius - kPull * tension * in;
    V3 c{r * std::cos(a), g.y + kBob * in * std::sin(0.8 * tau + 1.7 * p), r * std::sin(a)};
    if (tension > 0) {
        const double f = 2 * kPi * 3;
        c = c + V3{std::sin(f * t + 1.3 * p), std::sin(1.23 * f * t + 2.1 * p), std::sin(0.91 * f * t + 0.7 * p)} *
                    (kTremble * tension * in);
    }
    Pose o;
    o.c = c;
    const V3& ax = g.axis[std::size_t(p)];
    // Tumbling speeds up from rest at the burst's end (angle = omega (tau - (1 - e^-tau))).
    o.q = qmul(qaxis(ax.x, ax.y, ax.z, g.omega[std::size_t(p)] * (tau - (1 - std::exp(-tau)))), from.q);
    return o;
}

// The burst (as a fraction u of it): piece p from its place in `plan` out to its slot, keeping its orientation.
// Modes 0..11 go straight to the slot (lift .8 / 0 / 2 on the way; x the stagger: together, bottom first, top first,
// outer first). Modes 12..35 first push the piece out along its own direction from the figure's center (an exploded
// view: up-bias 0 / 1 / 2, 2.5 or 4 cells; x the stagger: together, top first, outer first, bottom first), then on
// to the slot. makePlan keeps the first mode under which no two pieces ever meet (checked cube by cube).
constexpr int kBurstModes = 36;
// What the burst of a plan needs, worked out once: where each piece starts and ends, its rank for each stagger order
// (1 bottom first, 2 top first, 3 outer first) and its cells.
struct BurstGeo {
    std::array<V3, kPieces> from{}, to{};
    std::array<std::array<int, kPieces>, 4> rank{};
    std::array<std::array<Cell, kMaxPieceCells>, kPieces> cells{};
};
BurstGeo burstGeo(const Plan& plan, const Ring& g) {
    BurstGeo b;
    for (int p = 0; p < kPieces; ++p) {
        b.from[std::size_t(p)] = placedPose(plan, p).c;
        b.to[std::size_t(p)] = slotAt(g, p);
        placedCells(p, plan.solution[std::size_t(p)], b.cells[std::size_t(p)].data());
    }
    for (int kind = 1; kind <= 3; ++kind) {
        auto key = [&](int q) {
            const V3& c = b.from[std::size_t(q)];
            return kind == 1 ? c.y : kind == 2 ? -c.y : -std::hypot(c.x, c.z);
        };
        for (int p = 0; p < kPieces; ++p) {
            int r = 0;
            for (int q = 0; q < kPieces; ++q) r += key(q) < key(p) || (key(q) == key(p) && q < p);
            b.rank[std::size_t(kind)][std::size_t(p)] = r;
        }
    }
    return b;
}

V3 burstAt(const BurstGeo& b, int p, double u, int mode) {
    mode = std::clamp(mode, 0, kBurstModes - 1);
    const bool radial = mode >= 12;
    const int m = radial ? mode - 12 : mode;
    const int stagger = radial ? m / 6 : m / 3;
    // straight: 0 none, 1 bottom, 2 top, 3 outer first; radial: 0 none, 1 top, 2 outer, 3 bottom first
    const int kind = radial ? (stagger == 1 ? 2 : stagger == 2 ? 3 : 1) : stagger;
    const double delay = stagger > 0 ? 0.06 * b.rank[std::size_t(kind)][std::size_t(p)] : 0.0;  // the last at .36
    const double v = std::clamp((u - delay) / (stagger > 0 ? 1 - 0.36 : 1.0), 0.0, 1.0);
    const V3 from = b.from[std::size_t(p)], to = b.to[std::size_t(p)];
    if (!radial) {
        static constexpr double kLift[3] = {0.8, 0.0, 2.0};
        const double e = 1 - (1 - v) * (1 - v) * (1 - v);
        return from + (to - from) * e + V3{0, kLift[m % 3] * std::sin(kPi * v), 0};
    }
    static constexpr double kUp[3] = {0.0, 1.0, 2.0};
    const double up = kUp[m % 3], dist = (m / 3) % 2 ? 4.0 : 2.5;
    V3 n = from + V3{0, up, 0};
    const double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    n = l > 1e-6 ? n * (1.0 / l) : V3{0, 1, 0};
    const V3 out = from + n * dist;
    if (v < 0.45) {
        const double w = v / 0.45, e = 1 - (1 - w) * (1 - w) * (1 - w);
        return from + (out - from) * e;
    }
    return out + (to - out) * smooth((v - 0.45) / 0.55);
}

bool burstClear(const BurstGeo& b, int mode) {
    constexpr int kSteps = 128;
    constexpr double kTol = 0.02;
    // Coarse to fine (every 16th step, every 4th, then the rest), so a mode that fails fails early.
    static const std::array<int, kSteps> kOrder = [] {
        std::array<int, kSteps> o{};
        std::array<bool, kSteps + 1> used{};
        int n = 0;
        for (int stride : {16, 4, 1})
            for (int st = stride; st <= kSteps; st += stride)
                if (!used[std::size_t(st)]) used[std::size_t(st)] = true, o[std::size_t(n++)] = st;
        return o;
    }();
    for (int st : kOrder) {
        const double u = double(st) / kSteps;
        std::array<V3, kPieces> d;  // how far each piece has moved
        for (int p = 0; p < kPieces; ++p) d[std::size_t(p)] = burstAt(b, p, u, mode) - b.from[std::size_t(p)];
        for (int a = 0; a < kPieces; ++a)
            for (int c = a + 1; c < kPieces; ++c) {
                const V3 e = (b.from[std::size_t(a)] + d[std::size_t(a)]) - (b.from[std::size_t(c)] + d[std::size_t(c)]);
                if (e.x * e.x + e.y * e.y + e.z * e.z > 16.0) continue;  // farther apart than two bounding spheres
                const V3 off = d[std::size_t(a)] - d[std::size_t(c)];
                for (int i = 0; i < pieces()[std::size_t(a)].count; ++i)
                    for (int j = 0; j < pieces()[std::size_t(c)].count; ++j) {
                        const Cell& x = b.cells[std::size_t(a)][std::size_t(i)];
                        const Cell& y = b.cells[std::size_t(c)][std::size_t(j)];
                        if (std::fabs(x.x - y.x + off.x) < 1 - kTol && std::fabs(x.y - y.y + off.y) < 1 - kTol &&
                            std::fabs(x.z - y.z + off.z) < 1 - kTol)
                            return false;
                    }
            }
    }
    return true;
}

void store(PieceFrame& out, int p, const Pose& pose) {
    const auto m = qmat(pose.q);
    const V3 cl = centroid(p);
    const V3 rc = mulm(m, cl);
    for (int k = 0; k < 9; ++k) out.rot[std::size_t(k)] = float(m[std::size_t(k)]);
    out.pos = {float(pose.c.x - rc.x), float(pose.c.y - rc.y), float(pose.c.z - rc.z)};
}

}  // namespace

bool chooseBurst(Plan& plan) {
    const BurstGeo geo = burstGeo(plan, ringFor(plan));
    for (int i = 0; i < kBurstModes; ++i) {
        const int mode = (i + 12) % kBurstModes;  // the exploded views first: they clear most often
        if (burstClear(geo, mode)) {
            plan.burst = int8_t(mode);
            return true;
        }
    }
    plan.burst = -1;
    return false;
}

double pieceRadius(int piece) {
    static const std::array<double, kPieces> kR = [] {
        std::array<double, kPieces> r{};
        for (int p = 0; p < kPieces; ++p) {
            const V3 c = centroid(p);
            const Piece& pc = pieces()[std::size_t(p)];
            for (int i = 0; i < pc.count; ++i)
                for (int k = 0; k < 8; ++k) {
                    const V3 corner{pc.cells[std::size_t(i)].x + (k & 1 ? 0.5 : -0.5),
                                    pc.cells[std::size_t(i)].y + (k & 2 ? 0.5 : -0.5),
                                    pc.cells[std::size_t(i)].z + (k & 4 ? 0.5 : -0.5)};
                    const V3 d = corner - c;
                    r[std::size_t(p)] = std::max(r[std::size_t(p)], std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
                }
        }
        return r;
    }();
    return kR[std::size_t(std::clamp(piece, 0, kPieces - 1))];
}

double ringRadius(const std::vector<int>& figures) {
    double rp = 0;
    for (int p = 0; p < kPieces; ++p) rp = std::max(rp, pieceRadius(p));
    double far = 0;
    for (int f : figures) far = std::max(far, reach(figure(f)));
    if (figures.empty()) far = reach(figure(0));
    // Outside the cylinder around the figure (whose pieces Expl may move out by kExplode), even pulled in and
    // trembling; and seven slots apart by two bounding spheres (chord = 2 R sin(pi / 7)).
    const double cylinder = far + kExplode + rp + kMargin + kPull + kTremble;
    const double spacing = (2 * rp + kMargin + 2 * kTremble) / (2 * std::sin(kPi / kPieces)) + kPull;
    return std::max(cylinder, spacing);
}

Frame frameAt(const Plan& prev, const Plan& cur, double turn, const Timing& timing, double explode, double tension,
              const View& view) {
    (void)view;  // the halo used to sit in view space; it is in figure space since every clearance is checked there
    const Timing tm = clampTiming(timing);
    Frame f;
    const double t = std::isfinite(turn) ? std::clamp(turn, 0.0, tm.len) : tm.len;
    f.turn = t;
    explode = std::isfinite(explode) ? std::clamp(explode, 0.0, 1.0) : 0.0;
    tension = std::isfinite(tension) ? std::clamp(tension, 0.0, 1.0) : 0.0;
    const double b0 = tm.hold, b1 = tm.hold + tm.burst;
    const double fl = flightBeats(tm);
    std::array<double, kPieces> lock{};
    for (int k = 0; k < kPieces; ++k) {
        const int p = std::clamp(int(cur.order[std::size_t(k)]), 0, kPieces - 1);
        lock[std::size_t(p)] = lockBeat(tm, k);
    }
    const Ring ring = ringFor(prev);
    const Ring next = ringFor(cur);  // where the current figure's pieces would burst to (Expl follows those paths)
    const BurstGeo prevGeo = burstGeo(prev, ring), curGeo = burstGeo(cur, next);
    const double lastLock = lockBeat(tm, kPieces - 1);
    // The previous figure completed on the line (beat 0); this one completes on its last lock.
    const double glow = t < b0 ? 0.5 * std::exp(-t) : t >= lastLock ? 0.5 * std::exp(-(t - lastLock)) : 0.0;
    double thump = t < b0 ? std::exp(-5 * t) : 0.0;
    if (t < b0) f.phase = Phase::Hold;
    else if (t < b1) f.phase = Phase::Burst;
    else if (t < lockBeat(tm, 0) - fl) f.phase = Phase::Hover;
    else f.phase = Phase::Build;
    f.figure = t < b0 ? prev.figure : cur.figure;
    const int prevLast = std::clamp(int(prev.order[kPieces - 1]), 0, kPieces - 1);
    const int prevBurst = std::max(0, int(prev.burst)), curBurst = std::max(0, int(cur.burst));
    // The crossing height: above the figure being built (and anything Expl lifts out of it) by a bounding sphere.
    const double top = halfHeight(cur.figure) + kExplode;
    for (int p = 0; p < kPieces; ++p) {
        PieceFrame& pf = f.pieces[std::size_t(p)];
        const Pose from = placedPose(prev, p);
        Pose pose;
        if (t < b0) {
            pose = from;
            if (explode > 0) pose.c = burstAt(prevGeo, p, kExplodeU * explode, prevBurst);
            pf.placed = true;
            pf.flash = float(std::max(glow, p == prevLast ? std::exp(-2.5 * t) : 0.0));
        } else if (t < b1) {
            // The burst: straight out to the slot, fast at first, keeping the orientation (checked when planned).
            pose.c = burstAt(prevGeo, p, (t - b0) / tm.burst, prevBurst);
            pose.q = from.q;
            pf.state = PieceState::Burst;
        } else {
            const double L = lock[std::size_t(p)], F = L - fl;
            const Pose target = placedPose(cur, p);
            if (t >= L) {
                pose = target;
                if (explode > 0) pose.c = burstAt(curGeo, p, kExplodeU * explode, curBurst);
                pf.placed = true;
                pf.flash = float(std::max(glow, std::exp(-2.5 * (t - L))));
                thump = std::max(thump, std::exp(-5 * (t - L)));
                ++f.placed;
            } else if (t < F) {
                pose = hoverPose(ring, from, p, t, b1, tension);
                pf.state = PieceState::Hover;
            } else {
                // The flight, a crane's path: rise straight up beside the ring while turning into place (the ring
                // keeps every piece's sphere clear of the others' and of the figure), cross over the top (above the
                // figure by a bounding sphere), then drop straight down into place, accelerating into the lock (its
                // column is clear: the assembly order), or come down beside the figure and slide in.
                const Pose start = hoverPose(ring, from, p, F, b1, tension);
                const double ytop = top + pieceRadius(p) + kMargin;
                const V3 dir = approachDir(cur.approach[std::size_t(p)]);
                V3 entry = target.c;  // straight above the place, or beside the figure along `dir`
                if (cur.approach[std::size_t(p)] != Approach::Above) {
                    Cell cells[kMaxPieceCells];
                    placedCells(p, cur.solution[std::size_t(p)], cells);
                    const Figure& fig = figure(cur.figure);
                    double minAlong = 1e9, figMax = -1e9;
                    for (int i = 0; i < pieces()[std::size_t(p)].count; ++i)
                        minAlong = std::min(minAlong, cells[i].x * dir.x + cells[i].z * dir.z);
                    for (int i = 0; i < fig.count; ++i)
                        figMax = std::max(figMax, fig.cells[std::size_t(i)].x * dir.x + fig.cells[std::size_t(i)].z * dir.z);
                    entry = target.c + dir * (figMax + 1 + kExplode - minAlong);
                }
                const double u = (t - F) / fl;
                const V3 high{start.c.x, ytop, start.c.z}, over{entry.x, ytop, entry.z};
                if (u < kRiseEnd) {
                    const double s = smooth(u / kRiseEnd);
                    pose.c = start.c + (high - start.c) * s;
                    pose.q = qslerp(start.q, target.q, s);
                    pf.state = PieceState::Flight;
                } else if (u < kCrossEnd) {
                    pose.c = high + (over - high) * smooth((u - kRiseEnd) / (kCrossEnd - kRiseEnd));
                    pose.q = target.q;
                    pf.state = PieceState::Flight;
                } else if (cur.approach[std::size_t(p)] == Approach::Above) {
                    const double v = (u - kCrossEnd) / (1 - kCrossEnd);
                    pose.c = over + (target.c - over) * (v * v);
                    pose.q = target.q;
                    pf.state = PieceState::Approach;
                } else if (u < kSideDropEnd) {
                    pose.c = over + (entry - over) * smooth((u - kCrossEnd) / (kSideDropEnd - kCrossEnd));
                    pose.q = target.q;
                    pf.state = PieceState::Approach;
                } else {
                    const double v = (u - kSideDropEnd) / (1 - kSideDropEnd);
                    pose.c = entry + (target.c - entry) * (v * v);
                    pose.q = target.q;
                    pf.state = PieceState::Approach;
                }
                // Coming into the figure it moves with it (Expl), as it will once placed.
                if (pf.state == PieceState::Approach && explode > 0)
                    pose.c = pose.c + (burstAt(curGeo, p, kExplodeU * explode, curBurst) - target.c);
            }
        }
        store(pf, p, pose);
    }
    f.thump = float(thump);
    // The figure dips a little with each lock.
    for (auto& pf : f.pieces)
        if (pf.placed) pf.pos[1] -= float(0.08 * thump);
    return f;
}

// ---- player -------------------------------------------------------------------------------------------------------

Player::Player(std::vector<int> figures) : solver_(std::make_unique<Solver>()), figs_(std::move(figures)) {
    if (figs_.empty()) figs_.push_back(0);
    for (int& f : figs_) f = std::clamp(f, 0, figureCount() - 1);
    ring_ = ringRadius(figs_);
}

Plan Player::plan(int64_t round) {
    ++clock_;
    for (Entry& e : cache_)
        if (e.valid && e.round == round) {
            e.used = clock_;
            return e.plan;
        }
    Entry* slot = &cache_[0];
    for (Entry& e : cache_)
        if (!e.valid || e.used < slot->used) slot = &e;
    const int64_t n = int64_t(figs_.size());
    const int fig = figs_[std::size_t(((round % n) + n) % n)];
    Plan p;
    if (!makePlan(*solver_, fig, hash2(uint64_t(round), 0x50AA), p, ring_)) makePlan(*solver_, 0, uint64_t(round), p, ring_);
    ++solves_;
    slot->round = round;
    slot->valid = true;
    slot->used = clock_;
    slot->plan = p;
    return p;
}

Frame Player::frame(double seed, double turn, const Timing& timing, double explode, double tension, const View& view) {
    double s = std::isfinite(seed) ? std::floor(std::fabs(seed)) : 0.0;
    s = std::min(s, 1e15);
    const int64_t round = int64_t(s);
    const Plan prev = plan(round - 1);
    const Plan cur = plan(round);
    return frameAt(prev, cur, turn, timing, explode, tension, view);
}

}  // namespace reforce::engine::soma
