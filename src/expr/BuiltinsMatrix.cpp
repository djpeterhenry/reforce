// VectorC matrix built-ins. Most of these are undocumented in [CP]; [VC] only
// gives their declarations. Semantics below are the spec's reading (§3.3)
// plus GUESSES where noted; each guess is marked so probes can settle it.

#include <algorithm>
#include <cmath>
#include <vector>

#include "expr/Builtins.hpp"
#include "expr/Vm.hpp"

namespace reforce::expr::mfn {

namespace {

// Size argument -> integer count in [1, limit]. NaN/negative -> 1.
int countArg(const Value& v, std::size_t limit = kMaxMatrixElements) {
    const double d = v.scalar();
    if (!(d >= 1.0)) return 1;
    if (d >= double(limit)) return int(limit);
    return int(d);  // truncate, like NUM
}

// Index argument -> [0, size). Negative indices count from the end (-1 = last).
// Out-of-range indices clamp.
int indexArg(double v, int size) {
    if (!std::isfinite(v)) return 0;
    long i = long(std::trunc(v));
    if (i < 0) i += size;
    if (i < 0) i = 0;
    if (i >= size) i = size - 1;
    return int(i);
}

Value columnOf(const Value& x, int c) {
    if (x.isScalar()) return x;
    auto out = std::make_shared<Mat>(x.rows(), 1);
    for (int r = 0; r < x.rows(); ++r) out->at(r, 0) = x.mat().at(r, c);
    return Value::fromMat(std::move(out));
}

double sampleRowLinear(const Mat& f, int r, double p, bool periodic) {
    const int n = f.cols;
    if (n == 1) return f.at(r, 0);
    if (periodic) {
        const double fl = std::floor(p);
        const double t = p - fl;
        long i0 = long(fl) % n;
        if (i0 < 0) i0 += n;
        const long i1 = (i0 + 1) % n;
        return f.at(r, int(i0)) * (1.0 - t) + f.at(r, int(i1)) * t;
    }
    p = std::clamp(p, 0.0, double(n - 1));
    const int i0 = std::min(int(p), n - 2);
    const double t = p - i0;
    return f.at(r, i0) * (1.0 - t) + f.at(r, i0 + 1) * t;
}

}  // namespace

// select(c, a, b): elementwise c ? a : b [VC]. JRM's select(ID, B2, update)
// keeps B2 for every instance except ID 0, consistent with "nonzero -> a".
Value select(EvalContext&, Value* a, int) {
    return mapTernary(a[0], a[1], a[2], [](double c, double x, double y) { return c != 0.0 ? x : y; });
}

// choice(x): a uniformly random column of x [VC] (an element for row vectors).
Value choice(EvalContext& ctx, Value* a, int) {
    const Value& x = a[0];
    if (x.isScalar()) return x;
    int c = int(ctx.rng.uniform01Exclusive() * x.cols());
    c = std::clamp(c, 0, x.cols() - 1);
    return columnOf(x, c);
}

// largest(x): the column whose top element is largest [VC].
Value largest(EvalContext&, Value* a, int) {
    const Value& x = a[0];
    if (x.isScalar()) return x;
    int best = 0;
    for (int c = 1; c < x.cols(); ++c)
        if (x.mat().at(0, c) > x.mat().at(0, best)) best = c;
    return columnOf(x, best);
}

// vector(N, v): a 1 x N row filled with v. GUESS for matrix v: rows(v) x N with
// column j = column (j mod cols(v)) of v (tiles a pattern across N columns).
Value vector(EvalContext&, Value* a, int) {
    const int n = countArg(a[0]);
    const Value& v = a[1];
    if (v.isScalar()) return Value::matrix(1, n, v.s_);
    Value out = Value::matrix(v.rows(), n);
    if (out.isScalar()) return Value(v.scalar());
    Mat& m = out.mutableMat();
    for (int r = 0; r < m.rows; ++r)
        for (int c = 0; c < m.cols; ++c) m.at(r, c) = v.at(r, c % v.cols());
    return out;
}

// vectorLR(N, l, r): N values from l to r inclusive, as a 1 x N row [VC].
// Column-vector l/r give one row per element (GUESS).
Value vectorLR(EvalContext&, Value* a, int) {
    const int n = countArg(a[0]);
    const int rows = broadcastDim(a[1].rows(), a[2].rows());
    Value out = Value::matrix(rows, n);
    if (out.isScalar()) return Value(a[1].scalar());
    Mat& m = out.mutableMat();
    for (int r = 0; r < m.rows; ++r) {
        const double l = a[1].at(r, 0), rr = a[2].at(r, 0);
        for (int c = 0; c < m.cols; ++c) m.at(r, c) = n == 1 ? l : l + (rr - l) * double(c) / double(n - 1);
    }
    return out;
}

Value rows(EvalContext&, Value* a, int) { return Value(double(a[0].rows())); }
Value cols(EvalContext&, Value* a, int) { return Value(double(a[0].cols())); }

Value transpose(EvalContext&, Value* a, int) {
    const Value& x = a[0];
    if (x.isScalar()) return x;
    auto out = std::make_shared<Mat>(x.cols(), x.rows());
    for (int r = 0; r < x.rows(); ++r)
        for (int c = 0; c < x.cols(); ++c) out->at(c, r) = x.mat().at(r, c);
    return Value::fromMat(std::move(out));
}

// subrange(x, c1, r1, c2, r2): inclusive block, zero-based; negative indices
// count from the right/bottom (-1 = last); clamped [VC].
Value subrange(EvalContext&, Value* a, int) {
    const Value& x = a[0];
    if (x.isScalar()) return x;
    int c1 = indexArg(a[1].scalar(), x.cols()), r1 = indexArg(a[2].scalar(), x.rows());
    int c2 = indexArg(a[3].scalar(), x.cols()), r2 = indexArg(a[4].scalar(), x.rows());
    if (c2 < c1) std::swap(c1, c2);
    if (r2 < r1) std::swap(r1, r2);
    auto out = std::make_shared<Mat>(r2 - r1 + 1, c2 - c1 + 1);
    for (int r = r1; r <= r2; ++r)
        for (int c = c1; c <= c2; ++c) out->at(r - r1, c - c1) = x.mat().at(r, c);
    return Value::fromMat(std::move(out));
}

// row(x, n): row n (1 x cols). A vector n gathers several rows (GUESS).
Value row(EvalContext&, Value* a, int) {
    const Value& x = a[0];
    if (x.isScalar()) return x;
    const Value& n = a[1];
    const int count = int(n.size());
    auto out = std::make_shared<Mat>(count, x.cols());
    for (int k = 0; k < count; ++k) {
        const int r = indexArg(n.flat(std::size_t(k)), x.rows());
        for (int c = 0; c < x.cols(); ++c) out->at(k, c) = x.mat().at(r, c);
    }
    return Value::fromMat(std::move(out));
}

// col(x, n): column n (rows x 1). A vector n gathers several columns (GUESS).
Value col(EvalContext&, Value* a, int) {
    const Value& x = a[0];
    if (x.isScalar()) return x;
    const Value& n = a[1];
    const int count = int(n.size());
    auto out = std::make_shared<Mat>(x.rows(), count);
    for (int k = 0; k < count; ++k) {
        const int c = indexArg(n.flat(std::size_t(k)), x.cols());
        for (int r = 0; r < x.rows(); ++r) out->at(r, k) = x.mat().at(r, c);
    }
    return Value::fromMat(std::move(out));
}

// noise(cols, rows, octaves): GUESS. A rows x cols matrix of fractal value
// noise in [0, 1]: octave o samples a (2^(o+1)+1)^2 lattice of RNG values with
// bilinear interpolation, amplitudes halving per octave. Uses (and advances)
// the context RNG, so srand() makes it reproducible [VC].
Value noise(EvalContext& ctx, Value* a, int) {
    const int nc = countArg(a[0], 4096), nr = countArg(a[1], 4096);
    const int octaves = std::clamp(countArg(a[2]), 1, 12);
    Value out = Value::matrix(nr, nc);
    std::vector<double> acc(std::size_t(out.rows()) * std::size_t(out.cols()), 0.0);
    const int R = out.rows(), C = out.cols();
    double ampSum = 0.0, amp = 1.0;
    for (int o = 0; o < octaves; ++o) {
        const int g = (1 << std::min(o + 1, 10)) + 1;
        std::vector<double> lat(std::size_t(g) * std::size_t(g));
        for (double& v : lat) v = ctx.rng.uniform01();
        for (int r = 0; r < R; ++r) {
            const double v = R == 1 ? 0.0 : double(r) / double(R - 1) * double(g - 1);
            const int y0 = std::min(int(v), g - 2);
            const double ty = v - y0;
            for (int c = 0; c < C; ++c) {
                const double u = C == 1 ? 0.0 : double(c) / double(C - 1) * double(g - 1);
                const int x0 = std::min(int(u), g - 2);
                const double tx = u - x0;
                auto L = [&](int yy, int xx) { return lat[std::size_t(yy) * std::size_t(g) + std::size_t(xx)]; };
                const double top = L(y0, x0) * (1 - tx) + L(y0, x0 + 1) * tx;
                const double bot = L(y0 + 1, x0) * (1 - tx) + L(y0 + 1, x0 + 1) * tx;
                acc[std::size_t(r) * std::size_t(C) + std::size_t(c)] += amp * (top * (1 - ty) + bot * ty);
            }
        }
        ampSum += amp;
        amp *= 0.5;
    }
    if (out.isScalar()) return Value(acc[0] / ampSum);
    Mat& m = out.mutableMat();
    for (std::size_t i = 0; i < acc.size(); ++i) m.d[i] = acc[i] / ampSum;
    return out;
}

// resample(x, cols, rows, interp): resize to rows x cols, corners mapped to
// corners; interp 0 = nearest, otherwise bilinear (GUESS on interp values).
Value resample(EvalContext&, Value* a, int) {
    const Value& x = a[0];
    const int nc = countArg(a[1], 1 << 16), nr = countArg(a[2], 1 << 16);
    const bool linear = a[3].scalar() != 0.0;
    Value out = Value::matrix(nr, nc);
    if (out.isScalar()) return Value(x.scalar());
    Mat& m = out.mutableMat();
    const int R = x.rows(), C = x.cols();
    for (int r = 0; r < m.rows; ++r) {
        const double sy = m.rows == 1 ? 0.0 : double(r) * double(R - 1) / double(m.rows - 1);
        for (int c = 0; c < m.cols; ++c) {
            const double sx = m.cols == 1 ? 0.0 : double(c) * double(C - 1) / double(m.cols - 1);
            if (!linear || x.isScalar()) {
                m.at(r, c) = x.at(int(std::lround(sy)), int(std::lround(sx)));
                continue;
            }
            const int x0 = std::min(int(sx), std::max(C - 2, 0)), y0 = std::min(int(sy), std::max(R - 2, 0));
            const int x1 = std::min(x0 + 1, C - 1), y1 = std::min(y0 + 1, R - 1);
            const double tx = sx - x0, ty = sy - y0;
            const double top = x.at(y0, x0) * (1 - tx) + x.at(y0, x1) * tx;
            const double bot = x.at(y1, x0) * (1 - tx) + x.at(y1, x1) * tx;
            m.at(r, c) = top * (1 - ty) + bot * ty;
        }
    }
    return out;
}

// map1D(f, x, interp): look up each element of x in each row of f, with x
// wrapped into [0, 1) across the row [VC]. GUESS: the row is treated as
// periodic (position wrap(x) * cols, so x = 1 maps back to element 0 and
// closed curves close), interp 0 = nearest, otherwise linear. Result has the
// shape of x when f has one row, else rows(f) x size(x).
Value map1D(EvalContext&, Value* a, int) {
    const Value& f = a[0];
    const Value& x = a[1];
    const bool linear = a[2].scalar() != 0.0;
    if (f.isScalar()) return mapUnary(x, [&](double) { return f.s_; });
    const Mat& fm = f.mat();
    auto sample = [&](int r, double xv) {
        if (!std::isfinite(xv)) return 0.0;
        const double p = (xv - std::floor(xv)) * double(fm.cols);
        if (!linear) {
            long i = long(std::floor(p + 0.5)) % fm.cols;
            return fm.at(r, int(i));
        }
        return sampleRowLinear(fm, r, p, true);
    };
    if (fm.rows == 1) return mapUnary(x, [&](double xv) { return sample(0, xv); });
    auto out = std::make_shared<Mat>(fm.rows, int(x.size()));
    for (int r = 0; r < fm.rows; ++r)
        for (int k = 0; k < out->cols; ++k) out->at(r, k) = sample(r, x.flat(std::size_t(k)));
    return Value::fromMat(std::move(out));
}

// map2D(f, x, y, interp): x, y in [0, 1] (clamped), (0,0) = top-left of f [VC].
// x spans columns, y spans rows, both corner-to-corner (GUESS).
Value map2D(EvalContext&, Value* a, int) {
    const Value& f = a[0];
    const bool linear = a[3].scalar() != 0.0;
    if (f.isScalar()) return mapBinary(a[1], a[2], [&](double, double) { return f.s_; });
    const Mat& fm = f.mat();
    return mapBinary(a[1], a[2], [&](double xv, double yv) {
        if (!std::isfinite(xv) || !std::isfinite(yv)) return 0.0;
        const double px = std::clamp(xv, 0.0, 1.0) * double(fm.cols - 1);
        const double py = std::clamp(yv, 0.0, 1.0) * double(fm.rows - 1);
        if (!linear) return fm.at(int(std::lround(py)), int(std::lround(px)));
        const int x0 = std::min(int(px), std::max(fm.cols - 2, 0)), y0 = std::min(int(py), std::max(fm.rows - 2, 0));
        const int x1 = std::min(x0 + 1, fm.cols - 1), y1 = std::min(y0 + 1, fm.rows - 1);
        const double tx = px - x0, ty = py - y0;
        const double top = fm.at(y0, x0) * (1 - tx) + fm.at(y0, x1) * tx;
        const double bot = fm.at(y1, x0) * (1 - tx) + fm.at(y1, x1) * tx;
        return top * (1 - ty) + bot * ty;
    });
}

// smooth(x, sigma): Gaussian blur along each row; negative sigma wraps around
// the row ends [VC]. GUESS: sigma is measured in elements (columns); edges
// clamp when not wrapping.
Value smooth(EvalContext&, Value* a, int) {
    const Value& x = a[0];
    const double sigmaArg = a[1].scalar();
    if (x.isScalar() || !std::isfinite(sigmaArg)) return x;
    const bool wrapEdges = sigmaArg < 0.0;
    const double sigma = std::fabs(sigmaArg);
    if (sigma < 1e-6) return x;
    const Mat& m = x.mat();
    const int radius = std::min(int(std::ceil(3.0 * sigma)), std::max(m.cols, 1) * 4);
    std::vector<double> w(std::size_t(2 * radius + 1));
    double wsum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        const double v = std::exp(-0.5 * double(k * k) / (sigma * sigma));
        w[std::size_t(k + radius)] = v;
        wsum += v;
    }
    auto out = std::make_shared<Mat>(m.rows, m.cols);
    for (int r = 0; r < m.rows; ++r)
        for (int c = 0; c < m.cols; ++c) {
            double acc = 0.0;
            for (int k = -radius; k <= radius; ++k) {
                int j = c + k;
                if (wrapEdges) {
                    j %= m.cols;
                    if (j < 0) j += m.cols;
                } else {
                    j = std::clamp(j, 0, m.cols - 1);
                }
                acc += w[std::size_t(k + radius)] * m.at(r, j);
            }
            out->at(r, c) = acc / wsum;
        }
    return Value::fromMat(std::move(out));
}

// dot(a, b) / dot(a) == dot(a, a). GUESS: columns are vectors, so a matrix
// with several rows yields one dot product per column (1 x cols); row vectors
// and scalars reduce over all elements to a scalar.
Value dot(EvalContext&, Value* a, int n) {
    const Value& x = a[0];
    const Value& y = n >= 2 ? a[1] : a[0];
    const Value prod = mapBinary(x, y, [](double p, double q) { return p * q; });
    if (prod.isScalar()) return prod;
    const Mat& m = prod.mat();
    if (m.rows == 1) {
        double s = 0.0;
        for (double v : m.d) s += v;
        return Value(s);
    }
    auto out = std::make_shared<Mat>(1, m.cols);
    for (int c = 0; c < m.cols; ++c) {
        double s = 0.0;
        for (int r = 0; r < m.rows; ++r) s += m.at(r, c);
        out->at(0, c) = s;
    }
    return Value::fromMat(std::move(out));
}

}  // namespace reforce::expr::mfn
