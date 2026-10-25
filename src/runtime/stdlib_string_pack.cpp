// string.pack, string.packsize and string.unpack, after Lua 5.4's
// lstrlib.c (Copyright (C) 1994-2023 Lua.org, PUC-Rio, MIT license).

#include "runtime/stdlib_aux.h"

#include <climits>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace brasslua {

namespace {

constexpr int kMaxIntSize = 16;
constexpr int kSzInt = static_cast<int>(sizeof(int64_t));
constexpr unsigned kMc = (1u << CHAR_BIT) - 1;
constexpr size_t kMaxSize = INT_MAX;

bool nativeLittle() noexcept {
    const uint16_t one = 1;
    unsigned char b;
    std::memcpy(&b, &one, 1);
    return b == 1;
}

enum class KOption { Int, Uint, Float, Number, Double, Char, String, Zstr, Padding, Paddalign, Nop };

struct Header {
    bool little = nativeLittle();
    int maxalign = 1;
};

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

int getNum(const char*& fmt, int df) noexcept {
    if (!isDigit(*fmt)) return df;
    int a = 0;
    do {
        a = a * 10 + (*fmt++ - '0');
    } while (isDigit(*fmt) && a <= (static_cast<int>(kMaxSize) - 9) / 10);
    return a;
}

int getNumLimit(const char*& fmt, int df) {
    const int sz = getNum(fmt, df);
    if (sz > kMaxIntSize || sz <= 0) {
        throw LuaRuntimeError("integral size (" + std::to_string(sz) + ") out of limits [1," +
                              std::to_string(kMaxIntSize) + "]");
    }
    return sz;
}

KOption getOption(Header& h, const char*& fmt, int& size) {
    struct cD {
        char c;
        union {
            double d;
            void* p;
            long long l;
        } u;
    };
    const int opt = *fmt++;
    size = 0;
    switch (opt) {
        case 'b': size = sizeof(char); return KOption::Int;
        case 'B': size = sizeof(char); return KOption::Uint;
        case 'h': size = sizeof(short); return KOption::Int;
        case 'H': size = sizeof(short); return KOption::Uint;
        case 'l': size = sizeof(long); return KOption::Int;
        case 'L': size = sizeof(long); return KOption::Uint;
        case 'j': size = sizeof(int64_t); return KOption::Int;
        case 'J': size = sizeof(int64_t); return KOption::Uint;
        case 'T': size = sizeof(size_t); return KOption::Uint;
        case 'f': size = sizeof(float); return KOption::Float;
        case 'n': size = sizeof(double); return KOption::Number;
        case 'd': size = sizeof(double); return KOption::Double;
        case 'i': size = getNumLimit(fmt, sizeof(int)); return KOption::Int;
        case 'I': size = getNumLimit(fmt, sizeof(int)); return KOption::Uint;
        case 's': size = getNumLimit(fmt, sizeof(size_t)); return KOption::String;
        case 'c':
            size = getNum(fmt, -1);
            if (size == -1) throw LuaRuntimeError("missing size for format option 'c'");
            return KOption::Char;
        case 'z': return KOption::Zstr;
        case 'x': size = 1; return KOption::Padding;
        case 'X': return KOption::Paddalign;
        case ' ': break;
        case '<': h.little = true; break;
        case '>': h.little = false; break;
        case '=': h.little = nativeLittle(); break;
        case '!': h.maxalign = getNumLimit(fmt, static_cast<int>(offsetof(cD, u))); break;
        default: throw LuaRuntimeError(std::string("invalid format option '") + static_cast<char>(opt) + "'");
    }
    return KOption::Nop;
}

KOption getDetails(const Args& a, Header& h, size_t totalsize, const char*& fmt, int& size, int& ntoalign) {
    const KOption opt = getOption(h, fmt, size);
    int align = size; // alignment usually follows size
    if (opt == KOption::Paddalign) { // 'X' takes the next option's alignment
        if (*fmt == '\0' || getOption(h, fmt, align) == KOption::Char || align == 0) {
            a.error(1, "invalid next option for option 'X'");
        }
    }
    if (align <= 1 || opt == KOption::Char) {
        ntoalign = 0;
    } else {
        if (align > h.maxalign) align = h.maxalign;
        if ((align & (align - 1)) != 0) a.error(1, "format asks for alignment not power of 2");
        ntoalign = (align - static_cast<int>(totalsize & static_cast<size_t>(align - 1))) & (align - 1);
    }
    return opt;
}

void packInt(StrBuf& b, uint64_t n, bool little, int size, bool neg) {
    char* buff = b.prepare(static_cast<size_t>(size));
    buff[little ? 0 : size - 1] = static_cast<char>(n & kMc);
    for (int i = 1; i < size; ++i) {
        n >>= CHAR_BIT;
        buff[little ? i : size - 1 - i] = static_cast<char>(n & kMc);
    }
    if (neg && size > kSzInt) { // sign-extend the extra bytes
        for (int i = kSzInt; i < size; ++i) buff[little ? i : size - 1 - i] = static_cast<char>(kMc);
    }
    b.commit(static_cast<size_t>(size));
}

void copyWithEndian(char* dest, const char* src, int size, bool little) noexcept {
    if (little == nativeLittle()) {
        std::memcpy(dest, src, static_cast<size_t>(size));
    } else {
        dest += size - 1;
        while (size-- != 0) *(dest--) = *(src++);
    }
}

uint64_t strPack(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.pack");
    char nb_f[kNumberBufSize];
    const char* fmt = a.checkString(1, nb_f).data();
    Header h;
    int64_t arg = 1;
    size_t totalsize = 0;
    StrBuf b;
    while (*fmt != '\0') {
        int size = 0, ntoalign = 0;
        const KOption opt = getDetails(a, h, totalsize, fmt, size, ntoalign);
        totalsize += static_cast<size_t>(ntoalign + size);
        while (ntoalign-- > 0) b.push('\0');
        ++arg;
        switch (opt) {
            case KOption::Int: {
                const int64_t n = a.checkInteger(arg);
                if (size < kSzInt) {
                    const int64_t lim = int64_t{1} << ((size * CHAR_BIT) - 1);
                    if (!(-lim <= n && n < lim)) a.error(arg, "integer overflow");
                }
                packInt(b, static_cast<uint64_t>(n), h.little, size, n < 0);
                break;
            }
            case KOption::Uint: {
                const int64_t n = a.checkInteger(arg);
                if (size < kSzInt && static_cast<uint64_t>(n) >= (uint64_t{1} << (size * CHAR_BIT))) {
                    a.error(arg, "unsigned overflow");
                }
                packInt(b, static_cast<uint64_t>(n), h.little, size, false);
                break;
            }
            case KOption::Float: {
                const float f = static_cast<float>(a.checkDouble(arg));
                copyWithEndian(b.prepare(sizeof(f)), reinterpret_cast<const char*>(&f), sizeof(f), h.little);
                b.commit(static_cast<size_t>(size));
                break;
            }
            case KOption::Number:
            case KOption::Double: {
                const double f = a.checkDouble(arg);
                copyWithEndian(b.prepare(sizeof(f)), reinterpret_cast<const char*>(&f), sizeof(f), h.little);
                b.commit(static_cast<size_t>(size));
                break;
            }
            case KOption::Char: {
                char nb[kNumberBufSize];
                std::string_view s = a.checkString(arg, nb);
                if (s.size() > static_cast<size_t>(size)) a.error(arg, "string longer than given size");
                b.append(s);
                for (size_t len = s.size(); len < static_cast<size_t>(size); ++len) b.push('\0');
                break;
            }
            case KOption::String: {
                char nb[kNumberBufSize];
                std::string_view s = a.checkString(arg, nb);
                if (!(size >= static_cast<int>(sizeof(size_t)) || s.size() < (size_t{1} << (size * CHAR_BIT)))) {
                    a.error(arg, "string length does not fit in given size");
                }
                packInt(b, s.size(), h.little, size, false);
                b.append(s);
                totalsize += s.size();
                break;
            }
            case KOption::Zstr: {
                char nb[kNumberBufSize];
                std::string_view s = a.checkString(arg, nb);
                if (std::strlen(s.data()) != s.size()) a.error(arg, "string contains zeros");
                b.append(s);
                b.push('\0');
                totalsize += s.size() + 1;
                break;
            }
            case KOption::Padding:
                b.push('\0');
                --arg;
                break;
            case KOption::Paddalign:
            case KOption::Nop:
                --arg;
                break;
        }
    }
    return b.toValue(rt);
}

uint64_t strPackSize(void*, const uint64_t* args, int64_t nargs) {
    Args a(args, nargs, "string.packsize");
    char nb_f[kNumberBufSize];
    const char* fmt = a.checkString(1, nb_f).data();
    Header h;
    size_t totalsize = 0;
    while (*fmt != '\0') {
        int size = 0, ntoalign = 0;
        const KOption opt = getDetails(a, h, totalsize, fmt, size, ntoalign);
        if (opt == KOption::String || opt == KOption::Zstr) a.error(1, "variable-length format");
        size += ntoalign;
        if (totalsize > kMaxSize - static_cast<size_t>(size)) a.error(1, "format result too large");
        totalsize += static_cast<size_t>(size);
    }
    return Value::fromInt64(static_cast<int64_t>(totalsize)).rawBits();
}

int64_t unpackInt(const char* str, bool little, int size, bool is_signed) {
    uint64_t res = 0;
    const int limit = (size <= kSzInt) ? size : kSzInt;
    for (int i = limit - 1; i >= 0; --i) {
        res <<= CHAR_BIT;
        res |= static_cast<unsigned char>(str[little ? i : size - 1 - i]);
    }
    if (size < kSzInt) {
        if (is_signed) { // sign-extend
            const uint64_t mask = uint64_t{1} << (size * CHAR_BIT - 1);
            res = (res ^ mask) - mask;
        }
    } else if (size > kSzInt) { // the bytes not read must be the sign's
        const unsigned mask = (!is_signed || static_cast<int64_t>(res) >= 0) ? 0 : kMc;
        for (int i = limit; i < size; ++i) {
            if (static_cast<unsigned char>(str[little ? i : size - 1 - i]) != mask) {
                throw LuaRuntimeError(std::to_string(size) + "-byte integer does not fit into Lua Integer");
            }
        }
    }
    return static_cast<int64_t>(res);
}

size_t posRelatI(int64_t pos, size_t len) noexcept {
    if (pos > 0) return static_cast<size_t>(pos);
    if (pos == 0) return 1;
    if (pos < -static_cast<int64_t>(len)) return 1;
    return len + static_cast<size_t>(pos) + 1;
}

uint64_t strUnpack(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "string.unpack");
    char nb_f[kNumberBufSize], nb_d[kNumberBufSize];
    const char* fmt = a.checkString(1, nb_f).data();
    std::string_view data_sv = a.checkString(2, nb_d);
    const char* data = data_sv.data();
    const size_t ld = data_sv.size();
    size_t pos = posRelatI(a.optInteger(3, 1), ld) - 1;
    if (pos > ld) a.error(3, "initial position out of string");
    Header h;
    RuntimeState::RootedValues rooted(rt); // the strings made so far
    std::vector<uint64_t>& out = rooted.values;
    while (*fmt != '\0') {
        int size = 0, ntoalign = 0;
        const KOption opt = getDetails(a, h, pos, fmt, size, ntoalign);
        if (static_cast<size_t>(ntoalign) + static_cast<size_t>(size) > ld - pos) a.error(2, "data string too short");
        pos += static_cast<size_t>(ntoalign);
        switch (opt) {
            case KOption::Int:
            case KOption::Uint:
                out.push_back(Value::fromInt64(unpackInt(data + pos, h.little, size, opt == KOption::Int)).rawBits());
                break;
            case KOption::Float: {
                float f;
                copyWithEndian(reinterpret_cast<char*>(&f), data + pos, sizeof(f), h.little);
                out.push_back(Value::fromDouble(static_cast<double>(f)).rawBits());
                break;
            }
            case KOption::Number:
            case KOption::Double: {
                double f;
                copyWithEndian(reinterpret_cast<char*>(&f), data + pos, sizeof(f), h.little);
                out.push_back(Value::fromDouble(f).rawBits());
                break;
            }
            case KOption::Char:
                out.push_back(rt.makeStringValue(std::string_view(data + pos, static_cast<size_t>(size))));
                break;
            case KOption::String: {
                const size_t len = static_cast<size_t>(unpackInt(data + pos, h.little, size, false));
                if (len > ld - pos - static_cast<size_t>(size)) a.error(2, "data string too short");
                out.push_back(rt.makeStringValue(std::string_view(data + pos + size, len)));
                pos += len;
                break;
            }
            case KOption::Zstr: {
                const size_t len = std::strlen(data + pos);
                if (pos + len >= ld) a.error(2, "unfinished string for format 'z'");
                out.push_back(rt.makeStringValue(std::string_view(data + pos, len)));
                pos += len + 1;
                break;
            }
            case KOption::Paddalign:
            case KOption::Padding:
            case KOption::Nop:
                break;
        }
        pos += static_cast<size_t>(size);
    }
    out.push_back(Value::fromInt64(static_cast<int64_t>(pos) + 1).rawBits());
    return returnValues(rt, out.data(), out.size());
}

} // namespace

void registerStringPack(RuntimeState& rt, LuaTable* str_tbl) {
    setTableFieldStr(rt, str_tbl, "pack", makeNativeClosure(rt, &strPack, 1));
    setTableFieldStr(rt, str_tbl, "packsize", makeNativeClosure(rt, &strPackSize, 1));
    setTableFieldStr(rt, str_tbl, "unpack", makeNativeClosure(rt, &strUnpack, 2));
}

} // namespace brasslua
