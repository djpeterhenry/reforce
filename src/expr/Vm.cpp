#include "expr/Vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "expr/Builtins.hpp"

namespace reforce::expr {

EvalContext::EvalContext() {
    set(Var::PI, M_PI);
    set(Var::NUM, 1);
    set(Var::X_EXTENTS, 1);
    set(Var::Y_EXTENTS, 1);
    set(Var::END_TIME, 1e30);  // regular WaveShape: `end_time < 1e30` is false (spec §5.2)
    set(Var::FADER, 1);
    stack.resize(64);
}

void EvalContext::ensureSlots(int family, int count) {
    auto& v = vars[std::size_t(family)];
    if (int(v.size()) < count) v.resize(std::size_t(count));
}

void EvalContext::ensureKeySlots(int count) {
    if (int(keys.size()) < count) keys.resize(std::size_t(count));
}

namespace {

std::int64_t toInt(double x) {
    if (!(x == x)) return 0;
    if (x >= 9.2e18) return INT64_MAX;
    if (x <= -9.2e18) return INT64_MIN;
    return std::int64_t(x);
}

}  // namespace

double scalarBinary(Op op, double a, double b) {
    switch (op) {
        case Op::Add: return a + b;
        case Op::Sub: return a - b;
        case Op::Mul: return a * b;
        case Op::Div: return a / b;  // IEEE: x/0 = +-inf, 0/0 = NaN (undocumented in G-Force)
        case Op::ModF: return b == 0.0 ? 0.0 : std::fmod(a, b);  // GUESS: x % 0 == 0
        case Op::ModInt: {
            const double ta = std::trunc(a), tb = std::trunc(b);
            return tb == 0.0 ? 0.0 : std::fmod(ta, tb);
        }
        case Op::Pow: return std::pow(a, b);
        case Op::Eq: return a == b ? 1.0 : 0.0;
        case Op::Ne: return a != b ? 1.0 : 0.0;
        case Op::Lt: return a < b ? 1.0 : 0.0;
        case Op::Gt: return a > b ? 1.0 : 0.0;
        case Op::Le: return a <= b ? 1.0 : 0.0;
        case Op::Ge: return a >= b ? 1.0 : 0.0;
        case Op::LogAnd: return (a != 0.0 && b != 0.0) ? 1.0 : 0.0;
        case Op::LogOr: return (a != 0.0 || b != 0.0) ? 1.0 : 0.0;
        case Op::BitAnd: return double(toInt(a) & toInt(b));
        case Op::BitOr: return double(toInt(a) | toInt(b));
        case Op::BitXor: return double(toInt(a) ^ toInt(b));
        case Op::Shl: {
            const auto s = toInt(b);
            return s <= 0 ? double(toInt(a)) : s >= 63 ? 0.0 : double(std::uint64_t(toInt(a)) << s);
        }
        case Op::Shr: {
            const auto s = toInt(b);
            return s <= 0 ? double(toInt(a)) : s >= 63 ? (a < 0 ? -1.0 : 0.0) : double(toInt(a) >> s);
        }
        default: return 0.0;
    }
}

Value applyBinary(Op op, const Value& a, const Value& b) {
    if (a.isScalar() && b.isScalar()) return Value(scalarBinary(op, a.s_, b.s_));
    switch (op) {
        case Op::Add: return mapBinary(a, b, [](double x, double y) { return x + y; });
        case Op::Sub: return mapBinary(a, b, [](double x, double y) { return x - y; });
        case Op::Mul: return mapBinary(a, b, [](double x, double y) { return x * y; });
        case Op::Div: return mapBinary(a, b, [](double x, double y) { return x / y; });
        default: return mapBinary(a, b, [op](double x, double y) { return scalarBinary(op, x, y); });
    }
}

Value applyUnary(Op op, const Value& a) {
    switch (op) {
        case Op::Neg: return mapUnary(a, [](double x) { return -x; });
        case Op::Not: return mapUnary(a, [](double x) { return x == 0.0 ? 1.0 : 0.0; });
        case Op::CastInt: return mapUnary(a, [](double x) { return std::trunc(x); });
        default: return a;
    }
}

// Matrix literal rules (scene-format §3.1; partly GUESS):
//  - all elements scalar          -> 1 x n row vector:  {1, 2, 3}
//  - otherwise each element contributes its rows, stacked top to bottom:
//    {{1,2},{3,4}} -> 2x2; {rowA, rowB} -> 2 x cols.
//    Width is the widest element; a 1-column element is broadcast across
//    the width, narrower multi-column rows are zero-padded.
Value buildMatrix(const Value* e, int n) {
    if (n <= 0) return Value(0.0);
    bool allScalar = true;
    int width = 1, height = 0;
    for (int i = 0; i < n; ++i) {
        if (!e[i].isScalar()) allScalar = false;
        width = std::max(width, e[i].cols());
        height += e[i].rows();
    }
    if (allScalar) {
        auto m = std::make_shared<Mat>(1, n);
        for (int i = 0; i < n; ++i) m->d[std::size_t(i)] = e[i].s_;
        return Value::fromMat(std::move(m));
    }
    auto m = std::make_shared<Mat>(height, width);
    int r0 = 0;
    for (int i = 0; i < n; ++i) {
        const Value& v = e[i];
        for (int r = 0; r < v.rows(); ++r) {
            for (int c = 0; c < width; ++c) {
                double x = 0.0;
                if (v.cols() == 1) x = v.at(r, 0);
                else if (c < v.cols()) x = v.at(r, c);
                m->at(r0 + r, c) = x;
            }
        }
        r0 += v.rows();
    }
    return Value::fromMat(std::move(m));
}

Value extractLane(const Value& v, int lane, int laneCount) {
    if (v.isScalar() || laneCount <= 1 || v.cols() != laneCount) return v;
    if (v.rows() == 1) return Value(v.mat().at(0, lane));
    auto m = std::make_shared<Mat>(v.rows(), 1);
    for (int r = 0; r < v.rows(); ++r) m->at(r, 0) = v.mat().at(r, lane);
    return Value::fromMat(std::move(m));
}

// GUESS: when an output expression yields a matrix whose width is not the lane
// count, element (lane mod size) in row-major order is used.
double laneScalar(const Value& v, int lane, int laneCount) {
    if (v.isScalar()) return v.s_;
    if (v.cols() == laneCount) return v.mat().at(0, lane);
    return v.flat(std::size_t(lane < 0 ? 0 : lane));
}

namespace {

void roundToFloat(Value& v) {
    if (v.isScalar()) v.s_ = double(float(v.s_));
    else v = mapUnary(v, [](double d) { return double(float(d)); });
}

template <bool F32>
Value runImpl(const Program& p, EvalContext& ctx) {
    if (int(ctx.stack.size()) < p.maxStack + 1) ctx.stack.resize(std::size_t(p.maxStack + 1));
    Value* const base = ctx.stack.data();
    Value* sp = base;
    for (const Instr& in : p.code) {
        switch (in.op) {
            case Op::PushConst: *sp++ = p.consts[std::size_t(in.arg)]; break;
            case Op::LoadA:
            case Op::LoadB:
            case Op::LoadC:
            case Op::LoadD: {
                const auto& fam = ctx.vars[std::size_t(int(in.op) - int(Op::LoadA))];
                if (std::size_t(in.arg) < fam.size()) *sp = fam[std::size_t(in.arg)];
                else sp->setScalar(0.0);
                ++sp;
                break;
            }
            case Op::LoadKey:
                if (std::size_t(in.arg) < ctx.keys.size()) *sp = ctx.keys[std::size_t(in.arg)];
                else sp->setScalar(0.0);
                ++sp;
                break;
            case Op::LoadVar: *sp++ = ctx.builtins[std::size_t(in.arg)]; break;
            case Op::Neg:
            case Op::Not:
            case Op::CastInt: {
                Value& a = sp[-1];
                if (a.isScalar()) {
                    a.s_ = in.op == Op::Neg ? -a.s_ : in.op == Op::Not ? (a.s_ == 0.0 ? 1.0 : 0.0) : std::trunc(a.s_);
                } else {
                    a = applyUnary(in.op, a);
                }
                break;
            }
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::ModF:
            case Op::ModInt:
            case Op::Pow:
            case Op::Eq:
            case Op::Ne:
            case Op::Lt:
            case Op::Gt:
            case Op::Le:
            case Op::Ge:
            case Op::LogAnd:
            case Op::LogOr:
            case Op::BitAnd:
            case Op::BitOr:
            case Op::BitXor:
            case Op::Shl:
            case Op::Shr: {
                Value& a = sp[-2];
                Value& b = sp[-1];
                if (a.isScalar() && b.isScalar()) {
                    switch (in.op) {
                        case Op::Add: a.s_ += b.s_; break;
                        case Op::Sub: a.s_ -= b.s_; break;
                        case Op::Mul: a.s_ *= b.s_; break;
                        case Op::Div: a.s_ /= b.s_; break;
                        default: a.s_ = scalarBinary(in.op, a.s_, b.s_); break;
                    }
                } else {
                    a = applyBinary(in.op, a, b);
                    b.m_.reset();
                }
                --sp;
                break;
            }
            case Op::CallS1: {
                Value& a = sp[-1];
                const BuiltinInfo& bi = builtinAt(in.arg);
                if (a.isScalar()) a.s_ = bi.s1(a.s_);
                else a = bi.fn(ctx, &a, 1);
                break;
            }
            case Op::CallS2: {
                Value& a = sp[-2];
                Value& b = sp[-1];
                const BuiltinInfo& bi = builtinAt(in.arg);
                if (a.isScalar() && b.isScalar()) a.s_ = bi.s2(a.s_, b.s_);
                else {
                    a = bi.fn(ctx, &a, 2);
                    b.m_.reset();
                }
                --sp;
                break;
            }
            case Op::Call: {
                const int argc = in.argc;
                Value* args = sp - argc;
                Value r = builtinAt(in.arg).fn(ctx, args, argc);
                for (int k = 1; k < argc; ++k) args[k].m_.reset();
                sp = args;
                *sp++ = std::move(r);
                break;
            }
            case Op::MakeMatrix: {
                const int n = in.argc;
                Value* elems = sp - n;
                Value r = buildMatrix(elems, n);
                for (int k = 0; k < n; ++k) elems[k].m_.reset();
                sp = elems;
                *sp++ = std::move(r);
                break;
            }
        }
        if constexpr (F32) roundToFloat(sp[-1]);
    }
    Value out = std::move(base[0]);
    base[0].m_.reset();
    return out;
}

}  // namespace

Value run(const Program& p, EvalContext& ctx) {
    if (p.code.empty()) return Value(0.0);
    return ctx.float32 ? runImpl<true>(p, ctx) : runImpl<false>(p, ctx);
}

}  // namespace reforce::expr
