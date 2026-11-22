#pragma once
// Built-in function table (scene-format §3.3, §3.5; corpus-stats "All functions").

#include <span>
#include <string_view>

#include "expr/Value.hpp"

namespace reforce::expr {

class EvalContext;

using BuiltinFn = Value (*)(EvalContext& ctx, Value* args, int argc);
using ScalarFn1 = double (*)(double);
using ScalarFn2 = double (*)(double, double);

struct BuiltinInfo {
    const char* name;   // lower-case canonical name
    int minArgs;
    int maxArgs;
    BuiltinFn fn;       // general implementation (handles matrices and all arities)
    ScalarFn1 s1;       // optional 1-arg scalar fast path
    ScalarFn2 s2;       // optional 2-arg scalar fast path
    bool pure;          // no RNG/audio/clock/side effects -> constant-foldable
    bool random;        // consumes or reseeds the RNG
    bool audio;         // reads mag/fft arrays
};

std::span<const BuiltinInfo> allBuiltins();
// Case-insensitive lookup; returns -1 if unknown.
int findBuiltin(std::string_view name);
const BuiltinInfo& builtinAt(int index);

// Scalar semantics exposed for tests and other modules.
namespace fn {
double sign(double x);   // 1 if x >= 0, else -1
double clamp01(double x);
double sqwv(double x);   // 1 if |x| <= 1 else 0
double trwv(double x);   // triangle through (0,0),(1,1),(2,0)
double wrap(double x);   // x - floor(x)
double flip(double x);   // sawtooth, see Builtins.cpp
double wyvill(double r);
double pos(double x);
}  // namespace fn

// Matrix built-ins (BuiltinsMatrix.cpp).
namespace mfn {
Value select(EvalContext&, Value* a, int n);
Value choice(EvalContext&, Value* a, int n);
Value largest(EvalContext&, Value* a, int n);
Value vector(EvalContext&, Value* a, int n);
Value vectorLR(EvalContext&, Value* a, int n);
Value rows(EvalContext&, Value* a, int n);
Value cols(EvalContext&, Value* a, int n);
Value transpose(EvalContext&, Value* a, int n);
Value subrange(EvalContext&, Value* a, int n);
Value row(EvalContext&, Value* a, int n);
Value col(EvalContext&, Value* a, int n);
Value noise(EvalContext&, Value* a, int n);
Value resample(EvalContext&, Value* a, int n);
Value map1D(EvalContext&, Value* a, int n);
Value map2D(EvalContext&, Value* a, int n);
Value smooth(EvalContext&, Value* a, int n);
Value dot(EvalContext&, Value* a, int n);
}  // namespace mfn

}  // namespace reforce::expr
