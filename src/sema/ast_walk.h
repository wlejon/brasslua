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

} // namespace brasslua
