#include "codegen/speculation.h"

#include "brasslua/runtime.h"

#include <brass/codegen/jit_exec.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/loop_opt.hpp>
#include <brass/mir/osr_entry.hpp>
#include <brass/mir/pass_pipeline.hpp>
#include <brass/mir/sroa.hpp>

#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace brasslua {

using brass::BasicBlock;
using brass::Function;
using brass::Instruction;
using brass::Opcode;

struct ChunkSpeculation::Armed {
    ChunkSpeculation* owner = nullptr;
    std::string name;
    brass::Type return_type;
    // Per guard (by resume id): the id of the site's slow block, which the
    // generic copy of the function shares.
    std::unordered_map<uint32_t, uint32_t> slow_block;
    // Per guard: its continuation, `(ptr live-ins) -> the function's result`,
    // and how many live-ins its buffer holds.
    struct Continuation {
        void* entry = nullptr;
        std::vector<bool> rematerialized;
    };
    std::unordered_map<uint32_t, Continuation> continuations;
};

ChunkSpeculation::ChunkSpeculation() = default;
ChunkSpeculation::~ChunkSpeculation() = default;

extern "C" uint64_t brasslua_rt_resume(void* armed, uint32_t resume_id, const uint64_t* state) {
    auto* a = static_cast<ChunkSpeculation::Armed*>(armed);
    return a->owner->resume(*a, resume_id, state);
}

void ChunkSpeculation::arm(brass::Module& mod, const std::vector<Bets>& bets) {
    bool any = false;
    for (const Bets& fb : bets) any = any || !fb.branches.empty();
    if (!any) return;
    for (Function* fn : mod.functions()) {
        if (fn && !fn->blocks().empty()) brass::sroa_function(*fn);
    }
    generic_ = brass::clone_module(mod);

    for (const Bets& fb : bets) {
        if (fb.branches.empty()) continue;
        Function& fn = *fb.fn;
        // A continuation's result is passed back as bits.
        if (fn.return_type() != brass::Type::i64() && fn.return_type() != brass::Type::f64()) continue;
        auto armed = std::make_unique<Armed>();
        armed->owner = this;
        armed->name = std::string(fn.name());
        armed->return_type = fn.return_type();
        // Every site's state first, from the body as the generic copy has it.
        struct Plan {
            Instruction* branch;
            std::vector<brass::Value*> state;
        };
        std::vector<Plan> plans;
        for (Instruction* br : fb.branches) {
            BasicBlock* slow = br->false_target().block;
            std::string why;
            auto plan = brass::plan_osr_entry(fn, *slow, &why);
            if (!plan) continue; // stays a branch
            Plan p{br, {}};
            // The slow block's parameters take the branch's arguments.
            const auto& slow_args = br->false_target().args;
            for (size_t i = 0; i < plan->live_ins.size(); ++i) {
                const auto& li = plan->live_ins[i];
                if (li.rematerialize) continue;
                brass::Value* v = i < slow_args.size() ? slow_args[i] : const_cast<brass::Value*>(li.value);
                p.state.push_back(v);
            }
            plans.push_back(std::move(p));
        }
        brass::Builder b(fn);
        for (Plan& p : plans) {
            BasicBlock* bb = p.branch->parent();
            BasicBlock* slow = p.branch->false_target().block;
            brass::BranchTarget fast = p.branch->true_target();
            brass::Value* hit = p.branch->operand(0);
            bb->remove_instruction(p.branch);
            b.position_at_end(bb);
            Instruction* guard = b.build_guard(
                hit, "", brass::Span<brass::Value* const>(p.state.data(), p.state.size()));
            const uint32_t resume_id = guard->resume_id();
            const std::string stub = armed->name + "_exit" + std::to_string(resume_id);
            guard->set_symbol(mod.string_pool().intern(stub));
            buildExitStub(mod, *armed, stub, resume_id, p.state);
            b.build_br(fast.block, brass::Span<brass::Value* const>(fast.args.data(), fast.args.size()));
            armed->slow_block[resume_id] = slow->id();
        }
        fn.rebuild_cfg_predecessors();
        if (!armed->slow_block.empty()) armed_.push_back(std::move(armed));
    }
}

void ChunkSpeculation::buildExitStub(brass::Module& mod, Armed& armed, const std::string& name,
                                     uint32_t resume_id, const std::vector<brass::Value*>& state) {
    // stub(state...): the state in a buffer, one word each, resumed by the
    // runtime; its result is the function's.
    std::vector<brass::Type> params;
    for (brass::Value* v : state) params.push_back(v->type());
    Function* stub = mod.create_function(name, armed.return_type, brass::Span<const brass::Type>(params));
    brass::Builder b(*stub);
    b.append_block("entry");
    std::vector<brass::Value*> args;
    for (size_t i = 0; i < params.size(); ++i) args.push_back(b.add_param(params[i]));
    brass::Value* buf = b.build_alloca(static_cast<uint32_t>(std::max<size_t>(params.size(), 1) * 8), 8);
    for (size_t i = 0; i < args.size(); ++i) b.build_store(params[i], buf, static_cast<int32_t>(i * 8), args[i]);
    brass::Value* bits = b.build_call(
        "brasslua_rt_resume", brass::Type::i64(),
        {b.build_iconst_i64(reinterpret_cast<intptr_t>(&armed)), b.build_iconst_i32(static_cast<int32_t>(resume_id)), buf});
    b.build_ret(armed.return_type.is_float() ? b.build_bitcast_f64_i64(bits) : bits);
}

void ChunkSpeculation::install(brass::codegen::JitExecutionEngine& engine) {
    engine_ = &engine;
}

uint64_t ChunkSpeculation::resume(Armed& armed, uint32_t resume_id, const uint64_t* state) {
    auto it = armed.continuations.find(resume_id);
    if (it == armed.continuations.end()) {
        buildContinuation(armed, resume_id);
        it = armed.continuations.find(resume_id);
    }
    const Armed::Continuation& cont = it->second;
    // Live-in i of the continuation is buffer[i]; the guard's state holds
    // the ones it does not rematerialize, in order.
    std::vector<uint64_t> buffer(cont.rematerialized.size(), 0);
    size_t j = 0;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (!cont.rematerialized[i]) buffer[i] = state[j++];
    }
    if (armed.return_type.is_float()) {
        double r = reinterpret_cast<double (*)(uint64_t*)>(cont.entry)(buffer.data());
        uint64_t bits = 0;
        std::memcpy(&bits, &r, sizeof(bits));
        return bits;
    }
    return reinterpret_cast<uint64_t (*)(uint64_t*)>(cont.entry)(buffer.data());
}

void* ChunkSpeculation::buildContinuation(Armed& armed, uint32_t resume_id) {
    Function* generic = generic_->get_function(armed.name);
    BasicBlock* slow = nullptr;
    const uint32_t slow_id = armed.slow_block.at(resume_id);
    for (BasicBlock* bb : generic->blocks()) {
        if (bb && bb->id() == slow_id) slow = bb;
    }
    std::string why;
    auto plan = slow ? brass::plan_osr_entry(*generic, *slow, &why) : std::nullopt;
    if (!plan) throw std::runtime_error("brasslua: no continuation for " + armed.name + ": " + why);

    auto mod = std::make_unique<brass::Module>(armed.name + "_cont");
    const std::string cont_name = armed.name + "_resume" + std::to_string(resume_id);
    Function* cont = brass::build_osr_entry_function(*plan, *mod, cont_name, &why);
    if (!cont) throw std::runtime_error("brasslua: continuation of " + armed.name + ": " + why);

    // What the region names is the chunk's (its functions, loaded) or the
    // runtime's: external here.
    auto cont_engine = std::make_unique<brass::codegen::JitExecutionEngine>();
    registerBrassLuaRuntimeSymbols(*cont_engine);
    std::unordered_set<std::string> declared;
    for (std::string_view sym : generic_->external_symbols()) {
        mod->add_external_symbol(sym);
        declared.emplace(sym);
    }
    for (BasicBlock* bb : cont->blocks()) {
        for (Instruction* inst : *bb) {
            if (!inst || (inst->opcode() != Opcode::call && inst->opcode() != Opcode::func_addr)) continue;
            const std::string sym(inst->symbol());
            if (sym.empty() || !declared.insert(sym).second) continue;
            Function* callee = generic_->get_function(sym);
            if (!callee) continue;
            mod->add_external_symbol(sym);
            cont_engine->register_external_symbol(sym, engine_->get_symbol_address(sym));
            cont_engine->register_function_signature(sym, callee->return_type(), callee->param_types());
        }
    }

    brass::PassPipelineOptions popts = brass::production_pass_pipeline_options();
    popts.enable_sroa = true;
    brass::run_pass_pipeline(*mod, popts);
    if (!cont_engine->compile_and_load(*mod)) {
        throw std::runtime_error("brasslua: continuation of " + armed.name + " failed to compile");
    }
    Armed::Continuation c;
    c.entry = cont_engine->get_symbol_address(cont_name);
    for (const auto& li : plan->live_ins) c.rematerialized.push_back(li.rematerialize);
    armed.continuations[resume_id] = std::move(c);
    continuation_engines_.push_back(std::move(cont_engine));
    return armed.continuations[resume_id].entry;
}

} // namespace brasslua
