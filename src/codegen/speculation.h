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
namespace runtime {
class DeoptFrame;
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

private:
    struct Armed;
    uint64_t resume(Armed& armed, const brass::runtime::DeoptFrame& frame);
    void* buildContinuation(Armed& armed, uint32_t resume_id);

    std::unique_ptr<brass::Module> generic_;
    std::vector<std::unique_ptr<Armed>> armed_;
    brass::codegen::JitExecutionEngine* engine_ = nullptr;
    std::vector<std::unique_ptr<brass::codegen::JitExecutionEngine>> continuation_engines_;
    std::vector<void*> resumer_entries_;
};

} // namespace brasslua
