#include "codegen/mir_lowering.h"

namespace brasslua {

using brass::BasicBlock;
using brass::Type;

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
                        lowerIndexStore(st, *idx, final_rhs[i]);
                    } else if (const auto* fld = std::get_if<FieldExpr>(&tgt.node)) {
                        lowerFieldStore(st, *fld, final_rhs[i]);
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
                lowerNumericFor(st, s);
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
