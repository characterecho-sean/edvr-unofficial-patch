#pragma once
// advanced.cull_pose, the graphics half (src/common/cull_pose.h and src/d3d11/pose_latch_patch.cpp; docs\terrain-culling.md): the mode
// names, codes and default, the instant each mode names, the engine patch (its bytes computed and DECODED, the gate and its refusals, the
// restore, the one-word atomic store on a scratch page and on a synthetic image), and the line a change writes. Which calls the runtime
// moves, and the arithmetic of the target instants, are held in tools\openxr_native_test and tools\openxr_pose_test.
#include "../../src/common/cull_pose.h"
#include "../../src/d3d11/pose_latch_patch.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace pose_cases {
namespace cp = edvr::cullpose;

// What a change writes, collected: the rig's stand-in for the log.
struct Lines {
    std::vector<std::string> v;
    void operator()(const char* line) { v.emplace_back(line); }
};
inline Lines*& sinkTarget() { static Lines* target = nullptr; return target; }
inline void sinkFn(const char* line) { if (sinkTarget()) sinkTarget()->v.emplace_back(line); }

// The 16 bytes at 0x4E36E8 of build 332841 (read from the exe): `be 11 01 00 00 00` ends the cmp at 0x4E36E7, then `0F 84 5B 01 00 00` is
// the je at 0x4E36EE, then `80 be 11 01` starts the cmp at 0x4E36F4.
inline constexpr uint8_t kOriginal16[16] = {0xBE, 0x11, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x84, 0x5B, 0x01, 0x00, 0x00, 0x80, 0xBE, 0x11, 0x01};

inline cp::LatchImage imageOf(const uint8_t* sixteen, uint32_t stamp = cp::kBuildStamp, uint32_t size = cp::kBuildImageSize, bool headers = true, bool bytesRead = true) {
    cp::LatchImage image;
    image.headers = headers;
    image.bytesRead = bytesRead;
    image.stamp = stamp;
    image.imageSize = size;
    std::memcpy(image.bytes, sixteen, 16);
    return image;
}

// A committed page the rig can write through and read back, execute-read like the game's code.
struct Page {
    uint8_t* base = nullptr;
    Page() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (base) {
            std::memset(base, 0xCC, 0x1000);
            DWORD old = 0;
            VirtualProtect(base, 0x1000, PAGE_EXECUTE_READ, &old);
        }
    }
    ~Page() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
    DWORD protection() const {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(base, &info, sizeof(info));
        return info.Protect;
    }
};

// A synthetic executable image: headers with the stamp and size, and the 16 bytes at RVA 0x4E36E8 on a page of their own.
struct Image {
    static constexpr size_t kSize = 0x500000;
    uint8_t* base = nullptr;
    bool built = false;
    Image(uint32_t stamp, uint32_t size, bool codePage, const uint8_t* sixteen = kOriginal16) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!base || !VirtualAlloc(base, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.TimeDateStamp = stamp;
        nt->OptionalHeader.SizeOfImage = size;
        if (codePage) {
            uint8_t* code = base + (cp::kLatchWordRva & ~0xFFFu);
            if (!VirtualAlloc(code, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return;
            std::memset(code, 0xCC, 0x1000);
            std::memcpy(base + cp::kLatchWordRva, sixteen, 16);
            DWORD old = 0;
            VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &old);
        }
        built = true;
    }
    ~Image() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
    uint8_t* word() const { return base + cp::kLatchWordRva; }
    DWORD codeProtection() const {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(base + cp::kLatchWordRva, &info, sizeof(info));
        return info.Protect;
    }
};

template <class Check>
void runPoseLatchCases(Check&& check) {
    // ---- the mode: names, codes, default, parsing ---------------------------------------------------------------------------------------
    {
        struct Case { const char* text; cp::Mode mode; };
        const Case cases[] = {{"display", cp::Mode::Display}, {"now", cp::Mode::Now}, {"next", cp::Mode::Next}, {"display_direct", cp::Mode::DisplayDirect},
                              {"next_direct", cp::Mode::NextDirect}, {"DISPLAY", cp::Mode::Display}, {"Now", cp::Mode::Now}, {"Next_Direct", cp::Mode::NextDirect},
                              {"DiSpLaY_dIrEcT", cp::Mode::DisplayDirect},
                              {"displays", cp::Mode::Display}, {"display_", cp::Mode::Display}, {"display direct", cp::Mode::Display}, {"direct", cp::Mode::Display},
                              {"nex", cp::Mode::Display}, {"nowhere", cp::Mode::Display}, {"off", cp::Mode::Display}, {"1", cp::Mode::Display}, {"", cp::Mode::Display},
                              {"junk", cp::Mode::Display}, {"next_direct ", cp::Mode::Display}, {" next", cp::Mode::Display}};
        bool parsed = true;
        for (const Case& c : cases) parsed = parsed && cp::parseMode(c.text) == c.mode;
        check(parsed && cp::parseMode(nullptr) == cp::Mode::Display,
              "advanced.cull_pose parses display, now, next, display_direct and next_direct in any case; a near miss, a number, off, a space, an empty value and junk read display, the default");
        check(static_cast<uint32_t>(cp::Mode::Display) == 0 && static_cast<uint32_t>(cp::Mode::Now) == 1 && static_cast<uint32_t>(cp::Mode::Next) == 2 &&
                  static_cast<uint32_t>(cp::Mode::DisplayDirect) == 3 && static_cast<uint32_t>(cp::Mode::NextDirect) == 4 && cp::kModeCodes == 5,
              "the codes EdvrNativeFrameOutput::cullPose carries are 0 display, 1 now, 2 next, 3 display_direct, 4 next_direct: the zero a half that cannot carry the field leaves is the default");
        bool names = true;
        for (uint32_t code = 0; code < 5; ++code) names = names && cp::parseMode(cp::modeName(cp::modeFromCode(code))) == cp::modeFromCode(code);
        check(names && cp::modeFromCode(5) == cp::Mode::Display && cp::modeFromCode(0xFFFFFFFFu) == cp::Mode::Display && std::strcmp(cp::modeName(cp::Mode::NextDirect), "next_direct") == 0 &&
                  std::strcmp(cp::modeName(cp::Mode::Display), "display") == 0 && std::strcmp(cp::modeName(cp::Mode::Now), "now") == 0,
              "every code round-trips through its name; a code past 4 is the default");
        check(cp::timeOf(cp::Mode::Now) == cp::Time::Now && cp::timeOf(cp::Mode::Display) == cp::Time::Display && cp::timeOf(cp::Mode::DisplayDirect) == cp::Time::Display &&
                  cp::timeOf(cp::Mode::Next) == cp::Time::Next && cp::timeOf(cp::Mode::NextDirect) == cp::Time::Next,
              "display and display_direct locate at the display time, next and next_direct one period later, now as before the fix");
        check(!cp::wantsBypass(cp::Mode::Display) && !cp::wantsBypass(cp::Mode::Now) && !cp::wantsBypass(cp::Mode::Next) && cp::wantsBypass(cp::Mode::DisplayDirect) &&
                  cp::wantsBypass(cp::Mode::NextDirect),
              "only the _direct modes want the engine patch");
        check(std::strcmp(cp::locatedAt(cp::Mode::Display), "the frame's display time") == 0 && std::strcmp(cp::locatedAt(cp::Mode::DisplayDirect), "the frame's display time") == 0 &&
                  std::strcmp(cp::locatedAt(cp::Mode::Next), "display time + one period") == 0 && std::strcmp(cp::locatedAt(cp::Mode::NextDirect), "display time + one period") == 0 &&
                  std::strcmp(cp::locatedAt(cp::Mode::Now), "now+prediction") == 0,
              "each mode says where it locates, in the words of the change line");
    }
    // ---- the patch bytes, computed and decoded ---------------------------------------------------------------------------------------
    {
        check(cp::latchTarget(cp::kLatchOriginal) == 0x4E384F && cp::latchTarget(cp::kLatchPatched) == 0x4E384F && cp::kLatchTargetRva == 0x4E384F,
              "the original `0F 84 5B 01 00 00` (je, 0x4E36EE + 6 + 0x15B) and the patched `90 E9 5B 01 00 00` (nop, then jmp ending at the same 0x4E36F4) both decode to 0x4E384F");
        const uint8_t junk[6] = {0x0F, 0x85, 0x5B, 0x01, 0x00, 0x00}, jump8[6] = {0xEB, 0x05, 0x00, 0x00, 0x00, 0x00}, otherDisp[6] = {0x90, 0xE9, 0x5C, 0x01, 0x00, 0x00};
        check(cp::latchTarget(junk) == 0 && cp::latchTarget(jump8) == 0 && cp::latchTarget(otherDisp) == 0x4E3850,
              "the decoder knows two forms only (jne and a short jump are neither), and reads a changed displacement as the changed target");
        const uint64_t patched = cp::latchWord(kOriginal16, true);
        uint8_t bytes[8];
        std::memcpy(bytes, &patched, 8);
        check(std::memcmp(bytes, kOriginal16, 6) == 0 && bytes[6] == 0x90 && bytes[7] == 0xE9, "the patched word is the current word with bytes 6 and 7 replaced by 90 E9, the six before them untouched");
        uint8_t after[16];
        std::memcpy(after, kOriginal16, 16);
        std::memcpy(after, &patched, 8);
        check(cp::latchTarget(after + cp::kLatchBranchOffset) == 0x4E384F && std::memcmp(after + 8, kOriginal16 + 8, 8) == 0 && after[8] == 0x5B && after[9] == 0x01,
              "...and with the displacement bytes in the next word unchanged the patched branch still decodes to 0x4E384F");
        const uint64_t restored = cp::latchWord(after, false);
        uint64_t original;
        std::memcpy(&original, kOriginal16, 8);
        check(restored == original, "restoring is the same arithmetic the other way: the original word, bit for bit");
        check(cp::kLatchWordRva % 8 == 0 && cp::kLatchBranchRva == cp::kLatchWordRva + 6 && cp::kLatchBranchRva == 0x4E36EE && cp::kLatchWordRva == 0x4E36E8,
              "both bytes sit in the aligned eight-byte word 0x4E36E8..0x4E36EF");
    }
    // ---- the gate and its refusals ------------------------------------------------------------------------------------------------------
    {
        check(cp::latchGate(imageOf(kOriginal16), false) == nullptr, "build 332841's stamp and size and the six original bytes open the gate");
        const char* noHeaders = cp::latchGate(imageOf(kOriginal16, 0, 0, false, false), false);
        check(noHeaders && std::strcmp(noHeaders, "the executable's headers could not be read") == 0, "...headers that cannot be read refuse");
        const char* stamp = cp::latchGate(imageOf(kOriginal16, cp::kBuildStamp + 1), false);
        const char* size = cp::latchGate(imageOf(kOriginal16, cp::kBuildStamp, cp::kBuildImageSize + 1), false);
        const char* stampLow = cp::latchGate(imageOf(kOriginal16, cp::kBuildStamp - 1), false);
        check(stamp && size && stampLow && std::strcmp(stamp, "not build 332841") == 0 && std::strcmp(size, "not build 332841") == 0 && std::strcmp(stampLow, "not build 332841") == 0,
              "...a stamp or an image size one off, either way, is not build 332841");
        const char* noBytes = cp::latchGate(imageOf(kOriginal16, cp::kBuildStamp, cp::kBuildImageSize, true, false), false);
        check(noBytes && std::strcmp(noBytes, "the branch's bytes could not be read") == 0, "...bytes that could not be read refuse");
        bool everyByte = true, patchedForm = true;
        for (unsigned i = 0; i < 6; ++i) {
            uint8_t changed[16];
            std::memcpy(changed, kOriginal16, 16);
            changed[cp::kLatchBranchOffset + i] ^= 0x01;
            const char* why = cp::latchGate(imageOf(changed), false);
            everyByte = everyByte && why && std::strcmp(why, "the branch's bytes differ from build 332841's") == 0;
        }
        check(everyByte, "...any one of the six branch bytes changed refuses");
        uint8_t already[16];
        std::memcpy(already, kOriginal16, 16);
        const uint64_t alreadyWord = cp::latchWord(kOriginal16, true);
        std::memcpy(already, &alreadyWord, 8);
        patchedForm = cp::latchGate(imageOf(already), true) == nullptr && cp::latchGate(imageOf(already), false) != nullptr && cp::latchGate(imageOf(kOriginal16), true) != nullptr &&
                      std::strcmp(cp::latchGate(imageOf(kOriginal16), true), "the branch is not the bytes this patch wrote") == 0;
        check(patchedForm, "the gate for a restore wants the patched bytes and refuses the original ones; for an apply it wants the original ones and refuses the patched");
        uint8_t otherByteInWord[16];
        std::memcpy(otherByteInWord, kOriginal16, 16);
        otherByteInWord[0] = 0x00;
        otherByteInWord[15] = 0x00;
        check(cp::latchGate(imageOf(otherByteInWord), false) == nullptr, "(bytes of the word outside the branch are not judged: only the six are)");
    }
    // ---- the store: one aligned word, on a scratch page ------------------------------------------------------------------------------
    {
        // The page is made writable once to lay the 16 bytes down, then handed back as execute-read for the store under test.
        Page page;
        check(page.base != nullptr && page.protection() == PAGE_EXECUTE_READ, "(the scratch page is execute-read, like the game's code)");
        DWORD old = 0;
        VirtualProtect(page.base, 0x1000, PAGE_READWRITE, &old);
        std::memcpy(page.base + 0x40, kOriginal16, 16);
        VirtualProtect(page.base, 0x1000, PAGE_EXECUTE_READ, &old);
        const uint64_t patched = cp::latchWord(kOriginal16, true);
        const bool stored = cp::writeWordAtomic(page.base + 0x40, patched);
        check(stored && std::memcmp(page.base + 0x40, kOriginal16, 6) == 0 && page.base[0x40 + 6] == 0x90 && page.base[0x40 + 7] == 0xE9 && std::memcmp(page.base + 0x48, kOriginal16 + 8, 8) == 0 &&
                  page.base[0x3F] == 0xCC && page.base[0x50] == 0xCC,
              "the atomic store changes exactly the eight bytes it was given: the branch's two opcode bytes, nothing before or after");
        check(page.protection() == PAGE_EXECUTE_READ, "...and hands the page back execute-read");
        const uint64_t back = cp::latchWord(page.base + 0x40, false);
        check(cp::writeWordAtomic(page.base + 0x40, back) && std::memcmp(page.base + 0x40, kOriginal16, 16) == 0, "...and the restore puts all sixteen bytes back");
        uint8_t before[16];
        std::memcpy(before, page.base + 0x40, 16);
        check(!cp::writeWordAtomic(page.base + 0x41, patched) && !cp::writeWordAtomic(page.base + 0x44, patched) && !cp::writeWordAtomic(page.base + 0x47, patched) &&
                  !cp::writeWordAtomic(nullptr, patched) && std::memcmp(page.base + 0x40, before, 16) == 0,
              "an address that is not eight-byte aligned, or null, is refused and nothing is stored");
        check(page.protection() == PAGE_EXECUTE_READ, "(a refusal leaves the page as it was)");
        // No half-written branch is ever seen: a reader that never stops looks at the word while the other side flips it.
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0}, seen{0};
        uint64_t originalWord, patchedWord;
        std::memcpy(&originalWord, kOriginal16, 8);
        patchedWord = cp::latchWord(kOriginal16, true);
        std::thread reader([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const uint64_t value = *reinterpret_cast<volatile const uint64_t*>(page.base + 0x40);
                seen.fetch_add(1, std::memory_order_relaxed);
                if (value != originalWord && value != patchedWord) torn.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (int i = 0; i < 20000; ++i) cp::writeWordAtomic(page.base + 0x40, (i & 1) ? originalWord : patchedWord);
        stop.store(true);
        reader.join();
        check(torn.load() == 0 && seen.load() > 0, "20000 flips between the two forms under a reader that never stops: it saw only whole words, never a half-written branch");
        // ...and the primitive on its own, a million times faster: the protection calls dominate the loop above, and a store done in two pieces
        // would only show in a window a few cycles wide.
        Page writable;
        DWORD wOld = 0;
        VirtualProtect(writable.base, 0x1000, PAGE_READWRITE, &wOld);
        std::memcpy(writable.base + 0x40, kOriginal16, 16);
        std::atomic<bool> stopTight{false};
        std::atomic<uint64_t> tornTight{0}, seenTight{0};
        std::thread tightReader([&] {
            while (!stopTight.load(std::memory_order_relaxed)) {
                const uint64_t value = *reinterpret_cast<volatile const uint64_t*>(writable.base + 0x40);
                seenTight.fetch_add(1, std::memory_order_relaxed);
                if (value != originalWord && value != patchedWord) tornTight.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (int i = 0; i < 20000000; ++i) cp::storeWordAtomic(writable.base + 0x40, (i & 1) ? originalWord : patchedWord);
        stopTight.store(true);
        tightReader.join();
        check(tornTight.load() == 0 && seenTight.load() > 0, "20 million flips of the bare store under a reader that never stops: only whole words, ever");
    }
    // ---- the controller: apply, hold, restore, refuse ----------------------------------------------------------------------------------
    {
        Page scratch;
        DWORD old = 0;
        VirtualProtect(scratch.base, 0x1000, PAGE_READWRITE, &old);
        std::memcpy(scratch.base + 0x40, kOriginal16, 16);
        VirtualProtect(scratch.base, 0x1000, PAGE_EXECUTE_READ, &old);
        unsigned reads = 0, writes = 0;
        uint32_t stamp = cp::kBuildStamp, size = cp::kBuildImageSize;
        const auto read = [&] {
            ++reads;
            return imageOf(scratch.base + 0x40, stamp, size);
        };
        const auto write = [&](uint64_t word) {
            ++writes;
            return cp::writeWordAtomic(scratch.base + 0x40, word);
        };
        cp::LatchBypass latch;
        latch.update(false, read, write);
        check(latch.kind() == cp::LatchBypass::Kind::Off && reads == 0 && writes == 0, "not wanted: nothing is read and nothing is written");
        latch.update(true, read, write);
        check(latch.kind() == cp::LatchBypass::Kind::On && latch.applied() && writes == 1 && scratch.base[0x46] == 0x90 && scratch.base[0x47] == 0xE9 && cp::latchTarget(scratch.base + 0x46) == 0x4E384F,
              "wanted: the gate opens, one word is stored, and the branch decodes to nop + jmp 0x4E384F");
        const unsigned readsAfterApply = reads;
        for (int i = 0; i < 5; ++i) latch.update(true, read, write);
        check(writes == 1 && reads == readsAfterApply, "...held: later frames read and write nothing");
        latch.update(false, read, write);
        check(latch.kind() == cp::LatchBypass::Kind::Off && writes == 2 && std::memcmp(scratch.base + 0x40, kOriginal16, 16) == 0, "the mode leaving _direct restores the original bytes, all sixteen");
        latch.update(false, read, write);
        check(writes == 2, "...once");
        // A refusal is final until the mode leaves bypass.
        stamp = cp::kBuildStamp + 1;
        latch.update(true, read, write);
        check(latch.kind() == cp::LatchBypass::Kind::Refused && std::strcmp(latch.why(), "not build 332841") == 0 && writes == 2 && std::memcmp(scratch.base + 0x40, kOriginal16, 16) == 0,
              "another build: refused with the reason, nothing written");
        const unsigned readsAtRefusal = reads;
        stamp = cp::kBuildStamp;
        latch.update(true, read, write);
        check(latch.kind() == cp::LatchBypass::Kind::Refused && reads == readsAtRefusal && writes == 2, "...and not retried on the next frame, though the image would now pass");
        latch.update(false, read, write);
        check(latch.kind() == cp::LatchBypass::Kind::Off && writes == 2, "leaving the mode clears the refusal");
        latch.update(true, read, write);
        check(latch.kind() == cp::LatchBypass::Kind::On && writes == 3, "...so wanting it again tries again");
        // Someone else changed the branch under us: the restore refuses and writes nothing.
        DWORD o2 = 0;
        VirtualProtect(scratch.base, 0x1000, PAGE_READWRITE, &o2);
        scratch.base[0x47] = 0xEB;
        VirtualProtect(scratch.base, 0x1000, PAGE_EXECUTE_READ, &o2);
        latch.update(false, read, write);
        check(latch.kind() == cp::LatchBypass::Kind::Refused && !latch.applied() && writes == 3 && scratch.base[0x47] == 0xEB &&
                  std::strcmp(latch.why(), "the branch is not the bytes this patch wrote") == 0,
              "a restore over bytes this did not write refuses and writes nothing");
        // A write that fails is a refusal too.
        cp::LatchBypass failing;
        DWORD o3 = 0;
        VirtualProtect(scratch.base, 0x1000, PAGE_READWRITE, &o3);
        std::memcpy(scratch.base + 0x40, kOriginal16, 16);
        VirtualProtect(scratch.base, 0x1000, PAGE_EXECUTE_READ, &o3);
        failing.update(true, read, [](uint64_t) { return false; });
        check(failing.kind() == cp::LatchBypass::Kind::Refused && std::strcmp(failing.why(), "the write failed") == 0 && !failing.applied(), "a store that fails is refused with that reason");
        // Different bytes at the branch: refused, untouched.
        cp::LatchBypass altered;
        DWORD o4 = 0;
        VirtualProtect(scratch.base, 0x1000, PAGE_READWRITE, &o4);
        scratch.base[0x47] = 0x85;
        VirtualProtect(scratch.base, 0x1000, PAGE_EXECUTE_READ, &o4);
        writes = 0;
        altered.update(true, read, write);
        check(altered.kind() == cp::LatchBypass::Kind::Refused && writes == 0 && scratch.base[0x47] == 0x85 &&
                  std::strcmp(altered.why(), "the branch's bytes differ from build 332841's") == 0,
              "a branch that is not the original six bytes is refused and left alone");
    }
    // ---- the line a change writes, and what the runtime is told -------------------------------------------------------------------------
    {
        char line[256];
        using Kind = cp::LatchBypass::Kind;
        check(std::strcmp(cp::poseLine(line, cp::Mode::Display, Kind::Off, ""), "cull pose: display -- Elite's game-thread head pose is located at the frame's display time; latched-pose bypass off") == 0,
              "display: `cull pose: display -- Elite's game-thread head pose is located at the frame's display time; latched-pose bypass off`");
        check(std::strcmp(cp::poseLine(line, cp::Mode::Now, Kind::Off, ""), "cull pose: now -- Elite's game-thread head pose is located at now+prediction; latched-pose bypass off") == 0,
              "now: located at now+prediction, bypass off");
        check(std::strcmp(cp::poseLine(line, cp::Mode::Next, Kind::Off, ""), "cull pose: next -- Elite's game-thread head pose is located at display time + one period; latched-pose bypass off") == 0,
              "next: located at display time + one period, bypass off");
        check(std::strcmp(cp::poseLine(line, cp::Mode::DisplayDirect, Kind::On, ""), "cull pose: display_direct -- Elite's game-thread head pose is located at the frame's display time; latched-pose bypass on") == 0 &&
                  std::strcmp(cp::poseLine(line, cp::Mode::NextDirect, Kind::On, ""), "cull pose: next_direct -- Elite's game-thread head pose is located at display time + one period; latched-pose bypass on") == 0,
              "display_direct and next_direct with the patch in: bypass on");
        check(std::strcmp(cp::poseLine(line, cp::Mode::NextDirect, Kind::Refused, "the branch's bytes differ from build 332841's"),
                          "cull pose: next_direct -- Elite's game-thread head pose is located at display time + one period; latched-pose bypass refused: the branch's bytes differ from build 332841's") == 0,
              "a refused patch says why, on the same line");
    }
    {
        Page scratch;
        DWORD old = 0;
        VirtualProtect(scratch.base, 0x1000, PAGE_READWRITE, &old);
        std::memcpy(scratch.base + 0x40, kOriginal16, 16);
        VirtualProtect(scratch.base, 0x1000, PAGE_EXECUTE_READ, &old);
        uint32_t stamp = cp::kBuildStamp;
        const auto read = [&] { return imageOf(scratch.base + 0x40, stamp, cp::kBuildImageSize); };
        const auto write = [&](uint64_t word) { return cp::writeWordAtomic(scratch.base + 0x40, word); };
        cp::Driver driver;
        Lines lines;
        check(driver.frame(cp::Mode::Display, read, write, lines) == 0u && lines.v.empty(), "a key left at its default writes nothing, and the runtime is told 0");
        check(driver.frame(cp::Mode::Now, read, write, lines) == 1u && lines.v.size() == 1 &&
                  lines.v[0] == "cull pose: now -- Elite's game-thread head pose is located at now+prediction; latched-pose bypass off",
              "now: the runtime is told 1, and one line says so");
        driver.frame(cp::Mode::Now, read, write, lines);
        check(lines.v.size() == 1, "...said once");
        check(driver.frame(cp::Mode::Next, read, write, lines) == 2u && lines.v.size() == 2 &&
                  lines.v[1] == "cull pose: next -- Elite's game-thread head pose is located at display time + one period; latched-pose bypass off",
              "next: 2, a new line");
        check(driver.frame(cp::Mode::DisplayDirect, read, write, lines) == 3u && lines.v.size() == 3 && driver.latch().applied() &&
                  lines.v[2] == "cull pose: display_direct -- Elite's game-thread head pose is located at the frame's display time; latched-pose bypass on" && scratch.base[0x47] == 0xE9,
              "display_direct: 3, the patch goes in, and the line says on");
        check(driver.frame(cp::Mode::NextDirect, read, write, lines) == 4u && lines.v.size() == 4 && driver.latch().applied() &&
                  lines.v[3] == "cull pose: next_direct -- Elite's game-thread head pose is located at display time + one period; latched-pose bypass on",
              "next_direct: 4, the patch stays in, a new line");
        check(driver.frame(cp::Mode::Next, read, write, lines) == 2u && !driver.latch().applied() && std::memcmp(scratch.base + 0x40, kOriginal16, 16) == 0 && lines.v.size() == 5 &&
                  lines.v[4] == "cull pose: next -- Elite's game-thread head pose is located at display time + one period; latched-pose bypass off",
              "leaving _direct restores the bytes in the same frame boundary and the line says off");
        check(driver.frame(cp::Mode::Display, read, write, lines) == 0u && lines.v.size() == 6 &&
                  lines.v[5] == "cull pose: display -- Elite's game-thread head pose is located at the frame's display time; latched-pose bypass off",
              "back to the default: 0, and the change is written");
        stamp = cp::kBuildStamp + 1;
        check(driver.frame(cp::Mode::NextDirect, read, write, lines) == 4u && lines.v.size() == 7 && !driver.latch().applied() && std::memcmp(scratch.base + 0x40, kOriginal16, 16) == 0 &&
                  lines.v[6] == "cull pose: next_direct -- Elite's game-thread head pose is located at display time + one period; latched-pose bypass refused: not build 332841",
              "next_direct on another build: the runtime is still told 4 (the time part needs no build), the patch is refused and not written, one line");
        driver.frame(cp::Mode::NextDirect, read, write, lines);
        check(lines.v.size() == 7, "...said once, not on every frame");
        check(driver.frame(cp::Mode::Next, read, write, lines) == 2u && lines.v.size() == 8 && driver.latch().kind() == cp::LatchBypass::Kind::Off,
              "leaving the _direct mode clears the refusal, with its line");
    }
    // ---- the production reads and the whole chain, on a synthetic image ------------------------------------------------------------------
    {
        Image image(cp::kBuildStamp, cp::kBuildImageSize, true);
        check(image.built, "(the synthetic image is built)");
        if (image.built) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(image.base);
            const cp::LatchImage read = cp::readLatchImage(base);
            check(read.headers && read.bytesRead && read.stamp == cp::kBuildStamp && read.imageSize == cp::kBuildImageSize && std::memcmp(read.bytes, kOriginal16, 16) == 0,
                  "readLatchImage reads the stamp, the size and the sixteen bytes from 0x4E36E8");
            cp::resetForTest();
            Lines lines;
            sinkTarget() = &lines;
            check(cp::frameFor(base, cp::Mode::DisplayDirect, &sinkFn) == 3u && image.word()[6] == 0x90 && image.word()[7] == 0xE9 && cp::latchTarget(image.word() + 6) == 0x4E384F &&
                      image.codeProtection() == PAGE_EXECUTE_READ && cp::driver().latch().applied(),
                  "frameFor on an image of build 332841: display_direct patches the real address, the branch decodes to nop + jmp 0x4E384F, and the page stays execute-read");
            check(cp::frameFor(base, cp::Mode::Display, &sinkFn) == 0u && std::memcmp(image.word(), kOriginal16, 16) == 0 && image.codeProtection() == PAGE_EXECUTE_READ && !cp::driver().latch().applied(),
                  "...and the default puts the sixteen bytes back");
            check(lines.v.size() == 2, "(two changes, two lines)");
            sinkTarget() = nullptr;
            cp::resetForTest();
        }
        struct Variant { const char* name; uint32_t stampDelta, sizeDelta; bool code; };
        const Variant variants[] = {{"another build's stamp", 1, 0, true}, {"another build's size", 0, 16, true}, {"an image smaller than 0x4E36E8", 0, 0, false}};
        for (const Variant& v : variants) {
            Image other(cp::kBuildStamp + v.stampDelta, cp::kBuildImageSize + v.sizeDelta, v.code);
            Lines lines;
            sinkTarget() = &lines;
            cp::resetForTest();
            bool ok = other.built;
            if (ok) {
                const uintptr_t base = reinterpret_cast<uintptr_t>(other.base);
                uint8_t before[16] = {};
                if (v.code) std::memcpy(before, other.word(), 16);
                const uint32_t told = cp::frameFor(base, cp::Mode::NextDirect, &sinkFn);
                cp::frameFor(base, cp::Mode::NextDirect, &sinkFn);
                ok = told == 4u && lines.v.size() == 1 && lines.v[0].find("latched-pose bypass refused: ") != std::string::npos && !cp::driver().latch().applied() &&
                     (!v.code || std::memcmp(other.word(), before, 16) == 0);
            }
            char name[160];
            std::snprintf(name, sizeof(name), "the gate on %s: the patch is refused and nothing is written, with one line", v.name);
            check(ok, name);
            sinkTarget() = nullptr;
            cp::resetForTest();
        }
    }
}

}  // namespace pose_cases
