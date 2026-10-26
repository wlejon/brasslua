#include "brasslua/runtime.h"

#include "ast_walk.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace brasslua {

namespace {

bool isNumeric(InferredType t) noexcept {
    return t == InferredType::Int64 || t == InferredType::Float64 || t == InferredType::Number;
}

InferredType joinTypes(InferredType a, InferredType b) noexcept {
    if (a == InferredType::Unknown) return b;
    if (b == InferredType::Unknown) return a;
    if (a == b) return a;
    if (isNumeric(a) && isNumeric(b)) return InferredType::Number;
    return InferredType::Any;
}

std::string varKey(uint32_t symbol_id, const std::string& name) {
    if (symbol_id != 0) {
        return "#" + std::to_string(symbol_id);
    }
    return name;
}

struct VarBinding {
    InferredType type = InferredType::Unknown;
    LocalAttr* attr = nullptr;
};

// Two runs over a chunk. The hinted run guesses: a table field or element,
// a global, a library call and an operator in a chunk that never calls
// setmetatable have the types the chunk's own code suggests; what it finds
// is each expression's `hint`, the type the speculative code bets on. The
// sound run then types only what the program cannot make otherwise (any
// value may have a metatable, a global or field may hold anything, a
// variable's type is the join of everything assigned to it anywhere), and
// its types are the ones the code relies on.
class TypeAnalyzer {
public:
    TypeAnalyzer(RuntimeState& rt, bool sound) : rt_(rt), sound_(sound) {}

    void run(Chunk& chunk) {
        // Pre-scan for `setmetatable` and table nil/truthiness checks
        prescanStmts(chunk.body);
        resolveFactoryClosures();
        if (sound_) {
            runSound(chunk);
            return;
        }

        // Pass 1: collect functions, intern strings, infer local/expr types, and gather call-site arg types
        in_pass1_ = true;
        for (auto& stmt : chunk.body) {
            analyzeStmt(*stmt);
        }
        // Propagate parameter types and table element types down call chains while in_pass1_ is active
        for (int iter = 0; iter < 2; ++iter) {
            for (const auto& fkey : function_order_) {
                auto it = functions_.find(fkey);
                if (it != functions_.end()) {
                    analyzeFuncBody(fkey, *it->second, false);
                }
            }
        }

        // Pass 2: finalize function parameter & return types and re-propagate
        in_pass1_ = false;
        field_types_.clear();
        table_elem_types_.clear();
        locals_.clear();
        for (auto& stmt : chunk.body) {
            analyzeStmt(*stmt);
        }
        for (const auto& fkey : function_order_) {
            auto it = functions_.find(fkey);
            if (it != functions_.end()) {
                finalizeFuncBody(fkey, *it->second);
            }
        }
        locals_.clear();
        for (auto& stmt : chunk.body) {
            analyzeStmt(*stmt);
        }
        for (const auto& fkey : function_order_) {
            auto it = functions_.find(fkey);
            if (it != functions_.end()) {
                finalizeFuncBody(fkey, *it->second);
            }
        }
    }

private:
    // Variables only gain types (locals_ is never cleared), so each pass
    // types a use with everything assigned to the variable on any path; the
    // passes run until nothing changes.
    void runSound(Chunk& chunk) {
        for (int pass = 0; pass < kMaxSoundPasses; ++pass) {
            in_pass1_ = pass == 0;
            changed_ = false;
            for (auto& stmt : chunk.body) analyzeStmt(*stmt);
            for (const auto& fkey : function_order_) {
                auto it = functions_.find(fkey);
                if (it != functions_.end()) finalizeFuncBody(fkey, *it->second);
            }
            if (!in_pass1_ && !changed_) return;
        }
        // Not settled: every local is Any, which is settled.
        all_locals_any_ = true;
        for (auto& stmt : chunk.body) analyzeStmt(*stmt);
        for (const auto& fkey : function_order_) {
            auto it = functions_.find(fkey);
            if (it != functions_.end()) finalizeFuncBody(fkey, *it->second);
        }
    }
    static constexpr int kMaxSoundPasses = 64;

    void noteType(InferredType& slot, InferredType t) {
        if (slot != t) {
            slot = t;
            changed_ = true;
        }
    }

    void prescanExpr(const Expr& expr) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, IdentExpr>) {
                    // Named other than as a call's callee: the value may be
                    // called from anywhere.
                    value_refs_.insert(varKey(e.symbol_id, e.name));
                } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                    if (e.op == UnOp::Not) {
                        if (const auto* idx = std::get_if<IndexExpr>(&e.operand->node)) {
                            if (const auto* base = std::get_if<IdentExpr>(&idx->table->node)) {
                                nil_checked_tables_.insert(varKey(base->symbol_id, base->name));
                            }
                        }
                    }
                    prescanExpr(*e.operand);
                } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                    if (e.op == BinOp::Eq || e.op == BinOp::Ne) {
                        bool rhs_nil = std::holds_alternative<NilExpr>(e.rhs->node);
                        bool lhs_nil = std::holds_alternative<NilExpr>(e.lhs->node);
                        if (rhs_nil) {
                            if (const auto* idx = std::get_if<IndexExpr>(&e.lhs->node)) {
                                if (const auto* base = std::get_if<IdentExpr>(&idx->table->node)) {
                                    nil_checked_tables_.insert(varKey(base->symbol_id, base->name));
                                }
                            }
                        }
                        if (lhs_nil) {
                            if (const auto* idx = std::get_if<IndexExpr>(&e.rhs->node)) {
                                if (const auto* base = std::get_if<IdentExpr>(&idx->table->node)) {
                                    nil_checked_tables_.insert(varKey(base->symbol_id, base->name));
                                }
                            }
                        }
                    }
                    prescanExpr(*e.lhs);
                    prescanExpr(*e.rhs);
                } else if constexpr (std::is_same_v<T, TableExpr>) {
                    for (const auto& f : e.fields) {
                        if (f.key) prescanExpr(*f.key);
                        if (f.value) prescanExpr(*f.value);
                    }
                } else if constexpr (std::is_same_v<T, IndexExpr>) {
                    prescanExpr(*e.table);
                    prescanExpr(*e.index);
                } else if constexpr (std::is_same_v<T, FieldExpr>) {
                    prescanExpr(*e.table);
                } else if constexpr (std::is_same_v<T, CallExpr>) {
                    if (const auto* callee = std::get_if<IdentExpr>(&e.callee->node)) {
                        ++total_calls_[varKey(callee->symbol_id, callee->name)];
                    } else {
                        prescanExpr(*e.callee);
                    }
                    for (const auto& a : e.args) prescanExpr(*a);
                } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                    prescanExpr(*e.object);
                    for (const auto& a : e.args) prescanExpr(*a);
                } else if constexpr (std::is_same_v<T, FuncExpr>) {
                    prescanStmts(e.func.body);
                }
            },
            expr.node);
    }

    void prescanStmts(const std::vector<StmtPtr>& stmts) {
        for (const auto& sp : stmts) {
            std::visit(
                [&](const auto& s) {
                    using T = std::decay_t<decltype(s)>;
                    if constexpr (std::is_same_v<T, LocalStmt>) {
                        for (const auto& ie : s.init) prescanExpr(*ie);
                        for (size_t i = 0; i < s.vars.size() && i < s.init.size(); ++i) {
                            std::string vkey = varKey(s.vars[i].symbol_id, s.vars[i].name);
                            if (auto* fexpr = std::get_if<FuncExpr>(&s.init[i]->node)) {
                                ++function_defs_[vkey];
                                if (FuncBody* ret_fn = singleReturnedClosure(fexpr->func)) {
                                    factory_closure_[vkey] = ret_fn;
                                }
                            } else if (const auto* cexpr = std::get_if<CallExpr>(&s.init[i]->node)) {
                                if (const auto* cid = std::get_if<IdentExpr>(&cexpr->callee->node)) {
                                    std::string ckey = varKey(cid->symbol_id, cid->name);
                                    ++local_init_calls_[ckey];
                                    local_init_from_call_[vkey] = ckey;
                                    ++function_defs_[vkey];
                                }
                            }
                        }
                    } else if constexpr (std::is_same_v<T, AssignStmt>) {
                        for (const auto& te : s.targets) {
                            prescanExpr(*te);
                            if (const auto* id = std::get_if<IdentExpr>(&te->node)) {
                                mutated_vars_.insert(varKey(id->symbol_id, id->name));
                            }
                        }
                        for (const auto& ve : s.values) prescanExpr(*ve);
                    } else if constexpr (std::is_same_v<T, ExprStmt>) {
                        prescanExpr(*s.expr);
                    } else if constexpr (std::is_same_v<T, BlockStmt>) {
                        prescanStmts(s.body);
                    } else if constexpr (std::is_same_v<T, IfStmt>) {
                        for (const auto& br : s.branches) {
                            if (const auto* idx = std::get_if<IndexExpr>(&br.condition->node)) {
                                if (const auto* base = std::get_if<IdentExpr>(&idx->table->node)) {
                                    nil_checked_tables_.insert(varKey(base->symbol_id, base->name));
                                }
                            }
                            prescanExpr(*br.condition);
                            prescanStmts(br.body);
                        }
                        prescanStmts(s.else_body);
                    } else if constexpr (std::is_same_v<T, WhileStmt>) {
                        prescanExpr(*s.condition);
                        prescanStmts(s.body);
                    } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                        prescanStmts(s.body);
                        prescanExpr(*s.until_cond);
                    } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                        prescanExpr(*s.start);
                        prescanExpr(*s.stop);
                        if (s.step) prescanExpr(*s.step);
                        prescanStmts(s.body);
                    } else if constexpr (std::is_same_v<T, GenericForStmt>) {
                        for (const auto& ie : s.iter_exprs) prescanExpr(*ie);
                        prescanStmts(s.body);
                    } else if constexpr (std::is_same_v<T, FuncDefStmt>) {
                        if (s.path.size() == 1 && s.method.empty()) {
                            std::string fkey = varKey(s.root_symbol_id, s.path[0]);
                            ++function_defs_[fkey];
                            if (FuncBody* ret_fn = singleReturnedClosure(s.func)) {
                                factory_closure_[fkey] = ret_fn;
                            }
                        } else if (!s.path.empty() && s.method.empty() && s.path.size() == 1) {
                            mutated_vars_.insert(varKey(s.root_symbol_id, s.path[0]));
                        }
                        prescanStmts(s.func.body);
                    } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                        for (const auto& v : s.values) prescanExpr(*v);
                    }
                },
                sp->node);
        }
    }

    void defineVar(uint32_t symbol_id, const std::string& name, InferredType t, LocalAttr* attr = nullptr) {
        if (sound_) {
            if (symbol_id == 0) return;
            VarBinding& vb = locals_[symbol_id];
            if (attr) vb.attr = attr;
            noteType(vb.type, all_locals_any_ ? InferredType::Any : joinTypes(vb.type, t));
            if (vb.attr) vb.attr->inferred_type = vb.type;
            return;
        }
        if (attr) {
            attr->inferred_type = t;
        }
        if (symbol_id != 0) {
            locals_[symbol_id] = VarBinding{t, attr};
        } else {
            globals_[name] = t;
        }
    }

    void updateVar(uint32_t symbol_id, const std::string& name, InferredType t) {
        if (sound_) {
            defineVar(symbol_id, name, t);
            return;
        }
        if (symbol_id != 0) {
            auto it = locals_.find(symbol_id);
            if (it != locals_.end()) {
                it->second.type = joinTypes(it->second.type, t);
                if (it->second.attr) {
                    it->second.attr->inferred_type = it->second.type;
                }
                return;
            }
            locals_[symbol_id] = VarBinding{t, nullptr};
            return;
        }
        auto it = globals_.find(name);
        if (it == globals_.end()) {
            globals_[name] = t;
        } else {
            it->second = joinTypes(it->second, t);
        }
    }

    InferredType lookupVarType(uint32_t symbol_id, const std::string& name) const {
        if (symbol_id != 0) {
            auto it = locals_.find(symbol_id);
            if (it != locals_.end()) {
                return it->second.type;
            }
            return InferredType::Any;
        }
        if (sound_) return InferredType::Any;
        auto git = globals_.find(name);
        if (git != globals_.end()) {
            return git->second;
        }
        return InferredType::Any;
    }

    void recordFieldType(const std::string& field, InferredType t) {
        if (t == InferredType::Unknown) return;
        auto it = field_types_.find(field);
        if (it == field_types_.end()) {
            field_types_[field] = t;
        } else {
            it->second = joinTypes(it->second, t);
        }
    }

    void recordTableElemType(const std::string& tbl_key, InferredType t) {
        if (t == InferredType::Unknown) return;
        auto it = table_elem_types_.find(tbl_key);
        if (it == table_elem_types_.end()) {
            table_elem_types_[tbl_key] = t;
        } else {
            it->second = joinTypes(it->second, t);
        }
    }

    void analyzeBlock(std::vector<StmtPtr>& stmts) {
        for (auto& s : stmts) {
            analyzeStmt(*s);
        }
    }

    void analyzeFuncBody(const std::string& func_key, FuncBody& fn, bool has_implicit_self = false) {
        size_t total_params = fn.params.size() + (has_implicit_self ? 1 : 0);
        if (fn.param_types.size() < total_params) {
            fn.param_types.resize(total_params, InferredType::Unknown);
        }
        if (!func_key.empty() && !callSitesKnown(func_key)) {
            for (auto& pt : fn.param_types) pt = InferredType::Any;
        }
        if (has_implicit_self) {
            // `obj.method(x)` calls a method with any self.
            defineVar(fn.self_symbol_id, "self", sound_ ? InferredType::Any : InferredType::Table);
        }
        for (size_t i = 0; i < fn.params.size(); ++i) {
            size_t type_idx = i + (has_implicit_self ? 1 : 0);
            InferredType pt = fn.param_types[type_idx];
            if (!in_pass1_ && pt == InferredType::Unknown) {
                pt = InferredType::Any;
            }
            uint32_t psym = i < fn.param_symbol_ids.size() ? fn.param_symbol_ids[i] : 0;
            defineVar(psym, fn.params[i], pt);
        }

        std::string prev_func_key = current_func_key_;
        InferredType prev_ret = current_return_type_;
        bool prev_saw_ret = saw_return_;
        current_func_key_ = func_key;
        current_return_type_ = InferredType::Unknown;
        saw_return_ = false;

        for (auto& s : fn.body) {
            analyzeStmt(*s);
        }
        if (sound_) {
            // A parameter's slot holds what the body assigns to it too.
            for (size_t i = 0; i < fn.params.size(); ++i) {
                size_t type_idx = i + (has_implicit_self ? 1 : 0);
                uint32_t psym = i < fn.param_symbol_ids.size() ? fn.param_symbol_ids[i] : 0;
                InferredType pt = lookupVarType(psym, fn.params[i]);
                noteType(fn.param_types[type_idx], joinTypes(fn.param_types[type_idx], pt));
            }
        }

        if (saw_return_) {
            fn.return_type = joinTypes(fn.return_type, current_return_type_);
        } else if (fn.return_type == InferredType::Unknown) {
            fn.return_type = InferredType::Nil;
        }

        current_func_key_ = prev_func_key;
        current_return_type_ = prev_ret;
        saw_return_ = prev_saw_ret;
    }

    void finalizeFuncBody(const std::string& fkey, FuncBody& fn) {
        const InferredType prev_return = fn.return_type;
        fn.return_type = InferredType::Unknown;
        analyzeFuncBody(fkey, fn, false);
        if (fn.return_type != prev_return) changed_ = true;
        bool all_params_num = !fn.params.empty();
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i >= fn.param_types.size() || !isNumeric(fn.param_types[i])) {
                all_params_num = false;
                break;
            }
        }
        fn.can_unbox = !fn.is_vararg && all_params_num && isNumeric(fn.return_type) &&
                       (fkey.empty() || callSitesKnown(fkey));    }

    void analyzeStmt(Stmt& stmt) {
        std::visit(
            [&](auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, LocalStmt>) {
                    for (auto& e : s.init) {
                        analyzeExpr(*e);
                    }
                    for (size_t i = 0; i < s.vars.size(); ++i) {
                        rt_.internString(s.vars[i].name);
                        std::string vkey = varKey(s.vars[i].symbol_id, s.vars[i].name);
                        InferredType t = InferredType::Unknown;
                        if (i < s.init.size()) {
                            t = s.init[i]->inferred_type;
                            if (auto* fe = std::get_if<FuncExpr>(&s.init[i]->node)) {
                                if (functions_.find(vkey) == functions_.end()) {
                                    function_order_.push_back(vkey);
                                }
                                functions_[vkey] = &fe->func;
                            } else if (auto fbit = factory_bound_vars_.find(vkey); fbit != factory_bound_vars_.end()) {
                                FuncBody* ret_fn = fbit->second;
                                std::string akey = anonFuncKey(ret_fn);
                                if (functions_.find(akey) == functions_.end()) {
                                    function_order_.push_back(akey);
                                    functions_[akey] = ret_fn;
                                }
                                functions_[vkey] = ret_fn;
                            } else if (const auto* te = std::get_if<TableExpr>(&s.init[i]->node)) {
                                InferredType seq_t = InferredType::Unknown;
                                for (const auto& f : te->fields) {
                                    if (!f.key && f.value) {
                                        seq_t = joinTypes(seq_t, f.value->inferred_type);
                                    }
                                }
                                if (seq_t != InferredType::Unknown) {
                                    recordTableElemType(vkey, seq_t);
                                }
                            } else if (const auto* src_id = std::get_if<IdentExpr>(&s.init[i]->node)) {
                                auto tit = table_elem_types_.find(varKey(src_id->symbol_id, src_id->name));
                                if (tit != table_elem_types_.end()) {
                                    recordTableElemType(vkey, tit->second);
                                }
                            }
                        } else if (!s.init.empty() &&
                                   (std::holds_alternative<CallExpr>(s.init.back()->node) ||
                                    std::holds_alternative<MethodCallExpr>(s.init.back()->node) ||
                                    std::holds_alternative<VarargExpr>(s.init.back()->node))) {
                            t = InferredType::Any;
                        }
                        defineVar(s.vars[i].symbol_id, s.vars[i].name, t, &s.vars[i]);
                    }
                } else if constexpr (std::is_same_v<T, AssignStmt>) {
                    for (auto& val : s.values) {
                        analyzeExpr(*val);
                    }
                    for (size_t i = 0; i < s.targets.size(); ++i) {
                        analyzeExpr(*s.targets[i]);
                        InferredType rhs_t =
                            i < s.values.size() ? s.values[i]->inferred_type : InferredType::Any;
                        if (auto* id = std::get_if<IdentExpr>(&s.targets[i]->node)) {
                            updateVar(id->symbol_id, id->name, rhs_t);
                            s.targets[i]->inferred_type = lookupVarType(id->symbol_id, id->name);
                        } else if (auto* fld = std::get_if<FieldExpr>(&s.targets[i]->node)) {
                            recordFieldType(fld->field, rhs_t);
                            if (!sound_) {
                                auto fit = field_types_.find(fld->field);
                                if (fit != field_types_.end() && isNumeric(fit->second)) {
                                    s.targets[i]->inferred_type = fit->second;
                                }
                            }
                        } else if (auto* idx = std::get_if<IndexExpr>(&s.targets[i]->node)) {
                            if (const auto* base = std::get_if<IdentExpr>(&idx->table->node)) {
                                std::string bkey = varKey(base->symbol_id, base->name);
                                recordTableElemType(bkey, rhs_t);
                                if (!sound_) {
                                    auto tit = table_elem_types_.find(bkey);
                                    if (tit != table_elem_types_.end() &&
                                        (isNumeric(tit->second) || tit->second == InferredType::Table)) {
                                        s.targets[i]->inferred_type = tit->second;
                                    }
                                }
                            }
                        }
                    }
                } else if constexpr (std::is_same_v<T, ExprStmt>) {
                    analyzeExpr(*s.expr);
                } else if constexpr (std::is_same_v<T, BlockStmt>) {
                    analyzeBlock(s.body);
                } else if constexpr (std::is_same_v<T, IfStmt>) {
                    for (auto& br : s.branches) {
                        analyzeExpr(*br.condition);
                        analyzeBlock(br.body);
                    }
                    if (!s.else_body.empty()) {
                        analyzeBlock(s.else_body);
                    }
                } else if constexpr (std::is_same_v<T, WhileStmt>) {
                    analyzeExpr(*s.condition);
                    analyzeBlock(s.body);
                } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                    for (auto& body_stmt : s.body) {
                        analyzeStmt(*body_stmt);
                    }
                    analyzeExpr(*s.until_cond);
                } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                    rt_.internString(s.var_name);
                    analyzeExpr(*s.start);
                    analyzeExpr(*s.stop);
                    if (s.step) {
                        analyzeExpr(*s.step);
                    }
                    const InferredType start_t = s.start->inferred_type;
                    const InferredType step_t = s.step ? s.step->inferred_type : InferredType::Int64;
                    using Mode = NumericForStmt::Mode;
                    if (start_t == InferredType::Int64 && step_t == InferredType::Int64) {
                        s.mode = Mode::Int;
                    } else if (start_t == InferredType::Float64 || step_t == InferredType::Float64) {
                        s.mode = Mode::Float;
                    } else {
                        s.mode = Mode::Dynamic;
                    }
                    InferredType loop_var_t = InferredType::Any;
                    if (s.mode == Mode::Int) {
                        loop_var_t = InferredType::Int64;
                    } else if (s.mode == Mode::Float) {
                        loop_var_t = InferredType::Float64;
                    } else if (in_pass1_ && (start_t == InferredType::Unknown || step_t == InferredType::Unknown)) {
                        loop_var_t = InferredType::Unknown;
                    }
                    defineVar(s.var_symbol_id, s.var_name, loop_var_t);
                    for (auto& body_stmt : s.body) {
                        analyzeStmt(*body_stmt);
                    }
                    s.var_type = lookupVarType(s.var_symbol_id, s.var_name);
                } else if constexpr (std::is_same_v<T, GenericForStmt>) {
                    for (auto& ie : s.iter_exprs) {
                        analyzeExpr(*ie);
                    }
                    for (size_t i = 0; i < s.var_names.size(); ++i) {
                        rt_.internString(s.var_names[i]);
                        uint32_t vsym = i < s.var_symbol_ids.size() ? s.var_symbol_ids[i] : 0;
                        defineVar(vsym, s.var_names[i], InferredType::Any);
                    }
                    for (auto& body_stmt : s.body) {
                        analyzeStmt(*body_stmt);
                    }
                } else if constexpr (std::is_same_v<T, FuncDefStmt>) {
                    for (const auto& part : s.path) {
                        rt_.internString(part);
                    }
                    if (!s.method.empty()) {
                        rt_.internString(s.method);
                    }
                    std::string fkey;
                    if (s.path.size() == 1 && s.method.empty()) {
                        fkey = varKey(s.root_symbol_id, s.path[0]);
                        if (s.is_local) {
                            defineVar(s.root_symbol_id, s.path[0], InferredType::Function);
                        } else {
                            updateVar(s.root_symbol_id, s.path[0], InferredType::Function);
                        }
                        if (functions_.find(fkey) == functions_.end()) {
                            function_order_.push_back(fkey);
                        }
                        functions_[fkey] = &s.func;
                    }
                    analyzeFuncBody(fkey, s.func, !s.method.empty());
                } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                    saw_return_ = true;
                    for (auto& v : s.values) {
                        analyzeExpr(*v);
                    }
                    InferredType rt =
                        s.values.empty() ? InferredType::Nil : s.values[0]->inferred_type;
                    if (s.values.size() > 1) {
                        rt = InferredType::Any;
                    }
                    current_return_type_ = joinTypes(current_return_type_, rt);
                } else if constexpr (std::is_same_v<T, GotoStmt> || std::is_same_v<T, LabelStmt>) {
                    rt_.internString(s.label);
                }
            },
            stmt.node);
    }

    // A library global may have been replaced, so only a guess.
    InferredType inferStdlibCallType(const Expr& callee) const {
        if (sound_) return InferredType::Unknown;
        if (const auto* id = std::get_if<IdentExpr>(&callee.node)) {
            if (id->symbol_id == 0) {
                if (id->name == "tonumber") return InferredType::Number;
                if (id->name == "tostring" || id->name == "type") return InferredType::String;
            }
        } else if (const auto* fe = std::get_if<FieldExpr>(&callee.node)) {
            if (const auto* base = std::get_if<IdentExpr>(&fe->table->node)) {
                if (base->symbol_id == 0) {
                    if (base->name == "math") {
                        if (fe->field == "type") return InferredType::String;
                        if (fe->field == "ult") return InferredType::Bool;
                        static constexpr const char* kFloatOnly[] = {"sqrt", "sin", "cos", "tan", "asin",
                                                                     "acos", "atan", "exp", "log"};
                        for (const char* f : kFloatOnly) {
                            if (fe->field == f) return InferredType::Float64;
                        }
                        return InferredType::Number;
                    }
                    if (base->name == "bit" || base->name == "bit32") {
                        if (fe->field == "tohex") return InferredType::String;
                        return InferredType::Int64;
                    }
                    if (base->name == "string") {
                        if (fe->field == "len" || fe->field == "byte") return InferredType::Int64;
                        if (fe->field == "sub" || fe->field == "format" || fe->field == "char" ||
                            fe->field == "lower" || fe->field == "upper" || fe->field == "rep" ||
                            fe->field == "reverse") {
                            return InferredType::String;
                        }
                    }
                    if (base->name == "os" && fe->field == "clock") {
                        return InferredType::Float64;
                    }
                }
            }
        }
        return InferredType::Unknown;
    }

    void analyzeExpr(Expr& expr) {
        expr.inferred_type = std::visit(
            [&](auto& e) -> InferredType {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, NilExpr>) {
                    return InferredType::Nil;
                } else if constexpr (std::is_same_v<T, BoolExpr>) {
                    return InferredType::Bool;
                } else if constexpr (std::is_same_v<T, IntExpr>) {
                    return InferredType::Int64;
                } else if constexpr (std::is_same_v<T, FloatExpr>) {
                    return InferredType::Float64;
                } else if constexpr (std::is_same_v<T, StringExpr>) {
                    e.intern_id = rt_.internString(e.value);
                    return InferredType::String;
                } else if constexpr (std::is_same_v<T, VarargExpr>) {
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, IdentExpr>) {
                    e.intern_id = rt_.internString(e.name);
                    return lookupVarType(e.symbol_id, e.name);
                } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                    analyzeExpr(*e.operand);
                    // A metamethod (__len, __bnot) may answer anything.
                    const InferredType ot = e.operand->inferred_type;
                    // The passes start from nothing known: an operand not
                    // yet typed types nothing yet.
                    if (sound_ && e.op != UnOp::Not && ot == InferredType::Unknown) return InferredType::Unknown;
                    switch (e.op) {
                        case UnOp::Not: return InferredType::Bool;
                        case UnOp::Len:
                            return !sound_ || ot == InferredType::String ? InferredType::Int64 : InferredType::Any;
                        case UnOp::Bnot:
                            return !sound_ || isNumeric(ot) ? InferredType::Int64 : InferredType::Any;
                        case UnOp::Neg:
                            if (e.operand->inferred_type == InferredType::Int64) {
                                return InferredType::Int64;
                            }
                            if (e.operand->inferred_type == InferredType::Float64) {
                                return InferredType::Float64;
                            }
                            if (isNumeric(e.operand->inferred_type) || !sound_) {
                                return InferredType::Number;
                            }
                            return InferredType::Any;
                    }
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                    analyzeExpr(*e.lhs);
                    analyzeExpr(*e.rhs);
                    InferredType lt = e.lhs->inferred_type;
                    InferredType rt = e.rhs->inferred_type;
                    const bool to_bool = e.op == BinOp::Eq || e.op == BinOp::Ne || e.op == BinOp::Lt ||
                                         e.op == BinOp::Le || e.op == BinOp::Gt || e.op == BinOp::Ge;
                    if (sound_ && !to_bool && e.op != BinOp::And && e.op != BinOp::Or &&
                        (lt == InferredType::Unknown || rt == InferredType::Unknown)) {
                        return InferredType::Unknown;
                    }
                    switch (e.op) {
                        case BinOp::Add:
                        case BinOp::Sub:
                        case BinOp::Mul:
                        case BinOp::Mod:
                        case BinOp::IDiv:
                            if (lt == InferredType::Int64 && rt == InferredType::Int64) {
                                return InferredType::Int64;
                            }
                            if ((lt == InferredType::Int64 && rt == InferredType::Unknown) ||
                                (lt == InferredType::Unknown && rt == InferredType::Int64)) {
                                return InferredType::Unknown;
                            }
                            // A float operand makes a float; otherwise an integer
                            // may come out, so the result is only a Number.
                            if ((lt == InferredType::Float64 && (isNumeric(rt) || !sound_)) ||
                                (rt == InferredType::Float64 && (isNumeric(lt) || !sound_))) {
                                return InferredType::Float64;
                            }
                            if ((isNumeric(lt) && isNumeric(rt)) || !sound_) {
                                return InferredType::Number;
                            }
                            return InferredType::Any;
                        case BinOp::Div:
                        case BinOp::Pow:
                            if ((isNumeric(lt) && isNumeric(rt)) || !sound_) {
                                return InferredType::Float64;
                            }
                            return InferredType::Any;
                        case BinOp::Band:
                        case BinOp::Bor:
                        case BinOp::Bxor:
                        case BinOp::Shl:
                        case BinOp::Shr:
                            return !sound_ || (isNumeric(lt) && isNumeric(rt)) ? InferredType::Int64
                                                                               : InferredType::Any;
                        case BinOp::Concat: {
                            auto concatenable = [](InferredType t) {
                                return isNumeric(t) || t == InferredType::String;
                            };
                            return !sound_ || (concatenable(lt) && concatenable(rt)) ? InferredType::String
                                                                                     : InferredType::Any;
                        }
                        case BinOp::Eq:
                        case BinOp::Ne:
                        case BinOp::Lt:
                        case BinOp::Le:
                        case BinOp::Gt:
                        case BinOp::Ge:
                            return InferredType::Bool;
                        case BinOp::And:
                        case BinOp::Or:
                            return joinTypes(lt, rt);
                    }
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, TableExpr>) {
                    for (auto& f : e.fields) {
                        if (f.key) {
                            analyzeExpr(*f.key);
                            if (auto* se = std::get_if<StringExpr>(&f.key->node)) {
                                if (f.field_name.empty()) {
                                    f.field_name = se->value;
                                }
                            }
                        }
                        if (!f.field_name.empty()) {
                            f.intern_id = rt_.internString(f.field_name);
                        }
                        if (f.value) {
                            analyzeExpr(*f.value);
                            if (!f.field_name.empty()) {
                                recordFieldType(f.field_name, f.value->inferred_type);
                            }
                        }
                    }
                    return InferredType::Table;
                } else if constexpr (std::is_same_v<T, IndexExpr>) {
                    analyzeExpr(*e.table);
                    analyzeExpr(*e.index);
                    if (!sound_) {
                        if (const auto* base = std::get_if<IdentExpr>(&e.table->node)) {
                            std::string bkey = varKey(base->symbol_id, base->name);
                            if (nil_checked_tables_.count(bkey) == 0) {
                                auto it = table_elem_types_.find(bkey);
                                if (it != table_elem_types_.end() &&
                                    (isNumeric(it->second) || it->second == InferredType::Table)) {
                                    return it->second;
                                }
                            }
                        }
                    }
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, FieldExpr>) {
                    analyzeExpr(*e.table);
                    e.intern_id = rt_.internString(e.field);
                    if (!sound_) {
                        auto it = field_types_.find(e.field);
                        if (it != field_types_.end() && isNumeric(it->second)) {
                            return it->second;
                        }
                    }
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, CallExpr>) {
                    analyzeExpr(*e.callee);
                    for (auto& arg : e.args) {
                        analyzeExpr(*arg);
                    }
                    InferredType std_t = inferStdlibCallType(*e.callee);
                    if (std_t != InferredType::Unknown) {
                        return std_t;
                    }
                    if (const auto* id = std::get_if<IdentExpr>(&e.callee->node)) {
                        std::string ckey = varKey(id->symbol_id, id->name);
                        auto fit = functions_.find(ckey);
                        // A global, or a local assigned again, may hold another function by the call.
                        if (sound_ && (id->symbol_id == 0 || mutated_vars_.count(ckey) || function_defs_[ckey] > 1)) {
                            fit = functions_.end();
                        }
                        if (fit != functions_.end()) {
                            FuncBody* target = fit->second;
                            if (target->param_types.size() < target->params.size()) {
                                target->param_types.resize(target->params.size(),
                                                           InferredType::Unknown);
                            }
                            bool is_self_call = (ckey == current_func_key_);
                            if ((!in_pass1_ || !is_self_call) && callSitesKnown(ckey)) {
                                for (size_t i = 0; i < target->params.size(); ++i) {
                                    InferredType arg_t =
                                        i < e.args.size() ? e.args[i]->inferred_type : InferredType::Nil;
                                    noteType(target->param_types[i], joinTypes(target->param_types[i], arg_t));
                                    if (i < e.args.size()) {
                                        if (const auto* arg_id = std::get_if<IdentExpr>(&e.args[i]->node)) {
                                            auto tit = table_elem_types_.find(varKey(arg_id->symbol_id, arg_id->name));
                                            if (tit != table_elem_types_.end()) {
                                                uint32_t psym = i < target->param_symbol_ids.size()
                                                                    ? target->param_symbol_ids[i]
                                                                    : 0;
                                                recordTableElemType(varKey(psym, target->params[i]), tit->second);
                                            }
                                        }
                                    }
                                }
                            }
                            if (is_self_call && current_return_type_ != InferredType::Unknown) {
                                return current_return_type_;
                            }
                            if (target->return_type != InferredType::Unknown) {
                                return target->return_type;
                            }
                        }
                    }
                    // What the hints cannot see is no evidence against a guess.
                    return sound_ ? InferredType::Any : InferredType::Unknown;
                } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                    analyzeExpr(*e.object);
                    e.intern_id = rt_.internString(e.method);
                    for (auto& arg : e.args) {
                        analyzeExpr(*arg);
                    }
                    return sound_ ? InferredType::Any : InferredType::Unknown;
                } else if constexpr (std::is_same_v<T, FuncExpr>) {
                    analyzeFuncBody(anonFuncKey(&e.func), e.func, false);
                    return InferredType::Function;
                }
            },
            expr.node);
        if (!sound_) expr.hint = expr.inferred_type;
    }

    static std::string anonFuncKey(const FuncBody* fn) {
        return "@anon_" + std::to_string(reinterpret_cast<uintptr_t>(fn));
    }

    void resolveFactoryClosures() {
        for (const auto& [fkey, ret_fn] : factory_closure_) {
            if (!varCallSitesKnown(fkey)) continue;
            int tot = total_calls_[fkey];
            int loc = local_init_calls_[fkey];
            bool all_known = (tot > 0 && tot == loc);
            for (const auto& [vkey, callee_key] : local_init_from_call_) {
                if (callee_key == fkey) {
                    if (varCallSitesKnown(vkey)) {
                        factory_bound_vars_[vkey] = ret_fn;
                    } else {
                        all_known = false;
                    }
                }
            }
            anon_call_sites_known_[anonFuncKey(ret_fn)] = all_known;
        }
    }

    RuntimeState& rt_;
    const bool sound_;
    bool changed_ = false;
    bool all_locals_any_ = false;
    bool in_pass1_ = true;
    std::string current_func_key_;
    std::unordered_map<uint32_t, VarBinding> locals_;
    std::unordered_map<std::string, InferredType> globals_;
    std::vector<std::string> function_order_;
    std::unordered_map<std::string, FuncBody*> functions_;
    std::unordered_set<std::string> mutated_vars_;
    std::unordered_map<std::string, FuncBody*> factory_closure_;
    std::unordered_map<std::string, int> total_calls_;
    std::unordered_map<std::string, int> local_init_calls_;
    std::unordered_map<std::string, std::string> local_init_from_call_;
    std::unordered_map<std::string, FuncBody*> factory_bound_vars_;
    std::unordered_map<std::string, bool> anon_call_sites_known_;
    // A function's parameter types are the join of its direct calls' argument
    // types only when those are all its calls: its variable is never used as a
    // value (`local g = f`, an argument, a table item), never assigned, and
    // names one function. Keys are scoped by lexical symbol_id (`#id` for
    // locals, `name` for globals).
    std::unordered_set<std::string> value_refs_;
    std::unordered_map<std::string, int> function_defs_;
    bool varCallSitesKnown(const std::string& fkey) const {
        // A global function may be called from any chunk.
        if (sound_ && fkey.rfind('#', 0) != 0) return false;
        auto it = function_defs_.find(fkey);
        return value_refs_.count(fkey) == 0 && mutated_vars_.count(fkey) == 0 &&
               (it == function_defs_.end() || it->second <= 1);
    }
    bool callSitesKnown(const std::string& fkey) const {
        auto ait = anon_call_sites_known_.find(fkey);
        if (ait != anon_call_sites_known_.end()) {
            return ait->second;
        }
        if (fkey.rfind("@anon_", 0) == 0) {
            return false;
        }
        auto fbit = factory_bound_vars_.find(fkey);
        if (fbit != factory_bound_vars_.end()) {
            auto ak = anon_call_sites_known_.find(anonFuncKey(fbit->second));
            return ak != anon_call_sites_known_.end() && ak->second && varCallSitesKnown(fkey);
        }
        return varCallSitesKnown(fkey);
    }
    std::unordered_set<std::string> nil_checked_tables_;
    std::unordered_map<std::string, InferredType> field_types_;
    std::unordered_map<std::string, InferredType> table_elem_types_;
    InferredType current_return_type_ = InferredType::Unknown;
    bool saw_return_ = false;
};

} // namespace

void analyzeAndInferTypes(Chunk& chunk, RuntimeState& rt) {
    resolveChunkSymbols(chunk);
    TypeAnalyzer(rt, /*sound=*/false).run(chunk);
    // What the hinted run left in the functions, which the sound run
    // derives again from nothing.
    forEachFuncBody(chunk, [](FuncBody& fn) {
        fn.param_types.clear();
        fn.return_type = InferredType::Unknown;
        fn.can_unbox = false;
    });
    TypeAnalyzer(rt, /*sound=*/true).run(chunk);
}

} // namespace brasslua
