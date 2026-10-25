#pragma once

#include "brasslua/value.h"
#include <cstddef>
#include <cstdint>

namespace brasslua {

struct alignas(8) LuaString {
    uint32_t hash = 0;
    uint32_t length = 0;
    uint32_t intern_id = 0;
    uint32_t reserved = 0;
    char data[]; // NUL-terminated 8-bit clean bytes (may contain embedded NULs)
};

inline constexpr uint64_t kTombstoneBits = kNilBits | 1ULL;

struct alignas(8) LuaHashEntry {
    uint64_t key = kNilBits;
    uint64_t val = kNilBits;
};

// The hash part of a table with none: a one-entry, never-written array, so
// compiled code probes `hash_data[hash & hash_mask]` with no capacity check.
extern const LuaHashEntry kEmptyHashPart[1];

// A Lua 5.4 table allocated on brass::gc::Heap:
// 1. Contiguous 0-based `array_data` (Lua index `i` is `array_data[i - 1]`).
// 2. Open-addressed power-of-2 `hash_data` (linear probing, load factor <= 1/2)
//    for every other key, string fields included. Compiled code reads a field
//    with a constant key by probing `hash_data[key.hash & hash_mask]` once.
// A table built with size hints (a constructor's) carries its first array part
// and hash part inline after the header, so `{x = 1, y = 2}` is one allocation;
// growth moves a part out of line for good.
struct alignas(8) LuaTable {
    static constexpr uint32_t kMaxInlineArray = 64;
    static constexpr uint32_t kMaxInlineHash = 64;

    uint64_t*     array_data = nullptr; // Offset 0:  storage for 1..array_cap (inline or on the heap)
    uint32_t      array_cap  = 0;       // Offset 8:  capacity of array_data
    uint32_t      array_len  = 0;       // Offset 12: hint for the border (see rawTableLength)
    uint32_t      hash_mask  = 0;       // Offset 16: hash_cap - 1, or 0 with no hash part
    uint32_t      flags      = 0;       // Offset 20: metamethod bitmask (0 = no metatable/metamethods)
    LuaTable*     metatable  = nullptr; // Offset 24: metatable pointer
    LuaHashEntry* hash_data  = nullptr; // Offset 32: the hash part, or kEmptyHashPart
    uint32_t      hash_len   = 0;       // Offset 40: occupied entries (live + tombstones)
    uint32_t      hash_cap   = 0;       // Offset 44: power-of-2 capacity of hash_data, 0 for none
    uint32_t      cached_mt_flags = 0;  // Offset 48: cached metamethod bitmask when used as a metatable
    uint32_t      inline_bytes    = 0;  // Offset 52: bytes of inline storage after the header
    uint64_t      cached_index_mm = kNilBits; // Offset 56: cached __index value when used as a metatable

    // Inline storage: the array part's first block, then the hash part's.
    uint64_t* inlineStorage() noexcept { return reinterpret_cast<uint64_t*>(this + 1); }
    bool isInline(const void* p) const noexcept {
        auto a = reinterpret_cast<uintptr_t>(p);
        auto lo = reinterpret_cast<uintptr_t>(this + 1);
        return a >= lo && a < lo + inline_bytes;
    }
};

static_assert(sizeof(LuaTable) == 64, "LuaTable header is 64 bytes");
static_assert(offsetof(LuaTable, array_data) == 0, "array_data must be at offset 0");
static_assert(offsetof(LuaTable, array_cap) == 8, "array_cap must be at offset 8");
static_assert(offsetof(LuaTable, hash_mask) == 16, "hash_mask must be at offset 16");
static_assert(offsetof(LuaTable, flags) == 20, "flags must be at offset 20");
static_assert(offsetof(LuaTable, metatable) == 24, "metatable must be at offset 24");
static_assert(offsetof(LuaTable, hash_data) == 32, "hash_data must be at offset 32");

struct alignas(8) LuaUpvalue {
    uint64_t value = kNilBits;
};

using LuaNativeFn = uint64_t (*)(void* closure, const uint64_t* args, int64_t nargs);

struct alignas(8) LuaClosure {
    LuaNativeFn  fn = nullptr;         // Offset 0: compiled or C function pointer
    void*        unboxed_fn = nullptr; // Offset 8: optional unboxed fast entry
    uint32_t     upvalue_count = 0;    // Offset 16
    uint32_t     arity = 0;            // Offset 20
    LuaUpvalue** upvalues = nullptr;   // Offset 24: points to inline trailing upvalue pointers
};

static_assert(offsetof(LuaClosure, fn) == 0, "fn must be at offset 0");
static_assert(offsetof(LuaClosure, unboxed_fn) == 8, "unboxed_fn must be at offset 8");
static_assert(offsetof(LuaClosure, upvalues) == 24, "upvalues must be at offset 24");

enum class CoroutineStatus : uint8_t {
    Suspended = 0,
    Running   = 1,
    Normal    = 2,
    Dead      = 3,
};

// Shares LuaTable's first 40 bytes of layout (no array part, an empty hash
// part, flags with kMetaNonTable) so compiled code that reads an Object-tagged
// value as a table finds nothing in it and takes the runtime's path.
struct alignas(8) LuaCoroutine {
    void*           pad_array_data = nullptr; // Offset 0
    uint32_t        pad_array_cap  = 0;       // Offset 8 (always 0)
    uint32_t        pad_array_len  = 0;       // Offset 12 (always 0)
    uint32_t        pad_hash_mask  = 0;       // Offset 16 (always 0)
    uint32_t        flags          = 1u << 31;// Offset 20 (kMetaNonTable)
    LuaTable*       metatable      = nullptr; // Offset 24
    const LuaHashEntry* pad_hash_data = kEmptyHashPart; // Offset 32
    uint32_t        coro_id        = 0;
    LuaClosure*     entry_closure  = nullptr;
    LuaCoroutine*   caller         = nullptr; // Offset 40
    void*           ctx_state      = nullptr; // Offset 48: heap-allocated CoroutineContext (its brass fiber)
    void (*trace_ctx)(void* ctx_state, void* tracer_ptr) = nullptr;
    CoroutineStatus status         = CoroutineStatus::Suspended;
    bool            error_occurred = false;
};

// The same LuaTable prefix as LuaCoroutine's.
struct alignas(8) LuaUserdata {
    void*     data          = nullptr; // Offset 0: userdata payload or host handle
    uint32_t  pad_array_cap = 0;       // Offset 8 (always 0)
    uint32_t  size          = 0;       // Offset 12: byte size of userdata
    uint32_t  pad_hash_mask = 0;       // Offset 16 (always 0)
    uint32_t  flags         = 1u << 31;// Offset 20 (kMetaNonTable)
    LuaTable* metatable     = nullptr; // Offset 24
    const LuaHashEntry* pad_hash_data = kEmptyHashPart; // Offset 32
    uint32_t  type_tag      = 0;       // host type tag (e.g. bronze::embed bridge)
    uint64_t  user_value    = kNilBits;// Lua 5.4 uservalue slot
    void (*finalizer)(void* data, uint32_t type_tag) = nullptr;
};

static_assert(offsetof(LuaCoroutine, pad_hash_mask) == offsetof(LuaTable, hash_mask) &&
              offsetof(LuaCoroutine, flags) == offsetof(LuaTable, flags) &&
              offsetof(LuaCoroutine, pad_hash_data) == offsetof(LuaTable, hash_data),
              "a coroutine reads as a table with nothing in it");
static_assert(offsetof(LuaUserdata, pad_hash_mask) == offsetof(LuaTable, hash_mask) &&
              offsetof(LuaUserdata, flags) == offsetof(LuaTable, flags) &&
              offsetof(LuaUserdata, pad_hash_data) == offsetof(LuaTable, hash_data),
              "a userdata reads as a table with nothing in it");

} // namespace brasslua
