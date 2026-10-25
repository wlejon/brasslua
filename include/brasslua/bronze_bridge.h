#pragma once

#include "brasslua/runtime.h"
#include "embed/embed.h"
#include "embed/embed_typed_array.h"

namespace brasslua::bronze_bridge {

// Type tag used on LuaUserdata wrapping a bronze::Value on the shared brass::gc::Heap.
inline constexpr uint32_t kBronzeHostTypeTag = 0xB50E0001u;

// Attaches `rt` to the current thread's `bronze` `brass::gc::Heap` (One Heap Rule)
// and installs the `HostBridge` and Lua globals (`null`, `await`, `totable`) so
// Lua code can transparently access `bronze` globals (`window`, `document`, `bro.*`,
// `<sibling>_api`, `Math`, `JSON`, `Promise`, etc.) in the same realm.
void attachBronzeBridge(RuntimeState& rt = RuntimeState::current());

// Boundary conversions enforcing all 6 rules from BRASSLUA_PLAN.md Section 2.5:
// 1. Strings: valid UTF-8 LuaString <-> JS String; non-UTF-8 binary LuaString <-> Uint8Array.
// 2. Tables: plain 1-based sequence LuaTable -> 0-based JS Array; plain record LuaTable -> JS Object.
//    When `convertPlainContainers` is true, JS Arrays and plain JS Objects are converted into
//    1-based Lua sequence/record tables; otherwise they are wrapped as live host objects on the
//    shared brass::gc::Heap.
// 3. `nil` <-> `undefined`; `Value::nullSentinel()` (`null` in Lua `_ENV`) <-> JS `null`.
// 4. 64-bit Integers: Int64 within [-2^53+1, 2^53-1] -> JS Number; Int64 outside -> JS BigInt;
//    JS BigInt -> Lua Int64.
// 5. Errors & Exceptions: JS throws -> LuaRuntimeError (caught by pcall/xpcall);
//    uncaught Lua error inside callback -> bronze::embed::throwError (caught by JS try/catch).
// 6. Promises & Coroutines: `await(promise)` yields/resumes Lua coroutines and drains microtasks.
bronze::Value luaToJs(RuntimeState& rt, Value v);
Value jsToLua(RuntimeState& rt, bronze::Value v, bool convertPlainContainers = false);

// Unwraps a LuaUserdata host wrapper back to its underlying bronze::Value (or returns undefined).
bool isBronzeHostObject(Value v) noexcept;
bronze::Value unwrapBronzeHostObject(Value v) noexcept;
Value wrapBronzeHostObject(RuntimeState& rt, bronze::Value v);

// Evaluates a Lua source chunk or file attached to the current thread's bronze realm & heap.
ExecutionResult evalLuaInBronzeRealm(std::string_view source, const ExecutionOptions& opts = {});
ExecutionResult evalLuaFileInBronzeRealm(const std::string& path,
                                         const std::vector<std::string>& script_args = {},
                                         const ExecutionOptions& opts = {});

} // namespace brasslua::bronze_bridge
