#include "codegen/mir_lowering.h"

namespace brasslua {

using brass::BasicBlock;
using brass::Type;

namespace {

bool exprContainsClosure(const Expr& expr) noexcept {
    return std::visit(
        [](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, FuncExpr>) {
                return true;
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                return exprContainsClosure(*e.operand);
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                return exprContainsClosure(*e.lhs) || exprContainsClosure(*e.rhs);
            } else if constexpr (std::is_same_v<T, TableExpr>) {
                for (const auto& f : e.fields) {
                    if (f.key && exprContainsClosure(*f.key)) return true;
                    if (f.value && exprContainsClosure(*f.value)) return true;
                }
                return false;
            } else if constexpr (std::is_same_v<T, IndexExpr>) {
                return exprContainsClosure(*e.table) || exprContainsClosure(*e.index);
            } else if constexpr (std::is_same_v<T, FieldExpr>) {
                return exprContainsClosure(*e.table);
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                if (exprContainsClosure(*e.callee)) return true;
                for (const auto& a : e.args) {
                    if (exprContainsClosure(*a)) return true;
                }
                return false;
            } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                if (exprContainsClosure(*e.object)) return true;
                for (const auto& a : e.args) {
                    if (exprContainsClosure(*a)) return true;
                }
                return false;
            } else {
                return false;
            }
        },
        expr.node);
}

bool canUnrollLoopBody(const std::vector<StmtPtr>& stmts, uint32_t loop_var_sym, bool in_nested_loop = false) noexcept {
    for (const auto& sp : stmts) {
        bool ok = std::visit(
            [&](const auto& s) -> bool {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, LocalStmt>) {
                    for (const auto& v : s.vars) {
                        if (v.is_close || v.is_captured) return false;
                    }
                    for (const auto& ie : s.init) {
                        if (exprContainsClosure(*ie)) return false;
                    }
                    return true;
                } else if constexpr (std::is_same_v<T, AssignStmt>) {
                    for (const auto& te : s.targets) {
                        if (const auto* id = std::get_if<IdentExpr>(&te->node)) {
                            if (id->symbol_id == loop_var_sym) return false;
                        }
                        if (exprContainsClosure(*te)) return false;
                    }
                    for (const auto& ve : s.values) {
                        if (exprContainsClosure(*ve)) return false;
                    }
                    return true;
                } else if constexpr (std::is_same_v<T, ExprStmt>) {
                    return !exprContainsClosure(*s.expr);
                } else if constexpr (std::is_same_v<T, BlockStmt>) {
                    return canUnrollLoopBody(s.body, loop_var_sym, in_nested_loop);
                } else if constexpr (std::is_same_v<T, IfStmt>) {
                    for (const auto& br : s.branches) {
                        if (exprContainsClosure(*br.condition)) return false;
                        if (!canUnrollLoopBody(br.body, loop_var_sym, in_nested_loop)) return false;
                    }
                    return canUnrollLoopBody(s.else_body, loop_var_sym, in_nested_loop);
                } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                    if (s.var_is_captured) return false;
                    if (exprContainsClosure(*s.start) || exprContainsClosure(*s.stop)) return false;
                    if (s.step && exprContainsClosure(*s.step)) return false;
                    return canUnrollLoopBody(s.body, loop_var_sym, true);
                } else if constexpr (std::is_same_v<T, BreakStmt>) {
                    return in_nested_loop;
                } else {
                    return false;
                }
            },
            sp->node);
        if (!ok) return false;
    }
    return true;
}

} // namespace

void MirModuleLowerer::lowerBlockStmts(FunctionBuildState& st, const std::vector<StmtPtr>& stmts) {
    // Pre-register any ::label:: targets in this block so forward `goto` works
    for (const auto& s : stmts) {
        if (const auto* lbl = std::get_if<LabelStmt>(&s->node)) {
            if (st.label_blocks.find(lbl->label) == st.label_blocks.end()) {
                st.label_blocks[lbl->label] = st.newBlock("lbl_" + lbl->label);
            }
        }
    }
    for (const auto& s : stmts) {
        if (st.isTerminated()) {
            break;
        }
        lowerStmt(st, *s);
    }
}

void MirModuleLowerer::lowerStmt(FunctionBuildState& st, const Stmt& stmt) {
    auto& b = st.builder;

    std::visit(
        [&](const auto& s) {
            using T = std::decay_t<decltype(s)>;

            if constexpr (std::is_same_v<T, LocalStmt>) {
                bool expand_last = !s.init.empty() && s.vars.size() > s.init.size() &&
                                   isCallOrVararg(*s.init.back());
                std::vector<MirValue> init_vals;
                init_vals.reserve(s.init.size());
                for (size_t i = 0; i < s.init.size(); ++i) {
                    const bool multi = expand_last && i + 1 == s.init.size();
                    init_vals.push_back(multi ? lowerMultiExpr(st, *s.init[i]) : lowerExpr(st, *s.init[i]));
                }

                for (size_t i = 0; i < s.vars.size(); ++i) {
                    const LocalAttr& vattr = s.vars[i];
                    Rep rep = (vattr.is_close || vattr.is_captured)
                                  ? Rep::Boxed
                                  : repForInferredType(vattr.inferred_type);
                    defineLocal(st, vattr.symbol_id, rep, vattr.is_captured, vattr.is_close);

                    MirValue rhs_mv;
                    if (i < init_vals.size()) {
                        if (expand_last && i + 1 == init_vals.size()) {
                            BValue* m0 = b.build_call("brasslua_rt_get_multiret", Type::i64(), {b.build_iconst_i64(0)});
                            rhs_mv = MirValue{m0, Rep::Boxed};
                        } else {
                            rhs_mv = init_vals[i];
                        }
                    } else if (expand_last) {
                        size_t m_idx = i - (init_vals.size() - 1);
                        BValue* mv = b.build_call("brasslua_rt_get_multiret", Type::i64(),
                                                 {b.build_iconst_i64(static_cast<int64_t>(m_idx))});
                        rhs_mv = MirValue{mv, Rep::Boxed};
                    } else {
                        rhs_mv = MirValue{b.build_iconst_i64(static_cast<int64_t>(kNilBits)), Rep::Boxed};
                    }
                    storeVar(st, vattr.symbol_id, vattr.name, 0, rhs_mv);
                }
            } else if constexpr (std::is_same_v<T, AssignStmt>) {
                bool expand_last = !s.values.empty() && s.targets.size() > s.values.size() &&
                                   isCallOrVararg(*s.values.back());
                std::vector<MirValue> rhs_vals;
                rhs_vals.reserve(s.values.size());
                for (size_t i = 0; i < s.values.size(); ++i) {
                    const bool multi = expand_last && i + 1 == s.values.size();
                    rhs_vals.push_back(multi ? lowerMultiExpr(st, *s.values[i]) : lowerExpr(st, *s.values[i]));
                }

                std::vector<MirValue> final_rhs;
                final_rhs.reserve(s.targets.size());
                for (size_t i = 0; i < s.targets.size(); ++i) {
                    if (i < rhs_vals.size()) {
                        if (expand_last && i + 1 == rhs_vals.size()) {
                            BValue* m0 = b.build_call("brasslua_rt_get_multiret", Type::i64(), {b.build_iconst_i64(0)});
                            final_rhs.push_back(MirValue{m0, Rep::Boxed});
                        } else {
                            final_rhs.push_back(rhs_vals[i]);
                        }
                    } else if (expand_last) {
                        size_t m_idx = i - (rhs_vals.size() - 1);
                        BValue* mv = b.build_call("brasslua_rt_get_multiret", Type::i64(),
                                                 {b.build_iconst_i64(static_cast<int64_t>(m_idx))});
                        final_rhs.push_back(MirValue{mv, Rep::Boxed});
                    } else {
                        final_rhs.push_back(MirValue{b.build_iconst_i64(static_cast<int64_t>(kNilBits)), Rep::Boxed});
                    }
                }

                for (size_t i = 0; i < s.targets.size(); ++i) {
                    const Expr& tgt = *s.targets[i];
                    if (const auto* id = std::get_if<IdentExpr>(&tgt.node)) {
                        storeVar(st, id->symbol_id, id->name, id->intern_id, final_rhs[i]);
                    } else if (const auto* idx = std::get_if<IndexExpr>(&tgt.node)) {
                        lowerIndexStore(st, *idx, final_rhs[i], tgt.inferred_type);
                    } else if (const auto* fld = std::get_if<FieldExpr>(&tgt.node)) {
                        lowerFieldStore(st, *fld, final_rhs[i], tgt.inferred_type);
                    }
                }
            } else if constexpr (std::is_same_v<T, ExprStmt>) {
                (void)lowerExpr(st, *s.expr);
            } else if constexpr (std::is_same_v<T, BlockStmt>) {
                pushScope(st);
                size_t close_mark = st.close_vars.size();
                lowerBlockStmts(st, s.body);
                if (!st.isTerminated()) {
                    while (st.close_vars.size() > close_mark) {
                        BValue* cv = b.build_load(Type::i64(), st.close_vars.back(), 0);
                        b.build_call("brasslua_rt_close_local", Type::void_type(), {cv});
                        st.close_vars.pop_back();
                    }
                } else {
                    st.close_vars.resize(close_mark);
                }
                popScope(st);
            } else if constexpr (std::is_same_v<T, IfStmt>) {
                BasicBlock* merge_bb = st.newBlock("if_end");
                for (size_t i = 0; i < s.branches.size(); ++i) {
                    const auto& br = s.branches[i];
                    MirValue cond_mv = lowerExpr(st, *br.condition);
                    BValue* cond_bit = emitTruthinessBit(st, cond_mv);

                    BasicBlock* then_bb = st.newBlock("if_then");
                    BasicBlock* next_bb = (i + 1 < s.branches.size() || !s.else_body.empty())
                                              ? st.newBlock("if_next")
                                              : merge_bb;
                    b.build_br_if(cond_bit, then_bb, next_bb);

                    b.position_at_end(then_bb);
                    pushScope(st);
                    lowerBlockStmts(st, br.body);
                    popScope(st);
                    if (!st.isTerminated()) {
                        b.build_br(merge_bb);
                    }
                    b.position_at_end(next_bb);
                }
                if (!s.else_body.empty()) {
                    pushScope(st);
                    lowerBlockStmts(st, s.else_body);
                    popScope(st);
                    if (!st.isTerminated()) {
                        b.build_br(merge_bb);
                    }
                }
                b.position_at_end(merge_bb);
            } else if constexpr (std::is_same_v<T, WhileStmt>) {
                BasicBlock* cond_bb = st.newBlock("while_cond");
                BasicBlock* body_bb = st.newBlock("while_body");
                BasicBlock* end_bb = st.newBlock("while_end");

                b.build_br(cond_bb);
                b.position_at_end(cond_bb);
                MirValue cond_mv = lowerExpr(st, *s.condition);
                BValue* cond_bit = emitTruthinessBit(st, cond_mv);
                b.build_br_if(cond_bit, body_bb, end_bb);

                b.position_at_end(body_bb);
                st.loops.push_back(LoopContext{end_bb});
                pushScope(st);
                lowerBlockStmts(st, s.body);
                popScope(st);
                st.loops.pop_back();
                if (!st.isTerminated()) {
                    b.build_br(cond_bb);
                }
                b.position_at_end(end_bb);
            } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                BasicBlock* body_bb = st.newBlock("repeat_body");
                BasicBlock* end_bb = st.newBlock("repeat_end");

                b.build_br(body_bb);
                b.position_at_end(body_bb);
                st.loops.push_back(LoopContext{end_bb});
                pushScope(st);
                lowerBlockStmts(st, s.body);
                if (!st.isTerminated()) {
                    MirValue cond_mv = lowerExpr(st, *s.until_cond);
                    BValue* cond_bit = emitTruthinessBit(st, cond_mv);
                    popScope(st);
                    b.build_br_if(cond_bit, end_bb, body_bb);
                } else {
                    popScope(st);
                }
                st.loops.pop_back();
                b.position_at_end(end_bb);
            } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                MirValue start_mv = lowerExpr(st, *s.start);
                MirValue stop_mv = lowerExpr(st, *s.stop);
                MirValue step_mv = s.step ? lowerExpr(st, *s.step)
                                          : MirValue{b.build_iconst_i64(1), Rep::Int64};

                int const_step_sign = 0; // +1 if known >= 0, -1 if known < 0
                if (!s.step) {
                    const_step_sign = 1;
                } else if (const auto* ie = std::get_if<IntExpr>(&s.step->node)) {
                    const_step_sign = (ie->value >= 0) ? 1 : -1;
                } else if (const auto* fe = std::get_if<FloatExpr>(&s.step->node)) {
                    const_step_sign = (fe->value >= 0.0) ? 1 : -1;
                }

                if (s.is_int_loop) {
                    BValue* start_i = emitUnboxInt64(st, start_mv);
                    BValue* stop_i = emitUnboxInt64(st, stop_mv);
                    BValue* step_i = emitUnboxInt64(st, step_mv);

                    bool start_c = start_i && start_i->is_instruction() &&
                                   start_i->defining_instruction()->opcode() == brass::Opcode::iconst_i64;
                    bool stop_c = stop_i && stop_i->is_instruction() &&
                                  stop_i->defining_instruction()->opcode() == brass::Opcode::iconst_i64;
                    bool step_c = step_i && step_i->is_instruction() &&
                                  step_i->defining_instruction()->opcode() == brass::Opcode::iconst_i64;
                    if (!s.var_is_captured && start_c && stop_c && step_c) {
                        int64_t a = start_i->defining_instruction()->imm_i64();
                        int64_t b_lim = stop_i->defining_instruction()->imm_i64();
                        int64_t stp = step_i->defining_instruction()->imm_i64();
                        if (stp > 0 && a <= b_lim + 16 && (b_lim - a) / stp < 8 &&
                            canUnrollLoopBody(s.body, s.var_symbol_id)) {
                            for (int64_t cur = a; cur <= b_lim; cur += stp) {
                                pushScope(st);
                                LocalVarInfo loop_var = defineLocal(st, s.var_symbol_id, Rep::Int64, false);
                                loop_var.has_const_int = true;
                                loop_var.const_int = cur;
                                st.scopes.back()[s.var_symbol_id] = loop_var;
                                b.build_store(Type::i64(), loop_var.slot_ptr, 0, b.build_iconst_i64(cur));
                                lowerBlockStmts(st, s.body);
                                popScope(st);
                            }
                            return;
                        }
                    }

                    BValue* idx_slot = st.emitEntryAlloca(8, 8);
                    b.build_store(Type::i64(), idx_slot, 0, start_i);

                    BasicBlock* cond_bb = st.newBlock("for_i_cond");
                    BasicBlock* body_bb = st.newBlock("for_i_body");
                    BasicBlock* end_bb = st.newBlock("for_i_end");

                    b.build_br(cond_bb);
                    b.position_at_end(cond_bb);
                    BValue* cur_i = b.build_load(Type::i64(), idx_slot, 0);
                    BValue* keep_going = nullptr;
                    if (const_step_sign > 0) {
                        keep_going = b.build_sle(cur_i, stop_i);
                    } else if (const_step_sign < 0) {
                        keep_going = b.build_sge(cur_i, stop_i);
                    } else {
                        BValue* step_pos = b.build_sge(step_i, b.build_iconst_i64(0));
                        BValue* cmp_pos = b.build_sle(cur_i, stop_i);
                        BValue* cmp_neg = b.build_sge(cur_i, stop_i);
                        keep_going = b.build_select(step_pos, cmp_pos, cmp_neg);
                    }
                    b.build_br_if(keep_going, body_bb, end_bb);

                    b.position_at_end(body_bb);
                    st.loops.push_back(LoopContext{end_bb});
                    pushScope(st);
                    defineLocal(st, s.var_symbol_id, Rep::Int64, s.var_is_captured);
                    storeVar(st, s.var_symbol_id, s.var_name, 0, MirValue{cur_i, Rep::Int64});
                    lowerBlockStmts(st, s.body);
                    popScope(st);
                    st.loops.pop_back();
                    if (!st.isTerminated()) {
                        BValue* next_i = b.build_add(cur_i, step_i);
                        b.build_store(Type::i64(), idx_slot, 0, next_i);
                        b.build_br(cond_bb);
                    }
                    b.position_at_end(end_bb);
                } else {
                    BValue* start_f = emitUnboxFloat64(st, start_mv);
                    BValue* stop_f = emitUnboxFloat64(st, stop_mv);
                    BValue* step_f = emitUnboxFloat64(st, step_mv);
                    BValue* idx_slot = st.emitEntryAlloca(8, 8);
                    b.build_store(Type::f64(), idx_slot, 0, start_f);

                    BasicBlock* cond_bb = st.newBlock("for_f_cond");
                    BasicBlock* body_bb = st.newBlock("for_f_body");
                    BasicBlock* end_bb = st.newBlock("for_f_end");

                    b.build_br(cond_bb);
                    b.position_at_end(cond_bb);
                    BValue* cur_f = b.build_load(Type::f64(), idx_slot, 0);
                    BValue* keep_going = nullptr;
                    if (const_step_sign > 0) {
                        keep_going = b.build_sle(cur_f, stop_f);
                    } else if (const_step_sign < 0) {
                        keep_going = b.build_sge(cur_f, stop_f);
                    } else {
                        BValue* step_pos = b.build_sge(step_f, b.build_fconst_f64(0.0));
                        BValue* cmp_pos = b.build_sle(cur_f, stop_f);
                        BValue* cmp_neg = b.build_sge(cur_f, stop_f);
                        keep_going = b.build_select(step_pos, cmp_pos, cmp_neg);
                    }
                    b.build_br_if(keep_going, body_bb, end_bb);

                    b.position_at_end(body_bb);
                    st.loops.push_back(LoopContext{end_bb});
                    pushScope(st);
                    defineLocal(st, s.var_symbol_id, Rep::Float64, s.var_is_captured);
                    storeVar(st, s.var_symbol_id, s.var_name, 0, MirValue{cur_f, Rep::Float64});
                    lowerBlockStmts(st, s.body);
                    popScope(st);
                    st.loops.pop_back();
                    if (!st.isTerminated()) {
                        BValue* next_f = b.build_add(cur_f, step_f);
                        b.build_store(Type::f64(), idx_slot, 0, next_f);
                        b.build_br(cond_bb);
                    }
                    b.position_at_end(end_bb);
                }
            } else if constexpr (std::is_same_v<T, GenericForStmt>) {
                // Evaluate iterator triplet: iter_fn, state, ctrl_var
                BValue* iter_fn = b.build_iconst_i64(static_cast<int64_t>(kNilBits));
                BValue* state_v = b.build_iconst_i64(static_cast<int64_t>(kNilBits));
                BValue* ctrl_v = b.build_iconst_i64(static_cast<int64_t>(kNilBits));

                if (!s.iter_exprs.empty()) {
                    const bool multi = s.iter_exprs.size() == 1 && isCallOrVararg(*s.iter_exprs[0]);
                    MirValue e0 = multi ? lowerMultiExpr(st, *s.iter_exprs[0]) : lowerExpr(st, *s.iter_exprs[0]);
                    iter_fn = emitBox(st, e0);
                    if (multi) {
                        iter_fn = b.build_call("brasslua_rt_get_multiret", Type::i64(), {b.build_iconst_i64(0)});
                        state_v = b.build_call("brasslua_rt_get_multiret", Type::i64(), {b.build_iconst_i64(1)});
                        ctrl_v = b.build_call("brasslua_rt_get_multiret", Type::i64(), {b.build_iconst_i64(2)});
                    } else {
                        if (s.iter_exprs.size() >= 2) {
                            state_v = emitBox(st, lowerExpr(st, *s.iter_exprs[1]));
                        }
                        if (s.iter_exprs.size() >= 3) {
                            ctrl_v = emitBox(st, lowerExpr(st, *s.iter_exprs[2]));
                        }
                    }
                }

                BValue* ctrl_slot = st.emitEntryAlloca(8, 8);
                b.build_store(Type::i64(), ctrl_slot, 0, ctrl_v);

                BasicBlock* cond_bb = st.newBlock("gfor_cond");
                BasicBlock* body_bb = st.newBlock("gfor_body");
                BasicBlock* end_bb = st.newBlock("gfor_end");

                b.build_br(cond_bb);
                b.position_at_end(cond_bb);
                BValue* cur_ctrl = b.build_load(Type::i64(), ctrl_slot, 0);
                BValue* next_ctrl = b.build_call("brasslua_rt_call_2", Type::i64(), {iter_fn, state_v, cur_ctrl});
                BValue* is_nil = b.build_eq(next_ctrl, b.build_iconst_i64(static_cast<int64_t>(kNilBits)));
                b.build_br_if(is_nil, end_bb, body_bb);

                b.position_at_end(body_bb);
                b.build_store(Type::i64(), ctrl_slot, 0, next_ctrl);
                st.loops.push_back(LoopContext{end_bb});
                pushScope(st);
                for (size_t i = 0; i < s.var_names.size(); ++i) {
                    uint32_t sym_id = i < s.var_symbol_ids.size() ? s.var_symbol_ids[i] : 0;
                    bool cap = i < s.var_is_captured.size() ? s.var_is_captured[i] : false;
                    defineLocal(st, sym_id, Rep::Boxed, cap);
                    BValue* mv = (i == 0)
                                    ? next_ctrl
                                    : b.build_call("brasslua_rt_get_multiret", Type::i64(),
                                                   {b.build_iconst_i64(static_cast<int64_t>(i))});
                    storeVar(st, sym_id, s.var_names[i], 0, MirValue{mv, Rep::Boxed});
                }
                lowerBlockStmts(st, s.body);
                popScope(st);
                st.loops.pop_back();
                if (!st.isTerminated()) {
                    b.build_br(cond_bb);
                }
                b.position_at_end(end_bb);
            } else if constexpr (std::is_same_v<T, FuncDefStmt>) {
                std::string debug_name = (s.path.size() == 1 && s.method.empty()) ? s.path[0] : "";
                if (s.is_local && !debug_name.empty()) {
                    defineLocal(st, s.root_symbol_id, Rep::Boxed, s.root_is_captured);
                }
                uint32_t fn_sym_id = (s.path.size() == 1 && s.method.empty()) ? s.root_symbol_id : 0;
                CompiledFuncSig sig = compileFuncBody(&st, debug_name, fn_sym_id, s.func, !s.method.empty());
                BValue* fn_addr = b.build_func_addr(sig.boxed_symbol);
                BValue* unboxed_addr = sig.has_unboxed ? b.build_func_addr(sig.unboxed_symbol)
                                                      : b.build_iconst_i64(0);
                BValue* uv_cnt = b.build_iconst_i32(static_cast<int32_t>(sig.captured_vars.size()));
                BValue* arity_c = b.build_iconst_i32(static_cast<int32_t>(sig.arity));
                BValue* cl_bits = b.build_call("brasslua_rt_closure_new", Type::i64(),
                                              {fn_addr, unboxed_addr, uv_cnt, arity_c});

                if (!sig.captured_vars.empty()) {
                    BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
                    BValue* cl_ptr = b.build_and(cl_bits, payload_mask);
                    BValue* uvs_ptr = b.build_load(Type::ptr(), cl_ptr, static_cast<int32_t>(offsetof(LuaClosure, upvalues)));
                    for (size_t u = 0; u < sig.captured_vars.size(); ++u) {
                        LocalVarInfo* ploc = lookupLocal(st, sig.captured_vars[u].symbol_id);
                        if (ploc && ploc->is_upvalue) {
                            BValue* uv_ptr = ploc->slot_ptr;
                            if (!uv_ptr && st.closure_arg) {
                                BValue* parent_uvs = b.build_load(Type::ptr(), st.closure_arg,
                                                                  static_cast<int32_t>(offsetof(LuaClosure, upvalues)));
                                uv_ptr = b.build_load(Type::ptr(), parent_uvs,
                                                      static_cast<int32_t>(ploc->upvalue_idx * 8));
                            }
                            if (uv_ptr) {
                                b.build_store(Type::ptr(), uvs_ptr, static_cast<int32_t>(u * 8), uv_ptr);
                            }
                        }
                    }
                }

                if (s.path.size() == 1 && s.method.empty()) {
                    storeVar(st, s.root_symbol_id, s.path[0], 0, MirValue{cl_bits, Rep::Boxed});
                } else if (!s.path.empty()) {
                    MirValue base = loadVar(st, s.root_symbol_id, s.path[0], 0);
                    for (size_t i = 1; i + (s.method.empty() ? 1 : 0) < s.path.size(); ++i) {
                        base = MirValue{emitFieldGet(st, emitBox(st, base), rt_.internString(s.path[i])), Rep::Boxed};
                    }
                    const std::string& final_field = s.method.empty() ? s.path.back() : s.method;
                    emitFieldSet(st, emitBox(st, base), rt_.internString(final_field), cl_bits);
                }
            } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                if (st.is_unboxed_variant) {
                    if (s.values.empty()) {
                        BValue* def_v = st.expected_return_rep == Rep::Float64
                                           ? b.build_fconst_f64(0.0)
                                           : b.build_iconst_i64(0);
                        b.build_ret(def_v);
                    } else {
                        MirValue mv = lowerExpr(st, *s.values[0]);
                        MirValue coerced = coerceToRep(st, mv, st.expected_return_rep);
                        b.build_ret(coerced.val);
                    }
                    return;
                }

                for (auto it = st.close_vars.rbegin(); it != st.close_vars.rend(); ++it) {
                    BValue* cv = b.build_load(Type::i64(), *it, 0);
                    b.build_call("brasslua_rt_close_local", Type::void_type(), {cv});
                }

                if (s.values.empty()) {
                    BValue* zero = b.build_iconst_i64(0);
                    b.build_call("brasslua_rt_set_multiret", Type::void_type(), {zero, zero});
                    b.build_ret(b.build_iconst_i64(static_cast<int64_t>(kNilBits)));
                    return;
                }

                // Tail call or vararg (`return f(...)`, `return obj:m(...)`, `return ...`):
                // forward multi-return buffer untouched
                if (s.values.size() == 1 && isCallOrVararg(*s.values[0])) {
                    MirValue mv = lowerMultiExpr(st, *s.values[0]);
                    BValue* boxed = emitBox(st, mv);
                    b.build_ret(boxed);
                    return;
                }

                if (s.values.size() == 1) {
                    MirValue mv = lowerExpr(st, *s.values[0]);
                    BValue* boxed = emitBox(st, mv);
                    // One value: whatever a call before it left in the multiret
                    // buffer is not this function's result.
                    auto slot = reinterpret_cast<intptr_t>(rt_.multiRetModeSlot());
                    b.build_store(Type::i8(), b.build_iconst_i64(slot), 0, b.build_trunc_i8(b.build_iconst_i32(0)));
                    b.build_ret(boxed);
                    return;
                }

                bool last_is_multiret = isCallOrVararg(*s.values.back());
                if (last_is_multiret) {
                    size_t fixed_count = s.values.size() - 1;
                    BValue* ret_buf = st.emitEntryAlloca(static_cast<uint32_t>(fixed_count * 8), 8);
                    for (size_t i = 0; i < fixed_count; ++i) {
                        MirValue mv = lowerExpr(st, *s.values[i]);
                        BValue* boxed = emitBox(st, mv);
                        b.build_store(Type::i64(), ret_buf, static_cast<int32_t>(i * 8), boxed);
                    }
                    (void)lowerMultiExpr(st, *s.values.back());
                    BValue* r0 = b.build_call("brasslua_rt_prepend_multiret", Type::i64(),
                                              {ret_buf, b.build_iconst_i64(static_cast<int64_t>(fixed_count))});
                    b.build_ret(r0);
                    return;
                }

                BValue* ret_buf = st.emitEntryAlloca(static_cast<uint32_t>(s.values.size() * 8), 8);
                BValue* r0 = nullptr;
                for (size_t i = 0; i < s.values.size(); ++i) {
                    MirValue mv = lowerExpr(st, *s.values[i]);
                    BValue* boxed = emitBox(st, mv);
                    if (i == 0) r0 = boxed;
                    b.build_store(Type::i64(), ret_buf, static_cast<int32_t>(i * 8), boxed);
                }
                b.build_call("brasslua_rt_set_multiret", Type::void_type(),
                             {ret_buf, b.build_iconst_i64(static_cast<int64_t>(s.values.size()))});
                b.build_ret(r0);
            } else if constexpr (std::is_same_v<T, BreakStmt>) {
                if (!st.loops.empty()) {
                    b.build_br(st.loops.back().break_block);
                }
            } else if constexpr (std::is_same_v<T, GotoStmt>) {
                auto it = st.label_blocks.find(s.label);
                if (it == st.label_blocks.end()) {
                    BasicBlock* lbb = st.newBlock("lbl_" + s.label);
                    st.label_blocks[s.label] = lbb;
                    b.build_br(lbb);
                } else {
                    b.build_br(it->second);
                }
            } else if constexpr (std::is_same_v<T, LabelStmt>) {
                BasicBlock* lbb = nullptr;
                auto it = st.label_blocks.find(s.label);
                if (it == st.label_blocks.end()) {
                    lbb = st.newBlock("lbl_" + s.label);
                    st.label_blocks[s.label] = lbb;
                } else {
                    lbb = it->second;
                }
                if (!st.isTerminated()) {
                    b.build_br(lbb);
                }
                b.position_at_end(lbb);
            }
        },
        stmt.node);
}

} // namespace brasslua
