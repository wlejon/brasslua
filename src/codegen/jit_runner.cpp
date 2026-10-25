#include "codegen/mir_lowering.h"

#include <brass/codegen/jit_exec.hpp>
#include <brass/mir/pass_pipeline.hpp>
#include <brass/mir/printer.hpp>
#include <brass/object/elf_writer.hpp>
#include <brass/object/object_writer.hpp>

#include "runtime/runtime_internal.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

namespace brasslua {

using brass::Type;

namespace {

thread_local std::vector<std::unique_ptr<brass::codegen::JitExecutionEngine>> g_live_engines;

} // namespace

void registerBrassLuaRuntimeSymbols(brass::codegen::JitExecutionEngine& engine) {
    auto reg = [&](const char* name, auto fn_ptr, Type ret, std::initializer_list<Type> params) {
        engine.register_external_symbol(name, reinterpret_cast<void*>(fn_ptr));
        engine.register_function_signature(name, ret, std::vector<Type>(params));
    };

    reg("brasslua_rt_table_new", &brasslua_rt_table_new, Type::i64(), {Type::i32(), Type::i32()});
    reg("brasslua_rt_table_get", &brasslua_rt_table_get, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_table_set", &brasslua_rt_table_set, Type::i64(), {Type::i64(), Type::i64(), Type::i64()});
    reg("brasslua_rt_table_get_int", &brasslua_rt_table_get_int, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_table_set_int", &brasslua_rt_table_set_int, Type::i64(), {Type::i64(), Type::i64(), Type::i64()});
    reg("brasslua_rt_table_get_field", &brasslua_rt_table_get_field, Type::i64(), {Type::i64(), Type::i32(), Type::i32()});
    reg("brasslua_rt_table_set_field", &brasslua_rt_table_set_field, Type::i64(), {Type::i64(), Type::i32(), Type::i32(), Type::i64()});
    reg("brasslua_rt_table_append_multiret", &brasslua_rt_table_append_multiret, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_table_len", &brasslua_rt_table_len, Type::i64(), {Type::i64()});
    reg("brasslua_rt_rawget", &brasslua_rt_rawget, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_rawset", &brasslua_rt_rawset, Type::i64(), {Type::i64(), Type::i64(), Type::i64()});
    reg("brasslua_rt_next", &brasslua_rt_next, Type::i64(), {Type::i64(), Type::i64(), Type::ptr()});

    reg("brasslua_rt_get_global", &brasslua_rt_get_global, Type::i64(), {Type::i32()});
    reg("brasslua_rt_set_global", &brasslua_rt_set_global, Type::void_type(), {Type::i32(), Type::i64()});

    reg("brasslua_rt_string_from_id", &brasslua_rt_string_from_id, Type::i64(), {Type::i32()});
    reg("brasslua_rt_box_int64", &brasslua_rt_box_int64, Type::i64(), {Type::i64()});
    reg("brasslua_rt_concat", &brasslua_rt_concat, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_obj_len", &brasslua_rt_obj_len, Type::i64(), {Type::i64()});

    reg("brasslua_rt_closure_new", &brasslua_rt_closure_new, Type::i64(), {Type::ptr(), Type::ptr(), Type::i32(), Type::i32()});
    reg("brasslua_rt_upvalue_new", &brasslua_rt_upvalue_new, Type::ptr(), {Type::i64()});
    reg("brasslua_rt_upvalue_get", &brasslua_rt_upvalue_get, Type::i64(), {Type::ptr(), Type::i32()});
    reg("brasslua_rt_upvalue_set", &brasslua_rt_upvalue_set, Type::void_type(), {Type::ptr(), Type::i32(), Type::i64()});
    reg("brasslua_rt_close_local", &brasslua_rt_close_local, Type::void_type(), {Type::i64()});

    reg("brasslua_rt_call", &brasslua_rt_call, Type::i64(), {Type::i64(), Type::ptr(), Type::i64()});
    reg("brasslua_rt_call_0", &brasslua_rt_call_0, Type::i64(), {Type::i64()});
    reg("brasslua_rt_call_1", &brasslua_rt_call_1, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_call_2", &brasslua_rt_call_2, Type::i64(), {Type::i64(), Type::i64(), Type::i64()});
    reg("brasslua_rt_call_3", &brasslua_rt_call_3, Type::i64(), {Type::i64(), Type::i64(), Type::i64(), Type::i64()});
    reg("brasslua_rt_call_4", &brasslua_rt_call_4, Type::i64(), {Type::i64(), Type::i64(), Type::i64(), Type::i64(), Type::i64()});
    reg("brasslua_rt_call_multiret", &brasslua_rt_call_multiret, Type::i64(), {Type::i64(), Type::ptr(), Type::i64()});
    reg("brasslua_rt_call_method", &brasslua_rt_call_method, Type::i64(), {Type::i64(), Type::i32(), Type::i32(), Type::ptr(), Type::i64()});
    reg("brasslua_rt_call_method_multiret", &brasslua_rt_call_method_multiret, Type::i64(), {Type::i64(), Type::i32(), Type::i32(), Type::ptr(), Type::i64()});
    reg("brasslua_rt_set_multiret", &brasslua_rt_set_multiret, Type::void_type(), {Type::ptr(), Type::i64()});
    reg("brasslua_rt_clear_multiret", &brasslua_rt_clear_multiret, Type::void_type(), {});
    reg("brasslua_rt_prepend_multiret", &brasslua_rt_prepend_multiret, Type::i64(), {Type::ptr(), Type::i64()});
    reg("brasslua_rt_set_vararg_multiret", &brasslua_rt_set_vararg_multiret, Type::i64(), {Type::ptr(), Type::i64(), Type::i64()});
    reg("brasslua_rt_get_multiret_count", &brasslua_rt_get_multiret_count, Type::i64(), {});
    reg("brasslua_rt_get_multiret", &brasslua_rt_get_multiret, Type::i64(), {Type::i64()});

    reg("brasslua_rt_add", &brasslua_rt_add, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_sub", &brasslua_rt_sub, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_mul", &brasslua_rt_mul, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_div", &brasslua_rt_div, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_idiv", &brasslua_rt_idiv, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_mod", &brasslua_rt_mod, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_pow", &brasslua_rt_pow, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_unm", &brasslua_rt_unm, Type::i64(), {Type::i64()});
    reg("brasslua_rt_band", &brasslua_rt_band, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_bor", &brasslua_rt_bor, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_bxor", &brasslua_rt_bxor, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_bnot", &brasslua_rt_bnot, Type::i64(), {Type::i64()});
    reg("brasslua_rt_shl", &brasslua_rt_shl, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_shr", &brasslua_rt_shr, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_eq", &brasslua_rt_eq, Type::i32(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_lt", &brasslua_rt_lt, Type::i32(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_le", &brasslua_rt_le, Type::i32(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_to_number", &brasslua_rt_to_number, Type::f64(), {Type::i64()});
    reg("brasslua_rt_to_int64", &brasslua_rt_to_int64, Type::i64(), {Type::i64()});

    reg("brasslua_rt_fmod", &brasslua_rt_fmod, Type::f64(), {Type::f64(), Type::f64()});
    reg("brasslua_rt_pow_f64", &brasslua_rt_pow_f64, Type::f64(), {Type::f64(), Type::f64()});
    reg("brasslua_rt_idiv_i64", &brasslua_rt_idiv_i64, Type::i64(), {Type::i64(), Type::i64()});
    reg("brasslua_rt_mod_i64", &brasslua_rt_mod_i64, Type::i64(), {Type::i64(), Type::i64()});
}

ExecutionResult evalLuaSource(std::string_view source, const ExecutionOptions& opts) {
    ExecutionResult result;
    auto& rt = RuntimeState::current();
    try {
        Chunk chunk = parseLuaSource(source, opts.source_name);
        analyzeAndInferTypes(chunk, rt);

        brass::Module mod(opts.source_name);
        MirModuleLowerer lowerer(rt, mod);
        std::string entry_sym = lowerer.lowerChunk(chunk);

        if (opts.optimize) {
            brass::PassPipelineOptions popts = brass::production_pass_pipeline_options();
            popts.enable_sroa = true;
            brass::run_pass_pipeline(mod, popts);
        }

        if (opts.dump_mir) {
            brass::print_module(mod, std::cerr);
        }

        auto engine = std::make_unique<brass::codegen::JitExecutionEngine>();
        registerBrassLuaRuntimeSymbols(*engine);
        if (!engine->compile_and_load(mod)) {
            result.error_message = "brass JIT compilation failed";
            return result;
        }

        auto entry_fn = engine->get_function_ptr<LuaNativeFn>(entry_sym);
        if (!entry_fn) {
            result.error_message = "entry symbol not found after JIT compilation";
            return result;
        }

        // Keep JIT engine alive so any closures created during execution remain valid
        g_live_engines.push_back(std::move(engine));

        rt.clearMultiRet();
        uint64_t ret_bits = entry_fn(nullptr, nullptr, 0);
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

    RuntimeState::current().setScriptArgs(path, script_args);
    ExecutionOptions file_opts = opts;
    file_opts.source_name = "@" + path;
    return evalLuaSource(content, file_opts);
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
