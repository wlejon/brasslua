// The numeric for (Lua 5.4's forprep / forloop). Lua picks the loop's kind
// when it starts: an integer loop when the initial value and the step are
// integers, else a float loop. Type inference decides it statically where it
// can (NumericForStmt::Mode); a Dynamic loop asks brasslua_rt_forprep and
// runs the kind it answers with one body whose variable is boxed.
//
// An integer loop runs while i <= limit (i >= limit for a negative step),
// the shape brass's loop passes and range analysis know. Lua's loop never
// overflows (it counts its iterations); here i + step could, when the limit
// is within a step of the integers' end. A loop with a small constant step
// also asks in its header that i not be before the initial value, which a
// wrapped i is; any other leaves after the iteration whose next step would
// wrap, a check behind a loop-invariant branch that brass unswitches.

#include "codegen/mir_lowering.h"

#include <algorithm>
#include <limits>

namespace brasslua {

using brass::BasicBlock;
using brass::Type;

namespace {

constexpr int64_t kMaxInt = std::numeric_limits<int64_t>::max();
constexpr int64_t kMinInt = std::numeric_limits<int64_t>::min();
// The largest constant step whose loop's header guards against a wrap,
// rather than a check after each iteration.
constexpr int64_t kGuardStepMax = int64_t{1} << 24;

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

bool constInt(BValue* v, int64_t& out) noexcept {
    if (!v || !v->is_instruction() || v->defining_instruction()->opcode() != brass::Opcode::iconst_i64) return false;
    out = v->defining_instruction()->imm_i64();
    return true;
}

bool isUnboxedNumber(MirValue v) noexcept {
    return v.rep == Rep::Int64 || v.rep == Rep::Float64;
}

} // namespace

void MirModuleLowerer::lowerNumericFor(FunctionBuildState& st, const NumericForStmt& s) {
    auto& b = st.builder;
    using Mode = NumericForStmt::Mode;
    const MirValue start_mv = lowerExpr(st, *s.start);
    const MirValue stop_mv = lowerExpr(st, *s.stop);
    const MirValue step_mv = s.step ? lowerExpr(st, *s.step) : MirValue{b.build_iconst_i64(1), Rep::Int64};

    BasicBlock* end_bb = st.newBlock("for_end");
    // The exit goes after the loop's blocks, where the code falls out of
    // it, whatever was made before them.
    auto finish = [&] {
        auto& blocks = st.fn->blocks();
        blocks.erase(std::find(blocks.begin(), blocks.end(), end_bb));
        blocks.push_back(end_bb);
        b.position_at_end(end_bb);
    };
    auto lowerBody = [&](Rep rep, MirValue var) {
        st.loops.push_back(LoopContext{end_bb});
        pushScope(st);
        // A body that assigns the variable something else boxes it.
        const Rep var_rep = repForInferredType(s.var_type) == rep ? rep : Rep::Boxed;
        defineLocal(st, s.var_symbol_id, var_rep, s.var_is_captured);
        storeVar(st, s.var_symbol_id, s.var_name, 0, var);
        lowerBlockStmts(st, s.body);
        popScope(st);
        st.loops.pop_back();
    };
    // forprep at run time: skips to end_bb or leaves its answer in out.
    auto forprep = [&](BValue*& mode, BValue*& out) {
        out = st.emitEntryAlloca(24, 8);
        mode = b.build_call("brasslua_rt_forprep", Type::i64(),
                            {emitBox(st, start_mv), emitBox(st, stop_mv), emitBox(st, step_mv), out});
        BasicBlock* run_bb = st.newBlock("for_prep");
        b.build_br_if(b.build_eq(mode, b.build_iconst_i64(0)), end_bb, run_bb);
        b.position_at_end(run_bb);
    };

    if (s.mode == Mode::Int) {
        BValue* start_i = emitUnboxInt64(st, start_mv);
        BValue* step_i = emitUnboxInt64(st, step_mv);
        BValue* stop_i = nullptr;
        int64_t step_c = 0;
        const bool step_const = constInt(step_i, step_c);
        const bool step_known = step_const && step_c != 0;
        if (stop_mv.rep == Rep::Int64 && !(step_const && step_c == 0)) {
            stop_i = stop_mv.val;
            if (!step_const) {
                // A zero step is Lua's error, which forprep raises.
                BasicBlock* zero_bb = st.newBlock("for_zero_step");
                BasicBlock* ok_bb = st.newBlock("for_step_ok");
                b.build_br_if(b.build_eq(step_i, b.build_iconst_i64(0)), zero_bb, ok_bb);
                b.position_at_end(zero_bb);
                b.build_call("brasslua_rt_forprep", Type::i64(),
                             {emitBox(st, start_mv), emitBox(st, stop_mv), emitBox(st, step_mv),
                              st.emitEntryAlloca(24, 8)});
                b.build_br(end_bb);
                b.position_at_end(ok_bb);
            }
        } else {
            BValue* mode = nullptr;
            BValue* out = nullptr;
            forprep(mode, out);
            start_i = b.build_load(Type::i64(), out, 0);
            stop_i = b.build_load(Type::i64(), out, 8);
            if (!step_known) step_i = b.build_load(Type::i64(), out, 16);
        }

        int64_t start_c = 0, stop_c = 0;
        const bool start_known = constInt(start_i, start_c);
        const bool stop_known = constInt(stop_i, stop_c);
        if (!s.var_is_captured && start_known && stop_known && step_known && step_c > 0 &&
            canUnrollLoopBody(s.body, s.var_symbol_id)) {
            const uint64_t trips = start_c > stop_c
                ? 0
                : (static_cast<uint64_t>(stop_c) - static_cast<uint64_t>(start_c)) / static_cast<uint64_t>(step_c) + 1;
            if (trips <= 8) {
                for (uint64_t k = 0; k < trips; ++k) {
                    const int64_t cur = static_cast<int64_t>(static_cast<uint64_t>(start_c) +
                                                             k * static_cast<uint64_t>(step_c));
                    pushScope(st);
                    LocalVarInfo loop_var = defineLocal(st, s.var_symbol_id, Rep::Int64, false);
                    loop_var.has_const_int = true;
                    loop_var.const_int = cur;
                    st.scopes.back()[s.var_symbol_id] = loop_var;
                    b.build_store(Type::i64(), loop_var.slot_ptr, 0, b.build_iconst_i64(cur));
                    lowerBlockStmts(st, s.body);
                    popScope(st);
                }
                if (!st.isTerminated()) b.build_br(end_bb);
                finish();
                return;
            }
        }

        // Whether i + step can wrap: never, always (a constant limit within a
        // step of the end), or as the run-time limit says.
        enum class Wrap { Never, Always, Maybe } wrap = Wrap::Maybe;
        BValue* wrap_bit = nullptr;
        BValue* last_safe = nullptr; // the largest (smallest) i whose next step does not wrap
        BValue* step_pos = nullptr;
        bool guard_start = false;
        if (step_known) {
            const int64_t safe = step_c > 0 ? kMaxInt - step_c : kMinInt - step_c;
            if (stop_known) {
                wrap = (step_c > 0 ? stop_c > safe : stop_c < safe) ? Wrap::Always : Wrap::Never;
            }
            if (wrap != Wrap::Never) {
                last_safe = b.build_iconst_i64(safe);
                if (wrap == Wrap::Maybe) {
                    wrap_bit = step_c > 0 ? b.build_sgt(stop_i, last_safe) : b.build_slt(stop_i, last_safe);
                }
            }
            if (wrap == Wrap::Maybe && step_c >= -kGuardStepMax && step_c <= kGuardStepMax) {
                // The header also asks that i not be before the initial
                // value: a step that wraps lands within a step of the other
                // end, before it, unless the loop starts there too. Only a
                // loop over nearly all the integers does; a small step's
                // then wraps after some 2^40 iterations, where Lua's would
                // go on.
                guard_start = true;
                wrap = Wrap::Never;
            }
        } else {
            step_pos = b.build_sgt(step_i, b.build_iconst_i64(0));
            last_safe = b.build_select(step_pos, b.build_sub(b.build_iconst_i64(kMaxInt), step_i),
                                       b.build_sub(b.build_iconst_i64(kMinInt), step_i));
            wrap_bit = b.build_select(step_pos, b.build_sgt(stop_i, last_safe), b.build_slt(stop_i, last_safe));
        }

        BValue* idx_slot = st.emitEntryAlloca(8, 8);
        b.build_store(Type::i64(), idx_slot, 0, start_i);
        BasicBlock* cond_bb = st.newBlock("for_i_cond");
        BasicBlock* body_bb = st.newBlock("for_i_body");
        b.build_br(cond_bb);

        b.position_at_end(cond_bb);
        BValue* cur_i = b.build_load(Type::i64(), idx_slot, 0);
        BValue* keep_going = nullptr;
        if (step_known) {
            keep_going = step_c > 0 ? b.build_sle(cur_i, stop_i) : b.build_sge(cur_i, stop_i);
            if (guard_start) {
                keep_going = b.build_and(keep_going, step_c > 0 ? b.build_sge(cur_i, start_i)
                                                                : b.build_sle(cur_i, start_i));
            }
        } else {
            keep_going = b.build_select(step_pos, b.build_sle(cur_i, stop_i), b.build_sge(cur_i, stop_i));
        }
        b.build_br_if(keep_going, body_bb, end_bb);

        b.position_at_end(body_bb);
        lowerBody(Rep::Int64, MirValue{cur_i, Rep::Int64});
        if (!st.isTerminated()) {
            BasicBlock* latch_bb = st.newBlock("for_i_latch");
            if (wrap != Wrap::Never) {
                BValue* past = nullptr;
                if (step_known) {
                    past = step_c > 0 ? b.build_sgt(cur_i, last_safe) : b.build_slt(cur_i, last_safe);
                } else {
                    past = b.build_select(step_pos, b.build_sgt(cur_i, last_safe), b.build_slt(cur_i, last_safe));
                }
                if (wrap == Wrap::Always) {
                    b.build_br_if(past, end_bb, latch_bb);
                } else {
                    // Branching on the loop-invariant bit first lets brass
                    // unswitch the check away from the loop that never wraps.
                    BasicBlock* chk_bb = st.newBlock("for_i_wrap");
                    b.build_br_if(wrap_bit, chk_bb, latch_bb);
                    b.position_at_end(chk_bb);
                    b.build_br_if(past, end_bb, latch_bb);
                }
            } else {
                b.build_br(latch_bb);
            }
            b.position_at_end(latch_bb);
            b.build_store(Type::i64(), idx_slot, 0, b.build_add(cur_i, step_i));
            b.build_br(cond_bb);
        }
        finish();
        return;
    }

    if (s.mode == Mode::Float) {
        BValue* start_f = nullptr;
        BValue* stop_f = nullptr;
        BValue* step_f = nullptr;
        int step_sign = 0;
        if (const auto* fe = s.step ? std::get_if<FloatExpr>(&s.step->node) : nullptr) {
            step_sign = fe->value > 0 ? 1 : (fe->value < 0 ? -1 : 0);
        } else if (const auto* ie = s.step ? std::get_if<IntExpr>(&s.step->node) : nullptr) {
            step_sign = ie->value > 0 ? 1 : (ie->value < 0 ? -1 : 0);
        }
        if (step_sign != 0 && isUnboxedNumber(start_mv) && isUnboxedNumber(stop_mv) && isUnboxedNumber(step_mv)) {
            start_f = emitUnboxFloat64(st, start_mv);
            stop_f = emitUnboxFloat64(st, stop_mv);
            step_f = emitUnboxFloat64(st, step_mv);
        } else {
            BValue* mode = nullptr;
            BValue* out = nullptr;
            forprep(mode, out);
            start_f = b.build_load(Type::f64(), out, 0);
            stop_f = b.build_load(Type::f64(), out, 8);
            step_f = b.build_load(Type::f64(), out, 16);
        }
        BValue* idx_slot = st.emitEntryAlloca(8, 8);
        b.build_store(Type::f64(), idx_slot, 0, start_f);
        BasicBlock* cond_bb = st.newBlock("for_f_cond");
        BasicBlock* body_bb = st.newBlock("for_f_body");
        b.build_br(cond_bb);

        b.position_at_end(cond_bb);
        BValue* cur_f = b.build_load(Type::f64(), idx_slot, 0);
        BValue* keep_going = nullptr;
        if (step_sign > 0) {
            keep_going = b.build_sle(cur_f, stop_f);
        } else if (step_sign < 0) {
            keep_going = b.build_sge(cur_f, stop_f);
        } else {
            BValue* pos = b.build_sgt(step_f, b.build_fconst_f64(0.0));
            keep_going = b.build_select(pos, b.build_sle(cur_f, stop_f), b.build_sge(cur_f, stop_f));
        }
        b.build_br_if(keep_going, body_bb, end_bb);

        b.position_at_end(body_bb);
        lowerBody(Rep::Float64, MirValue{cur_f, Rep::Float64});
        if (!st.isTerminated()) {
            b.build_store(Type::f64(), idx_slot, 0, b.build_add(cur_f, step_f));
            b.build_br(cond_bb);
        }
        finish();
        return;
    }

    // Dynamic: one loop over raw bits, an int64_t or a double as forprep said.
    BValue* mode = nullptr;
    BValue* out = nullptr;
    forprep(mode, out);
    BValue* is_int = b.build_eq(mode, b.build_iconst_i64(1));
    BValue* stop_bits = b.build_load(Type::i64(), out, 8);
    BValue* step_bits = b.build_load(Type::i64(), out, 16);
    BValue* stop_f = b.build_bitcast_f64_i64(stop_bits);
    BValue* step_f = b.build_bitcast_f64_i64(step_bits);
    BValue* zero = b.build_iconst_i64(0);
    BValue* step_pos = b.build_select(is_int, b.build_sgt(step_bits, zero), b.build_sgt(step_f, b.build_fconst_f64(0.0)));
    BValue* last_safe = b.build_select(step_pos, b.build_sub(b.build_iconst_i64(kMaxInt), step_bits),
                                       b.build_sub(b.build_iconst_i64(kMinInt), step_bits));
    BValue* idx_slot = st.emitEntryAlloca(8, 8);
    b.build_store(Type::i64(), idx_slot, 0, b.build_load(Type::i64(), out, 0));

    BasicBlock* cond_bb = st.newBlock("for_d_cond");
    BasicBlock* box_bb = st.newBlock("for_d_box");
    BasicBlock* box_i_bb = st.newBlock("for_d_box_i");
    BasicBlock* box_f_bb = st.newBlock("for_d_box_f");
    BasicBlock* body_bb = st.newBlock("for_d_body", {Type::i64()});
    b.build_br(cond_bb);

    b.position_at_end(cond_bb);
    BValue* cur = b.build_load(Type::i64(), idx_slot, 0);
    BValue* cur_f = b.build_bitcast_f64_i64(cur);
    BValue* int_go = b.build_select(step_pos, b.build_sle(cur, stop_bits), b.build_sge(cur, stop_bits));
    BValue* flt_go = b.build_select(step_pos, b.build_sle(cur_f, stop_f), b.build_sge(cur_f, stop_f));
    b.build_br_if(b.build_select(is_int, int_go, flt_go), box_bb, end_bb);

    b.position_at_end(box_bb);
    b.build_br_if(is_int, box_i_bb, box_f_bb);
    b.position_at_end(box_i_bb);
    b.build_br(body_bb, {emitBox(st, MirValue{cur, Rep::Int64})});
    b.position_at_end(box_f_bb);
    b.build_br(body_bb, {emitBox(st, MirValue{cur_f, Rep::Float64})});

    b.position_at_end(body_bb);
    lowerBody(Rep::Boxed, MirValue{body_bb->param(0), Rep::Boxed});
    if (!st.isTerminated()) {
        BasicBlock* next_i_bb = st.newBlock("for_d_next_i");
        BasicBlock* step_i_bb = st.newBlock("for_d_step_i");
        BasicBlock* next_f_bb = st.newBlock("for_d_next_f");
        b.build_br_if(is_int, next_i_bb, next_f_bb);

        b.position_at_end(next_i_bb);
        BValue* past = b.build_select(step_pos, b.build_sgt(cur, last_safe), b.build_slt(cur, last_safe));
        b.build_br_if(past, end_bb, step_i_bb);
        b.position_at_end(step_i_bb);
        b.build_store(Type::i64(), idx_slot, 0, b.build_add(cur, step_bits));
        b.build_br(cond_bb);

        b.position_at_end(next_f_bb);
        b.build_store(Type::i64(), idx_slot, 0, b.build_bitcast_i64_f64(b.build_add(cur_f, step_f)));
        b.build_br(cond_bb);
    }
    finish();
}

} // namespace brasslua
