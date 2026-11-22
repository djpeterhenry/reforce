#pragma once
// Expression tokenizer. Comments (// and /* */) inside an expression string are
// skipped. Lenient fixes (each recorded as a warning):
//   1L / 1f      C-style numeric suffix dropped
//   .5.5         second decimal point dropped (".55")

#include <string>
#include <string_view>
#include <vector>

#include "expr/Diagnostics.hpp"

namespace reforce::expr {

enum class Tok {
    Number,
    Ident,
    LParen, RParen, LBrace, RBrace, Comma,
    // operators
    Plus, Minus, Star, Slash, Percent, Caret, CaretCaret,
    EqEq, NotEq, Less, Greater, LessEq, GreaterEq,
    AndAnd, OrOr, Amp, Pipe, Shl, Shr, Bang,
    End,
};

struct Token {
    Tok kind = Tok::End;
    double number = 0.0;
    std::string text;  // identifier spelling or operator text
    int pos = 0;       // byte offset into the expression
};

const char* tokName(Tok t);

// Tokenizes `src`. Always ends with a Tok::End token. Unknown characters are
// reported as errors and skipped. `line`/`key` are used only for diagnostics.
std::vector<Token> tokenize(std::string_view src, Diagnostics& diags, int line = 0, const std::string& key = {});

}  // namespace reforce::expr
