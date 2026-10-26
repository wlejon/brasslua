#pragma once

// A test that crashes or aborts on Windows reports to stderr and exits, rather
// than opening a dialog that waits for someone to dismiss it.

#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace brasslua::tests {

inline void disableErrorDialogs() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    const int types[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (int type : types) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
}

} // namespace brasslua::tests
