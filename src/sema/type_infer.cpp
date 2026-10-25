#include "brasslua/runtime.h"

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

struct VarBinding {
    InferredType type = InferredType::Unknown;
    LocalAttr* attr = nullptr;
};

class TypeAnalyzer {
public:
    explicit TypeAnalyzer(RuntimeState& rt) : rt_(rt) {
        pushScope();
    }

    void run(Chunk& chunk) {
        static constexpr const char* kCommonFields[] = {
            "x", "y", "z", "vx", "vy", "vz", "mass", "next", "prev", "left", "right", "val", "key"
        };
        for (const char* f : kCommonFields) {
            uint32_t id = rt_.internString(f);
            rt_.getOrAssignFieldSlot(id);
        }

        // Pre-scan for `setmetatable`, stdlib aliases, and table nil/truthiness checks
        prescanStmts(chunk.body);

        // Pass 1: collect functions, intern strings, infer local/expr types, and gather call-site arg types
        in_pass1_ = true;
        for (auto& stmt : chunk.body) {
            analyzeStmt(*stmt);
        }
        // Propagate parameter types and table element types down call chains while in_pass1_ is active
        for (int iter = 0; iter < 2; ++iter) {
            for (const auto& name : function_order_) {
                auto it = functions_.find(name);
                if (it != functions_.end()) {
                    analyzeFuncBody(name, *it->second, false);
                }
            }
        }

        // Pass 2: finalize function parameter & return types and re-propagate
        in_pass1_ = false;
        field_types_.clear();
        table_elem_types_.clear();
        scopes_.clear();
        pushScope();
        for (auto& stmt : chunk.body) {
            analyzeStmt(*stmt);
        }
        for (const auto& name : function_order_) {
            auto it = functions_.find(name);
            if (it != functions_.end()) {
                finalizeFuncBody(name, *it->second);
            }
        }
        scopes_.clear();
        pushScope();
        for (auto& stmt : chunk.body) {
            analyzeStmt(*stmt);
        }
        for (const auto& name : function_order_) {
            auto it = functions_.find(name);
            if (it != functions_.end()) {
                finalizeFuncBody(name, *it->second);
            }
        }
    }

private:
    void prescanExpr(const Expr& expr) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, IdentExpr>) {
                    if (e.name == "setmetatable") {
                        uses_metatables_ = true;
                    }
                    // Named other than as a call's callee: the value may be
                    // called from anywhere.
                    value_refs_.insert(e.name);
                } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                    if (e.op == UnOp::Not) {
                        if (const auto* idx = std::get_if<IndexExpr>(&e.operand->node)) {
                            if (const auto* base = std::get_if<IdentExpr>(&idx->table->node)) {
                                nil_checked_tables_.insert(base->name);
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
                                    nil_checked_tables_.insert(base->name);
                                }
                            }
                        }
                        if (lhs_nil) {
                            if (const auto* idx = std::get_if<IndexExpr>(&e.rhs->node)) {
                                if (const auto* base = std::get_if<IdentExpr>(&idx->table->node)) {
                                    nil_checked_tables_.insert(base->name);
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
                        if (callee->name == "setmetatable") uses_metatables_ = true;
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
                            if (std::holds_alternative<FuncExpr>(s.init[i]->node)) {
                                ++function_defs_[s.vars[i].name];
                            }
                        }
                        for (size_t i = 0; i < s.vars.size() && i < s.init.size(); ++i) {
                            if (const auto* fe = std::get_if<FieldExpr>(&s.init[i]->node)) {
                                if (const auto* base = std::get_if<IdentExpr>(&fe->table->node)) {
                                    if (base->name == "math" || base->name == "bit" || base->name == "bit32") {
                                        stdlib_aliases_[s.vars[i].name] = {base->name, fe->field};
                                    }
                                }
                            }
                        }
                    } else if constexpr (std::is_same_v<T, AssignStmt>) {
                        for (const auto& te : s.targets) {
                            prescanExpr(*te);
                            if (const auto* id = std::get_if<IdentExpr>(&te->node)) {
                                mutated_vars_.insert(id->name);
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
                                    nil_checked_tables_.insert(base->name);
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
                            ++function_defs_[s.path[0]];
                        }
                        prescanStmts(s.func.body);
                    } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                        for (const auto& v : s.values) prescanExpr(*v);
                    }
                },
                sp->node);
        }
    }

    void pushScope() {
        scopes_.emplace_back();
    }

    void popScope() {
        if (scopes_.size() > 1) {
            scopes_.pop_back();
        }
    }

    VarBinding* findVar(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    void defineVar(const std::string& name, InferredType t, LocalAttr* attr = nullptr) {
        if (attr) {
            attr->inferred_type = t;
        }
        scopes_.back()[name] = VarBinding{t, attr};
    }

    void updateVar(const std::string& name, InferredType t) {
        if (auto* b = findVar(name)) {
            b->type = joinTypes(b->type, t);
            if (b->attr) {
                b->attr->inferred_type = b->type;
            }
            return;
        }
        auto it = globals_.find(name);
        if (it == globals_.end()) {
            globals_[name] = t;
        } else {
            it->second = joinTypes(it->second, t);
        }
    }

    InferredType lookupVarType(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) {
                return found->second.type;
            }
        }
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

    void recordTableElemType(const std::string& tbl_var, InferredType t) {
        if (t == InferredType::Unknown) return;
        auto it = table_elem_types_.find(tbl_var);
        if (it == table_elem_types_.end()) {
            table_elem_types_[tbl_var] = t;
        } else {
            it->second = joinTypes(it->second, t);
        }
    }

    void analyzeBlock(std::vector<StmtPtr>& stmts) {
        pushScope();
        for (auto& s : stmts) {
            analyzeStmt(*s);
        }
        popScope();
    }

    void analyzeFuncBody(const std::string& func_name, FuncBody& fn, bool has_implicit_self = false) {
        size_t total_params = fn.params.size() + (has_implicit_self ? 1 : 0);
        if (fn.param_types.size() < total_params) {
            fn.param_types.resize(total_params, InferredType::Unknown);
        }
        if (!func_name.empty() && !callSitesKnown(func_name)) {
            for (auto& pt : fn.param_types) pt = InferredType::Any;
        }
        pushScope();
        if (has_implicit_self) {
            defineVar("self", InferredType::Table);
        }
        for (size_t i = 0; i < fn.params.size(); ++i) {
            size_t type_idx = i + (has_implicit_self ? 1 : 0);
            InferredType pt = fn.param_types[type_idx];
            if (!in_pass1_ && pt == InferredType::Unknown) {
                pt = InferredType::Any;
            }
            defineVar(fn.params[i], pt);
        }

        std::string prev_func_name = current_func_name_;
        InferredType prev_ret = current_return_type_;
        bool prev_saw_ret = saw_return_;
        current_func_name_ = func_name;
        current_return_type_ = InferredType::Unknown;
        saw_return_ = false;

        for (auto& s : fn.body) {
            analyzeStmt(*s);
        }

        if (saw_return_) {
            fn.return_type = joinTypes(fn.return_type, current_return_type_);
        } else if (fn.return_type == InferredType::Unknown) {
            fn.return_type = InferredType::Nil;
        }

        current_func_name_ = prev_func_name;
        current_return_type_ = prev_ret;
        saw_return_ = prev_saw_ret;
        popScope();
    }

    void finalizeFuncBody(const std::string& name, FuncBody& fn) {
        fn.return_type = InferredType::Unknown;
        analyzeFuncBody(name, fn, false);
        bool all_params_num = !fn.params.empty();
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i >= fn.param_types.size() || !isNumeric(fn.param_types[i])) {
                all_params_num = false;
                break;
            }
        }
        fn.can_unbox = !fn.is_vararg && all_params_num && isNumeric(fn.return_type);
    }

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
                        InferredType t = InferredType::Unknown;
                        if (i < s.init.size()) {
                            t = s.init[i]->inferred_type;
                            if (auto* fe = std::get_if<FuncExpr>(&s.init[i]->node)) {
                                if (functions_.find(s.vars[i].name) == functions_.end()) {
                                    function_order_.push_back(s.vars[i].name);
                                }
                                functions_[s.vars[i].name] = &fe->func;
                            } else if (const auto* te = std::get_if<TableExpr>(&s.init[i]->node)) {
                                InferredType seq_t = InferredType::Unknown;
                                for (const auto& f : te->fields) {
                                    if (!f.key && f.value) {
                                        seq_t = joinTypes(seq_t, f.value->inferred_type);
                                    }
                                }
                                if (seq_t != InferredType::Unknown) {
                                    recordTableElemType(s.vars[i].name, seq_t);
                                }
                            } else if (const auto* src_id = std::get_if<IdentExpr>(&s.init[i]->node)) {
                                auto tit = table_elem_types_.find(src_id->name);
                                if (tit != table_elem_types_.end()) {
                                    recordTableElemType(s.vars[i].name, tit->second);
                                }
                            }
                        } else if (!s.init.empty() &&
                                   (std::holds_alternative<CallExpr>(s.init.back()->node) ||
                                    std::holds_alternative<MethodCallExpr>(s.init.back()->node) ||
                                    std::holds_alternative<VarargExpr>(s.init.back()->node))) {
                            t = InferredType::Any;
                        }
                        defineVar(s.vars[i].name, t, &s.vars[i]);
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
                            updateVar(id->name, rhs_t);
                            s.targets[i]->inferred_type = lookupVarType(id->name);
                        } else if (auto* fld = std::get_if<FieldExpr>(&s.targets[i]->node)) {
                            recordFieldType(fld->field, rhs_t);
                            if (!uses_metatables_) {
                                auto fit = field_types_.find(fld->field);
                                if (fit != field_types_.end() && isNumeric(fit->second)) {
                                    s.targets[i]->inferred_type = fit->second;
                                }
                            }
                        } else if (auto* idx = std::get_if<IndexExpr>(&s.targets[i]->node)) {
                            if (const auto* base = std::get_if<IdentExpr>(&idx->table->node)) {
                                recordTableElemType(base->name, rhs_t);
                                if (!uses_metatables_) {
                                    auto tit = table_elem_types_.find(base->name);
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
                    pushScope();
                    for (auto& body_stmt : s.body) {
                        analyzeStmt(*body_stmt);
                    }
                    analyzeExpr(*s.until_cond);
                    popScope();
                } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                    rt_.internString(s.var_name);
                    analyzeExpr(*s.start);
                    analyzeExpr(*s.stop);
                    if (s.step) {
                        analyzeExpr(*s.step);
                    }
                    bool start_int = s.start->inferred_type == InferredType::Int64;
                    bool stop_int = s.stop->inferred_type == InferredType::Int64;
                    bool step_int = !s.step || s.step->inferred_type == InferredType::Int64;
                    s.is_int_loop = start_int && stop_int && step_int;

                    pushScope();
                    InferredType loop_var_t = s.is_int_loop
                        ? InferredType::Int64
                        : ((in_pass1_ && start_int && step_int && s.stop->inferred_type == InferredType::Unknown)
                               ? InferredType::Unknown
                               : InferredType::Number);
                    defineVar(s.var_name, loop_var_t);
                    for (auto& body_stmt : s.body) {
                        analyzeStmt(*body_stmt);
                    }
                    popScope();
                } else if constexpr (std::is_same_v<T, GenericForStmt>) {
                    for (auto& ie : s.iter_exprs) {
                        analyzeExpr(*ie);
                    }
                    pushScope();
                    for (auto& vn : s.var_names) {
                        rt_.internString(vn);
                        defineVar(vn, InferredType::Any);
                    }
                    for (auto& body_stmt : s.body) {
                        analyzeStmt(*body_stmt);
                    }
                    popScope();
                } else if constexpr (std::is_same_v<T, FuncDefStmt>) {
                    for (const auto& part : s.path) {
                        rt_.internString(part);
                    }
                    if (!s.method.empty()) {
                        uint32_t mid = rt_.internString(s.method);
                        rt_.getOrAssignFieldSlot(mid);
                    } else if (s.path.size() > 1) {
                        uint32_t fid = rt_.internString(s.path.back());
                        rt_.getOrAssignFieldSlot(fid);
                    }
                    std::string fname;
                    if (s.path.size() == 1 && s.method.empty()) {
                        fname = s.path[0];
                        if (s.is_local) {
                            defineVar(fname, InferredType::Function);
                        } else {
                            updateVar(fname, InferredType::Function);
                        }
                        if (functions_.find(fname) == functions_.end()) {
                            function_order_.push_back(fname);
                        }
                        functions_[fname] = &s.func;
                    }
                    analyzeFuncBody(fname, s.func, !s.method.empty());
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

    InferredType inferStdlibCallType(const Expr& callee) const {
        if (const auto* id = std::get_if<IdentExpr>(&callee.node)) {
            if (id->name == "tonumber") return InferredType::Number;
            if (id->name == "tostring" || id->name == "type") return InferredType::String;
        } else if (const auto* fe = std::get_if<FieldExpr>(&callee.node)) {
            if (const auto* base = std::get_if<IdentExpr>(&fe->table->node)) {
                if (base->name == "math") {
                    if (fe->field == "type") return InferredType::String;
                    if (fe->field == "ult") return InferredType::Bool;
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
                    return lookupVarType(e.name);
                } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                    analyzeExpr(*e.operand);
                    switch (e.op) {
                        case UnOp::Not: return InferredType::Bool;
                        case UnOp::Len: return InferredType::Int64;
                        case UnOp::Bnot: return InferredType::Int64;
                        case UnOp::Neg:
                            if (e.operand->inferred_type == InferredType::Int64) {
                                return InferredType::Int64;
                            }
                            if (isNumeric(e.operand->inferred_type) || !uses_metatables_) {
                                return InferredType::Float64;
                            }
                            return InferredType::Any;
                    }
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                    analyzeExpr(*e.lhs);
                    analyzeExpr(*e.rhs);
                    InferredType lt = e.lhs->inferred_type;
                    InferredType rt = e.rhs->inferred_type;
                    switch (e.op) {
                        case BinOp::Add:
                        case BinOp::Sub:
                        case BinOp::Mul:
                        case BinOp::Mod:
                            if (lt == InferredType::Int64 && rt == InferredType::Int64) {
                                return InferredType::Int64;
                            }
                            if ((lt == InferredType::Int64 && rt == InferredType::Unknown) ||
                                (lt == InferredType::Unknown && rt == InferredType::Int64)) {
                                return InferredType::Unknown;
                            }
                            if (isNumeric(lt) && isNumeric(rt)) {
                                return InferredType::Float64;
                            }
                            if (!uses_metatables_) {
                                return InferredType::Float64;
                            }
                            return InferredType::Any;
                        case BinOp::Div:
                        case BinOp::Pow:
                            if ((isNumeric(lt) && isNumeric(rt)) || !uses_metatables_) {
                                return InferredType::Float64;
                            }
                            return InferredType::Any;
                        case BinOp::IDiv:
                            if (lt == InferredType::Int64 && rt == InferredType::Int64) {
                                return InferredType::Int64;
                            }
                            if ((isNumeric(lt) && isNumeric(rt)) || !uses_metatables_) {
                                return InferredType::Float64;
                            }
                            return InferredType::Any;
                        case BinOp::Band:
                        case BinOp::Bor:
                        case BinOp::Bxor:
                        case BinOp::Shl:
                        case BinOp::Shr:
                            return InferredType::Int64;
                        case BinOp::Concat:
                            return InferredType::String;
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
                            f.slot_hint = rt_.getOrAssignFieldSlot(f.intern_id);
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
                    if (!uses_metatables_) {
                        if (const auto* base = std::get_if<IdentExpr>(&e.table->node)) {
                            if (nil_checked_tables_.count(base->name) == 0) {
                                auto it = table_elem_types_.find(base->name);
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
                    e.slot_hint = rt_.getOrAssignFieldSlot(e.intern_id);
                    if (!uses_metatables_) {
                        auto it = field_types_.find(e.field);
                        if (it != field_types_.end() && isNumeric(it->second)) {
                            return it->second;
                        }
                    }
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, CallExpr>) {
                    // Rewrite unmutated stdlib alias calls (`local bxor = bit.bxor`, `local sqrt = math.sqrt`)
                    if (const auto* id = std::get_if<IdentExpr>(&e.callee->node)) {
                        if (mutated_vars_.count(id->name) == 0) {
                            auto ait = stdlib_aliases_.find(id->name);
                            if (ait != stdlib_aliases_.end()) {
                                auto base_expr = std::make_unique<Expr>(
                                    IdentExpr{ait->second.first, rt_.internString(ait->second.first)},
                                    e.callee->loc);
                                FieldExpr fe;
                                fe.table = std::move(base_expr);
                                fe.field = ait->second.second;
                                fe.intern_id = rt_.internString(fe.field);
                                fe.slot_hint = rt_.getOrAssignFieldSlot(fe.intern_id);
                                e.callee->node = std::move(fe);
                            }
                        }
                    }
                    analyzeExpr(*e.callee);
                    for (auto& arg : e.args) {
                        analyzeExpr(*arg);
                    }
                    InferredType std_t = inferStdlibCallType(*e.callee);
                    if (std_t != InferredType::Unknown) {
                        return std_t;
                    }
                    if (const auto* id = std::get_if<IdentExpr>(&e.callee->node)) {
                        auto fit = functions_.find(id->name);
                        if (fit != functions_.end()) {
                            FuncBody* target = fit->second;
                            if (target->param_types.size() < target->params.size()) {
                                target->param_types.resize(target->params.size(),
                                                           InferredType::Unknown);
                            }
                            bool is_self_call = (id->name == current_func_name_);
                            if ((!in_pass1_ || !is_self_call) && callSitesKnown(id->name)) {
                                for (size_t i = 0; i < target->params.size(); ++i) {
                                    InferredType arg_t =
                                        i < e.args.size() ? e.args[i]->inferred_type : InferredType::Nil;
                                    target->param_types[i] = joinTypes(target->param_types[i], arg_t);
                                    if (i < e.args.size()) {
                                        if (const auto* arg_id = std::get_if<IdentExpr>(&e.args[i]->node)) {
                                            auto tit = table_elem_types_.find(arg_id->name);
                                            if (tit != table_elem_types_.end()) {
                                                recordTableElemType(target->params[i], tit->second);
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
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                    analyzeExpr(*e.object);
                    e.intern_id = rt_.internString(e.method);
                    e.slot_hint = rt_.getOrAssignFieldSlot(e.intern_id);
                    for (auto& arg : e.args) {
                        analyzeExpr(*arg);
                    }
                    return InferredType::Any;
                } else if constexpr (std::is_same_v<T, FuncExpr>) {
                    analyzeFuncBody("", e.func, false);
                    return InferredType::Function;
                }
            },
            expr.node);
    }

    RuntimeState& rt_;
    bool in_pass1_ = true;
    bool uses_metatables_ = false;
    std::string current_func_name_;
    std::vector<std::unordered_map<std::string, VarBinding>> scopes_;
    std::unordered_map<std::string, InferredType> globals_;
    std::vector<std::string> function_order_;
    std::unordered_map<std::string, FuncBody*> functions_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> stdlib_aliases_;
    std::unordered_set<std::string> mutated_vars_;
    // A function's parameter types are the join of its direct calls' argument
    // types only when those are all its calls: its name is never used as a
    // value (`local g = f`, an argument, a table item), never assigned, and
    // names one function. Names are not scoped here, so any use of the name
    // counts.
    std::unordered_set<std::string> value_refs_;
    std::unordered_map<std::string, int> function_defs_;
    bool callSitesKnown(const std::string& name) const {
        auto it = function_defs_.find(name);
        return value_refs_.count(name) == 0 && mutated_vars_.count(name) == 0 &&
               (it == function_defs_.end() || it->second <= 1);
    }
    std::unordered_set<std::string> nil_checked_tables_;
    std::unordered_map<std::string, InferredType> field_types_;
    std::unordered_map<std::string, InferredType> table_elem_types_;
    InferredType current_return_type_ = InferredType::Unknown;
    bool saw_return_ = false;
};

} // namespace

void analyzeAndInferTypes(Chunk& chunk, RuntimeState& rt) {
    TypeAnalyzer analyzer(rt);
    analyzer.run(chunk);
}

} // namespace brasslua
