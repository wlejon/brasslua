#include "runtime/stdlib_aux.h"

#include <string>

namespace brasslua {

void StrBuf::grow(size_t need) {
    size_t cap = cap_ * 2;
    if (cap < len_ + need) cap = len_ + need;
    std::unique_ptr<char[]> bigger(new char[cap]);
    std::memcpy(bigger.get(), data_, len_);
    heap_ = std::move(bigger);
    data_ = heap_.get();
    cap_ = cap;
}

const char* typeNameOf(Value v) noexcept {
    if (v.isNil()) return "nil";
    if (v.isBool()) return "boolean";
    if (v.isNumeric()) return "number";
    if (v.isString()) return "string";
    if (v.isTable()) return "table";
    if (v.isFunction()) return "function";
    if (v.isCoroutine()) return "thread";
    if (v.isUserdata()) return "userdata";
    return "nil";
}

void Args::error(int64_t i, std::string_view extramsg) const {
    std::string msg = "bad argument #" + std::to_string(i) + " to '" + fname_ + "' (";
    msg.append(extramsg);
    msg.push_back(')');
    throw LuaRuntimeError(std::move(msg));
}

void Args::typeError(int64_t i, const char* expected) const {
    std::string got;
    if (isNone(i)) {
        got = "no value";
    } else {
        Value v = at(i);
        Value name;
        if (v.isTable() || v.isFullUserdata()) {
            name = lookupMetamethod(RuntimeState::current(), v, "__name");
        }
        if (name.isString()) {
            got = std::string(valueToStringView(RuntimeState::current(), name));
        } else if (v.isLightUserdata()) {
            got = "light userdata";
        } else {
            got = typeNameOf(v);
        }
    }
    error(i, std::string(expected) + " expected, got " + got);
}

void Args::checkAny(int64_t i) const {
    if (isNone(i)) error(i, "value expected");
}

std::string_view Args::checkString(int64_t i, char* numbuf) const {
    Value v = at(i);
    if (!isNone(i) && v.isString()) {
        auto* s = static_cast<const LuaString*>(v.asPointer());
        return std::string_view(s->data, s->length);
    }
    if (!isNone(i) && v.isNumeric()) {
        return std::string_view(numbuf, numberToChars(v, numbuf));
    }
    typeError(i, "string");
}

int64_t Args::checkInteger(int64_t i) const {
    Value v = at(i);
    if (!isNone(i)) {
        if (v.isInteger()) return v.asInt64();
        int64_t out = 0;
        if (tryCoerceToInt64(v, out)) return out;
        double d = 0.0;
        if (tryCoerceToDouble(v, d)) error(i, "number has no integer representation");
    }
    typeError(i, "number");
}

Value Args::checkNumber(int64_t i) const {
    Value v = at(i);
    if (!isNone(i)) {
        if (v.isNumeric()) return v;
        if (v.isString()) {
            auto* s = static_cast<const LuaString*>(v.asPointer());
            Value n;
            if (stringToNumber(std::string_view(s->data, s->length), n)) return n;
        }
    }
    typeError(i, "number");
}

double Args::checkDouble(int64_t i) const {
    Value n = checkNumber(i);
    return n.isInteger() ? static_cast<double>(n.asInt64()) : n.asDouble();
}

LuaTable* Args::checkTable(int64_t i) const {
    Value v = at(i);
    if (isNone(i) || !v.isTable()) typeError(i, "table");
    return static_cast<LuaTable*>(v.asPointer());
}

} // namespace brasslua
