#pragma once
// Lua 5.4's pattern matcher (lstrlib.c's MatchState and match), behind
// string.find, match, gmatch and gsub. The subject and the pattern must
// each be followed by a NUL, as Lua strings and Args's views are: the
// matcher reads the byte after either's end, as Lua's does.

#include "runtime/runtime_internal.h"

#include <cstddef>
#include <string_view>

namespace brasslua {

class LuaPatternMatch {
public:
    static constexpr int kMaxCaptures = 32;
    static constexpr ptrdiff_t kCapUnfinished = -1;
    static constexpr ptrdiff_t kCapPosition = -2;

    LuaPatternMatch(std::string_view subject, std::string_view pattern) noexcept
        : src_init_(subject.data()), src_end_(subject.data() + subject.size()),
          p_end_(pattern.data() + pattern.size()) {}

    // Forgets the captures of the last attempt (reprepstate).
    void reset() noexcept { level_ = 0; }
    // The end of the match of pattern p at subject position s, or null.
    const char* match(const char* s, const char* p);

    const char* subjectBegin() const noexcept { return src_init_; }
    const char* subjectEnd() const noexcept { return src_end_; }

    // The values of a match's captures (the whole match [s, e) when the
    // pattern has none and s is not null), written to `out`, which holds
    // kMaxCaptures; returns how many.
    size_t captures(RuntimeState& rt, const char* s, const char* e, uint64_t* out) const;
    // Capture i's bytes (get_onecapture): its length, or kCapPosition when
    // it is a position capture, whose 1-based position is `*pos`.
    ptrdiff_t capture(int i, const char* s, const char* e, const char** cap, int64_t* pos) const;
    uint64_t captureValue(RuntimeState& rt, int i, const char* s, const char* e) const;

private:
    const char* classEnd(const char* p) const;
    bool singleMatch(const char* s, const char* p, const char* ep) const noexcept;
    const char* matchBalance(const char* s, const char* p) const;
    const char* maxExpand(const char* s, const char* p, const char* ep);
    const char* minExpand(const char* s, const char* p, const char* ep);
    const char* startCapture(const char* s, const char* p, ptrdiff_t what);
    const char* endCapture(const char* s, const char* p);
    const char* matchCapture(const char* s, int l) const;
    int checkCapture(int l) const;
    int captureToClose() const;

    const char* src_init_;
    const char* src_end_;
    const char* p_end_;
    int matchdepth_ = 200; // MAXCCALLS
    int level_ = 0;
    struct {
        const char* init;
        ptrdiff_t len;
    } capture_[kMaxCaptures] = {};
};

// Whether a pattern has none of Lua's special characters (a plain search
// finds it).
bool patternHasNoSpecials(std::string_view pattern) noexcept;

} // namespace brasslua
