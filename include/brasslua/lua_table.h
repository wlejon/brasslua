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

struct alignas(8) LuaHashEntry {
    uint64_t key = kNilBits;
    uint64_t val = kNilBits;
};

// Fixed-offset hybrid Lua 5.4 table allocated on brass::gc::Heap:
// 1. Contiguous 0-based `array_data` pointer (where Lua index `i` is `array_data[i - 1]`).
// 2. 16 inline record slots (`slots[16]`) at fixed offset 32 for O(1) zero-hash field loads/stores.
// 3. Heap-allocated `hash_data` array for arbitrary dynamic keys beyond the 16 inline slots.
struct alignas(8) LuaTable {
    static constexpr size_t kInlineSlots = 16;

    uint64_t*     array_data = nullptr; // Offset 0:  contiguous storage on brass::gc::Heap for 1..array_cap
    uint32_t      array_cap  = 0;       // Offset 8:  allocated capacity of array_data
    uint32_t      array_len  = 0;       // Offset 12: high-water contiguous 1..N length
    uint32_t      shape_id   = 0;       // Offset 16: shape identifier for record fields
    uint32_t      flags      = 0;       // Offset 20: metamethod bitmask (0 = no metatable/metamethods)
    LuaTable*     metatable  = nullptr; // Offset 24: metatable pointer
    uint64_t      slots[kInlineSlots];  // Offset 32..159: 16 inline record field slots
    LuaHashEntry* hash_data  = nullptr; // Offset 160: overflow key/val entries on brass::gc::Heap
    uint32_t      hash_len   = 0;       // Offset 168: number of entries in hash_data
    uint32_t      hash_cap   = 0;       // Offset 172: capacity of hash_data

    void initInlineSlots() noexcept {
        for (size_t i = 0; i < kInlineSlots; ++i) {
            slots[i] = kNilBits;
        }
    }
};

static_assert(offsetof(LuaTable, array_data) == 0, "array_data must be at offset 0");
static_assert(offsetof(LuaTable, array_cap) == 8, "array_cap must be at offset 8");
static_assert(offsetof(LuaTable, array_len) == 12, "array_len must be at offset 12");
static_assert(offsetof(LuaTable, shape_id) == 16, "shape_id must be at offset 16");
static_assert(offsetof(LuaTable, flags) == 20, "flags must be at offset 20");
static_assert(offsetof(LuaTable, metatable) == 24, "metatable must be at offset 24");
static_assert(offsetof(LuaTable, slots) == 32, "slots must be at offset 32");

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

// Shares the first 32 bytes layout prefix with LuaTable (array_cap=0, flags=non-zero)
// so any speculative JIT table guard safely takes the slow path without extra checks.
struct alignas(8) LuaCoroutine {
    void*           pad_array_data = nullptr; // Offset 0
    uint32_t        pad_array_cap  = 0;       // Offset 8 (always 0)
    uint32_t        pad_array_len  = 0;       // Offset 12 (always 0)
    uint32_t        coro_id        = 0;       // Offset 16
    uint32_t        flags          = 1u << 31;// Offset 20 (non-zero so JIT table fast-path skips)
    LuaTable*       metatable      = nullptr; // Offset 24
    LuaClosure*     entry_closure  = nullptr; // Offset 32
    LuaCoroutine*   caller         = nullptr; // Offset 40
    void*           ctx_state      = nullptr; // Offset 48: heap-allocated CoroutineContext (its brass fiber)
    void (*trace_ctx)(void* ctx_state, void* tracer_ptr) = nullptr;
    CoroutineStatus status         = CoroutineStatus::Suspended;
    bool            error_occurred = false;
};

struct alignas(8) LuaUserdata {
    void*     data          = nullptr; // Offset 0: userdata payload or host handle
    uint32_t  pad_array_cap = 0;       // Offset 8 (always 0)
    uint32_t  size          = 0;       // Offset 12: byte size of userdata
    uint32_t  type_tag      = 0;       // Offset 16: host type tag (e.g. bronze::embed bridge)
    uint32_t  flags         = 1u << 31;// Offset 20 (non-zero)
    LuaTable* metatable     = nullptr; // Offset 24
    uint64_t  user_value    = kNilBits;// Offset 32: Lua 5.4 uservalue slot
    void (*finalizer)(void* data, uint32_t type_tag) = nullptr;
};

} // namespace brasslua
