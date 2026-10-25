#include "codegen/mir_lowering.h"
#include "runtime/runtime_internal.h"

namespace brasslua {

using brass::BasicBlock;
using brass::Type;

namespace {

bool isCallOrVararg(const Expr& e) noexcept {
    return std::holds_alternative<CallExpr>(e.node) ||
           std::holds_alternative<MethodCallExpr>(e.node) ||
           std::holds_alternative<VarargExpr>(e.node);
}

} // namespace

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
                b.build_call("brasslua_rt_table_set_int", Type::i64(), {tbl_bits, idx_i64, val_bits});
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
            b.build_call("brasslua_rt_table_set_int", Type::i64(), {tbl_bits, idx_i64, val_bits});
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
        b.build_call("brasslua_rt_table_set_int", Type::i64(), {tbl_bits, idx_i64, val_bits});
        b.build_br(merge_bb);

        b.position_at_end(merge_bb);
        return;
    }

    BValue* val_bits = emitBox(st, rhs);
    BValue* key_bits = emitBox(st, key_mv);
    b.build_call("brasslua_rt_table_set", Type::i64(), {tbl_bits, key_bits, val_bits});
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
        BasicBlock* slow_bb = st.newBlock("fld_ld_slow");
        BasicBlock* merge_bb = st.newBlock("fld_ld_merge", {Type::i64()});
        b.build_br_if(is_tbl, check_meta_bb, slow_bb);

        b.position_at_end(check_meta_bb);
        BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
        BValue* tbl_ptr = b.build_and(tbl_bits, payload_mask);
        int32_t slot_offset = static_cast<int32_t>(offsetof(LuaTable, slots)) + slot * 8;
        BValue* slot_val = b.build_load(Type::i64(), tbl_ptr, slot_offset);
        BValue* flags = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, flags)));
        BValue* can_fast = nullptr;
        if (rt_.hostBridge().isHostObject) {
            can_fast = b.build_eq(flags, b.build_iconst_i32(0));
        } else {
            BValue* not_nil = b.build_ne(slot_val, b.build_iconst_i64(static_cast<int64_t>(kNilBits)));
            BValue* no_index_mm = b.build_eq(b.build_and(flags, b.build_iconst_i32(static_cast<int32_t>(kMetaIndex))),
                                             b.build_iconst_i32(0));
            can_fast = b.build_or(not_nil, no_index_mm);
        }
        b.build_br_if(can_fast, merge_bb, {slot_val}, slow_bb, {});

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
        int32_t slot_offset = static_cast<int32_t>(offsetof(LuaTable, slots)) + slot * 8;
        BValue* flags = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, flags)));
        BValue* can_fast = nullptr;
        if (rt_.hostBridge().isHostObject) {
            can_fast = b.build_eq(flags, b.build_iconst_i32(0));
        } else {
            BValue* cur_val = b.build_load(Type::i64(), tbl_ptr, slot_offset);
            BValue* exists = b.build_ne(cur_val, b.build_iconst_i64(static_cast<int64_t>(kNilBits)));
            BValue* no_newidx = b.build_eq(b.build_and(flags, b.build_iconst_i32(static_cast<int32_t>(kMetaNewIndex))),
                                           b.build_iconst_i32(0));
            BValue* val_not_nil = b.build_ne(val_bits, b.build_iconst_i64(static_cast<int64_t>(kNilBits)));
            can_fast = b.build_and(b.build_or(exists, no_newidx), val_not_nil);
        }
        b.build_br_if(can_fast, fast_bb, slow_bb);

        b.position_at_end(fast_bb);
        b.build_store(Type::i64(), tbl_ptr, slot_offset, val_bits);
        b.build_br(merge_bb);

        b.position_at_end(slow_bb);
        BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
        BValue* slot_c = b.build_iconst_i32(slot);
        b.build_call("brasslua_rt_table_set_field", Type::i64(), {tbl_bits, id_c, slot_c, val_bits});
        b.build_br(merge_bb);

        b.position_at_end(merge_bb);
        return;
    }

    BValue* val_bits = emitBox(st, rhs);
    BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(id));
    BValue* slot_c = b.build_iconst_i32(static_cast<int32_t>(0xFFFFFFFFu));
    b.build_call("brasslua_rt_table_set_field", Type::i64(), {tbl_bits, id_c, slot_c, val_bits});
}

MirValue MirModuleLowerer::lowerTableExpr(FunctionBuildState& st, const TableExpr& tbl_expr) {
    auto& b = st.builder;

    bool last_is_multiret = !tbl_expr.fields.empty() &&
                            !tbl_expr.fields.back().key &&
                            tbl_expr.fields.back().value &&
                            isCallOrVararg(*tbl_expr.fields.back().value);
    size_t non_multi_count = last_is_multiret ? (tbl_expr.fields.size() - 1) : tbl_expr.fields.size();

    struct EvalField {
        BValue* key_bits = nullptr;
        BValue* val_bits = nullptr;
        uint32_t intern_id = 0;
        int32_t slot = -1;
    };
    std::vector<EvalField> pre_eval(non_multi_count);
    uint32_t actual_hash_hint = 0;
    bool needs_tbl_ptr = false;
    bool needs_arr_ptr = false;
    for (size_t i = 0; i < non_multi_count; ++i) {
        const auto& f = tbl_expr.fields[i];
        if (!f.key) {
            needs_tbl_ptr = true;
            if (tbl_expr.array_count > LuaTable::kInlineArrayCap) {
                needs_arr_ptr = true;
            }
        } else if (!f.field_name.empty()) {
            uint32_t id = f.intern_id != 0 ? f.intern_id : rt_.internString(f.field_name);
            int32_t slot = f.slot_hint >= 0 ? f.slot_hint : rt_.getOrAssignFieldSlot(id);
            pre_eval[i].intern_id = id;
            pre_eval[i].slot = slot;
            if (slot >= 0 && slot < static_cast<int32_t>(LuaTable::kInlineSlots)) {
                needs_tbl_ptr = true;
            } else {
                ++actual_hash_hint;
            }
        } else {
            MirValue kmv = lowerExpr(st, *f.key);
            pre_eval[i].key_bits = emitBox(st, kmv);
            ++actual_hash_hint;
        }
        MirValue val_mv = lowerExpr(st, *f.value);
        pre_eval[i].val_bits = emitBox(st, val_mv);
    }

    BValue* arr_hint = b.build_iconst_i32(static_cast<int32_t>(tbl_expr.array_count));
    BValue* hash_hint = b.build_iconst_i32(static_cast<int32_t>(actual_hash_hint));
    BValue* tbl_bits = b.build_call("brasslua_rt_table_new", Type::i64(), {arr_hint, hash_hint});

    BValue* tbl_ptr = nullptr;
    BValue* arr_ptr = nullptr;
    if (needs_tbl_ptr) {
        BValue* payload_mask = b.build_iconst_i64(static_cast<int64_t>(kPayloadMask));
        tbl_ptr = b.build_and(tbl_bits, payload_mask);
        if (needs_arr_ptr) {
            arr_ptr = b.build_load(Type::ptr(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_data)));
        }
    }

    int64_t seq_idx = 1;
    for (size_t i = 0; i < non_multi_count; ++i) {
        const auto& f = tbl_expr.fields[i];
        BValue* val_bits = pre_eval[i].val_bits;
        if (!f.key) {
            int64_t cur_idx = seq_idx++;
            if (tbl_expr.array_count <= LuaTable::kInlineArrayCap &&
                cur_idx <= static_cast<int64_t>(LuaTable::kInlineArrayCap)) {
                int32_t off = static_cast<int32_t>(offsetof(LuaTable, inline_array)) +
                              static_cast<int32_t>((cur_idx - 1) * 8);
                b.build_store(Type::i64(), tbl_ptr, off, val_bits);
            } else if (cur_idx <= static_cast<int64_t>(tbl_expr.array_count) && arr_ptr) {
                int32_t off = static_cast<int32_t>((cur_idx - 1) * 8);
                b.build_store(Type::i64(), arr_ptr, off, val_bits);
            } else {
                BValue* idx_c = b.build_iconst_i64(cur_idx);
                tbl_bits = b.build_call("brasslua_rt_table_set_int", Type::i64(), {tbl_bits, idx_c, val_bits});
            }
        } else if (!f.field_name.empty()) {
            int32_t slot = pre_eval[i].slot;
            if (slot >= 0 && slot < static_cast<int32_t>(LuaTable::kInlineSlots)) {
                int32_t off = static_cast<int32_t>(offsetof(LuaTable, slots)) + slot * 8;
                b.build_store(Type::i64(), tbl_ptr, off, val_bits);
            } else {
                BValue* id_c = b.build_iconst_i32(static_cast<int32_t>(pre_eval[i].intern_id));
                BValue* slot_c = b.build_iconst_i32(static_cast<int32_t>(0xFFFFFFFFu));
                tbl_bits = b.build_call("brasslua_rt_table_set_field", Type::i64(), {tbl_bits, id_c, slot_c, val_bits});
            }
        } else {
            tbl_bits = b.build_call("brasslua_rt_table_set", Type::i64(), {tbl_bits, pre_eval[i].key_bits, val_bits});
        }
    }

    if (last_is_multiret) {
        (void)lowerMultiExpr(st, *tbl_expr.fields.back().value);
        BValue* start_c = b.build_iconst_i64(seq_idx);
        tbl_bits = b.build_call("brasslua_rt_table_append_multiret", Type::i64(), {tbl_bits, start_c});
    }

    return MirValue{tbl_bits, Rep::Boxed};
}

} // namespace brasslua
