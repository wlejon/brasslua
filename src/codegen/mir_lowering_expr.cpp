#include "codegen/mir_lowering.h"

namespace brasslua {

using brass::BasicBlock;
using brass::Type;

MirValue MirModuleLowerer::lowerIndexLoad(FunctionBuildState& st, const IndexExpr& idx_expr,
                                          InferredType inferred_type) {
    auto& b = st.builder;
    MirValue tbl_mv = lowerExpr(st, *idx_expr.table);
    BValue* tbl_bits = emitBox(st, tbl_mv);
    MirValue key_mv = lowerExpr(st, *idx_expr.index);

    if (key_mv.rep == Rep::Int64 || idx_expr.index->inferred_type == InferredType::Int64) {
        BValue* idx_i64 = emitUnboxInt64(st, key_mv);
        bool known_plain_tbl = (inferred_type == InferredType::Float64 ||
                                inferred_type == InferredType::Int64 ||
                                inferred_type == InferredType::Number ||
                                inferred_type == InferredType::Table);
        if (known_plain_tbl) {
            BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
            BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
            BValue* zero_idx = b.build_sub(idx_i64, b.build_iconst_i64(1));
            BValue* arr_ptr = b.build_load(Type::ptr(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_data)));
            if (inferred_type == InferredType::Float64) {
                BValue* fval = b.build_load_indexed(Type::f64(), arr_ptr, zero_idx, 8);
                return MirValue{fval, Rep::Float64};
            }
            BValue* fast_val = b.build_load_indexed(Type::i64(), arr_ptr, zero_idx, 8);
            return MirValue{fast_val, Rep::Boxed};
        }
        // Fast inline array load when:
        // 1. (tbl_bits >> 48) == Tag::Object (0xFFF1)
        // 2. tbl->flags == 0 (regular table without __index metamethod)
        // 3. (uint64_t)(idx - 1) < (uint64_t)tbl->array_cap
        BValue* tag = b.build_lshr(tbl_bits, b.build_iconst_i64(48));
        BValue* is_tbl = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Object)));

        BasicBlock* check_bounds_bb = st.newBlock("arr_ld_bounds");
        BasicBlock* fast_ld_bb = st.newBlock("arr_ld_fast");
        BasicBlock* slow_ld_bb = st.newBlock("arr_ld_slow");
        BasicBlock* merge_bb = st.newBlock("arr_ld_merge", {Type::i64()});
        b.build_br_if(is_tbl, check_bounds_bb, slow_ld_bb);

        b.position_at_end(check_bounds_bb);
        BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
        BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
        BValue* flags = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, flags)));
        BValue* no_meta = b.build_eq(flags, b.build_iconst_i32(0));
        BValue* cap_i32 = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_cap)));
        BValue* cap_i64 = b.build_zext_i64(cap_i32);
        BValue* zero_idx = b.build_sub(idx_i64, b.build_iconst_i64(1));
        BValue* in_bounds = b.build_ult(zero_idx, cap_i64);
        BValue* ok_fast = b.build_and(no_meta, in_bounds);
        b.build_br_if(ok_fast, fast_ld_bb, slow_ld_bb);

        b.position_at_end(fast_ld_bb);
        BValue* arr_ptr = b.build_load(Type::ptr(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_data)));
        BValue* fast_val = b.build_load_indexed(Type::i64(), arr_ptr, zero_idx, 8);
        b.build_br(merge_bb, {fast_val});

        b.position_at_end(slow_ld_bb);
        BValue* slow_val = b.build_call("brasslua_rt_table_get_int", Type::i64(), {tbl_bits, idx_i64});
        b.build_br(merge_bb, {slow_val});

        b.position_at_end(merge_bb);
        return MirValue{merge_bb->param(0), Rep::Boxed};
    }

    BValue* key_bits = emitBox(st, key_mv);
    BValue* res = b.build_call("brasslua_rt_table_get", Type::i64(), {tbl_bits, key_bits});
    return MirValue{res, Rep::Boxed};
}

void MirModuleLowerer::lowerIndexStore(FunctionBuildState& st, const IndexExpr& idx_expr, MirValue rhs,
                                       InferredType target_type) {
    auto& b = st.builder;
    MirValue tbl_mv = lowerExpr(st, *idx_expr.table);
    BValue* tbl_bits = emitBox(st, tbl_mv);
    MirValue key_mv = lowerExpr(st, *idx_expr.index);

    if (key_mv.rep == Rep::Int64 || idx_expr.index->inferred_type == InferredType::Int64) {
        BValue* idx_i64 = emitUnboxInt64(st, key_mv);
        bool known_plain_tbl = (target_type == InferredType::Float64 ||
                                target_type == InferredType::Int64 ||
                                target_type == InferredType::Number ||
                                target_type == InferredType::Table);
        if (known_plain_tbl) {
            BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
            BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
            BValue* cap_i32 = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_cap)));
            BValue* cap_i64 = b.build_zext_i64(cap_i32);
            BValue* zero_idx = b.build_sub(idx_i64, b.build_iconst_i64(1));
            BValue* in_bounds = b.build_ult(zero_idx, cap_i64);

            BasicBlock* fast_st_bb = st.newBlock("arr_st_fast");
            BasicBlock* slow_st_bb = st.newBlock("arr_st_slow");
            BasicBlock* merge_bb = st.newBlock("arr_st_merge");

            if (rhs.rep == Rep::Float64) {
                b.build_br_if(in_bounds, fast_st_bb, slow_st_bb);

                b.position_at_end(fast_st_bb);
                BValue* arr_ptr = b.build_load(Type::ptr(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_data)));
                b.build_store_indexed(Type::f64(), arr_ptr, zero_idx, 8, rhs.val);
                b.build_br(merge_bb);

                b.position_at_end(slow_st_bb);
                BValue* val_bits = emitBox(st, rhs);
                b.build_call("brasslua_rt_table_set_int", Type::void_type(), {tbl_bits, idx_i64, val_bits});
                b.build_br(merge_bb);

                b.position_at_end(merge_bb);
                return;
            }

            BValue* val_bits = nullptr;
            if (rhs.rep == Rep::Int64) {
                if (rhs.val && rhs.val->is_instruction() &&
                    rhs.val->defining_instruction()->opcode() == brass::Opcode::iconst_i64 &&
                    Value::fitsInlineInt(rhs.val->defining_instruction()->imm_i64())) {
                    int64_t imm = rhs.val->defining_instruction()->imm_i64();
                    uint64_t bits = (static_cast<uint64_t>(Tag::Int47) << kTagShift) |
                                    (static_cast<uint64_t>(imm) & kPayloadMask);
                    val_bits = b.build_iconst_i64(static_cast<int64_t>(bits));
                } else {
                    BValue* tag_bits = b.build_iconst_i64(
                        static_cast<int64_t>(static_cast<uint64_t>(Tag::Int47) << kTagShift));
                    val_bits = b.build_or(tag_bits, b.build_and(rhs.val, payload_mask));
                }
            } else {
                val_bits = emitBox(st, rhs);
            }

            b.build_br_if(in_bounds, fast_st_bb, slow_st_bb);

            b.position_at_end(fast_st_bb);
            BValue* arr_ptr = b.build_load(Type::ptr(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_data)));
            b.build_store_indexed(Type::i64(), arr_ptr, zero_idx, 8, val_bits);
            b.build_br(merge_bb);

            b.position_at_end(slow_st_bb);
            b.build_call("brasslua_rt_table_set_int", Type::void_type(), {tbl_bits, idx_i64, val_bits});
            b.build_br(merge_bb);

            b.position_at_end(merge_bb);
            return;
        }

        BValue* val_bits = emitBox(st, rhs);
        BValue* tag = b.build_lshr(tbl_bits, b.build_iconst_i64(48));
        BValue* is_tbl = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Object)));

        BasicBlock* check_bounds_bb = st.newBlock("arr_st_bounds");
        BasicBlock* fast_st_bb = st.newBlock("arr_st_fast");
        BasicBlock* slow_st_bb = st.newBlock("arr_st_slow");
        BasicBlock* merge_bb = st.newBlock("arr_st_merge");
        b.build_br_if(is_tbl, check_bounds_bb, slow_st_bb);

        b.position_at_end(check_bounds_bb);
        BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
        BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
        BValue* flags = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, flags)));
        BValue* no_meta = b.build_eq(flags, b.build_iconst_i32(0));
        BValue* cap_i32 = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_cap)));
        BValue* cap_i64 = b.build_zext_i64(cap_i32);
        BValue* zero_idx = b.build_sub(idx_i64, b.build_iconst_i64(1));
        BValue* in_bounds = b.build_ult(zero_idx, cap_i64);
        BValue* not_nil = b.build_ne(val_bits, b.build_iconst_i64(static_cast<int64_t>(kNilBits)));
        BValue* ok1 = b.build_and(no_meta, in_bounds);
        BValue* ok_fast = b.build_and(ok1, not_nil);
        b.build_br_if(ok_fast, fast_st_bb, slow_st_bb);

        b.position_at_end(fast_st_bb);
        BValue* arr_ptr = b.build_load(Type::ptr(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_data)));
        b.build_store_indexed(Type::i64(), arr_ptr, zero_idx, 8, val_bits);
        b.build_br(merge_bb);

        b.position_at_end(slow_st_bb);
        b.build_call("brasslua_rt_table_set_int", Type::void_type(), {tbl_bits, idx_i64, val_bits});
        b.build_br(merge_bb);

        b.position_at_end(merge_bb);
        return;
    }

    BValue* val_bits = emitBox(st, rhs);
    BValue* key_bits = emitBox(st, key_mv);
    b.build_call("brasslua_rt_table_set", Type::void_type(), {tbl_bits, key_bits, val_bits});
}

MirValue MirModuleLowerer::lowerFieldLoad(FunctionBuildState& st, const FieldExpr& fld_expr,
                                          InferredType inferred_type) {
    auto& b = st.builder;
    MirValue tbl_mv = lowerExpr(st, *fld_expr.table);
    BValue* tbl_bits = emitBox(st, tbl_mv);
    uint32_t id = fld_expr.intern_id != 0 ? fld_expr.intern_id : rt_.internString(fld_expr.field);
    int32_t slot = fld_expr.slot_hint >= 0 ? fld_expr.slot_hint : rt_.findFieldSlot(id);

    if (slot >= 0 && slot < static_cast<int32_t>(LuaTable::kInlineSlots)) {
        if (inferred_type == InferredType::Float64 ||
            inferred_type == InferredType::Int64 ||
            inferred_type == InferredType::Number) {
            BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
            BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
            int32_t slot_offset = static_cast<int32_t>(offsetof(LuaTable, slots)) + slot * 8;
            if (inferred_type == InferredType::Float64) {
                BValue* fval = b.build_load(Type::f64(), tbl_ptr, slot_offset);
                return MirValue{fval, Rep::Float64};
            }
            BValue* slot_val = b.build_load(Type::i64(), tbl_ptr, slot_offset);
            return MirValue{slot_val, Rep::Boxed};
        }

        BValue* tag = b.build_lshr(tbl_bits, b.build_iconst_i64(48));
        BValue* is_tbl = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Object)));

        BasicBlock* check_meta_bb = st.newBlock("fld_ld_meta");
        BasicBlock* fast_bb = st.newBlock("fld_ld_fast");
        BasicBlock* slow_bb = st.newBlock("fld_ld_slow");
        BasicBlock* merge_bb = st.newBlock("fld_ld_merge", {Type::i64()});
        b.build_br_if(is_tbl, check_meta_bb, slow_bb);

        b.position_at_end(check_meta_bb);
        BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
        BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
        BValue* flags = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, flags)));
        BValue* no_meta = b.build_eq(flags, b.build_iconst_i32(0));
        b.build_br_if(no_meta, fast_bb, slow_bb);

        b.position_at_end(fast_bb);
        int32_t slot_offset = static_cast<int32_t>(offsetof(LuaTable, slots)) + slot * 8;
        BValue* slot_val = b.build_load(Type::i64(), tbl_ptr, slot_offset);
        b.build_br(merge_bb, {slot_val});

        b.position_at_end(slow_bb);
        BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
        BValue* slot_c = b.build_iconst_i32(slot);
        BValue* slow_val = b.build_call("brasslua_rt_table_get_field", Type::i64(), {tbl_bits, id_c, slot_c});
        b.build_br(merge_bb, {slow_val});

        b.position_at_end(merge_bb);
        return MirValue{merge_bb->param(0), Rep::Boxed};
    }

    BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
    BValue* slot_c = b.build_iconst_i32(static_cast<int32_t>(0xFFFFFFFFu));
    BValue* res = b.build_call("brasslua_rt_table_get_field", Type::i64(), {tbl_bits, id_c, slot_c});
    return MirValue{res, Rep::Boxed};
}

void MirModuleLowerer::lowerFieldStore(FunctionBuildState& st, const FieldExpr& fld_expr, MirValue rhs,
                                       InferredType target_type) {
    auto& b = st.builder;
    MirValue tbl_mv = lowerExpr(st, *fld_expr.table);
    BValue* tbl_bits = emitBox(st, tbl_mv);
    uint32_t id = fld_expr.intern_id != 0 ? fld_expr.intern_id : rt_.internString(fld_expr.field);
    int32_t slot = fld_expr.slot_hint >= 0 ? fld_expr.slot_hint : rt_.getOrAssignFieldSlot(id);

    if (slot >= 0 && slot < static_cast<int32_t>(LuaTable::kInlineSlots)) {
        if (target_type == InferredType::Float64 ||
            target_type == InferredType::Int64 ||
            target_type == InferredType::Number) {
            BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
            BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
            int32_t slot_offset = static_cast<int32_t>(offsetof(LuaTable, slots)) + slot * 8;
            if (rhs.rep == Rep::Float64) {
                b.build_store(Type::f64(), tbl_ptr, slot_offset, rhs.val);
                return;
            }
            BValue* val_bits = emitBox(st, rhs);
            b.build_store(Type::i64(), tbl_ptr, slot_offset, val_bits);
            return;
        }

        BValue* val_bits = emitBox(st, rhs);
        BValue* tag = b.build_lshr(tbl_bits, b.build_iconst_i64(48));
        BValue* is_tbl = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Object)));

        BasicBlock* check_meta_bb = st.newBlock("fld_st_meta");
        BasicBlock* fast_bb = st.newBlock("fld_st_fast");
        BasicBlock* slow_bb = st.newBlock("fld_st_slow");
        BasicBlock* merge_bb = st.newBlock("fld_st_merge");
        b.build_br_if(is_tbl, check_meta_bb, slow_bb);

        b.position_at_end(check_meta_bb);
        BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
        BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
        BValue* flags = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, flags)));
        BValue* no_meta = b.build_eq(flags, b.build_iconst_i32(0));
        b.build_br_if(no_meta, fast_bb, slow_bb);

        b.position_at_end(fast_bb);
        int32_t slot_offset = static_cast<int32_t>(offsetof(LuaTable, slots)) + slot * 8;
        b.build_store(Type::i64(), tbl_ptr, slot_offset, val_bits);
        b.build_br(merge_bb);

        b.position_at_end(slow_bb);
        BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
        BValue* slot_c = b.build_iconst_i32(slot);
        b.build_call("brasslua_rt_table_set_field", Type::void_type(), {tbl_bits, id_c, slot_c, val_bits});
        b.build_br(merge_bb);

        b.position_at_end(merge_bb);
        return;
    }

    BValue* val_bits = emitBox(st, rhs);
    BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
    BValue* slot_c = b.build_iconst_i32(static_cast<int32_t>(0xFFFFFFFFu));
    b.build_call("brasslua_rt_table_set_field", Type::void_type(), {tbl_bits, id_c, slot_c, val_bits});
}

MirValue MirModuleLowerer::lowerTableExpr(FunctionBuildState& st, const TableExpr& tbl_expr) {
    auto& b = st.builder;
    BValue* arr_hint = b.build_iconst_i32(static_cast<int32_t>(tbl_expr.array_count + 4));
    BValue* hash_hint = b.build_iconst_i32(static_cast<int32_t>(tbl_expr.hash_count));
    BValue* tbl_bits = b.build_call("brasslua_rt_table_new", Type::i64(), {arr_hint, hash_hint});

    int64_t seq_idx = 1;
    for (size_t i = 0; i < tbl_expr.fields.size(); ++i) {
        const auto& f = tbl_expr.fields[i];
        bool is_last = (i + 1 == tbl_expr.fields.size());
        bool is_multiret_expr = is_last && !f.key && f.value &&
                                (std::holds_alternative<CallExpr>(f.value->node) ||
                                 std::holds_alternative<MethodCallExpr>(f.value->node));
        MirValue val_mv = is_multiret_expr ? lowerMultiExpr(st, *f.value) : lowerExpr(st, *f.value);
        BValue* val_bits = emitBox(st, val_mv);

        if (is_multiret_expr) {
            BValue* mcount = b.build_call("brasslua_rt_get_multiret_count", Type::i64());
            BValue* loop_slot = st.emitEntryAlloca(8, 8);
            b.build_store(Type::i64(), loop_slot, 0, b.build_iconst_i64(0));
            BasicBlock* cond_bb = st.newBlock("tbl_mret_cond");
            BasicBlock* body_bb = st.newBlock("tbl_mret_body");
            BasicBlock* end_bb = st.newBlock("tbl_mret_end");
            b.build_br(cond_bb);

            b.position_at_end(cond_bb);
            BValue* k = b.build_load(Type::i64(), loop_slot, 0);
            BValue* cmp = b.build_slt(k, mcount);
            b.build_br_if(cmp, body_bb, end_bb);

            b.position_at_end(body_bb);
            BValue* mv = b.build_call("brasslua_rt_get_multiret", Type::i64(), {k});
            BValue* dst_idx = b.build_add(b.build_iconst_i64(seq_idx), k);
            b.build_call("brasslua_rt_table_set_int", Type::void_type(), {tbl_bits, dst_idx, mv});
            b.build_store(Type::i64(), loop_slot, 0, b.build_add(k, b.build_iconst_i64(1)));
            b.build_br(cond_bb);

            b.position_at_end(end_bb);
            continue;
        }

        if (!f.key) {
            BValue* idx_c = b.build_iconst_i64(seq_idx++);
            b.build_call("brasslua_rt_table_set_int", Type::void_type(), {tbl_bits, idx_c, val_bits});
        } else if (!f.field_name.empty()) {
            uint32_t id = f.intern_id != 0 ? f.intern_id : rt_.internString(f.field_name);
            int32_t slot = f.slot_hint >= 0 ? f.slot_hint : rt_.getOrAssignFieldSlot(id);
            BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
            BValue* slot_c = b.build_iconst_i32(slot >= 0 ? slot : static_cast<int32_t>(0xFFFFFFFFu));
            b.build_call("brasslua_rt_table_set_field", Type::void_type(), {tbl_bits, id_c, slot_c, val_bits});
        } else {
            MirValue kmv = lowerExpr(st, *f.key);
            BValue* k_bits = emitBox(st, kmv);
            b.build_call("brasslua_rt_table_set", Type::void_type(), {tbl_bits, k_bits, val_bits});
        }
    }
    return MirValue{tbl_bits, Rep::Boxed};
}

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
                    r_i32 = b.build_call("brasslua_rt_lt", Type::i32(), {la, rb});
                    bit_res = b.build_ne(r_i32, b.build_iconst_i32(0));
                    break;
                case BinOp::Le:
                    r_i32 = b.build_call("brasslua_rt_le", Type::i32(), {la, rb});
                    bit_res = b.build_ne(r_i32, b.build_iconst_i32(0));
                    break;
                case BinOp::Gt:
                    r_i32 = b.build_call("brasslua_rt_lt", Type::i32(), {rb, la});
                    bit_res = b.build_ne(r_i32, b.build_iconst_i32(0));
                    break;
                case BinOp::Ge:
                    r_i32 = b.build_call("brasslua_rt_le", Type::i32(), {rb, la});
                    bit_res = b.build_ne(r_i32, b.build_iconst_i32(0));
                    break;
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

    if (lhs_known_num && rhs_known_num) {
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
    return MirValue{b.build_call(rt_fn, Type::i64(), {la, rb}), Rep::Boxed};
}

MirValue MirModuleLowerer::lowerMultiExpr(FunctionBuildState& st, const Expr& expr) {
    MirValue mv = lowerExpr(st, expr);
    if (!mv.single) return mv;
    auto& b = st.builder;
    BValue* boxed = emitBox(st, mv);
    BValue* one = st.emitEntryAlloca(8, 8);
    b.build_store(Type::i64(), one, 0, boxed);
    b.build_call("brasslua_rt_set_multiret", Type::void_type(), {one, b.build_iconst_i64(1)});
    return MirValue{boxed, Rep::Boxed};
}

MirValue MirModuleLowerer::lowerCallExpr(FunctionBuildState& st, const CallExpr& call, InferredType /*result_type*/) {
    auto& b = st.builder;

    // 1. Check for inline math.* and bit.* intrinsics when arguments are numeric
    if (const auto* fe = std::get_if<FieldExpr>(&call.callee->node)) {
        if (const auto* base = std::get_if<IdentExpr>(&fe->table->node)) {
            if (!lookupLocal(st, base->name)) {
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

    // 2. Direct unboxed call when calling a known function with `has_unboxed`
    if (const auto* id = std::get_if<IdentExpr>(&call.callee->node)) {
        auto fit = known_functions_.find(id->name);
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
            if (sig.fbody && id->name != st.self_func_name &&
                !sig.fbody->body.empty() && sig.fbody->body.size() <= 4) {
                bool can_inline_ast = true;
                for (size_t k = 0; k + 1 < sig.fbody->body.size(); ++k) {
                    const auto* ls = std::get_if<LocalStmt>(&sig.fbody->body[k]->node);
                    if (!ls) {
                        can_inline_ast = false;
                        break;
                    }
                    for (const auto& v : ls->vars) {
                        if (v.is_close) can_inline_ast = false;
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
                        LocalVarInfo linfo = defineLocal(st, sig.fbody->params[i], pr, false);
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

    // 3. General boxed function call (`obj.m(x)` or `f(x)` with NO implicit receiver)
    MirValue callee_mv = lowerExpr(st, *call.callee);
    BValue* callee_bits = emitBox(st, callee_mv);

    bool last_is_multiret = !call.args.empty() &&
                            (std::holds_alternative<CallExpr>(call.args.back()->node) ||
                             std::holds_alternative<MethodCallExpr>(call.args.back()->node));

    if (!last_is_multiret && call.args.size() <= 4) {
        std::vector<BValue*> bargs;
        bargs.push_back(callee_bits);
        for (const auto& arg : call.args) {
            MirValue amv = lowerExpr(st, *arg);
            bargs.push_back(emitBox(st, amv));
        }
        const char* helper = "brasslua_rt_call_0";
        if (call.args.size() == 1) helper = "brasslua_rt_call_1";
        else if (call.args.size() == 2) helper = "brasslua_rt_call_2";
        else if (call.args.size() == 3) helper = "brasslua_rt_call_3";
        else if (call.args.size() == 4) helper = "brasslua_rt_call_4";
        BValue* res = b.build_call(helper, Type::i64(), brass::Span<BValue* const>(bargs));
        return MirValue{res, Rep::Boxed};
    }

    size_t fixed_count = last_is_multiret ? (call.args.size() - 1) : call.args.size();
    BValue* arg_buf = fixed_count > 12 ? st.emitEntryAlloca(static_cast<uint32_t>((fixed_count + 8) * 8), 8)
                                      : st.getArgsScratch();
    for (size_t i = 0; i < fixed_count; ++i) {
        MirValue amv = lowerExpr(st, *call.args[i]);
        BValue* abits = emitBox(st, amv);
        b.build_store(Type::i64(), arg_buf, static_cast<int32_t>(i * 8), abits);
    }

    if (last_is_multiret) {
        (void)lowerMultiExpr(st, *call.args.back());
        BValue* mcount = b.build_call("brasslua_rt_get_multiret_count", Type::i64());
        BValue* k_slot = st.emitEntryAlloca(8, 8);
        b.build_store(Type::i64(), k_slot, 0, b.build_iconst_i64(0));
        BasicBlock* cond_bb = st.newBlock("call_mret_cond");
        BasicBlock* body_bb = st.newBlock("call_mret_body");
        BasicBlock* end_bb = st.newBlock("call_mret_end");
        b.build_br(cond_bb);

        b.position_at_end(cond_bb);
        BValue* k = b.build_load(Type::i64(), k_slot, 0);
        BValue* cmp = b.build_slt(k, mcount);
        b.build_br_if(cmp, body_bb, end_bb);

        b.position_at_end(body_bb);
        BValue* mv = b.build_call("brasslua_rt_get_multiret", Type::i64(), {k});
        BValue* dst_idx = b.build_add(b.build_iconst_i64(static_cast<int64_t>(fixed_count)), k);
        b.build_store_indexed(Type::i64(), arg_buf, dst_idx, 8, mv);
        b.build_store(Type::i64(), k_slot, 0, b.build_add(k, b.build_iconst_i64(1)));
        b.build_br(cond_bb);

        b.position_at_end(end_bb);
        BValue* total_nargs = b.build_add(b.build_iconst_i64(static_cast<int64_t>(fixed_count)), mcount);
        BValue* res = b.build_call("brasslua_rt_call", Type::i64(), {callee_bits, arg_buf, total_nargs});
        return MirValue{res, Rep::Boxed};
    }

    BValue* nargs_v = b.build_iconst_i64(static_cast<int64_t>(call.args.size()));
    BValue* res = b.build_call("brasslua_rt_call", Type::i64(), {callee_bits, arg_buf, nargs_v});
    return MirValue{res, Rep::Boxed};
}

MirValue MirModuleLowerer::lowerMethodCallExpr(FunctionBuildState& st, const MethodCallExpr& mcall) {
    // Rule 6: `obj:m(x)` statically emits `brasslua_rt_call_method`, passing `obj` as the
    // explicit receiver and `(x)` as the arguments!
    auto& b = st.builder;
    MirValue obj_mv = lowerExpr(st, *mcall.object);
    BValue* obj_bits = emitBox(st, obj_mv);
    uint32_t id = mcall.intern_id != 0 ? mcall.intern_id : rt_.internString(mcall.method);
    int32_t slot = mcall.slot_hint >= 0 ? mcall.slot_hint : rt_.findFieldSlot(id);

    size_t nargs = mcall.args.size();
    BValue* arg_buf = nargs > 14 ? st.emitEntryAlloca(static_cast<uint32_t>(nargs * 8), 8) : st.getArgsScratch();
    for (size_t i = 0; i < nargs; ++i) {
        MirValue amv = lowerExpr(st, *mcall.args[i]);
        BValue* abits = emitBox(st, amv);
        b.build_store(Type::i64(), arg_buf, static_cast<int32_t>(i * 8), abits);
    }

    BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
    BValue* slot_c = b.build_iconst_i32(slot >= 0 ? slot : static_cast<int32_t>(0xFFFFFFFFu));
    BValue* nargs_c = b.build_iconst_i64(static_cast<int64_t>(nargs));
    BValue* res = b.build_call("brasslua_rt_call_method", Type::i64(), {obj_bits, id_c, slot_c, arg_buf, nargs_c});
    return MirValue{res, Rep::Boxed};
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
                BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
                return MirValue{b.build_call("brasslua_rt_string_from_id", Type::i64(), {id_c}), Rep::Boxed};
            } else if constexpr (std::is_same_v<T, VarargExpr>) {
                if (st.args_ptr_arg != nullptr) {
                    BValue* has_va = b.build_sgt(st.nargs_arg, b.build_iconst_i64(0));
                    BasicBlock* ld_bb = st.newBlock("va_ld");
                    BasicBlock* nil_bb = st.newBlock("va_nil");
                    BasicBlock* m_bb = st.newBlock("va_merge", {Type::i64()});
                    b.build_br_if(has_va, ld_bb, nil_bb);
                    b.position_at_end(ld_bb);
                    BValue* v0 = b.build_load(Type::i64(), st.args_ptr_arg, 0);
                    b.build_br(m_bb, {v0});
                    b.position_at_end(nil_bb);
                    b.build_br(m_bb, {b.build_iconst_i64(static_cast<int64_t>(kNilBits))});
                    b.position_at_end(m_bb);
                    return MirValue{m_bb->param(0), Rep::Boxed};
                }
                return MirValue{b.build_iconst_i64(static_cast<int64_t>(kNilBits)), Rep::Boxed};
            } else if constexpr (std::is_same_v<T, IdentExpr>) {
                return loadVar(st, e.name, e.intern_id);
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
                CompiledFuncSig sig = compileFuncBody(&st, "", e.func, false);
                BValue* fn_addr = b.build_func_addr(sig.boxed_symbol);
                BValue* unboxed_addr = sig.has_unboxed ? b.build_func_addr(sig.unboxed_symbol)
                                                      : b.build_iconst_i64(0);
                BValue* uv_cnt = b.build_iconst_i32(static_cast<int32_t>(sig.captured_names.size()));
                BValue* arity_c = b.build_iconst_i32(static_cast<int32_t>(sig.arity));
                BValue* cl_bits = b.build_call("brasslua_rt_closure_new", Type::i64(),
                                              {fn_addr, unboxed_addr, uv_cnt, arity_c});

                if (!sig.captured_names.empty()) {
                    BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
                    BValue* cl_ptr = b.build_and(cl_bits, payload_mask);
                    BValue* uvs_ptr = b.build_load(Type::ptr(), cl_ptr, static_cast<int32_t>(offsetof(LuaClosure, upvalues)));
                    for (size_t u = 0; u < sig.captured_names.size(); ++u) {
                        LocalVarInfo* ploc = lookupLocal(st, sig.captured_names[u]);
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
                return MirValue{cl_bits, Rep::Boxed};
            }
        },
        expr.node);
}

} // namespace brasslua
