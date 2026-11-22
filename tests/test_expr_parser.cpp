#include <doctest/doctest.h>

#include "expr_test_util.hpp"
#include "expr/Lexer.hpp"

using namespace reforce::expr;
using reforce::test::eval;
using reforce::test::num;
using reforce::test::sexpr;

TEST_CASE("lexer: number formats") {
    Diagnostics d;
    auto t = tokenize("1 0.5 .5 5. 1e30 1.5e-3 2E+2 0x1F", d);
    REQUIRE(t.size() == 9);
    CHECK(t[0].number == 1);
    CHECK(t[1].number == 0.5);
    CHECK(t[2].number == 0.5);
    CHECK(t[3].number == 5);
    CHECK(t[4].number == 1e30);
    CHECK(t[5].number == doctest::Approx(1.5e-3));
    CHECK(t[6].number == 200);
    CHECK(t[7].number == 31);
    CHECK(t[8].kind == Tok::End);
    CHECK(d.empty());
}

TEST_CASE("lexer: operators, identifiers and comments") {
    Diagnostics d;
    auto t = tokenize("a0^^B1 <= c // tail \" quote\n + /* block */ !x && y || z != w == q >= r", d);
    std::vector<Tok> kinds;
    for (auto& tk : t) kinds.push_back(tk.kind);
    std::vector<Tok> want = {Tok::Ident, Tok::CaretCaret, Tok::Ident, Tok::LessEq, Tok::Ident, Tok::Plus,
                             Tok::Bang, Tok::Ident, Tok::AndAnd, Tok::Ident, Tok::OrOr, Tok::Ident,
                             Tok::NotEq, Tok::Ident, Tok::EqEq, Tok::Ident, Tok::GreaterEq, Tok::Ident, Tok::End};
    CHECK(kinds == want);
    CHECK(d.empty());
}

TEST_CASE("lexer: lenient fixes are warnings") {
    Diagnostics d;
    auto t = tokenize("1L + 2f", d);
    CHECK(t[0].number == 1);
    CHECK(t[2].number == 2);
    CHECK(d.countKind("numeric-suffix") == 2);
    CHECK(d.errorCount() == 0);

    Diagnostics d2;
    auto t2 = tokenize(".5.5+s", d2);
    CHECK(t2[0].number == doctest::Approx(0.55));
    CHECK(t2[1].kind == Tok::Plus);
    CHECK(d2.countKind("malformed-number") == 1);
    CHECK(d2.errorCount() == 0);

    // An identifier starting with L after a number is not a suffix.
    Diagnostics d3;
    auto t3 = tokenize("2Lx", d3);
    CHECK(t3[0].kind == Tok::Number);
    CHECK(t3[1].kind == Tok::Ident);
}

TEST_CASE("lexer: unknown characters are errors") {
    Diagnostics d;
    tokenize("1 $ 2", d);
    CHECK(d.countKind("bad-character") == 1);
    CHECK(d.errorCount() == 1);
}

TEST_CASE("precedence: probe-confirmed defaults") {
    // Probes (2026-11): -1^2 == 1, 2^3^2 == 64.
    CHECK(sexpr("-1^2") == "((-1) ^ 2)");
    CHECK(num("-1^2") == 1);
    CHECK(num("-2^2") == 4);
    CHECK(sexpr("2^3^2") == "((2 ^ 3) ^ 2)");
    CHECK(num("2^3^2") == 64);
    CHECK(num("2^-1") == 0.5);
    CHECK(num("2^-1*4") == 2);  // (2^-1)*4
    CHECK(sexpr("a*b^c") == "(a * (b ^ c))");
    CHECK(sexpr("-a*b") == "((-a) * b)");
}

TEST_CASE("precedence: spec-guess flags still selectable") {
    LanguageOptions spec;
    spec.unaryBindsTighterThanPow = false;
    spec.powRightAssociative = true;
    CHECK(sexpr("-1^2", spec) == "(-(1 ^ 2))");
    CHECK(num("-1^2", spec) == -1);
    CHECK(num("2^3^2", spec) == 512);
    CHECK(sexpr("2^3^2", spec) == "(2 ^ (3 ^ 2))");
}

TEST_CASE("precedence: C-like ladder") {
    CHECK(num("1 + 2 * 3") == 7);
    CHECK(num("10 - 4 - 3") == 3);
    CHECK(num("12 / 3 / 2") == 2);
    CHECK(num("1 + 2 < 4") == 1);
    CHECK(num("1 < 2 == 1") == 1);
    CHECK(num("1 || 0 && 0") == 1);         // && binds tighter than ||
    CHECK(sexpr("a || b && c") == "(a || (b && c))");
    CHECK(sexpr("a | b ^^ c & d") == "(a | (b ^^ (c & d)))");
    CHECK(sexpr("a == b < c") == "(a == (b < c))");
    CHECK(sexpr("a << b + c") == "(a << (b + c))");
    CHECK(sexpr("!a * 2 - 1") == "(((!a) * 2) - 1)");
    CHECK(num("11 % 3") == 2);
    CHECK(num("(int) 2.7 + (float) .5") == 2.5);
}

TEST_CASE("parser: calls, matrices, nesting") {
    CHECK(sexpr("max(1, min(2, 3))") == "max(1, min(2, 3))");
    CHECK(sexpr("clock()") == "clock()");
    CHECK(sexpr("{1, -2, a}") == "{1, (-2), a}");
    CHECK(sexpr("-{1, 2} * .4") == "((-{1, 2}) * 0.4)");
    CHECK(sexpr("{ {1, 2}, {3, 4} }") == "{{1, 2}, {3, 4}}");
    CHECK(sexpr("  ( ( x ) ) ") == "x");
}

TEST_CASE("parser: multi-line expressions with comments") {
    Diagnostics d;
    auto r = parseExpression("( a0 + 1 ) * { cos( b0 )\n , sin( b0 ) // trailing note\n }", d);
    REQUIRE(r.root);
    CHECK(d.empty());
    CHECK(toSExpr(*r.root) == "((a0 + 1) * {cos(b0), sin(b0)})");
}

TEST_CASE("parser: errors and empty expressions") {
    Diagnostics d;
    auto e = parseExpression("   /* nothing */ ", d);
    CHECK(e.empty);
    CHECK(!e.root);
    CHECK(d.empty());

    for (const char* bad : {"1 +", "(1", "f(1 2)", "{1 2}", "1 2", ")"}) {
        Diagnostics de;
        auto r = parseExpression(bad, de);
        CHECK_MESSAGE(!r.root, bad);
        CHECK_MESSAGE(de.countKind("parse-error") == 1, bad);
    }
}

TEST_CASE("parser: deep nesting is bounded") {
    std::string deep(2000, '(');
    deep += "1";
    deep += std::string(2000, ')');
    Diagnostics d;
    auto r = parseExpression(deep, d);
    CHECK(!r.root);
    CHECK(d.errorCount() == 1);
}

TEST_CASE("compile: failed parse evaluates to 0 and is flagged") {
    Diagnostics d;
    BasicResolver names;
    Program p = compileExpression("1 +* 2", names, d);
    CHECK(p.defined);
    CHECK(p.failed);
    EvalContext ctx;
    CHECK(run(p, ctx).scalar() == 0);
}

TEST_CASE("compile: constant folding") {
    Diagnostics d;
    BasicResolver names;
    Program p = compileExpression("2 * pi * sqrt(4) + {1,2,3} * 0", names, d);
    CHECK(d.empty());
    REQUIRE(p.isConstant());
    Program q = compileExpression("sin(t) * 2", names, d);
    CHECK_FALSE(q.isConstant());
    Program r = compileExpression("rnd(1)", names, d);
    CHECK_FALSE(r.isConstant());  // impure: never folded
    CHECK(r.usesRandom);
    Program s = compileExpression("mag(.5)", names, d);
    CHECK(s.usesAudio);
}
