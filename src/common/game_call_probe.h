#pragma once
#include "call_probe_budget.h"
#include <windows.h>
#include <cstdio>

namespace edvr {
struct GameCallStack {
    unsigned captured=0, gameFrames=0;
    char rvas[512]{};
};
// Return addresses only: no stack/argument memory is dumped, no symbols loaded,
// and no process/thread is suspended. A short/empty unwind is explicit.
inline GameCallStack captureGameCallStack() noexcept {
    GameCallStack out{};void* frames[32]{};
    out.captured=CaptureStackBackTrace(0,static_cast<DWORD>(sizeof(frames)/sizeof(frames[0])),frames,nullptr);
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(!dos || dos->e_magic!=IMAGE_DOS_SIGNATURE)return out;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE)return out;
    const uintptr_t size=nt->OptionalHeader.SizeOfImage;
    size_t used=0;
    for(unsigned i=0;i<out.captured;++i) {
        const auto address=reinterpret_cast<uintptr_t>(frames[i]);
        if(address<base || address-base>=size)continue;
        const int n=_snprintf_s(out.rvas+used,sizeof(out.rvas)-used,_TRUNCATE,
            "%s0x%llX",out.gameFrames?"/":"",static_cast<unsigned long long>(address-base));
        if(n<0)break;
        used+=size_t(n);++out.gameFrames;
    }
    return out;
}
}
