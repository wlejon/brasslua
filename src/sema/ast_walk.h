#pragma once

#include "brasslua/ast.h"

#include <functional>

namespace brasslua {

// Calls `fn` on every function of the chunk, outer ones before the ones
// they contain.
void forEachFuncBody(Chunk& chunk, const std::function<void(FuncBody&)>& fn);

// The function a function returns when its body is statements with no
// return and then `return function ... end` (a factory), else null.
FuncBody* singleReturnedClosure(const FuncBody& fn);

// What a call of a library function likely gives (Unknown when nothing is
// guessed): a library global may have been replaced, so only a guess.
// `builtin` names the math or bit function the callee likely is, if any.
InferredType stdlibCallHint(const Expr& callee, std::string_view builtin);

} // namespace brasslua
