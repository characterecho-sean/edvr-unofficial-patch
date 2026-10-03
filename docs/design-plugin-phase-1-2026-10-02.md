# EDVR Phase 1 Plugin Architecture Design

## Status
- **State:** Approved, Implementation Phase 1
- **Open hypotheses:** None
- **Ruled-out pointer:** Dynamic DLL-based plugins (ruled out due to render-thread overhead, antivirus heuristics, and /MT CRT heap boundaries; Q1 monolithic static libraries adopted).
- **Next flight:** Canary migration (Night Vision in `cockpit-visuals`) validated against verdict replay test rig and relative census gate.

## Executive Summary
This document defines the Phase 1 architecture for EDVR's plugin system based on `docs/design-plugin-architecture-2026-09-30.md`. To uphold the **Performance North Star** (zero allocations, no `std::function`, no virtual fan-out, flat pre-sorted dispatch tables), EDVR adopts a **monolithic static library model** for first-party plugins. 

**Performance Gates (Relative):** Each migration phase ships only if its flight census shows EDVR's render-thread CPU and GPU cost no worse than the previous build within noise (measured at identical spots: Epic hangar in flat; carrier and on foot in VR; AA on/off). Disabled or uninstalled plugins register nothing, immediately reducing render-thread overhead.

## 1. Monolithic Static Plugin Model & Dual-Binary Host
- **No Dynamic DLLs:** First-party plugins compile directly as static libraries into the monolithic binaries (`d3d11.dll` and `openvr_api.dll`).
- **Dual-Binary Architecture (Asymmetrical Needs):** 
  - Both `d3d11.dll` and `openvr_api.dll` link the core registry logic and read the same install receipt.
  - The relationship is asymmetrical: `d3d11.dll` never calls `openvr_api.dll`. Graphics-only plugins (e.g. `exposure`) exist only in `d3d11.dll`. Dual-sided plugins (e.g. `temporal-aa`, `comfort`, `scanners`) provide a graphics module in `d3d11.dll` and an XR module in `openvr_api.dll` to handle runtime doors and submit events.
- **Authority:** The installer is the authority for installed plugins, writing the selection and exact versions to `edvr_install_receipt.json`.
- **Startup:** At initialization outside loader lock, the core registry reads the receipt and registers only installed plugins. `edvr.ini` provides runtime toggles within that active set.

## 2. Draw Claim Dispatch & Binding Shadows
To handle the 19+ distinct render verdicts efficiently without per-draw overhead:
- **Binding Shadows & Interest Cache:** When VS/PS shaders are bound, the binding shadow caches the shader interest (FNV1a64 hash lookup).
- **Dispatch Table:** A flat array of C claimant function pointers pre-sorted by declared precedence.
- **Execution:** On a draw call, only claimants matching the bound shader are invoked in declared precedence order to evaluate secondary predicates (SRV dimensions, buffer sizes, vertex counts) until one claims the verdict.

## 3. Cross-Plugin State: POD FrameContext
- All hooks receive a pointer to a single zero-allocation, POD `FrameContext` (containing published eye sizes, camera flags, shared state like `isFssActive` or motion hints).
- **Isolation:** Plugins never call each other directly. Shared state flows strictly through the core `FrameContext` or core host services.

## 4. Phase 1 Migration Strategy
1. **Core Data Table with Legacy Thunks:** The 19-verdict ladder in `vscreen.cpp` is converted into a central data table. All unmigrated features are initially wrapped as legacy thunks in the table.
2. **Census Families & Decoupling:**
   - Move GPU and CPU census families to the core registry so logging stays owned by the core.
   - Separate `static_props` and the `scheduler_stack_probe` from `temporalPassConfigure` into core frame services so deselecting `temporal-aa` never orphans them.
   - Core registry takes over draw-gate subscriptions (`drawGateSubscribed`).
3. **Canary Extraction (Night Vision):** Extract night vision from `vscreen.cpp` into a static `cockpit-visuals` module, replacing its legacy thunk in the dispatch table.
4. **Verification & Gates:**
   - **Byte-Identical Verdict Replay Rig:** Runs recorded draw streams through the new dispatch table, verifying that verdicts match the legacy ladder byte-for-byte.
   - **Receipt Verification:** Core fails gracefully if the receipt is missing or malformed.
   - **Relative Census Gate:** Flight census verifies that render-thread time is lower than legacy (uninstalled/disabled features stop walking the ladder).

## 5. File System & Manifests
Plugins reside in `src/plugins/<id>/` with an internal manifest (`plugin.manifest.json`). The installer reads manifests and outputs `edvr_install_receipt.json`. Schemas are formalized in `schemas/plugin.manifest.schema.json` and `schemas/edvr_install_receipt.schema.json`.
