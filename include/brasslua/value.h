#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

extern "C" uint64_t brasslua_rt_box_int64(int64_t v);

namespace brasslua {

// 64-bit NaN-boxed Value encoding, bit-compatible with bronze::Value reference
// tags ({0xFFF1, 0xFFF2, 0xFFF8, 0xFFFB}) on the shared brass::gc::Heap while
// supporting Lua 5.4's distinct 64-bit integer and 64-bit float subtypes.
enum class Tag : uint16_t {
    Object        = 0xFFF1, // LuaTable*, LuaCoroutine*, or LuaUserdata* (distinguished by host_bits)
    String        = 0xFFF2, // LuaString*
    Int47         = 0xFFF3, // 48-bit signed integer inline payload [-2^47, 2^47 - 1]
    Int32         = 0xFFF3, // alias for Int47
    Bool          = 0xFFF4, // 0 or 1
    LightUserdata = 0xFFF5, // raw C pointer or JS null sentinel (non-GC)
    Null          = 0xFFF5, // alias for LightUserdata null sentinel
    Undefined     = 0xFFF6, // Lua nil (and JS undefined)
    Nil           = 0xFFF6, // alias for Undefined
    Hole          = 0xFFF7,
    Function      = 0xFFF8, // LuaClosure* (matches reference tag 0xFFF8 on brass::gc::Heap)
    BoxedInt64    = 0xFFFB, // LuaBoxedInt64* for int64_t outside [-2^47, 2^47 - 1] (matches 0xFFFB)
};

// Stored in brass::gc::ObjectHeader::host_bits (immediately preceding payload at ptr[-1])
enum HostObjectKind : uint8_t {
    kHostKindTable      = 0,
    kHostKindCoroutine  = 1,
    kHostKindUserdata   = 2,
    kHostKindUpvalue    = 3,
    kHostKindClosure    = 4,
    kHostKindString     = 5,
    kHostKindBoxedInt64 = 6,
    kHostKindBuffer     = 7,
};

struct alignas(8) LuaBoxedInt64 {
    int64_t value = 0;
};

constexpr uint64_t kCanonicalNaNBits = 0x7FF8000000000000ULL;
constexpr uint64_t kNumberMaxBits    = 0xFFF0000000000000ULL;
constexpr uint64_t kPayloadMask      = 0x0000FFFFFFFFFFFFULL;
constexpr uint32_t kTagShift         = 48;

constexpr int64_t kMinInlineInt = -(1LL << 47);
constexpr int64_t kMaxInlineInt = (1LL << 47) - 1;

constexpr uint64_t kNilBits   = static_cast<uint64_t>(Tag::Undefined) << kTagShift;
constexpr uint64_t kFalseBits = (static_cast<uint64_t>(Tag::Bool) << kTagShift) | 0ULL;
constexpr uint64_t kTrueBits  = (static_cast<uint64_t>(Tag::Bool) << kTagShift) | 1ULL;

class Value {
public:
    constexpr Value() noexcept : bits_(kNilBits) {}
    constexpr explicit Value(uint64_t bits) noexcept : bits_(bits) {}

    static constexpr Value nil() noexcept { return Value(kNilBits); }

    static constexpr Value fromBool(bool b) noexcept {
        return Value(b ? kTrueBits : kFalseBits);
    }

    static Value fromDouble(double d) noexcept {
        if (std::isnan(d)) {
            return Value(kCanonicalNaNBits);
        }
        return Value(std::bit_cast<uint64_t>(d));
    }

    static constexpr bool fitsInlineInt(int64_t i) noexcept {
        return i >= kMinInlineInt && i <= kMaxInlineInt;
    }

    static constexpr int64_t decodeInlineInt(uint64_t bits) noexcept {
        return static_cast<int64_t>(bits << 16) >> 16;
    }

    static constexpr Value fromInlineInt(int64_t i) noexcept {
        return Value((static_cast<uint64_t>(Tag::Int47) << kTagShift) |
                     (static_cast<uint64_t>(i) & kPayloadMask));
    }

    static constexpr Value fromInt32Tag(int32_t i) noexcept {
        return fromInlineInt(static_cast<int64_t>(i));
    }

    static Value fromInt64(int64_t i) noexcept {
        if (fitsInlineInt(i)) {
            return fromInlineInt(i);
        }
        return Value(brasslua_rt_box_int64(i));
    }

    static Value fromBoxedInt64Ptr(const LuaBoxedInt64* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::BoxedInt64) << kTagShift) | (addr & kPayloadMask));
    }

    static Value fromTable(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::Object) << kTagShift) | (addr & kPayloadMask));
    }

    static Value fromCoroutine(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::Object) << kTagShift) | (addr & kPayloadMask));
    }

    static Value fromUserdata(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::Object) << kTagShift) | (addr & kPayloadMask));
    }

    static Value fromLightUserdata(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::LightUserdata) << kTagShift) | (addr & kPayloadMask));
    }

    static constexpr Value nullSentinel() noexcept {
        return Value(static_cast<uint64_t>(Tag::LightUserdata) << kTagShift);
    }

    static Value fromString(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::String) << kTagShift) | (addr & kPayloadMask));
    }

    static Value fromFunction(const void* ptr) noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return Value((static_cast<uint64_t>(Tag::Function) << kTagShift) | (addr & kPayloadMask));
    }

    constexpr uint64_t rawBits() const noexcept { return bits_; }
    constexpr uint16_t tag() const noexcept { return static_cast<uint16_t>(bits_ >> kTagShift); }
    constexpr uint64_t payload() const noexcept { return bits_ & kPayloadMask; }

    uint8_t hostKind() const noexcept {
        auto addr = static_cast<uintptr_t>(payload());
        if (addr == 0) return 0xFF;
        return reinterpret_cast<const uint8_t*>(addr)[-1];
    }

    // Float subtype (64-bit IEEE-754 double; kNumberMaxBits itself is -inf)
    constexpr bool isFloat() const noexcept { return bits_ <= kNumberMaxBits; }
    constexpr bool isNumber() const noexcept { return bits_ <= kNumberMaxBits; }

    // Integer subtype (48-bit signed inline Int47 OR heap-allocated 64-bit BoxedInt64)
    constexpr bool isInlineInt() const noexcept { return tag() == static_cast<uint16_t>(Tag::Int47); }
    constexpr bool isInt32Tag() const noexcept { return isInlineInt(); }
    constexpr bool isBoxedInt64() const noexcept { return tag() == static_cast<uint16_t>(Tag::BoxedInt64); }
    constexpr bool isInteger() const noexcept { return isInlineInt() || isBoxedInt64(); }

    // Any Lua number (either integer or float)
    constexpr bool isNumeric() const noexcept { return isFloat() || isInteger(); }

    constexpr bool isNil() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::Undefined);
    }
    constexpr bool isBool() const noexcept { return tag() == static_cast<uint16_t>(Tag::Bool); }
    constexpr bool isObjectTag() const noexcept { return tag() == static_cast<uint16_t>(Tag::Object); }
    bool isTable() const noexcept {
        return isObjectTag() && hostKind() == kHostKindTable;
    }
    bool isCoroutine() const noexcept {
        return isObjectTag() && hostKind() == kHostKindCoroutine;
    }
    bool isFullUserdata() const noexcept {
        return isObjectTag() && hostKind() == kHostKindUserdata;
    }
    constexpr bool isLightUserdata() const noexcept {
        return tag() == static_cast<uint16_t>(Tag::LightUserdata);
    }
    constexpr bool isNullSentinel() const noexcept {
        return isLightUserdata() && payload() == 0;
    }
    bool isUserdata() const noexcept {
        return isLightUserdata() || isFullUserdata();
    }
    constexpr bool isString() const noexcept { return tag() == static_cast<uint16_t>(Tag::String); }
    constexpr bool isFunction() const noexcept { return tag() == static_cast<uint16_t>(Tag::Function); }

    constexpr bool isGcPointer() const noexcept {
        uint16_t t = tag();
        return t == static_cast<uint16_t>(Tag::Object) ||
               t == static_cast<uint16_t>(Tag::String) ||
               t == static_cast<uint16_t>(Tag::Function) ||
               t == static_cast<uint16_t>(Tag::BoxedInt64);
    }

    // Lua truthiness: ONLY nil and false are falsy! (0 and "" are truthy in Lua)
    constexpr bool isTruthy() const noexcept {
        return !isNil() && bits_ != kFalseBits;
    }

    double asDouble() const noexcept {
        if (isFloat()) {
            return std::bit_cast<double>(bits_);
        }
        if (isInlineInt()) {
            return static_cast<double>(decodeInlineInt(bits_));
        }
        if (isBoxedInt64()) {
            return static_cast<double>(*reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(payload())));
        }
        return 0.0;
    }

    int64_t asInt64() const noexcept {
        if (isInlineInt()) {
            return decodeInlineInt(bits_);
        }
        if (isBoxedInt64()) {
            return *reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(payload()));
        }
        if (isFloat()) {
            return static_cast<int64_t>(std::bit_cast<double>(bits_));
        }
        return 0;
    }

    constexpr bool asBool() const noexcept { return (bits_ & 1ULL) != 0; }

    void* asPointer() const noexcept {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(payload()));
    }

    bool operator==(const Value& other) const noexcept {
        if (bits_ == other.bits_) return true;
        if (isBoxedInt64() && other.isBoxedInt64()) {
            return asInt64() == other.asInt64();
        }
        return false;
    }
    bool operator!=(const Value& other) const noexcept { return !(*this == other); }

private:
    uint64_t bits_;
};

} // namespace brasslua
