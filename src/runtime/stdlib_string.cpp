// The string library, after Lua 5.4's lstrlib.c (Copyright (C) 1994-2023
// Lua.org, PUC-Rio, MIT license): the same arguments, results and messages.
// string.pack, packsize and unpack are stdlib_string_pack.cpp's.

#include "runtime/lua_pattern.h"
#include "runtime/stdlib_aux.h"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace brasslua {

namespace {

// posrelatI: a start position, 1 for anything before the string.
size_t posRelatI(int64_t pos, size_t len) noexcept {
    if (pos > 0) return static_cast<size_t>(pos);
    if (pos == 0) return 1;
    if (pos < -static_cast<int64_t>(len)) return 1;
    return len + static_cast<size_t>(pos) + 1;
}

// getendpos: an end position, clipped to [0, len].
size_t endPos(const Args& a, int64_t i, int64_t def, size_t len) {
    const int64_t pos = a.optInteger(i, def);
    if (pos > static_cast<int64_t>(len)) return len;
    if (pos >= 0) return static_cast<size_t>(pos);
    if (pos < -static_cast<int64_t>(len)) return 0;
    return len + static_cast<size_t>(pos) + 1;
}

uint64_t strLen(void*, const uint64_t* args, int64_t nargs) {
    Args a(args, nargs, "string.len");
    char nb[kNumberBufSize];
    return Value::fromInt64(static_cast<int64_t>(a.checkString(1, nb).size())).rawBits();
}

uint64_t strSub(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.sub");
    char nb[kNumberBufSize];
    std::string_view s = a.checkString(1, nb);
    size_t start = posRelatI(a.checkInteger(2), s.size());
    size_t end = endPos(a, 3, -1, s.size());
    if (start <= end) return rt.makeStringValue(s.substr(start - 1, end - start + 1));
    return rt.makeStringValue("");
}

uint64_t strReverse(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.reverse");
    char nb[kNumberBufSize];
    std::string_view s = a.checkString(1, nb);
    StrBuf b;
    char* p = b.prepare(s.size());
    for (size_t i = 0; i < s.size(); ++i) p[i] = s[s.size() - 1 - i];
    b.commit(s.size());
    return b.toValue(rt);
}

template <int (*Map)(int)>
uint64_t strMapBytes(const char* name, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, name);
    char nb[kNumberBufSize];
    std::string_view s = a.checkString(1, nb);
    StrBuf b;
    char* p = b.prepare(s.size());
    for (size_t i = 0; i < s.size(); ++i) p[i] = static_cast<char>(Map(static_cast<unsigned char>(s[i])));
    b.commit(s.size());
    return b.toValue(rt);
}

int toLowerByte(int c) { return std::tolower(c); }
int toUpperByte(int c) { return std::toupper(c); }

uint64_t strLower(void*, const uint64_t* args, int64_t nargs) {
    return strMapBytes<toLowerByte>("string.lower", args, nargs);
}

uint64_t strUpper(void*, const uint64_t* args, int64_t nargs) {
    return strMapBytes<toUpperByte>("string.upper", args, nargs);
}

uint64_t strRep(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.rep");
    char nb[kNumberBufSize], nb_sep[kNumberBufSize];
    std::string_view s = a.checkString(1, nb);
    int64_t n = a.checkInteger(2);
    std::string_view sep = a.optString(3, nb_sep, "");
    if (n <= 0) return rt.makeStringValue("");
    if (s.size() + sep.size() > static_cast<size_t>(INT_MAX) / static_cast<uint64_t>(n)) {
        throw LuaRuntimeError("resulting string too large");
    }
    StrBuf b;
    for (int64_t i = 0; i < n; ++i) {
        b.append(s);
        if (i + 1 < n) b.append(sep);
    }
    return b.toValue(rt);
}

uint64_t strByte(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.byte");
    char nb[kNumberBufSize];
    std::string_view s = a.checkString(1, nb);
    const int64_t pi = a.optInteger(2, 1);
    const size_t posi = posRelatI(pi, s.size());
    const size_t pose = endPos(a, 3, pi, s.size());
    if (posi > pose) return returnValues(rt, nullptr, 0);
    if (pose - posi >= static_cast<size_t>(INT_MAX)) throw LuaRuntimeError("string slice too long");
    const size_t n = pose - posi + 1;
    uint64_t small[16];
    std::unique_ptr<uint64_t[]> big;
    uint64_t* out = small;
    if (n > 16) {
        big.reset(new uint64_t[n]);
        out = big.get();
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = Value::fromInt64(static_cast<unsigned char>(s[posi + i - 1])).rawBits();
    }
    return returnValues(rt, out, n);
}

uint64_t strChar(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.char");
    StrBuf b;
    for (int64_t i = 1; i <= nargs; ++i) {
        const uint64_t c = static_cast<uint64_t>(a.checkInteger(i));
        if (c > UCHAR_MAX) a.error(i, "value out of range");
        b.push(static_cast<char>(c));
    }
    return b.toValue(rt);
}

// ---- string.format ----

constexpr const char* kFlagsF = "-+ #0"; // a A e E f F g G
constexpr const char* kFlagsX = "-#0";   // o x X
constexpr const char* kFlagsI = "-+ 0";  // d i
constexpr const char* kFlagsU = "-0";    // u
constexpr const char* kFlagsC = "-";     // c p s
constexpr size_t kMaxItem = 120;
constexpr size_t kMaxItemF = 110 + 308; // '%99.99f' of the largest double
constexpr size_t kMaxFormat = 32;

const char* get2Digits(const char* s) noexcept {
    if (std::isdigit(static_cast<unsigned char>(*s))) {
        ++s;
        if (std::isdigit(static_cast<unsigned char>(*s))) ++s;
    }
    return s;
}

void checkFormat(const char* form, const char* flags, bool precision) {
    const char* spec = form + 1; // after the '%'
    spec += std::strspn(spec, flags);
    if (*spec != '0') { // a width cannot start with '0'
        spec = get2Digits(spec);
        if (*spec == '.' && precision) {
            ++spec;
            spec = get2Digits(spec);
        }
    }
    if (!std::isalpha(static_cast<unsigned char>(*spec))) {
        throw LuaRuntimeError(std::string("invalid conversion specification: '") + form + "'");
    }
}

// Copies the item's flags, width, precision and conversion into form.
const char* getFormat(const char* strfrmt, char* form) {
    size_t len = std::strspn(strfrmt, "-+ #0" "123456789.");
    ++len; // the conversion
    if (len >= kMaxFormat - 10) throw LuaRuntimeError("invalid format string to 'format'");
    *(form++) = '%';
    std::memcpy(form, strfrmt, len);
    form[len] = '\0';
    return strfrmt + len - 1;
}

// Inserts a length modifier before the conversion.
void addLenMod(char* form, const char* lenmod) noexcept {
    const size_t l = std::strlen(form);
    const size_t lm = std::strlen(lenmod);
    const char spec = form[l - 1];
    std::memcpy(form + l - 1, lenmod, lm);
    form[l + lm - 1] = spec;
    form[l + lm] = '\0';
}

// '%a' as Lua writes it itself (lstrlib.c's num2straux, which Windows
// builds of Lua use): the fewest hex digits, "0x1.8p+0" rather than the C
// library's "0x1.8000000000000p+0".
int hexFloat(char* buff, size_t sz, double x) {
    if (x != x || x == HUGE_VAL || x == -HUGE_VAL) return std::snprintf(buff, sz, "%.14g", x);
    if (x == 0) return std::snprintf(buff, sz, "%.14gx0p+0", x); // "0" or "-0"
    int e = 0;
    double m = std::frexp(x, &e);
    int n = 0;
    if (m < 0) {
        buff[n++] = '-';
        m = -m;
    }
    buff[n++] = '0';
    buff[n++] = 'x';
    auto addDigit = [&](double v) {
        const double dd = std::floor(v);
        const int d = static_cast<int>(dd);
        buff[n++] = static_cast<char>(d < 10 ? d + '0' : d - 10 + 'a');
        return v - dd;
    };
    constexpr int kFirstDigitBits = (53 - 1) % 4 + 1; // the mantissa's bits before the point
    m = addDigit(m * (1 << kFirstDigitBits));
    e -= kFirstDigitBits;
    if (m > 0) {
        buff[n++] = '.';
        do {
            m = addDigit(m * 16);
        } while (m > 0);
    }
    n += std::snprintf(buff + n, sz - static_cast<size_t>(n), "p%+d", e);
    return n;
}

void addQuoted(StrBuf& b, std::string_view s) {
    b.push('"');
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\\' || c == '\n') {
            b.push('\\');
            b.push(c);
        } else if (std::iscntrl(static_cast<unsigned char>(c))) {
            char buf[10];
            const bool digit_next = i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]));
            int n = std::snprintf(buf, sizeof(buf), digit_next ? "\\%03d" : "\\%d", static_cast<unsigned char>(c));
            b.append(buf, static_cast<size_t>(n));
        } else {
            b.push(c);
        }
    }
    b.push('"');
}

void addLiteral(RuntimeState& rt, StrBuf& b, const Args& a, int64_t arg) {
    Value v = a.at(arg);
    if (v.isString()) {
        addQuoted(b, valueToStringView(rt, v));
    } else if (v.isNumeric()) {
        char* buf = b.prepare(kMaxItem);
        int nb = 0;
        if (v.isFloat()) {
            const double n = v.asDouble();
            if (n == HUGE_VAL) {
                nb = std::snprintf(buf, kMaxItem, "1e9999");
            } else if (n == -HUGE_VAL) {
                nb = std::snprintf(buf, kMaxItem, "-1e9999");
            } else if (n != n) {
                nb = std::snprintf(buf, kMaxItem, "(0/0)");
            } else {
                nb = hexFloat(buf, kMaxItem, n);
            }
        } else {
            const int64_t n = v.asInt64();
            nb = std::snprintf(buf, kMaxItem, n == INT64_MIN ? "0x%llx" : "%lld", static_cast<long long>(n));
        }
        b.commit(static_cast<size_t>(nb));
    } else if (v.isNil() || v.isBool()) {
        b.append(valueToStdString(rt, v));
    } else {
        a.error(arg, "value has no literal form");
    }
}

uint64_t strFormat(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.format");
    char nb_fmt[kNumberBufSize];
    std::string_view fmt = a.checkString(1, nb_fmt);
    const char* strfrmt = fmt.data();
    const char* const strfrmt_end = fmt.data() + fmt.size();
    int64_t arg = 1;
    StrBuf b;
    while (strfrmt < strfrmt_end) {
        if (*strfrmt != '%') {
            b.push(*strfrmt++);
            continue;
        }
        if (*++strfrmt == '%') {
            b.push(*strfrmt++);
            continue;
        }
        char form[kMaxFormat];
        if (++arg > nargs) a.error(arg, "no value");
        strfrmt = getFormat(strfrmt, form);
        int nb = 0;
        switch (*strfrmt++) {
            case 'c': {
                checkFormat(form, kFlagsC, false);
                const int64_t c = a.checkInteger(arg);
                char* buf = b.prepare(kMaxItem);
                nb = std::snprintf(buf, kMaxItem, form, static_cast<int>(c));
                break;
            }
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                const char conv = *(strfrmt - 1);
                const char* flags = (conv == 'd' || conv == 'i') ? kFlagsI : conv == 'u' ? kFlagsU : kFlagsX;
                const int64_t n = a.checkInteger(arg);
                checkFormat(form, flags, true);
                addLenMod(form, "ll");
                char* buf = b.prepare(kMaxItem);
                nb = std::snprintf(buf, kMaxItem, form, static_cast<long long>(n));
                break;
            }
            case 'a': case 'A': {
                checkFormat(form, kFlagsF, true);
                const double n = a.checkDouble(arg);
                if (form[1] != 'a' && form[1] != 'A') {
                    throw LuaRuntimeError("modifiers for format '%a'/'%A' not implemented");
                }
                char* buf = b.prepare(kMaxItem);
                nb = hexFloat(buf, kMaxItem, n);
                if (form[1] == 'A') {
                    for (int i = 0; i < nb; ++i) buf[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(buf[i])));
                }
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                const double n = a.checkDouble(arg);
                checkFormat(form, kFlagsF, true);
                char* buf = b.prepare(kMaxItemF);
                nb = std::snprintf(buf, kMaxItemF, form, n);
                break;
            }
            case 'p': {
                checkFormat(form, kFlagsC, false);
                Value v = a.at(arg);
                const bool is_object = v.isTable() || v.isFunction() || v.isCoroutine() || v.isUserdata() ||
                                       v.isString();
                const void* p = is_object ? v.asPointer() : nullptr;
                char* buf = b.prepare(kMaxItem);
                if (p == nullptr) {
                    form[std::strlen(form) - 1] = 's';
                    nb = std::snprintf(buf, kMaxItem, form, "(null)");
                } else {
                    nb = std::snprintf(buf, kMaxItem, form, p);
                }
                break;
            }
            case 'q': {
                if (form[2] != '\0') throw LuaRuntimeError("specifier '%q' cannot have modifiers");
                addLiteral(rt, b, a, arg);
                break;
            }
            case 's': {
                std::string s = valueToStdString(rt, a.at(arg));
                if (form[2] == '\0') { // no modifiers: the whole string
                    b.append(s);
                } else {
                    if (s.size() != std::strlen(s.c_str())) a.error(arg, "string contains zeros");
                    checkFormat(form, kFlagsC, true);
                    if (std::strchr(form, '.') == nullptr && s.size() >= 100) {
                        b.append(s); // no precision and too long to format
                    } else {
                        char* buf = b.prepare(kMaxItem);
                        nb = std::snprintf(buf, kMaxItem, form, s.c_str());
                    }
                }
                break;
            }
            default:
                throw LuaRuntimeError(std::string("invalid conversion '") + form + "' to 'format'");
        }
        b.commit(static_cast<size_t>(nb));
    }
    return b.toValue(rt);
}

// ---- find, match, gmatch, gsub ----

uint64_t findAux(const uint64_t* args, int64_t nargs, bool find) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, find ? "string.find" : "string.match");
    char nb_s[kNumberBufSize], nb_p[kNumberBufSize];
    std::string_view s = a.checkString(1, nb_s);
    std::string_view p = a.checkString(2, nb_p);
    const size_t init = posRelatI(a.optInteger(3, 1), s.size()) - 1;
    if (init > s.size()) return kNilBits; // starts after the string's end
    if (find && (a.at(4).isTruthy() || patternHasNoSpecials(p))) {
        const size_t pos = s.find(p, init);
        if (pos != std::string_view::npos) {
            const uint64_t res[2] = {Value::fromInt64(static_cast<int64_t>(pos + 1)).rawBits(),
                                     Value::fromInt64(static_cast<int64_t>(pos + p.size())).rawBits()};
            return returnValues(rt, res, 2);
        }
        return kNilBits;
    }
    const bool anchor = !p.empty() && p[0] == '^';
    if (anchor) p.remove_prefix(1);
    LuaPatternMatch ms(s, p);
    const char* s1 = s.data() + init;
    do {
        ms.reset();
        if (const char* e = ms.match(s1, p.data())) {
            uint64_t out[LuaPatternMatch::kMaxCaptures + 2];
            if (find) {
                out[0] = Value::fromInt64((s1 - s.data()) + 1).rawBits();
                out[1] = Value::fromInt64(e - s.data()).rawBits();
                const size_t n = ms.captures(rt, nullptr, nullptr, out + 2);
                return returnValues(rt, out, n + 2);
            }
            const size_t n = ms.captures(rt, s1, e, out);
            return returnValues(rt, out, n);
        }
    } while (s1++ < ms.subjectEnd() && !anchor);
    return kNilBits;
}

uint64_t strFind(void*, const uint64_t* args, int64_t nargs) {
    return findAux(args, nargs, true);
}

uint64_t strMatch(void*, const uint64_t* args, int64_t nargs) {
    return findAux(args, nargs, false);
}

// gmatch's iterator: upvalues are the subject, the pattern, the position
// to search from and the end of the last match (-1 for none), as offsets.
uint64_t strGmatchIter(void* cl_ptr, const uint64_t*, int64_t) {
    auto& rt = RuntimeState::current();
    auto* cl = static_cast<LuaClosure*>(cl_ptr);
    std::string_view s = valueToStringView(rt, Value(cl->upvalues[0]->value));
    std::string_view p = valueToStringView(rt, Value(cl->upvalues[1]->value));
    const int64_t pos = Value(cl->upvalues[2]->value).asInt64();
    const int64_t last = Value(cl->upvalues[3]->value).asInt64();
    LuaPatternMatch ms(s, p);
    const char* lastmatch = last < 0 ? nullptr : s.data() + last;
    for (const char* src = s.data() + pos; src <= ms.subjectEnd(); ++src) {
        ms.reset();
        const char* e = ms.match(src, p.data());
        if (e != nullptr && e != lastmatch) {
            const uint64_t end_off = Value::fromInt64(e - s.data()).rawBits();
            cl->upvalues[2]->value = end_off;
            cl->upvalues[3]->value = end_off;
            uint64_t out[LuaPatternMatch::kMaxCaptures];
            const size_t n = ms.captures(rt, src, e, out);
            return returnValues(rt, out, n);
        }
    }
    cl->upvalues[2]->value = Value::fromInt64(static_cast<int64_t>(s.size()) + 1).rawBits();
    return returnValues(rt, nullptr, 0);
}

uint64_t strGmatch(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.gmatch");
    char nb_s[kNumberBufSize], nb_p[kNumberBufSize];
    std::string_view s = a.checkString(1, nb_s);
    std::string_view p = a.checkString(2, nb_p);
    size_t init = posRelatI(a.optInteger(3, 1), s.size()) - 1;
    if (init > s.size()) init = s.size() + 1;
    // The strings as values the iterator keeps (a number argument becomes one).
    const uint64_t sv = a.at(1).isString() ? a.at(1).rawBits() : rt.makeStringValue(s);
    const uint64_t pv = a.at(2).isString() ? a.at(2).rawBits() : rt.makeStringValue(p);
    LuaClosure* iter = rt.allocClosure(&strGmatchIter, nullptr, 4, 0, true);
    iter->upvalues[0]->value = sv;
    iter->upvalues[1]->value = pv;
    iter->upvalues[2]->value = Value::fromInt64(static_cast<int64_t>(init)).rawBits();
    iter->upvalues[3]->value = Value::fromInt64(-1).rawBits();
    return Value::fromFunction(iter).rawBits();
}

// add_s: the replacement string with its %0-%9 and %%.
void addReplacement(RuntimeState& rt, const LuaPatternMatch& ms, StrBuf& b, std::string_view news,
                    const char* s, const char* e) {
    size_t l = news.size();
    const char* n = news.data();
    const char* p = nullptr;
    while ((p = static_cast<const char*>(std::memchr(n, '%', l))) != nullptr) {
        b.append(n, static_cast<size_t>(p - n));
        ++p; // the escape
        if (*p == '%') {
            b.push(*p);
        } else if (*p == '0') {
            b.append(s, static_cast<size_t>(e - s));
        } else if (std::isdigit(static_cast<unsigned char>(*p))) {
            const char* cap = nullptr;
            int64_t pos = 0;
            const ptrdiff_t resl = ms.capture(*p - '1', s, e, &cap, &pos);
            if (resl == LuaPatternMatch::kCapPosition) {
                char buf[kNumberBufSize];
                b.append(buf, numberToChars(Value::fromInt64(pos), buf));
            } else {
                b.append(cap, static_cast<size_t>(resl));
            }
        } else {
            throw LuaRuntimeError("invalid use of '%' in replacement string");
        }
        l -= static_cast<size_t>(p + 1 - n);
        n = p + 1;
    }
    b.append(n, l);
    (void)rt;
}

uint64_t strGsub(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.gsub");
    char nb_s[kNumberBufSize], nb_p[kNumberBufSize], nb_r[kNumberBufSize];
    std::string_view src_all = a.checkString(1, nb_s);
    std::string_view p = a.checkString(2, nb_p);
    Value repl = a.at(3);
    const int64_t max_s = a.optInteger(4, static_cast<int64_t>(src_all.size()) + 1);
    const bool repl_is_string = repl.isString() || repl.isNumeric();
    if (a.isNone(3) || !(repl_is_string || repl.isFunction() || repl.isTable())) {
        a.typeError(3, "string/function/table");
    }
    std::string_view repl_s = repl_is_string ? a.checkString(3, nb_r) : std::string_view();
    const bool anchor = !p.empty() && p[0] == '^';
    if (anchor) p.remove_prefix(1);
    LuaPatternMatch ms(src_all, p);
    const char* src = src_all.data();
    const char* lastmatch = nullptr;
    int64_t n = 0;
    bool changed = false;
    StrBuf b;
    while (n < max_s) {
        ms.reset();
        const char* e = ms.match(src, p.data());
        if (e != nullptr && e != lastmatch) {
            ++n;
            if (repl_is_string) {
                addReplacement(rt, ms, b, repl_s, src, e);
                changed = true;
            } else {
                uint64_t res = kNilBits;
                if (repl.isFunction()) {
                    uint64_t caps[LuaPatternMatch::kMaxCaptures];
                    const size_t nc = ms.captures(rt, src, e, caps);
                    res = brasslua_rt_call(repl.rawBits(), caps, static_cast<int64_t>(nc));
                } else {
                    res = brasslua_rt_table_get(repl.rawBits(), ms.captureValue(rt, 0, src, e));
                }
                Value rv(res);
                if (!rv.isTruthy()) {
                    b.append(src, static_cast<size_t>(e - src)); // keep the original text
                } else if (rv.isString() || rv.isNumeric()) {
                    char nb[kNumberBufSize];
                    b.append(rv.isString() ? valueToStringView(rt, rv)
                                           : std::string_view(nb, numberToChars(rv, nb)));
                    changed = true;
                } else {
                    throw LuaRuntimeError(std::string("invalid replacement value (a ") + typeNameOf(rv) + ")");
                }
            }
            src = lastmatch = e;
        } else if (src < ms.subjectEnd()) {
            b.push(*src++);
        } else {
            break;
        }
        if (anchor) break;
    }
    uint64_t out[2];
    if (!changed) {
        out[0] = a.at(1).isString() ? a.at(1).rawBits() : rt.makeStringValue(src_all);
    } else {
        b.append(src, static_cast<size_t>(ms.subjectEnd() - src));
        out[0] = b.toValue(rt);
    }
    out[1] = Value::fromInt64(n).rawBits();
    return returnValues(rt, out, 2);
}

} // namespace

void registerStringPack(RuntimeState& rt, LuaTable* str_tbl);

void registerStringStdlib(RuntimeState& rt) {
    LuaTable* str_tbl = rt.allocTable(0, 32);
    setTableFieldStr(rt, str_tbl, "byte", makeNativeClosure(rt, &strByte, 1));
    setTableFieldStr(rt, str_tbl, "char", makeNativeClosure(rt, &strChar, 0));
    setTableFieldStr(rt, str_tbl, "find", makeNativeClosure(rt, &strFind, 2));
    setTableFieldStr(rt, str_tbl, "format", makeNativeClosure(rt, &strFormat, 1));
    setTableFieldStr(rt, str_tbl, "gmatch", makeNativeClosure(rt, &strGmatch, 2));
    setTableFieldStr(rt, str_tbl, "gsub", makeNativeClosure(rt, &strGsub, 3));
    setTableFieldStr(rt, str_tbl, "len", makeNativeClosure(rt, &strLen, 1));
    setTableFieldStr(rt, str_tbl, "lower", makeNativeClosure(rt, &strLower, 1));
    setTableFieldStr(rt, str_tbl, "match", makeNativeClosure(rt, &strMatch, 2));
    setTableFieldStr(rt, str_tbl, "rep", makeNativeClosure(rt, &strRep, 2));
    setTableFieldStr(rt, str_tbl, "reverse", makeNativeClosure(rt, &strReverse, 1));
    setTableFieldStr(rt, str_tbl, "sub", makeNativeClosure(rt, &strSub, 2));
    setTableFieldStr(rt, str_tbl, "upper", makeNativeClosure(rt, &strUpper, 1));
    registerStringPack(rt, str_tbl);
    rt.setGlobal("string", Value::fromTable(str_tbl));

    LuaTable* str_mt = rt.allocTable(0, 2);
    setTableFieldStr(rt, str_mt, "__index", Value::fromTable(str_tbl));
    setStringMetatable(rt, str_mt);
}

} // namespace brasslua
