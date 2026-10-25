#pragma once

#include "brasslua/ast.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace brasslua {

enum class TokenKind : uint8_t {
    Eof = 0,
    Identifier,
    IntLiteral,
    FloatLiteral,
    StringLiteral,

    // Keywords
    KwAnd,
    KwBreak,
    KwDo,
    KwElse,
    KwElseif,
    KwEnd,
    KwFalse,
    KwFor,
    KwFunction,
    KwGoto,
    KwIf,
    KwIn,
    KwLocal,
    KwNil,
    KwNot,
    KwOr,
    KwRepeat,
    KwReturn,
    KwThen,
    KwTrue,
    KwUntil,
    KwWhile,

    // Operators and punctuation
    Plus,       // +
    Minus,      // -
    Star,       // *
    Slash,      // /
    SlashSlash, // //
    Percent,    // %
    Caret,      // ^
    Amp,        // &
    Pipe,       // |
    Tilde,      // ~
    Shl,        // <<
    Shr,        // >>
    DotDot,     // ..
    Lt,         // <
    Le,         // <=
    Gt,         // >
    Ge,         // >=
    EqEq,       // ==
    TildeEq,    // ~=
    Hash,       // #
    LParen,     // (
    RParen,     // )
    LBrace,     // {
    RBrace,     // }
    LBracket,   // [
    RBracket,   // ]
    ColonColon, // ::
    Semicolon,  // ;
    Colon,      // :
    Comma,      // ,
    Dot,        // .
    DotDotDot,  // ...
    Assign,     // =
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string text;
    int64_t int_val = 0;
    double float_val = 0.0;
    SourceLoc loc;
};

const char* tokenKindName(TokenKind kind) noexcept;

class Lexer {
public:
    explicit Lexer(std::string_view source, std::string_view source_name = "=stdin");

    Token nextToken();
    std::vector<Token> tokenize();

private:
    char peek(size_t offset = 0) const noexcept;
    char advance() noexcept;
    bool isAtEnd() const noexcept { return pos_ >= source_.size(); }
    void skipWhitespaceAndComments();
    int checkLongBracket() const noexcept;
    std::string readLongBracketContent(int sep_count);
    Token readNumber(SourceLoc start_loc);
    Token readShortString(SourceLoc start_loc, char quote);
    Token readIdentifierOrKeyword(SourceLoc start_loc);

    std::string_view source_;
    std::string_view source_name_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;
};

std::vector<Token> tokenizeLua(std::string_view source, std::string_view source_name = "=stdin");

} // namespace brasslua
