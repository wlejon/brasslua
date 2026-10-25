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

    // String interning & field slot assignment (8-bit clean byte strings)
    uint32_t internString(std::string_view str);
    std::string_view getStringById(uint32_t id) const;
    LuaString* getLuaStringById(uint32_t id);
    uint64_t makeStringValue(std::string_view str);

    // Deterministic inline slot mapping for known record fields (0..15)
    int32_t getOrAssignFieldSlot(uint32_t intern_id);
    int32_t findFieldSlot(uint32_t intern_id) const;

    // Globals (_ENV)
    void setGlobal(std::string_view name, Value val);
    void setGlobalById(uint32_t intern_id, Value val);
    Value getGlobal(std::string_view name);
    Value getGlobalById(uint32_t intern_id);
    LuaTable* envTable() const noexcept;

    // Allocations on brass::gc::Heap
    LuaTable* allocTable(uint32_t array_hint = 0, uint32_t hash_hint = 0);
    void ensureTableArrayCapacity(LuaTable* tbl, uint32_t min_cap);
    void ensureTableHashCapacity(LuaTable* tbl, uint32_t min_cap);
    LuaClosure* allocClosure(LuaNativeFn fn, void* unboxed_fn, uint32_t upval_count, uint32_t arity);
    LuaUpvalue* allocUpvalue(uint64_t initial_val);
    LuaBoxedInt64* allocBoxedInt64(int64_t val);
    LuaCoroutine* allocCoroutine(LuaClosure* entry);
    LuaUserdata* allocUserdata(size_t size, uint32_t type_tag = 0);

    // Multi-return scratch buffer
    void setMultiRet(const uint64_t* vals, size_t count);
    size_t multiRetCount() const noexcept { return multiret_.size(); }
    uint64_t getMultiRet(size_t idx) const noexcept {
        return idx < multiret_.size() ? multiret_[idx] : kNilBits;
    }
    const std::vector<uint64_t>& multiRetVec() const noexcept { return multiret_; }

    // Host bridge registration (for bronze / bro integration)
    void setHostBridge(HostBridge bridge) { host_bridge_ = std::move(bridge); }
    const HostBridge& hostBridge() const noexcept { return host_bridge_; }

    // Output capture for testing
    void setOutputCallback(std::function<void(std::string_view)> cb) { output_cb_ = std::move(cb); }
    void writeOutput(std::string_view text);

    // Command-line args (`arg` table in Lua)
    void setScriptArgs(const std::string& script_path, const std::vector<std::string>& args);

    struct Impl;
    Impl* impl() noexcept { return impl_; }
    const Impl* impl() const noexcept { return impl_; }

private:
    RuntimeState();
    ~RuntimeState();

    Impl* impl_;
    std::vector<uint64_t> multiret_;
    HostBridge host_bridge_;
    std::function<void(std::string_view)> output_cb_;
};

// Front-end entry points
Chunk parseLuaSource(std::string_view source, std::string_view source_name = "=stdin");

// Sema & Type/Shape Inference
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

ExecutionResult evalLuaSource(std::string_view source, const ExecutionOptions& opts = {});
ExecutionResult evalLuaFile(const std::string& path, const std::vector<std::string>& script_args = {},
                            const ExecutionOptions& opts = {});
bool compileLuaToObjectFile(std::string_view source, const std::string& output_path,
                            const ExecutionOptions& opts = {});

void registerBrassLuaRuntimeSymbols(brass::codegen::JitExecutionEngine& engine);

} // namespace brasslua
