#pragma once

#include "brasslua/runtime.h"

#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/speculation_sites.hpp>
#include <brass/mir/types.hpp>

#include <deque>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace brasslua {

using BValue = brass::Value;

enum class Rep : uint8_t {
    Boxed = 0,   // brass::Type::i64() (NaN-boxed Value: Tag::Int47, Tag::BoxedInt64, double, etc.)
    Int64 = 1,   // brass::Type::i64() (unboxed int64_t)
    Float64 = 2, // brass::Type::f64() (unboxed double)
    BoolBit = 3, // brass::Type::i32() (unboxed 0/1 boolean condition)
};

// A call or `...` that may give any number of values (not in parentheses).
bool isCallOrVararg(const Expr& e) noexcept;

struct MirValue {
    brass::Value* val = nullptr;
    Rep rep = Rep::Boxed;
    // A call that made its one value without the multiret buffer (an
    // inlined body, a function's unboxed entry); see
    // lowerMultiExpr.
    bool single = false;
};

struct LocalVarInfo {
    brass::Value* slot_ptr = nullptr; // alloca pointer, or upvalue pointer
    Rep rep = Rep::Boxed;
    bool is_upvalue = false;
    bool is_close = false;
    uint32_t upvalue_idx = 0;
    bool has_const_int = false;
    int64_t const_int = 0;
};

struct CompiledFuncSig {
    std::string boxed_symbol;
    std::string unboxed_symbol;
    bool has_unboxed = false;
    std::vector<Rep> param_reps;
    Rep return_rep = Rep::Boxed;
    uint32_t arity = 0;
    std::vector<CapturedVar> captured_vars;
    const FuncBody* fbody = nullptr;
};

struct LoopContext {
    brass::BasicBlock* break_block = nullptr;
};

struct FunctionBuildState {
    brass::Function* fn = nullptr;
    brass::Builder builder;
    // This function's speculation sites (MirModuleLowerer::beginFunction).
    brass::SpecSiteEmitter* sites = nullptr;
    bool is_unboxed_variant = false;
    // Set by lowerMultiExpr for the call it lowers, which then leaves all its
    // results in the multiret buffer; any other call wants one result.
    bool multi_result_wanted = false;
    Rep expected_return_rep = Rep::Boxed;

    brass::Value* closure_arg = nullptr;
    brass::Value* args_ptr_arg = nullptr;
    brass::Value* nargs_arg = nullptr;
    brass::Value* args_scratch = nullptr; // alloca(128) for scratch use

    std::vector<std::unordered_map<uint32_t, LocalVarInfo>> scopes;
    std::vector<LoopContext> loops;
    std::unordered_map<std::string, brass::BasicBlock*> label_blocks;
    std::vector<brass::Value*> close_vars;
    std::string self_func_name;
    uint32_t self_func_symbol_id = 0;
    uint32_t vararg_offset = 0;

    explicit FunctionBuildState(brass::Function& f) : fn(&f), builder(f) {}

    brass::BasicBlock* newBlock(std::string_view name = "") {
        brass::BasicBlock* bb = builder.create_block(name);
        fn->append_block(bb);
        return bb;
    }

    // A slow path (the runtime's call, a bet lost): brass's inliner does not
    // weigh it in this function's size.
    brass::BasicBlock* newColdBlock(std::string_view name) {
        brass::BasicBlock* bb = newBlock(name);
        bb->set_cold();
        return bb;
    }

    brass::BasicBlock* newBlock(std::string_view name, std::initializer_list<brass::Type> param_types) {
        brass::BasicBlock* bb = builder.create_block(name);
        fn->append_block(bb);
        for (brass::Type t : param_types) {
            builder.add_block_param(bb, t);
        }
        return bb;
    }

    bool isTerminated() const noexcept {
        return builder.current_block() && builder.current_block()->terminator() != nullptr;
    }

    // A frame buffer of raw words (unboxed numbers): no GC root.
    brass::Value* emitEntryAlloca(uint32_t size = 8, uint32_t align = 8) {
        return atEntry([&] { return builder.build_alloca(size, align); });
    }
    // A frame buffer of `words` Lua values: each a GC root at every GC point
    // of the function, whichever tier runs it.
    brass::Value* emitEntrySlots(uint32_t words) {
        return atEntry([&] { return builder.build_alloca_tagged(words); });
    }

    brass::Value* getArgsScratch() {
        if (!args_scratch) {
            args_scratch = emitEntrySlots(16);
        }
        return args_scratch;
    }

private:
    template <typename Build>
    brass::Value* atEntry(Build&& build) {
        brass::BasicBlock* cur = builder.current_block();
        brass::BasicBlock* entry = fn->entry_block();
        if (entry && !entry->is_empty()) {
            builder.position_before(entry->head());
        } else if (entry) {
            builder.position_at_end(entry);
        }
        brass::Value* ptr = build();
        builder.position_at_end(cur);
        return ptr;
    }
};

// What a speculation site tests (brass::SpecFeedback's kinds).
enum class SpecKind : uint8_t {
    Arith = 0,   // operands of the types their hints say
    Builtin = 1, // the callee is the library function the hints name
    IntKey = 2,  // a table key is an inline integer
};

class MirModuleLowerer {
public:
    // `feedback`: the chunk's speculation counts, for a module brass's tiered
    // pipeline runs (its sites then carry the guards tier 2 arms); null
    // lowers every site as its plain branch.
    MirModuleLowerer(RuntimeState& rt, brass::Module& mod, brass::SpecFeedback* feedback = nullptr)
        : rt_(rt), mod_(mod), feedback_(feedback) {}

    std::string lowerChunk(const Chunk& chunk);

    // Representation helpers
    static Rep repForInferredType(InferredType t) noexcept;
    static brass::Type mirTypeForRep(Rep r) noexcept;
    static std::string varKey(uint32_t symbol_id, const std::string& name) {
        return symbol_id != 0 ? ("#" + std::to_string(symbol_id)) : name;
    }

    brass::Value* emitBox(FunctionBuildState& st, MirValue v);
    brass::Value* emitUnboxInt64(FunctionBuildState& st, MirValue v);
    brass::Value* emitUnboxFloat64(FunctionBuildState& st, MirValue v);
    MirValue coerceToRep(FunctionBuildState& st, MirValue v, Rep target);
    brass::Value* emitTruthinessBit(FunctionBuildState& st, MirValue v);

    // Scope & variable helpers (keyed by lexical symbol_id; symbol_id == 0 is global)
    void pushScope(FunctionBuildState& st);
    void popScope(FunctionBuildState& st);
    LocalVarInfo* lookupLocal(FunctionBuildState& st, uint32_t symbol_id);
    LocalVarInfo defineLocal(FunctionBuildState& st, uint32_t symbol_id, Rep rep,
                             bool as_upvalue = false, bool is_close = false);
    MirValue loadVar(FunctionBuildState& st, uint32_t symbol_id, const std::string& name, uint32_t intern_id);
    void storeVar(FunctionBuildState& st, uint32_t symbol_id, const std::string& name, uint32_t intern_id, MirValue val);

    // Statement & expression lowering
    void lowerStmt(FunctionBuildState& st, const Stmt& stmt);
    void lowerBlockStmts(FunctionBuildState& st, const std::vector<StmtPtr>& stmts);
    void lowerNumericFor(FunctionBuildState& st, const NumericForStmt& s); // mir_lowering_for.cpp
    MirValue lowerExpr(FunctionBuildState& st, const Expr& expr);
    // lowerExpr for a call or `...` whose values are then read from the
    // multiret buffer (the last of a call's arguments or a table's items, the
    // last of a multiple assignment's values, a generic for's iterator, a
    // returned call): a call made without the buffer leaves its value there.
    MirValue lowerMultiExpr(FunctionBuildState& st, const Expr& expr);
    // A call of a closure that is likely `symbol`'s: the compiled function
    // called by name when the closure's entry is it, else as any other.
    brass::Value* emitTargetCall(FunctionBuildState& st, const std::string& symbol, brass::Value* callee_bits,
                                 brass::Value* arg_buf, brass::Value* nargs_v, bool want_multi);
    // Sets the multiret state to this one value, as a call's return would.
    void emitSetSingleRet(FunctionBuildState& st, brass::Value* boxed);
    MirValue lowerBinaryExpr(FunctionBuildState& st, const BinaryExpr& bin, InferredType result_type);
    MirValue lowerCallExpr(FunctionBuildState& st, const CallExpr& call, InferredType result_type);
    MirValue lowerMethodCallExpr(FunctionBuildState& st, const MethodCallExpr& mcall);
    MirValue lowerTableExpr(FunctionBuildState& st, const TableExpr& tbl_expr);
    // A table element or field may hold anything (the hints bet on a key
    // being an integer, not on what is stored).
    MirValue lowerIndexLoad(FunctionBuildState& st, const IndexExpr& idx_expr);
    void lowerIndexStore(FunctionBuildState& st, const IndexExpr& idx_expr, MirValue rhs);
    MirValue lowerFieldLoad(FunctionBuildState& st, const FieldExpr& fld_expr);
    void lowerFieldStore(FunctionBuildState& st, const FieldExpr& fld_expr, MirValue rhs);

    // Field access at a constant string key (mir_lowering_table.cpp). The
    // key is an interned string: its Value bits are a constant of the
    // compiled code. emitFieldProbe branches to `hit_bb(value)` when a Lua
    // table holds the key with a non-nil value, or, through_index, when the
    // table's metatable has a table __index (one level) that does; with
    // nil_is_hit it also takes the key's absence from a table with no __index
    // to `hit_bb(nil)`. Anything else (another object, a collision in the
    // first probe, a function __index) goes to miss_bb.
    brass::Value* emitStringConst(FunctionBuildState& st, uint32_t intern_id);
    void emitFieldProbe(FunctionBuildState& st, brass::Value* obj_bits, uint32_t intern_id,
                        bool through_index, bool nil_is_hit,
                        brass::BasicBlock* hit_bb, brass::BasicBlock* miss_bb);
    brass::Value* emitFieldGet(FunctionBuildState& st, brass::Value* obj_bits, uint32_t intern_id);
    // The env table (_G) as a constant of the code.
    brass::Value* emitEnvConst(FunctionBuildState& st);
    void emitFieldSet(FunctionBuildState& st, brass::Value* obj_bits, uint32_t intern_id, brass::Value* val_bits);

    // Function lowering
    CompiledFuncSig compileFuncBody(FunctionBuildState* parent_st, const std::string& debug_name,
                                    uint32_t func_symbol_id, const FuncBody& fbody, bool has_implicit_self);

    // Starts `st`'s function: its speculation sites, finished once the
    // chunk's CFG is (lowerChunk).
    void beginFunction(FunctionBuildState& st);
    // A speculation site where the hints say `hit` is the likely case
    // (brass/mir/speculation_sites.hpp): ends the block with
    // `br_if hit, fast, slow`, slow the generic code, and leaves the builder
    // at the end of `slow`.
    void emitSite(FunctionBuildState& st, brass::Value* hit, brass::BasicBlock* fast, brass::BasicBlock* slow,
                  SpecKind kind);

    RuntimeState& rt() noexcept { return rt_; }
    brass::Module& mod() noexcept { return mod_; }
    const std::unordered_map<std::string, CompiledFuncSig>& knownFunctions() const noexcept {
        return known_functions_;
    }
    void registerKnownFunction(const std::string& key, const CompiledFuncSig& sig) {
        known_functions_[key] = sig;
    }

private:
    void declareRuntimeExternals();

    RuntimeState& rt_;
    brass::Module& mod_;
    uint32_t next_fn_id_ = 1;
    std::unordered_map<std::string, CompiledFuncSig> known_functions_;
    brass::SpecFeedback* feedback_ = nullptr;
    // Every function's sites, at stable addresses (a nested function's
    // lowering interleaves with its parent's).
    std::deque<std::pair<brass::Function*, brass::SpecSiteEmitter>> sites_;
};

} // namespace brasslua
