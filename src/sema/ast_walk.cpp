#include "ast_walk.h"

namespace brasslua {

namespace {

void walkStmts(std::vector<StmtPtr>& stmts, const std::function<void(FuncBody&)>& fn);

void walkFunc(FuncBody& body, const std::function<void(FuncBody&)>& fn) {
    fn(body);
    walkStmts(body.body, fn);
}

void walkExpr(Expr& expr, const std::function<void(FuncBody&)>& fn) {
    std::visit(
        [&](auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, UnaryExpr>) {
                walkExpr(*e.operand, fn);
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                walkExpr(*e.lhs, fn);
                walkExpr(*e.rhs, fn);
            } else if constexpr (std::is_same_v<T, TableExpr>) {
                for (auto& f : e.fields) {
                    if (f.key) walkExpr(*f.key, fn);
                    if (f.value) walkExpr(*f.value, fn);
                }
            } else if constexpr (std::is_same_v<T, IndexExpr>) {
                walkExpr(*e.table, fn);
                walkExpr(*e.index, fn);
            } else if constexpr (std::is_same_v<T, FieldExpr>) {
                walkExpr(*e.table, fn);
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                walkExpr(*e.callee, fn);
                for (auto& a : e.args) walkExpr(*a, fn);
            } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
                walkExpr(*e.object, fn);
                for (auto& a : e.args) walkExpr(*a, fn);
            } else if constexpr (std::is_same_v<T, FuncExpr>) {
                walkFunc(e.func, fn);
            }
        },
        expr.node);
}

void walkStmts(std::vector<StmtPtr>& stmts, const std::function<void(FuncBody&)>& fn) {
    for (auto& sp : stmts) {
        std::visit(
            [&](auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, LocalStmt>) {
                    for (auto& e : s.init) walkExpr(*e, fn);
                } else if constexpr (std::is_same_v<T, AssignStmt>) {
                    for (auto& e : s.targets) walkExpr(*e, fn);
                    for (auto& e : s.values) walkExpr(*e, fn);
                } else if constexpr (std::is_same_v<T, ExprStmt>) {
                    walkExpr(*s.expr, fn);
                } else if constexpr (std::is_same_v<T, BlockStmt>) {
                    walkStmts(s.body, fn);
                } else if constexpr (std::is_same_v<T, IfStmt>) {
                    for (auto& br : s.branches) {
                        walkExpr(*br.condition, fn);
                        walkStmts(br.body, fn);
                    }
                    walkStmts(s.else_body, fn);
                } else if constexpr (std::is_same_v<T, WhileStmt>) {
                    walkExpr(*s.condition, fn);
                    walkStmts(s.body, fn);
                } else if constexpr (std::is_same_v<T, RepeatStmt>) {
                    walkStmts(s.body, fn);
                    walkExpr(*s.until_cond, fn);
                } else if constexpr (std::is_same_v<T, NumericForStmt>) {
                    walkExpr(*s.start, fn);
                    walkExpr(*s.stop, fn);
                    if (s.step) walkExpr(*s.step, fn);
                    walkStmts(s.body, fn);
                } else if constexpr (std::is_same_v<T, GenericForStmt>) {
                    for (auto& e : s.iter_exprs) walkExpr(*e, fn);
                    walkStmts(s.body, fn);
                } else if constexpr (std::is_same_v<T, FuncDefStmt>) {
                    walkFunc(s.func, fn);
                } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                    for (auto& e : s.values) walkExpr(*e, fn);
                }
            },
            sp->node);
    }
}

bool stmtsHaveReturn(const std::vector<StmtPtr>& stmts);

bool stmtHasReturn(const Stmt& stmt) {
    if (std::holds_alternative<ReturnStmt>(stmt.node)) return true;
    if (const auto* bs = std::get_if<BlockStmt>(&stmt.node)) {
        return stmtsHaveReturn(bs->body);
    } else if (const auto* ifs = std::get_if<IfStmt>(&stmt.node)) {
        for (const auto& br : ifs->branches) {
            if (stmtsHaveReturn(br.body)) return true;
        }
        return stmtsHaveReturn(ifs->else_body);
    } else if (const auto* ws = std::get_if<WhileStmt>(&stmt.node)) {
        return stmtsHaveReturn(ws->body);
    } else if (const auto* rs = std::get_if<RepeatStmt>(&stmt.node)) {
        return stmtsHaveReturn(rs->body);
    } else if (const auto* nfs = std::get_if<NumericForStmt>(&stmt.node)) {
        return stmtsHaveReturn(nfs->body);
    } else if (const auto* gfs = std::get_if<GenericForStmt>(&stmt.node)) {
        return stmtsHaveReturn(gfs->body);
    }
    return false;
}

bool stmtsHaveReturn(const std::vector<StmtPtr>& stmts) {
    for (const auto& sp : stmts) {
        if (stmtHasReturn(*sp)) return true;
    }
    return false;
}

} // namespace

void forEachFuncBody(Chunk& chunk, const std::function<void(FuncBody&)>& fn) {
    walkStmts(chunk.body, fn);
}

FuncBody* singleReturnedClosure(const FuncBody& fn) {
    if (fn.body.empty()) return nullptr;
    for (size_t i = 0; i + 1 < fn.body.size(); ++i) {
        if (stmtHasReturn(*fn.body[i])) return nullptr;
    }
    const auto* ret = std::get_if<ReturnStmt>(&fn.body.back()->node);
    if (!ret || ret->values.size() != 1) return nullptr;
    auto* fe = std::get_if<FuncExpr>(&ret->values[0]->node);
    return fe ? const_cast<FuncBody*>(&fe->func) : nullptr;
}

} // namespace brasslua
