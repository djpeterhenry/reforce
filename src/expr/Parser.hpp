#pragma once
// Pratt parser: tokens -> AST. Precedence comes from OperatorTable.

#include <string>
#include <string_view>

#include "expr/Ast.hpp"
#include "expr/Diagnostics.hpp"
#include "expr/Operators.hpp"

namespace reforce::expr {

struct ParseResult {
    NodePtr root;    // null when the expression is empty or failed to parse
    bool empty = false;  // only whitespace/comments
    bool ok = false;
};

// Parses a complete expression. Errors go to `diags` (kind "parse-error").
ParseResult parseExpression(std::string_view src, Diagnostics& diags, const LanguageOptions& opts = {},
                            int line = 0, const std::string& key = {});

}  // namespace reforce::expr
