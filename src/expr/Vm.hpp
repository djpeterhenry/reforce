#pragma once
// Stack VM and evaluation context (architecture §4.2).
//
// EvalContext holds everything an expression can read: the A/B/C/D slot
// arrays, key-reference slots, built-in variable bindings, audio arrays for
// mag()/fft(), and a deterministic RNG. Slots persist between evaluations;
// the tier driver (scene/Evaluate) decides when each family is re-evaluated.
// Reading a slot that has not been written yet in the current pass therefore
// yields its previous value (initially 0).

#include <array>
#include <functional>
#include <vector>

#include "expr/Operators.hpp"
#include "expr/Program.hpp"
#include "expr/Rng.hpp"
#include "expr/Value.hpp"
#include "expr/Vars.hpp"

namespace reforce::expr {

// Caller-owned audio arrays. mag(x)/fft(x) index them with x in 0..1.
struct AudioInput {
    const float* wave = nullptr;  // PCM samples, NUM_SAMPLE_BINS values (signed)
    int waveCount = 0;
    const float* fft = nullptr;   // spectrum, NUM_FFT_BINS values (>= 0)
    int fftCount = 0;
};

class EvalContext {
public:
    EvalContext();

    std::array<std::vector<Value>, 4> vars;  // A, B, C, D
    std::vector<Value> keys;                 // key-reference slots
    std::array<Value, kVarCount> builtins{};
    AudioInput audio;
    Pcg32 rng;

    // Number of lanes for per-element randomness: when > 1, rnd(scalar)
    // returns a 1 x randomLanes row (FlowField D/src tiers, "rnd() acting in a
    // piecewise fashion with x"). 1 elsewhere.
    int randomLanes = 1;

    // Setting reforce.FlowPrecision = float32 (FlowFields only; default off):
    // every instruction's result is rounded to float32, as if G-Force
    // evaluated in single precision. Corpus sweep 5B.4: hardly measurable
    // (docs/sweep.md notes), so reforce keeps doubles by default.
    bool float32 = false;

    // When true, clock() and time() derive from `t` instead of the wall clock.
    bool deterministicClock = true;
    std::function<void(const Value&)> printHook;  // print(x); default: no-op

    void ensureSlots(int family, int count);
    void ensureKeySlots(int count);
    void set(Var v, double x) { builtins[std::size_t(v)].setScalar(x); }
    void set(Var v, Value x) { builtins[std::size_t(v)] = std::move(x); }
    double get(Var v) const { return builtins[std::size_t(v)].scalar(); }
    const Value& value(Var v) const { return builtins[std::size_t(v)]; }

    Value& slot(int family, int index) { return vars[std::size_t(family)][std::size_t(index)]; }

    std::vector<Value> stack;  // VM scratch, reused across runs
};

// Runs a program. Never throws; undefined programs evaluate to 0.
Value run(const Program& p, EvalContext& ctx);
// Convenience: scalar result (first element of a matrix result).
inline double runScalar(const Program& p, EvalContext& ctx) { return run(p, ctx).scalar(); }

// Operator semantics shared by the VM and the compiler's constant folder.
Value applyBinary(Op op, const Value& a, const Value& b);
Value applyUnary(Op op, const Value& a);
double scalarBinary(Op op, double a, double b);
// Builds a matrix literal from evaluated elements (see Vm.cpp for the rules).
Value buildMatrix(const Value* elems, int n);

// Lane extraction for per-step tiers evaluated one lane at a time: a matrix
// whose column count equals `laneCount` yields column `lane` (a scalar for a
// row vector); any other value is returned unchanged.
Value extractLane(const Value& v, int lane, int laneCount);
// Scalar for output keys: column `lane` of row 0 when cols == laneCount,
// otherwise the flat element at lane % size (guess; see Vm.cpp).
double laneScalar(const Value& v, int lane, int laneCount);

}  // namespace reforce::expr
