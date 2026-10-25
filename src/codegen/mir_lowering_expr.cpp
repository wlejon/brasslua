#include "codegen/mir_lowering.h"

namespace brasslua {

using brass::BasicBlock;
using brass::Type;

bool isCallOrVararg(const Expr& e) noexcept {
    if (e.parenthesized) return false;
    if (std::holds_alternative<VarargExpr>(e.node) || std::holds_alternative<MethodCallExpr>(e.node)) {
        return true;
    }
    if (const auto* ce = std::get_if<CallExpr>(&e.node)) {
        if (const auto* fe = std::get_if<FieldExpr>(&ce->callee->node)) {
            if (const auto* base = std::get_if<IdentExpr>(&fe->table->node)) {
                if (base->symbol_id == 0 &&
                    (base->name == "math" || base->name == "bit" || base->name == "bit32")) {
                    if (fe->field != "frexp" && fe->field != "modf") {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    return false;
}

namespace {

void populateClosureUpvalues(MirModuleLowerer& lowerer, FunctionBuildState& st,
                             const CompiledFuncSig& sig, BValue* cl_bits) {
    if (sig.captured_vars.empty()) return;
    auto& b = st.builder;
    BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
    BValue* cl_ptr = b.build_and(cl_bits, payload_mask);
    BValue* uvs_ptr = b.build_load(Type::ptr(), cl_ptr, static_cast<int32_t>(offsetof(LuaClosure, upvalues)));
    for (size_t u = 0; u < sig.captured_vars.size(); ++u) {
        LocalVarInfo* ploc = lowerer.lookupLocal(st, sig.captured_vars[u].symbol_id);
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

// The boxed-operand form of `+ - * /` and the order comparisons: two inline
// integers (integer ops other than `/`), or two numbers of which one is a
// float, are done here; anything else (a big or boxed integer result, strings,
// metamethods) calls `rt_fn`, the runtime's whole operator. Arithmetic gives a
// boxed result, a comparison a BoolBit.
MirValue emitDynamicNumeric(MirModuleLowerer& lowerer, FunctionBuildState& st, BinOp op,
                            BValue* la, BValue* rb, const char* rt_fn) {
    auto& b = st.builder;
    const bool is_cmp = op == BinOp::Lt || op == BinOp::Le || op == BinOp::Gt || op == BinOp::Ge;
    Type res_ty = is_cmp ? Type::i32() : Type::i64();
    BValue* c48 = b.build_iconst_i64(48);
    BValue* c16 = b.build_iconst_i64(16);
    BValue* int_tag = b.build_iconst_i64(static_cast<int64_t>(Tag::Int47));
    BValue* l_int = b.build_eq(b.build_lshr(la, c48), int_tag);
    BValue* r_int = b.build_eq(b.build_lshr(rb, c48), int_tag);

    BasicBlock* int_bb = st.newBlock("dyn_int");
    BasicBlock* chk_num_bb = st.newBlock("dyn_chk_num");
    BasicBlock* flt_bb = st.newBlock("dyn_flt");
    BasicBlock* slow_bb = st.newBlock("dyn_slow");
    BasicBlock* merge_bb = st.newBlock("dyn_merge", {res_ty});
    b.build_br_if(b.build_and(l_int, r_int), op == BinOp::Div ? chk_num_bb : int_bb, chk_num_bb);

    // Two inline integers.
    b.position_at_end(int_bb);
    BValue* li = b.build_ashr(b.build_shl(la, c16), c16);
    BValue* ri = b.build_ashr(b.build_shl(rb, c16), c16);
    if (is_cmp) {
        BValue* r = nullptr;
        switch (op) {
            case BinOp::Lt: r = b.build_slt(li, ri); break;
            case BinOp::Le: r = b.build_sle(li, ri); break;
            case BinOp::Gt: r = b.build_sgt(li, ri); break;
            default:        r = b.build_sge(li, ri); break;
        }
        b.build_br(merge_bb, {r});
    } else {
        // Lua integers wrap at 64 bits, so the wrapped product is the answer;
        // one that does not fit inline is boxed by the runtime's operator.
        BValue* r = op == BinOp::Add ? b.build_add(li, ri)
                  : op == BinOp::Sub ? b.build_sub(li, ri)
                                     : b.build_mul(li, ri);
        BValue* fits = b.build_eq(b.build_ashr(b.build_shl(r, c16), c16), r);
        BasicBlock* box_bb = st.newBlock("dyn_int_box");
        b.build_br_if(fits, box_bb, slow_bb);
        b.position_at_end(box_bb);
        BValue* payload = b.build_and(r, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
        BValue* tag_bits = b.build_iconst_i64(static_cast<int64_t>(static_cast<uint64_t>(Tag::Int47) << kTagShift));
        b.build_br(merge_bb, {b.build_or(payload, tag_bits)});
    }

    // Two numbers, at least one a float (or `/`).
    b.position_at_end(chk_num_bb);
    BValue* max_num = b.build_iconst_i64(static_cast<int64_t>(kNumberMaxBits));
    BValue* l_num = b.build_or(b.build_ule(la, max_num), l_int);
    BValue* r_num = b.build_or(b.build_ule(rb, max_num), r_int);
    b.build_br_if(b.build_and(l_num, r_num), flt_bb, slow_bb);

    b.position_at_end(flt_bb);
    BValue* lf = b.build_select(l_int, b.build_sitofp_f64_i64(b.build_ashr(b.build_shl(la, c16), c16)),
                                b.build_bitcast_f64_i64(la));
    BValue* rf = b.build_select(r_int, b.build_sitofp_f64_i64(b.build_ashr(b.build_shl(rb, c16), c16)),
                                b.build_bitcast_f64_i64(rb));
    if (is_cmp) {
        BValue* r = nullptr;
        switch (op) {
            case BinOp::Lt: r = b.build_slt(lf, rf); break;
            case BinOp::Le: r = b.build_sle(lf, rf); break;
            case BinOp::Gt: r = b.build_sgt(lf, rf); break;
            default:        r = b.build_sge(lf, rf); break;
        }
        b.build_br(merge_bb, {r});
    } else {
        BValue* r = op == BinOp::Add ? b.build_add(lf, rf)
                  : op == BinOp::Sub ? b.build_sub(lf, rf)
                  : op == BinOp::Mul ? b.build_mul(lf, rf)
                                     : b.build_sdiv(lf, rf);
        b.build_br(merge_bb, {lowerer.emitBox(st, MirValue{r, Rep::Float64})});
    }

    b.position_at_end(slow_bb);
    BValue* slow = nullptr;
    if (is_cmp) {
        // a > b is b < a, a >= b is b <= a.
        const bool swap = op == BinOp::Gt || op == BinOp::Ge;
        BValue* r = b.build_call(rt_fn, Type::i32(), {swap ? rb : la, swap ? la : rb});
        slow = b.build_ne(r, b.build_iconst_i32(0));
    } else {
        slow = b.build_call(rt_fn, Type::i64(), {la, rb});
    }
    b.build_br(merge_bb, {slow});

    b.position_at_end(merge_bb);
    return MirValue{merge_bb->param(0), is_cmp ? Rep::BoolBit : Rep::Boxed};
}

// A call wanting one result: a Lua closure is called through its entry here
// (its return sets the multi-return state, which nothing reads after a
// one-result call); anything else is the runtime's (`__call`, errors).
BValue* emitClosureCall(FunctionBuildState& st, BValue* callee_bits, BValue* arg_buf, BValue* nargs_v) {
    auto& b = st.builder;
    BValue* tag = b.build_lshr(callee_bits, b.build_iconst_i64(48));
    BValue* is_fn = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Function)));
    BasicBlock* fn_bb = st.newBlock("ccall_fn");
    BasicBlock* slow_bb = st.newBlock("ccall_slow");
    BasicBlock* merge_bb = st.newBlock("ccall_merge", {Type::i64()});
    b.build_br_if(is_fn, fn_bb, slow_bb);

    b.position_at_end(fn_bb);
    BValue* cl = b.build_and(callee_bits, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    BValue* entry = b.build_load(Type::ptr(), cl, static_cast<int32_t>(offsetof(LuaClosure, fn)));
    BValue* fast = b.build_call_indirect(entry, Type::i64(), {cl, arg_buf, nargs_v});
    b.build_br(merge_bb, {fast});

    b.position_at_end(slow_bb);
    BValue* slow = b.build_call("brasslua_rt_call", Type::i64(), {callee_bits, arg_buf, nargs_v});
    b.build_br(merge_bb, {slow});

    b.position_at_end(merge_bb);
    return merge_bb->param(0);
}

} // namespace

MirValue MirModuleLowerer::lowerBinaryExpr(FunctionBuildState& st, const BinaryExpr& bin, InferredType result_type) {
    auto& b = st.builder;

    if (bin.op == BinOp::And || bin.op == BinOp::Or) {
        MirValue lhs_mv = lowerExpr(st, *bin.lhs);
        BValue* lhs_boxed = emitBox(st, lhs_mv);
        BValue* truthy = emitTruthinessBit(st, MirValue{lhs_boxed, Rep::Boxed});

        BasicBlock* rhs_bb = st.newBlock("logic_rhs");
        BasicBlock* merge_bb = st.newBlock("logic_merge", {Type::i64()});
        if (bin.op == BinOp::And) {
            b.build_br_if(truthy, rhs_bb, {}, merge_bb, {lhs_boxed});
        } else {
            b.build_br_if(truthy, merge_bb, {lhs_boxed}, rhs_bb, {});
        }

        b.position_at_end(rhs_bb);
        MirValue rhs_mv = lowerExpr(st, *bin.rhs);
        BValue* rhs_boxed = emitBox(st, rhs_mv);
        b.build_br(merge_bb, {rhs_boxed});

        b.position_at_end(merge_bb);
        return MirValue{merge_bb->param(0), Rep::Boxed};
    }

    MirValue lhs = lowerExpr(st, *bin.lhs);
    MirValue rhs = lowerExpr(st, *bin.rhs);

    if (bin.op == BinOp::Concat) {
        BValue* la = emitBox(st, lhs);
        BValue* rb = emitBox(st, rhs);
        return MirValue{b.build_call("brasslua_rt_concat", Type::i64(), {la, rb}), Rep::Boxed};
    }

    // Bitwise operators: inline i64 when both operands are unboxed numeric or inferred Int64
    if (bin.op == BinOp::Band || bin.op == BinOp::Bor || bin.op == BinOp::Bxor ||
        bin.op == BinOp::Shl || bin.op == BinOp::Shr) {
        bool lhs_num = lhs.rep != Rep::Boxed || bin.lhs->inferred_type == InferredType::Int64;
        bool rhs_num = rhs.rep != Rep::Boxed || bin.rhs->inferred_type == InferredType::Int64;
        if (lhs_num && rhs_num) {
            BValue* li = emitUnboxInt64(st, lhs);
            BValue* ri = emitUnboxInt64(st, rhs);
            switch (bin.op) {
                case BinOp::Band: return MirValue{b.build_and(li, ri), Rep::Int64};
                case BinOp::Bor:  return MirValue{b.build_or(li, ri), Rep::Int64};
                case BinOp::Bxor: return MirValue{b.build_xor(li, ri), Rep::Int64};
                case BinOp::Shl: {
                    BValue* in_range = b.build_ult(ri, b.build_iconst_i64(64));
                    BValue* sh = b.build_shl(li, b.build_and(ri, b.build_iconst_i64(63)));
                    BValue* res = b.build_select(in_range, sh, b.build_iconst_i64(0));
                    return MirValue{res, Rep::Int64};
                }
                case BinOp::Shr: {
                    BValue* in_range = b.build_ult(ri, b.build_iconst_i64(64));
                    BValue* sh = b.build_lshr(li, b.build_and(ri, b.build_iconst_i64(63)));
                    BValue* res = b.build_select(in_range, sh, b.build_iconst_i64(0));
                    return MirValue{res, Rep::Int64};
                }
                default: break;
            }
        }
        BValue* la = emitBox(st, lhs);
        BValue* rb = emitBox(st, rhs);
        const char* helper = "brasslua_rt_band";
        if (bin.op == BinOp::Bor) helper = "brasslua_rt_bor";
        else if (bin.op == BinOp::Bxor) helper = "brasslua_rt_bxor";
        else if (bin.op == BinOp::Shl) helper = "brasslua_rt_shl";
        else if (bin.op == BinOp::Shr) helper = "brasslua_rt_shr";
        return MirValue{b.build_call(helper, Type::i64(), {la, rb}), Rep::Boxed};
    }

    // Comparisons
    bool both_int64 = (lhs.rep == Rep::Int64 || bin.lhs->inferred_type == InferredType::Int64) &&
                      (rhs.rep == Rep::Int64 || bin.rhs->inferred_type == InferredType::Int64);
    bool lhs_known_num = (lhs.rep != Rep::Boxed) ||
                         (bin.lhs->inferred_type == InferredType::Int64 ||
                          bin.lhs->inferred_type == InferredType::Float64 ||
                          bin.lhs->inferred_type == InferredType::Number);
    bool rhs_known_num = (rhs.rep != Rep::Boxed) ||
                         (bin.rhs->inferred_type == InferredType::Int64 ||
                          bin.rhs->inferred_type == InferredType::Float64 ||
                          bin.rhs->inferred_type == InferredType::Number);

    if (bin.op == BinOp::Eq || bin.op == BinOp::Ne ||
        bin.op == BinOp::Lt || bin.op == BinOp::Le ||
        bin.op == BinOp::Gt || bin.op == BinOp::Ge) {
        BValue* bit_res = nullptr;
        if (both_int64) {
            BValue* li = emitUnboxInt64(st, lhs);
            BValue* ri = emitUnboxInt64(st, rhs);
            switch (bin.op) {
                case BinOp::Eq: bit_res = b.build_eq(li, ri); break;
                case BinOp::Ne: bit_res = b.build_ne(li, ri); break;
                case BinOp::Lt: bit_res = b.build_slt(li, ri); break;
                case BinOp::Le: bit_res = b.build_sle(li, ri); break;
                case BinOp::Gt: bit_res = b.build_sgt(li, ri); break;
                case BinOp::Ge: bit_res = b.build_sge(li, ri); break;
                default: break;
            }
        } else if (lhs_known_num && rhs_known_num) {
            BValue* lf = emitUnboxFloat64(st, lhs);
            BValue* rf = emitUnboxFloat64(st, rhs);
            switch (bin.op) {
                case BinOp::Eq: bit_res = b.build_eq(lf, rf); break;
                case BinOp::Ne: bit_res = b.build_ne(lf, rf); break;
                case BinOp::Lt: bit_res = b.build_slt(lf, rf); break;
                case BinOp::Le: bit_res = b.build_sle(lf, rf); break;
                case BinOp::Gt: bit_res = b.build_sgt(lf, rf); break;
                case BinOp::Ge: bit_res = b.build_sge(lf, rf); break;
                default: break;
            }
        } else {
            BValue* la = emitBox(st, lhs);
            BValue* rb = emitBox(st, rhs);
            BValue* r_i32 = nullptr;
            switch (bin.op) {
                case BinOp::Eq:
                case BinOp::Ne:
                    r_i32 = b.build_call("brasslua_rt_eq", Type::i32(), {la, rb});
                    bit_res = (bin.op == BinOp::Eq)
                                  ? b.build_ne(r_i32, b.build_iconst_i32(0))
                                  : b.build_eq(r_i32, b.build_iconst_i32(0));
                    break;
                case BinOp::Lt:
                case BinOp::Gt:
                    return emitDynamicNumeric(*this, st, bin.op, la, rb, "brasslua_rt_lt");
                case BinOp::Le:
                case BinOp::Ge:
                    return emitDynamicNumeric(*this, st, bin.op, la, rb, "brasslua_rt_le");
                default:
                    break;
            }
        }
        return MirValue{bit_res, Rep::BoolBit};
    }

    // Pure integer arithmetic (`+`, `-`, `*`, `//`, `%`) when both operands are Int64
    if (both_int64 &&
        (bin.op == BinOp::Add || bin.op == BinOp::Sub || bin.op == BinOp::Mul ||
         bin.op == BinOp::IDiv || bin.op == BinOp::Mod)) {
        BValue* li = emitUnboxInt64(st, lhs);
        BValue* ri = emitUnboxInt64(st, rhs);
        bool li_const = li && li->is_instruction() &&
                        li->defining_instruction()->opcode() == brass::Opcode::iconst_i64;
        bool ri_const = ri && ri->is_instruction() &&
                        ri->defining_instruction()->opcode() == brass::Opcode::iconst_i64;
        if (li_const && ri_const) {
            uint64_t a = static_cast<uint64_t>(li->defining_instruction()->imm_i64());
            uint64_t c = static_cast<uint64_t>(ri->defining_instruction()->imm_i64());
            if (bin.op == BinOp::Add) return MirValue{b.build_iconst_i64(static_cast<int64_t>(a + c)), Rep::Int64};
            if (bin.op == BinOp::Sub) return MirValue{b.build_iconst_i64(static_cast<int64_t>(a - c)), Rep::Int64};
            if (bin.op == BinOp::Mul) return MirValue{b.build_iconst_i64(static_cast<int64_t>(a * c)), Rep::Int64};
        }
        switch (bin.op) {
            case BinOp::Add:  return MirValue{b.build_add(li, ri), Rep::Int64};
            case BinOp::Sub:  return MirValue{b.build_sub(li, ri), Rep::Int64};
            case BinOp::Mul:  return MirValue{b.build_mul(li, ri), Rep::Int64};
            case BinOp::IDiv: return MirValue{b.build_call("brasslua_rt_idiv_i64", Type::i64(), {li, ri}), Rep::Int64};
            case BinOp::Mod:  return MirValue{b.build_call("brasslua_rt_mod_i64", Type::i64(), {li, ri}), Rep::Int64};
            default: break;
        }
    }

    // Float arithmetic only when the result is a float whatever the integers are:
    // `/` and `^`, or a side known to be a float (a Number may be an integer).
    auto known_float = [](const MirValue& v, const Expr& e) {
        return v.rep == Rep::Float64 || (v.rep == Rep::Boxed && e.inferred_type == InferredType::Float64);
    };
    const bool float_result = bin.op == BinOp::Div || bin.op == BinOp::Pow ||
                              known_float(lhs, *bin.lhs) || known_float(rhs, *bin.rhs);
    if (lhs_known_num && rhs_known_num && float_result) {
        BValue* lf = emitUnboxFloat64(st, lhs);
        BValue* rf = emitUnboxFloat64(st, rhs);
        switch (bin.op) {
            case BinOp::Add:  return MirValue{b.build_add(lf, rf), Rep::Float64};
            case BinOp::Sub:  return MirValue{b.build_sub(lf, rf), Rep::Float64};
            case BinOp::Mul:  return MirValue{b.build_mul(lf, rf), Rep::Float64};
            case BinOp::Div:  return MirValue{b.build_sdiv(lf, rf), Rep::Float64};
            case BinOp::IDiv: {
                BValue* q = b.build_sdiv(lf, rf);
                return MirValue{b.build_floor_f64(q), Rep::Float64};
            }
            case BinOp::Mod:
                return MirValue{b.build_call("brasslua_rt_fmod", Type::f64(), {lf, rf}), Rep::Float64};
            case BinOp::Pow:
                return MirValue{b.build_call("brasslua_rt_pow_f64", Type::f64(), {lf, rf}), Rep::Float64};
            default:
                break;
        }
    }

    // Dynamic boxed arithmetic (preserves integer vs float subtype and metamethods!)
    BValue* la = emitBox(st, lhs);
    BValue* rb = emitBox(st, rhs);
    const char* rt_fn = "brasslua_rt_add";
    switch (bin.op) {
        case BinOp::Add:  rt_fn = "brasslua_rt_add"; break;
        case BinOp::Sub:  rt_fn = "brasslua_rt_sub"; break;
        case BinOp::Mul:  rt_fn = "brasslua_rt_mul"; break;
        case BinOp::Div:  rt_fn = "brasslua_rt_div"; break;
        case BinOp::IDiv: rt_fn = "brasslua_rt_idiv"; break;
        case BinOp::Mod:  rt_fn = "brasslua_rt_mod"; break;
        case BinOp::Pow:  rt_fn = "brasslua_rt_pow"; break;
        default: break;
    }
    (void)result_type;
    if (bin.op == BinOp::Add || bin.op == BinOp::Sub || bin.op == BinOp::Mul || bin.op == BinOp::Div) {
        return emitDynamicNumeric(*this, st, bin.op, la, rb, rt_fn);
    }
    return MirValue{b.build_call(rt_fn, Type::i64(), {la, rb}), Rep::Boxed};
}

MirValue MirModuleLowerer::lowerMultiExpr(FunctionBuildState& st, const Expr& expr) {
    auto& b = st.builder;
    if (std::holds_alternative<VarargExpr>(expr.node)) {
        if (st.args_ptr_arg != nullptr) {
            BValue* off_v = b.build_iconst_i64(static_cast<int64_t>(st.vararg_offset));
            BValue* r0 = b.build_call("brasslua_rt_set_vararg_multiret", Type::i64(),
                                      {st.args_ptr_arg, st.nargs_arg, off_v});
            return MirValue{r0, Rep::Boxed, false};
        }
        BValue* zero = b.build_iconst_i64(0);
        b.build_call("brasslua_rt_set_multiret", Type::void_type(), {zero, zero});
        return MirValue{b.build_iconst_i64(static_cast<int64_t>(kNilBits)), Rep::Boxed, false};
    }
    st.multi_result_wanted = true;
    MirValue mv = lowerExpr(st, expr);
    st.multi_result_wanted = false;
    if (!mv.single) return mv;
    BValue* boxed = emitBox(st, mv);
    BValue* one = st.emitEntryAlloca(8, 8);
    b.build_store(Type::i64(), one, 0, boxed);
    b.build_call("brasslua_rt_set_multiret", Type::void_type(), {one, b.build_iconst_i64(1)});
    b.build_store(Type::i64(), one, 0, b.build_iconst_i64(0));
    return MirValue{boxed, Rep::Boxed, false};
}

MirValue MirModuleLowerer::lowerCallExpr(FunctionBuildState& st, const CallExpr& call, InferredType /*result_type*/) {
    auto& b = st.builder;
    const bool want_multi = std::exchange(st.multi_result_wanted, false);

    bool last_is_multiret = !call.args.empty() && isCallOrVararg(*call.args.back());

    // 1. Check for inline math.* and bit.* intrinsics when arguments are numeric (and not multiret tail)
    if (!last_is_multiret) {
        if (const auto* fe = std::get_if<FieldExpr>(&call.callee->node)) {
            if (const auto* base = std::get_if<IdentExpr>(&fe->table->node)) {
                if (base->symbol_id == 0) {
                    if (base->name == "math" && call.args.size() == 1) {
                        if (fe->field == "sqrt") {
                            MirValue a0 = lowerExpr(st, *call.args[0]);
                            BValue* f0 = emitUnboxFloat64(st, a0);
                            return MirValue{b.build_sqrt_f64(f0), Rep::Float64, true};
                        }
                        if (fe->field == "abs" && call.args[0]->inferred_type == InferredType::Float64) {
                            MirValue a0 = lowerExpr(st, *call.args[0]);
                            BValue* f0 = emitUnboxFloat64(st, a0);
                            return MirValue{b.build_fabs_f64(f0), Rep::Float64, true};
                        }
                    }
                    if ((base->name == "bit" || base->name == "bit32") && !call.args.empty()) {
                        auto argI32 = [&](const Expr& e) -> BValue* {
                            MirValue mv = lowerExpr(st, e);
                            BValue* i64 = emitUnboxInt64(st, mv);
                            return b.build_trunc_i32(i64);
                        };
                        auto sext32 = [&](BValue* v32) -> MirValue {
                            return MirValue{b.build_sext_i64(v32), Rep::Int64, true};
                        };
                        if (fe->field == "bnot" && call.args.size() == 1) {
                            BValue* x = argI32(*call.args[0]);
                            return sext32(b.build_xor(x, b.build_iconst_i32(-1)));
                        }
                        if (fe->field == "tobit" && call.args.size() == 1) {
                            return sext32(argI32(*call.args[0]));
                        }
                        if (fe->field == "band" && call.args.size() >= 2) {
                            BValue* acc = argI32(*call.args[0]);
                            for (size_t i = 1; i < call.args.size(); ++i) {
                                acc = b.build_and(acc, argI32(*call.args[i]));
                            }
                            return sext32(acc);
                        }
                        if (fe->field == "bor" && call.args.size() >= 2) {
                            BValue* acc = argI32(*call.args[0]);
                            for (size_t i = 1; i < call.args.size(); ++i) {
                                acc = b.build_or(acc, argI32(*call.args[i]));
                            }
                            return sext32(acc);
                        }
                        if (fe->field == "bxor" && call.args.size() >= 2) {
                            BValue* acc = argI32(*call.args[0]);
                            for (size_t i = 1; i < call.args.size(); ++i) {
                                acc = b.build_xor(acc, argI32(*call.args[i]));
                            }
                            return sext32(acc);
                        }
                        if (fe->field == "lshift" && call.args.size() == 2) {
                            BValue* x = argI32(*call.args[0]);
                            BValue* n = b.build_and(argI32(*call.args[1]), b.build_iconst_i32(31));
                            return sext32(b.build_shl(x, n));
                        }
                        if (fe->field == "rshift" && call.args.size() == 2) {
                            BValue* x = argI32(*call.args[0]);
                            BValue* n = b.build_and(argI32(*call.args[1]), b.build_iconst_i32(31));
                            return sext32(b.build_lshr(x, n));
                        }
                        if (fe->field == "arshift" && call.args.size() == 2) {
                            BValue* x = argI32(*call.args[0]);
                            BValue* n = b.build_and(argI32(*call.args[1]), b.build_iconst_i32(31));
                            return sext32(b.build_ashr(x, n));
                        }
                    }
                }
            }
        }
    }

    // 2. Direct unboxed call when calling a known function with `has_unboxed`
    if (!last_is_multiret) {
        if (const auto* id = std::get_if<IdentExpr>(&call.callee->node)) {
            auto fit = known_functions_.find(varKey(id->symbol_id, id->name));
            if (fit != known_functions_.end() && fit->second.has_unboxed &&
                call.args.size() == fit->second.param_reps.size()) {
                const CompiledFuncSig& sig = fit->second;
                std::vector<MirValue> eval_args;
                eval_args.reserve(call.args.size());
                for (size_t i = 0; i < call.args.size(); ++i) {
                    MirValue amv = lowerExpr(st, *call.args[i]);
                    eval_args.push_back(coerceToRep(st, amv, sig.param_reps[i]));
                }

                // Inline small straight-line numeric helper functions directly at AST/MIR lowering time
                bool is_self_call = (id->symbol_id != 0) ? (id->symbol_id == st.self_func_symbol_id)
                                                         : (id->name == st.self_func_name);
                if (sig.fbody && !is_self_call &&
                    !sig.fbody->body.empty() && sig.fbody->body.size() <= 4) {
                    bool can_inline_ast = true;
                    for (size_t k = 0; k + 1 < sig.fbody->body.size(); ++k) {
                        const auto* ls = std::get_if<LocalStmt>(&sig.fbody->body[k]->node);
                        if (!ls) {
                            can_inline_ast = false;
                            break;
                        }
                        for (const auto& v : ls->vars) {
                            if (v.is_close || v.is_captured) can_inline_ast = false;
                        }
                    }
                    const auto* ret_s = std::get_if<ReturnStmt>(&sig.fbody->body.back()->node);
                    if (!ret_s || ret_s->values.size() != 1) {
                        can_inline_ast = false;
                    }
                    if (can_inline_ast) {
                        pushScope(st);
                        for (size_t i = 0; i < sig.fbody->params.size(); ++i) {
                            Rep pr = sig.param_reps[i];
                            uint32_t psym = i < sig.fbody->param_symbol_ids.size() ? sig.fbody->param_symbol_ids[i] : 0;
                            LocalVarInfo linfo = defineLocal(st, psym, pr, false);
                            b.build_store(mirTypeForRep(pr), linfo.slot_ptr, 0, eval_args[i].val);
                        }
                        for (size_t k = 0; k + 1 < sig.fbody->body.size(); ++k) {
                            lowerStmt(st, *sig.fbody->body[k]);
                        }
                        MirValue ret_mv = lowerExpr(st, *ret_s->values[0]);
                        MirValue coerced_ret = coerceToRep(st, ret_mv, sig.return_rep);
                        popScope(st);
                        coerced_ret.single = true;
                        return coerced_ret;
                    }
                }

                std::vector<BValue*> uargs;
                uargs.reserve(eval_args.size());
                for (const auto& emv : eval_args) {
                    uargs.push_back(emv.val);
                }
                Type ret_t = mirTypeForRep(sig.return_rep);
                BValue* res = b.build_call(sig.unboxed_symbol, ret_t, brass::Span<BValue* const>(uargs));
                return MirValue{res, sig.return_rep, true};
            }
        }
    }

    // 3. General boxed function call (`obj.m(x)` or `f(x)` with NO implicit receiver)
    MirValue callee_mv = lowerExpr(st, *call.callee);
    BValue* callee_bits = emitBox(st, callee_mv);

    // Allocate a dedicated stack buffer for this call's fixed prefix arguments so nested calls
    // with > 4 arguments never overwrite an outer call's arguments.
    size_t fixed_count = last_is_multiret ? (call.args.size() - 1) : call.args.size();
    BValue* arg_buf = fixed_count > 0
                          ? st.emitEntryAlloca(static_cast<uint32_t>(fixed_count * 8), 8)
                          : b.build_iconst_i64(0);
    for (size_t i = 0; i < fixed_count; ++i) {
        MirValue amv = lowerExpr(st, *call.args[i]);
        BValue* abits = emitBox(st, amv);
        b.build_store(Type::i64(), arg_buf, static_cast<int32_t>(i * 8), abits);
    }

    BValue* res = nullptr;
    if (last_is_multiret) {
        (void)lowerMultiExpr(st, *call.args.back());
        BValue* fixed_v = b.build_iconst_i64(static_cast<int64_t>(fixed_count));
        res = b.build_call("brasslua_rt_call_multiret", Type::i64(), {callee_bits, arg_buf, fixed_v});
    } else {
        BValue* nargs_v = b.build_iconst_i64(static_cast<int64_t>(call.args.size()));
        res = want_multi ? b.build_call("brasslua_rt_call", Type::i64(), {callee_bits, arg_buf, nargs_v})
                         : emitClosureCall(st, callee_bits, arg_buf, nargs_v);
    }

    if (fixed_count > 0) {
        BValue* zero = b.build_iconst_i64(0);
        for (size_t i = 0; i < fixed_count; ++i) {
            b.build_store(Type::i64(), arg_buf, static_cast<int32_t>(i * 8), zero);
        }
    }
    return MirValue{res, Rep::Boxed};
}

MirValue MirModuleLowerer::lowerMethodCallExpr(FunctionBuildState& st, const MethodCallExpr& mcall) {
    // Rule 6: `obj:m(x)` statically emits `brasslua_rt_call_method`, passing `obj` as the
    // explicit receiver and `(x)` as the arguments!
    auto& b = st.builder;
    const bool want_multi = std::exchange(st.multi_result_wanted, false);
    MirValue obj_mv = lowerExpr(st, *mcall.object);
    BValue* obj_bits = emitBox(st, obj_mv);
    uint32_t id = mcall.intern_id != 0 ? mcall.intern_id : rt_.internString(mcall.method);

    // The arguments with the receiver first, as the method is called.
    bool last_is_multiret = !mcall.args.empty() && isCallOrVararg(*mcall.args.back());
    size_t fixed_count = (last_is_multiret ? (mcall.args.size() - 1) : mcall.args.size()) + 1;
    BValue* arg_buf = st.emitEntryAlloca(static_cast<uint32_t>(fixed_count * 8), 8);
    b.build_store(Type::i64(), arg_buf, 0, obj_bits);
    for (size_t i = 1; i < fixed_count; ++i) {
        MirValue amv = lowerExpr(st, *mcall.args[i - 1]);
        BValue* abits = emitBox(st, amv);
        b.build_store(Type::i64(), arg_buf, static_cast<int32_t>(i * 8), abits);
    }
    if (last_is_multiret) {
        (void)lowerMultiExpr(st, *mcall.args.back());
    }
    BValue* fixed_c = b.build_iconst_i64(static_cast<int64_t>(fixed_count));

    // A Lua table holding the method, or whose table __index does: call it
    // here. Anything else (a host object's method, a function __index) is
    // the runtime's.
    BasicBlock* hit_bb = st.newBlock("mcall_hit", {Type::i64()});
    BasicBlock* slow_bb = st.newBlock("mcall_slow");
    BasicBlock* merge_bb = st.newBlock("mcall_merge", {Type::i64()});
    emitFieldProbe(st, obj_bits, id, /*through_index=*/true, /*nil_is_hit=*/false, hit_bb, slow_bb);

    b.position_at_end(hit_bb);
    BValue* fast_res = last_is_multiret
        ? b.build_call("brasslua_rt_call_multiret", Type::i64(), {hit_bb->param(0), arg_buf, fixed_c})
        : want_multi ? b.build_call("brasslua_rt_call", Type::i64(), {hit_bb->param(0), arg_buf, fixed_c})
                     : emitClosureCall(st, hit_bb->param(0), arg_buf, fixed_c);
    b.build_br(merge_bb, {fast_res});

    b.position_at_end(slow_bb);
    BValue* key_c = emitStringConst(st, id);
    BValue* slow_res = b.build_call(last_is_multiret ? "brasslua_rt_call_method_multiret" : "brasslua_rt_call_method",
                                    Type::i64(), {obj_bits, key_c, arg_buf, fixed_c});
    b.build_br(merge_bb, {slow_res});

    b.position_at_end(merge_bb);
    BValue* zero = b.build_iconst_i64(0);
    for (size_t i = 0; i < fixed_count; ++i) {
        b.build_store(Type::i64(), arg_buf, static_cast<int32_t>(i * 8), zero);
    }
    return MirValue{merge_bb->param(0), Rep::Boxed};
}

MirValue MirModuleLowerer::lowerExpr(FunctionBuildState& st, const Expr& expr) {
    auto& b = st.builder;
    return std::visit(
        [&](const auto& e) -> MirValue {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, NilExpr>) {
                return MirValue{b.build_iconst_i64(static_cast<int64_t>(kNilBits)), Rep::Boxed};
            } else if constexpr (std::is_same_v<T, BoolExpr>) {
                return MirValue{b.build_iconst_i64(static_cast<int64_t>(e.value ? kTrueBits : kFalseBits)),
                                Rep::Boxed};
            } else if constexpr (std::is_same_v<T, IntExpr>) {
                return MirValue{b.build_iconst_i64(e.value), Rep::Int64};
            } else if constexpr (std::is_same_v<T, FloatExpr>) {
                return MirValue{b.build_fconst_f64(e.value), Rep::Float64};
            } else if constexpr (std::is_same_v<T, StringExpr>) {
                uint32_t id = e.intern_id != 0 ? e.intern_id : rt_.internString(e.value);
                return MirValue{emitStringConst(st, id), Rep::Boxed};
            } else if constexpr (std::is_same_v<T, VarargExpr>) {
                if (st.args_ptr_arg != nullptr) {
                    BValue* off_v = b.build_iconst_i64(static_cast<int64_t>(st.vararg_offset));
                    BValue* has_va = b.build_sgt(st.nargs_arg, off_v);
                    BasicBlock* ld_bb = st.newBlock("va_ld");
                    BasicBlock* nil_bb = st.newBlock("va_nil");
                    BasicBlock* m_bb = st.newBlock("va_merge", {Type::i64()});
                    b.build_br_if(has_va, ld_bb, nil_bb);
                    b.position_at_end(ld_bb);
                    BValue* v0 = b.build_load(Type::i64(), st.args_ptr_arg,
                                              static_cast<int32_t>(st.vararg_offset * 8));
                    b.build_br(m_bb, {v0});
                    b.position_at_end(nil_bb);
                    b.build_br(m_bb, {b.build_iconst_i64(static_cast<int64_t>(kNilBits))});
                    b.position_at_end(m_bb);
                    return MirValue{m_bb->param(0), Rep::Boxed, true};
                }
                return MirValue{b.build_iconst_i64(static_cast<int64_t>(kNilBits)), Rep::Boxed, true};
            } else if constexpr (std::is_same_v<T, IdentExpr>) {
                return loadVar(st, e.symbol_id, e.name, e.intern_id);
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                MirValue op_v = lowerExpr(st, *e.operand);
                switch (e.op) {
                    case UnOp::Not: {
                        BValue* tbit = emitTruthinessBit(st, op_v);
                        return MirValue{b.build_eq(tbit, b.build_iconst_i32(0)), Rep::BoolBit};
                    }
                    case UnOp::Len: {
                        BValue* boxed = emitBox(st, op_v);
                        BValue* len_i64 = b.build_call("brasslua_rt_obj_len", Type::i64(), {boxed});
                        return MirValue{len_i64, Rep::Int64};
                    }
                    case UnOp::Bnot: {
                        if (op_v.rep == Rep::Int64) {
                            return MirValue{b.build_not(op_v.val), Rep::Int64};
                        }
                        BValue* boxed = emitBox(st, op_v);
                        return MirValue{b.build_call("brasslua_rt_bnot", Type::i64(), {boxed}), Rep::Boxed};
                    }
                    case UnOp::Neg: {
                        if (op_v.rep == Rep::Int64) {
                            return MirValue{b.build_neg(op_v.val), Rep::Int64};
                        }
                        if (op_v.rep == Rep::Float64) {
                            return MirValue{b.build_neg(op_v.val), Rep::Float64};
                        }
                        BValue* boxed = emitBox(st, op_v);
                        return MirValue{b.build_call("brasslua_rt_unm", Type::i64(), {boxed}), Rep::Boxed};
                    }
                }
                return op_v;
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                return lowerBinaryExpr(st, e, expr.inferred_type);
            } else if constexpr (std::is_same_v<T, TableExpr>) {
                return lowerTableExpr(st, e);
            } else if constexpr (std::is_same_v<T, IndexExpr>) {
                // A table holds whatever was stored, so the inferred type is
                // only the likely case: the unbox checks the tag.
                MirValue mv = lowerIndexLoad(st, e, expr.inferred_type);
                if (expr.inferred_type == InferredType::Float64 && mv.rep == Rep::Boxed) {
                    return MirValue{emitUnboxFloat64(st, mv), Rep::Float64};
                }
                if (expr.inferred_type == InferredType::Int64 && mv.rep == Rep::Boxed) {
                    return MirValue{emitUnboxInt64(st, mv), Rep::Int64};
                }
                return mv;
            } else if constexpr (std::is_same_v<T, FieldExpr>) {
                MirValue mv = lowerFieldLoad(st, e, expr.inferred_type);
                if (expr.inferred_type == InferredType::Float64 && mv.rep == Rep::Boxed) {
                    return MirValue{emitUnboxFloat64(st, mv), Rep::Float64};
                }
                if (expr.inferred_type == InferredType::Int64 && mv.rep == Rep::Boxed) {
                    return MirValue{emitUnboxInt64(st, mv), Rep::Int64};
                }
                return mv;
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                return lowerCallExpr(st, e, expr.inferred_type);
            } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                return lowerMethodCallExpr(st, e);
            } else if constexpr (std::is_same_v<T, FuncExpr>) {
                CompiledFuncSig sig = compileFuncBody(&st, "", 0, e.func, false);
                BValue* fn_addr = b.build_func_addr(sig.boxed_symbol);
                BValue* unboxed_addr = sig.has_unboxed ? b.build_func_addr(sig.unboxed_symbol)
                                                      : b.build_iconst_i64(0);
                BValue* uv_cnt = b.build_iconst_i32(static_cast<int32_t>(sig.captured_vars.size()));
                BValue* arity_c = b.build_iconst_i32(static_cast<int32_t>(sig.arity));
                BValue* cl_bits = b.build_call("brasslua_rt_closure_new", Type::i64(),
                                              {fn_addr, unboxed_addr, uv_cnt, arity_c});
                populateClosureUpvalues(*this, st, sig, cl_bits);
                return MirValue{cl_bits, Rep::Boxed};
            }
        },
        expr.node);
}

} // namespace brasslua
