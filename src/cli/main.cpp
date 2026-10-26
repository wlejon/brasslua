#include "brasslua/runtime.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    brasslua::ExecutionOptions opts;
    std::vector<std::string> inline_chunks;
    std::string script_path;
    std::string emit_obj_path;
    std::vector<std::string> script_args;

    for (int i = 1; i < argc; ++i) {
        if (!script_path.empty()) {
            script_args.emplace_back(argv[i]);
            continue;
        }
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--version") == 0) {
            std::cout << "BrassLua 0.1.0 (Lua 5.4 on brass JIT/AOT)\n";
            return 0;
        }
        if (std::strcmp(argv[i], "--dump-mir") == 0) {
            opts.dump_mir = true;
            continue;
        }
        if (std::strncmp(argv[i], "--tier=", 7) == 0) {
            const std::string t = argv[i] + 7;
            if (t == "interp") opts.tier = brasslua::ExecutionTier::Interpreter;
            else if (t == "baseline") opts.tier = brasslua::ExecutionTier::Baseline;
            else if (t == "auto") opts.tier = brasslua::ExecutionTier::Auto;
            else {
                std::cerr << "brasslua: --tier= takes interp, baseline or auto\n";
                return 1;
            }
            continue;
        }
        if (std::strcmp(argv[i], "-O0") == 0) {
            opts.optimize = false;
            continue;
        }
        if (std::strcmp(argv[i], "--emit-obj") == 0 && i + 1 < argc) {
            emit_obj_path = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            inline_chunks.emplace_back(argv[++i]);
            continue;
        }
        if ( std::strncmp(argv[i], "-e", 2) == 0 && std::strlen(argv[i]) > 2) {
            inline_chunks.emplace_back(argv[i] + 2);
            continue;
        }
        script_path = argv[i];
    }

    for (const auto& code : inline_chunks) {
        opts.source_name = "=(command line)";
        auto res = brasslua::evalLuaSource(code, opts);
        if (!res.success) {
            std::cerr << "brasslua: " << res.error_message << "\n";
            return 1;
        }
    }

    if (!script_path.empty()) {
        if (!emit_obj_path.empty()) {
            std::ifstream ifs(script_path, std::ios::binary);
            if (!ifs) {
                std::cerr << "brasslua: cannot open " << script_path << "\n";
                return 1;
            }
            std::string src((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (!brasslua::compileLuaToObjectFile(src, emit_obj_path, opts)) {
                std::cerr << "brasslua: failed to emit object file " << emit_obj_path << "\n";
                return 1;
            }
            return 0;
        }
        auto res = brasslua::evalLuaFile(script_path, script_args, opts);
        if (!res.success) {
            std::cerr << "brasslua: " << res.error_message << "\n";
            return 1;
        }
    }

    return 0;
}
