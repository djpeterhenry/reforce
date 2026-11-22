#include <doctest/doctest.h>

#include <cmath>
#include <set>
#include <vector>

#include "expr/Builtins.hpp"
#include "expr_test_util.hpp"

using namespace reforce::expr;
using doctest::Approx;
using reforce::test::eval;
using reforce::test::num;

namespace {
std::string str(std::string_view src) { return eval(src).toString(); }
}  // namespace

TEST_CASE("builtins: every corpus function is registered") {
    // corpus-stats.md "All functions": 54 names.
    const char* corpus[] = {"rnd", "sin", "cos", "floor", "sgn", "col", "trwv", "abs", "fft", "mag", "clip", "sign",
                            "row", "select", "choice", "sqrt", "wrap", "sqwv", "sqr", "pos", "round", "tan",
                            "vectorlr", "map1d", "vector", "min", "atan", "log", "flip", "max", "trunc", "transpose",
                            "srand", "cols", "clamp", "asin", "log10", "exp", "atan2", "hypot", "pow", "smooth",
                            "subrange", "rows", "map2d", "dot", "noise", "resample", "largest", "dim", "ceil", "acos",
                            "hypot2", "leg"};
    CHECK(sizeof(corpus) / sizeof(corpus[0]) == 54);
    for (const char* f : corpus) CHECK_MESSAGE(findBuiltin(f) >= 0, f);
    CHECK(findBuiltin("MAP1D") == findBuiltin("map1D"));
    CHECK(findBuiltin("ABS") >= 0);
    CHECK(findBuiltin("nosuch") < 0);
}

TEST_CASE("builtins: common math") {
    CHECK(num("abs(-3)") == 3);
    CHECK(num("dim(5, 3)") == 2);
    CHECK(num("dim(3, 5)") == 0);
    CHECK(num("sqr(-3)") == 9);
    CHECK(num("sqrt(16)") == 4);
    CHECK(num("exp(0)") == 1);
    CHECK(num("log(exp(2))") == Approx(2));
    CHECK(num("log10(1000)") == Approx(3));
    CHECK(num("pow(2, 10)") == 1024);
    CHECK(num("sin(pi/2)") == Approx(1));
    CHECK(num("cos(pi)") == Approx(-1));
    CHECK(num("tan(pi/4)") == Approx(1));
    CHECK(num("asin(1)") == Approx(M_PI / 2));
    CHECK(num("acos(-1)") == Approx(M_PI));
    CHECK(num("atan(1)") == Approx(M_PI / 4));
    CHECK(num("atan2(1, -1)") == Approx(3 * M_PI / 4));
    CHECK(num("atan2(1)") == Approx(M_PI / 4));  // deprecated 1-arg form == atan
    CHECK(num("hypot(3, 4)") == 5);
    CHECK(num("hypot2(3, 4)") == 25);
    CHECK(num("leg(5, 3)") == 4);
    CHECK(num("leg(3, 5)") == 0);
    CHECK(num("wyvill(0)") == 1);
    CHECK(num("wyvill(1)") == Approx(0));
    CHECK(num("min(2, -1)") == -1);
    CHECK(num("max(2, -1)") == 2);
    CHECK(num("min({3, 1, 2})") == 1);  // 1-arg reduce (guess)
    CHECK(num("max({3, 1, 2})") == 3);
}

TEST_CASE("builtins: logical and discrete") {
    CHECK(num("pos(-2)") == 0);
    CHECK(num("pos(2)") == 2);
    CHECK(num("sign(0)") == 1);  // [CP]
    CHECK(num("sign(-0.1)") == -1);
    CHECK(num("sgn(-5)") == -1);
    CHECK(num("clamp(1.5)") == 1);
    CHECK(num("clamp(-1)") == 0);
    CHECK(num("clip(.3)") == Approx(.3));
    CHECK(num("sqwv(1)") == 1);
    CHECK(num("sqwv(-1.01)") == 0);
    CHECK(num("select(1, 2, 3)") == 2);
    CHECK(num("select(0, 2, 3)") == 3);
    CHECK(str("select({1, 0, 1}, 5, {7, 8, 9})") == "{5, 8, 5}");
    CHECK(str("largest({{1, 5, 3}, {9, 8, 7}})") == "{{5}, {8}}");
    CHECK(num("largest({1, 5, 3})") == 5);
    CHECK(num("largest(-{1, 5, 3})") == -1);
}

TEST_CASE("builtins: rounding and waves") {
    CHECK(num("trunc(-2.7)") == -2);
    CHECK(num("trnc(2.7)") == 2);
    CHECK(num("floor(-2.7)") == -3);
    CHECK(num("flor(2.7)") == 2);
    CHECK(num("ceil(2.1)") == 3);
    CHECK(num("round(2.5)") == 3);
    CHECK(num("round(-2.5)") == -3);
    CHECK(num("wrap(-2.7)") == Approx(.3));  // [CP]
    CHECK(num("wrap(3011.34)") == Approx(.34));
    // trwv through (0,0), (1,1), (2,0)
    CHECK(num("trwv(0)") == 0);
    CHECK(num("trwv(.5)") == .5);
    CHECK(num("trwv(1)") == 1);
    CHECK(num("trwv(1.5)") == .5);
    CHECK(num("trwv(2)") == 0);
    CHECK(num("trwv(-1)") == 1);
    // flip (guess): sawtooth through (-3,-1), (-1,1)|(-1,-1), (1,1)|(1,-1)
    CHECK(num("flip(0)") == 0);
    CHECK(num("flip(-3)") == -1);
    CHECK(num("flip(-1.0001)") == Approx(1).epsilon(1e-3));
    CHECK(num("flip(-1)") == -1);
    CHECK(num("flip(.5)") == .5);
    CHECK(num("flip(1)") == -1);
}

TEST_CASE("builtins: rnd / srand / choice determinism") {
    EvalContext a, b;
    a.rng.reseed(42);
    b.rng.reseed(42);
    for (int i = 0; i < 20; ++i) {
        const double x = eval("rnd(10)", a).scalar();
        CHECK(x >= 0);
        CHECK(x <= 10);
        CHECK(x == eval("rnd(10)", b).scalar());
    }
    // srand reseeds and returns its argument.
    EvalContext c;
    CHECK(eval("srand(7)", c).scalar() == 7);
    const double first = eval("rnd(1)", c).scalar();
    eval("srand(7)", c);
    CHECK(eval("rnd(1)", c).scalar() == first);
    eval("seed(8)", c);
    CHECK(eval("rnd(1)", c).scalar() != first);

    // rnd keeps the argument's shape.
    EvalContext d;
    Value v = eval("rnd({1, 2, 3})", d);
    CHECK(v.cols() == 3);
    CHECK(v.mat().d[1] <= 2);

    // Per-lane randomness: scalar rnd returns a row when randomLanes > 1.
    EvalContext e;
    e.randomLanes = 5;
    Value lanes = eval("rnd(1)", e);
    CHECK(lanes.cols() == 5);

    // choice picks one column.
    EvalContext f;
    std::set<double> seen;
    for (int i = 0; i < 200; ++i) seen.insert(eval("choice({4, 5, 6, 7})", f).scalar());
    CHECK(seen == std::set<double>{4, 5, 6, 7});
    CHECK(eval("choice({{1, 2}, {3, 4}})", f).rows() == 2);
    CHECK(num("choice(3)") == 3);
}

TEST_CASE("builtins: mag and fft lookups") {
    EvalContext ctx;
    std::vector<float> wave = {0, .1f, .2f, .3f, .4f};
    std::vector<float> spec = {1, 2, 3};
    ctx.audio = {wave.data(), int(wave.size()), spec.data(), int(spec.size())};
    CHECK(eval("mag(0)", ctx).scalar() == 0);
    CHECK(eval("mag(1)", ctx).scalar() == Approx(.4));   // mag(1) is the last bin
    CHECK(eval("mag(.5)", ctx).scalar() == Approx(.2));
    CHECK(eval("mag(-.25)", ctx).scalar() == Approx(.3)); // wraps to .75
    CHECK(eval("fft(1)", ctx).scalar() == 3);
    CHECK(eval("fft(.5)", ctx).scalar() == 2);
    CHECK(eval("fft({0, 1})", ctx).toString() == "{1, 3}");
    EvalContext none;
    CHECK(eval("mag(.5) + fft(.5)", none).scalar() == 0);
}

TEST_CASE("builtins: clock, time, print") {
    EvalContext ctx;
    ctx.set(Var::T, 2.5);
    CHECK(eval("clock()", ctx).scalar() == 2500);
    CHECK(eval("time()", ctx).scalar() == 2.5);
    double printed = 0;
    ctx.printHook = [&](const Value& v) { printed = v.scalar(); };
    CHECK(eval("print(3) + 1", ctx).scalar() == 4);
    CHECK(printed == 3);
}

TEST_CASE("builtins: vector constructors and shape queries") {
    CHECK(str("vector(3, 2)") == "{2, 2, 2}");
    CHECK(str("vector(5, {1, 2})") == "{1, 2, 1, 2, 1}");
    CHECK(str("vectorLR(5, 0, 1)") == "{0, 0.25, 0.5, 0.75, 1}");
    CHECK(num("vectorLR(1, 7, 9)") == 7);
    CHECK(num("cols(vectorLR(4, 0, 1))") == 4);
    CHECK(num("rows(vectorLR(4, 0, 1))") == 1);
    CHECK(num("rows(transpose(vectorLR(4, 0, 1)))") == 4);
    CHECK(num("cols(5)") == 1);
    CHECK(num("cols(vector(-3, 1))") == 1);  // counts clamp to >= 1
    CHECK(str("transpose({{1, 2, 3}, {4, 5, 6}})") == "{{1, 4}, {2, 5}, {3, 6}}");
}

TEST_CASE("builtins: row, col, subrange") {
    const char* m = "{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}";
    CHECK(str(std::string("row(") + m + ", 1)") == "{4, 5, 6}");
    CHECK(str(std::string("row(") + m + ", -1)") == "{7, 8, 9}");
    CHECK(str(std::string("row(") + m + ", 99)") == "{7, 8, 9}");  // clamped
    CHECK(str(std::string("col(") + m + ", 0)") == "{{1}, {4}, {7}}");
    CHECK(num("col({10, 20, 30}, 2)") == 30);
    CHECK(str("col({10, 20, 30}, {2, 0})") == "{30, 10}");
    CHECK(str(std::string("subrange(") + m + ", 1, 0, -1, 1)") == "{{2, 3}, {5, 6}}");
    CHECK(str(std::string("subrange(") + m + ", 0, 1, -1, -1)") == "{{4, 5, 6}, {7, 8, 9}}");
    CHECK(num(std::string("subrange(") + m + ", 2, 2, 2, 2)") == 9);
}

TEST_CASE("builtins: map1D / map2D / resample") {
    // map1D: periodic lookup across a row.
    CHECK(num("map1D({0, 10, 20, 30}, 0, 0)") == 0);
    CHECK(num("map1D({0, 10, 20, 30}, .25, 0)") == 10);
    CHECK(num("map1D({0, 10, 20, 30}, 1, 0)") == 0);      // wraps
    CHECK(num("map1D({0, 10, 20, 30}, .125, 1)") == 5);   // linear
    CHECK(num("map1D({0, 10, 20, 30}, .875, 1)") == 15);  // between last and first
    CHECK(str("map1D({0, 10, 20, 30}, {0, .5}, 0)") == "{0, 20}");
    CHECK(eval("map1D({{0, 10}, {1, 11}}, {0, .5}, 0)").rows() == 2);
    // map2D: (0,0) top-left, clamped.
    const char* f = "{{0, 1}, {2, 3}}";
    CHECK(num(std::string("map2D(") + f + ", 0, 0, 0)") == 0);
    CHECK(num(std::string("map2D(") + f + ", 1, 0, 0)") == 1);
    CHECK(num(std::string("map2D(") + f + ", 0, 1, 0)") == 2);
    CHECK(num(std::string("map2D(") + f + ", 5, 5, 0)") == 3);
    CHECK(num(std::string("map2D(") + f + ", .5, .5, 1)") == 1.5);
    CHECK(str(std::string("map2D(") + f + ", {0, 1}, 1, 1)") == "{2, 3}");
    // resample corner to corner.
    CHECK(str("resample({0, 10}, 3, 1, 1)") == "{0, 5, 10}");
    CHECK(str("resample({0, 10}, 3, 1, 0)") == "{0, 10, 10}");
    CHECK(eval("resample({{1, 2}, {3, 4}}, 4, 3, 1)").rows() == 3);
}

TEST_CASE("builtins: smooth, dot, noise") {
    // A constant row stays constant; total mass preserved for wrapped smoothing.
    CHECK(str("smooth({2, 2, 2, 2}, 1.5)") == "{2, 2, 2, 2}");
    Value s = eval("smooth({0, 0, 4, 0, 0}, 1)");
    CHECK(s.mat().d[2] < 4);
    CHECK(s.mat().d[1] > 0);
    Value w = eval("smooth({4, 0, 0, 0, 0, 0}, -1)");
    double sum = 0;
    for (double x : w.mat().d) sum += x;
    CHECK(sum == Approx(4));
    CHECK(w.mat().d[5] > 0);  // wrapped around
    CHECK(str("smooth({1, 2}, 0)") == "{1, 2}");

    CHECK(num("dot({1, 2, 3}, {4, 5, 6})") == 32);
    CHECK(num("dot({3, 4})") == 25);
    CHECK(num("dot({1, 2, 3}, 1)") == 6);
    CHECK(str("dot({{1, 2}, {3, 4}})") == "{10, 20}");  // column-wise

    EvalContext a, b;
    a.rng.reseed(3);
    b.rng.reseed(3);
    Value n1 = eval("noise(8, 4, 3)", a), n2 = eval("noise(8, 4, 3)", b);
    CHECK(n1.cols() == 8);
    CHECK(n1.rows() == 4);
    CHECK(n1.mat().d == n2.mat().d);
    for (double x : n1.mat().d) {
        CHECK(x >= 0);
        CHECK(x <= 1);
    }
}

TEST_CASE("builtins: elementwise math over matrices") {
    CHECK(str("abs({-1, 2})") == "{1, 2}");
    CHECK(str("max({1, 5}, 3)") == "{3, 5}");
    CHECK(str("pow(2, {1, 2, 3})") == "{2, 4, 8}");
    CHECK(str("floor({1.5, -1.5})") == "{1, -2}");
}

TEST_CASE("builtins: arity problems are warnings, not errors") {
    Diagnostics d;
    CHECK(eval("sin()", {}, &d).scalar() == 0);
    CHECK(eval("sin(1, 2)", {}, &d).scalar() == Approx(std::sin(1)));
    CHECK(d.countKind("arity") == 2);
    CHECK(d.errorCount() == 0);
    Diagnostics u;
    CHECK(eval("nosuch(1) + 2", {}, &u).scalar() == 2);
    CHECK(u.countKind("unknown-function") == 1);
}
