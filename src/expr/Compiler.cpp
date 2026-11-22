#include "expr/Compiler.hpp"

#include <algorithm>
#include <cmath>

#include "expr/Builtins.hpp"
#include "expr/Parser.hpp"
#include "expr/Vm.hpp"

namespace reforce::expr {

NameRef BasicResolver::resolve(std::string_view name) const {
    NameRef r;
    if (parseUserVar(name, r.family, r.index)) {
        r.kind = NameRef::Kind::UserVar;
        return r;
    }
    if (auto v = lookupVar(name)) {
        r.kind = NameRef::Kind::BuiltinVar;
        r.var = *v;
        return r;
    }
    auto it = keys.find(toUpper(name));
    if (it != keys.end()) {
        r.kind = NameRef::Kind::KeyRef;
        r.keySlot = it->second;
    }
    return r;
}

namespace {

struct Code {
    std::vector<Instr> ins;
    bool isConst = false;
    Value cval;
};

class Compiler {
public:
    Compiler(Program& p, const NameResolver& names, Diagnostics& diags, const LanguageOptions& opts, int line,
             const std::string& key)
        : p_(p), names_(names), diags_(diags), opts_(opts), line_(line), key_(key) {}

    Code node(const Node& n) {
        switch (n.kind) {
            case Node::Kind::Number: return constant(n.number);
            case Node::Kind::Ident: return ident(n);
            case Node::Kind::Unary: return unary(n);
            case Node::Kind::Binary: return binary(n);
            case Node::Kind::Call: return call(n);
            case Node::Kind::Matrix: return matrix(n);
        }
        return constant(0.0);
    }

    void materialize(Code& c) {
        if (!c.isConst) return;
        c.ins.clear();
        c.ins.push_back({Op::PushConst, 0, int(p_.consts.size())});
        p_.consts.push_back(c.cval);
        c.isConst = false;
    }

private:
    static Code constant(Value v) {
        Code c;
        c.isConst = true;
        c.cval = std::move(v);
        return c;
    }

    static Code single(Op op, int arg = 0, int argc = 0) {
        Code c;
        c.ins.push_back({op, std::uint16_t(argc), arg});
        return c;
    }

    void append(Code& dst, Code& src) {
        materialize(src);
        dst.ins.insert(dst.ins.end(), src.ins.begin(), src.ins.end());
    }

    Code ident(const Node& n) {
        const NameRef r = names_.resolve(n.name);
        switch (r.kind) {
            case NameRef::Kind::UserVar: {
                p_.userRefs.emplace_back(r.family, r.index);
                return single(Op(int(Op::LoadA) + r.family), r.index);
            }
            case NameRef::Kind::BuiltinVar: {
                p_.varsUsed |= varBit(r.var);
                if (r.nonNative)
                    diags_.warn("identifier-not-available",
                                "'" + n.name + "' is not provided for this scene type (reads 0)", line_, key_);
                if (r.var == Var::PI) return constant(M_PI);
                return single(Op::LoadVar, int(r.var));
            }
            case NameRef::Kind::KeyRef: {
                if (std::find(p_.keyRefs.begin(), p_.keyRefs.end(), r.keySlot) == p_.keyRefs.end())
                    p_.keyRefs.push_back(r.keySlot);
                return single(Op::LoadKey, r.keySlot);
            }
            case NameRef::Kind::Unknown: break;
        }
        diags_.warn("unknown-identifier", "unknown identifier '" + n.name + "' evaluates to 0", line_, key_);
        return constant(0.0);
    }

    Code unary(const Node& n) {
        Code a = node(*n.kids[0]);
        Op op;
        switch (n.unop) {
            case UnOp::Plus:
            case UnOp::CastFloat: return a;
            case UnOp::Neg: op = Op::Neg; break;
            case UnOp::Not: op = Op::Not; break;
            case UnOp::CastInt: op = Op::CastInt; break;
            default: return a;
        }
        if (a.isConst) return constant(applyUnary(op, a.cval));
        a.ins.push_back({op, 0, 0});
        return a;
    }

    Op binOp(BinOp b) const {
        switch (b) {
            case BinOp::Add: return Op::Add;
            case BinOp::Sub: return Op::Sub;
            case BinOp::Mul: return Op::Mul;
            case BinOp::Div: return Op::Div;
            case BinOp::Mod: return opts_.modulo == ModuloMode::Fmod ? Op::ModF : Op::ModInt;
            case BinOp::Pow: return Op::Pow;
            case BinOp::Eq: return Op::Eq;
            case BinOp::Ne: return Op::Ne;
            case BinOp::Lt: return Op::Lt;
            case BinOp::Gt: return Op::Gt;
            case BinOp::Le: return Op::Le;
            case BinOp::Ge: return Op::Ge;
            case BinOp::LogAnd: return Op::LogAnd;
            case BinOp::LogOr: return Op::LogOr;
            case BinOp::BitAnd: return Op::BitAnd;
            case BinOp::BitOr: return Op::BitOr;
            case BinOp::BitXor: return Op::BitXor;
            case BinOp::Shl: return Op::Shl;
            case BinOp::Shr: return Op::Shr;
        }
        return Op::Add;
    }

    Code binary(const Node& n) {
        Code a = node(*n.kids[0]);
        Code b = node(*n.kids[1]);
        const Op op = binOp(n.binop);
        if (a.isConst && b.isConst) return constant(applyBinary(op, a.cval, b.cval));
        Code out;
        append(out, a);
        append(out, b);
        out.ins.push_back({op, 0, 0});
        return out;
    }

    Code call(const Node& n) {
        const int idx = findBuiltin(n.name);
        if (idx < 0) {
            diags_.warn("unknown-function", "unknown function '" + n.name + "' evaluates to 0", line_, key_);
            return constant(0.0);
        }
        const BuiltinInfo& bi = builtinAt(idx);
        int argc = int(n.kids.size());
        if (argc < bi.minArgs || argc > bi.maxArgs) {
            diags_.warn("arity", std::string(bi.name) + "() called with " + std::to_string(argc) +
                                     " argument(s); expected " + std::to_string(bi.minArgs) +
                                     (bi.maxArgs != bi.minArgs ? ".." + std::to_string(bi.maxArgs) : "") +
                                     " (missing args are 0, extra args ignored)",
                        line_, key_);
        }
        if (bi.random) p_.usesRandom = true;
        if (bi.audio) p_.usesAudio = true;
        const int used = std::min(argc, bi.maxArgs);
        std::vector<Code> args;
        bool allConst = true;
        for (int i = 0; i < used; ++i) {
            args.push_back(node(*n.kids[std::size_t(i)]));
            allConst = allConst && args.back().isConst;
        }
        while (int(args.size()) < bi.minArgs) args.push_back(constant(0.0));
        argc = int(args.size());
        if (bi.pure && allConst) {
            std::vector<Value> vals;
            for (auto& c : args) vals.push_back(c.cval);
            EvalContext scratch;
            return constant(bi.fn(scratch, vals.data(), argc));
        }
        Code out;
        for (auto& c : args) append(out, c);
        if (argc == 1 && bi.s1) out.ins.push_back({Op::CallS1, 1, idx});
        else if (argc == 2 && bi.s2) out.ins.push_back({Op::CallS2, 2, idx});
        else out.ins.push_back({Op::Call, std::uint16_t(argc), idx});
        return out;
    }

    Code matrix(const Node& n) {
        if (n.kids.empty()) {
            diags_.warn("empty-matrix", "empty matrix literal {} evaluates to 0", line_, key_);
            return constant(0.0);
        }
        if (n.kids.size() > 65535) {
            diags_.error("matrix-too-large", "matrix literal has too many elements", line_, key_);
            return constant(0.0);
        }
        std::vector<Code> elems;
        bool allConst = true;
        for (const auto& k : n.kids) {
            elems.push_back(node(*k));
            allConst = allConst && elems.back().isConst;
        }
        if (allConst) {
            std::vector<Value> vals;
            for (auto& c : elems) vals.push_back(c.cval);
            return constant(buildMatrix(vals.data(), int(vals.size())));
        }
        Code out;
        for (auto& c : elems) append(out, c);
        out.ins.push_back({Op::MakeMatrix, std::uint16_t(elems.size()), 0});
        return out;
    }

    Program& p_;
    const NameResolver& names_;
    Diagnostics& diags_;
    const LanguageOptions& opts_;
    int line_;
    const std::string& key_;
};

int computeMaxStack(const std::vector<Instr>& code) {
    int depth = 0, maxDepth = 0;
    for (const Instr& in : code) {
        switch (in.op) {
            case Op::PushConst:
            case Op::LoadA:
            case Op::LoadB:
            case Op::LoadC:
            case Op::LoadD:
            case Op::LoadKey:
            case Op::LoadVar: ++depth; break;
            case Op::Neg:
            case Op::Not:
            case Op::CastInt:
            case Op::CallS1: break;
            case Op::Call:
            case Op::MakeMatrix: depth = depth - in.argc + 1; break;
            default: --depth; break;  // binary ops, CallS2
        }
        maxDepth = std::max(maxDepth, depth);
    }
    return maxDepth;
}

}  // namespace

Program compile(const Node* root, const NameResolver& names, Diagnostics& diags, const LanguageOptions& opts,
                int line, const std::string& key) {
    Program p;
    if (!root) return p;
    p.defined = true;
    Compiler c(p, names, diags, opts, line, key);
    Code code = c.node(*root);
    c.materialize(code);
    p.code = std::move(code.ins);
    // Drop constants orphaned by folding.
    std::vector<Value> used;
    for (Instr& in : p.code)
        if (in.op == Op::PushConst) {
            used.push_back(p.consts[std::size_t(in.arg)]);
            in.arg = int(used.size() - 1);
        }
    p.consts = std::move(used);
    p.maxStack = computeMaxStack(p.code);
    std::sort(p.userRefs.begin(), p.userRefs.end());
    p.userRefs.erase(std::unique(p.userRefs.begin(), p.userRefs.end()), p.userRefs.end());
    return p;
}

Program compileExpression(std::string_view src, const NameResolver& names, Diagnostics& diags,
                          const LanguageOptions& opts, int line, const std::string& key) {
    ParseResult r = parseExpression(src, diags, opts, line, key);
    if (r.empty) return Program{};
    if (!r.root) {
        Program p;
        p.defined = true;
        p.failed = true;
        p.consts.push_back(Value(0.0));
        p.code.push_back({Op::PushConst, 0, 0});
        p.maxStack = 1;
        return p;
    }
    return compile(r.root.get(), names, diags, opts, line, key);
}

}  // namespace reforce::expr
