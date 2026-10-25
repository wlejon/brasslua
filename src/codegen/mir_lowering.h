#pragma once

#include "brasslua/runtime.h"

#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/types.hpp>

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

struct MirValue {
    brass::Value* val = nullptr;
    Rep rep = Rep::Boxed;
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
    std::vector<std::string> captured_names;
    const FuncBody* fbody = nullptr;
};

struct LoopContext {
    brass::BasicBlock* break_block = nullptr;
};

struct FunctionBuildState {
    brass::Function* fn = nullptr;
    brass::Builder builder;
    bool is_unboxed_variant = false;
    Rep expected_return_rep = Rep::Boxed;

    brass::Value* closure_arg = nullptr;
    brass::Value* args_ptr_arg = nullptr;
    brass::Value* nargs_arg = nullptr;
    brass::Value* args_scratch = nullptr; // alloca(128) for call args

    std::vector<std::unordered_map<std::string, LocalVarInfo>> scopes;
    std::vector<LoopContext> loops;
    std::unordered_map<std::string, brass::BasicBlock*> label_blocks;
    std::vector<brass::Value*> close_vars;
    std::string self_func_name;

    explicit FunctionBuildState(brass::Function& f) : fn(&f), builder(f) {}

    brass::BasicBlock* newBlock(std::string_view name = "") {
        brass::BasicBlock* bb = builder.create_block(name);
        fn->append_block(bb);
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

    brass::Value* emitEntryAlloca(uint32_t size = 8, uint32_t align = 8) {
        brass::BasicBlock* cur = builder.current_block();
        brass::BasicBlock* entry = fn->entry_block();
        if (entry && !entry->is_empty()) {
            builder.position_before(entry->head());
        } else if (entry) {
            builder.position_at_end(entry);
        }
        brass::Value* ptr = builder.build_alloca(size, align);
        builder.position_at_end(cur);
        return ptr;
    }

    brass::Value* getArgsScratch() {
        if (!args_scratch) {
            args_scratch = emitEntryAlloca(128, 8);
        }
        return args_scratch;
    }
};

class MirModuleLowerer {
public:
    MirModuleLowerer(RuntimeState& rt, brass::Module& mod)
        : rt_(rt), mod_(mod) {}

    std::string lowerChunk(const Chunk& chunk);

    // Representation helpers
    static Rep repForInferredType(InferredType t) noexcept;
    static brass::Type mirTypeForRep(Rep r) noexcept;

    brass::Value* emitBox(FunctionBuildState& st, MirValue v);
    brass::Value* emitUnboxInt64(FunctionBuildState& st, MirValue v);
    brass::Value* emitUnboxFloat64(FunctionBuildState& st, MirValue v);
    MirValue coerceToRep(FunctionBuildState& st, MirValue v, Rep target);
    brass::Value* emitTruthinessBit(FunctionBuildState& st, MirValue v);

    // Scope & variable helpers
    void pushScope(FunctionBuildState& st);
    void popScope(FunctionBuildState& st);
    LocalVarInfo* lookupLocal(FunctionBuildState& st, const std::string& name);
    LocalVarInfo defineLocal(FunctionBuildState& st, const std::string& name, Rep rep,
                             bool as_upvalue = false, bool is_close = false);
    MirValue loadVar(FunctionBuildState& st, const std::string& name, uint32_t intern_id);
    void storeVar(FunctionBuildState& st, const std::string& name, uint32_t intern_id, MirValue val);

    // Statement & expression lowering
    void lowerStmt(FunctionBuildState& st, const Stmt& stmt);
    void lowerBlockStmts(FunctionBuildState& st, const std::vector<StmtPtr>& stmts);
    MirValue lowerExpr(FunctionBuildState& st, const Expr& expr);
    MirValue lowerBinaryExpr(FunctionBuildState& st, const BinaryExpr& bin, InferredType result_type);
    MirValue lowerCallExpr(FunctionBuildState& st, const CallExpr& call, InferredType result_type);
    MirValue lowerMethodCallExpr(FunctionBuildState& st, const MethodCallExpr& mcall);
    MirValue lowerTableExpr(FunctionBuildState& st, const TableExpr& tbl_expr);
    MirValue lowerIndexLoad(FunctionBuildState& st, const IndexExpr& idx_expr,
                            InferredType inferred_type = InferredType::Any);
    void lowerIndexStore(FunctionBuildState& st, const IndexExpr& idx_expr, MirValue rhs,
                         InferredType target_type = InferredType::Any);
    MirValue lowerFieldLoad(FunctionBuildState& st, const FieldExpr& fld_expr,
                            InferredType inferred_type = InferredType::Any);
    void lowerFieldStore(FunctionBuildState& st, const FieldExpr& fld_expr, MirValue rhs,
                         InferredType target_type = InferredType::Any);

    // Function lowering
    CompiledFuncSig compileFuncBody(FunctionBuildState* parent_st, const std::string& debug_name,
                                    const FuncBody& fbody, bool has_implicit_self);

    RuntimeState& rt() noexcept { return rt_; }
    brass::Module& mod() noexcept { return mod_; }
    const std::unordered_map<std::string, CompiledFuncSig>& knownFunctions() const noexcept {
        return known_functions_;
    }
    void registerKnownFunction(const std::string& name, const CompiledFuncSig& sig) {
        known_functions_[name] = sig;
    }

private:
    void declareRuntimeExternals();

    RuntimeState& rt_;
    brass::Module& mod_;
    uint32_t next_fn_id_ = 1;
    std::unordered_map<std::string, CompiledFuncSig> known_functions_;
};

} // namespace brasslua
