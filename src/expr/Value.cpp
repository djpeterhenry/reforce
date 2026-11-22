#include "expr/Value.hpp"

#include <cmath>
#include <cstdio>

namespace reforce::expr {

Value Value::matrix(int rows, int cols, double fill) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (std::size_t(rows) * std::size_t(cols) > kMaxMatrixElements) {
        // Keep the requested aspect roughly, but clamp the element count.
        if (std::size_t(cols) > kMaxMatrixElements) cols = int(kMaxMatrixElements);
        rows = int(kMaxMatrixElements / std::size_t(cols));
        if (rows < 1) rows = 1;
    }
    if (rows == 1 && cols == 1) return Value(fill);
    Value v;
    v.m_ = std::make_shared<Mat>(rows, cols, fill);
    return v;
}

Value Value::fromMat(std::shared_ptr<Mat> m) {
    if (!m || m->d.empty()) return Value(0.0);
    if (m->rows == 1 && m->cols == 1) return Value(m->d[0]);
    Value v;
    v.m_ = std::move(m);
    return v;
}

Value Value::row(const std::vector<double>& values) {
    if (values.empty()) return Value(0.0);
    auto m = std::make_shared<Mat>(1, int(values.size()));
    m->d = values;
    return fromMat(std::move(m));
}

bool Value::allFinite() const {
    if (!m_) return std::isfinite(s_);
    for (double x : m_->d)
        if (!std::isfinite(x)) return false;
    return true;
}

std::string Value::toString() const {
    char buf[64];
    if (!m_) {
        std::snprintf(buf, sizeof buf, "%g", s_);
        return buf;
    }
    std::string out = "{";
    for (int r = 0; r < m_->rows; ++r) {
        if (m_->rows > 1) out += (r ? ", {" : "{");
        for (int c = 0; c < m_->cols; ++c) {
            std::snprintf(buf, sizeof buf, "%s%g", c ? ", " : "", m_->at(r, c));
            out += buf;
        }
        if (m_->rows > 1) out += "}";
    }
    out += "}";
    return out;
}

}  // namespace reforce::expr
