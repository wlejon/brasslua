#include "runtime/runtime_internal.h"

#include <brass/gc/heap.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace brasslua {

namespace {

bool tryExtractArrayIndex(Value k, int64_t& idx_out) noexcept {
    if (k.isInlineInt()) {
        idx_out = k.asInt64();
        return true;
    }
    if (k.isBoxedInt64()) {
        idx_out = k.asInt64();
        return true;
    }
    if (k.isFloat()) {
        double d = k.asDouble();
        return tryConvertFloatToExactInt64(d, idx_out);
    }
    return false;
}

uint64_t normalizeKey(RuntimeState& rt, uint64_t key_bits) {
    Value k(key_bits);
    if (k.isFloat()) {
        double d = k.asDouble();
        if (std::isnan(d)) {
            throw LuaRuntimeError("table index is NaN");
        }
        int64_t iv = 0;
        if (tryConvertFloatToExactInt64(d, iv)) {
            return Value::fromInt64(iv).rawBits();
        }
        if (d == 0.0) {
            return Value::fromInt64(0).rawBits();
        }
    } else if (k.isString()) {
        auto* s = static_cast<const LuaString*>(k.asPointer());
        uint32_t id = rt.internString(std::string_view(s->data, s->length));
        return Value::fromString(rt.getLuaStringById(id)).rawBits();
    }
    return key_bits;
}

bool keysEqual(uint64_t k1, uint64_t k2) noexcept {
    if (k1 == k2) return true;
    return Value(k1) == Value(k2);
}

uint64_t rawTableGet(RuntimeState& rt, LuaTable* tbl, uint64_t key_bits) {
    if (!tbl) return kNilBits;
    Value k(key_bits);
    if (k.isNil()) return kNilBits;

    int64_t idx = 0;
    if (tryExtractArrayIndex(k, idx)) {
        if (idx >= 1 && static_cast<uint64_t>(idx) <= tbl->array_cap) {
            return tbl->array_data[idx - 1];
        }
        key_bits = Value::fromInt64(idx).rawBits();
    } else if (k.isString()) {
        auto* s = static_cast<const LuaString*>(k.asPointer());
        int32_t slot = rt.findFieldSlot(s->intern_id);
        if (slot >= 0 && slot < static_cast<int32_t>(LuaTable::kInlineSlots)) {
            return tbl->slots[slot];
        }
        key_bits = Value::fromString(rt.getLuaStringById(s->intern_id)).rawBits();
    } else {
        key_bits = normalizeKey(rt, key_bits);
    }

    for (uint32_t i = 0; i < tbl->hash_len; ++i) {
        if (keysEqual(tbl->hash_data[i].key, key_bits)) {
            return tbl->hash_data[i].val;
        }
    }
    return kNilBits;
}

void rawTableSet(RuntimeState& rt, LuaTable* tbl, uint64_t key_bits, uint64_t val_bits) {
    if (!tbl) return;
    Value k(key_bits);
    if (k.isNil()) {
        throw LuaRuntimeError("table index is nil");
    }

    int64_t idx = 0;
    if (tryExtractArrayIndex(k, idx)) {
        if (idx >= 1 && idx <= (1LL << 26)) {
            uint32_t u_idx = static_cast<uint32_t>(idx);
            if (u_idx > tbl->array_cap) {
                if (u_idx <= tbl->array_cap * 2 + 64 || u_idx <= 4096) {
                    rt.ensureTableArrayCapacity(tbl, u_idx);
                }
            }
            if (u_idx <= tbl->array_cap) {
                tbl->array_data[u_idx - 1] = val_bits;
                if (!Value(val_bits).isNil()) {
                    if (u_idx > tbl->array_len) {
                        tbl->array_len = u_idx;
                    }
                } else if (u_idx == tbl->array_len) {
                    while (tbl->array_len > 0 && Value(tbl->array_data[tbl->array_len - 1]).isNil()) {
                        --tbl->array_len;
                    }
                }
                rt.heap().remember(reinterpret_cast<uintptr_t>(tbl));
                return;
            }
        }
        key_bits = Value::fromInt64(idx).rawBits();
    } else if (k.isString()) {
        auto* s = static_cast<const LuaString*>(k.asPointer());
        uint32_t id = s->intern_id;
        int32_t slot = rt.findFieldSlot(id);
        if (slot < 0) {
            slot = rt.getOrAssignFieldSlot(id);
        }
        if (slot >= 0 && slot < static_cast<int32_t>(LuaTable::kInlineSlots)) {
            tbl->slots[slot] = val_bits;
            rt.heap().remember(reinterpret_cast<uintptr_t>(tbl));
            return;
        }
        key_bits = Value::fromString(rt.getLuaStringById(id)).rawBits();
    } else {
        key_bits = normalizeKey(rt, key_bits);
    }

    for (uint32_t i = 0; i < tbl->hash_len; ++i) {
        if (keysEqual(tbl->hash_data[i].key, key_bits)) {
            tbl->hash_data[i].val = val_bits;
            rt.heap().remember(reinterpret_cast<uintptr_t>(tbl));
            return;
        }
    }
    if (!Value(val_bits).isNil()) {
        for (uint32_t i = 0; i < tbl->hash_len; ++i) {
            if (Value(tbl->hash_data[i].val).isNil()) {
                tbl->hash_data[i].key = key_bits;
                tbl->hash_data[i].val = val_bits;
                rt.heap().remember(reinterpret_cast<uintptr_t>(tbl));
                return;
            }
        }
        rt.ensureTableHashCapacity(tbl, tbl->hash_len + 1);
        tbl->hash_data[tbl->hash_len] = LuaHashEntry{key_bits, val_bits};
        ++tbl->hash_len;
        rt.heap().remember(reinterpret_cast<uintptr_t>(tbl));
    }
}

} // namespace

bool tryConvertFloatToExactInt64(double d, int64_t& out) noexcept {
    if (!std::isfinite(d)) return false;
    constexpr double kMaxIntBound = 9223372036854775808.0;  // 2^63
    constexpr double kMinIntBound = -9223372036854775808.0; // -2^63
    if (d < kMinIntBound || d >= kMaxIntBound) return false;
    int64_t iv = static_cast<int64_t>(d);
    if (static_cast<double>(iv) != d) return false;
    out = iv;
    return true;
}

void recomputeMetatableFlags(RuntimeState& rt, LuaTable* tbl) {
    if (!tbl) return;
    if (!tbl->metatable) {
        tbl->flags = 0;
        return;
    }
    LuaTable* mt = tbl->metatable;
    uint32_t flags = 0;
    auto check = [&](std::string_view name, uint32_t bit) {
        uint32_t id = rt.internString(name);
        uint64_t v = rawTableGet(rt, mt, Value::fromString(rt.getLuaStringById(id)).rawBits());
        if (!Value(v).isNil()) {
            flags |= bit;
        }
    };
    check("__index", kMetaIndex);
    check("__newindex", kMetaNewIndex);
    check("__len", kMetaLen);
    check("__eq", kMetaEq);
    check("__lt", kMetaLt);
    check("__le", kMetaLe);
    check("__add", kMetaAdd);
    check("__sub", kMetaSub);
    check("__mul", kMetaMul);
    check("__div", kMetaDiv);
    check("__idiv", kMetaIDiv);
    check("__mod", kMetaMod);
    check("__pow", kMetaPow);
    check("__unm", kMetaUnm);
    check("__concat", kMetaConcat);
    check("__call", kMetaCall);
    check("__tostring", kMetaToString);
    check("__band", kMetaBand);
    check("__bor", kMetaBor);
    check("__bxor", kMetaBxor);
    check("__bnot", kMetaBnot);
    check("__shl", kMetaShl);
    check("__shr", kMetaShr);
    check("__close", kMetaClose);
    check("__gc", kMetaGc);

    uint32_t mode_id = rt.internString("__mode");
    Value mode_val(rawTableGet(rt, mt, Value::fromString(rt.getLuaStringById(mode_id)).rawBits()));
    if (mode_val.isString()) {
        std::string_view m = valueToStringView(rt, mode_val);
        if (m.find('k') != std::string_view::npos) flags |= kMetaWeakKeys;
        if (m.find('v') != std::string_view::npos) flags |= kMetaWeakValues;
    }

    tbl->flags = (tbl->flags & kMetaGcArmed) | flags;
    armTableFinalizerIfNeeded(rt, tbl);
}

Value lookupMetamethod(RuntimeState& rt, Value obj, std::string_view event) {
    LuaTable* mt = nullptr;
    if (obj.isTable()) {
        auto* tbl = static_cast<LuaTable*>(obj.asPointer());
        if (tbl) mt = tbl->metatable;
    } else if (obj.isFullUserdata()) {
        auto* ud = static_cast<LuaUserdata*>(obj.asPointer());
        if (ud) mt = ud->metatable;
    } else if (obj.isString()) {
        mt = getStringMetatable(rt);
    }
    if (!mt) return Value::nil();
    uint32_t id = rt.internString(event);
    return Value(rawTableGet(rt, mt, Value::fromString(rt.getLuaStringById(id)).rawBits()));
}

bool tryCoerceToDouble(Value v, double& out) noexcept {
    if (v.isFloat()) {
        out = v.asDouble();
        return true;
    }
    if (v.isInteger()) {
        out = static_cast<double>(v.asInt64());
        return true;
    }
    if (v.isString()) {
        auto* s = static_cast<const LuaString*>(v.asPointer());
        if (!s || s->length == 0) return false;
        char* end = nullptr;
        double d = std::strtod(s->data, &end);
        while (end && *end != '\0' && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            ++end;
        }
        if (end == s->data + s->length) {
            out = d;
            return true;
        }
    }
    return false;
}

bool tryCoerceToInt64(Value v, int64_t& out) noexcept {
    if (v.isInteger()) {
        out = v.asInt64();
        return true;
    }
    if (v.isFloat()) {
        return tryConvertFloatToExactInt64(v.asDouble(), out);
    }
    if (v.isString()) {
        auto* s = static_cast<const LuaString*>(v.asPointer());
        if (!s || s->length == 0) return false;
        char* end = nullptr;
        long long iv = std::strtoll(s->data, &end, 0);
        while (end && *end != '\0' && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            ++end;
        }
        if (end == s->data + s->length) {
            out = static_cast<int64_t>(iv);
            return true;
        }
        double d = 0.0;
        if (tryCoerceToDouble(v, d)) {
            return tryConvertFloatToExactInt64(d, out);
        }
    }
    return false;
}

std::string_view valueToStringView(RuntimeState& /*rt*/, Value v) {
    if (v.isString()) {
        auto* s = static_cast<const LuaString*>(v.asPointer());
        if (s) return std::string_view(s->data, s->length);
    }
    return {};
}

std::string valueToStdString(RuntimeState& rt, Value v) {
    if (v.isString()) {
        auto* s = static_cast<const LuaString*>(v.asPointer());
        return s ? std::string(s->data, s->length) : std::string();
    }
    if (v.isNil()) return "nil";
    if (v.isBool()) return v.asBool() ? "true" : "false";
    if (v.isInteger()) {
        return std::to_string(v.asInt64());
    }
    if (v.isFloat()) {
        double d = v.asDouble();
        if (std::isnan(d)) return "-nan";
        if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
        int64_t iv = 0;
        if (tryConvertFloatToExactInt64(d, iv) && std::fabs(d) < 1e14) {
            return std::to_string(iv);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.14g", d);
        return std::string(buf);
    }
    if (v.isTable() || v.isFullUserdata()) {
        Value mm = lookupMetamethod(rt, v, "__tostring");
        if (!mm.isNil()) {
            uint64_t res = brasslua_rt_call_1(mm.rawBits(), v.rawBits());
            return valueToStdString(rt, Value(res));
        }
        if (v.isFullUserdata() && rt.hostBridge().toString) {
            std::string hs = rt.hostBridge().toString(v.rawBits());
            if (!hs.empty()) return hs;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s: %p", v.isTable() ? "table" : "userdata", v.asPointer());
        return std::string(buf);
    }
    if (v.isCoroutine()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "thread: %p", v.asPointer());
        return std::string(buf);
    }
    if (v.isLightUserdata()) {
        if (v.payload() == 0) {
            if (rt.hostBridge().toString) {
                std::string hs = rt.hostBridge().toString(v.rawBits());
                if (!hs.empty()) return hs;
            }
            return "userdata: (nil)";
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "userdata: %p", v.asPointer());
        return std::string(buf);
    }
    if (v.isFunction()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "function: %p", v.asPointer());
        return std::string(buf);
    }
    return "nil";
}

Value makeNativeClosure(RuntimeState& rt, LuaNativeFn fn, uint32_t arity) {
    LuaClosure* cl = rt.allocClosure(fn, nullptr, 0, arity);
    return Value::fromFunction(cl);
}

void setTableFieldStr(RuntimeState& rt, LuaTable* tbl, std::string_view key, Value val) {
    uint32_t id = rt.internString(key);
    int32_t slot = rt.findFieldSlot(id);
    brasslua_rt_table_set_field(
        Value::fromTable(tbl).rawBits(),
        id,
        slot >= 0 ? static_cast<uint32_t>(slot) : 0xFFFFFFFFu,
        val.rawBits());
}

uint64_t returnMulti(RuntimeState& rt, const std::vector<uint64_t>& vals) {
    rt.setMultiRet(vals.data(), vals.size());
    return vals.empty() ? kNilBits : vals[0];
}

} // namespace brasslua

using namespace brasslua;

extern "C" {

uint64_t brasslua_rt_box_int64(int64_t v) {
    if (Value::fitsInlineInt(v)) {
        return Value::fromInlineInt(v).rawBits();
    }
    auto& rt = RuntimeState::current();
    LuaBoxedInt64* box = rt.allocBoxedInt64(v);
    return Value::fromBoxedInt64Ptr(box).rawBits();
}

uint64_t brasslua_rt_table_new(uint32_t array_hint, uint32_t hash_hint) {
    auto& rt = RuntimeState::current();
    LuaTable* tbl = rt.allocTable(array_hint, hash_hint);
    return Value::fromTable(tbl).rawBits();
}

uint64_t brasslua_rt_rawget(uint64_t table, uint64_t key) {
    Value tv(table);
    if (!tv.isTable()) return kNilBits;
    auto& rt = RuntimeState::current();
    return rawTableGet(rt, static_cast<LuaTable*>(tv.asPointer()), key);
}

uint64_t brasslua_rt_rawset(uint64_t table, uint64_t key, uint64_t val) {
    Value tv(table);
    if (!tv.isTable()) return table;
    auto& rt = RuntimeState::current();
    rawTableSet(rt, static_cast<LuaTable*>(tv.asPointer()), key, val);
    return table;
}

uint64_t brasslua_rt_table_get(uint64_t table, uint64_t key) {
    auto& rt = RuntimeState::current();
    Value cur(table);
    for (int depth = 0; depth < 32; ++depth) {
        if (rt.hostBridge().isHostObject && rt.hostBridge().isHostObject(cur.rawBits())) {
            if (Value(key).isString() && rt.hostBridge().getProperty) {
                return rt.hostBridge().getProperty(cur.rawBits(), valueToStringView(rt, Value(key)));
            }
            if (rt.hostBridge().getIndex) {
                return rt.hostBridge().getIndex(cur.rawBits(), key);
            }
        }
        if (cur.isTable()) {
            auto* tbl = static_cast<LuaTable*>(cur.asPointer());
            uint64_t v = rawTableGet(rt, tbl, key);
            if (!Value(v).isNil() || (tbl->flags & kMetaIndex) == 0) {
                return v;
            }
            Value idx_mm = lookupMetamethod(rt, cur, "__index");
            if (idx_mm.isNil()) return kNilBits;
            if (idx_mm.isFunction()) {
                return brasslua_rt_call_2(idx_mm.rawBits(), cur.rawBits(), key);
            }
            cur = idx_mm;
            continue;
        }
        Value idx_mm = lookupMetamethod(rt, cur, "__index");
        if (idx_mm.isNil()) {
            throw LuaRuntimeError("attempt to index a non-table value");
        }
        if (idx_mm.isFunction()) {
            return brasslua_rt_call_2(idx_mm.rawBits(), cur.rawBits(), key);
        }
        cur = idx_mm;
    }
    return kNilBits;
}

void brasslua_rt_table_set(uint64_t table, uint64_t key, uint64_t val) {
    auto& rt = RuntimeState::current();
    Value cur(table);
    for (int depth = 0; depth < 32; ++depth) {
        if (rt.hostBridge().isHostObject && rt.hostBridge().isHostObject(cur.rawBits())) {
            if (Value(key).isString() && rt.hostBridge().setProperty) {
                rt.hostBridge().setProperty(cur.rawBits(), valueToStringView(rt, Value(key)), val);
                return;
            }
            if (rt.hostBridge().setIndex) {
                rt.hostBridge().setIndex(cur.rawBits(), key, val);
                return;
            }
        }
        if (cur.isTable()) {
            auto* tbl = static_cast<LuaTable*>(cur.asPointer());
            if ((tbl->flags & kMetaNewIndex) != 0) {
                uint64_t existing = rawTableGet(rt, tbl, key);
                if (Value(existing).isNil()) {
                    Value ni_mm = lookupMetamethod(rt, cur, "__newindex");
                    if (!ni_mm.isNil()) {
                        if (ni_mm.isFunction()) {
                            brasslua_rt_call_3(ni_mm.rawBits(), cur.rawBits(), key, val);
                            return;
                        }
                        cur = ni_mm;
                        continue;
                    }
                }
            }
            rawTableSet(rt, tbl, key, val);
            return;
        }
        Value ni_mm = lookupMetamethod(rt, cur, "__newindex");
        if (!ni_mm.isNil()) {
            if (ni_mm.isFunction()) {
                brasslua_rt_call_3(ni_mm.rawBits(), cur.rawBits(), key, val);
                return;
            }
            cur = ni_mm;
            continue;
        }
        throw LuaRuntimeError("attempt to index a non-table value");
    }
}

uint64_t brasslua_rt_table_get_int(uint64_t table, int64_t idx) {
    Value tv(table);
    if (tv.isTable()) {
        auto* tbl = static_cast<LuaTable*>(tv.asPointer());
        if (idx >= 1 && static_cast<uint64_t>(idx) <= tbl->array_cap) {
            uint64_t v = tbl->array_data[idx - 1];
            if (!Value(v).isNil() || (tbl->flags & kMetaIndex) == 0) {
                return v;
            }
        } else if ((tbl->flags & kMetaIndex) == 0) {
            uint64_t k = Value::fromInt64(idx).rawBits();
            for (uint32_t i = 0; i < tbl->hash_len; ++i) {
                if (keysEqual(tbl->hash_data[i].key, k)) {
                    return tbl->hash_data[i].val;
                }
            }
            return kNilBits;
        }
    }
    return brasslua_rt_table_get(table, Value::fromInt64(idx).rawBits());
}

void brasslua_rt_table_set_int(uint64_t table, int64_t idx, uint64_t val) {
    Value tv(table);
    if (tv.isTable()) {
        auto* tbl = static_cast<LuaTable*>(tv.asPointer());
        if ((tbl->flags & kMetaNewIndex) == 0 && idx >= 1 && idx <= (1LL << 26)) {
            uint32_t u_idx = static_cast<uint32_t>(idx);
            auto& rt = RuntimeState::current();
            if (u_idx > tbl->array_cap) {
                rt.ensureTableArrayCapacity(tbl, u_idx);
            }
            tbl->array_data[u_idx - 1] = val;
            if (!Value(val).isNil()) {
                if (u_idx > tbl->array_len) {
                    tbl->array_len = u_idx;
                }
            } else if (u_idx == tbl->array_len) {
                while (tbl->array_len > 0 && Value(tbl->array_data[tbl->array_len - 1]).isNil()) {
                    --tbl->array_len;
                }
            }
            rt.heap().remember(reinterpret_cast<uintptr_t>(tbl));
            return;
        }
    }
    brasslua_rt_table_set(table, Value::fromInt64(idx).rawBits(), val);
}

uint64_t brasslua_rt_table_get_field(uint64_t table, uint32_t intern_id, uint32_t slot_hint) {
    Value tv(table);
    if (tv.isTable()) {
        auto* tbl = static_cast<LuaTable*>(tv.asPointer());
        if (slot_hint < LuaTable::kInlineSlots) {
            uint64_t v = tbl->slots[slot_hint];
            if (!Value(v).isNil() || (tbl->flags & kMetaIndex) == 0) {
                return v;
            }
        }
    }
    auto& rt = RuntimeState::current();
    LuaString* s = rt.getLuaStringById(intern_id);
    if (!s) return kNilBits;
    return brasslua_rt_table_get(table, Value::fromString(s).rawBits());
}

void brasslua_rt_table_set_field(uint64_t table, uint32_t intern_id, uint32_t slot_hint, uint64_t val) {
    Value tv(table);
    if (tv.isTable()) {
        auto* tbl = static_cast<LuaTable*>(tv.asPointer());
        if ((tbl->flags & kMetaNewIndex) == 0 && slot_hint < LuaTable::kInlineSlots) {
            tbl->slots[slot_hint] = val;
            RuntimeState::current().heap().remember(reinterpret_cast<uintptr_t>(tbl));
            return;
        }
    }
    auto& rt = RuntimeState::current();
    LuaString* s = rt.getLuaStringById(intern_id);
    if (!s) return;
    brasslua_rt_table_set(table, Value::fromString(s).rawBits(), val);
}

int64_t brasslua_rt_table_len(uint64_t table) {
    Value tv(table);
    if (!tv.isTable()) return 0;
    auto* tbl = static_cast<LuaTable*>(tv.asPointer());
    if ((tbl->flags & kMetaLen) != 0) {
        auto& rt = RuntimeState::current();
        Value mm = lookupMetamethod(rt, tv, "__len");
        if (!mm.isNil()) {
            uint64_t res = brasslua_rt_call_1(mm.rawBits(), table);
            return Value(res).asInt64();
        }
    }
    uint32_t len = 0;
    while (len < tbl->array_cap && !Value(tbl->array_data[len]).isNil()) {
        ++len;
    }
    return static_cast<int64_t>(len);
}

uint64_t brasslua_rt_next(uint64_t table, uint64_t key, uint64_t* out_val) {
    if (out_val) *out_val = kNilBits;
    Value tv(table);
    if (!tv.isTable()) return kNilBits;
    auto* tbl = static_cast<LuaTable*>(tv.asPointer());
    auto& rt = RuntimeState::current();

    auto nextFromArray = [&](uint32_t start_idx) -> uint64_t {
        for (uint32_t i = start_idx; i < tbl->array_cap; ++i) {
            uint64_t v = tbl->array_data[i];
            if (!Value(v).isNil()) {
                if (out_val) *out_val = v;
                return Value::fromInt64(static_cast<int64_t>(i + 1)).rawBits();
            }
        }
        return kNilBits;
    };

    auto nextFromSlots = [&](uint32_t start_slot) -> uint64_t {
        for (uint32_t s = start_slot; s < LuaTable::kInlineSlots; ++s) {
            uint64_t v = tbl->slots[s];
            if (!Value(v).isNil()) {
                uint32_t id = getSlotInternId(rt, s);
                LuaString* lstr = rt.getLuaStringById(id);
                if (lstr) {
                    if (out_val) *out_val = v;
                    return Value::fromString(lstr).rawBits();
                }
            }
        }
        return kNilBits;
    };

    auto nextFromHash = [&](uint32_t start_h) -> uint64_t {
        for (uint32_t h = start_h; h < tbl->hash_len; ++h) {
            const auto& entry = tbl->hash_data[h];
            if (!Value(entry.key).isNil() && !Value(entry.val).isNil()) {
                if (out_val) *out_val = entry.val;
                return entry.key;
            }
        }
        return kNilBits;
    };

    Value kv(key);
    if (kv.isNil()) {
        uint64_t r = nextFromArray(0);
        if (!Value(r).isNil()) return r;
        r = nextFromSlots(0);
        if (!Value(r).isNil()) return r;
        return nextFromHash(0);
    }

    int64_t idx = 0;
    if (tryExtractArrayIndex(kv, idx) && idx >= 1 && static_cast<uint64_t>(idx) <= tbl->array_cap) {
        uint64_t r = nextFromArray(static_cast<uint32_t>(idx));
        if (!Value(r).isNil()) return r;
        r = nextFromSlots(0);
        if (!Value(r).isNil()) return r;
        return nextFromHash(0);
    }

    if (kv.isString()) {
        auto* s = static_cast<const LuaString*>(kv.asPointer());
        int32_t slot = rt.findFieldSlot(s->intern_id);
        if (slot >= 0 && slot < static_cast<int32_t>(LuaTable::kInlineSlots)) {
            uint64_t r = nextFromSlots(static_cast<uint32_t>(slot + 1));
            if (!Value(r).isNil()) return r;
            return nextFromHash(0);
        }
    }

    uint64_t norm_k = normalizeKey(rt, key);
    for (uint32_t h = 0; h < tbl->hash_len; ++h) {
        if (keysEqual(tbl->hash_data[h].key, norm_k)) {
            return nextFromHash(h + 1);
        }
    }
    return kNilBits;
}

} // extern "C"
