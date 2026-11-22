#pragma once
// Compiled bytecode for one expression.

#include <cstdint>
#include <utility>
#include <vector>

#include "expr/Value.hpp"
#include "expr/Vars.hpp"

namespace reforce::expr {

enum class Op : std::uint8_t {
    PushConst,   // arg = constant index
    LoadA, LoadB, LoadC, LoadD,  // arg = slot index
    LoadKey,     // arg = key slot
    LoadVar,     // arg = Var
    Neg, Not, CastInt,
    Add, Sub, Mul, Div, ModF, ModInt, Pow,
    Eq, Ne, Lt, Gt, Le, Ge,
    LogAnd, LogOr, BitAnd, BitOr, BitXor, Shl, Shr,
    Call,        // arg = builtin index, argc = arg count
    CallS1,      // arg = builtin index; 1-arg scalar fast path
    CallS2,      // arg = builtin index; 2-arg scalar fast path
    MakeMatrix,  // argc = element count
};

struct Instr {
    Op op = Op::PushConst;
    std::uint16_t argc = 0;
    std::int32_t arg = 0;
};

struct Program {
    std::vector<Instr> code;
    std::vector<Value> consts;
    int maxStack = 0;

    // --- metadata from resolution ---
    bool defined = false;              // false for empty/missing expressions (value: default or 0)
    bool failed = false;               // parse error; compiled as constant 0
    std::uint64_t varsUsed = 0;        // bitmask of varBit(Var)
    std::vector<int> keyRefs;          // key slots referenced
    std::vector<std::pair<int, int>> userRefs;  // (family, index) referenced
    bool usesRandom = false;           // rnd/choice/srand/noise
    bool usesAudio = false;            // mag/fft

    bool usesVar(Var v) const { return (varsUsed & varBit(v)) != 0; }
    bool isConstant() const { return code.size() == 1 && code[0].op == Op::PushConst; }
    double constantValue() const { return isConstant() ? consts[std::size_t(code[0].arg)].scalar() : 0.0; }
};

}  // namespace reforce::expr
