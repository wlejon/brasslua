#pragma once
// What Lua's auxiliary library (lauxlib.c) gives the standard library's
// native functions: argument checks with the same conversions and the same
// messages ("bad argument #2 to 'sub' (number expected, got nil)"), and a
// buffer to build a result in. Arguments are numbered from 1, as Lua
// numbers them.

#include "runtime/runtime_internal.h"

#include <cstring>
#include <memory>
#include <string_view>

namespace brasslua {

// luaL_typename: "nil", "number", ... ("no value" is Args's).
const char* typeNameOf(Value v) noexcept;

class Args {
public:
    Args(const uint64_t* args, int64_t nargs, const char* fname) noexcept
        : args_(args), nargs_(nargs), fname_(fname) {}

    int64_t count() const noexcept { return nargs_; }
    Value at(int64_t i) const noexcept { return i <= nargs_ ? Value(args_[i - 1]) : Value::nil(); }
    bool isNone(int64_t i) const noexcept { return i > nargs_; }
    bool isNoneOrNil(int64_t i) const noexcept { return i > nargs_ || Value(args_[i - 1]).isNil(); }

    // luaL_argerror / luaL_typeerror.
    [[noreturn]] void error(int64_t i, std::string_view extramsg) const;
    [[noreturn]] void typeError(int64_t i, const char* expected) const;

    void checkAny(int64_t i) const;
    // A string, or a number written into `numbuf` (kNumberBufSize bytes) as
    // Lua converts one. Either way the bytes are followed by a NUL.
    std::string_view checkString(int64_t i, char* numbuf) const;
    std::string_view optString(int64_t i, char* numbuf, std::string_view def) const {
        return isNoneOrNil(i) ? def : checkString(i, numbuf);
    }
    int64_t checkInteger(int64_t i) const;
    int64_t optInteger(int64_t i, int64_t def) const { return isNoneOrNil(i) ? def : checkInteger(i); }
    // A number keeping its subtype (strings read as Lua reads them).
    Value checkNumber(int64_t i) const;
    double checkDouble(int64_t i) const;
    LuaTable* checkTable(int64_t i) const;

private:
    const uint64_t* args_;
    int64_t nargs_;
    const char* fname_;
};

// luaL_Buffer: bytes built up in place (on the stack while they are few).
class StrBuf {
public:
    StrBuf() noexcept = default;
    StrBuf(const StrBuf&) = delete;
    StrBuf& operator=(const StrBuf&) = delete;

    void push(char c) {
        if (len_ == cap_) grow(1);
        data_[len_++] = c;
    }
    void append(const char* s, size_t n) {
        if (n > cap_ - len_) grow(n);
        std::memcpy(data_ + len_, s, n);
        len_ += n;
    }
    void append(std::string_view s) { append(s.data(), s.size()); }
    // Room for n more bytes, written by the caller and then commit()ted.
    char* prepare(size_t n) {
        if (n > cap_ - len_) grow(n);
        return data_ + len_;
    }
    void commit(size_t n) noexcept { len_ += n; }
    size_t size() const noexcept { return len_; }
    std::string_view view() const noexcept { return std::string_view(data_, len_); }
    // The buffer as a Lua string.
    uint64_t toValue(RuntimeState& rt) const { return rt.makeStringValue(view()); }

private:
    void grow(size_t need);

    char inline_[256];
    char* data_ = inline_;
    size_t len_ = 0;
    size_t cap_ = sizeof(inline_);
    std::unique_ptr<char[]> heap_;
};

// Returns vals[0..n) as a native function's results.
inline uint64_t returnValues(RuntimeState& rt, const uint64_t* vals, size_t n) {
    rt.setMultiRet(vals, n);
    return n == 0 ? kNilBits : vals[0];
}

} // namespace brasslua
