#pragma once
// Expression syntax tree produced by the Pratt parser.

#include <memory>
#include <string>
#include <vector>

#include "expr/Operators.hpp"

namespace reforce::expr {

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
    enum class Kind { Number, Ident, Unary, Binary, Call, Matrix };

    Kind kind = Kind::Number;
    double number = 0.0;          // Number
    std::string name;             // Ident / Call (original spelling)
    UnOp unop = UnOp::Neg;        // Unary
    BinOp binop = BinOp::Add;     // Binary
    std::vector<NodePtr> kids;    // Unary: 1, Binary: 2, Call: args, Matrix: elements
    int pos = 0;                  // byte offset of the token that started this node

    static NodePtr num(double v, int pos = 0);
    static NodePtr ident(std::string name, int pos = 0);
    static NodePtr unary(UnOp op, NodePtr a, int pos = 0);
    static NodePtr binary(BinOp op, NodePtr a, NodePtr b, int pos = 0);
};

// Fully parenthesized rendering, used by tests to pin precedence:
// "-2^2" -> "(-(2 ^ 2))".
std::string toSExpr(const Node& n);

}  // namespace reforce::expr
