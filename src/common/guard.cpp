#include "guard.h"

#include <windows.h>

#include <cstdio>

#include "log.h"

namespace edvr {

int guardFilter(unsigned long code, const char* site) {
    Log::get().note("FAULT exception=0x%08lX site=%s", code, site ? site : "?");
    return EXCEPTION_EXECUTE_HANDLER;
}

void FaultBudget::charge() {
    if (m_remaining > 0) {
        --m_remaining;
        if (m_remaining == 0) {
            Log::get().note("FEATURE-DISABLED %s exhausted its fault budget", m_name);
        }
    }
}

Sentinel::Sentinel(const wchar_t* dir, const wchar_t* name) {
    _snwprintf_s(m_path, _TRUNCATE, L"%s\\%s.armed", dir, name);
    m_tripped = GetFileAttributesW(m_path) != INVALID_FILE_ATTRIBUTES;
}

void Sentinel::arm() {
    HANDLE f = CreateFileW(m_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const char msg[] = "edvr: a hook was armed and never confirmed. "
                           "Delete this file to re-enable it.\r\n";
        DWORD written = 0;
        WriteFile(f, msg, sizeof(msg) - 1, &written, nullptr);
        FlushFileBuffers(f);
        CloseHandle(f);
    }
    m_armed = true;
}

void Sentinel::confirm() {
    if (!m_armed) return;
    DeleteFileW(m_path);
    m_armed = false;
}

}  // namespace edvr
