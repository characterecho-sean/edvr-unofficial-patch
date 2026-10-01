#include "format_support_log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>

#include "../common/format_query_log.h"
#include "../common/guard.h"
#include "../common/log.h"

namespace edvr {
namespace {

// The game's queries, remembered lock-free. Zero-initialised static storage; nothing
// here runs at load.
FormatQueryLedger g_ledger;
// Queries that arrived before the log was open. They are counted and not remembered, so
// the same query asked again once the log is open still gets its line.
std::atomic<uint32_t> g_preOpen{0};
// Devices seen by formatSupportLogDevice, for "first" and for the adapter-line cap.
std::atomic<uint32_t> g_devices{0};
// Every entry into either hook, reported or not, so the closing count can say whether
// the hooks were reached at all (and a session that never reaches them can say so).
std::atomic<uint32_t> g_hookRuns{0};

// Closing-count timing, in ticks of about a second.
constexpr uint32_t kQuietTicks = 5;
constexpr uint32_t kOverdueTicks = 60;

// Faults in EDVR's own questions, and in the reports of the game's, each under a budget
// of its own: a fault costs the line and not the device hook's install (attachToDevice's
// budget covers the whole install) and not the game's answer.
FaultBudget g_selfBudget("formatSupport.selfQuery", 2);
FaultBudget g_reportBudget("formatSupport.gameQuery", 3);

void noteLine(const char* line) {
    Log::get().note("%s", line);
}

// ---- THE CALLER, WITHOUT THE LOADER LOCK ---------------------------------------
//
// "from=exe+0x..." says which call site in Elite's own image asked, which is what turns
// a line into something a disassembler can follow to the code that picks the format. A
// caller in any other module prints as its raw address: naming the module would take
// the loader lock (GetModuleFileName) on a game thread while another thread may be
// loading a DLL, and this runs on the startup path of everybody's game.
std::atomic<uintptr_t> g_exeBase{0};
std::atomic<uintptr_t> g_exeExtent{0};

void callerTag(const void* caller, char* buf, size_t cap) {
    uintptr_t base = g_exeBase.load(std::memory_order_relaxed);
    uintptr_t extent = g_exeExtent.load(std::memory_order_relaxed);
    if (!base) {
        // The main image's own headers, read once. Racing threads compute the same
        // answer, so there is nothing to lock.
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const BYTE*>(dos) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                extent = nt->OptionalHeader.SizeOfImage;
                base = reinterpret_cast<uintptr_t>(dos);
                g_exeExtent.store(extent, std::memory_order_relaxed);
                g_exeBase.store(base, std::memory_order_relaxed);
            }
        }
    }
    const uintptr_t at = reinterpret_cast<uintptr_t>(caller);
    fsdetail::TextOut o(buf, cap);
    if (base && at - base < extent) {
        o.adds("exe+0x");
        o.addx(at - base);
    } else {
        o.adds("0x");
        o.addh(at, 16);
    }
    o.finish();
}

// ---- THE SELF-QUERY -------------------------------------------------------------

// The adapter the device is on, as DXGI says. A failure names the step, because
// "no adapter line" and "the device would not say" are different findings.
void logAdapter(ID3D11Device* device, uint32_t index) {
    char line[768];
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (FAILED(hr) || !dxgi) {
        formatAdapterFailureLine(device, index, "QueryInterface(IDXGIDevice)",
                                 FAILED(hr) ? hr : E_POINTER, line, sizeof(line));
        noteLine(line);
        return;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) {
        formatAdapterFailureLine(device, index, "IDXGIDevice::GetAdapter",
                                 FAILED(hr) ? hr : E_POINTER, line, sizeof(line));
        noteLine(line);
        return;
    }
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) {
        formatAdapterFailureLine(device, index, "IDXGIAdapter::GetDesc", hr, line, sizeof(line));
        noteLine(line);
        return;
    }
    // UTF-8, with anything that would break a quoted field or a line turned into '?'.
    char name[400] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr,
                            nullptr) <= 0) {
        std::strcpy(name, "(unreadable)");
    }
    for (char* c = name; *c; ++c) {
        if (static_cast<unsigned char>(*c) < 0x20 || *c == '"') *c = '?';
    }
    FqAdapter a;
    a.device = device;
    a.index = index;
    a.description = name;
    a.vendorId = desc.VendorId;
    a.deviceId = desc.DeviceId;
    a.subSysId = desc.SubSysId;
    a.revision = desc.Revision;
    a.dedicatedVideo = desc.DedicatedVideoMemory;
    a.dedicatedSystem = desc.DedicatedSystemMemory;
    a.sharedSystem = desc.SharedSystemMemory;
    a.luidHigh = static_cast<uint32_t>(desc.AdapterLuid.HighPart);
    a.luidLow = desc.AdapterLuid.LowPart;
    formatAdapterLine(a, line, sizeof(line));
    noteLine(line);
}

// The formats the world target, its bloom chain and the depth are chosen from, asked
// of the device exactly as a game asks. Every answer is printed as hex and as names.
const DXGI_FORMAT kSelfFormats[] = {
    DXGI_FORMAT_R11G11B10_FLOAT,       // what Windows gets for the full-size world
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_R10G10B10A2_TYPELESS,  // what DXMT's session got (format 23)
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_TYPELESS,
    DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    DXGI_FORMAT_R32G8X24_TYPELESS,
    DXGI_FORMAT_R24G8_TYPELESS,
};
static_assert(sizeof(kSelfFormats) / sizeof(kSelfFormats[0]) == kFqSelfFormatLines,
              "the line budget counts one self-query line per format");
static_assert(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS) == 14 * sizeof(uint32_t),
              "the options decode reads fourteen words");
static_assert(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2) == 8 * sizeof(uint32_t),
              "the options2 decode reads eight words");

void selfQuery(ID3D11Device* device) {
    char line[768];
    for (const DXGI_FORMAT format : kSelfFormats) {
        FqSelfFormat s;
        s.format = static_cast<uint32_t>(format);
        UINT support = 0;
        s.supportHr = device->CheckFormatSupport(format, &support);
        s.support = support;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2{format, 0};
        s.support2Hr = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2));
        s.support2 = f2.OutFormatSupport2;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT f1{format, 0};
        s.viaHr = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &f1, sizeof(f1));
        s.via = f1.OutFormatSupport;
        formatSelfQueryLine(s, line, sizeof(line));
        noteLine(line);
    }
    FqSelfOptions options;
    D3D11_FEATURE_DATA_D3D11_OPTIONS o1{};
    options.hr1 = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o1, sizeof(o1));
    std::memcpy(options.words1, &o1, sizeof(o1));
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
    options.hr2 = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2));
    std::memcpy(options.words2, &o2, sizeof(o2));
    formatOptionsLine(options, line, sizeof(line));
    noteLine(line);
}

// ---- THE GAME'S QUERIES ---------------------------------------------------------

// One distinct query to a line. Everything the hook read is passed in by value, so the
// only pointer dereferenced here is the game's output buffer, and only after a
// successful call.
void noteCheckFormat(const void* caller, uint32_t format, int32_t hr, uint32_t support) {
    if (!Log::get().isOpen()) {
        g_preOpen.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t answer = hr >= 0 ? support : 0;
    const FormatQueryLedger::Verdict v =
        g_ledger.note(formatQueryKey(kFqCheckFormatSupport, format, hr, answer, 0));
    if (!v.print) return;
    FqGameQuery q;
    q.kind = kFqCheckFormatSupport;
    q.callNumber = v.callNumber;
    q.asked = format;
    q.hr = hr;
    q.answer = answer;
    q.thread = GetCurrentThreadId();
    char from[48];
    callerTag(caller, from, sizeof(from));
    q.from = from;
    char line[768];
    formatGameQueryLine(q, line, sizeof(line));
    noteLine(line);
}

void noteCheckFeature(const void* caller, uint32_t feature, const void* data, uint32_t size,
                      int32_t hr) {
    if (!Log::get().isOpen()) {
        g_preOpen.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    FqGameQuery q;
    q.hr = hr;
    q.dataSize = size;
    uint32_t answer = 0;
    // The two format queries carry {format, mask} and nothing else; a call that passes
    // any other size is not that query, so it is logged as the feature it names.
    const bool formatQuery = (feature == kFqFeatureFormatSupportId ||
                              feature == kFqFeatureFormatSupport2Id) && size == 8 && data;
    if (formatQuery) {
        const uint32_t* w = static_cast<const uint32_t*>(data);
        q.kind = feature == kFqFeatureFormatSupportId ? kFqFeatureFormatSupport
                                                      : kFqFeatureFormatSupport2;
        q.asked = w[0];
        answer = hr >= 0 ? w[1] : 0;
        q.answer = answer;
    } else {
        q.kind = kFqFeatureOther;
        q.asked = feature;
        // The output, only when the call says it wrote one. A failed call leaves the
        // buffer as the game had it, which is not an answer.
        if (hr >= 0 && data && size) {
            const uint32_t bytes = size < 256 ? size : 256;
            answer = fqFnv1a32(data, bytes);
            const uint32_t words = bytes / 4 < kFqWordsKept ? bytes / 4 : static_cast<uint32_t>(kFqWordsKept);
            std::memcpy(q.words, data, words * sizeof(uint32_t));
            q.wordCount = words;
        }
    }
    const FormatQueryLedger::Verdict v = g_ledger.note(
        formatQueryKey(q.kind, q.asked, hr, answer, q.kind == kFqFeatureOther ? size : 0));
    if (!v.print) return;
    q.callNumber = v.callNumber;
    q.thread = GetCurrentThreadId();
    char from[48];
    callerTag(caller, from, sizeof(from));
    q.from = from;
    char line[768];
    formatGameQueryLine(q, line, sizeof(line));
    noteLine(line);
}

}  // namespace

void formatSupportLogDevice(ID3D11Device* device) {
    // Nothing is spent before the log can take the lines: a device seen now, with the
    // log closed, must not use up the once-only table.
    if (!device || !Log::get().isOpen()) return;
    const uint32_t index = g_devices.fetch_add(1, std::memory_order_relaxed);
    if (index >= kFqMaxAdapterLines) return;
    guardedBudget(g_selfBudget, [&] {
        logAdapter(device, index);
        if (index == 0) selfQuery(device);
    });
}

HRESULT formatSupportCheckFormat(PFN_CheckFormatSupport real, ID3D11Device* self,
                                 DXGI_FORMAT format, UINT* support, bool report,
                                 const void* caller) {
    const HRESULT hr = real(self, format, support);
    g_hookRuns.fetch_add(1, std::memory_order_relaxed);
    if (report) {
        guardedBudget(g_reportBudget, [&] {
            // `support` is the game's pointer: read only after a success, inside the guard.
            noteCheckFormat(caller, static_cast<uint32_t>(format), static_cast<int32_t>(hr),
                            SUCCEEDED(hr) && support ? static_cast<uint32_t>(*support) : 0u);
        });
    }
    return hr;
}

HRESULT formatSupportCheckFeature(PFN_CheckFeatureSupport real, ID3D11Device* self,
                                  D3D11_FEATURE feature, void* data, UINT size, bool report,
                                  const void* caller) {
    const HRESULT hr = real(self, feature, data, size);
    g_hookRuns.fetch_add(1, std::memory_order_relaxed);
    if (report) {
        guardedBudget(g_reportBudget, [&] {
            noteCheckFeature(caller, static_cast<uint32_t>(feature), data,
                             static_cast<uint32_t>(size), static_cast<int32_t>(hr));
        });
    }
    return hr;
}

void formatSupportTick() {
    // The render thread's alone, so plain statics.
    static uint32_t lastRuns = 0;
    static uint32_t quietTicks = 0;
    static uint32_t ticksTotal = 0;
    static uint32_t ticksSinceFirst = 0;
    static uint32_t summaries = 0;
    static uint32_t summarisedRuns = ~0u;   // nothing summarised yet

    if (summaries >= kFqMaxSummaryLines) return;
    ++ticksTotal;
    // The clock is the hooks' entries, reported or not: queries that only EDVR or another
    // device made are still queries the hooks saw.
    const uint32_t runs = g_hookRuns.load(std::memory_order_relaxed);
    if (runs == 0) {
        // The hooks have not been reached at all. Say so once, after a minute of frames,
        // so that "no game lines" cannot look the same as "the hooks never ran".
        if (ticksTotal < kOverdueTicks || summaries != 0) return;
    } else {
        ++ticksSinceFirst;
        if (runs != lastRuns) {
            lastRuns = runs;
            quietTicks = 0;
        } else {
            ++quietTicks;
        }
        if (quietTicks < kQuietTicks && ticksSinceFirst < kOverdueTicks) return;
    }
    if (runs == summarisedRuns || !Log::get().isOpen()) return;

    FqSummary s;
    s.calls = g_ledger.calls();
    s.distinct = g_ledger.distinct();
    s.printed = g_ledger.printed();
    s.suppressed = g_ledger.suppressed();
    s.untracked = g_ledger.untracked();
    s.preOpen = g_preOpen.load(std::memory_order_relaxed);
    s.cap = FormatQueryLedger::kMaxLines;
    s.hookRuns = runs;
    char line[448];
    formatSummaryLine(s, line, sizeof(line));
    noteLine(line);
    ++summaries;
    summarisedRuns = runs;
    ticksSinceFirst = 0;   // a second summary waits for another quiet spell, or another minute
}

}  // namespace edvr
