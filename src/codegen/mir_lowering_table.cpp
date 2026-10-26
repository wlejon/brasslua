#include "codegen/mir_lowering.h"
#include "runtime/runtime_internal.h"

namespace brasslua {

using brass::BasicBlock;
using brass::Type;

namespace {

// A boxed key the hints bet is an inline integer: ends the block with the
// site, positions at its fast side and gives the key's integer there, the
// slow side `slow_bb` (null when the key is not such a bet).
BValue* emitIntKeyBet(MirModuleLowerer& lowerer, FunctionBuildState& st, const Expr& key, MirValue key_mv,
                      BValue* key_bits, BasicBlock*& slow_bb) {
    slow_bb = nullptr;
    if (key_mv.rep != Rep::Boxed || key.hint != InferredType::Int64) return nullptr;
    auto& b = st.builder;
    BValue* is_int = b.build_eq(b.build_lshr(key_bits, b.build_iconst_i64(48)),
                                b.build_iconst_i64(static_cast<int64_t>(Tag::Int47)));
    BasicBlock* fast_bb = st.newBlock("key_int");
    slow_bb = st.newColdBlock("key_any");
    lowerer.emitSite(st, is_int, fast_bb, slow_bb, SpecKind::IntKey);
    b.position_at_end(fast_bb);
    BValue* c16 = b.build_iconst_i64(16);
    return b.build_ashr(b.build_shl(key_bits, c16), c16);
}

} // namespace

MirValue MirModuleLowerer::lowerIndexLoad(FunctionBuildState& st, const IndexExpr& idx_expr) {
    auto& b = st.builder;
    MirValue tbl_mv = lowerExpr(st, *idx_expr.table);
    BValue* tbl_bits = emitBox(st, tbl_mv);
    MirValue key_mv = lowerExpr(st, *idx_expr.index);

    // Boxed only where the generic code reads it: boxing a big integer is a
    // call nothing would remove.
    BasicBlock* key_any_bb = nullptr;
    BValue* key_bits = key_mv.rep == Rep::Boxed ? key_mv.val : nullptr;
    BValue* bet_idx = emitIntKeyBet(*this, st, *idx_expr.index, key_mv, key_bits, key_any_bb);
    if (bet_idx || key_mv.rep == Rep::Int64 || idx_expr.index->inferred_type == InferredType::Int64) {
        BValue* idx_i64 = bet_idx ? bet_idx : emitUnboxInt64(st, key_mv);
        // Fast inline array load when:
        // 1. (tbl_bits >> 48) == Tag::Object (0xFFF1)
        // 2. tbl->flags == 0 (regular table without __index metamethod)
        // 3. (uint64_t)(idx - 1) < (uint64_t)tbl->array_cap
        BValue* tag = b.build_lshr(tbl_bits, b.build_iconst_i64(48));
        BValue* is_tbl = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Object)));

        BasicBlock* check_bounds_bb = st.newBlock("arr_ld_bounds");
        BasicBlock* fast_ld_bb = st.newBlock("arr_ld_fast");
        BasicBlock* slow_ld_bb = st.newColdBlock("arr_ld_slow");
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
        if (!key_any_bb) return MirValue{merge_bb->param(0), Rep::Boxed};
        BasicBlock* done_bb = st.newBlock("key_merge", {Type::i64()});
        b.build_br(done_bb, {merge_bb->param(0)});
        b.position_at_end(key_any_bb);
        b.build_br(done_bb, {b.build_call("brasslua_rt_table_get", Type::i64(), {tbl_bits, key_bits})});
        b.position_at_end(done_bb);
        return MirValue{done_bb->param(0), Rep::Boxed};
    }

    if (!key_bits) key_bits = emitBox(st, key_mv);
    BValue* res = b.build_call("brasslua_rt_table_get", Type::i64(), {tbl_bits, key_bits});
    return MirValue{res, Rep::Boxed};
}

void MirModuleLowerer::lowerIndexStore(FunctionBuildState& st, const IndexExpr& idx_expr, MirValue rhs) {
    auto& b = st.builder;
    MirValue tbl_mv = lowerExpr(st, *idx_expr.table);
    BValue* tbl_bits = emitBox(st, tbl_mv);
    MirValue key_mv = lowerExpr(st, *idx_expr.index);

    BValue* val_bits = emitBox(st, rhs);
    BasicBlock* key_any_bb = nullptr;
    BValue* key_bits = key_mv.rep == Rep::Boxed ? key_mv.val : nullptr;
    BValue* bet_idx = emitIntKeyBet(*this, st, *idx_expr.index, key_mv, key_bits, key_any_bb);
    if (bet_idx || key_mv.rep == Rep::Int64 || idx_expr.index->inferred_type == InferredType::Int64) {
        BValue* idx_i64 = bet_idx ? bet_idx : emitUnboxInt64(st, key_mv);
        BValue* tag = b.build_lshr(tbl_bits, b.build_iconst_i64(48));
        BValue* is_tbl = b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Object)));

        BasicBlock* check_bounds_bb = st.newBlock("arr_st_bounds");
        BasicBlock* fast_st_bb = st.newBlock("arr_st_fast");
        BasicBlock* slow_st_bb = st.newColdBlock("arr_st_slow");
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
        if (!key_any_bb) return;
        BasicBlock* done_bb = st.newBlock("key_merge");
        b.build_br(done_bb);
        b.position_at_end(key_any_bb);
        b.build_call("brasslua_rt_table_set", Type::i64(), {tbl_bits, key_bits, val_bits});
        b.build_br(done_bb);
        b.position_at_end(done_bb);
        return;
    }

    if (!key_bits) key_bits = emitBox(st, key_mv);
    b.build_call("brasslua_rt_table_set", Type::i64(), {tbl_bits, key_bits, val_bits});
}

BValue* MirModuleLowerer::emitStringConst(FunctionBuildState& st, uint32_t intern_id) {
    // Interned strings are pinned and live as long as the runtime.
    LuaString* s = rt_.getLuaStringById(intern_id);
    return st.builder.build_iconst_i64(static_cast<int64_t>(Value::fromString(s).rawBits()));
}

MirValue MirModuleLowerer::lowerFieldLoad(FunctionBuildState& st, const FieldExpr& fld_expr) {
    MirValue tbl_mv = lowerExpr(st, *fld_expr.table);
    BValue* tbl_bits = emitBox(st, tbl_mv);
    uint32_t id = fld_expr.intern_id != 0 ? fld_expr.intern_id : rt_.internString(fld_expr.field);
    return MirValue{emitFieldGet(st, tbl_bits, id), Rep::Boxed};
}

void MirModuleLowerer::lowerFieldStore(FunctionBuildState& st, const FieldExpr& fld_expr, MirValue rhs) {
    MirValue tbl_mv = lowerExpr(st, *fld_expr.table);
    BValue* tbl_bits = emitBox(st, tbl_mv);
    uint32_t id = fld_expr.intern_id != 0 ? fld_expr.intern_id : rt_.internString(fld_expr.field);
    emitFieldSet(st, tbl_bits, id, emitBox(st, rhs));
}

namespace {

static_assert(kHostKindTable == 0, "the table check reads host_bits == 0 as the header's top byte");

// The runtime's linear probing for a constant key (tbl_ptr is the LuaTable*,
// as an i64): branches to found_bb(entry address) when the hash part holds the
// key, whatever its value, and to absent_bb at the first empty entry. The
// load factor is at most 1/2, so an empty entry ends every search (a table
// with no hash part probes kEmptyHashPart's one).
void emitHashFind(FunctionBuildState& st, BValue* tbl_ptr, BValue* key_c, uint32_t hash,
                  BasicBlock* found_bb, BasicBlock* absent_bb) {
    auto& b = st.builder;
    BValue* hd = b.build_load(Type::i64(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, hash_data)));
    BValue* mask = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, hash_mask)));
    BasicBlock* loop_bb = st.newBlock("hfind_loop", {Type::i32()});
    BasicBlock* other_bb = st.newBlock("hfind_other");
    BasicBlock* next_bb = st.newBlock("hfind_next");
    b.build_br(loop_bb, {b.build_and(mask, b.build_iconst_i32(static_cast<int32_t>(hash)))});

    b.position_at_end(loop_bb);
    BValue* idx = loop_bb->param(0);
    BValue* entry = b.build_add(hd, b.build_shl(b.build_zext_i64(idx), b.build_iconst_i64(4)));
    BValue* key = b.build_load(Type::i64(), entry, 0);
    b.build_br_if(b.build_eq(key, key_c), found_bb, {entry}, other_bb, {});

    b.position_at_end(other_bb);
    b.build_br_if(b.build_eq(key, b.build_iconst_i64(static_cast<int64_t>(kNilBits))), absent_bb, next_bb);

    b.position_at_end(next_bb);
    b.build_br(loop_bb, {b.build_and(b.build_add(idx, b.build_iconst_i32(1)), mask)});
}

// Leaves the builder in a block reached only when obj_bits is Object-tagged
// (else no_bb), and returns its pointer (an i64). A coroutine or userdata
// reads as a table with no array or hash part and kMetaNonTable in its flags
// (lua_table.h), so what follows finds nothing in it and goes to the runtime.
BValue* emitTableCheck(FunctionBuildState& st, BValue* obj_bits, BasicBlock* no_bb) {
    auto& b = st.builder;
    BasicBlock* yes_bb = st.newBlock("tbl_chk_ok");
    BValue* tag = b.build_lshr(obj_bits, b.build_iconst_i64(48));
    b.build_br_if(b.build_eq(tag, b.build_iconst_i64(static_cast<int64_t>(Tag::Object))), yes_bb, no_bb);
    b.position_at_end(yes_bb);
    return b.build_and(obj_bits, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
}

} // namespace

void MirModuleLowerer::emitFieldProbe(FunctionBuildState& st, BValue* obj_bits, uint32_t intern_id,
                                      bool through_index, bool nil_is_hit,
                                      BasicBlock* hit_bb, BasicBlock* miss_bb) {
    auto& b = st.builder;
    LuaString* s = rt_.getLuaStringById(intern_id);
    const uint32_t hash = s->hash;
    BValue* key_c = b.build_iconst_i64(static_cast<int64_t>(Value::fromString(s).rawBits()));
    BValue* nil_c = b.build_iconst_i64(static_cast<int64_t>(kNilBits));

    BValue* tbl_ptr = emitTableCheck(st, obj_bits, miss_bb);
    BasicBlock* found_bb = st.newBlock("fld_found", {Type::i64()});
    BasicBlock* absent_bb = st.newBlock("fld_absent");
    emitHashFind(st, tbl_ptr, key_c, hash, found_bb, absent_bb);

    // A key holding nil is as absent as one not there.
    b.position_at_end(found_bb);
    BValue* val = b.build_load(Type::i64(), found_bb->param(0), 8);
    b.build_br_if(b.build_ne(val, nil_c), hit_bb, {val}, absent_bb, {});

    // Absent: nil from a table with no __index, a table __index's value, or
    // the runtime's answer (another __index, a coroutine or userdata).
    b.position_at_end(absent_bb);
    BValue* flags = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, flags)));
    BValue* zero32 = b.build_iconst_i32(0);
    BValue* has_index = b.build_ne(b.build_and(flags, b.build_iconst_i32(static_cast<int32_t>(kMetaIndex))), zero32);
    BasicBlock* index_bb = through_index ? st.newBlock("fld_index") : miss_bb;
    if (nil_is_hit) {
        BasicBlock* plain_bb = st.newBlock("fld_absent_plain");
        b.build_br_if(has_index, index_bb, plain_bb);
        b.position_at_end(plain_bb);
        BValue* non_table = b.build_ne(b.build_and(flags, b.build_iconst_i32(static_cast<int32_t>(kMetaNonTable))),
                                       zero32);
        b.build_br_if(non_table, miss_bb, {}, hit_bb, {nil_c});
    } else {
        b.build_br_if(has_index, index_bb, miss_bb);
    }
    if (!through_index) return;

    // One level of a table __index, read from the metatable's cache.
    b.position_at_end(index_bb);
    BValue* mt = b.build_load(Type::i64(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, metatable)));
    BValue* mt_flags = b.build_load(Type::i32(), mt, static_cast<int32_t>(offsetof(LuaTable, cached_mt_flags)));
    BValue* valid = b.build_ne(b.build_and(mt_flags, b.build_iconst_i32(static_cast<int32_t>(kMetaCacheValid))),
                               b.build_iconst_i32(0));
    BasicBlock* cached_bb = st.newBlock("fld_index_cached");
    b.build_br_if(valid, cached_bb, miss_bb);
    b.position_at_end(cached_bb);
    BValue* index_mm = b.build_load(Type::i64(), mt, static_cast<int32_t>(offsetof(LuaTable, cached_index_mm)));
    BValue* idx_ptr = emitTableCheck(st, index_mm, miss_bb);
    BasicBlock* idx_found_bb = st.newBlock("fld_index_found", {Type::i64()});
    emitHashFind(st, idx_ptr, key_c, hash, idx_found_bb, miss_bb);
    b.position_at_end(idx_found_bb);
    BValue* idx_val = b.build_load(Type::i64(), idx_found_bb->param(0), 8);
    b.build_br_if(b.build_ne(idx_val, nil_c), hit_bb, {idx_val}, miss_bb, {});
}

BValue* MirModuleLowerer::emitFieldGet(FunctionBuildState& st, BValue* obj_bits, uint32_t intern_id) {
    auto& b = st.builder;
    BasicBlock* slow_bb = st.newColdBlock("fld_ld_slow");
    BasicBlock* merge_bb = st.newBlock("fld_ld_merge", {Type::i64()});
    emitFieldProbe(st, obj_bits, intern_id, /*through_index=*/true, /*nil_is_hit=*/true, merge_bb, slow_bb);
    b.position_at_end(slow_bb);
    BValue* slow_val = b.build_call("brasslua_rt_table_get_field", Type::i64(), {obj_bits, emitStringConst(st, intern_id)});
    b.build_br(merge_bb, {slow_val});
    b.position_at_end(merge_bb);
    return merge_bb->param(0);
}

void MirModuleLowerer::emitFieldSet(FunctionBuildState& st, BValue* obj_bits, uint32_t intern_id, BValue* val_bits) {
    auto& b = st.builder;
    LuaString* s = rt_.getLuaStringById(intern_id);
    BasicBlock* slow_bb = st.newColdBlock("fld_st_slow");
    BasicBlock* merge_bb = st.newBlock("fld_st_merge");
    // A "__" key may be a metamethod of a table used as a metatable: its
    // store goes through the runtime, which drops the metatable's cache.
    const bool may_be_metamethod = s->length >= 2 && s->data[0] == '_' && s->data[1] == '_';
    if (!may_be_metamethod) {
        BValue* key_c = b.build_iconst_i64(static_cast<int64_t>(Value::fromString(s).rawBits()));
        BValue* nil_c = b.build_iconst_i64(static_cast<int64_t>(kNilBits));
        BValue* tbl_ptr = emitTableCheck(st, obj_bits, slow_bb);
        BasicBlock* found_bb = st.newBlock("fld_st_found", {Type::i64()});
        emitHashFind(st, tbl_ptr, key_c, s->hash, found_bb, slow_bb);
        // The key is there: a store, unless it holds nil and __newindex wants the absence.
        b.position_at_end(found_bb);
        BValue* entry = found_bb->param(0);
        BValue* old_val = b.build_load(Type::i64(), entry, 8);
        BValue* flags = b.build_load(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, flags)));
        BValue* no_newindex = b.build_eq(b.build_and(flags, b.build_iconst_i32(static_cast<int32_t>(kMetaNewIndex))),
                                         b.build_iconst_i32(0));
        BasicBlock* fast_bb = st.newBlock("fld_st_fast");
        b.build_br_if(b.build_or(b.build_ne(old_val, nil_c), no_newindex), fast_bb, slow_bb);
        b.position_at_end(fast_bb);
        b.build_store(Type::i64(), entry, 8, val_bits);
        b.build_br(merge_bb);
    } else {
        b.build_br(slow_bb);
    }
    b.position_at_end(slow_bb);
    b.build_call("brasslua_rt_table_set_field", Type::i64(), {obj_bits, emitStringConst(st, intern_id), val_bits});
    b.build_br(merge_bb);
    b.position_at_end(merge_bb);
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
        int32_t hash_pos = -1; // its entry in the inline hash part, placed here
    };
    std::vector<EvalField> pre_eval(non_multi_count);
    uint32_t hash_hint = 0;
    bool computed_keys = false;
    for (size_t i = 0; i < non_multi_count; ++i) {
        const auto& f = tbl_expr.fields[i];
        if (!f.key) {
        } else if (!f.field_name.empty()) {
            pre_eval[i].intern_id = f.intern_id != 0 ? f.intern_id : rt_.internString(f.field_name);
            ++hash_hint;
        } else {
            MirValue kmv = lowerExpr(st, *f.key);
            pre_eval[i].key_bits = emitBox(st, kmv);
            computed_keys = true;
            ++hash_hint;
        }
        MirValue val_mv = lowerExpr(st, *f.value);
        pre_eval[i].val_bits = emitBox(st, val_mv);
    }

    // allocTable lays the parts out inline exactly so: the array part's
    // array_count words, then a hash part of tableHashCapFor(hash_hint)
    // entries. Constant string keys are placed in it here, by the runtime's
    // own linear probing, unless a computed key could collide with one.
    const uint32_t array_count = static_cast<uint32_t>(tbl_expr.array_count);
    const bool inline_array = array_count <= LuaTable::kMaxInlineArray;
    uint32_t hash_cap = tableHashCapFor(hash_hint);
    const bool place_keys = hash_cap > 0 && hash_cap <= LuaTable::kMaxInlineHash && !computed_keys;
    if (place_keys) {
        // Room enough (up to 4x) that every key sits in its home entry, where
        // a field access's first probe looks.
        auto collision_free = [&](uint32_t cap) {
            std::vector<uint64_t> home(cap, kNilBits);
            for (size_t i = 0; i < non_multi_count; ++i) {
                if (!tbl_expr.fields[i].key || tbl_expr.fields[i].field_name.empty()) continue;
                LuaString* s = rt_.getLuaStringById(pre_eval[i].intern_id);
                const uint64_t key = Value::fromString(s).rawBits();
                uint64_t& h = home[s->hash & (cap - 1)];
                if (h != kNilBits && h != key) return false;
                h = key;
            }
            return true;
        };
        for (uint32_t cap = hash_cap; cap <= hash_cap * 4 && cap <= LuaTable::kMaxInlineHash; cap *= 2) {
            if (collision_free(cap)) {
                hash_cap = cap;
                hash_hint = cap / 2; // tableHashCapFor(cap / 2) == cap
                break;
            }
        }
    }
    const int32_t hash_off = static_cast<int32_t>(sizeof(LuaTable) + (inline_array ? array_count * 8 : 0));
    std::vector<uint64_t> placed(place_keys ? hash_cap : 0, kNilBits);
    uint32_t placed_count = 0;
    if (place_keys) {
        for (size_t i = 0; i < non_multi_count; ++i) {
            if (!tbl_expr.fields[i].key || tbl_expr.fields[i].field_name.empty()) continue;
            LuaString* s = rt_.getLuaStringById(pre_eval[i].intern_id);
            const uint64_t key = Value::fromString(s).rawBits();
            uint32_t h = s->hash & (hash_cap - 1);
            while (placed[h] != kNilBits && placed[h] != key) h = (h + 1) & (hash_cap - 1);
            if (placed[h] == kNilBits) ++placed_count;
            placed[h] = key;
            pre_eval[i].hash_pos = static_cast<int32_t>(h);
        }
    }

    BValue* arr_hint = b.build_iconst_i32(static_cast<int32_t>(array_count));
    BValue* hash_hint_c = b.build_iconst_i32(static_cast<int32_t>(hash_hint));
    BValue* tbl_bits = b.build_call("brasslua_rt_table_new", Type::i64(), {arr_hint, hash_hint_c});
    BValue* tbl_ptr = b.build_and(tbl_bits, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    BValue* arr_ptr = nullptr;
    if (array_count > 0 && !inline_array) {
        arr_ptr = b.build_load(Type::i64(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_data)));
    }
    if (placed_count > 0) {
        b.build_store(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, hash_len)),
                      b.build_iconst_i32(static_cast<int32_t>(placed_count)));
    }

    int64_t seq_idx = 1;
    for (size_t i = 0; i < non_multi_count; ++i) {
        const auto& f = tbl_expr.fields[i];
        BValue* val_bits = pre_eval[i].val_bits;
        if (!f.key) {
            int64_t cur_idx = seq_idx++;
            if (cur_idx <= static_cast<int64_t>(array_count)) {
                const int32_t off = static_cast<int32_t>((cur_idx - 1) * 8);
                if (inline_array) {
                    b.build_store(Type::i64(), tbl_ptr, static_cast<int32_t>(sizeof(LuaTable)) + off, val_bits);
                } else {
                    b.build_store(Type::i64(), arr_ptr, off, val_bits);
                }
            } else {
                BValue* idx_c = b.build_iconst_i64(cur_idx);
                tbl_bits = b.build_call("brasslua_rt_table_set_int", Type::i64(), {tbl_bits, idx_c, val_bits});
            }
        } else if (pre_eval[i].hash_pos >= 0) {
            const int32_t off = hash_off + pre_eval[i].hash_pos * static_cast<int32_t>(sizeof(LuaHashEntry));
            b.build_store(Type::i64(), tbl_ptr, off, emitStringConst(st, pre_eval[i].intern_id));
            b.build_store(Type::i64(), tbl_ptr, off + 8, val_bits);
        } else if (!f.field_name.empty()) {
            tbl_bits = b.build_call("brasslua_rt_table_set_field", Type::i64(),
                                    {tbl_bits, emitStringConst(st, pre_eval[i].intern_id), val_bits});
        } else {
            tbl_bits = b.build_call("brasslua_rt_table_set", Type::i64(), {tbl_bits, pre_eval[i].key_bits, val_bits});
        }
    }
    if (array_count > 0) {
        // The border hint (rawTableLength checks it before trusting it).
        b.build_store(Type::i32(), tbl_ptr, static_cast<int32_t>(offsetof(LuaTable, array_len)),
                      b.build_iconst_i32(static_cast<int32_t>(array_count)));
    }

    if (last_is_multiret) {
        (void)lowerMultiExpr(st, *tbl_expr.fields.back().value);
        BValue* start_c = b.build_iconst_i64(seq_idx);
        tbl_bits = b.build_call("brasslua_rt_table_append_multiret", Type::i64(), {tbl_bits, start_c});
    }

    return MirValue{tbl_bits, Rep::Boxed};
}

} // namespace brasslua
