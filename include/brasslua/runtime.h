#pragma once

#include "brasslua/abi.h"
#include "brasslua/ast.h"
#include "brasslua/lua_table.h"
#include "brasslua/value.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace brass::codegen {
class JitExecutionEngine;
}

namespace brass::gc {
class Heap;
}

namespace brasslua {

// Host bridge hook so bronze (when linked via brasslua_bronze) can intercept
// global lookups and operations on host objects (bro.*, DOM, <sibling>_api).
struct HostBridge {
    std::function<uint64_t(std::string_view name)> getGlobal;
    std::function<void(std::string_view name, uint64_t val)> setGlobal;
    std::function<uint64_t(uint64_t obj, std::string_view key)> getProperty;
    std::function<void(uint64_t obj, std::string_view key, uint64_t val)> setProperty;
    std::function<uint64_t(uint64_t obj, uint64_t key)> getIndex;
    std::function<void(uint64_t obj, uint64_t key, uint64_t val)> setIndex;
    std::function<uint64_t(uint64_t callee, uint64_t this_val, const uint64_t* args, size_t nargs)> callHost;
    std::function<uint64_t(uint64_t obj, std::string_view method, const uint64_t* args, size_t nargs)> callMethod;
    std::function<bool(uint64_t val)> isHostObject;
    std::function<std::string(uint64_t val)> toString;
};

class RuntimeState {
public:
    static RuntimeState& current();

    void reset();
    void registerStdlib();

    // Shared brass::gc::Heap integration (One Heap Rule)
    brass::gc::Heap& heap() noexcept;
    void attachExternalHeap(brass::gc::Heap* external_heap);
    void collectGarbage();

    // String interning (8-bit clean byte strings)
    // internString's string lives as long as the runtime (its id may be
    // compiled into code); makeStringValue's is collected when unreferenced.
    uint32_t internString(std::string_view str);
    std::string_view getStringById(uint32_t id) const;
    LuaString* getLuaStringById(uint32_t id);
    uint64_t makeStringValue(std::string_view str);
    // The interned string equal to a string value: the value itself when it
    // is interned; else the interned string of its bytes, which `insert`
    // makes the value itself when there is none (so a key stored once is
    // found by every equal string after it). Nil bits when there is none and
    // `insert` is false: no table holds such a key.
    uint64_t internKey(uint64_t str_bits, bool insert);
    // A new string of these bytes, not interned (internString interns one).
    LuaString* allocString(std::string_view str);

    // Globals: the fields of the env table (_G), raw, a missing one asked of
    // the host bridge. Compiled code reads and writes them as fields.
    void setGlobal(std::string_view name, Value val);
    Value getGlobal(std::string_view name);
    LuaTable* envTable() const noexcept;

    // Allocations on brass::gc::Heap
    LuaTable* allocTable(uint32_t array_hint = 0, uint32_t hash_hint = 0);
    void ensureTableArrayCapacity(LuaTable* tbl, uint32_t min_cap);
    void ensureTableHashCapacity(LuaTable* tbl, uint32_t min_cap);
    LuaClosure* allocClosure(LuaNativeFn fn, void* unboxed_fn, uint32_t upval_count, uint32_t arity,
                             bool init_upvalues = true);
    LuaUpvalue* allocUpvalue(uint64_t initial_val);
    LuaBoxedInt64* allocBoxedInt64(int64_t val);
    LuaCoroutine* allocCoroutine(LuaClosure* entry);
    LuaUserdata* allocUserdata(size_t size, uint32_t type_tag = 0);

    // Multi-return scratch buffer (mode 0 = unset, 1 = single_ret_, 2 = multiret_ vector)
    void clearMultiRet() noexcept {
        multiret_mode_ = 0;
    }
    void setSingleRet(uint64_t val) noexcept {
        single_ret_ = val;
        multiret_mode_ = 1;
    }
    void setMultiRet(const uint64_t* vals, size_t count);
    bool isMultiRetSet() const noexcept { return multiret_mode_ != 0; }
    // Compiled code clears the mode with a store here (its code is this state's).
    uint8_t* multiRetModeSlot() noexcept { return &multiret_mode_; }
    size_t multiRetCount() const noexcept {
        if (multiret_mode_ == 1) return 1;
        return multiret_mode_ == 2 ? multiret_.size() : 0;
    }
    uint64_t getMultiRet(size_t idx) const noexcept {
        if (multiret_mode_ == 1) return idx == 0 ? single_ret_ : kNilBits;
        return (multiret_mode_ == 2 && idx < multiret_.size()) ? multiret_[idx] : kNilBits;
    }
    const std::vector<uint64_t>& multiRetVec();

    // Host bridge registration (for bronze / bro integration)
    void setHostBridge(HostBridge bridge) { host_bridge_ = std::move(bridge); }
    const HostBridge& hostBridge() const noexcept { return host_bridge_; }

    // Output capture for testing
    void setOutputCallback(std::function<void(std::string_view)> cb) { output_cb_ = std::move(cb); }
    void writeOutput(std::string_view text);

    // Command-line args (`arg` table in Lua)
    void setScriptArgs(const std::string& script_path, const std::vector<std::string>& args);

    // Values a native function keeps in a vector of its own (off the stack,
    // where the conservative scan does not look) while it allocates: each
    // collection visits them while the guard lives. Guards nest.
    class RootedValues {
    public:
        explicit RootedValues(RuntimeState& rt);
        ~RootedValues();
        RootedValues(const RootedValues&) = delete;
        RootedValues& operator=(const RootedValues&) = delete;
        std::vector<uint64_t> values;

    private:
        RuntimeState& rt_;
    };

    struct Impl;
    Impl* impl() noexcept { return impl_; }
    const Impl* impl() const noexcept { return impl_; }

private:
    RuntimeState();
    ~RuntimeState();

    // The interned string of these bytes, made (weakly held or not) when
    // there is none.
    uint32_t internAs(std::string_view str, bool weak);
    // Enters a string not yet interned into the intern table.
    uint32_t adoptInterned(LuaString* lstr, uint32_t hash, bool weak);

    Impl* impl_;
    std::vector<uint64_t> multiret_;
    uint64_t single_ret_ = kNilBits;
    uint8_t multiret_mode_ = 0;
    HostBridge host_bridge_;
    std::function<void(std::string_view)> output_cb_;
};

// Front-end entry points
Chunk parseLuaSource(std::string_view source, std::string_view source_name = "=stdin");

// Sema & Type/Shape Inference
void resolveChunkSymbols(Chunk& chunk);
void analyzeAndInferTypes(Chunk& chunk, RuntimeState& rt);

// JIT & AOT Execution Engine (backed by brass)
struct ExecutionOptions {
    bool optimize = true;
    bool dump_mir = false;
    std::string source_name = "=chunk";
};

struct ExecutionResult {
    bool success = false;
    Value return_value = Value::nil();
    std::vector<Value> return_values;
    std::string error_message;
};

// Compiles a chunk to its main function, a vararg Lua function whose `...`
// are its arguments, its code loaded for the life of the thread. Throws the
// syntax or compile error.
LuaNativeFn compileLuaChunk(std::string_view source, const ExecutionOptions& opts = {});
ExecutionResult evalLuaSource(std::string_view source, const ExecutionOptions& opts = {},
                              const uint64_t* args = nullptr, size_t nargs = 0);
ExecutionResult evalLuaFile(const std::string& path, const std::vector<std::string>& script_args = {},
                            const ExecutionOptions& opts = {});
bool compileLuaToObjectFile(std::string_view source, const std::string& output_path,
                            const ExecutionOptions& opts = {});

void registerBrassLuaRuntimeSymbols(brass::codegen::JitExecutionEngine& engine);

} // namespace brasslua
