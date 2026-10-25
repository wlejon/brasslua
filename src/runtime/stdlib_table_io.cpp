// The table library, after Lua 5.4's ltablib.c (Copyright (C) 1994-2023
// Lua.org, PUC-Rio, MIT license): the same arguments, results, messages
// and sort. Elements are read and written as t[i] is (metamethods
// included). Then the part of the io library brasslua has.

#include "runtime/stdlib_aux.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace brasslua {

namespace {

constexpr int kTabR = 1; // read
constexpr int kTabW = 2; // write
constexpr int kTabL = 4; // length

// checktab: a table, or something whose metatable gives what is needed.
void checkTab(const Args& a, int64_t arg, int what) {
    Value v = a.at(arg);
    if (v.isTable()) return;
    auto& rt = RuntimeState::current();
    const bool ok = !a.isNone(arg) &&
                    (!(what & kTabR) || !lookupMetamethod(rt, v, "__index").isNil()) &&
                    (!(what & kTabW) || !lookupMetamethod(rt, v, "__newindex").isNil()) &&
                    (!(what & kTabL) || !lookupMetamethod(rt, v, "__len").isNil());
    if (!ok) a.typeError(arg, "table");
}

int64_t auxGetN(const Args& a, int64_t arg, int what) {
    checkTab(a, arg, what | kTabL);
    return brasslua_rt_obj_len(a.at(arg).rawBits());
}

uint64_t geti(uint64_t t, int64_t i) { return brasslua_rt_table_get_int(t, i); }
void seti(uint64_t t, int64_t i, uint64_t v) { brasslua_rt_table_set_int(t, i, v); }

uint64_t tblInsert(void*, const uint64_t* args, int64_t nargs) {
    Args a(args, nargs, "table.insert");
    const uint64_t t = a.at(1).rawBits();
    const int64_t e = static_cast<int64_t>(static_cast<uint64_t>(auxGetN(a, 1, kTabR | kTabW)) + 1);
    int64_t pos = 0;
    switch (nargs) {
        case 2:
            pos = e;
            break;
        case 3:
            pos = a.checkInteger(2);
            if (static_cast<uint64_t>(pos) - 1u >= static_cast<uint64_t>(e)) a.error(2, "position out of bounds");
            for (int64_t i = e; i > pos; --i) seti(t, i, geti(t, i - 1));
            break;
        default:
            throw LuaRuntimeError("wrong number of arguments to 'insert'");
    }
    seti(t, pos, args[nargs - 1]);
    return returnValues(RuntimeState::current(), nullptr, 0);
}

uint64_t tblRemove(void*, const uint64_t* args, int64_t nargs) {
    Args a(args, nargs, "table.remove");
    const uint64_t t = a.at(1).rawBits();
    const int64_t size = auxGetN(a, 1, kTabR | kTabW);
    int64_t pos = a.optInteger(2, size);
    if (pos != size && static_cast<uint64_t>(pos) - 1u > static_cast<uint64_t>(size)) {
        a.error(2, "position out of bounds");
    }
    const uint64_t result = geti(t, pos);
    for (; pos < size; ++pos) seti(t, pos, geti(t, pos + 1));
    seti(t, pos, kNilBits);
    return result;
}

uint64_t tblMove(void*, const uint64_t* args, int64_t nargs) {
    Args a(args, nargs, "table.move");
    const int64_t f = a.checkInteger(2);
    const int64_t e = a.checkInteger(3);
    const int64_t t = a.checkInteger(4);
    const int64_t tt = !a.isNoneOrNil(5) ? 5 : 1;
    checkTab(a, 1, kTabR);
    checkTab(a, tt, kTabW);
    const uint64_t src = a.at(1).rawBits();
    const uint64_t dst = a.at(tt).rawBits();
    if (e >= f) {
        if (!(f > 0 || e < INT64_MAX + f)) a.error(3, "too many elements to move");
        const int64_t n = e - f + 1;
        if (t > INT64_MAX - n + 1) a.error(4, "destination wrap around");
        if (t > e || t <= f || (tt != 1 && brasslua_rt_eq(src, dst) == 0)) {
            for (int64_t i = 0; i < n; ++i) seti(dst, t + i, geti(src, f + i));
        } else {
            for (int64_t i = n - 1; i >= 0; --i) seti(dst, t + i, geti(src, f + i));
        }
    }
    return dst;
}

void addField(RuntimeState& rt, StrBuf& b, uint64_t t, int64_t i) {
    Value v(geti(t, i));
    if (v.isString()) {
        b.append(valueToStringView(rt, v));
    } else if (v.isNumeric()) {
        char nb[kNumberBufSize];
        b.append(nb, numberToChars(v, nb));
    } else {
        throw LuaRuntimeError(std::string("invalid value (") + typeNameOf(v) + ") at index " + std::to_string(i) +
                              " in table for 'concat'");
    }
}

uint64_t tblConcat(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "table.concat");
    int64_t last = auxGetN(a, 1, kTabR);
    char nb[kNumberBufSize];
    std::string_view sep = a.optString(2, nb, "");
    int64_t i = a.optInteger(3, 1);
    last = a.optInteger(4, last);
    const uint64_t t = a.at(1).rawBits();
    StrBuf b;
    for (; i < last; ++i) {
        addField(rt, b, t, i);
        b.append(sep);
    }
    if (i == last) addField(rt, b, t, i);
    return b.toValue(rt);
}

uint64_t tblPack(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    LuaTable* tbl = rt.allocTable(static_cast<uint32_t>(nargs), 1);
    const uint64_t t = Value::fromTable(tbl).rawBits();
    for (int64_t i = nargs; i >= 1; --i) seti(t, i, args[i - 1]);
    setTableFieldStr(rt, tbl, "n", Value::fromInt64(nargs));
    return t;
}

uint64_t tblUnpack(void*, const uint64_t* args, int64_t nargs) {
    auto& rt = RuntimeState::current();
    Args a(args, nargs, "table.unpack");
    const uint64_t t = a.at(1).rawBits();
    int64_t i = a.optInteger(2, 1);
    const int64_t e = a.isNoneOrNil(3) ? brasslua_rt_obj_len(t) : a.checkInteger(3);
    if (i > e) return returnValues(rt, nullptr, 0);
    uint64_t n = static_cast<uint64_t>(e) - static_cast<uint64_t>(i); // one less than the count
    constexpr uint64_t kMaxResults = 1000000; // LUAI_MAXSTACK
    if (n >= kMaxResults) throw LuaRuntimeError("too many results to unpack");
    ++n;
    RuntimeState::RootedValues out(rt);
    out.values.reserve(static_cast<size_t>(n));
    for (; i < e; ++i) out.values.push_back(geti(t, i));
    out.values.push_back(geti(t, e));
    return returnValues(rt, out.values.data(), out.values.size());
}

// ---- sort: Lua's quicksort ----

constexpr unsigned kRanLimit = 100u;

unsigned randomizePivot() noexcept {
    const clock_t c = std::clock();
    const time_t t = std::time(nullptr);
    unsigned buff[(sizeof(c) + sizeof(t)) / sizeof(unsigned)];
    std::memcpy(buff, &c, sizeof(c));
    std::memcpy(reinterpret_cast<char*>(buff) + sizeof(c), &t, sizeof(t));
    unsigned rnd = 0;
    for (unsigned x : buff) rnd += x;
    return rnd;
}

struct Sorter {
    uint64_t t;
    uint64_t comp; // nil bits for '<'

    bool lt(uint64_t x, uint64_t y) const {
        if (Value(comp).isNil()) return brasslua_rt_lt(x, y) != 0;
        const uint64_t cargs[2] = {x, y};
        return Value(brasslua_rt_call(comp, cargs, 2)).isTruthy();
    }
    uint64_t get(uint64_t i) const { return geti(t, static_cast<int64_t>(i)); }
    void set(uint64_t i, uint64_t v) const { seti(t, static_cast<int64_t>(i), v); }

    // pivot is a[up - 1]'s value.
    unsigned partition(unsigned lo, unsigned up, uint64_t pivot) const {
        unsigned i = lo;     // incremented before its first use
        unsigned j = up - 1; // decremented before its first use
        // invariant: a[lo .. i] <= P <= a[j .. up], a[up - 1] == P
        for (;;) {
            uint64_t ai = 0;
            while (ai = get(++i), lt(ai, pivot)) {
                if (i == up - 1) throw LuaRuntimeError("invalid order function for sorting");
            }
            uint64_t aj = 0;
            while (aj = get(--j), lt(pivot, aj)) {
                if (j < i) throw LuaRuntimeError("invalid order function for sorting");
            }
            if (j < i) {
                set(up - 1, ai);
                set(i, pivot);
                return i;
            }
            set(i, aj);
            set(j, ai);
        }
    }

    static unsigned choosePivot(unsigned lo, unsigned up, unsigned rnd) noexcept {
        const unsigned r4 = (up - lo) / 4;
        return rnd % (r4 * 2) + (lo + r4);
    }

    void sort(unsigned lo, unsigned up, unsigned rnd) const {
        while (lo < up) { // a loop for the tail call
            {
                const uint64_t alo = get(lo), aup = get(up);
                if (lt(aup, alo)) {
                    set(lo, aup);
                    set(up, alo);
                }
            }
            if (up - lo == 1) break;
            unsigned p = (up - lo < kRanLimit || rnd == 0) ? (lo + up) / 2 : choosePivot(lo, up, rnd);
            {
                const uint64_t ap = get(p), alo = get(lo);
                if (lt(ap, alo)) {
                    set(p, alo);
                    set(lo, ap);
                } else {
                    const uint64_t aup = get(up);
                    if (lt(aup, ap)) {
                        set(p, aup);
                        set(up, ap);
                    }
                }
            }
            if (up - lo == 2) break;
            const uint64_t pivot = get(p);
            set(p, get(up - 1));
            set(up - 1, pivot);
            p = partition(lo, up, pivot);
            unsigned n = 0;
            if (p - lo < up - p) { // the lower interval is shorter
                sort(lo, p - 1, rnd);
                n = p - lo;
                lo = p + 1;
            } else {
                sort(p + 1, up, rnd);
                n = up - p;
                up = p - 1;
            }
            if ((up - lo) / 128 > n) rnd = randomizePivot(); // too imbalanced
        }
    }
};

uint64_t tblSort(void*, const uint64_t* args, int64_t nargs) {
    Args a(args, nargs, "table.sort");
    const int64_t n = auxGetN(a, 1, kTabR | kTabW);
    if (n > 1) {
        if (n >= INT_MAX) a.error(1, "array too big");
        if (!a.isNoneOrNil(2) && !a.at(2).isFunction()) a.typeError(2, "function");
        Sorter s{a.at(1).rawBits(), a.isNone(2) ? kNilBits : a.at(2).rawBits()};
        s.sort(1, static_cast<unsigned>(n), 0);
    }
    return returnValues(RuntimeState::current(), nullptr, 0);
}

// ---- io ----

uint64_t ioWrite(void*, const uint64_t* args, int64_t nargs) {
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

uint64_t ioStderrWrite(void*, const uint64_t* args, int64_t nargs) {
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

uint64_t ioFlush(void*, const uint64_t*, int64_t) {
    std::fflush(stdout);
    std::fflush(stderr);
    return Value::fromBool(true).rawBits();
}

} // namespace

void registerTableStdlib(RuntimeState& rt) {
    LuaTable* tbl_lib = rt.allocTable(0, 8);
    setTableFieldStr(rt, tbl_lib, "concat", makeNativeClosure(rt, &tblConcat, 1));
    setTableFieldStr(rt, tbl_lib, "insert", makeNativeClosure(rt, &tblInsert, 2));
    setTableFieldStr(rt, tbl_lib, "move", makeNativeClosure(rt, &tblMove, 4));
    setTableFieldStr(rt, tbl_lib, "pack", makeNativeClosure(rt, &tblPack, 0));
    setTableFieldStr(rt, tbl_lib, "remove", makeNativeClosure(rt, &tblRemove, 1));
    setTableFieldStr(rt, tbl_lib, "sort", makeNativeClosure(rt, &tblSort, 1));
    setTableFieldStr(rt, tbl_lib, "unpack", makeNativeClosure(rt, &tblUnpack, 1));
    rt.setGlobal("table", Value::fromTable(tbl_lib));
}

void registerIoStdlib(RuntimeState& rt) {
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
