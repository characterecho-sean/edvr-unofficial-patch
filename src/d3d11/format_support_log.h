// The device's capability answers, in the log: which adapter the game's device is on,
// what the device says about the formats the world target is chosen from, and what
// the game asks and is told.
//
// DIAGNOSTIC ONLY, and it must never change an answer the game gets. Under CrossOver
// on macOS d3d11.dll is DXMT, a D3D11-to-Metal layer, and Elite renders its full-size
// world into DXGI format 23 (R10G10B10A2_TYPELESS) where Windows gets 26
// (R11G11B10_FLOAT); Elite chooses from the device's answers to format-support
// queries, and EDVR logged none of them (it calls CheckFormatSupport for its own
// resources only). One launch with this in the build shows exactly what DXMT answers
// and what Elite asked, in the order it asked. docs\macos-dxmt-2026-09-30.md.
//
// Three parts, all on both profiles, always on, at most 58 log lines a session:
//   formatSupportLogDevice  adapter line per device (first three), and the self-query
//                           table on the first: 11 formats, both masks, plus the
//                           D3D11 options, from the device as created;
//   formatSupportCheck*     the bodies of device_hook.cpp's two pass-through hooks:
//                           forward the game's CheckFormatSupport / CheckFeatureSupport
//                           call, return its answer untouched, and log each distinct
//                           (query, format, answer) once, the first 40;
//   formatSupportTick       the closing count, once the game's queries go quiet, or
//                           after a minute if they never come (then it says the hooks
//                           ran 0 times).
// device_hook.cpp adds the 58th line: that the hooks went on, or why they could not.
// Without it, a session with no game lines could be a game that asked nothing or hooks
// that never went on.
//
// The bodies live here, not in device_hook.cpp, so tools\format_support_test runs the
// very code the hooks run, against a real device, and compares every answer with and
// without the hook. The decoding and every line format are pure and live in
// src\common\ (format_support_decode.h, format_query_log.h), pinned by the same rig.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace edvr {

// The two methods' own signatures, as the vtable holds them.
typedef HRESULT(STDMETHODCALLTYPE* PFN_CheckFormatSupport)(ID3D11Device*, DXGI_FORMAT, UINT*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CheckFeatureSupport)(ID3D11Device*, D3D11_FEATURE, void*, UINT);

// From d3d11_proxy.cpp's attachToDevice, right after the "D3D11 device ... created"
// line and before any hook goes on. Read-only calls on the device; every fault is
// contained here, so a device whose answers crash EDVR's question cannot take the
// device hook's install down with it. Safe to call with a null device and before the
// log is open (then it does nothing and spends nothing).
void formatSupportLogDevice(ID3D11Device* device);

// The hook bodies. The real call is made FIRST, unguarded and with the caller's own
// arguments, and its HRESULT is what comes back on every path; what it wrote is the
// caller's before anything here looks at it. Then, if `report` (the hook says: this is
// the game's device and the call is not EDVR's own), the answer is read and logged
// inside a fault budget of its own, so a report that faults costs the line and never
// the answer. `caller` is the hook's return address, for the "from=" tag. No lock, no
// allocation, safe on any thread and before the log is open.
HRESULT formatSupportCheckFormat(PFN_CheckFormatSupport real, ID3D11Device* self,
                                 DXGI_FORMAT format, UINT* support, bool report,
                                 const void* caller);
HRESULT formatSupportCheckFeature(PFN_CheckFeatureSupport real, ID3D11Device* self,
                                  D3D11_FEATURE feature, void* data, UINT size, bool report,
                                  const void* caller);

// About once a second from the frame boundary, render thread only: prints the closing
// count line when the hooks have been quiet for five ticks (or sixty ticks after the
// first entry, if they never are), at most twice a session. If the hooks have not been
// entered at all after sixty ticks it prints the line once, with zeros, so that
// silence from them is itself a result.
void formatSupportTick();

}  // namespace edvr
