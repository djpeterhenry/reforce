#include "expr/Builtins.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>

#include "expr/Vars.hpp"
#include "expr/Vm.hpp"

namespace reforce::expr {

namespace fn {

double sign(double x) { return x >= 0.0 ? 1.0 : -1.0; }  // [CP]: sign(0) == 1
double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
double sqwv(double x) { return std::fabs(x) <= 1.0 ? 1.0 : 0.0; }
double wrap(double x) { return x - std::floor(x); }
double pos(double x) { return x > 0.0 ? x : 0.0; }

// Triangle wave through (0,0), (1,1), (2,0), period 2.
double trwv(double x) { return 1.0 - std::fabs(2.0 * wrap(x * 0.5) - 1.0); }

// GUESS (scene-format §10.6): [CP] lists the points ...(-3,-1),(-1,1),(-1,-1),
// (1,1),(1,-1)..., i.e. a period-2 sawtooth rising from -1 to 1 that drops at
// odd integers. 2*wrap((x+1)/2) - 1 passes through exactly those points.
// Unverified against G-Force output; flip(0) == 0.
double flip(double x) { return 2.0 * wrap((x + 1.0) * 0.5) - 1.0; }

// [CP]: -4/9 r^3 + 17/9 r^2 - 22/9 r + 1. GUESS (§10.7): [VC] names the
// parameter r2; we apply the polynomial to the argument as given.
double wyvill(double r) { return ((-4.0 / 9.0 * r + 17.0 / 9.0) * r - 22.0 / 9.0) * r + 1.0; }

}  // namespace fn

namespace {

double sAbs(double x) { return std::fabs(x); }
double sSqr(double x) { return x * x; }
double sSqrt(double x) { return std::sqrt(x); }
double sExp(double x) { return std::exp(x); }
double sLog(double x) { return std::log(x); }
double sLog10(double x) { return std::log10(x); }
double sSin(double x) { return std::sin(x); }
double sCos(double x) { return std::cos(x); }
double sTan(double x) { return std::tan(x); }
double sAsin(double x) { return std::asin(x); }
double sAcos(double x) { return std::acos(x); }
double sAtan(double x) { return std::atan(x); }
double sTrunc(double x) { return std::trunc(x); }
double sFloor(double x) { return std::floor(x); }
double sCeil(double x) { return std::ceil(x); }
double sRound(double x) { return std::round(x); }  // halves away from zero (guess)

double sDim(double a, double b) { return a - b > 0.0 ? a - b : 0.0; }
double sPow(double a, double b) { return std::pow(a, b); }
double sAtan2(double y, double x) { return std::atan2(y, x); }
double sHypot(double a, double b) { return std::sqrt(a * a + b * b); }
double sHypot2(double a, double b) { return a * a + b * b; }
double sLeg(double h, double x) {
    const double d = h * h - x * x;
    return d > 0.0 ? std::sqrt(d) : 0.0;
}
double sMin(double a, double b) { return a < b ? a : b; }
double sMax(double a, double b) { return a > b ? a : b; }

template <ScalarFn1 F>
Value unary(EvalContext&, Value* a, int) {
    return mapUnary(a[0], F);
}

template <ScalarFn2 F>
Value binary(EvalContext&, Value* a, int) {
    return mapBinary(a[0], a[1], F);
}

// atan2(y, x); the deprecated one-argument form is atan [CP].
Value bAtan2(EvalContext&, Value* a, int n) {
    if (n == 1) return mapUnary(a[0], sAtan);
    return mapBinary(a[0], a[1], sAtan2);
}

// min/max: elementwise with two args. GUESS: one arg reduces over all elements.
template <bool IsMax>
Value bMinMax(EvalContext&, Value* a, int n) {
    if (n >= 2) return mapBinary(a[0], a[1], IsMax ? sMax : sMin);
    if (a[0].isScalar()) return a[0];
    double r = a[0].mat().d[0];
    for (double x : a[0].mat().d) r = IsMax ? (x > r ? x : r) : (x < r ? x : r);
    return Value(r);
}

// rnd(x): uniform in [0, x], same shape as x [CP][VC]. In per-lane tiers a
// scalar argument yields one value per lane (FlowField D/src, [FL]).
Value bRnd(EvalContext& ctx, Value* a, int) {
    if (a[0].isScalar()) {
        if (ctx.randomLanes > 1) {
            Value out = Value::matrix(1, ctx.randomLanes);
            if (out.isMatrix())
                for (double& d : out.mutableMat().d) d = a[0].s_ * ctx.rng.uniform01();
            return out;
        }
        return Value(a[0].s_ * ctx.rng.uniform01());
    }
    auto out = std::make_shared<Mat>(a[0].rows(), a[0].cols());
    for (std::size_t i = 0; i < out->d.size(); ++i) out->d[i] = a[0].mat().d[i] * ctx.rng.uniform01();
    return Value::fromMat(std::move(out));
}

// srand(x)/seed(x): reseed from the bits of x; returns x [CP][VC].
Value bSrand(EvalContext& ctx, Value* a, int) {
    ctx.rng.reseedFromDouble(a[0].scalar());
    return a[0];
}

// mag()/fft(): discrete nearest-bin lookup, x in 0..1 (mag(1) is the last bin).
// Values outside [0, 1] wrap [VH 2.5.0]; exactly 1 does not wrap (the 2.7.1
// "edge fix").
double audioLookup(const float* data, int count, double x) {
    if (!data || count <= 0 || !std::isfinite(x)) return 0.0;
    if (x < 0.0 || x > 1.0) x = fn::wrap(x);
    long idx = std::lround(x * double(count - 1));
    if (idx < 0) idx = 0;
    if (idx >= count) idx = count - 1;
    return double(data[idx]);
}

Value bMag(EvalContext& ctx, Value* a, int) {
    const auto& au = ctx.audio;
    return mapUnary(a[0], [&](double x) { return audioLookup(au.wave, au.waveCount, x); });
}

Value bFft(EvalContext& ctx, Value* a, int) {
    const auto& au = ctx.audio;
    return mapUnary(a[0], [&](double x) { return audioLookup(au.fft, au.fftCount, x); });
}

// clock(): system time in ms; time(): seconds since midnight [CP]. In
// deterministic mode both derive from `t`.
Value bClock(EvalContext& ctx, Value*, int) {
    if (ctx.deterministicClock) return Value(ctx.get(Var::T) * 1000.0);
    using namespace std::chrono;
    return Value(double(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()));
}

Value bTime(EvalContext& ctx, Value*, int) {
    if (ctx.deterministicClock) return Value(ctx.get(Var::T));
    const std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    return Value(double(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec));
}

Value bPrint(EvalContext& ctx, Value* a, int) {
    if (ctx.printHook) ctx.printHook(a[0]);
    return a[0];
}

constexpr BuiltinInfo B(const char* name, int mn, int mx, BuiltinFn f, ScalarFn1 s1 = nullptr,
                        ScalarFn2 s2 = nullptr, bool pure = true, bool random = false, bool audio = false) {
    return BuiltinInfo{name, mn, mx, f, s1, s2, pure, random, audio};
}

#define U1(name, F) B(name, 1, 1, unary<F>, F)
#define U2(name, F) B(name, 2, 2, binary<F>, nullptr, F)

const std::array kBuiltins = {
    // Common [CP]
    U1("abs", sAbs),
    U2("dim", sDim),
    U1("sqr", sSqr),
    U1("sqrt", sSqrt),
    U1("exp", sExp),
    U1("log", sLog),
    U1("log10", sLog10),
    U2("pow", sPow),
    U1("sin", sSin),
    U1("cos", sCos),
    U1("tan", sTan),
    U1("asin", sAsin),
    U1("acos", sAcos),
    U1("atan", sAtan),
    B("atan2", 1, 2, bAtan2, sAtan, sAtan2),
    U2("hypot", sHypot),
    U2("hypot2", sHypot2),
    U2("leg", sLeg),
    U1("wyvill", fn::wyvill),
    // Logical / discrete
    U1("pos", fn::pos),
    U1("sign", fn::sign),
    U1("sgn", fn::sign),        // deprecated alias
    U1("clamp", fn::clamp01),
    U1("clip", fn::clamp01),    // deprecated alias
    U1("sqwv", fn::sqwv),
    B("select", 3, 3, mfn::select),
    B("choice", 1, 1, mfn::choice, nullptr, nullptr, false, true),
    B("largest", 1, 1, mfn::largest),
    // Miscellaneous
    B("rnd", 1, 1, bRnd, nullptr, nullptr, false, true),
    U1("trunc", sTrunc),
    U1("trnc", sTrunc),         // deprecated alias
    U1("floor", sFloor),
    U1("flor", sFloor),         // deprecated alias
    U1("ceil", sCeil),          // undocumented, used once in the corpus
    U1("round", sRound),
    U1("wrap", fn::wrap),
    U1("flip", fn::flip),
    U1("trwv", fn::trwv),
    B("min", 1, 2, bMinMax<false>, nullptr, sMin),
    B("max", 1, 2, bMinMax<true>, nullptr, sMax),
    B("srand", 1, 1, bSrand, nullptr, nullptr, false, true),
    B("seed", 1, 1, bSrand, nullptr, nullptr, false, true),  // deprecated alias
    B("clock", 0, 0, bClock, nullptr, nullptr, false),
    B("time", 0, 0, bTime, nullptr, nullptr, false),
    B("print", 1, 1, bPrint, nullptr, nullptr, false),
    // Audio
    B("mag", 1, 1, bMag, nullptr, nullptr, false, false, true),
    B("fft", 1, 1, bFft, nullptr, nullptr, false, false, true),
    // VectorC matrix utilities [VC]
    B("vector", 2, 2, mfn::vector),
    B("vectorlr", 3, 3, mfn::vectorLR),
    B("rows", 1, 1, mfn::rows),
    B("cols", 1, 1, mfn::cols),
    B("transpose", 1, 1, mfn::transpose),
    B("subrange", 5, 5, mfn::subrange),
    B("row", 2, 2, mfn::row),
    B("col", 2, 2, mfn::col),
    B("noise", 3, 3, mfn::noise, nullptr, nullptr, false, true),
    B("resample", 4, 4, mfn::resample),
    B("map1d", 3, 3, mfn::map1D),
    B("map2d", 4, 4, mfn::map2D),
    B("smooth", 2, 2, mfn::smooth),
    B("dot", 1, 2, mfn::dot),
};

#undef U1
#undef U2

}  // namespace

std::span<const BuiltinInfo> allBuiltins() { return {kBuiltins.data(), kBuiltins.size()}; }

int findBuiltin(std::string_view name) {
    const std::string lower = toLower(name);
    for (std::size_t i = 0; i < kBuiltins.size(); ++i)
        if (lower == kBuiltins[i].name) return int(i);
    return -1;
}

const BuiltinInfo& builtinAt(int index) { return kBuiltins[std::size_t(index)]; }

}  // namespace reforce::expr
