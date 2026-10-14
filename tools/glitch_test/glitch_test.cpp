// glitch_test -- drives the transition-flash engine fix's camera-buffer tap
// (src/d3d11/glitch_frame.cpp) without the game.
//
// The tap is the half of the fix that SEES a frame: vScreen's Map and Unmap hooks
// hand it every fill of the game's 5376-byte scene constant buffer, and while the
// engine fix has an event window open it passes the fill on. A wiring mistake
// there -- a Map gate that never nominates the buffer, a window that does not
// reach the tap, a frame counter that does not advance -- produces a clean build
// and a fix that silently never fires, so each of those is asserted here.
//
// What this proves:
//
//   1. The tap nominates the 5376-byte scene buffer (the sun-glare fix and the
//      temporal pass read the same block) whether or not the engine fix is armed.
//   2. It passes a fill on only while the engine fix's window is open, with the
//      pre-advance frame number, and never a non-finite or wrong-sized fill.
//   3. The frame counter the engine fix's window tests count in advances once
//      per boundary.
//   4. The camera geometry settings are checked before the engine fix may report
//      Armed (a size of 0, an offset outside the buffer, another size), and a
//      stale fix.transition_flash line in the ini changes nothing: there is no
//      key, and the tap is available with none.
//   5. A hold marks the frame, reports whether a compositor was there to honour
//      it, and the verdict word changes when the boundary behind it publishes.
//
// What it does NOT prove: that the hooks in vscreen.cpp call any of this, or that
// the engine fix finds the right frame. tools\native_temporal_test drives the
// engine fix through the tap end to end, and the flights prove the rest.
//
// Usage: glitch_test.exe <scratch dir>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/d3d11/glitch_frame.h"

namespace {
unsigned g_noteCalls = 0;
uint32_t g_noteFrame = 0;
size_t g_noteBytes = 0;
}  // namespace

namespace edvr {
// Linker stub: this rig drives glitch_frame.cpp alone, without the engine fix's
// module (transition_flash_eye_base.cpp, which needs CodeHook and the game). It
// records what the tap hands over.
void transitionFlashEyeBaseNoteSceneCB(uint32_t frame, const void*, size_t bytes) {
    ++g_noteCalls;
    g_noteFrame = frame;
    g_noteBytes = bytes;
}
}  // namespace edvr

using namespace edvr;

namespace {

int g_fails = 0;

void ok(const char* what) { printf("  ok    %s\n", what); }

void fail(const char* what, const std::string& detail) {
    printf("  FAIL  %s -- %s\n", what, detail.c_str());
    ++g_fails;
}

void check(const char* what, bool cond, const std::string& detail = "") {
    if (cond) ok(what);
    else fail(what, detail);
}

std::wstring widen(const char* p) {
    std::wstring out;
    for (const char* c = p; *c; ++c) out.push_back(static_cast<wchar_t>(*c));
    return out;
}

constexpr uint32_t kBytes = 5376;
constexpr uint32_t kOffset = 1100;      // in floats
constexpr uint32_t kFloats = kBytes / 4;

// One 5376-byte constant buffer with a camera position in it.
struct Buffer {
    float f[kFloats] = {};
    void setPos(float x, float y, float z) {
        f[kOffset + 0] = x;
        f[kOffset + 1] = y;
        f[kOffset + 2] = z;
    }
};

bool writeIni(const std::wstring& dir, const char* body) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\edvr.ini";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, body, static_cast<DWORD>(strlen(body)), &written, nullptr);
    CloseHandle(f);
    return true;
}

// Reinstall the tap from an ini body and report Armed, the way the engine fix
// does once its consumer hook is in.
void reinstall(const std::wstring& scratch, const char* ini) {
    shutdownGlitchFrameFix();
    glitchFrameEngineWindow(false);
    glitchFrameNoteEngineFix(false);
    writeIni(scratch, ini);
    Config::get().init(scratch);
    installGlitchFrameFix();
}

void resetNotes() {
    g_noteCalls = 0;
    g_noteFrame = 0;
    g_noteBytes = 0;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        printf("usage: glitch_test.exe <scratch dir>\n");
        return 2;
    }
    const std::wstring scratch = widen(argv[1]);

    // --- 1. Nomination: nothing until Armed, the scene buffer after -----------
    //
    // The ini states no fix.transition_flash line at all: the fix has no key.
    SetEnvironmentVariableA("EDVR_TEST_NO_CAMERA_TAP", nullptr);
    reinstall(scratch, "[log]\r\nenabled = 0\r\n");
    check("the tap wants vScreen's hooks with the shipped geometry", glitchFrameWantsHooks());
    // The sun-glare fix and the temporal pass read the same block through vScreen's
    // Unmap tee on this nomination, so it must not wait for, or follow, the engine fix.
    check("it nominates the 5376-byte scene buffer before the engine fix reports", glitchFrameWantsBuffer(kBytes));
    check("...and no other size", !glitchFrameWantsBuffer(256));
    glitchFrameNoteEngineFix(true);
    check("it still nominates it once Armed", glitchFrameWantsBuffer(kBytes));
    glitchFrameNoteEngineFix(false);
    check("...and when the engine fix reports Not armed (the other readers keep their feed)", glitchFrameWantsBuffer(kBytes));
    glitchFrameNoteEngineFix(true);

    // --- 2. The window gates the hand-over --------------------------------------
    Buffer b;
    b.setPos(1000.0f, 0.0f, 0.0f);
    resetNotes();
    glitchFrameObserve(b.f, kBytes, nullptr);
    check("a fill outside the engine fix's event window is not handed over", g_noteCalls == 0);

    glitchFrameEngineWindow(true);
    glitchFrameObserve(b.f, kBytes, nullptr);
    check("a fill inside the window is handed over once", g_noteCalls == 1,
          std::to_string(g_noteCalls) + " calls");
    check("...with the buffer's size", g_noteBytes == kBytes);

    resetNotes();
    glitchFrameObserve(b.f, 256, nullptr);
    check("a fill of another size is not handed over", g_noteCalls == 0);

    Buffer nan;
    nan.setPos(NAN, 0.0f, 0.0f);
    glitchFrameObserve(nan.f, kBytes, nullptr);
    check("a non-finite camera position is not handed over", g_noteCalls == 0);
    Buffer inf;
    inf.setPos(0.0f, INFINITY, 0.0f);
    glitchFrameObserve(inf.f, kBytes, nullptr);
    check("an infinite camera position is not handed over", g_noteCalls == 0);

    // A head-only row is a perfectly finite position near the origin: the tap
    // does not judge it (the engine fix does), it passes it on.
    Buffer head;
    head.setPos(0.1f, 1.2f, -0.3f);
    glitchFrameObserve(head.f, kBytes, nullptr);
    check("a head-pose position is passed on for the engine fix to judge", g_noteCalls == 1);

    glitchFrameEngineWindow(false);
    resetNotes();
    glitchFrameObserve(b.f, kBytes, nullptr);
    check("a fill after the window closes is not handed over", g_noteCalls == 0);

    // --- 3. The frame counter ----------------------------------------------------
    glitchFrameEngineWindow(true);
    glitchFrameObserve(b.f, kBytes, nullptr);
    const uint32_t before = g_noteFrame;
    glitchFrameBoundary();
    glitchFrameObserve(b.f, kBytes, nullptr);
    check("the frame number a fill carries advances by one per boundary", g_noteFrame == before + 1,
          std::to_string(before) + " then " + std::to_string(g_noteFrame));
    glitchFrameBoundary();
    glitchFrameBoundary();
    glitchFrameObserve(b.f, kBytes, nullptr);
    check("...and by two across two boundaries", g_noteFrame == before + 3,
          std::to_string(before) + " then " + std::to_string(g_noteFrame));
    glitchFrameEngineWindow(false);

    // --- 4. Geometry before Armed ------------------------------------------------
    //
    // The geometry is fixed (5376 bytes, float 1100) and has no ini key: the two
    // advanced keys that carried it were removed 2026-10-14. A stale line in an old
    // file, whatever it says, is a line this build does not read, and the tap is the
    // same. The test rigs' switch (EDVR_TEST_NO_CAMERA_TAP=1) is the only way off.
    {
        struct Case { const char* name; const char* ini; const char* env; bool tap; bool hooks; };
        const Case cases[] = {
            {"no settings at all", "[log]\r\nenabled = 0\r\n", nullptr, true, true},
            {"a stale buffer size 0 line", "[advanced]\r\ncamera_buffer_bytes=0\r\n", nullptr, true, true},
            {"a stale offset line outside the buffer", "[advanced]\r\ncamera_buffer_offset=5000\r\n", nullptr, true, true},
            {"a stale buffer of another size", "[advanced]\r\ncamera_buffer_bytes=4096\r\ncamera_buffer_offset=100\r\n", nullptr, true, true},
            {"a stale fix.transition_flash = 0 line", "[fix]\r\ntransition_flash=0\r\n", nullptr, true, true},
            {"a stale fix.transition_flash = 1 line", "[fix]\r\ntransition_flash=1\r\n", nullptr, true, true},
            {"the test switch set to 1", "[log]\r\nenabled = 0\r\n", "1", false, false},
            {"the test switch set to 0 (only 1 switches it)", "[log]\r\nenabled = 0\r\n", "0", true, true},
            {"the test switch set to 11 (only exactly 1)", "[log]\r\nenabled = 0\r\n", "11", true, true},
            {"the test switch set to an empty string", "[log]\r\nenabled = 0\r\n", "", true, true},
        };
        for (const Case& c : cases) {
            SetEnvironmentVariableA("EDVR_TEST_NO_CAMERA_TAP", c.env);
            reinstall(scratch, c.ini);
            SetEnvironmentVariableA("EDVR_TEST_NO_CAMERA_TAP", nullptr);
            char why[256] = "";
            const bool tap = glitchFrameEngineTapAvailable(why, sizeof(why));
            check((std::string("tap availability: ") + c.name).c_str(), tap == c.tap,
                  tap ? "the tap reported available" : why);
            check((std::string("wants vScreen's hooks: ") + c.name).c_str(), glitchFrameWantsHooks() == c.hooks,
                  "the hook request differs");
            if (!c.tap) check((std::string("a reason is given: ") + c.name).c_str(), why[0] != '\0', "no reason text");
        }
    }

    // A tap that was never installed answers "not available" with a reason.
    shutdownGlitchFrameFix();
    {
        char why[256] = "";
        check("a tap that is not installed is not available", !glitchFrameEngineTapAvailable(why, sizeof(why)) && why[0] != '\0');
        check("...and asks for no hooks", !glitchFrameWantsHooks());
    }

    // --- 5. A hold ---------------------------------------------------------------
    reinstall(scratch, "[log]\r\nenabled = 0\r\n");
    clearGlitchFrame();
    check("no frame is marked before a hold", !glitchFrameMarked());
    const bool honoured = glitchFrameEngineFixHold();
    check("a hold marks the frame", glitchFrameMarked());
    check("...and reports whether a compositor was there to honour it", honoured == glitchConsumerPresent());
    clearGlitchFrame();
    check("clearing the frame at its boundary releases the mark", !glitchFrameMarked());
    const uint32_t verdictBefore = jumpVerdictPacked();
    glitchFrameEngineFixVerdict();
    check("the verdict word changes when the boundary behind a hold publishes it",
          jumpVerdictPacked() != verdictBefore);
    check("...to \"the camera stayed\" (2)", (jumpVerdictPacked() & 3u) == 2u);
    check("a hold notes a world jump for the arrival census", (glitchFrameEngineFixHold(), takeWorldJump()));
    clearGlitchFrame();

    shutdownGlitchFrameFix();

    if (g_fails) {
        printf("GLITCH TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("GLITCH TEST PASSED\n");
    return 0;
}
