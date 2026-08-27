// Entry point: window by default, console when the command line asks for
// something specific.
//
// Built as a GUI-subsystem executable so that double-clicking it -- which is
// what nearly everybody will do -- opens the window with no console flashing
// behind it. That costs one piece of plumbing: a GUI process launched from a
// prompt has no console of its own, so a command-line run has to borrow the
// prompt's. If it cannot (launched from Explorer with arguments, say), it falls
// back to the window with those arguments prefilled rather than doing its work
// invisibly.
#include <windows.h>
#include <shellapi.h>

#include "app.h"

namespace {

// True when there is somewhere for text to go: a redirected handle inherited
// at startup, or the parent's console.
bool attachToConsole() {
    HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing && existing != INVALID_HANDLE_VALUE) return true;

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    SetStdHandle(STD_OUTPUT_HANDLE, out);
    SetStdHandle(STD_ERROR_HANDLE, out);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace edvr::installer;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const AppArgs args = parseArgs(argc ? argc : 0, argv);
    if (argv) LocalFree(argv);

    // --autorun is the elevated relaunch of a window session: it belongs in the
    // window, showing the result, not in a console nobody is watching.
    const bool wantsConsole =
        !args.autorun && (args.action != AppArgs::Act::None || args.help || args.dryRun ||
                          args.badArg);

    if (wantsConsole && attachToConsole()) return runConsole(args);
    return runGui(instance, args);
}
