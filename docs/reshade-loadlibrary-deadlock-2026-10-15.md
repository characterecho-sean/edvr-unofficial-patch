# ReShade LoadLibrary Deadlock Guard Collision with EDHM

## Status

- **State**: Closed / Documented (2026-10-15).
- **Symptom**: Solid black menu tiles/text in all menus; HUD recoloring drops back to stock orange.
- **Root Cause**: ReShade (`dxgi.dll`) deadlock guard detours `kernel32!LoadLibraryA/W` globally. Any dynamic library load initiated while inside a graphics render callback returns `NULL`. EDHM (`d3d11_edhm.dll` / 3Dmigoto) dynamic loads of `ADVAPI32.DLL` fail, leaving texture buffer `t120` (`IniParams`) zeroed out.
- **Fix in Code**: Commit `65696ec2` in `src/d3d11/d3d11_proxy.cpp`. Preloads common system modules and patches IAT of chained proxy and game executable to service `LoadLibrary` queries via `GetModuleHandleEx`.
- **User Workaround**: Uninstalling ReShade (`dxgi.dll`) removes the hook and resolves the collision completely.

---

## Technical Analysis

### 1. The Conflict
Elite Dangerous Odyssey with EDVR proxies Direct3D 11 (`d3d11.dll`) and chains EDHM (`d3d11_edhm.dll`, 3Dmigoto v22.03) via `advanced.real_dll`. ReShade (v6.8.0) is commonly injected via `dxgi.dll`.

ReShade installs a global detour on `kernel32!LoadLibraryA` and `kernel32!LoadLibraryW`. To prevent recursive loader deadlocks when third-party software attempts to load DLLs during a DirectX frame event, ReShade evaluates `is_inside_hook()`. If true on the calling thread, it logs:
```
WARN | Ignoring LoadLibrary('ADVAPI32.DLL') call to avoid possible deadlock.
```
and returns `NULL` immediately without passing the call to Windows' loader.

### 2. Failure Manifestation in EDHM (3Dmigoto)
EDHM dynamically loads `ADVAPI32.DLL` (along with `USER32.DLL`, `GDI32.DLL`, `SETUPAPI.DLL`, `HID.DLL`, and `DINPUT.DLL`) during render callbacks to parse registry settings, theme parameters, and `ThemeSettings.json`.

When `LoadLibrary` returns `NULL`:
1. EDHM fails to initialize its theme subsystem.
2. The custom HUD color matrix and filter parameters in texture buffer `t120` (`IniParams`) remain completely unpopulated (all zeros).
3. In menu pixel shader `35aac13bbb1540de-ps.txt`:
   - Line 67: `ld_indexable(texture1d) r2.xy, l(105, 0, 0, 0), t120.zwxy` tests filter selection against UI atlas `d33c62f7`.
   - Line 70: `ld_indexable(texture1d) r4.xyz, l(159, 0, 0, 0), t120.xyzw` samples the theme color.
   - Line 81: `movc r1.xyzw, r2.zzzz, l(0,0,0,0), r1.xyzw` outputs `(0, 0, 0, 0)` -> **solid black rectangles over selected text and menu buttons**.
4. All in-cockpit HUD shaders fall back to unmodified Frontier default orange.

### 3. Resolution

#### Code-Level IAT Bypass (`edvr-unofficial-patch`)
In `src/d3d11/d3d11_proxy.cpp`:
- All standard system libraries (`advapi32.dll`, `user32.dll`, `gdi32.dll`, `setupapi.dll`, `hid.dll`, `dinput8.dll`, etc.) are preloaded during `initOnceCallback` outside of any active DirectX render callback.
- Import Address Table (IAT) hooks are installed for `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExA`, and `LoadLibraryExW` on both the chained proxy module (`d3d11_edhm.dll`) and the game executable using `iatHookInstallIn`.
- When `d3d11_edhm.dll` requests `LoadLibrary("ADVAPI32.DLL")`, the hook calls `GetModuleHandleEx` to query the already-mapped module and returns the valid `HMODULE` immediately, bypassing ReShade's detour without acquiring the loader lock.

#### Standalone User Resolution
Removing ReShade (`dxgi.dll` and its shaders) completely eliminates the deadlock-guard hook, allowing EDHM and EDVR to operate cleanly together.
