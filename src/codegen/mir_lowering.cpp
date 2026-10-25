#include "codegen/mir_lowering.h"

#include <unordered_set>

namespace brasslua {

using brass::BasicBlock;
using brass::Function;
using brass::Type;

namespace {

void collectIdentifiersInStmts(const std::vector<StmtPtr>& stmts, std::unordered_set<std::string>& out);

void collectIdentifiersInExpr(const Expr& expr, std::unordered_set<std::string>& out) {
    std::visit(
        [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, IdentExpr>) {
                out.insert(e.name);
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                collectIdentifiersInExpr(*e.operand, out);
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                collectIdentifiersInExpr(*e.lhs, out);
                collectIdentifiersInExpr(*e.rhs, out);
            } else if constexpr (std::is_same_v<T, TableExpr>) {
                for (const auto& f : e.fields) {
                    if (f.key) collectIdentifiersInExpr(*f.key, out);
                    if (f.value) collectIdentifiersInExpr(*f.value, out);
                }
            } else if constexpr (std::is_same_v<T, IndexExpr>) {
                collectIdentifiersInExpr(*e.table, out);
                collectIdentifiersInExpr(*e.index, out);
            } else if constexpr (std::is_same_v<T, FieldExpr>) {
                collectIdentifiersInExpr(*e.table, out);
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                collectIdentifiersInExpr(*e.callee, out);
                for (const auto& a : e.args) collectIdentifiersInExpr(*a, out);
            } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                collectIdentifiersInExpr(*e.object, out);
                for (const auto& a : e.args) collectIdentifiersInExpr(*a, out);
            } else if constexpr (std::is_same_v<T, FuncExpr>) {
                collectIdentifiersInStmts(e.func.body, out);
            }
        },
        expr.node);
}

void collectIdentifiersInStmt(const Stmt& stmt, std::unordered_set<std::string>& out) {
    std::visit(
        [&](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, LocalStmt>) {
                for (const auto& e : s.init) collectIdentifiersInExpr(*e, out);
            } else if constexpr (std::is_same_v<T, AssignStmt>) {
                for (const auto& t : s.targets) collectIdentifiersInExpr(*t, out);
                for (const auto& v : s.values) collectIdentifiersInExpr(*v, out);
            } else if constexpr (std::is_same_v<T, ExprStmt>) {
                collectIdentifiersInExpr(*s.expr, out);
            } else if constexpr (std::is_same_v<T, BlockStmt>) {
                collectIdentifiersInStmts(s.body, out);
            } else if constexpr (std::is_same_v<T, IfStmt>) {
                for (const auto& br : s.branches) {
                    collectIdentifiersInExpr(*br.condition, out);
                    collectIdentifiersInStmts(br.body, out);
                }
                collectIdentifiersInStmts(s.else_body, out);
            } else if constexpr (std::is_same_v<T, WhileStmt>) {
                collectIdentifiersInExpr(*s.condition, out);
                collectIdentifiersInStmts(s.body, out);
            } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                collectIdentifiersInStmts(s.body, out);
                collectIdentifiersInExpr(*s.until_cond, out);
            } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                collectIdentifiersInExpr(*s.start, out);
                collectIdentifiersInExpr(*s.stop, out);
                if (s.step) collectIdentifiersInExpr(*s.step, out);
                collectIdentifiersInStmts(s.body, out);
            } else if constexpr (std::is_same_v<T, GenericForStmt>) {
                for (const auto& ie : s.iter_exprs) collectIdentifiersInExpr(*ie, out);
                collectIdentifiersInStmts(s.body, out);
            } else if constexpr (std::is_same_v<T, FuncDefStmt>) {
                for (const auto& p : s.path) out.insert(p);
                collectIdentifiersInStmts(s.func.body, out);
            } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                for (const auto& v : s.values) collectIdentifiersInExpr(*v, out);
            }
        },
        stmt.node);
}

void collectIdentifiersInStmts(const std::vector<StmtPtr>& stmts, std::unordered_set<std::string>& out) {
    for (const auto& s : stmts) {
        collectIdentifiersInStmt(*s, out);
    }
}

void collectInnerFunctionRefs(const std::vector<StmtPtr>& stmts, std::unordered_set<std::string>& out);

void collectInnerFunctionRefsInExpr(const Expr& expr, std::unordered_set<std::string>& out) {
    std::visit(
        [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, FuncExpr>) {
                collectIdentifiersInStmts(e.func.body, out);
                collectInnerFunctionRefs(e.func.body, out);
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                collectInnerFunctionRefsInExpr(*e.operand, out);
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                collectInnerFunctionRefsInExpr(*e.lhs, out);
                collectInnerFunctionRefsInExpr(*e.rhs, out);
            } else if constexpr (std::is_same_v<T, TableExpr>) {
                for (const auto& f : e.fields) {
                    if (f.key) collectInnerFunctionRefsInExpr(*f.key, out);
                    if (f.value) collectInnerFunctionRefsInExpr(*f.value, out);
                }
            } else if constexpr (std::is_same_v<T, IndexExpr>) {
                collectInnerFunctionRefsInExpr(*e.table, out);
                collectInnerFunctionRefsInExpr(*e.index, out);
            } else if constexpr (std::is_same_v<T, FieldExpr>) {
                collectInnerFunctionRefsInExpr(*e.table, out);
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                collectInnerFunctionRefsInExpr(*e.callee, out);
                for (const auto& a : e.args) collectInnerFunctionRefsInExpr(*a, out);
            } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                collectInnerFunctionRefsInExpr(*e.object, out);
                for (const auto& a : e.args) collectInnerFunctionRefsInExpr(*a, out);
            }
        },
        expr.node);
}

void collectInnerFunctionRefs(const std::vector<StmtPtr>& stmts, std::unordered_set<std::string>& out) {
    for (const auto& s : stmts) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, FuncDefStmt>) {
                    collectIdentifiersInStmts(node.func.body, out);
                    collectInnerFunctionRefs(node.func.body, out);
                } else if constexpr (std::is_same_v<T, LocalStmt>) {
                    for (const auto& e : node.init) collectInnerFunctionRefsInExpr(*e, out);
                } else if constexpr (std::is_same_v<T, AssignStmt>) {
                    for (const auto& t : node.targets) collectInnerFunctionRefsInExpr(*t, out);
                    for (const auto& v : node.values) collectInnerFunctionRefsInExpr(*v, out);
                } else if constexpr (std::is_same_v<T, ExprStmt>) {
                    collectInnerFunctionRefsInExpr(*node.expr, out);
                } else if constexpr (std::is_same_v<T, BlockStmt>) {
                    collectInnerFunctionRefs(node.body, out);
                } else if constexpr (std::is_same_v<T, IfStmt>) {
                    for (const auto& br : node.branches) {
                        collectInnerFunctionRefsInExpr(*br.condition, out);
                        collectInnerFunctionRefs(br.body, out);
                    }
                    collectInnerFunctionRefs(node.else_body, out);
                } else if constexpr (std::is_same_v<T, WhileStmt>) {
                    collectInnerFunctionRefsInExpr(*node.condition, out);
                    collectInnerFunctionRefs(node.body, out);
                } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                    collectInnerFunctionRefs(node.body, out);
                    collectInnerFunctionRefsInExpr(*node.until_cond, out);
                } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                    collectInnerFunctionRefsInExpr(*node.start, out);
                    collectInnerFunctionRefsInExpr(*node.stop, out);
                    if (node.step) collectInnerFunctionRefsInExpr(*node.step, out);
                    collectInnerFunctionRefs(node.body, out);
                } else if constexpr (std::is_same_v<T, GenericForStmt>) {
                    for (const auto& ie : node.iter_exprs) collectInnerFunctionRefsInExpr(*ie, out);
                    collectInnerFunctionRefs(node.body, out);
                } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                    for (const auto& v : node.values) collectInnerFunctionRefsInExpr(*v, out);
                }
            },
            s->node);
    }
}

} // namespace

Rep MirModuleLowerer::repForInferredType(InferredType t) noexcept {
    switch (t) {
        case InferredType::Int64:
            return Rep::Int64;
        case InferredType::Float64:
        case InferredType::Number:
            return Rep::Float64;
        default:
            return Rep::Boxed;
    }
}

Type MirModuleLowerer::mirTypeForRep(Rep r) noexcept {
    return r == Rep::Float64 ? Type::f64() : Type::i64();
}

void MirModuleLowerer::declareRuntimeExternals() {
    auto decl = [&](const char* name) {
        mod_.add_external_symbol(name);
    };

    decl("brasslua_rt_table_new");
    decl("brasslua_rt_table_get");
    decl("brasslua_rt_table_set");
    decl("brasslua_rt_table_get_int");
    decl("brasslua_rt_table_set_int");
    decl("brasslua_rt_table_get_field");
    decl("brasslua_rt_table_set_field");
    decl("brasslua_rt_table_len");
    decl("brasslua_rt_rawget");
    decl("brasslua_rt_rawset");
    decl("brasslua_rt_next");

    decl("brasslua_rt_get_global");
    decl("brasslua_rt_set_global");

    decl("brasslua_rt_string_from_id");
    decl("brasslua_rt_box_int64");
    decl("brasslua_rt_concat");
    decl("brasslua_rt_obj_len");

    decl("brasslua_rt_closure_new");
    decl("brasslua_rt_upvalue_new");
    decl("brasslua_rt_upvalue_get");
    decl("brasslua_rt_upvalue_set");
    decl("brasslua_rt_close_local");

    decl("brasslua_rt_call");
    decl("brasslua_rt_call_0");
    decl("brasslua_rt_call_1");
    decl("brasslua_rt_call_2");
    decl("brasslua_rt_call_3");
    decl("brasslua_rt_call_4");
    decl("brasslua_rt_call_method");
    decl("brasslua_rt_set_multiret");
    decl("brasslua_rt_get_multiret_count");
    decl("brasslua_rt_get_multiret");

    decl("brasslua_rt_add");
    decl("brasslua_rt_sub");
    decl("brasslua_rt_mul");
    decl("brasslua_rt_div");
    decl("brasslua_rt_idiv");
    decl("brasslua_rt_mod");
    decl("brasslua_rt_pow");
    decl("brasslua_rt_unm");
    decl("brasslua_rt_band");
    decl("brasslua_rt_bor");
    decl("brasslua_rt_bxor");
    decl("brasslua_rt_bnot");
    decl("brasslua_rt_shl");
    decl("brasslua_rt_shr");
    decl("brasslua_rt_eq");
    decl("brasslua_rt_lt");
    decl("brasslua_rt_le");
    decl("brasslua_rt_to_number");
    decl("brasslua_rt_to_int64");

    decl("brasslua_rt_fmod");
    decl("brasslua_rt_pow_f64");
    decl("brasslua_rt_idiv_i64");
    decl("brasslua_rt_mod_i64");
}

BValue* MirModuleLowerer::emitBox(FunctionBuildState& st, MirValue v) {
    auto& b = st.builder;
    if (v.rep == Rep::Boxed) {
        return v.val;
    }
    if (v.rep == Rep::BoolBit) {
        BValue* t_bits = b.build_iconst_i64(static_cast<int64_t>(kTrueBits));
        BValue* f_bits = b.build_iconst_i64(static_cast<int64_t>(kFalseBits));
        return b.build_select(v.val, t_bits, f_bits);
    }
    if (v.rep == Rep::Float64) {
        return b.build_bitcast_i64_f64(v.val);
    }
    // Constant folding for Int64 literals that fit in Tag::Int47
    if (v.val && v.val->is_instruction() &&
        v.val->defining_instruction()->opcode() == brass::Opcode::iconst_i64) {
        int64_t imm = v.val->defining_instruction()->imm_i64();
        if (Value::fitsInlineInt(imm)) {
            uint64_t bits = (static_cast<uint64_t>(Tag::Int47) << kTagShift) |
                            (static_cast<uint64_t>(imm) & kPayloadMask);
            return b.build_iconst_i64(static_cast<int64_t>(bits));
        }
    }
    // Rep::Int64: fast inline Tag::Int47 for [-2^47, 2^47 - 1], heap Tag::BoxedInt64 on overflow
    BValue* c16 = b.build_iconst_i64(16);
    BValue* shl = b.build_shl(v.val, c16);
    BValue* sext = b.build_ashr(shl, c16);
    BValue* fits = b.build_eq(sext, v.val);

    BasicBlock* fast_bb = st.newBlock("box_i64_fast");
    BasicBlock* slow_bb = st.newBlock("box_i64_slow");
    BasicBlock* merge_bb = st.newBlock("box_i64_merge", {Type::i64()});
    b.build_br_if(fits, fast_bb, slow_bb);

    b.position_at_end(fast_bb);
    BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
    BValue* tag_bits = b.build_iconst_i64(static_cast<int64_t>(static_cast<uint64_t>(Tag::Int47) << kTagShift));
    BValue* masked = b.build_and(v.val, payload_mask);
    BValue* inline_boxed = b.build_or(tag_bits, masked);
    b.build_br(merge_bb, {inline_boxed});

    b.position_at_end(slow_bb);
    BValue* heap_boxed = b.build_call("brasslua_rt_box_int64", Type::i64(), {v.val});
    b.build_br(merge_bb, {heap_boxed});

    b.position_at_end(merge_bb);
    return merge_bb->param(0);
}

BValue* MirModuleLowerer::emitUnboxInt64(FunctionBuildState& st, MirValue v) {
    auto& b = st.builder;
    if (v.rep == Rep::Int64) {
        return v.val;
    }
    if (v.rep == Rep::Float64) {
        return b.build_fptosi_i64(v.val);
    }
    // Rep::Boxed -> unboxed int64_t: fast path for Tag::Int47 (0xFFF3)
    BValue* tag = b.build_lshr(v.val, b.build_iconst_i64(48));
    BValue* is_inline = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Int47)));

    BasicBlock* fast_bb = st.newBlock("unbox_i64_fast");
    BasicBlock* slow_bb = st.newBlock("unbox_i64_slow");
    BasicBlock* merge_bb = st.newBlock("unbox_i64_merge", {Type::i64()});
    b.build_br_if(is_inline, fast_bb, slow_bb);

    b.position_at_end(fast_bb);
    BValue* c16 = b.build_iconst_i64(16);
    BValue* fast_val = b.build_ashr(b.build_shl(v.val, c16), c16);
    b.build_br(merge_bb, {fast_val});

    b.position_at_end(slow_bb);
    BValue* slow_val = b.build_call("brasslua_rt_to_int64", Type::i64(), {v.val});
    b.build_br(merge_bb, {slow_val});

    b.position_at_end(merge_bb);
    return merge_bb->param(0);
}

BValue* MirModuleLowerer::emitUnboxFloat64(FunctionBuildState& st, MirValue v) {
    auto& b = st.builder;
    if (v.rep == Rep::Float64) {
        return v.val;
    }
    if (v.rep == Rep::Int64) {
        return b.build_sitofp_f64_i64(v.val);
    }
    // Rep::Boxed -> unboxed double: fast path 1 for IEEE-754 double (< kNumberMaxBits),
    // fast path 2 for Tag::Int47 (0xFFF3), slow path for BoxedInt64/string
    BValue* max_num = b.build_iconst_i64(static_cast<int64_t>(kNumberMaxBits));
    BValue* is_flt = b.build_ult(v.val, max_num);

    BasicBlock* flt_bb = st.newBlock("unbox_f64_flt");
    BasicBlock* check_int_bb = st.newBlock("unbox_f64_chk_int");
    BasicBlock* int_bb = st.newBlock("unbox_f64_int");
    BasicBlock* slow_bb = st.newBlock("unbox_f64_slow");
    BasicBlock* merge_bb = st.newBlock("unbox_f64_merge", {Type::f64()});
    b.build_br_if(is_flt, flt_bb, check_int_bb);

    b.position_at_end(flt_bb);
    BValue* f_fast = b.build_bitcast_f64_i64(v.val);
    b.build_br(merge_bb, {f_fast});

    b.position_at_end(check_int_bb);
    BValue* tag = b.build_lshr(v.val, b.build_iconst_i64(48));
    BValue* is_inline = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Int47)));
    b.build_br_if(is_inline, int_bb, slow_bb);

    b.position_at_end(int_bb);
    BValue* c16 = b.build_iconst_i64(16);
    BValue* sext = b.build_ashr(b.build_shl(v.val, c16), c16);
    BValue* f_int = b.build_sitofp_f64_i64(sext);
    b.build_br(merge_bb, {f_int});

    b.position_at_end(slow_bb);
    BValue* f_slow = b.build_call("brasslua_rt_to_number", Type::f64(), {v.val});
    b.build_br(merge_bb, {f_slow});

    b.position_at_end(merge_bb);
    return merge_bb->param(0);
}

MirValue MirModuleLowerer::coerceToRep(FunctionBuildState& st, MirValue v, Rep target) {
    if (v.rep == target) return v;
    switch (target) {
        case Rep::Boxed:
            return MirValue{emitBox(st, v), Rep::Boxed};
        case Rep::Int64:
            return MirValue{emitUnboxInt64(st, v), Rep::Int64};
        case Rep::Float64:
            return MirValue{emitUnboxFloat64(st, v), Rep::Float64};
        case Rep::BoolBit:
            return MirValue{emitTruthinessBit(st, v), Rep::BoolBit};
    }
    return v;
}

BValue* MirModuleLowerer::emitTruthinessBit(FunctionBuildState& st, MirValue v) {
    auto& b = st.builder;
    if (v.rep == Rep::BoolBit) {
        return v.val;
    }
    // In Lua, ALL numbers (including 0 and 0.0) are truthy!
    if (v.rep == Rep::Int64 || v.rep == Rep::Float64) {
        return b.build_iconst_i32(1);
    }
    BValue* nil_bits = b.build_iconst_i64(static_cast<int64_t>(kNilBits));
    BValue* false_bits = b.build_iconst_i64(static_cast<int64_t>(kFalseBits));
    BValue* not_nil = b.build_ne(v.val, nil_bits);
    BValue* not_false = b.build_ne(v.val, false_bits);
    return b.build_and(not_nil, not_false);
}

void MirModuleLowerer::pushScope(FunctionBuildState& st) {
    st.scopes.emplace_back();
}

void MirModuleLowerer::popScope(FunctionBuildState& st) {
    if (st.scopes.size() > 1) {
        st.scopes.pop_back();
    }
}

LocalVarInfo* MirModuleLowerer::lookupLocal(FunctionBuildState& st, const std::string& name) {
    for (auto it = st.scopes.rbegin(); it != st.scopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

LocalVarInfo MirModuleLowerer::defineLocal(FunctionBuildState& st, const std::string& name, Rep rep,
                                           bool as_upvalue, bool is_close) {
    auto& b = st.builder;
    LocalVarInfo info;
    if (as_upvalue || is_close) {
        info.rep = Rep::Boxed;
        info.is_upvalue = as_upvalue;
        info.is_close = is_close;
        if (as_upvalue) {
            BValue* nil_c = b.build_iconst_i64(static_cast<int64_t>(kNilBits));
            info.slot_ptr = b.build_call("brasslua_rt_upvalue_new", Type::ptr(), {nil_c});
        } else {
            info.slot_ptr = st.emitEntryAlloca(8, 8);
            st.close_vars.push_back(info.slot_ptr);
        }
    } else {
        info.rep = rep;
        info.is_upvalue = false;
        info.slot_ptr = st.emitEntryAlloca(8, 8);
    }
    st.scopes.back()[name] = info;
    return info;
}

MirValue MirModuleLowerer::loadVar(FunctionBuildState& st, const std::string& name, uint32_t intern_id) {
    auto& b = st.builder;
    if (auto* loc = lookupLocal(st, name)) {
        if (loc->has_const_int) {
            return MirValue{b.build_iconst_i64(loc->const_int), Rep::Int64};
        }
        Type mt = mirTypeForRep(loc->rep);
        BValue* v = b.build_load(mt, loc->slot_ptr, 0);
        return MirValue{v, loc->rep};
    }
    if (intern_id == 0) {
        intern_id = rt_.internString(name);
    }
    BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(intern_id));
    BValue* gv = b.build_call("brasslua_rt_get_global", Type::i64(), {id_c});
    return MirValue{gv, Rep::Boxed};
}

void MirModuleLowerer::storeVar(FunctionBuildState& st, const std::string& name, uint32_t intern_id, MirValue val) {
    auto& b = st.builder;
    if (auto* loc = lookupLocal(st, name)) {
        loc->has_const_int = false;
        if (loc->is_upvalue) {
            BValue* boxed = emitBox(st, val);
            if (val.rep != Rep::Boxed) {
                b.build_store(Type::i64(), loc->slot_ptr, 0, boxed);
            } else {
                BValue* idx = b.build_iconst_i32(0);
                b.build_call("brasslua_rt_upvalue_set", Type::void_type(), {loc->slot_ptr, idx, boxed});
            }
            return;
        }
        MirValue coerced = coerceToRep(st, val, loc->rep);
        b.build_store(mirTypeForRep(loc->rep), loc->slot_ptr, 0, coerced.val);
        return;
    }
    if (intern_id == 0) {
        intern_id = rt_.internString(name);
    }
    BValue* boxed = emitBox(st, val);
    BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(intern_id));
    b.build_call("brasslua_rt_set_global", Type::void_type(), {id_c, boxed});
}

CompiledFuncSig MirModuleLowerer::compileFuncBody(FunctionBuildState* parent_st,
                                                  const std::string& debug_name,
                                                  const FuncBody& fbody,
                                                  bool has_implicit_self) {
    uint32_t fn_id = next_fn_id_++;
    CompiledFuncSig sig;
    sig.boxed_symbol = "brasslua_fn_" + std::to_string(fn_id);
    sig.arity = static_cast<uint32_t>(fbody.params.size() + (has_implicit_self ? 1 : 0));
    sig.fbody = &fbody;

    std::unordered_set<std::string> inner_refs;
    collectInnerFunctionRefs(fbody.body, inner_refs);

    if (parent_st) {
        std::unordered_set<std::string> used_ids;
        collectIdentifiersInStmts(fbody.body, used_ids);
        for (const auto& p : fbody.params) used_ids.erase(p);
        if (has_implicit_self) used_ids.erase("self");
        if (!debug_name.empty() && fbody.can_unbox) used_ids.erase(debug_name);

        for (const auto& id : used_ids) {
            auto kfit = known_functions_.find(id);
            if (kfit != known_functions_.end() && kfit->second.has_unboxed) {
                continue;
            }
            if (auto* ploc = lookupLocal(*parent_st, id)) {
                if (!ploc->is_upvalue) {
                    auto& pb = parent_st->builder;
                    BValue* cur_v = pb.build_load(mirTypeForRep(ploc->rep), ploc->slot_ptr, 0);
                    BValue* boxed_v = emitBox(*parent_st, MirValue{cur_v, ploc->rep});
                    BValue* uv_ptr = pb.build_call("brasslua_rt_upvalue_new", Type::ptr(), {boxed_v});
                    ploc->slot_ptr = uv_ptr;
                    ploc->rep = Rep::Boxed;
                    ploc->is_upvalue = true;
                    ploc->upvalue_idx = 0;
                }
                sig.captured_names.push_back(id);
            }
        }
    }

    bool allow_unbox = fbody.can_unbox && !has_implicit_self && sig.captured_names.empty() && inner_refs.empty();
    if (allow_unbox) {
        sig.has_unboxed = true;
        sig.unboxed_symbol = sig.boxed_symbol + "_unboxed";
        sig.return_rep = repForInferredType(fbody.return_type);
        if (sig.return_rep == Rep::Boxed) sig.return_rep = Rep::Float64;
        for (size_t i = 0; i < fbody.params.size(); ++i) {
            Rep pr = i < fbody.param_types.size() ? repForInferredType(fbody.param_types[i]) : Rep::Float64;
            if (pr == Rep::Boxed) pr = Rep::Float64;
            sig.param_reps.push_back(pr);
        }
    }

    if (!debug_name.empty()) {
        known_functions_[debug_name] = sig;
    }

    // 1. Emit unboxed fast entry if eligible
    if (sig.has_unboxed) {
        std::vector<Type> param_types;
        for (Rep pr : sig.param_reps) {
            param_types.push_back(mirTypeForRep(pr));
        }
        Type ret_t = mirTypeForRep(sig.return_rep);
        Function* ufn = mod_.create_function(sig.unboxed_symbol, ret_t, brass::Span<const Type>(param_types));
        FunctionBuildState ust(*ufn);
        ust.is_unboxed_variant = true;
        ust.expected_return_rep = sig.return_rep;
        ust.self_func_name = debug_name;
        pushScope(ust);

        ust.builder.append_block("entry");
        std::vector<BValue*> entry_params;
        for (size_t i = 0; i < fbody.params.size(); ++i) {
            entry_params.push_back(ust.builder.add_param(mirTypeForRep(sig.param_reps[i])));
        }

        for (size_t i = 0; i < fbody.params.size(); ++i) {
            Rep pr = sig.param_reps[i];
            LocalVarInfo linfo = defineLocal(ust, fbody.params[i], pr, false);
            ust.builder.build_store(mirTypeForRep(pr), linfo.slot_ptr, 0, entry_params[i]);
        }

        lowerBlockStmts(ust, fbody.body);
        if (!ust.isTerminated()) {
            BValue* def_ret = sig.return_rep == Rep::Float64
                                 ? ust.builder.build_fconst_f64(0.0)
                                 : ust.builder.build_iconst_i64(0);
            ust.builder.build_ret(def_ret);
        }
    }

    // 2. Emit standard boxed entry: (ptr closure, ptr args, i64 nargs) -> i64
    Function* bfn = mod_.create_function(sig.boxed_symbol, Type::i64(), {Type::ptr(), Type::ptr(), Type::i64()});
    FunctionBuildState bst(*bfn);
    bst.is_unboxed_variant = false;
    bst.expected_return_rep = Rep::Boxed;
    bst.self_func_name = debug_name;
    pushScope(bst);

    bst.builder.append_block("entry");
    bst.closure_arg = bst.builder.add_param(Type::ptr());
    bst.args_ptr_arg = bst.builder.add_param(Type::ptr());
    bst.nargs_arg = bst.builder.add_param(Type::i64());
    bst.args_scratch = bst.builder.build_alloca(128, 8);

    if (!sig.captured_names.empty()) {
        std::unordered_set<std::string> assigned_in_body;
        bool has_external_calls = false;
        auto checkExprCalls = [&](auto& self_e, const Expr& expr) -> void {
            std::visit(
                [&](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, UnaryExpr>) {
                        self_e(self_e, *e.operand);
                    } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                        self_e(self_e, *e.lhs);
                        self_e(self_e, *e.rhs);
                    } else if constexpr (std::is_same_v<T, TableExpr>) {
                        for (const auto& f : e.fields) {
                            if (f.key) self_e(self_e, *f.key);
                            if (f.value) self_e(self_e, *f.value);
                        }
                    } else if constexpr (std::is_same_v<T, IndexExpr>) {
                        self_e(self_e, *e.table);
                        self_e(self_e, *e.index);
                    } else if constexpr (std::is_same_v<T, FieldExpr>) {
                        self_e(self_e, *e.table);
                    } else if constexpr (std::is_same_v<T, CallExpr>) {
                        bool is_intrinsic = false;
                        if (const auto* fe = std::get_if<FieldExpr>(&e.callee->node)) {
                            if (const auto* b = std::get_if<IdentExpr>(&fe->table->node)) {
                                if (b->name == "math" || b->name == "bit" || b->name == "bit32") {
                                    is_intrinsic = true;
                                }
                            }
                        }
                        if (!is_intrinsic) has_external_calls = true;
                        for (const auto& a : e.args) self_e(self_e, *a);
                    } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                        has_external_calls = true;
                    }
                },
                expr.node);
        };
        auto collectAssigned = [&](auto& self, const std::vector<StmtPtr>& stmts) -> void {
            for (const auto& sp : stmts) {
                if (const auto* ls = std::get_if<LocalStmt>(&sp->node)) {
                    for (const auto& ie : ls->init) checkExprCalls(checkExprCalls, *ie);
                } else if (const auto* as = std::get_if<AssignStmt>(&sp->node)) {
                    for (const auto& te : as->targets) {
                        if (const auto* id = std::get_if<IdentExpr>(&te->node)) {
                            assigned_in_body.insert(id->name);
                        }
                        checkExprCalls(checkExprCalls, *te);
                    }
                    for (const auto& ve : as->values) checkExprCalls(checkExprCalls, *ve);
                } else if (const auto* es = std::get_if<ExprStmt>(&sp->node)) {
                    checkExprCalls(checkExprCalls, *es->expr);
                } else if (const auto* bs = std::get_if<BlockStmt>(&sp->node)) {
                    self(self, bs->body);
                } else if (const auto* ifs = std::get_if<IfStmt>(&sp->node)) {
                    for (const auto& br : ifs->branches) {
                        checkExprCalls(checkExprCalls, *br.condition);
                        self(self, br.body);
                    }
                    self(self, ifs->else_body);
                } else if (const auto* ws = std::get_if<WhileStmt>(&sp->node)) {
                    checkExprCalls(checkExprCalls, *ws->condition);
                    self(self, ws->body);
                } else if (const auto* rs = std::get_if<RepeatStmt>(&sp->node)) {
                    self(self, rs->body);
                    checkExprCalls(checkExprCalls, *rs->until_cond);
                } else if (const auto* nfs = std::get_if<NumericForStmt>(&sp->node)) {
                    checkExprCalls(checkExprCalls, *nfs->start);
                    checkExprCalls(checkExprCalls, *nfs->stop);
                    if (nfs->step) checkExprCalls(checkExprCalls, *nfs->step);
                    self(self, nfs->body);
                } else if (const auto* gfs = std::get_if<GenericForStmt>(&sp->node)) {
                    has_external_calls = true;
                    self(self, gfs->body);
                } else if (const auto* rets = std::get_if<ReturnStmt>(&sp->node)) {
                    for (const auto& rv : rets->values) checkExprCalls(checkExprCalls, *rv);
                }
            }
        };
        collectAssigned(collectAssigned, fbody.body);

        BValue* uvs_ptr = bst.builder.build_load(
            Type::ptr(), bst.closure_arg, static_cast<int32_t>(offsetof(LuaClosure, upvalues)));
        for (size_t u = 0; u < sig.captured_names.size(); ++u) {
            const std::string& cap_name = sig.captured_names[u];
            BValue* uv_ptr = bst.builder.build_load(
                Type::ptr(), uvs_ptr, static_cast<int32_t>(u * 8));
            if (inner_refs.empty() && !has_external_calls && assigned_in_body.count(cap_name) == 0) {
                BValue* uv_val = bst.builder.build_load(Type::i64(), uv_ptr, 0);
                LocalVarInfo linfo = defineLocal(bst, cap_name, Rep::Boxed, false);
                bst.builder.build_store(Type::i64(), linfo.slot_ptr, 0, uv_val);
            } else {
                LocalVarInfo uv_info;
                uv_info.slot_ptr = uv_ptr;
                uv_info.rep = Rep::Boxed;
                uv_info.is_upvalue = true;
                uv_info.upvalue_idx = static_cast<uint32_t>(u);
                bst.scopes.back()[cap_name] = uv_info;
            }
        }
    }

    auto loadBoxedArg = [&](size_t arg_index) -> BValue* {
        auto& b = bst.builder;
        BValue* idx_v = b.build_iconst_i64(static_cast<int64_t>(arg_index));
        BValue* has_arg = b.build_slt(idx_v, bst.nargs_arg);
        BasicBlock* load_bb = bst.newBlock("arg_load");
        BasicBlock* nil_bb = bst.newBlock("arg_nil");
        BasicBlock* merge_bb = bst.newBlock("arg_merge", {Type::i64()});
        b.build_br_if(has_arg, load_bb, nil_bb);

        b.position_at_end(load_bb);
        BValue* loaded = b.build_load(Type::i64(), bst.args_ptr_arg, static_cast<int32_t>(arg_index * 8));
        b.build_br(merge_bb, {loaded});

        b.position_at_end(nil_bb);
        BValue* nil_c = b.build_iconst_i64(static_cast<int64_t>(kNilBits));
        b.build_br(merge_bb, {nil_c});

        b.position_at_end(merge_bb);
        return merge_bb->param(0);
    };

    size_t arg_offset = 0;
    if (has_implicit_self) {
        bool cap = inner_refs.count("self") != 0;
        LocalVarInfo sinfo = defineLocal(bst, "self", Rep::Boxed, cap);
        BValue* sv = loadBoxedArg(0);
        storeVar(bst, "self", 0, MirValue{sv, Rep::Boxed});
        (void)sinfo;
        arg_offset = 1;
    }

    for (size_t i = 0; i < fbody.params.size(); ++i) {
        const std::string& pname = fbody.params[i];
        bool cap = inner_refs.count(pname) != 0;
        Rep prep = Rep::Boxed;
        size_t type_idx = i + arg_offset;
        if (!cap && type_idx < fbody.param_types.size()) {
            prep = repForInferredType(fbody.param_types[type_idx]);
        }
        defineLocal(bst, pname, prep, cap);
        BValue* raw_arg = loadBoxedArg(i + arg_offset);
        storeVar(bst, pname, 0, MirValue{raw_arg, Rep::Boxed});
    }

    lowerBlockStmts(bst, fbody.body);
    if (!bst.isTerminated()) {
        for (auto it = bst.close_vars.rbegin(); it != bst.close_vars.rend(); ++it) {
            BValue* cv = bst.builder.build_load(Type::i64(), *it, 0);
            bst.builder.build_call("brasslua_rt_close_local", Type::void_type(), {cv});
        }
        BValue* nil_c = bst.builder.build_iconst_i64(static_cast<int64_t>(kNilBits));
        bst.builder.build_ret(nil_c);
    }

    return sig;
}

std::string MirModuleLowerer::lowerChunk(const Chunk& chunk) {
    declareRuntimeExternals();
    std::string entry_sym = "brasslua_chunk_main";
    Function* bfn = mod_.create_function(entry_sym, Type::i64(), {Type::ptr(), Type::ptr(), Type::i64()});
    FunctionBuildState bst(*bfn);
    bst.is_unboxed_variant = false;
    bst.expected_return_rep = Rep::Boxed;
    pushScope(bst);

    std::unordered_set<std::string> inner_refs;
    collectInnerFunctionRefs(chunk.body, inner_refs);

    bst.builder.append_block("entry");
    bst.closure_arg = bst.builder.add_param(Type::ptr());
    bst.args_ptr_arg = bst.builder.add_param(Type::ptr());
    bst.nargs_arg = bst.builder.add_param(Type::i64());
    bst.args_scratch = bst.builder.build_alloca(128, 8);

    // Pre-mark any top-level locals captured by inner closures
    for (const auto& stmt : chunk.body) {
        if (const auto* ls = std::get_if<LocalStmt>(&stmt->node)) {
            for (const auto& v : ls->vars) {
                if (inner_refs.count(v.name) != 0) {
                    defineLocal(bst, v.name, Rep::Boxed, /*as_upvalue=*/true);
                }
            }
        } else if (const auto* fs = std::get_if<FuncDefStmt>(&stmt->node)) {
            if (fs->is_local && fs->path.size() == 1 && inner_refs.count(fs->path[0]) != 0) {
                defineLocal(bst, fs->path[0], Rep::Boxed, /*as_upvalue=*/true);
            }
        }
    }

    lowerBlockStmts(bst, chunk.body);
    if (!bst.isTerminated()) {
        for (auto it = bst.close_vars.rbegin(); it != bst.close_vars.rend(); ++it) {
            BValue* cv = bst.builder.build_load(Type::i64(), *it, 0);
            bst.builder.build_call("brasslua_rt_close_local", Type::void_type(), {cv});
        }
        BValue* nil_c = bst.builder.build_iconst_i64(static_cast<int64_t>(kNilBits));
        bst.builder.build_ret(nil_c);
    }
    return entry_sym;
}

} // namespace brasslua
