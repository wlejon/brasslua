// Lua 5.4's pattern matching, after lstrlib.c (Copyright (C) 1994-2023
// Lua.org, PUC-Rio, MIT license): the same matcher, limits and messages.

#include "runtime/lua_pattern.h"

#include <cctype>
#include <cstring>
#include <string>

namespace brasslua {

namespace {

constexpr char kEsc = '%';

[[noreturn]] void patternError(const std::string& msg) {
    throw LuaRuntimeError(msg);
}

bool matchClass(int c, int cl) noexcept {
    int res;
    switch (std::tolower(cl)) {
        case 'a': res = std::isalpha(c); break;
        case 'c': res = std::iscntrl(c); break;
        case 'd': res = std::isdigit(c); break;
        case 'g': res = std::isgraph(c); break;
        case 'l': res = std::islower(c); break;
        case 'p': res = std::ispunct(c); break;
        case 's': res = std::isspace(c); break;
        case 'u': res = std::isupper(c); break;
        case 'w': res = std::isalnum(c); break;
        case 'x': res = std::isxdigit(c); break;
        default: return cl == c;
    }
    if (std::isupper(cl)) res = !res;
    return res != 0;
}

// p is at the '[' of a class whose ']' is at ec.
bool matchBracketClass(int c, const char* p, const char* ec) noexcept {
    bool sig = true;
    if (*(p + 1) == '^') {
        sig = false;
        ++p;
    }
    while (++p < ec) {
        if (*p == kEsc) {
            ++p;
            if (matchClass(c, static_cast<unsigned char>(*p))) return sig;
        } else if (*(p + 1) == '-' && p + 2 < ec) {
            p += 2;
            if (static_cast<unsigned char>(*(p - 2)) <= c && c <= static_cast<unsigned char>(*p)) return sig;
        } else if (static_cast<unsigned char>(*p) == c) {
            return sig;
        }
    }
    return !sig;
}

} // namespace

bool patternHasNoSpecials(std::string_view pattern) noexcept {
    for (char c : pattern) {
        if (std::strchr("^$*+?.([%-", c) != nullptr && c != '\0') return false;
    }
    return true;
}

int LuaPatternMatch::checkCapture(int l) const {
    l -= '1';
    if (l < 0 || l >= level_ || capture_[l].len == kCapUnfinished) {
        patternError("invalid capture index %" + std::to_string(l + 1));
    }
    return l;
}

int LuaPatternMatch::captureToClose() const {
    for (int level = level_ - 1; level >= 0; --level) {
        if (capture_[level].len == kCapUnfinished) return level;
    }
    patternError("invalid pattern capture");
}

const char* LuaPatternMatch::classEnd(const char* p) const {
    switch (*p++) {
        case kEsc:
            if (p == p_end_) patternError("malformed pattern (ends with '%')");
            return p + 1;
        case '[':
            if (*p == '^') ++p;
            do { // look for a ']'
                if (p == p_end_) patternError("malformed pattern (missing ']')");
                if (*(p++) == kEsc && p < p_end_) ++p; // skip escapes (e.g. '%]')
            } while (*p != ']');
            return p + 1;
        default:
            return p;
    }
}

bool LuaPatternMatch::singleMatch(const char* s, const char* p, const char* ep) const noexcept {
    if (s >= src_end_) return false;
    const int c = static_cast<unsigned char>(*s);
    switch (*p) {
        case '.': return true;
        case kEsc: return matchClass(c, static_cast<unsigned char>(*(p + 1)));
        case '[': return matchBracketClass(c, p, ep - 1);
        default: return static_cast<unsigned char>(*p) == c;
    }
}

const char* LuaPatternMatch::matchBalance(const char* s, const char* p) const {
    if (p >= p_end_ - 1) patternError("malformed pattern (missing arguments to '%b')");
    if (s >= src_end_ || *s != *p) return nullptr;
    const char b = *p;
    const char e = *(p + 1);
    int cont = 1;
    while (++s < src_end_) {
        if (*s == e) {
            if (--cont == 0) return s + 1;
        } else if (*s == b) {
            ++cont;
        }
    }
    return nullptr; // the string ends out of balance
}

const char* LuaPatternMatch::maxExpand(const char* s, const char* p, const char* ep) {
    ptrdiff_t i = 0; // the most repetitions of the item
    while (singleMatch(s + i, p, ep)) ++i;
    for (; i >= 0; --i) { // the rest of the pattern after as many as can be
        if (const char* res = match(s + i, ep + 1)) return res;
    }
    return nullptr;
}

const char* LuaPatternMatch::minExpand(const char* s, const char* p, const char* ep) {
    for (;;) {
        if (const char* res = match(s, ep + 1)) return res;
        if (!singleMatch(s, p, ep)) return nullptr;
        ++s; // one more repetition
    }
}

const char* LuaPatternMatch::startCapture(const char* s, const char* p, ptrdiff_t what) {
    if (level_ >= kMaxCaptures) patternError("too many captures");
    capture_[level_].init = s;
    capture_[level_].len = what;
    ++level_;
    const char* res = match(s, p);
    if (res == nullptr) --level_; // undo the capture
    return res;
}

const char* LuaPatternMatch::endCapture(const char* s, const char* p) {
    const int l = captureToClose();
    capture_[l].len = s - capture_[l].init;
    const char* res = match(s, p);
    if (res == nullptr) capture_[l].len = kCapUnfinished;
    return res;
}

const char* LuaPatternMatch::matchCapture(const char* s, int l) const {
    l = checkCapture(l);
    const size_t len = static_cast<size_t>(capture_[l].len);
    if (static_cast<size_t>(src_end_ - s) >= len && std::memcmp(capture_[l].init, s, len) == 0) return s + len;
    return nullptr;
}

const char* LuaPatternMatch::match(const char* s, const char* p) {
    if (matchdepth_-- == 0) patternError("pattern too complex");
init: // a goto for tail calls, as Lua's
    if (p != p_end_) {
        switch (*p) {
            case '(':
                if (*(p + 1) == ')') {
                    s = startCapture(s, p + 2, kCapPosition);
                } else {
                    s = startCapture(s, p + 1, kCapUnfinished);
                }
                break;
            case ')':
                s = endCapture(s, p + 1);
                break;
            case '$':
                if (p + 1 != p_end_) goto dflt; // not the pattern's last character
                s = (s == src_end_) ? s : nullptr;
                break;
            case kEsc:
                switch (*(p + 1)) {
                    case 'b':
                        s = matchBalance(s, p + 2);
                        if (s != nullptr) {
                            p += 4;
                            goto init;
                        }
                        break;
                    case 'f': {
                        p += 2;
                        if (*p != '[') patternError("missing '[' after '%f' in pattern");
                        const char* ep = classEnd(p);
                        const char previous = (s == src_init_) ? '\0' : *(s - 1);
                        const char current = (s < src_end_) ? *s : '\0';
                        if (!matchBracketClass(static_cast<unsigned char>(previous), p, ep - 1) &&
                            matchBracketClass(static_cast<unsigned char>(current), p, ep - 1)) {
                            p = ep;
                            goto init;
                        }
                        s = nullptr;
                        break;
                    }
                    case '0': case '1': case '2': case '3': case '4':
                    case '5': case '6': case '7': case '8': case '9':
                        s = matchCapture(s, static_cast<unsigned char>(*(p + 1)));
                        if (s != nullptr) {
                            p += 2;
                            goto init;
                        }
                        break;
                    default:
                        goto dflt;
                }
                break;
            default:
            dflt: {
                const char* ep = classEnd(p); // at the optional suffix
                if (!singleMatch(s, p, ep)) {
                    if (*ep == '*' || *ep == '?' || *ep == '-') { // may match empty
                        p = ep + 1;
                        goto init;
                    }
                    s = nullptr; // '+' or no suffix
                } else {
                    switch (*ep) {
                        case '?': {
                            const char* res = match(s + 1, ep + 1);
                            if (res != nullptr) {
                                s = res;
                            } else {
                                p = ep + 1;
                                goto init;
                            }
                            break;
                        }
                        case '+':
                            ++s; // one already matched
                            s = maxExpand(s, p, ep);
                            break;
                        case '*':
                            s = maxExpand(s, p, ep);
                            break;
                        case '-':
                            s = minExpand(s, p, ep);
                            break;
                        default:
                            ++s;
                            p = ep;
                            goto init;
                    }
                }
                break;
            }
        }
    }
    ++matchdepth_;
    return s;
}

ptrdiff_t LuaPatternMatch::capture(int i, const char* s, const char* e, const char** cap, int64_t* pos) const {
    if (i >= level_) {
        if (i != 0) patternError("invalid capture index %" + std::to_string(i + 1));
        *cap = s;
        return e - s;
    }
    const ptrdiff_t capl = capture_[i].len;
    *cap = capture_[i].init;
    if (capl == kCapUnfinished) patternError("unfinished capture");
    if (capl == kCapPosition) *pos = (capture_[i].init - src_init_) + 1;
    return capl;
}

uint64_t LuaPatternMatch::captureValue(RuntimeState& rt, int i, const char* s, const char* e) const {
    const char* cap = nullptr;
    int64_t pos = 0;
    const ptrdiff_t l = capture(i, s, e, &cap, &pos);
    if (l == kCapPosition) return Value::fromInt64(pos).rawBits();
    return rt.makeStringValue(std::string_view(cap, static_cast<size_t>(l)));
}

size_t LuaPatternMatch::captures(RuntimeState& rt, const char* s, const char* e, uint64_t* out) const {
    const int n = (level_ == 0 && s != nullptr) ? 1 : level_;
    for (int i = 0; i < n; ++i) out[i] = captureValue(rt, i, s, e);
    return static_cast<size_t>(n);
}

} // namespace brasslua
