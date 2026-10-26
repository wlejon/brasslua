#pragma once

#include <brass/mir/module.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace brass {
class Instruction;
namespace codegen {
class JitExecutionEngine;
}
} // namespace brass

namespace brasslua {

// Speculation. The lowering makes generic code: every fast path that is
// right only for some operands (two floats, a builtin as the callee) is a
// site, `br_if %hit, fast, slow`, whose slow side is the generic code. A
// site whose hint (the hinted type run) says the fast path is the likely one
// is a bet: the code that runs has a guard on %hit in place of the branch,
// its slow side gone, so what follows the site sees only the fast path's
// values. A failed guard resumes the call in the generic code: the
// function's unarmed body, from the site's slow block, built and compiled
// the first time that site fails (brass's OSR entry function, whose live-ins
// are the guard's state).
class ChunkSpeculation {
public:
    ChunkSpeculation();
    ~ChunkSpeculation();
    ChunkSpeculation(const ChunkSpeculation&) = delete;
    ChunkSpeculation& operator=(const ChunkSpeculation&) = delete;

    // The bets of one function, br_ifs of its body.
    struct Bets {
        brass::Function* fn = nullptr;
        std::vector<brass::Instruction*> branches;
    };

    // Before the optimizer: promotes the module's locals (SROA, so a guard's
    // state is values rather than frame slots), keeps a copy of the generic
    // module for the continuations, and turns each bet into its guard.
    void arm(brass::Module& mod, const std::vector<Bets>& bets);
    // Once the armed module is loaded in `engine` (which outlives this):
    // each armed function's failed guards resume in its continuations.
    void install(brass::codegen::JitExecutionEngine& engine);

    // One armed function. A failed guard calls its exit stub, which hands
    // the guard's state to the runtime (brasslua_rt_resume): `resume` runs
    // the site's continuation and returns the function's result as bits.
    // A stub is an ordinary call, so a function inlined with its guards
    // resumes in its own continuation and the caller goes on with the result.
    struct Armed;
    uint64_t resume(Armed& armed, uint32_t resume_id, const uint64_t* state);

private:
    void buildExitStub(brass::Module& mod, Armed& armed, const std::string& name, uint32_t resume_id,
                       const std::vector<brass::Value*>& state);
    void* buildContinuation(Armed& armed, uint32_t resume_id);

    std::unique_ptr<brass::Module> generic_;
    std::vector<std::unique_ptr<Armed>> armed_;
    brass::codegen::JitExecutionEngine* engine_ = nullptr;
    std::vector<std::unique_ptr<brass::codegen::JitExecutionEngine>> continuation_engines_;
};

extern "C" uint64_t brasslua_rt_resume(void* armed, uint32_t resume_id, const uint64_t* state);

} // namespace brasslua
