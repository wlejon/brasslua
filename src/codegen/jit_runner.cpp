#include "codegen/mir_lowering.h"

#include <brass/mir/pass_pipeline.hpp>
#include <brass/mir/printer.hpp>
#include <brass/object/elf_writer.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/runtime/code_installer.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>
#include <brass/runtime/native_function.hpp>
#include <brass/runtime/osr_coordinator.hpp>
#include <brass/runtime/tiering.hpp>

#include "runtime/runtime_internal.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

namespace brasslua {

namespace {

// Every runtime function lowered code calls.
#define BRASSLUA_RT_FUNCTIONS(X)                                                                          \
    X(brasslua_rt_table_new) X(brasslua_rt_table_get) X(brasslua_rt_table_set)                           \
    X(brasslua_rt_table_get_int) X(brasslua_rt_table_set_int) X(brasslua_rt_table_get_field)             \
    X(brasslua_rt_table_set_field) X(brasslua_rt_table_append_multiret) X(brasslua_rt_table_len)         \
    X(brasslua_rt_rawget) X(brasslua_rt_rawset) X(brasslua_rt_next) X(brasslua_rt_get_global)            \
    X(brasslua_rt_set_global) X(brasslua_rt_box_int64) X(brasslua_rt_concat) X(brasslua_rt_obj_len)      \
    X(brasslua_rt_closure_new) X(brasslua_rt_upvalue_new) X(brasslua_rt_upvalue_get)                     \
    X(brasslua_rt_upvalue_set) X(brasslua_rt_close_local) X(brasslua_rt_call) X(brasslua_rt_call_0)      \
    X(brasslua_rt_call_1) X(brasslua_rt_call_2) X(brasslua_rt_call_3) X(brasslua_rt_call_4)              \
    X(brasslua_rt_call_multiret) X(brasslua_rt_call_method) X(brasslua_rt_call_method_multiret)          \
    X(brasslua_rt_set_multiret) X(brasslua_rt_prepend_multiret) X(brasslua_rt_set_vararg_multiret)       \
    X(brasslua_rt_get_multiret_count) X(brasslua_rt_get_multiret) X(brasslua_rt_add) X(brasslua_rt_sub)  \
    X(brasslua_rt_mul) X(brasslua_rt_div) X(brasslua_rt_idiv) X(brasslua_rt_mod) X(brasslua_rt_pow)      \
    X(brasslua_rt_unm) X(brasslua_rt_band) X(brasslua_rt_bor) X(brasslua_rt_bxor) X(brasslua_rt_bnot)    \
    X(brasslua_rt_shl) X(brasslua_rt_shr) X(brasslua_rt_eq) X(brasslua_rt_lt) X(brasslua_rt_le)          \
    X(brasslua_rt_to_number) X(brasslua_rt_to_int64) X(brasslua_rt_fmod) X(brasslua_rt_pow_f64)          \
    X(brasslua_rt_idiv_i64) X(brasslua_rt_forprep) X(brasslua_rt_mod_i64)

template <class Target>
void registerRuntimeFunctions(Target& target) {
#define BRASSLUA_RT_REGISTER(name) brass::runtime::register_native_function(target, #name, &name);
    BRASSLUA_RT_FUNCTIONS(BRASSLUA_RT_REGISTER)
#undef BRASSLUA_RT_REGISTER
}

// A chunk on brass's tiered pipeline: its module, the program (handles,
// tiering, background compiler) that runs it, and its speculation counts,
// which every tier's code increments in place. The table goes first, since
// it stops the compiler that reads the module and the counts.
struct TieredChunk {
    std::unique_ptr<brass::SpecFeedback> feedback;
    std::unique_ptr<brass::Module> mod;
    std::unique_ptr<brass::runtime::FunctionDispatchTable> table;
    ~TieredChunk() { table.reset(); }
};
thread_local std::vector<std::unique_ptr<TieredChunk>> g_live_tiered;

// Backedges an interpreted or baseline loop takes before a loop still running
// asks for its OSR entry (bronze's threshold).
constexpr uint64_t kOsrBackedgeThreshold = 1000;

brass::PassPipelineOptions optimizedPasses() {
    brass::PassPipelineOptions popts = brass::production_pass_pipeline_options();
    popts.enable_sroa = true;
    // Calls of the function a call is likely of are hinted (emitTargetCall).
    popts.enable_inlining = true;
    popts.inliner.only_inline_leaf_functions = false;
    popts.inliner.max_total_caller_instructions = 6000;
    return popts;
}

brass::runtime::TieringConfig tieringConfigFor(ExecutionTier tier) {
    brass::runtime::TieringConfig config;
    config.set_tier0_interpreter(brass::runtime::Tier0Interpreter::Fast);
    switch (tier) {
        case ExecutionTier::Interpreter: config.max_tier = brass::runtime::TierLevel::Tier0_Interpreter; break;
        case ExecutionTier::Baseline: config.max_tier = brass::runtime::TierLevel::Tier1_Baseline; break;
        case ExecutionTier::Auto:
            config.enable_background_compile = true;
            config.enable_background_tier1 = true;
            break;
    }
    return config;
}

LuaNativeFn loadTiered(std::unique_ptr<TieredChunk> chunk, const std::string& entry_sym, ExecutionTier tier) {
    chunk->table = std::make_unique<brass::runtime::FunctionDispatchTable>();
    brass::runtime::MultiTierPipeline& pipeline = chunk->table->pipeline();
    pipeline.initialize(tieringConfigFor(tier));
    registerRuntimeFunctions(pipeline);
    if (tier == ExecutionTier::Auto) {
        // A hot function's copy, or a hot loop's OSR entry, has the bets
        // that held armed as guards (a lost one finishes the call in Tier 0)
        // and the rest kept as branches, then is optimized.
        brass::SpecFeedback* feedback = chunk->feedback.get();
        pipeline.set_tier2_front_pass([feedback](brass::Module& copy) {
            brass::arm_speculation_sites(copy, *feedback);
        });
        // A guard that failed often enough to drop its code is never armed
        // again.
        pipeline.set_invalidation_observer(
            [feedback](std::string_view, uint32_t resume_id) { feedback->mark_failed(resume_id); });
        pipeline.set_tier2_passes(optimizedPasses());
        brass::runtime::OsrCoordinator& osr = chunk->table->osr();
        osr.set_threshold(kOsrBackedgeThreshold);
        osr.set_enabled(true);
    }
    const brass::Function* entry = chunk->mod->get_function(entry_sym);
    if (!entry) throw std::runtime_error("entry symbol not found in the chunk's module");
    for (const brass::Function* fn : chunk->mod->functions()) {
        if (fn && fn->block_count() != 0) chunk->table->get_or_create(fn->name(), fn);
    }
    auto entry_fn = reinterpret_cast<LuaNativeFn>(pipeline.function_address(entry_sym, entry));
    if (!entry_fn) throw std::runtime_error("the chunk's entry has no address");
    g_live_tiered.push_back(std::move(chunk));
    return entry_fn;
}

} // namespace

LuaNativeFn compileLuaChunk(std::string_view source, const ExecutionOptions& opts) {
    auto& rt = RuntimeState::current();
    Chunk chunk = parseLuaSource(source, opts.source_name);
    analyzeAndInferTypes(chunk, rt);

    auto tiered = std::make_unique<TieredChunk>();
    // Only optimized code speculates: a lower tier runs each site's branch.
    // A bet is armed while it has held every time.
    if (opts.tier == ExecutionTier::Auto) {
        tiered->feedback = std::make_unique<brass::SpecFeedback>(
            [](uint8_t, uint32_t misses) { return misses == 0; });
    }
    tiered->mod = std::make_unique<brass::Module>(opts.source_name);
    MirModuleLowerer lowerer(rt, *tiered->mod, tiered->feedback.get());
    std::string entry_sym = lowerer.lowerChunk(chunk);
    if (opts.dump_mir) brass::print_module(*tiered->mod, std::cerr);
    // The chunk's code, and the closures it makes, live as long as the thread.
    return loadTiered(std::move(tiered), entry_sym, opts.tier);
}

ExecutionResult evalLuaSource(std::string_view source, const ExecutionOptions& opts,
                              const uint64_t* args, size_t nargs) {
    ExecutionResult result;
    auto& rt = RuntimeState::current();
    try {
        LuaNativeFn entry_fn = compileLuaChunk(source, opts);
        rt.clearMultiRet();
        uint64_t ret_bits = entry_fn(nullptr, args, static_cast<int64_t>(nargs));
        result.success = true;
        result.return_value = Value(ret_bits);
        const auto& mret = rt.multiRetVec();
        if (rt.isMultiRetSet()) {
            result.return_values.reserve(mret.size());
            for (uint64_t b : mret) {
                result.return_values.emplace_back(b);
            }
        } else {
            result.return_values.push_back(result.return_value);
        }
        return result;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        return result;
    }
}

ExecutionResult evalLuaFile(const std::string& path, const std::vector<std::string>& script_args,
                            const ExecutionOptions& opts) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        ExecutionResult res;
        res.error_message = "cannot open " + path + ": No such file or directory";
        return res;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string content = oss.str();

    // Strip optional Unix shebang line `#!...` preserving line numbers
    if (content.size() >= 2 && content[0] == '#' && content[1] == '!') {
        size_t nl = content.find('\n');
        if (nl != std::string::npos) {
            content.erase(0, nl);
        } else {
            content.clear();
        }
    }

    auto& rt = RuntimeState::current();
    rt.setScriptArgs(path, script_args);
    ExecutionOptions file_opts = opts;
    file_opts.source_name = "@" + path;
    // The script's arguments are its `...`.
    RuntimeState::RootedValues argv(rt);
    for (const std::string& a : script_args) argv.values.push_back(rt.makeStringValue(a));
    return evalLuaSource(content, file_opts, argv.values.data(), argv.values.size());
}

bool compileLuaToObjectFile(std::string_view source, const std::string& output_path,
                            const ExecutionOptions& opts) {
    auto& rt = RuntimeState::current();
    Chunk chunk = parseLuaSource(source, opts.source_name);
    analyzeAndInferTypes(chunk, rt);

    brass::Module mod(opts.source_name);
    MirModuleLowerer lowerer(rt, mod);
    (void)lowerer.lowerChunk(chunk);

    if (opts.optimize) {
        brass::PassPipelineOptions popts = brass::production_pass_pipeline_options();
        popts.enable_sroa = true;
        brass::run_pass_pipeline(mod, popts);
    }

    brass::object::ModuleCompiler compiler(brass::Target::host());
    compiler.set_enable_mir_opts(false);
    brass::object::ObjectFile obj = compiler.compile(mod);
    brass::object::ElfWriter writer(obj);
    return writer.write_to_file(output_path);
}

} // namespace brasslua
