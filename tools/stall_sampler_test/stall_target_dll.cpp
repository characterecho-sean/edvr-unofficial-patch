// A module the stall sampler's rig blocks a thread in, so the sampler has to name an image that is not the
// test's own executable (the way it has to name nvwgf2umx.dll or an EDHM DLL in the game).
//
// stall_target_spin spins until *stop is set, counting in *progress; the function is not inlined and keeps
// its frame so a sample's top frame lands inside it. Build: cl /LD.
#include <windows.h>

#pragma optimize("", off)

extern "C" __declspec(dllexport) __declspec(noinline) void stall_target_spin(volatile LONG* stop, volatile LONG64* progress) {
    // A local array so the function has a frame of its own and so unwind data (a leaf that never touches the
    // stack pointer has none, and the rig looks the function's range up by it).
    volatile char pad[64];
    pad[0] = 0;
    while (!*stop) {
        InterlockedIncrement64(progress);
        pad[0] = static_cast<char>(pad[0] + 1);
    }
}

extern "C" __declspec(dllexport) __declspec(noinline) void stall_target_wait(HANDLE event) {
    WaitForSingleObject(event, INFINITE);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
