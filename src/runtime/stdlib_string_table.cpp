#include "runtime/runtime_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace brasslua {

namespace {

std::string argStr(RuntimeState& rt, const uint64_t* args, int64_t nargs, int64_t idx) {
    if (idx >= nargs) return {};
    Value v(args[idx]);
    if (v.isString()) {
        auto* s = static_cast<const LuaString*>(v.asPointer());
        return s ? std::string(s->data, s->length) : std::string();
    }
    if (v.isNumeric()) {
        return valueToStdString(rt, v);
    }
    throw LuaRuntimeError("bad argument to string function (string expected)");
}

int64_t posRelat(int64_t pos, size_t len) noexcept {
    if (pos >= 0) return pos;
    if (static_cast<size_t>(-pos) > len) return 0;
    return static_cast<int64_t>(len) + pos + 1;
}

uint64_t strLen(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    return Value::fromInt64(static_cast<int64_t>(s.size())).rawBits();
}

uint64_t strSub(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    size_t len = s.size();
    int64_t start = posRelat(nargs >= 2 ? Value(args[1]).asInt64() : 1, len);
    int64_t end = posRelat((nargs >= 3 && !Value(args[2]).isNil()) ? Value(args[2]).asInt64() : -1, len);
    if (start < 1) start = 1;
    if (end > static_cast<int64_t>(len)) end = static_cast<int64_t>(len);
    if (start > end) return rt.makeStringValue("");
    return rt.makeStringValue(
        std::string_view(s.data() + (start - 1), static_cast<size_t>(end - start + 1)));
}

uint64_t strByte(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    size_t len = s.size();
    int64_t posi = posRelat((nargs >= 2 && !Value(args[1]).isNil()) ? Value(args[1]).asInt64() : 1, len);
    int64_t pose = posRelat((nargs >= 3 && !Value(args[2]).isNil()) ? Value(args[2]).asInt64() : posi, len);
    if (posi < 1) posi = 1;
    if (pose > static_cast<int64_t>(len)) pose = static_cast<int64_t>(len);
    if (posi > pose) return returnMulti(rt, {});
    std::vector<uint64_t> ret;
    ret.reserve(static_cast<size_t>(pose - posi + 1));
    for (int64_t k = posi; k <= pose; ++k) {
        unsigned char c = static_cast<unsigned char>(s[static_cast<size_t>(k - 1)]);
        ret.push_back(Value::fromInt64(static_cast<int64_t>(c)).rawBits());
    }
    return returnMulti(rt, ret);
}

uint64_t strChar(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string out;
    out.resize(static_cast<size_t>(nargs > 0 ? nargs : 0));
    for (int64_t i = 0; i < nargs; ++i) {
        int64_t c = Value(args[i]).asInt64();
        if (c < 0 || c > 255) {
            throw LuaRuntimeError("bad argument to 'string.char' (value out of range)");
        }
        out[static_cast<size_t>(i)] = static_cast<char>(static_cast<unsigned char>(c));
    }
    return rt.makeStringValue(out);
}

uint64_t strLower(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return rt.makeStringValue(s);
}

uint64_t strUpper(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return rt.makeStringValue(s);
}

uint64_t strReverse(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    std::reverse(s.begin(), s.end());
    return rt.makeStringValue(s);
}

uint64_t strRep(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    int64_t n = nargs >= 2 ? Value(args[1]).asInt64() : 0;
    std::string sep = (nargs >= 3 && !Value(args[2]).isNil()) ? argStr(rt, args, nargs, 2) : std::string();
    if (n <= 0) return rt.makeStringValue("");
    std::string out;
    out.reserve(s.size() * static_cast<size_t>(n) + sep.size() * static_cast<size_t>(n - 1));
    for (int64_t i = 0; i < n; ++i) {
        if (i > 0 && !sep.empty()) out.append(sep);
        out.append(s);
    }
    return rt.makeStringValue(out);
}

uint64_t strFormat(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string fmt = argStr(rt, args, nargs, 0);
    std::string out;
    int64_t arg_idx = 1;
    size_t i = 0;
    while (i < fmt.size()) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i++]);
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out.push_back('%');
            i += 2;
            continue;
        }
        size_t spec_start = i++;
        while (i < fmt.size() &&
               (std::isdigit(static_cast<unsigned char>(fmt[i])) || fmt[i] == '-' ||
                fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '.')) {
            ++i;
        }
        if (i >= fmt.size()) break;
        char conv = fmt[i++];
        std::string sub_fmt = fmt.substr(spec_start, i - spec_start);
        Value arg = arg_idx < nargs ? Value(args[arg_idx++]) : Value::nil();
        char buf[512];
        switch (conv) {
            case 'd':
            case 'i': {
                std::string lfmt = sub_fmt.substr(0, sub_fmt.size() - 1) + "lld";
                int64_t iv = 0;
                tryCoerceToInt64(arg, iv);
                std::snprintf(buf, sizeof(buf), lfmt.c_str(), static_cast<long long>(iv));
                out.append(buf);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                std::string lfmt = sub_fmt.substr(0, sub_fmt.size() - 1) + "ll" + conv;
                int64_t iv = 0;
                tryCoerceToInt64(arg, iv);
                std::snprintf(buf, sizeof(buf), lfmt.c_str(),
                              static_cast<unsigned long long>(static_cast<uint64_t>(iv)));
                out.append(buf);
                break;
            }
            case 'f':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                double dv = 0.0;
                tryCoerceToDouble(arg, dv);
                std::snprintf(buf, sizeof(buf), sub_fmt.c_str(), dv);
                out.append(buf);
                break;
            }
            case 'c': {
                int64_t iv = 0;
                tryCoerceToInt64(arg, iv);
                out.push_back(static_cast<char>(iv));
                break;
            }
            case 's': {
                std::string sv = valueToStdString(rt, arg);
                if (sub_fmt == "%s") {
                    out.append(sv);
                } else {
                    std::snprintf(buf, sizeof(buf), sub_fmt.c_str(), sv.c_str());
                    out.append(buf);
                }
                break;
            }
            case 'q': {
                if (arg.isString()) {
                    std::string sv = valueToStdString(rt, arg);
                    out.push_back('"');
                    for (unsigned char ch : sv) {
                        if (ch == '"' || ch == '\\' || ch == '\n') {
                            out.push_back('\\');
                            out.push_back(static_cast<char>(ch));
                        } else if (ch < 32 || ch == 127) {
                            char esc[8];
                            std::snprintf(esc, sizeof(esc), "\\%d", static_cast<int>(ch));
                            out.append(esc);
                        } else {
                            out.push_back(static_cast<char>(ch));
                        }
                    }
                    out.push_back('"');
                } else {
                    out.append(valueToStdString(rt, arg));
                }
                break;
            }
            default:
                out.append(sub_fmt);
                break;
        }
    }
    return rt.makeStringValue(out);
}

// Simple Lua pattern matching helper supporting literals, `.`, `%d`, `%a`, `%w`, `%s`,
// `+`, `*`, `-`, `?`, `^`, `$`, and captures `(...)`
bool matchCharClass(unsigned char c, unsigned char cl) noexcept {
    bool res = false;
    switch (std::tolower(cl)) {
        case 'a': res = std::isalpha(c) != 0; break;
        case 'c': res = std::iscntrl(c) != 0; break;
        case 'd': res = std::isdigit(c) != 0; break;
        case 'l': res = std::islower(c) != 0; break;
        case 'p': res = std::ispunct(c) != 0; break;
        case 's': res = std::isspace(c) != 0; break;
        case 'u': res = std::isupper(c) != 0; break;
        case 'w': res = std::isalnum(c) != 0; break;
        case 'x': res = std::isxdigit(c) != 0; break;
        case 'z': res = (c == 0); break;
        default: return c == cl;
    }
    return std::islower(cl) ? res : !res;
}

bool matchSingle(unsigned char c, std::string_view pat, size_t p, size_t& next_p) noexcept {
    if (pat[p] == '%') {
        if (p + 1 < pat.size()) {
            next_p = p + 2;
            return matchCharClass(c, static_cast<unsigned char>(pat[p + 1]));
        }
    }
    if (pat[p] == '[') {
        size_t close = p + 1;
        bool inv = false;
        if (close < pat.size() && pat[close] == '^') {
            inv = true;
            ++close;
        }
        if (close < pat.size() && pat[close] == ']') ++close;
        bool matched = false;
        while (close < pat.size() && pat[close] != ']') {
            if (pat[close] == '%' && close + 1 < pat.size()) {
                if (matchCharClass(c, static_cast<unsigned char>(pat[close + 1]))) matched = true;
                close += 2;
            } else if (close + 2 < pat.size() && pat[close + 1] == '-' && pat[close + 2] != ']') {
                unsigned char lo = static_cast<unsigned char>(pat[close]);
                unsigned char hi = static_cast<unsigned char>(pat[close + 2]);
                if (c >= lo && c <= hi) matched = true;
                close += 3;
            } else {
                if (c == static_cast<unsigned char>(pat[close])) matched = true;
                ++close;
            }
        }
        next_p = close < pat.size() ? close + 1 : pat.size();
        return inv ? !matched : matched;
    }
    next_p = p + 1;
    if (pat[p] == '.') return true;
    return c == static_cast<unsigned char>(pat[p]);
}

bool matchHere(std::string_view s, size_t si, std::string_view pat, size_t pi,
               size_t& out_end, std::vector<std::string>& captures) {
    while (pi < pat.size()) {
        if (pat[pi] == '(') {
            ++pi;
            continue;
        }
        if (pat[pi] == ')') {
            ++pi;
            continue;
        }
        if (pat[pi] == '$' && pi + 1 == pat.size()) {
            if (si == s.size()) {
                out_end = si;
                return true;
            }
            return false;
        }
        size_t item_end = pi;
        unsigned char dummy = 0;
        matchSingle(dummy, pat, pi, item_end);
        char quant = (item_end < pat.size()) ? pat[item_end] : '\0';
        if (quant == '*' || quant == '+' || quant == '-' || quant == '?') {
            size_t after_quant = item_end + 1;
            if (quant == '+') {
                size_t next_p = 0;
                if (si >= s.size() || !matchSingle(static_cast<unsigned char>(s[si]), pat, pi, next_p)) {
                    return false;
                }
                ++si;
            }
            if (quant == '-') {
                for (size_t k = si; k <= s.size(); ++k) {
                    if (matchHere(s, k, pat, after_quant, out_end, captures)) return true;
                    size_t np = 0;
                    if (k == s.size() || !matchSingle(static_cast<unsigned char>(s[k]), pat, pi, np)) break;
                }
                return false;
            }
            if (quant == '?') {
                size_t np = 0;
                if (si < s.size() && matchSingle(static_cast<unsigned char>(s[si]), pat, pi, np)) {
                    if (matchHere(s, si + 1, pat, after_quant, out_end, captures)) return true;
                }
                return matchHere(s, si, pat, after_quant, out_end, captures);
            }
            // Greedy '*' or '+'
            size_t max_si = si;
            while (max_si < s.size()) {
                size_t np = 0;
                if (!matchSingle(static_cast<unsigned char>(s[max_si]), pat, pi, np)) break;
                ++max_si;
            }
            for (size_t k = max_si; k >= si; --k) {
                if (matchHere(s, k, pat, after_quant, out_end, captures)) return true;
                if (k == si) break;
            }
            return false;
        }
        size_t np = 0;
        if (si < s.size() && matchSingle(static_cast<unsigned char>(s[si]), pat, pi, np)) {
            ++si;
            pi = item_end;
            continue;
        }
        return false;
    }
    out_end = si;
    return true;
}

bool findPattern(std::string_view s, std::string_view pat, size_t start_pos, bool plain,
                 size_t& match_start, size_t& match_end) {
    if (plain) {
        size_t pos = s.find(pat, start_pos);
        if (pos == std::string_view::npos) return false;
        match_start = pos;
        match_end = pos + pat.size();
        return true;
    }
    bool anchored = !pat.empty() && pat[0] == '^';
    std::string_view eff_pat = anchored ? pat.substr(1) : pat;
    for (size_t i = start_pos; i <= s.size(); ++i) {
        size_t end_pos = 0;
        std::vector<std::string> caps;
        if (matchHere(s, i, eff_pat, 0, end_pos, caps)) {
            match_start = i;
            match_end = end_pos;
            return true;
        }
        if (anchored) break;
    }
    return false;
}

uint64_t strFind(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    std::string pat = argStr(rt, args, nargs, 1);
    int64_t init = posRelat((nargs >= 3 && !Value(args[2]).isNil()) ? Value(args[2]).asInt64() : 1, s.size());
    if (init < 1) init = 1;
    if (static_cast<size_t>(init) > s.size() + 1) return returnMulti(rt, {kNilBits});
    bool plain = (nargs >= 4 && Value(args[3]).isTruthy());
    size_t ms = 0, me = 0;
    if (!findPattern(s, pat, static_cast<size_t>(init - 1), plain, ms, me)) {
        return returnMulti(rt, {kNilBits});
    }
    return returnMulti(rt, {
        Value::fromInt64(static_cast<int64_t>(ms + 1)).rawBits(),
        Value::fromInt64(static_cast<int64_t>(me)).rawBits()
    });
}

uint64_t strMatch(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    std::string pat = argStr(rt, args, nargs, 1);
    int64_t init = posRelat((nargs >= 3 && !Value(args[2]).isNil()) ? Value(args[2]).asInt64() : 1, s.size());
    if (init < 1) init = 1;
    if (static_cast<size_t>(init) > s.size() + 1) return kNilBits;
    size_t ms = 0, me = 0;
    if (!findPattern(s, pat, static_cast<size_t>(init - 1), false, ms, me)) {
        return kNilBits;
    }
    return rt.makeStringValue(std::string_view(s.data() + ms, me - ms));
}

uint64_t strGsub(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    std::string s = argStr(rt, args, nargs, 0);
    std::string pat = argStr(rt, args, nargs, 1);
    Value repl = nargs >= 3 ? Value(args[2]) : Value::nil();
    int64_t max_n = (nargs >= 4 && !Value(args[3]).isNil()) ? Value(args[3]).asInt64() : static_cast<int64_t>(s.size() + 1);

    std::string out;
    int64_t count = 0;
    size_t pos = 0;
    while (count < max_n && pos <= s.size()) {
        size_t ms = 0, me = 0;
        if (!findPattern(s, pat, pos, false, ms, me)) break;
        out.append(s.data() + pos, ms - pos);
        std::string matched(s.data() + ms, me - ms);
        if (repl.isString() || repl.isNumeric()) {
            std::string rstr = valueToStdString(rt, repl);
            for (size_t k = 0; k < rstr.size(); ++k) {
                if (rstr[k] == '%' && k + 1 < rstr.size()) {
                    if (rstr[k + 1] == '0' || rstr[k + 1] == '1') {
                        out.append(matched);
                    } else {
                        out.push_back(rstr[k + 1]);
                    }
                    ++k;
                } else {
                    out.push_back(rstr[k]);
                }
            }
        } else if (repl.isFunction()) {
            uint64_t res = brasslua_rt_call_1(repl.rawBits(), rt.makeStringValue(matched));
            if (Value(res).isTruthy()) {
                out.append(valueToStdString(rt, Value(res)));
            } else {
                out.append(matched);
            }
        } else if (repl.isTable()) {
            uint64_t res = brasslua_rt_table_get(repl.rawBits(), rt.makeStringValue(matched));
            if (Value(res).isTruthy()) {
                out.append(valueToStdString(rt, Value(res)));
            } else {
                out.append(matched);
            }
        }
        ++count;
        if (me == ms) {
            if (me < s.size()) out.push_back(s[me]);
            pos = me + 1;
        } else {
            pos = me;
        }
    }
    if (pos <= s.size()) {
        out.append(s.data() + pos, s.size() - pos);
    }
    return returnMulti(rt, {rt.makeStringValue(out), Value::fromInt64(count).rawBits()});
}

// ---- `table` Library ----

uint64_t tblInsert(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 2 || !Value(args[0]).isTable()) {
        throw LuaRuntimeError("bad argument #1 to 'insert' (table expected)");
    }
    uint64_t tbl_bits = args[0];
    int64_t len = brasslua_rt_table_len(tbl_bits);
    if (nargs == 2) {
        brasslua_rt_table_set_int(tbl_bits, len + 1, args[1]);
    } else {
        int64_t pos = Value(args[1]).asInt64();
        uint64_t val = args[2];
        for (int64_t k = len; k >= pos; --k) {
            brasslua_rt_table_set_int(tbl_bits, k + 1, brasslua_rt_table_get_int(tbl_bits, k));
        }
        brasslua_rt_table_set_int(tbl_bits, pos, val);
    }
    return kNilBits;
}

uint64_t tblRemove(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1 || !Value(args[0]).isTable()) {
        throw LuaRuntimeError("bad argument #1 to 'remove' (table expected)");
    }
    uint64_t tbl_bits = args[0];
    int64_t len = brasslua_rt_table_len(tbl_bits);
    int64_t pos = (nargs >= 2 && !Value(args[1]).isNil()) ? Value(args[1]).asInt64() : len;
    if (len == 0 || pos < 1 || pos > len + 1) return kNilBits;
    uint64_t removed = brasslua_rt_table_get_int(tbl_bits, pos);
    for (int64_t k = pos; k < len; ++k) {
        brasslua_rt_table_set_int(tbl_bits, k, brasslua_rt_table_get_int(tbl_bits, k + 1));
    }
    brasslua_rt_table_set_int(tbl_bits, len, kNilBits);
    return removed;
}

uint64_t tblConcat(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    if (nargs < 1 || !Value(args[0]).isTable()) {
        throw LuaRuntimeError("bad argument #1 to 'concat' (table expected)");
    }
    uint64_t tbl_bits = args[0];
    std::string sep = (nargs >= 2 && !Value(args[1]).isNil()) ? argStr(rt, args, nargs, 1) : std::string();
    int64_t i = (nargs >= 3 && !Value(args[2]).isNil()) ? Value(args[2]).asInt64() : 1;
    int64_t j = (nargs >= 4 && !Value(args[3]).isNil()) ? Value(args[3]).asInt64() : brasslua_rt_table_len(tbl_bits);
    std::string out;
    for (int64_t k = i; k <= j; ++k) {
        if (k > i && !sep.empty()) out.append(sep);
        Value elem(brasslua_rt_table_get_int(tbl_bits, k));
        if (!elem.isString() && !elem.isNumeric()) {
            throw LuaRuntimeError("invalid value in table for 'concat'");
        }
        out.append(valueToStdString(rt, elem));
    }
    return rt.makeStringValue(out);
}

uint64_t tblSort(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 1 || !Value(args[0]).isTable()) {
        throw LuaRuntimeError("bad argument #1 to 'sort' (table expected)");
    }
    uint64_t tbl_bits = args[0];
    uint64_t comp = (nargs >= 2 && !Value(args[1]).isNil()) ? args[1] : kNilBits;
    int64_t len = brasslua_rt_table_len(tbl_bits);
    if (len <= 1) return kNilBits;

    std::vector<uint64_t> elems(static_cast<size_t>(len));
    for (int64_t i = 1; i <= len; ++i) {
        elems[static_cast<size_t>(i - 1)] = brasslua_rt_table_get_int(tbl_bits, i);
    }
    std::sort(elems.begin(), elems.end(), [&](uint64_t a, uint64_t b) {
        if (!Value(comp).isNil()) {
            uint64_t res = brasslua_rt_call_2(comp, a, b);
            return Value(res).isTruthy();
        }
        return brasslua_rt_lt(a, b) != 0;
    });
    for (int64_t i = 1; i <= len; ++i) {
        brasslua_rt_table_set_int(tbl_bits, i, elems[static_cast<size_t>(i - 1)]);
    }
    return kNilBits;
}

uint64_t tblPack(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    LuaTable* tbl = rt.allocTable(static_cast<uint32_t>(nargs > 0 ? nargs : 1), 2);
    uint64_t tbl_bits = Value::fromTable(tbl).rawBits();
    for (int64_t i = 0; i < nargs; ++i) {
        brasslua_rt_table_set_int(tbl_bits, i + 1, args[i]);
    }
    setTableFieldStr(rt, tbl, "n", Value::fromInt64(nargs));
    return tbl_bits;
}

uint64_t tblMove(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    if (nargs < 4) throw LuaRuntimeError("bad argument to 'table.move'");
    uint64_t a1 = args[0];
    int64_t f = Value(args[1]).asInt64();
    int64_t e = Value(args[2]).asInt64();
    int64_t t = Value(args[3]).asInt64();
    uint64_t a2 = (nargs >= 5 && !Value(args[4]).isNil()) ? args[4] : a1;
    if (e >= f) {
        int64_t n = e - f + 1;
        if (a1 != a2 || t <= f || t > e) {
            for (int64_t i = 0; i < n; ++i) {
                brasslua_rt_table_set_int(a2, t + i, brasslua_rt_table_get_int(a1, f + i));
            }
        } else {
            for (int64_t i = n - 1; i >= 0; --i) {
                brasslua_rt_table_set_int(a2, t + i, brasslua_rt_table_get_int(a1, f + i));
            }
        }
    }
    return a2;
}

// ---- `io` Library ----

uint64_t ioWrite(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    int64_t start = 0;
    if (nargs >= 1 && Value(args[0]).isTable()) {
        start = 1; // called as io.stdout:write(...)
    }
    for (int64_t i = start; i < nargs; ++i) {
        rt.writeOutput(valueToStdString(rt, Value(args[i])));
    }
    return Value::fromBool(true).rawBits();
}

uint64_t ioStderrWrite(void* /*cl*/, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    int64_t start = 0;
    if (nargs >= 1 && Value(args[0]).isTable()) {
        start = 1; // called as io.stderr:write(...)
    }
    for (int64_t i = start; i < nargs; ++i) {
        std::string s = valueToStdString(rt, Value(args[i]));
        std::fwrite(s.data(), 1, s.size(), stderr);
    }
    return Value::fromBool(true).rawBits();
}

uint64_t ioFlush(void* /*cl*/, const uint64_t* /*args*/, int64_t /*nargs*/) {
    std::fflush(stdout);
    std::fflush(stderr);
    return Value::fromBool(true).rawBits();
}

} // namespace

void registerStringAndTableStdlib(RuntimeState& rt) {
    LuaTable* str_tbl = rt.allocTable(0, 16);
    setTableFieldStr(rt, str_tbl, "len", makeNativeClosure(rt, &strLen, 1));
    setTableFieldStr(rt, str_tbl, "sub", makeNativeClosure(rt, &strSub, 2));
    setTableFieldStr(rt, str_tbl, "byte", makeNativeClosure(rt, &strByte, 1));
    setTableFieldStr(rt, str_tbl, "char", makeNativeClosure(rt, &strChar, 1));
    setTableFieldStr(rt, str_tbl, "lower", makeNativeClosure(rt, &strLower, 1));
    setTableFieldStr(rt, str_tbl, "upper", makeNativeClosure(rt, &strUpper, 1));
    setTableFieldStr(rt, str_tbl, "reverse", makeNativeClosure(rt, &strReverse, 1));
    setTableFieldStr(rt, str_tbl, "rep", makeNativeClosure(rt, &strRep, 2));
    setTableFieldStr(rt, str_tbl, "format", makeNativeClosure(rt, &strFormat, 1));
    setTableFieldStr(rt, str_tbl, "find", makeNativeClosure(rt, &strFind, 2));
    setTableFieldStr(rt, str_tbl, "match", makeNativeClosure(rt, &strMatch, 2));
    setTableFieldStr(rt, str_tbl, "gsub", makeNativeClosure(rt, &strGsub, 3));
    rt.setGlobal("string", Value::fromTable(str_tbl));

    LuaTable* str_mt = rt.allocTable(0, 2);
    setTableFieldStr(rt, str_mt, "__index", Value::fromTable(str_tbl));
    setStringMetatable(rt, str_mt);

    LuaTable* tbl_lib = rt.allocTable(0, 8);
    setTableFieldStr(rt, tbl_lib, "insert", makeNativeClosure(rt, &tblInsert, 2));
    setTableFieldStr(rt, tbl_lib, "remove", makeNativeClosure(rt, &tblRemove, 1));
    setTableFieldStr(rt, tbl_lib, "concat", makeNativeClosure(rt, &tblConcat, 1));
    setTableFieldStr(rt, tbl_lib, "sort", makeNativeClosure(rt, &tblSort, 1));
    setTableFieldStr(rt, tbl_lib, "pack", makeNativeClosure(rt, &tblPack, 0));
    setTableFieldStr(rt, tbl_lib, "unpack", rt.getGlobal("unpack"));
    setTableFieldStr(rt, tbl_lib, "move", makeNativeClosure(rt, &tblMove, 4));
    rt.setGlobal("table", Value::fromTable(tbl_lib));

    LuaTable* io_tbl = rt.allocTable(0, 8);
    Value write_fn = makeNativeClosure(rt, &ioWrite, 1);
    Value flush_fn = makeNativeClosure(rt, &ioFlush, 0);
    setTableFieldStr(rt, io_tbl, "write", write_fn);
    setTableFieldStr(rt, io_tbl, "flush", flush_fn);
    LuaTable* stdout_tbl = rt.allocTable(0, 4);
    setTableFieldStr(rt, stdout_tbl, "write", write_fn);
    setTableFieldStr(rt, stdout_tbl, "flush", flush_fn);
    LuaTable* stderr_tbl = rt.allocTable(0, 4);
    setTableFieldStr(rt, stderr_tbl, "write", makeNativeClosure(rt, &ioStderrWrite, 1));
    setTableFieldStr(rt, stderr_tbl, "flush", flush_fn);
    setTableFieldStr(rt, io_tbl, "stdout", Value::fromTable(stdout_tbl));
    setTableFieldStr(rt, io_tbl, "stderr", Value::fromTable(stderr_tbl));
    rt.setGlobal("io", Value::fromTable(io_tbl));
}

} // namespace brasslua
