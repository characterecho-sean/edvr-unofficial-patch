#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace edvr::crash_context {

using Sink = void (*)(const char* line, void* context) noexcept;

namespace detail {

// Fixed storage and hand formatting: the process may already be failing.
struct Line {
    char text[192];
    size_t size = 0;
    void character(char c) noexcept {
        if (size < sizeof(text) - 1) text[size++] = c;
    }
    void append(const char* s) noexcept {
        while (*s && size < sizeof(text) - 1) character(*s++);
    }
    void hex(uint64_t v) noexcept {
        append("0x");
        bool lead = true;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const unsigned d = unsigned((v >> shift) & 15);
            if (lead && d == 0 && shift) continue;
            lead = false;
            character(char(d < 10 ? '0' + d : 'A' + d - 10));
        }
    }
    void field(const char* name, uint64_t v) noexcept { append(name); hex(v); }
    void send(Sink sink, void* context) noexcept {
        text[size] = 0;
        sink(text, context);
    }
};

inline bool readQword(uintptr_t address, uintptr_t& value) noexcept {
    __try {
        value = *reinterpret_cast<const uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void stop(const char* reason, Sink sink, void* context) noexcept {
    Line line;
    line.append("crash: unwind stop=");
    line.append(reason);
    line.send(sink, context);
}

__declspec(noinline) inline void emitStack(const CONTEXT& c, Sink sink, void* context) noexcept {
    for (unsigned first = 0; first < 32; first += 4) {
        Line line;
        line.append("crash: stack");
        for (unsigned i = first; i < first + 4; ++i) {
            const uintptr_t offset = i * sizeof(uintptr_t);
            uintptr_t value = 0;
            line.field(" +", offset);
            line.append("=");
            if (c.Rsp <= UINTPTR_MAX - offset && readQword(c.Rsp + offset, value)) {
                line.hex(value);
            } else {
                line.append("<unreadable>");
            }
        }
        line.send(sink, context);
    }
}

inline void moduleName(Line& line, DWORD64 rip) noexcept {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rip), &module)) {
        line.append(" module=unknown");
        return;
    }
    wchar_t path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length >= MAX_PATH) length = MAX_PATH - 1;
    DWORD base = 0;
    for (DWORD i = 0; i < length; ++i) if (path[i] == L'\\') base = i + 1;
    line.append(" module=");
    for (DWORD i = base; i < length && i - base < 64; ++i) {
        line.character(path[i] < 0x80 ? char(path[i]) : '?');
    }
    line.field(" rva=", rip - reinterpret_cast<uintptr_t>(module));
}

// Keep these buffers out of the top-level filter so its stack-overflow
// bypass does not acquire another large frame before branching.
__declspec(noinline) inline void emitUnwind(const CONTEXT& original, Sink sink,
                                          void* context) noexcept {
    CONTEXT c = original;
    for (unsigned frame = 0; frame < 8; ++frame) {
        if (!c.Rip) { stop("end", sink, context); return; }
        Line line;
        line.append("crash: unwind");
        line.field(" frame=", frame);
        line.field(" rip=", c.Rip);
        line.field(" rsp=", c.Rsp);
        moduleName(line, c.Rip);
        line.send(sink, context);

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = nullptr;
        const DWORD64 oldRip = c.Rip, oldRsp = c.Rsp;
        __try {
            function = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            stop("metadata-unreadable", sink, context); return;
        }
        if (function) {
            DWORD64 establisher = 0;
            PVOID handlerData = nullptr;
            __try {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, function,
                                 &c, &handlerData, &establisher, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                stop("frame-unreadable", sink, context); return;
            }
        } else {
            // Leaf functions have no unwind entry; their return PC is at RSP.
            uintptr_t next = 0;
            if (c.Rsp > UINTPTR_MAX - sizeof(uintptr_t) || !readQword(c.Rsp, next)) {
                stop("leaf-unreadable", sink, context); return;
            }
            c.Rip = next;
            c.Rsp += sizeof(uintptr_t);
        }
        if (c.Rsp <= oldRsp || c.Rip == oldRip) {
            stop("no-progress", sink, context); return;
        }
    }
    stop("frame-limit", sink, context);
}

} // namespace detail

__declspec(noinline) inline void report(EXCEPTION_POINTERS* info, Sink sink,
                                       void* context = nullptr) noexcept {
    if (!sink) return;
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    const CONTEXT* c = info ? info->ContextRecord : nullptr;
    detail::Line line;
    line.append("crash:");
    line.field(" thread=", GetCurrentThreadId());
    line.field(" code=", record ? record->ExceptionCode : 0);
    if (record && (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                   record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)) {
        if (record->NumberParameters < 2) {
            line.append(" params=truncated");
        } else {
            const ULONG_PTR op = record->ExceptionInformation[0];
            line.append(" op=");
            line.append(op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute" : "unknown");
            line.field(" address=", record->ExceptionInformation[1]);
            if (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
                if (record->NumberParameters < 3) line.append(" status=truncated");
                else line.field(" status=", record->ExceptionInformation[2]);
            }
        }
    }
    line.send(sink, context);
    if (!c) return;
    const char* names[] = {" rax=", " rbx=", " rcx=", " rdx=", " rsi=", " rdi=",
                          " rbp=", " rsp=", " r8=", " r9=", " r10=", " r11=",
                          " r12=", " r13=", " r14=", " r15=", " rip=", " eflags="};
    const DWORD64 values[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi,
                             c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11,
                             c->R12, c->R13, c->R14, c->R15, c->Rip, c->EFlags};
    for (unsigned first = 0; first < 18; first += 4) {
        detail::Line regs;
        regs.append("crash: regs");
        for (unsigned i = first; i < first + 4 && i < 18; ++i) regs.field(names[i], values[i]);
        regs.send(sink, context);
    }
    detail::emitStack(*c, sink, context);
    detail::emitUnwind(*c, sink, context);
}

} // namespace edvr::crash_context
