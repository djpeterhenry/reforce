// reforce extension (docs/3d-design.md "Jigsaw"): the jigsaw cut, one source for C++ (engine/Jigsaw.cpp, the tests)
// and Metal (render/JigsawPipeline.cpp, the board and piece shaders), so both cut exactly the same pieces.
//
// Included with JIGSAW_SHAPE_SOURCE(...) defined either as the code itself (C++, inside a namespace that provides
// float2, uint, length, dot, abs, min, max, clamp, sin, floor) or as #__VA_ARGS__ (a string for the shader compiler).
// So: no preprocessor lines and no string literals in here, only the subset of C++ that is also Metal.
//
// Board units: a cell is 1 high and `cw` wide; the frame is [0, cols * cw] x [0, rows], y down.
//   - Corners: a grid jittered by up to 7.5% of a cell (border corners only along their border; frame corners fixed).
//   - Edge field F(p) of the edge between two corners: the signed distance to its line (< 0 on the "-" side: above a
//     horizontal edge, left of a vertical one), bent by a gentle wave that is 0 at both corners, and with a knob (tab)
//     in its middle: a round head on a narrower neck, smoothly joined, protruding into one side (random). A tab on
//     the "+" side is smin(F, K) (the knob belongs to the "-" piece), on the "-" side smax(F, -K). Each piece uses
//     F of its four edges with the sign of its side, so a tab and its neighbor's blank are the same curve: the pieces
//     tile the frame exactly (no gaps, no overlaps).
//   - Piece field: the max of its four signed edge fields (< 0 inside; about the distance to its outline).
//   - Border edges are straight. Knobs stay in the middle 50% of an edge and reach at most a third of a cell, so they
//     never meet each other or a corner.
JIGSAW_SHAPE_SOURCE(
struct JsGrid { int cols; int rows; float cw; uint seed; };
struct JsHit { int index; float g; int index2; float g2; };

inline uint js_hash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

inline float js_rand(uint seed, uint a, uint b, uint salt) {
    const uint h = js_hash(seed ^ (a * 0x9E3779B1u) ^ (b * 0x85EBCA77u) ^ (salt * 0xC2B2AE3Du));
    return float(h >> 8u) * (1.0f / 16777216.0f);
}

inline float js_smin(float a, float b, float k) {
    const float h = max(k - abs(a - b), 0.0f) / k;
    return min(a, b) - h * h * k * 0.25f;
}

inline float js_box(float2 p, float2 c, float2 hs) {
    const float2 d = abs(p - c) - hs;
    return length(max(d, float2(0.0f, 0.0f))) + min(max(d.x, d.y), 0.0f);
}

inline float2 js_corner(JsGrid g, int i, int j) {
    float x = float(i) * g.cw;
    float y = float(j);
    if (i > 0 && i < g.cols) x += (js_rand(g.seed, uint(i), uint(j), 1u) * 2.0f - 1.0f) * 0.075f * g.cw;
    if (j > 0 && j < g.rows) y += (js_rand(g.seed, uint(i), uint(j), 2u) * 2.0f - 1.0f) * 0.075f;
    return float2(x, y);
}

inline float js_edge(JsGrid g, int horiz, int i, int j, float2 p) {
    const float2 a = js_corner(g, i, j);
    const float2 b = horiz != 0 ? js_corner(g, i + 1, j) : js_corner(g, i, j + 1);
    const float2 dv = b - a;
    const float L = max(length(dv), 1e-4f);
    const float2 dir = dv / L;
    const float2 n = horiz != 0 ? float2(-dir.y, dir.x) : float2(dir.y, -dir.x);
    const float2 r = p - a;
    float s = dot(r, n);
    const bool border = horiz != 0 ? (j <= 0 || j >= g.rows) : (i <= 0 || i >= g.cols);
    if (border) return s;
    const float u = dot(r, dir);
    const float t = clamp(u / L, 0.0f, 1.0f);
    const uint salt = horiz != 0 ? 16u : 48u;
    const uint ei = uint(i);
    const uint ej = uint(j);
    const float S = min(g.cw, 1.0f);
    const float w1 = (js_rand(g.seed, ei, ej, salt + 1u) * 2.0f - 1.0f) * 0.035f * S;
    const float w2 = (js_rand(g.seed, ei, ej, salt + 2u) * 2.0f - 1.0f) * 0.018f * S;
    s -= w1 * sin(3.14159265f * t) + w2 * sin(6.28318531f * t);
    const float side = js_rand(g.seed, ei, ej, salt + 3u) < 0.5f ? -1.0f : 1.0f;
    const float uc = L * (0.5f + (js_rand(g.seed, ei, ej, salt + 4u) - 0.5f) * 0.12f);
    const float kb = 0.05f * S;
    // Far from the knob (a circle of .32 S around it holds all of it; its smooth union lowers K by at most .015 S):
    // K >= |s| + kb, so the result is exactly s. Most pixels end here.
    const float lb = length(float2(u - uc, side * s - 0.18f * S)) - 0.335f * S;
    if (lb >= abs(s) + kb) return s;
    const float rad = S * 0.135f * (0.88f + 0.24f * js_rand(g.seed, ei, ej, salt + 5u));
    const float hd = S * 0.185f * (0.9f + 0.2f * js_rand(g.seed, ei, ej, salt + 6u));
    const float skew = S * 0.05f * (js_rand(g.seed, ei, ej, salt + 7u) - 0.5f);
    const float nw = rad * (0.58f + 0.1f * js_rand(g.seed, ei, ej, salt + 8u));
    const float2 q = float2(u - uc, side * s);
    const float head = length(q - float2(skew, hd)) - rad;
    const float neck = js_box(q, float2(0.0f, 0.5f * hd - 0.05f * S), float2(nw, 0.5f * hd + 0.05f * S));
    const float K = js_smin(head, neck, 0.06f * S);
    return side > 0.0f ? js_smin(s, K, kb) : -js_smin(-s, K, kb);
}

inline float js_piece(JsGrid g, int col, int row, float2 p) {
    const float top = -js_edge(g, 1, col, row, p);
    const float bottom = js_edge(g, 1, col, row + 1, p);
    const float left = -js_edge(g, 0, col, row, p);
    const float right = js_edge(g, 0, col + 1, row, p);
    return max(max(top, bottom), max(left, right));
}

inline JsHit js_owner(JsGrid g, float2 p) {
    // The 3 x 3 pieces around p's cell share 24 edges: each is evaluated once.
    const int c0 = clamp(int(floor(p.x / g.cw)), 0, g.cols - 1);
    const int r0 = clamp(int(floor(p.y)), 0, g.rows - 1);
    float he[12];
    float ve[12];
    for (int k = 0; k < 12; ++k) {
        he[k] = 1e9f;
        ve[k] = 1e9f;
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 4; ++b) {
            const int hi = c0 - 1 + a;
            const int hj = r0 - 1 + b;
            if (hi >= 0 && hi < g.cols && hj >= 0 && hj <= g.rows) he[a * 4 + b] = js_edge(g, 1, hi, hj, p);
            const int vi = c0 - 1 + b;
            const int vj = r0 - 1 + a;
            if (vi >= 0 && vi <= g.cols && vj >= 0 && vj < g.rows) ve[b * 3 + a] = js_edge(g, 0, vi, vj, p);
        }
    }
    JsHit h;
    h.index = -1;
    h.g = 1e9f;
    h.index2 = -1;
    h.g2 = 1e9f;
    for (int dr = 0; dr < 3; ++dr) {
        for (int dc = 0; dc < 3; ++dc) {
            const int c = c0 - 1 + dc;
            const int r = r0 - 1 + dr;
            if (c < 0 || r < 0 || c >= g.cols || r >= g.rows) continue;
            const float v = max(max(-he[dc * 4 + dr], he[dc * 4 + dr + 1]),
                                max(-ve[dc * 3 + dr], ve[(dc + 1) * 3 + dr]));
            const int idx = r * g.cols + c;
            if (v < h.g) {
                h.index2 = h.index;
                h.g2 = h.g;
                h.index = idx;
                h.g = v;
            } else if (v < h.g2) {
                h.index2 = idx;
                h.g2 = v;
            }
        }
    }
    return h;
}
)
