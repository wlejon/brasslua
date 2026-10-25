#include "lex/lexer.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>

namespace brasslua {

namespace {

bool isAlphaOrUnderscore(char c) noexcept {
    auto uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) || c == '_';
}

bool isAlnumOrUnderscore(char c) noexcept {
    auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

bool isDigitChar(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool isHexDigitChar(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexVal(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

void appendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x1FFFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x3FFFFFFu) {
        out.push_back(static_cast<char>(0xF8u | (cp >> 24)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 18) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xFCu | (cp >> 30)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 24) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 18) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

TokenKind matchKeyword(std::string_view text) noexcept {
    if (text == "and") return TokenKind::KwAnd;
    if (text == "break") return TokenKind::KwBreak;
    if (text == "do") return TokenKind::KwDo;
    if (text == "else") return TokenKind::KwElse;
    if (text == "elseif") return TokenKind::KwElseif;
    if (text == "end") return TokenKind::KwEnd;
    if (text == "false") return TokenKind::KwFalse;
    if (text == "for") return TokenKind::KwFor;
    if (text == "function") return TokenKind::KwFunction;
    if (text == "goto") return TokenKind::KwGoto;
    if (text == "if") return TokenKind::KwIf;
    if (text == "in") return TokenKind::KwIn;
    if (text == "local") return TokenKind::KwLocal;
    if (text == "nil") return TokenKind::KwNil;
    if (text == "not") return TokenKind::KwNot;
    if (text == "or") return TokenKind::KwOr;
    if (text == "repeat") return TokenKind::KwRepeat;
    if (text == "return") return TokenKind::KwReturn;
    if (text == "then") return TokenKind::KwThen;
    if (text == "true") return TokenKind::KwTrue;
    if (text == "until") return TokenKind::KwUntil;
    if (text == "while") return TokenKind::KwWhile;
    return TokenKind::Identifier;
}

} // namespace

const char* tokenKindName(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::Eof: return "<eof>";
        case TokenKind::Identifier: return "identifier";
        case TokenKind::IntLiteral: return "integer";
        case TokenKind::FloatLiteral: return "float";
        case TokenKind::StringLiteral: return "string";
        case TokenKind::KwAnd: return "and";
        case TokenKind::KwBreak: return "break";
        case TokenKind::KwDo: return "do";
        case TokenKind::KwElse: return "else";
        case TokenKind::KwElseif: return "elseif";
        case TokenKind::KwEnd: return "end";
        case TokenKind::KwFalse: return "false";
        case TokenKind::KwFor: return "for";
        case TokenKind::KwFunction: return "function";
        case TokenKind::KwGoto: return "goto";
        case TokenKind::KwIf: return "if";
        case TokenKind::KwIn: return "in";
        case TokenKind::KwLocal: return "local";
        case TokenKind::KwNil: return "nil";
        case TokenKind::KwNot: return "not";
        case TokenKind::KwOr: return "or";
        case TokenKind::KwRepeat: return "repeat";
        case TokenKind::KwReturn: return "return";
        case TokenKind::KwThen: return "then";
        case TokenKind::KwTrue: return "true";
        case TokenKind::KwUntil: return "until";
        case TokenKind::KwWhile: return "while";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::SlashSlash: return "//";
        case TokenKind::Percent: return "%";
        case TokenKind::Caret: return "^";
        case TokenKind::Amp: return "&";
        case TokenKind::Pipe: return "|";
        case TokenKind::Tilde: return "~";
        case TokenKind::Shl: return "<<";
        case TokenKind::Shr: return ">>";
        case TokenKind::DotDot: return "..";
        case TokenKind::Lt: return "<";
        case TokenKind::Le: return "<=";
        case TokenKind::Gt: return ">";
        case TokenKind::Ge: return ">=";
        case TokenKind::EqEq: return "==";
        case TokenKind::TildeEq: return "~=";
        case TokenKind::Hash: return "#";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::LBrace: return "{";
        case TokenKind::RBrace: return "}";
        case TokenKind::LBracket: return "[";
        case TokenKind::RBracket: return "]";
        case TokenKind::ColonColon: return "::";
        case TokenKind::Semicolon: return ";";
        case TokenKind::Colon: return ":";
        case TokenKind::Comma: return ",";
        case TokenKind::Dot: return ".";
        case TokenKind::DotDotDot: return "...";
        case TokenKind::Assign: return "=";
    }
    return "<unknown>";
}

Lexer::Lexer(std::string_view source, std::string_view source_name)
    : source_(source), source_name_(source_name) {
    // Skip UTF-8 BOM if present
    if (source_.size() >= 3 &&
        static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB &&
        static_cast<unsigned char>(source_[2]) == 0xBF) {
        pos_ += 3;
    }
    // Skip shebang (#!) on the first line
    if (pos_ + 1 < source_.size() && source_[pos_] == '#' && source_[pos_ + 1] == '!') {
        while (!isAtEnd() && peek() != '\n') {
            advance();
        }
    }
}

char Lexer::peek(size_t offset) const noexcept {
    if (pos_ + offset >= source_.size()) return '\0';
    return source_[pos_ + offset];
}

char Lexer::advance() noexcept {
    if (pos_ >= source_.size()) return '\0';
    char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

int Lexer::checkLongBracket() const noexcept {
    if (peek(0) != '[') return -1;
    size_t idx = 1;
    while (peek(idx) == '=') {
        ++idx;
    }
    if (peek(idx) == '[') {
        return static_cast<int>(idx - 1);
    }
    return -1;
}

std::string Lexer::readLongBracketContent(int sep_count) {
    // Consume opening `[` + `=`*sep_count + `[`
    for (int i = 0; i < sep_count + 2; ++i) {
        advance();
    }
    // Lua 5.4: A newline immediately following the opening long bracket is ignored.
    if (peek() == '\r' && peek(1) == '\n') {
        advance();
        advance();
    } else if (peek() == '\n' || peek() == '\r') {
        advance();
    }

    std::string content;
    while (!isAtEnd()) {
        if (peek() == ']') {
            bool matched = true;
            for (int i = 1; i <= sep_count; ++i) {
                if (peek(static_cast<size_t>(i)) != '=') {
                    matched = false;
                    break;
                }
            }
            if (matched && peek(static_cast<size_t>(sep_count + 1)) == ']') {
                for (int i = 0; i < sep_count + 2; ++i) {
                    advance();
                }
                return content;
            }
        }
        char c = advance();
        if (c == '\r' && peek() == '\n') {
            advance();
            content.push_back('\n');
        } else if (c == '\r') {
            content.push_back('\n');
        } else {
            content.push_back(c);
        }
    }
    throw std::runtime_error("unfinished long string or comment in " + std::string(source_name_));
}

void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            advance();
            continue;
        }
        if (c == '-' && peek(1) == '-') {
            advance();
            advance();
            int sep = checkLongBracket();
            if (sep >= 0) {
                readLongBracketContent(sep);
            } else {
                while (!isAtEnd() && peek() != '\n') {
                    advance();
                }
            }
            continue;
        }
        break;
    }
}

Token Lexer::readNumber(SourceLoc start_loc) {
    size_t start_pos = pos_;
    bool is_hex = false;
    bool is_float = false;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        is_hex = true;
        advance();
        advance();
        while (isHexDigitChar(peek())) {
            advance();
        }
        if (peek() == '.') {
            is_float = true;
            advance();
            while (isHexDigitChar(peek())) {
                advance();
            }
        }
        if (peek() == 'p' || peek() == 'P') {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            while (isDigitChar(peek())) {
                advance();
            }
        }
    } else {
        while (isDigitChar(peek())) {
            advance();
        }
        if (peek() == '.' && peek(1) != '.') {
            is_float = true;
            advance();
            while (isDigitChar(peek())) {
                advance();
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            while (isDigitChar(peek())) {
                advance();
            }
        }
    }

    std::string raw(source_.substr(start_pos, pos_ - start_pos));
    Token tok;
    tok.loc = start_loc;
    tok.text = raw;

    if (!is_float) {
        if (is_hex) {
            uint64_t uval = 0;
            for (size_t i = 2; i < raw.size(); ++i) {
                uval = (uval << 4) | static_cast<uint64_t>(hexVal(raw[i]));
            }
            tok.kind = TokenKind::IntLiteral;
            tok.int_val = static_cast<int64_t>(uval);
            tok.float_val = static_cast<double>(tok.int_val);
            return tok;
        }
        errno = 0;
        char* end_ptr = nullptr;
        long long iv = std::strtoll(raw.c_str(), &end_ptr, 10);
        if (errno == 0 && end_ptr == raw.c_str() + raw.size()) {
            tok.kind = TokenKind::IntLiteral;
            tok.int_val = static_cast<int64_t>(iv);
            tok.float_val = static_cast<double>(tok.int_val);
            return tok;
        }
    }

    char* end_ptr = nullptr;
    double dv = std::strtod(raw.c_str(), &end_ptr);
    tok.kind = TokenKind::FloatLiteral;
    tok.float_val = dv;
    tok.int_val = static_cast<int64_t>(dv);
    return tok;
}

Token Lexer::readShortString(SourceLoc start_loc, char quote) {
    advance(); // consume opening quote
    std::string value;
    while (!isAtEnd()) {
        char c = advance();
        if (c == quote) {
            Token tok;
            tok.kind = TokenKind::StringLiteral;
            tok.text = std::move(value);
            tok.loc = start_loc;
            return tok;
        }
        if (c == '\n' || c == '\r') {
            throw std::runtime_error("unfinished string literal at line " +
                                     std::to_string(start_loc.line));
        }
        if (c == '\\') {
            if (isAtEnd()) {
                throw std::runtime_error("unfinished escape in string literal");
            }
            char esc = advance();
            switch (esc) {
                case 'a': value.push_back('\a'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'v': value.push_back('\v'); break;
                case '\\': value.push_back('\\'); break;
                case '"': value.push_back('"'); break;
                case '\'': value.push_back('\''); break;
                case '\n': value.push_back('\n'); break;
                case '\r':
                    if (peek() == '\n') advance();
                    value.push_back('\n');
                    break;
                case 'z':
                    while (!isAtEnd()) {
                        char wc = peek();
                        if (wc == ' ' || wc == '\t' || wc == '\r' || wc == '\n' ||
                            wc == '\v' || wc == '\f') {
                            advance();
                        } else {
                            break;
                        }
                    }
                    break;
                case 'x': {
                    if (!isHexDigitChar(peek()) || !isHexDigitChar(peek(1))) {
                        throw std::runtime_error("hexadecimal digit expected after \\x");
                    }
                    int hi = hexVal(advance());
                    int lo = hexVal(advance());
                    value.push_back(static_cast<char>((hi << 4) | lo));
                    break;
                }
                case 'u': {
                    if (peek() != '{') {
                        throw std::runtime_error("missing '{' in \\u{xxx} escape");
                    }
                    advance(); // '{'
                    if (!isHexDigitChar(peek())) {
                        throw std::runtime_error("hexadecimal digit expected in \\u{xxx}");
                    }
                    uint32_t cp = 0;
                    while (isHexDigitChar(peek())) {
                        cp = (cp << 4) | static_cast<uint32_t>(hexVal(advance()));
                    }
                    if (peek() != '}') {
                        throw std::runtime_error("missing '}' in \\u{xxx} escape");
                    }
                    advance(); // '}'
                    appendUtf8Codepoint(value, cp);
                    break;
                }
                default: {
                    if (isDigitChar(esc)) {
                        int d = esc - '0';
                        if (isDigitChar(peek())) {
                            d = d * 10 + (advance() - '0');
                            if (isDigitChar(peek())) {
                                d = d * 10 + (advance() - '0');
                            }
                        }
                        if (d > 255) {
                            throw std::runtime_error("decimal escape too large");
                        }
                        value.push_back(static_cast<char>(static_cast<unsigned char>(d)));
                    } else {
                        value.push_back(esc);
                    }
                    break;
                }
            }
        } else {
            value.push_back(c);
        }
    }
    throw std::runtime_error("unfinished string literal at line " + std::to_string(start_loc.line));
}

Token Lexer::readIdentifierOrKeyword(SourceLoc start_loc) {
    size_t start_pos = pos_;
    while (isAlnumOrUnderscore(peek())) {
        advance();
    }
    std::string_view text = source_.substr(start_pos, pos_ - start_pos);
    Token tok;
    tok.kind = matchKeyword(text);
    tok.text = std::string(text);
    tok.loc = start_loc;
    return tok;
}

Token Lexer::nextToken() {
    skipWhitespaceAndComments();
    SourceLoc loc{line_, col_};
    if (isAtEnd()) {
        return Token{TokenKind::Eof, "", 0, 0.0, loc};
    }

    char c = peek();
    if (isAlphaOrUnderscore(c)) {
        return readIdentifierOrKeyword(loc);
    }
    if (isDigitChar(c) || (c == '.' && isDigitChar(peek(1)))) {
        return readNumber(loc);
    }
    if (c == '"' || c == '\'') {
        return readShortString(loc, c);
    }
    if (c == '[') {
        int sep = checkLongBracket();
        if (sep >= 0) {
            Token tok;
            tok.kind = TokenKind::StringLiteral;
            tok.text = readLongBracketContent(sep);
            tok.loc = loc;
            return tok;
        }
    }

    advance();
    auto makeTok = [&](TokenKind k, const char* s) {
        return Token{k, s, 0, 0.0, loc};
    };

    switch (c) {
        case '+': return makeTok(TokenKind::Plus, "+");
        case '-': return makeTok(TokenKind::Minus, "-");
        case '*': return makeTok(TokenKind::Star, "*");
        case '%': return makeTok(TokenKind::Percent, "%");
        case '^': return makeTok(TokenKind::Caret, "^");
        case '&': return makeTok(TokenKind::Amp, "&");
        case '|': return makeTok(TokenKind::Pipe, "|");
        case '#': return makeTok(TokenKind::Hash, "#");
        case '(': return makeTok(TokenKind::LParen, "(");
        case ')': return makeTok(TokenKind::RParen, ")");
        case '{': return makeTok(TokenKind::LBrace, "{");
        case '}': return makeTok(TokenKind::RBrace, "}");
        case '[': return makeTok(TokenKind::LBracket, "[");
        case ']': return makeTok(TokenKind::RBracket, "]");
        case ';': return makeTok(TokenKind::Semicolon, ";");
        case ',': return makeTok(TokenKind::Comma, ",");
        case '/':
            if (peek() == '/') {
                advance();
                return makeTok(TokenKind::SlashSlash, "//");
            }
            return makeTok(TokenKind::Slash, "/");
        case '~':
            if (peek() == '=') {
                advance();
                return makeTok(TokenKind::TildeEq, "~=");
            }
            return makeTok(TokenKind::Tilde, "~");
        case '=':
            if (peek() == '=') {
                advance();
                return makeTok(TokenKind::EqEq, "==");
            }
            return makeTok(TokenKind::Assign, "=");
        case '<':
            if (peek() == '=') {
                advance();
                return makeTok(TokenKind::Le, "<=");
            }
            if (peek() == '<') {
                advance();
                return makeTok(TokenKind::Shl, "<<");
            }
            return makeTok(TokenKind::Lt, "<");
        case '>':
            if (peek() == '=') {
                advance();
                return makeTok(TokenKind::Ge, ">=");
            }
            if (peek() == '>') {
                advance();
                return makeTok(TokenKind::Shr, ">>");
            }
            return makeTok(TokenKind::Gt, ">");
        case ':':
            if (peek() == ':') {
                advance();
                return makeTok(TokenKind::ColonColon, "::");
            }
            return makeTok(TokenKind::Colon, ":");
        case '.':
            if (peek() == '.') {
                advance();
                if (peek() == '.') {
                    advance();
                    return makeTok(TokenKind::DotDotDot, "...");
                }
                return makeTok(TokenKind::DotDot, "..");
            }
            return makeTok(TokenKind::Dot, ".");
        default:
            throw std::runtime_error(std::string("unexpected character '") + c +
                                     "' at line " + std::to_string(loc.line));
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        Token t = nextToken();
        TokenKind k = t.kind;
        tokens.push_back(std::move(t));
        if (k == TokenKind::Eof) break;
    }
    return tokens;
}

std::vector<Token> tokenizeLua(std::string_view source, std::string_view source_name) {
    Lexer lexer(source, source_name);
    return lexer.tokenize();
}

} // namespace brasslua
