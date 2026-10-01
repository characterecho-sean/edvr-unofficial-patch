#pragma once

// The flat F8 panel's graphics-wrapper note (src\d3d11\flat_wrapper_note.h).
//
// The decision: which sessions name a wrapper. An in-place session on a probe that found the context's methods outside
// Windows' d3d11.dll does, with the file that backs them; a live copy never does (EDHM and 3Dmigoto sit at 96 of 96 in
// Windows' own code and cost nothing), and neither does a mode the person forced. The words: the sentence, naming the
// file, wrapped by the panel's ruler like the settings warning, and not at all unless a temporal mode is selected. The
// probe's own finding (which module) is vtable_test's; the wiring, source scans in flat_temporal_test.cpp.

#include <cstdio>
#include <cstring>
#include <string>
#include "../../src/d3d11/flat_wrapper_note.h"

inline int flatWrapperNoteTests() {
    using namespace edvr;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: wrapper note %s\n", name); ++failures; }
    };
    auto ruler = [](const char* text, void*) { return static_cast<int>(std::strlen(text)) * 20; };   // 20 px a character
    constexpr int kWidthPx = 806;   // the flat card less its padding, at the default size

    // ---- the decision ------------------------------------------------------------------------
    const HookMode inPlace = HookMode::InPlace, live = HookMode::LiveCopy, copy = HookMode::CopyVptr;
    {
        const char* f = flatWrapperFile(inPlace, inPlace, "dxgi.dll");
        expect(f && std::strcmp(f, "dxgi.dll") == 0, "an in-place session on a wrapped context names the wrapper's file (ReShade as dxgi.dll)");
        const char* g = flatWrapperFile(inPlace, inPlace, "d3d11.dll");
        expect(g && std::strcmp(g, "d3d11.dll") == 0, "a wrapper installed under the name d3d11.dll is named by that file name");
        expect(flatWrapperFile(inPlace, inPlace, "") == nullptr && flatWrapperFile(inPlace, inPlace, nullptr) == nullptr,
               "in place, but no module was named: no note");
        expect(flatWrapperFile(live, live, "dxgi.dll") == nullptr, "a live copy (the runtime's own code backs the methods) never shows the note");
        expect(flatWrapperFile(live, live, "") == nullptr, "and EDHM at 96 of 96, with no other module to name, does not either");
        expect(flatWrapperFile(live, inPlace, "dxgi.dll") == nullptr,
               "a live copy the person forced on a wrapped context is their experiment: no note");
        expect(flatWrapperFile(copy, inPlace, "dxgi.dll") == nullptr && flatWrapperFile(copy, copy, "dxgi.dll") == nullptr,
               "a forced private copy: no note");
        expect(flatWrapperFile(inPlace, live, "dxgi.dll") == nullptr,
               "a forced in-place session on the runtime's own context: nothing is wrapped, no note");
        expect(flatWrapperFile(inPlace, copy, "dxgi.dll") == nullptr, "and the same for a probe that read a private copy");
    }

    // ---- the words ---------------------------------------------------------------------------
    {
        const char* sentence = "dxgi.dll handles every graphics call (likely ReShade): anti-aliasing costs more frame time with it.";
        FlatSettingsWarning wide;
        flatComposeWrapperNote(true, "dxgi.dll", 1000000, ruler, nullptr, &wide);
        expect(wide.count == 1 && std::strcmp(wide.line[0], sentence) == 0,
               "the sentence names the file and says what it costs, on one line when the panel is wide enough");

        FlatSettingsWarning card;
        flatComposeWrapperNote(true, "dxgi.dll", kWidthPx, ruler, nullptr, &card);
        std::string joined;
        bool fits = card.count > 1;
        for (int i = 0; i < card.count; ++i) {
            fits = fits && static_cast<int>(std::strlen(card.line[i])) * 20 <= kWidthPx;
            joined += (i ? " " : "") + std::string(card.line[i]);
        }
        expect(fits && joined == sentence, "at the card's width it wraps to several lines that fit, and read as the same sentence");
        expect(std::strstr(card.line[0], "dxgi.dll") != nullptr, "the file name is on the first line");

        FlatSettingsWarning other;
        flatComposeWrapperNote(true, "MyWrapper.dll", kWidthPx, ruler, nullptr, &other);
        expect(other.count >= 1 && std::strncmp(other.line[0], "MyWrapper.dll handles", 21) == 0,
               "another file's name is the one said");

        FlatSettingsWarning off;
        flatComposeWrapperNote(false, "dxgi.dll", kWidthPx, ruler, nullptr, &off);
        expect(off.count == 0, "with no temporal mode selected there is no note");
        flatComposeWrapperNote(true, nullptr, kWidthPx, ruler, nullptr, &off);
        expect(off.count == 0, "with no wrapper named there is no note");
        flatComposeWrapperNote(true, "", kWidthPx, ruler, nullptr, &off);
        expect(off.count == 0, "and an empty name is no wrapper");
    }
    return failures;
}
