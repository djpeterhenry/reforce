#include "expr/Operators.hpp"

namespace reforce::expr {

OperatorTable OperatorTable::make(const LanguageOptions& opts) {
    // Precedence levels (scene-format §3.2), loosest first.
    enum : int {
        kLogOr = 1,
        kLogAnd,
        kBitOr,
        kBitXor,
        kBitAnd,
        kEquality,
        kRelational,
        kShift,
        kAdditive,
        kMultiplicative,
        kUnaryDefault,  // unary - + ! casts
        kPow,           // `^`
        kUnaryTight,    // used when unaryBindsTighterThanPow
    };
    OperatorTable t;
    const bool powRight = opts.powRightAssociative;
    t.binary = {{
        {Tok::OrOr, BinOp::LogOr, kLogOr, false, "||"},
        {Tok::AndAnd, BinOp::LogAnd, kLogAnd, false, "&&"},
        {Tok::Pipe, BinOp::BitOr, kBitOr, false, "|"},
        {Tok::CaretCaret, BinOp::BitXor, kBitXor, false, "^^"},
        {Tok::Amp, BinOp::BitAnd, kBitAnd, false, "&"},
        {Tok::EqEq, BinOp::Eq, kEquality, false, "=="},
        {Tok::NotEq, BinOp::Ne, kEquality, false, "!="},
        {Tok::Less, BinOp::Lt, kRelational, false, "<"},
        {Tok::Greater, BinOp::Gt, kRelational, false, ">"},
        {Tok::LessEq, BinOp::Le, kRelational, false, "<="},
        {Tok::GreaterEq, BinOp::Ge, kRelational, false, ">="},
        {Tok::Shl, BinOp::Shl, kShift, false, "<<"},
        {Tok::Shr, BinOp::Shr, kShift, false, ">>"},
        {Tok::Plus, BinOp::Add, kAdditive, false, "+"},
        {Tok::Minus, BinOp::Sub, kAdditive, false, "-"},
        {Tok::Star, BinOp::Mul, kMultiplicative, false, "*"},
        {Tok::Slash, BinOp::Div, kMultiplicative, false, "/"},
        {Tok::Percent, BinOp::Mod, kMultiplicative, false, "%"},
        {Tok::Caret, BinOp::Pow, kPow, powRight, "^"},
    }};
    t.unaryPrecedence = opts.unaryBindsTighterThanPow ? kUnaryTight : kUnaryDefault;
    return t;
}

const char* binOpText(BinOp op) {
    static const OperatorTable t = OperatorTable::make();
    for (const auto& b : t.binary)
        if (b.op == op) return b.text;
    return "?";
}

}  // namespace reforce::expr
