#include "brasslua/runtime.h"
#include "lex/lexer.h"

#include <iostream>
#include <string>

namespace brasslua::tests {

namespace {

#define EXPECT_TRUE(cond, msg)                                                      \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << (msg)  \
                      << "\n";                                                      \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

} // namespace

int runLexerParserTests() {
    int failures = 0;

    // 1. Lexer: hex integers, hex floats, long strings, comments, Lua 5.4 operators
    {
        std::string src = R"(
            -- short comment
            --[[ multi-line
                 comment ]]
            local a = 0x10
            local b = 0x1.8p1
            local s = [==[hello [[world]]]==]
            local q = a // 3 ~ b << 2 >> 1 .. s
        )";
        auto tokens = tokenizeLua(src, "=lex_test");
        EXPECT_TRUE(!tokens.empty() && tokens.back().kind == TokenKind::Eof, "lexer reaches EOF");
    }

    // 2. Parser: method call vs plain call AST distinction (Rule 6)
    {
        std::string src = R"(
            local x = obj:method(1, 2)
            local y = obj.method(1, 2)
        )";
        Chunk chunk = parseLuaSource(src, "=parse_test");
        EXPECT_TRUE(chunk.body.size() == 2, "parsed 2 statements");
        auto* ls1 = std::get_if<LocalStmt>(&chunk.body[0]->node);
        auto* ls2 = std::get_if<LocalStmt>(&chunk.body[1]->node);
        EXPECT_TRUE(ls1 && !ls1->init.empty() && std::holds_alternative<MethodCallExpr>(ls1->init[0]->node),
                    "obj:method(1, 2) parses as MethodCallExpr");
        EXPECT_TRUE(ls2 && !ls2->init.empty() && std::holds_alternative<CallExpr>(ls2->init[0]->node),
                    "obj.method(1, 2) parses as CallExpr");
    }

    // 3. Parser: Lua 5.4 <const> and <close> attributes
    {
        std::string src = R"(
            local a <const>, b <close> = 10, nil
        )";
        Chunk chunk = parseLuaSource(src, "=attr_test");
        EXPECT_TRUE(chunk.body.size() == 1, "parsed attribute local stmt");
        auto* ls = std::get_if<LocalStmt>(&chunk.body[0]->node);
        EXPECT_TRUE(ls && ls->vars.size() == 2 && ls->vars[0].is_const && ls->vars[1].is_close,
                    "parsed <const> and <close> attributes");
    }

    return failures;
}

} // namespace brasslua::tests
