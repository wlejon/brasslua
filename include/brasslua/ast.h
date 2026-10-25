#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace brasslua {

struct SourceLoc {
    uint32_t line = 1;
    uint32_t col = 1;
};

enum class InferredType : uint8_t {
    Unknown = 0,
    Nil,
    Bool,
    Int64,
    Float64,
    Number, // either Int64 or Float64 (unboxes to f64)
    String,
    Table,
    Function,
    Any,
};

enum class BinOp : uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    IDiv,
    Mod,
    Pow,
    Concat,
    Band,
    Bor,
    Bxor,
    Shl,
    Shr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
};

enum class UnOp : uint8_t {
    Neg,
    Not,
    Len,
    Bnot,
};

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

struct NilExpr {};
struct BoolExpr { bool value = false; };
struct IntExpr { int64_t value = 0; };
struct FloatExpr { double value = 0.0; };
struct StringExpr {
    std::string value;
    uint32_t intern_id = 0;
};
struct VarargExpr {};

struct IdentExpr {
    std::string name;
    uint32_t intern_id = 0;
};

struct UnaryExpr {
    UnOp op;
    ExprPtr operand;
};

struct BinaryExpr {
    BinOp op;
    ExprPtr lhs;
    ExprPtr rhs;
};

struct TableField {
    ExprPtr key;       // null for positional list element; StringExpr for record field
    std::string field_name; // non-empty for `name = expr`
    uint32_t intern_id = 0;
    int32_t slot_hint = -1;
    ExprPtr value;
};

struct TableExpr {
    std::vector<TableField> fields;
    uint32_t array_count = 0;
    uint32_t hash_count = 0;
};

struct IndexExpr {
    ExprPtr table;
    ExprPtr index;
};

struct FieldExpr {
    ExprPtr table;
    std::string field;
    uint32_t intern_id = 0;
    int32_t slot_hint = -1;
};

struct CallExpr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

struct MethodCallExpr {
    ExprPtr object;
    std::string method;
    uint32_t intern_id = 0;
    int32_t slot_hint = -1;
    std::vector<ExprPtr> args;
};

struct FuncBody {
    std::vector<std::string> params;
    bool is_vararg = false;
    std::vector<StmtPtr> body;
    std::vector<InferredType> param_types;
    InferredType return_type = InferredType::Unknown;
    bool can_unbox = false;
};

struct FuncExpr {
    FuncBody func;
};

struct Expr {
    using Variant = std::variant<
        NilExpr,
        BoolExpr,
        IntExpr,
        FloatExpr,
        StringExpr,
        VarargExpr,
        IdentExpr,
        UnaryExpr,
        BinaryExpr,
        TableExpr,
        IndexExpr,
        FieldExpr,
        CallExpr,
        MethodCallExpr,
        FuncExpr
    >;

    Variant node;
    SourceLoc loc;
    InferredType inferred_type = InferredType::Unknown;

    template <typename T>
    explicit Expr(T&& v, SourceLoc l = {})
        : node(std::forward<T>(v)), loc(l) {}
};

// ---- Statements ----

struct LocalAttr {
    std::string name;
    bool is_const = false;
    bool is_close = false;
    InferredType inferred_type = InferredType::Unknown;
};

struct LocalStmt {
    std::vector<LocalAttr> vars;
    std::vector<ExprPtr> init;
};

struct AssignStmt {
    std::vector<ExprPtr> targets; // IdentExpr, IndexExpr, or FieldExpr
    std::vector<ExprPtr> values;
};

struct ExprStmt {
    ExprPtr expr;
};

struct BlockStmt {
    std::vector<StmtPtr> body;
};

struct IfBranch {
    ExprPtr condition;
    std::vector<StmtPtr> body;
};

struct IfStmt {
    std::vector<IfBranch> branches;
    std::vector<StmtPtr> else_body;
};

struct WhileStmt {
    ExprPtr condition;
    std::vector<StmtPtr> body;
};

struct RepeatStmt {
    std::vector<StmtPtr> body;
    ExprPtr until_cond;
};

struct NumericForStmt {
    std::string var_name;
    ExprPtr start;
    ExprPtr stop;
    ExprPtr step; // may be null (default 1)
    std::vector<StmtPtr> body;
    bool is_int_loop = false;
};

struct GenericForStmt {
    std::vector<std::string> var_names;
    std::vector<ExprPtr> iter_exprs;
    std::vector<StmtPtr> body;
};

struct FuncDefStmt {
    std::vector<std::string> path; // e.g. {"a", "b"} for function a.b()
    std::string method;            // non-empty for function a.b:m()
    bool is_local = false;
    FuncBody func;
};

struct ReturnStmt {
    std::vector<ExprPtr> values;
};

struct BreakStmt {};

struct GotoStmt {
    std::string label;
};

struct LabelStmt {
    std::string label;
};

struct Stmt {
    using Variant = std::variant<
        LocalStmt,
        AssignStmt,
        ExprStmt,
        BlockStmt,
        IfStmt,
        WhileStmt,
        RepeatStmt,
        NumericForStmt,
        GenericForStmt,
        FuncDefStmt,
        ReturnStmt,
        BreakStmt,
        GotoStmt,
        LabelStmt
    >;

    Variant node;
    SourceLoc loc;

    template <typename T>
    explicit Stmt(T&& v, SourceLoc l = {})
        : node(std::forward<T>(v)), loc(l) {}
};

struct Chunk {
    std::string source_name;
    std::vector<StmtPtr> body;
};

} // namespace brasslua
