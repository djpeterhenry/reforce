#include "expr/Parser.hpp"

#include <cstdio>
#include <stdexcept>

#include "expr/Lexer.hpp"

namespace reforce::expr {

NodePtr Node::num(double v, int pos) {
    auto n = std::make_unique<Node>();
    n->kind = Kind::Number;
    n->number = v;
    n->pos = pos;
    return n;
}

NodePtr Node::ident(std::string name, int pos) {
    auto n = std::make_unique<Node>();
    n->kind = Kind::Ident;
    n->name = std::move(name);
    n->pos = pos;
    return n;
}

NodePtr Node::unary(UnOp op, NodePtr a, int pos) {
    auto n = std::make_unique<Node>();
    n->kind = Kind::Unary;
    n->unop = op;
    n->kids.push_back(std::move(a));
    n->pos = pos;
    return n;
}

NodePtr Node::binary(BinOp op, NodePtr a, NodePtr b, int pos) {
    auto n = std::make_unique<Node>();
    n->kind = Kind::Binary;
    n->binop = op;
    n->kids.push_back(std::move(a));
    n->kids.push_back(std::move(b));
    n->pos = pos;
    return n;
}

std::string toSExpr(const Node& n) {
    switch (n.kind) {
        case Node::Kind::Number: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", n.number);
            return buf;
        }
        case Node::Kind::Ident: return n.name;
        case Node::Kind::Unary: {
            const char* op = n.unop == UnOp::Neg ? "-" : n.unop == UnOp::Plus ? "+" : n.unop == UnOp::Not ? "!"
                           : n.unop == UnOp::CastInt ? "(int)" : "(float)";
            return std::string("(") + op + toSExpr(*n.kids[0]) + ")";
        }
        case Node::Kind::Binary:
            return "(" + toSExpr(*n.kids[0]) + " " + binOpText(n.binop) + " " + toSExpr(*n.kids[1]) + ")";
        case Node::Kind::Call: {
            std::string s = n.name + "(";
            for (std::size_t i = 0; i < n.kids.size(); ++i) s += (i ? ", " : "") + toSExpr(*n.kids[i]);
            return s + ")";
        }
        case Node::Kind::Matrix: {
            std::string s = "{";
            for (std::size_t i = 0; i < n.kids.size(); ++i) s += (i ? ", " : "") + toSExpr(*n.kids[i]);
            return s + "}";
        }
    }
    return "?";
}

namespace {

struct ParseError : std::runtime_error {
    int pos;
    ParseError(const std::string& m, int p) : std::runtime_error(m), pos(p) {}
};

class Parser {
public:
    Parser(const std::vector<Token>& toks, const OperatorTable& table) : t_(toks), table_(table) {}

    NodePtr parseAll() {
        NodePtr n = expr(0);
        if (peek().kind != Tok::End)
            throw ParseError(std::string("unexpected '") + describe(peek()) + "' after a complete expression",
                             peek().pos);
        return n;
    }

private:
    static constexpr int kMaxDepth = 512;

    const Token& peek(int k = 0) const {
        const std::size_t j = i_ + std::size_t(k);
        return j < t_.size() ? t_[j] : t_.back();
    }
    const Token& take() {
        const Token& tk = peek();
        if (tk.kind != Tok::End) ++i_;
        return tk;
    }
    static std::string describe(const Token& tk) {
        if (tk.kind == Tok::Number || tk.kind == Tok::Ident) return tk.text;
        return tokName(tk.kind);
    }
    void expect(Tok k, const char* context) {
        const Token& tk = take();
        if (tk.kind != k)
            throw ParseError(std::string("expected '") + tokName(k) + "' " + context + ", got '" + describe(tk) + "'",
                             tk.pos);
    }

    struct DepthGuard {
        int& d;
        DepthGuard(int& depth, int pos) : d(depth) {
            if (++d > kMaxDepth) throw ParseError("expression nested too deeply", pos);
        }
        ~DepthGuard() { --d; }
    };

    NodePtr expr(int minPrec) {
        DepthGuard g(depth_, peek().pos);
        NodePtr lhs = prefix();
        for (;;) {
            const BinOpInfo* op = table_.find(peek().kind);
            if (!op || op->precedence < minPrec) break;
            const int pos = take().pos;
            NodePtr rhs = expr(op->rightAssoc ? op->precedence : op->precedence + 1);
            lhs = Node::binary(op->op, std::move(lhs), std::move(rhs), pos);
        }
        return lhs;
    }

    NodePtr prefix() {
        const Token& tk = peek();
        const int pos = tk.pos;
        if (tk.kind == Tok::Minus || tk.kind == Tok::Plus || tk.kind == Tok::Bang) {
            const UnOp op = tk.kind == Tok::Minus ? UnOp::Neg : tk.kind == Tok::Plus ? UnOp::Plus : UnOp::Not;
            take();
            // The operand absorbs only operators that bind tighter than unary
            // (by default `^`, so -2^2 == -(2^2)).
            return Node::unary(op, expr(table_.unaryPrecedence), pos);
        }
        // Casts: (int) x, (float) x
        if (tk.kind == Tok::LParen && peek(1).kind == Tok::Ident && peek(2).kind == Tok::RParen) {
            const std::string& w = peek(1).text;
            const bool isInt = w == "int", isFloat = w == "float";
            if (isInt || isFloat) {
                i_ += 3;
                return Node::unary(isInt ? UnOp::CastInt : UnOp::CastFloat, expr(table_.unaryPrecedence), pos);
            }
        }
        return primary();
    }

    NodePtr primary() {
        const Token& tk = take();
        switch (tk.kind) {
            case Tok::Number: return Node::num(tk.number, tk.pos);
            case Tok::Ident: {
                if (peek().kind == Tok::LParen) {
                    take();
                    auto n = std::make_unique<Node>();
                    n->kind = Node::Kind::Call;
                    n->name = tk.text;
                    n->pos = tk.pos;
                    if (peek().kind == Tok::RParen) {
                        take();
                        return n;
                    }
                    for (;;) {
                        n->kids.push_back(expr(0));
                        const Token& sep = take();
                        if (sep.kind == Tok::RParen) break;
                        if (sep.kind != Tok::Comma)
                            throw ParseError("expected ',' or ')' in call to " + tk.text + ", got '" + describe(sep) + "'",
                                             sep.pos);
                    }
                    return n;
                }
                return Node::ident(tk.text, tk.pos);
            }
            case Tok::LParen: {
                NodePtr n = expr(0);
                expect(Tok::RParen, "to close '('");
                return n;
            }
            case Tok::LBrace: {
                auto n = std::make_unique<Node>();
                n->kind = Node::Kind::Matrix;
                n->pos = tk.pos;
                if (peek().kind == Tok::RBrace) {
                    take();
                    return n;
                }
                for (;;) {
                    n->kids.push_back(expr(0));
                    const Token& sep = take();
                    if (sep.kind == Tok::RBrace) break;
                    if (sep.kind != Tok::Comma)
                        throw ParseError("expected ',' or '}' in matrix literal, got '" + describe(sep) + "'", sep.pos);
                }
                return n;
            }
            case Tok::End: throw ParseError("unexpected end of expression", tk.pos);
            default: throw ParseError("unexpected '" + describe(tk) + "'", tk.pos);
        }
    }

    const std::vector<Token>& t_;
    const OperatorTable& table_;
    std::size_t i_ = 0;
    int depth_ = 0;
};

}  // namespace

ParseResult parseExpression(std::string_view src, Diagnostics& diags, const LanguageOptions& opts, int line,
                            const std::string& key) {
    ParseResult r;
    const auto before = diags.errorCount();
    auto toks = tokenize(src, diags, line, key);
    if (toks.size() == 1) {
        r.empty = true;
        r.ok = diags.errorCount() == before;
        return r;
    }
    const OperatorTable table = OperatorTable::make(opts);
    try {
        Parser p(toks, table);
        r.root = p.parseAll();
        r.ok = diags.errorCount() == before;
    } catch (const ParseError& e) {
        std::string snippet(src.substr(0, 80));
        for (char& c : snippet)
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        diags.error("parse-error", std::string(e.what()) + " at offset " + std::to_string(e.pos) + " in \"" + snippet +
                                       (src.size() > 80 ? "...\"" : "\""),
                    line, key);
        r.root.reset();
        r.ok = false;
    }
    return r;
}

}  // namespace reforce::expr
