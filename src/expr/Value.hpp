#pragma once
// VectorC-style value: a scalar double (fast path, no allocation) or a dense
// row-major matrix of doubles. A 1x1 matrix is always normalized to a scalar.
//
// Shape conventions (scene-format §3.1/§3.3): `{a, b, c}` and vectorLR(N, ...)
// are row vectors (1 row x N cols). Columns are "points": transpose({x, y})
// is a 2x1 column.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace reforce::expr {

// Upper bound on elements in any matrix the VM creates (vector(N), noise, ...).
// Larger requests are clamped so a typo cannot allocate gigabytes.
inline constexpr std::size_t kMaxMatrixElements = std::size_t(1) << 22;

struct Mat {
    int rows = 1;
    int cols = 1;
    std::vector<double> d;  // rows*cols, row-major

    Mat() = default;
    Mat(int r, int c, double fill = 0.0) : rows(r), cols(c), d(std::size_t(r) * std::size_t(c), fill) {}
    double& at(int r, int c) { return d[std::size_t(r) * std::size_t(cols) + std::size_t(c)]; }
    double at(int r, int c) const { return d[std::size_t(r) * std::size_t(cols) + std::size_t(c)]; }
};

class Value {
public:
    Value() = default;
    Value(double v) : s_(v) {}  // NOLINT(google-explicit-constructor): scalars convert freely

    // Allocates a rows x cols matrix (filled). 1x1 yields a scalar. Dimensions
    // are clamped to >= 1 and to kMaxMatrixElements in total.
    static Value matrix(int rows, int cols, double fill = 0.0);
    // Wraps an existing matrix; 1x1 is normalized to a scalar.
    static Value fromMat(std::shared_ptr<Mat> m);
    static Value row(const std::vector<double>& values);

    bool isScalar() const { return !m_; }
    bool isMatrix() const { return static_cast<bool>(m_); }
    int rows() const { return m_ ? m_->rows : 1; }
    int cols() const { return m_ ? m_->cols : 1; }
    std::size_t size() const { return m_ ? m_->d.size() : 1; }

    // Scalar view: the value itself, or the first element of a matrix.
    double scalar() const { return m_ ? m_->d[0] : s_; }
    // Flat element access (row-major), wrapping the index modulo size().
    double flat(std::size_t i) const { return m_ ? m_->d[i % m_->d.size()] : s_; }
    // Element (r, c) with broadcasting: a dimension of size 1 always reads
    // index 0; a mismatched dimension cycles (index modulo size).
    double at(int r, int c) const {
        if (!m_) return s_;
        return m_->at(m_->rows == 1 ? 0 : r % m_->rows, m_->cols == 1 ? 0 : c % m_->cols);
    }

    const Mat& mat() const { return *m_; }
    Mat& mutableMat() { return *m_; }   // caller must own the only reference
    const std::shared_ptr<Mat>& matPtr() const { return m_; }

    void setScalar(double v) { s_ = v; m_.reset(); }
    bool allFinite() const;
    std::string toString() const;

    // Raw fields are public to the VM for its scalar fast path.
    double s_ = 0.0;
    std::shared_ptr<Mat> m_;
};

// Result shape of an elementwise operation between shapes (r1,c1) and (r2,c2).
inline int broadcastDim(int a, int b) { return a == b ? a : (a == 1 ? b : (b == 1 ? a : (a > b ? a : b))); }

template <class F>
Value mapUnary(const Value& a, F f) {
    if (a.isScalar()) return Value(f(a.s_));
    auto out = std::make_shared<Mat>(a.rows(), a.cols());
    const auto& src = a.mat().d;
    for (std::size_t i = 0; i < src.size(); ++i) out->d[i] = f(src[i]);
    return Value::fromMat(std::move(out));
}

template <class F>
Value mapBinary(const Value& a, const Value& b, F f) {
    if (a.isScalar() && b.isScalar()) return Value(f(a.s_, b.s_));
    const int r = broadcastDim(a.rows(), b.rows());
    const int c = broadcastDim(a.cols(), b.cols());
    auto out = std::make_shared<Mat>(r, c);
    if (a.rows() == r && a.cols() == c && b.isScalar()) {
        const auto& ad = a.mat().d;
        for (std::size_t i = 0; i < ad.size(); ++i) out->d[i] = f(ad[i], b.s_);
    } else if (b.rows() == r && b.cols() == c && a.isScalar()) {
        const auto& bd = b.mat().d;
        for (std::size_t i = 0; i < bd.size(); ++i) out->d[i] = f(a.s_, bd[i]);
    } else if (a.rows() == r && a.cols() == c && b.rows() == r && b.cols() == c) {
        const auto& ad = a.mat().d;
        const auto& bd = b.mat().d;
        for (std::size_t i = 0; i < ad.size(); ++i) out->d[i] = f(ad[i], bd[i]);
    } else {
        for (int i = 0; i < r; ++i)
            for (int j = 0; j < c; ++j) out->at(i, j) = f(a.at(i, j), b.at(i, j));
    }
    return Value::fromMat(std::move(out));
}

template <class F>
Value mapTernary(const Value& a, const Value& b, const Value& c, F f) {
    if (a.isScalar() && b.isScalar() && c.isScalar()) return Value(f(a.s_, b.s_, c.s_));
    const int r = broadcastDim(broadcastDim(a.rows(), b.rows()), c.rows());
    const int cc = broadcastDim(broadcastDim(a.cols(), b.cols()), c.cols());
    auto out = std::make_shared<Mat>(r, cc);
    for (int i = 0; i < r; ++i)
        for (int j = 0; j < cc; ++j) out->at(i, j) = f(a.at(i, j), b.at(i, j), c.at(i, j));
    return Value::fromMat(std::move(out));
}

}  // namespace reforce::expr
