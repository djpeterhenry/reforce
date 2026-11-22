#pragma once
// Resolver + compiler: AST -> bytecode Program.
//
// Every identifier resolves at compile time (case-insensitively) to one of:
//   user var slot (A#/B#/C#/D#) | built-in variable | key reference | unknown.
// Unknown identifiers and functions are warnings and evaluate to 0.
// Pure sub-expressions over constants are folded (e.g. 500-element literals).

#include <map>
#include <string>
#include <string_view>

#include "expr/Ast.hpp"
#include "expr/Diagnostics.hpp"
#include "expr/Operators.hpp"
#include "expr/Program.hpp"
#include "expr/Vars.hpp"

namespace reforce::expr {

struct NameRef {
    enum class Kind { UserVar, BuiltinVar, KeyRef, Unknown };
    Kind kind = Kind::Unknown;
    int family = 0;   // UserVar
    int index = 0;    // UserVar
    Var var = Var::T; // BuiltinVar
    int keySlot = -1; // KeyRef
    bool nonNative = false;  // BuiltinVar not provided by this scene type (warned, reads its binding, normally 0)
};

class NameResolver {
public:
    virtual ~NameResolver() = default;
    virtual NameRef resolve(std::string_view name) const = 0;
};

// Resolves user vars, every built-in variable (all native), and an optional
// set of keys (upper-case name -> key slot). Used by tests and tools.
class BasicResolver : public NameResolver {
public:
    std::map<std::string, int> keys;
    NameRef resolve(std::string_view name) const override;
};

Program compile(const Node* root, const NameResolver& names, Diagnostics& diags, const LanguageOptions& opts = {},
                int line = 0, const std::string& key = {});

// Parse + compile. Empty expressions yield an undefined Program (defined == false).
// Parse errors yield a Program with failed == true that evaluates to 0.
Program compileExpression(std::string_view src, const NameResolver& names, Diagnostics& diags,
                          const LanguageOptions& opts = {}, int line = 0, const std::string& key = {});

}  // namespace reforce::expr
