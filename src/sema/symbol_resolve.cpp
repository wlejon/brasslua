#include "brasslua/runtime.h"

#include <unordered_map>
#include <vector>

namespace brasslua {

namespace {

struct DeclEntry {
    uint32_t symbol_id = 0;
    size_t func_depth = 0;
    bool* bool_captured_ptr = nullptr;
    uint8_t* u8_captured_ptr = nullptr;
};

class SymbolResolver {
public:
    void run(Chunk& chunk) {
        func_stack_.push_back(nullptr); // depth 0 = top-level chunk
        pushScope();
        resolveBlockStmts(chunk.body);
        popScope();
        func_stack_.pop_back();
    }

private:
    void pushScope() {
        scopes_.emplace_back();
    }

    void popScope() {
        if (!scopes_.empty()) {
            scopes_.pop_back();
        }
    }

    uint32_t defineSymbol(const std::string& name, bool* bool_cap, uint8_t* u8_cap) {
        uint32_t id = next_symbol_id_++;
        if (bool_cap) *bool_cap = false;
        if (u8_cap) *u8_cap = 0;
        scopes_.back()[name] = DeclEntry{id, func_stack_.size() - 1, bool_cap, u8_cap};
        return id;
    }

    uint32_t useSymbol(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) {
                DeclEntry& entry = found->second;
                size_t cur_depth = func_stack_.size() - 1;
                if (entry.func_depth < cur_depth) {
                    if (entry.bool_captured_ptr) *entry.bool_captured_ptr = true;
                    if (entry.u8_captured_ptr) *entry.u8_captured_ptr = 1;
                    for (size_t d = entry.func_depth + 1; d <= cur_depth; ++d) {
                        FuncBody* fb = func_stack_[d];
                        if (!fb) continue;
                        bool cap_by_inner = (d < cur_depth);
                        bool exists = false;
                        for (auto& cv : fb->captured_vars) {
                            if (cv.symbol_id == entry.symbol_id) {
                                if (cap_by_inner) cv.captured_by_inner = true;
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            fb->captured_vars.push_back(CapturedVar{entry.symbol_id, name, cap_by_inner});
                        }
                    }
                }
                return entry.symbol_id;
            }
        }
        return 0; // global (_ENV)
    }

    void resolveFuncBody(FuncBody& fn, bool has_implicit_self) {
        for (FuncBody* ancestor : func_stack_) {
            if (ancestor) {
                ancestor->has_inner_closures = true;
            }
        }
        fn.has_inner_closures = false;
        fn.captured_vars.clear();

        func_stack_.push_back(&fn);
        pushScope();

        if (has_implicit_self) {
            fn.self_symbol_id = defineSymbol("self", &fn.self_is_captured, nullptr);
        } else {
            fn.self_symbol_id = 0;
            fn.self_is_captured = false;
        }

        fn.param_symbol_ids.resize(fn.params.size());
        fn.param_is_captured.assign(fn.params.size(), 0);
        for (size_t i = 0; i < fn.params.size(); ++i) {
            fn.param_symbol_ids[i] = defineSymbol(fn.params[i], nullptr, &fn.param_is_captured[i]);
        }

        resolveBlockStmts(fn.body);

        popScope();
        func_stack_.pop_back();
    }

    void resolveBlockStmts(std::vector<StmtPtr>& stmts) {
        for (auto& s : stmts) {
            resolveStmt(*s);
        }
    }

    void resolveScopedBlock(std::vector<StmtPtr>& stmts) {
        pushScope();
        resolveBlockStmts(stmts);
        popScope();
    }

    void resolveStmt(Stmt& stmt) {
        std::visit(
            [&](auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, LocalStmt>) {
                    // In Lua, RHS init expressions are evaluated before LHS locals enter scope
                    for (auto& ie : s.init) {
                        resolveExpr(*ie);
                    }
                    for (auto& v : s.vars) {
                        v.symbol_id = defineSymbol(v.name, &v.is_captured, nullptr);
                    }
                } else if constexpr (std::is_same_v<T, AssignStmt>) {
                    for (auto& te : s.targets) {
                        resolveExpr(*te);
                        if (const auto* id = std::get_if<IdentExpr>(&te->node)) {
                            if (id->symbol_id == 0 && id->name == "_ENV") assigns_free_env_ = true;
                        }
                    }
                    for (auto& ve : s.values) {
                        resolveExpr(*ve);
                    }
                } else if constexpr (std::is_same_v<T, ExprStmt>) {
                    resolveExpr(*s.expr);
                } else if constexpr (std::is_same_v<T, BlockStmt>) {
                    resolveScopedBlock(s.body);
                } else if constexpr (std::is_same_v<T, IfStmt>) {
                    for (auto& br : s.branches) {
                        resolveExpr(*br.condition);
                        resolveScopedBlock(br.body);
                    }
                    if (!s.else_body.empty()) {
                        resolveScopedBlock(s.else_body);
                    }
                } else if constexpr (std::is_same_v<T, WhileStmt>) {
                    resolveExpr(*s.condition);
                    resolveScopedBlock(s.body);
                } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                    // In Lua, `until cond` is inside the scope of the repeat body
                    pushScope();
                    resolveBlockStmts(s.body);
                    resolveExpr(*s.until_cond);
                    popScope();
                } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                    resolveExpr(*s.start);
                    resolveExpr(*s.stop);
                    if (s.step) {
                        resolveExpr(*s.step);
                    }
                    pushScope();
                    s.var_symbol_id = defineSymbol(s.var_name, &s.var_is_captured, nullptr);
                    resolveBlockStmts(s.body);
                    popScope();
                } else if constexpr (std::is_same_v<T, GenericForStmt>) {
                    for (auto& ie : s.iter_exprs) {
                        resolveExpr(*ie);
                    }
                    pushScope();
                    s.var_symbol_ids.resize(s.var_names.size());
                    s.var_is_captured.assign(s.var_names.size(), 0);
                    for (size_t i = 0; i < s.var_names.size(); ++i) {
                        s.var_symbol_ids[i] = defineSymbol(s.var_names[i], nullptr, &s.var_is_captured[i]);
                    }
                    resolveBlockStmts(s.body);
                    popScope();
                } else if constexpr (std::is_same_v<T, FuncDefStmt>) {
                    if (s.is_local && !s.path.empty()) {
                        // `local function f()` defines `f` before resolving its body
                        s.root_symbol_id = defineSymbol(s.path[0], &s.root_is_captured, nullptr);
                    } else if (!s.path.empty()) {
                        if (s.path[0] != "_ENV" && !isVisible(s.path[0]) && isVisible("_ENV")) {
                            s.path.insert(s.path.begin(), "_ENV");
                        }
                        s.root_symbol_id = useSymbol(s.path[0]);
                        s.root_is_captured = false;
                    }
                    resolveFuncBody(s.func, !s.method.empty());
                } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                    for (auto& v : s.values) {
                        resolveExpr(*v);
                    }
                }
            },
            stmt.node);
    }

    bool isVisible(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->count(name)) return true;
        }
        return false;
    }

    void resolveExpr(Expr& expr) {
        // A free name is a field of _ENV: of a visible local _ENV, as one;
        // otherwise of the env table, a global.
        if (auto* id = std::get_if<IdentExpr>(&expr.node);
            id && id->name != "_ENV" && !isVisible(id->name) && isVisible("_ENV")) {
            std::string name = std::move(id->name);
            expr.node = FieldExpr{std::make_unique<Expr>(IdentExpr{"_ENV"}, expr.loc), std::move(name)};
        }
        std::visit(
            [&](auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, IdentExpr>) {
                    e.symbol_id = useSymbol(e.name);
                } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                    resolveExpr(*e.operand);
                } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                    resolveExpr(*e.lhs);
                    resolveExpr(*e.rhs);
                } else if constexpr (std::is_same_v<T, TableExpr>) {
                    for (auto& f : e.fields) {
                        if (f.key) resolveExpr(*f.key);
                        if (f.value) resolveExpr(*f.value);
                    }
                } else if constexpr (std::is_same_v<T, IndexExpr>) {
                    resolveExpr(*e.table);
                    resolveExpr(*e.index);
                } else if constexpr (std::is_same_v<T, FieldExpr>) {
                    resolveExpr(*e.table);
                } else if constexpr (std::is_same_v<T, CallExpr>) {
                    resolveExpr(*e.callee);
                    for (auto& a : e.args) resolveExpr(*a);
                } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                    resolveExpr(*e.object);
                    for (auto& a : e.args) resolveExpr(*a);
                } else if constexpr (std::is_same_v<T, FuncExpr>) {
                    resolveFuncBody(e.func, false);
                }
            },
            expr.node);
    }

public:
    bool assigns_free_env_ = false;

private:
    uint32_t next_symbol_id_ = 1;
    std::vector<FuncBody*> func_stack_;
    std::vector<std::unordered_map<std::string, DeclEntry>> scopes_;
};

} // namespace

void resolveChunkSymbols(Chunk& chunk) {
    SymbolResolver resolver;
    resolver.run(chunk);
    if (!resolver.assigns_free_env_) return;
    // The chunk's _ENV is its upvalue, which an assignment changes for every
    // function of the chunk: it is a local at the chunk's top, made from
    // the env table (a free _ENV read), and the chunk resolved again.
    LocalStmt env;
    env.vars.push_back(LocalAttr{"_ENV"});
    env.init.push_back(std::make_unique<Expr>(IdentExpr{"_ENV"}));
    chunk.body.insert(chunk.body.begin(), std::make_unique<Stmt>(std::move(env)));
    SymbolResolver again;
    again.run(chunk);
}

} // namespace brasslua
