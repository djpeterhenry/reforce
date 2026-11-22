#pragma once
// Operator precedence and associativity: the ONE table the parser reads.
//
// G-Force does not document precedence (scene-format §3.2, §10.1). Black-box
// probes against real G-Force (architecture §7, 2026-11) measured:
//   -1^2   == +1   -> unary minus binds TIGHTER than `^`
//   2^3^2  == 64   -> `^` is LEFT-associative
// Those are the defaults. Both remain LanguageOptions flags so the spec's
// original C/math-style guess can still be selected:
//   unaryBindsTighterThanPow = false   -2^2 == -4
//   powRightAssociative      = true    2^3^2 == 2^9
// `%` is an integer remainder (operands truncated toward zero): measured by
// the corpus sweep (docs/sweep.md: "Bits Between Bits", "Split and Twist",
// "Glass Blocks" only match G-Force with it). Fmod stays selectable.

#include <array>

#include "expr/Lexer.hpp"

namespace reforce::expr {

enum class BinOp {
    Add, Sub, Mul, Div, Mod, Pow,
    Eq, Ne, Lt, Gt, Le, Ge,
    LogAnd, LogOr,
    BitAnd, BitOr, BitXor, Shl, Shr,
};

enum class UnOp { Neg, Plus, Not, CastInt, CastFloat };

enum class ModuloMode {
    Fmod,         // C fmod: sign of the dividend, fractional results kept (earlier guess)
    IntegerTrunc, // integer remainder: both operands truncated toward zero first (G-Force, corpus sweep)
};

struct LanguageOptions {
    bool unaryBindsTighterThanPow = true;   // probe: -1^2 == 1
    bool powRightAssociative = false;       // probe: 2^3^2 == 64
    ModuloMode modulo = ModuloMode::IntegerTrunc;  // corpus sweep; x % 0 evaluates to 0 in both modes
};

struct BinOpInfo {
    Tok token;
    BinOp op;
    int precedence;     // higher binds tighter
    bool rightAssoc;
    const char* text;
};

struct OperatorTable {
    static constexpr int kBinaryCount = 19;
    std::array<BinOpInfo, kBinaryCount> binary{};
    int unaryPrecedence = 0;  // prefix - + ! (int) (float)

    // Returns nullptr when `t` is not a binary operator.
    const BinOpInfo* find(Tok t) const {
        for (const auto& b : binary)
            if (b.token == t) return &b;
        return nullptr;
    }

    static OperatorTable make(const LanguageOptions& opts = {});
};

const char* binOpText(BinOp op);

}  // namespace reforce::expr
