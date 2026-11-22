#pragma once
// Helpers shared by the expression-engine tests.

#include <string>
#include <string_view>

#include "expr/Compiler.hpp"
#include "expr/Parser.hpp"
#include "expr/Vm.hpp"

namespace reforce::test {

using namespace reforce::expr;

inline Value eval(std::string_view src, EvalContext& ctx, const LanguageOptions& opts = {},
                  Diagnostics* diagsOut = nullptr) {
    Diagnostics d;
    BasicResolver names;
    Program p = compileExpression(src, names, d, opts);
    for (const auto& [fam, idx] : p.userRefs) ctx.ensureSlots(fam, idx + 1);
    if (diagsOut) diagsOut->append(d);
    return run(p, ctx);
}

inline Value eval(std::string_view src, const LanguageOptions& opts = {}, Diagnostics* diagsOut = nullptr) {
    EvalContext ctx;
    return eval(src, ctx, opts, diagsOut);
}

inline double num(std::string_view src, const LanguageOptions& opts = {}) { return eval(src, opts).scalar(); }

inline std::string sexpr(std::string_view src, const LanguageOptions& opts = {}) {
    Diagnostics d;
    auto r = parseExpression(src, d, opts);
    return r.root ? toSExpr(*r.root) : std::string("<error>");
}

}  // namespace reforce::test
