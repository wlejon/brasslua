#include "brasslua/runtime.h"
#include "lex/lexer.h"

#include <stdexcept>
#include <utility>

namespace brasslua {

namespace {

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string_view source_name)
        : tokens_(std::move(tokens)), source_name_(source_name) {}

    Chunk parseChunk() {
        Chunk chunk;
        chunk.source_name = std::string(source_name_);
        chunk.body = parseBlockBody();
        expect(TokenKind::Eof, "expected <eof>");
        return chunk;
    }

private:
    const Token& peek(size_t offset = 0) const noexcept {
        size_t idx = pos_ + offset;
        if (idx >= tokens_.size()) return tokens_.back();
        return tokens_[idx];
    }

    const Token& advance() noexcept {
        const Token& tok = peek();
        if (pos_ + 1 < tokens_.size()) ++pos_;
        return tok;
    }

    bool check(TokenKind k, size_t offset = 0) const noexcept {
        return peek(offset).kind == k;
    }

    bool match(TokenKind k) noexcept {
        if (check(k)) {
            advance();
            return true;
        }
        return false;
    }

    const Token& expect(TokenKind k, const char* msg) {
        if (!check(k)) {
            throw std::runtime_error(std::string(source_name_) + ":" +
                                     std::to_string(peek().loc.line) + ":" +
                                     std::to_string(peek().loc.col) + ": " + msg +
                                     " (got '" + tokenKindName(peek().kind) + "')");
        }
        return advance();
    }

    bool isBlockFollow() const noexcept {
        switch (peek().kind) {
            case TokenKind::Eof:
            case TokenKind::KwEnd:
            case TokenKind::KwElse:
            case TokenKind::KwElseif:
            case TokenKind::KwUntil:
                return true;
            default:
                return false;
        }
    }

    std::vector<StmtPtr> parseBlockBody() {
        std::vector<StmtPtr> stmts;
        while (!isBlockFollow()) {
            if (match(TokenKind::Semicolon)) {
                continue;
            }
            if (check(TokenKind::KwReturn)) {
                stmts.push_back(parseReturnStmt());
                match(TokenKind::Semicolon);
                break;
            }
            stmts.push_back(parseStmt());
        }
        return stmts;
    }

    StmtPtr parseStmt() {
        SourceLoc loc = peek().loc;
        switch (peek().kind) {
            case TokenKind::KwLocal:
                return parseLocalStmt();
            case TokenKind::KwIf:
                return parseIfStmt();
            case TokenKind::KwWhile:
                return parseWhileStmt();
            case TokenKind::KwRepeat:
                return parseRepeatStmt();
            case TokenKind::KwFor:
                return parseForStmt();
            case TokenKind::KwFunction:
                return parseFuncDefStmt(false);
            case TokenKind::KwDo: {
                advance();
                BlockStmt blk;
                blk.body = parseBlockBody();
                expect(TokenKind::KwEnd, "expected 'end' to close 'do' block");
                return std::make_unique<Stmt>(std::move(blk), loc);
            }
            case TokenKind::KwBreak:
                advance();
                return std::make_unique<Stmt>(BreakStmt{}, loc);
            case TokenKind::KwGoto: {
                advance();
                const Token& label = expect(TokenKind::Identifier, "expected label name after 'goto'");
                return std::make_unique<Stmt>(GotoStmt{label.text}, loc);
            }
            case TokenKind::ColonColon: {
                advance();
                const Token& label = expect(TokenKind::Identifier, "expected label name after '::'");
                expect(TokenKind::ColonColon, "expected '::' after label name");
                return std::make_unique<Stmt>(LabelStmt{label.text}, loc);
            }
            case TokenKind::KwReturn:
                return parseReturnStmt();
            default:
                return parseExprOrAssignStmt();
        }
    }

    StmtPtr parseReturnStmt() {
        SourceLoc loc = peek().loc;
        expect(TokenKind::KwReturn, "expected 'return'");
        ReturnStmt ret;
        if (!isBlockFollow() && !check(TokenKind::Semicolon)) {
            ret.values = parseExprList();
        }
        match(TokenKind::Semicolon);
        return std::make_unique<Stmt>(std::move(ret), loc);
    }

    StmtPtr parseLocalStmt() {
        SourceLoc loc = peek().loc;
        expect(TokenKind::KwLocal, "expected 'local'");
        if (check(TokenKind::KwFunction)) {
            return parseFuncDefStmt(true);
        }

        LocalStmt stmt;
        do {
            const Token& nameTok = expect(TokenKind::Identifier, "expected variable name after 'local'");
            LocalAttr attr;
            attr.name = nameTok.text;
            if (match(TokenKind::Lt)) {
                const Token& attrTok = expect(TokenKind::Identifier, "expected attribute name inside '<...>'");
                if (attrTok.text == "const") {
                    attr.is_const = true;
                } else if (attrTok.text == "close") {
                    attr.is_close = true;
                } else {
                    throw std::runtime_error("unknown local attribute '<" + attrTok.text + ">'");
                }
                expect(TokenKind::Gt, "expected '>' after local attribute");
            }
            stmt.vars.push_back(std::move(attr));
        } while (match(TokenKind::Comma));

        if (match(TokenKind::Assign)) {
            stmt.init = parseExprList();
        }
        return std::make_unique<Stmt>(std::move(stmt), loc);
    }

    StmtPtr parseIfStmt() {
        SourceLoc loc = peek().loc;
        expect(TokenKind::KwIf, "expected 'if'");
        IfStmt stmt;

        IfBranch first;
        first.condition = parseExpr();
        expect(TokenKind::KwThen, "expected 'then' after 'if' condition");
        first.body = parseBlockBody();
        stmt.branches.push_back(std::move(first));

        while (match(TokenKind::KwElseif)) {
            IfBranch branch;
            branch.condition = parseExpr();
            expect(TokenKind::KwThen, "expected 'then' after 'elseif' condition");
            branch.body = parseBlockBody();
            stmt.branches.push_back(std::move(branch));
        }

        if (match(TokenKind::KwElse)) {
            stmt.else_body = parseBlockBody();
        }
        expect(TokenKind::KwEnd, "expected 'end' to close 'if' statement");
        return std::make_unique<Stmt>(std::move(stmt), loc);
    }

    StmtPtr parseWhileStmt() {
        SourceLoc loc = peek().loc;
        expect(TokenKind::KwWhile, "expected 'while'");
        WhileStmt stmt;
        stmt.condition = parseExpr();
        expect(TokenKind::KwDo, "expected 'do' after 'while' condition");
        stmt.body = parseBlockBody();
        expect(TokenKind::KwEnd, "expected 'end' to close 'while' loop");
        return std::make_unique<Stmt>(std::move(stmt), loc);
    }

    StmtPtr parseRepeatStmt() {
        SourceLoc loc = peek().loc;
        expect(TokenKind::KwRepeat, "expected 'repeat'");
        RepeatStmt stmt;
        stmt.body = parseBlockBody();
        expect(TokenKind::KwUntil, "expected 'until' to close 'repeat' loop");
        stmt.until_cond = parseExpr();
        return std::make_unique<Stmt>(std::move(stmt), loc);
    }

    StmtPtr parseForStmt() {
        SourceLoc loc = peek().loc;
        expect(TokenKind::KwFor, "expected 'for'");
        const Token& firstVar = expect(TokenKind::Identifier, "expected loop variable name after 'for'");

        if (match(TokenKind::Assign)) {
            NumericForStmt stmt;
            stmt.var_name = firstVar.text;
            stmt.start = parseExpr();
            expect(TokenKind::Comma, "expected ',' after numeric 'for' start expression");
            stmt.stop = parseExpr();
            if (match(TokenKind::Comma)) {
                stmt.step = parseExpr();
            }
            expect(TokenKind::KwDo, "expected 'do' in numeric 'for' loop");
            stmt.body = parseBlockBody();
            expect(TokenKind::KwEnd, "expected 'end' to close numeric 'for' loop");
            return std::make_unique<Stmt>(std::move(stmt), loc);
        }

        GenericForStmt stmt;
        stmt.var_names.push_back(firstVar.text);
        while (match(TokenKind::Comma)) {
            const Token& nextVar = expect(TokenKind::Identifier, "expected variable name in generic 'for'");
            stmt.var_names.push_back(nextVar.text);
        }
        expect(TokenKind::KwIn, "expected 'in' in generic 'for' loop");
        stmt.iter_exprs = parseExprList();
        expect(TokenKind::KwDo, "expected 'do' in generic 'for' loop");
        stmt.body = parseBlockBody();
        expect(TokenKind::KwEnd, "expected 'end' to close generic 'for' loop");
        return std::make_unique<Stmt>(std::move(stmt), loc);
    }

    StmtPtr parseFuncDefStmt(bool is_local) {
        SourceLoc loc = peek().loc;
        expect(TokenKind::KwFunction, "expected 'function'");
        FuncDefStmt stmt;
        stmt.is_local = is_local;

        const Token& first = expect(TokenKind::Identifier, "expected function name");
        stmt.path.push_back(first.text);

        if (!is_local) {
            while (match(TokenKind::Dot)) {
                const Token& part = expect(TokenKind::Identifier, "expected field name after '.'");
                stmt.path.push_back(part.text);
            }
            if (match(TokenKind::Colon)) {
                const Token& meth = expect(TokenKind::Identifier, "expected method name after ':'");
                stmt.method = meth.text;
            }
        }

        stmt.func = parseFuncBody();
        return std::make_unique<Stmt>(std::move(stmt), loc);
    }

    FuncBody parseFuncBody() {
        FuncBody fn;
        expect(TokenKind::LParen, "expected '(' for function parameter list");
        if (!check(TokenKind::RParen)) {
            for (;;) {
                if (match(TokenKind::DotDotDot)) {
                    fn.is_vararg = true;
                    break;
                }
                const Token& param = expect(TokenKind::Identifier, "expected parameter name");
                fn.params.push_back(param.text);
                if (!match(TokenKind::Comma)) break;
            }
        }
        expect(TokenKind::RParen, "expected ')' after function parameters");
        fn.body = parseBlockBody();
        expect(TokenKind::KwEnd, "expected 'end' to close function body");
        return fn;
    }

    StmtPtr parseExprOrAssignStmt() {
        SourceLoc loc = peek().loc;
        ExprPtr first = parseSuffixedExpr();

        if (check(TokenKind::Comma) || check(TokenKind::Assign)) {
            AssignStmt assign;
            assign.targets.push_back(std::move(first));
            while (match(TokenKind::Comma)) {
                assign.targets.push_back(parseSuffixedExpr());
            }
            expect(TokenKind::Assign, "expected '=' in assignment statement");
            assign.values = parseExprList();
            return std::make_unique<Stmt>(std::move(assign), loc);
        }

        if (!std::holds_alternative<CallExpr>(first->node) &&
            !std::holds_alternative<MethodCallExpr>(first->node)) {
            throw std::runtime_error(std::string(source_name_) + ":" +
                                     std::to_string(loc.line) + ":" +
                                     std::to_string(loc.col) +
                                     ": syntax error: statement expression must be a function call or assignment");
        }
        return std::make_unique<Stmt>(ExprStmt{std::move(first)}, loc);
    }

    std::vector<ExprPtr> parseExprList() {
        std::vector<ExprPtr> list;
        list.push_back(parseExpr());
        while (match(TokenKind::Comma)) {
            list.push_back(parseExpr());
        }
        return list;
    }

    ExprPtr parseExpr() {
        return parseOrExpr();
    }

    // Precedence 1: `or`
    ExprPtr parseOrExpr() {
        ExprPtr lhs = parseAndExpr();
        while (check(TokenKind::KwOr)) {
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseAndExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{BinOp::Or, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    // Precedence 2: `and`
    ExprPtr parseAndExpr() {
        ExprPtr lhs = parseCmpExpr();
        while (check(TokenKind::KwAnd)) {
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseCmpExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{BinOp::And, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    // Precedence 3: `<`, `>`, `<=`, `>=`, `~=`, `==`
    ExprPtr parseCmpExpr() {
        ExprPtr lhs = parseBitOrExpr();
        for (;;) {
            BinOp op;
            switch (peek().kind) {
                case TokenKind::Lt: op = BinOp::Lt; break;
                case TokenKind::Gt: op = BinOp::Gt; break;
                case TokenKind::Le: op = BinOp::Le; break;
                case TokenKind::Ge: op = BinOp::Ge; break;
                case TokenKind::TildeEq: op = BinOp::Ne; break;
                case TokenKind::EqEq: op = BinOp::Eq; break;
                default: return lhs;
            }
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseBitOrExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{op, std::move(lhs), std::move(rhs)}, loc);
        }
    }

    // Precedence 4: `|`
    ExprPtr parseBitOrExpr() {
        ExprPtr lhs = parseBitXorExpr();
        while (check(TokenKind::Pipe)) {
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseBitXorExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{BinOp::Bor, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    // Precedence 5: `~` (binary XOR)
    ExprPtr parseBitXorExpr() {
        ExprPtr lhs = parseBitAndExpr();
        while (check(TokenKind::Tilde)) {
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseBitAndExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{BinOp::Bxor, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    // Precedence 6: `&`
    ExprPtr parseBitAndExpr() {
        ExprPtr lhs = parseShiftExpr();
        while (check(TokenKind::Amp)) {
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseShiftExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{BinOp::Band, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    // Precedence 7: `<<`, `>>`
    ExprPtr parseShiftExpr() {
        ExprPtr lhs = parseConcatExpr();
        while (check(TokenKind::Shl) || check(TokenKind::Shr)) {
            BinOp op = check(TokenKind::Shl) ? BinOp::Shl : BinOp::Shr;
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseConcatExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{op, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    // Precedence 8: `..` (right-associative!)
    ExprPtr parseConcatExpr() {
        ExprPtr lhs = parseAddExpr();
        if (check(TokenKind::DotDot)) {
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseConcatExpr();
            return std::make_unique<Expr>(BinaryExpr{BinOp::Concat, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    // Precedence 9: `+`, `-`
    ExprPtr parseAddExpr() {
        ExprPtr lhs = parseMulExpr();
        while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
            BinOp op = check(TokenKind::Plus) ? BinOp::Add : BinOp::Sub;
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseMulExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{op, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    // Precedence 10: `*`, `/`, `//`, `%`
    ExprPtr parseMulExpr() {
        ExprPtr lhs = parseUnaryExpr();
        for (;;) {
            BinOp op;
            switch (peek().kind) {
                case TokenKind::Star: op = BinOp::Mul; break;
                case TokenKind::Slash: op = BinOp::Div; break;
                case TokenKind::SlashSlash: op = BinOp::IDiv; break;
                case TokenKind::Percent: op = BinOp::Mod; break;
                default: return lhs;
            }
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseUnaryExpr();
            lhs = std::make_unique<Expr>(BinaryExpr{op, std::move(lhs), std::move(rhs)}, loc);
        }
    }

    // Precedence 11: unary `not`, `#`, `-`, `~`
    ExprPtr parseUnaryExpr() {
        if (check(TokenKind::KwNot) || check(TokenKind::Hash) ||
            check(TokenKind::Minus) || check(TokenKind::Tilde)) {
            UnOp op = UnOp::Neg;
            switch (peek().kind) {
                case TokenKind::KwNot: op = UnOp::Not; break;
                case TokenKind::Hash: op = UnOp::Len; break;
                case TokenKind::Minus: op = UnOp::Neg; break;
                case TokenKind::Tilde: op = UnOp::Bnot; break;
                default: break;
            }
            SourceLoc loc = advance().loc;
            ExprPtr operand = parseUnaryExpr();
            return std::make_unique<Expr>(UnaryExpr{op, std::move(operand)}, loc);
        }
        return parsePowExpr();
    }

    // Precedence 12: `^` (right-associative, binds tighter than unary!)
    ExprPtr parsePowExpr() {
        ExprPtr lhs = parseSuffixedExpr();
        if (check(TokenKind::Caret)) {
            SourceLoc loc = advance().loc;
            ExprPtr rhs = parseUnaryExpr();
            return std::make_unique<Expr>(BinaryExpr{BinOp::Pow, std::move(lhs), std::move(rhs)}, loc);
        }
        return lhs;
    }

    std::vector<ExprPtr> parseCallArgs() {
        std::vector<ExprPtr> args;
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) {
                args = parseExprList();
            }
            expect(TokenKind::RParen, "expected ')' after call arguments");
            return args;
        }
        if (check(TokenKind::StringLiteral)) {
            const Token& s = advance();
            args.push_back(std::make_unique<Expr>(StringExpr{s.text, 0}, s.loc));
            return args;
        }
        if (check(TokenKind::LBrace)) {
            args.push_back(parseTableExpr());
            return args;
        }
        throw std::runtime_error("expected function call arguments");
    }

    ExprPtr parseSuffixedExpr() {
        ExprPtr primary = parsePrimaryExpr();
        for (;;) {
            SourceLoc loc = peek().loc;
            if (match(TokenKind::Dot)) {
                const Token& fld = expect(TokenKind::Identifier, "expected field name after '.'");
                FieldExpr fe;
                fe.table = std::move(primary);
                fe.field = fld.text;
                primary = std::make_unique<Expr>(std::move(fe), loc);
            } else if (match(TokenKind::LBracket)) {
                ExprPtr idx = parseExpr();
                expect(TokenKind::RBracket, "expected ']' after index expression");
                IndexExpr ie;
                ie.table = std::move(primary);
                ie.index = std::move(idx);
                primary = std::make_unique<Expr>(std::move(ie), loc);
            } else if (match(TokenKind::Colon)) {
                const Token& meth = expect(TokenKind::Identifier, "expected method name after ':'");
                MethodCallExpr mce;
                mce.object = std::move(primary);
                mce.method = meth.text;
                mce.args = parseCallArgs();
                primary = std::make_unique<Expr>(std::move(mce), loc);
            } else if (check(TokenKind::LParen) || check(TokenKind::StringLiteral) ||
                       check(TokenKind::LBrace)) {
                CallExpr ce;
                ce.callee = std::move(primary);
                ce.args = parseCallArgs();
                primary = std::make_unique<Expr>(std::move(ce), loc);
            } else {
                break;
            }
        }
        return primary;
    }

    ExprPtr parsePrimaryExpr() {
        SourceLoc loc = peek().loc;
        switch (peek().kind) {
            case TokenKind::KwNil:
                advance();
                return std::make_unique<Expr>(NilExpr{}, loc);
            case TokenKind::KwFalse:
                advance();
                return std::make_unique<Expr>(BoolExpr{false}, loc);
            case TokenKind::KwTrue:
                advance();
                return std::make_unique<Expr>(BoolExpr{true}, loc);
            case TokenKind::IntLiteral: {
                int64_t val = advance().int_val;
                return std::make_unique<Expr>(IntExpr{val}, loc);
            }
            case TokenKind::FloatLiteral: {
                double val = advance().float_val;
                return std::make_unique<Expr>(FloatExpr{val}, loc);
            }
            case TokenKind::StringLiteral: {
                std::string text = advance().text;
                return std::make_unique<Expr>(StringExpr{std::move(text), 0}, loc);
            }
            case TokenKind::DotDotDot:
                advance();
                return std::make_unique<Expr>(VarargExpr{}, loc);
            case TokenKind::Identifier: {
                std::string name = advance().text;
                return std::make_unique<Expr>(IdentExpr{std::move(name), 0}, loc);
            }
            case TokenKind::LParen: {
                advance();
                ExprPtr inner = parseExpr();
                expect(TokenKind::RParen, "expected ')' after parenthesized expression");
                inner->parenthesized = true;
                return inner;
            }
            case TokenKind::LBrace:
                return parseTableExpr();
            case TokenKind::KwFunction: {
                advance();
                FuncExpr fe;
                fe.func = parseFuncBody();
                return std::make_unique<Expr>(std::move(fe), loc);
            }
            default:
                throw std::runtime_error(std::string(source_name_) + ":" +
                                         std::to_string(loc.line) + ":" +
                                         std::to_string(loc.col) +
                                         ": unexpected token '" + tokenKindName(peek().kind) +
                                         "' in expression");
        }
    }

    ExprPtr parseTableExpr() {
        SourceLoc loc = peek().loc;
        expect(TokenKind::LBrace, "expected '{'");
        TableExpr tbl;
        while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
            TableField field;
            if (match(TokenKind::LBracket)) {
                field.key = parseExpr();
                expect(TokenKind::RBracket, "expected ']' after table field key");
                expect(TokenKind::Assign, "expected '=' after '[key]' in table constructor");
                field.value = parseExpr();
                if (auto* se = std::get_if<StringExpr>(&field.key->node)) {
                    field.field_name = se->value;
                }
                ++tbl.hash_count;
            } else if (check(TokenKind::Identifier) && check(TokenKind::Assign, 1)) {
                SourceLoc keyLoc = peek().loc;
                std::string name = advance().text;
                advance(); // '='
                field.field_name = name;
                field.key = std::make_unique<Expr>(StringExpr{name, 0}, keyLoc);
                field.value = parseExpr();
                ++tbl.hash_count;
            } else {
                field.value = parseExpr();
                ++tbl.array_count;
            }
            tbl.fields.push_back(std::move(field));
            if (!match(TokenKind::Comma) && !match(TokenKind::Semicolon)) {
                break;
            }
        }
        expect(TokenKind::RBrace, "expected '}' to close table constructor");
        return std::make_unique<Expr>(std::move(tbl), loc);
    }

    std::vector<Token> tokens_;
    std::string_view source_name_;
    size_t pos_ = 0;
};

} // namespace

Chunk parseLuaSource(std::string_view source, std::string_view source_name) {
    Lexer lexer(source, source_name);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens), source_name);
    return parser.parseChunk();
}

} // namespace brasslua
