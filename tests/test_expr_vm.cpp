#include <doctest/doctest.h>

#include <cmath>

#include "expr_test_util.hpp"

using namespace reforce::expr;
using doctest::Approx;
using reforce::test::eval;
using reforce::test::num;

TEST_CASE("vm: scalar arithmetic and comparisons") {
    CHECK(num("1 + 2 * 3 - 4 / 2") == 5);
    CHECK(num("7 % 3") == 1);
    CHECK(num("-7 % 3") == -1);       // sign of the dividend (guess)
    CHECK(num("5.5 % 2") == 1);       // integer remainder (default ModuloMode::IntegerTrunc; corpus sweep)
    CHECK(num("-5.5 % 2") == -1);     // operands truncated toward zero
    CHECK(num("5 % 0") == 0);         // guess: no NaN
    LanguageOptions fmodMod;
    fmodMod.modulo = ModuloMode::Fmod;
    CHECK(num("5.5 % 2", fmodMod) == 1.5);
    CHECK(num("3 == 3") == 1);
    CHECK(num("3 != 3") == 0);
    CHECK(num("2 < 3") + num("3 > 2") + num("3 <= 3") + num("3 >= 4") == 3);
    CHECK(num("!0") == 1);
    CHECK(num("!5") == 0);
    CHECK(num("!(1e20 < 1e30)") == 0);
    CHECK(num("2 && 0") == 0);
    CHECK(num("0 || -1") == 1);
    CHECK(num("6 ^^ 3") == 5);
    CHECK(num("6 & 3") == 2);
    CHECK(num("6 | 3") == 7);
    CHECK(num("1 << 4") == 16);
    CHECK(num("256 >> 4") == 16);
    CHECK(std::isinf(num("1 / 0")));
}

TEST_CASE("vm: matrix literals and broadcasting") {
    CHECK(eval("{1, 2, 3}").toString() == "{1, 2, 3}");
    CHECK(eval("{1, 2, 3} * 2").toString() == "{2, 4, 6}");
    CHECK(eval("10 - {1, 2}").toString() == "{9, 8}");
    CHECK(eval("-{1, 2}").toString() == "{-1, -2}");
    CHECK(eval("{{1, 2}, {3, 4}}").toString() == "{{1, 2}, {3, 4}}");
    CHECK(eval("{{1, 2}, {3, 4}} + {10, 20}").toString() == "{{11, 22}, {13, 24}}");
    CHECK(eval("{1, 2} == {1, 3}").toString() == "{1, 0}");
    CHECK(eval("{{1, 2, 3}, 5}").toString() == "{{1, 2, 3}, {5, 5, 5}}");  // scalar row broadcast (guess)
    CHECK(num("{7}") == 7);  // 1x1 normalizes to scalar
    Value v = eval("transpose({1, 2}) * {10, 20, 30}");  // 2x1 * 1x3 -> 2x3 outer broadcast
    CHECK(v.rows() == 2);
    CHECK(v.cols() == 3);
    CHECK(v.at(1, 2) == 60);
}

TEST_CASE("vm: matrix literal with runtime elements") {
    EvalContext ctx;
    ctx.ensureSlots(FamB, 1);
    ctx.slot(FamB, 0) = 0.5;
    CHECK(eval("{ cos(b0 * 0), sin(B0 * 0), b0 }", ctx).toString() == "{1, 0, 0.5}");
    ctx.set(Var::S, Value::row({0, 0.5, 1}));
    Value m = eval("{ s, s * 2 }", ctx);
    CHECK(m.rows() == 2);
    CHECK(m.cols() == 3);
    CHECK(m.at(1, 2) == 2);
}

TEST_CASE("vm: names are case-insensitive") {
    EvalContext ctx;
    ctx.ensureSlots(FamA, 3);
    ctx.slot(FamA, 2) = 4;
    ctx.set(Var::NUM_S_STEPS, 10);
    CHECK(eval("a2 + A2", ctx).scalar() == 8);
    CHECK(eval("PI - pi + Pi", ctx).scalar() == Approx(M_PI));
    CHECK(eval("Num_S_Steps + NUM_S_STEPS", ctx).scalar() == 20);
    CHECK(eval("ABS(-1) + wraP(1.25) + MAP1D({1,2}, 0, 0)", ctx).scalar() == 2.25);
}

TEST_CASE("vm: unread slots read 0; slots persist between runs") {
    EvalContext ctx;
    BasicResolver names;
    Diagnostics d;
    Program p = compileExpression("B0 + 1", names, d);
    ctx.ensureSlots(FamB, 1);
    for (int frame = 1; frame <= 3; ++frame) {
        ctx.slot(FamB, 0) = run(p, ctx);
        CHECK(ctx.slot(FamB, 0).scalar() == frame);
    }
    // Missing slot arrays (not prepared) read 0 instead of crashing.
    EvalContext bare;
    Program q = compileExpression("C9 + D3 + 1", names, d);
    CHECK(run(q, bare).scalar() == 1);
}

TEST_CASE("vm: key references and unknown identifiers") {
    BasicResolver names;
    names.keys["SIDE"] = 0;
    Diagnostics d;
    Program p = compileExpression("1 + side", names, d);
    REQUIRE(p.keyRefs.size() == 1);
    EvalContext ctx;
    ctx.ensureKeySlots(1);
    ctx.keys[0] = 5;
    CHECK(run(p, ctx).scalar() == 6);

    Diagnostics u;
    Program q = compileExpression("rnd16 + 2", names, u);
    CHECK(u.countKind("unknown-identifier") == 1);
    CHECK(u.errorCount() == 0);
    CHECK(run(q, ctx).scalar() == 2);
}

TEST_CASE("vm: program metadata") {
    BasicResolver names;
    Diagnostics d;
    Program p = compileExpression("A0 + b3 * sin(t) + c1 + id", names, d);
    CHECK(p.usesVar(Var::T));
    CHECK(p.usesVar(Var::ID));
    CHECK_FALSE(p.usesVar(Var::S));
    CHECK(p.userRefs == std::vector<std::pair<int, int>>{{FamA, 0}, {FamB, 3}, {FamC, 1}});
    CHECK(p.maxStack >= 2);
    Program empty = compileExpression("", names, d);
    CHECK_FALSE(empty.defined);
    EvalContext ctx;
    CHECK(run(empty, ctx).scalar() == 0);
}

TEST_CASE("vm: lane helpers") {
    Value row = Value::row({1, 2, 3});
    CHECK(extractLane(row, 1, 3).scalar() == 2);
    CHECK(extractLane(row, 1, 4).cols() == 3);  // width mismatch: unchanged
    CHECK(laneScalar(row, 2, 3) == 3);
    CHECK(laneScalar(row, 4, 7) == 2);          // flat index modulo size
    CHECK(laneScalar(Value(9.0), 5, 10) == 9);
    Value two = eval("{{1, 2}, {3, 4}}");
    Value lane = extractLane(two, 1, 2);
    CHECK(lane.rows() == 2);
    CHECK(lane.at(1, 0) == 4);
}

TEST_CASE("vm: float32 mode rounds every result to single precision (reforce.FlowPrecision)") {
    EvalContext ctx;
    CHECK(eval("1 / 3", ctx).scalar() == 1.0 / 3.0);
    ctx.float32 = true;
    CHECK(eval("1 / 3", ctx).scalar() == double(1.0f / 3.0f));
    CHECK(eval("{1, 2} / 3", ctx).mat().d[1] == double(2.0f / 3.0f));
    CHECK(eval("sqrt( 2 )", ctx).scalar() == double(float(std::sqrt(2.0))));
}
