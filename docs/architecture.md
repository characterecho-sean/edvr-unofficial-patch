# Module and thread lifetime in the graphics DLL

Which threads run the graphics DLL's (d3d11.dll's) code without anyone waiting
for them, what keeps the DLL mapped under them, and how that was measured. Opened
by the RC4 review of 2026-09-29 (F1) and its fix. Later architecture notes go in
the journal below, dated.

## Status

State, 2026-09-29: fixed and gated. The graphics DLL is pinned once, at the first
device creation, and `tools\journal_unload_test` proves it against a real
`FreeLibrary` on every full build. Nothing rendering changed, so no flight is
owed; the one visible change is a log line.

- The pin: `initOnceCallback` (`src\d3d11\d3d11_proxy.cpp`) calls
  `pinGraphicsModuleOnce` (`src\common\module_pin.h`), which is
  `GetModuleHandleExW` with `FROM_ADDRESS | PIN`, as the OpenXR runtime does for
  itself (`src\openxr\native_module.cpp`, `configureModule`). It runs after the log
  opens and before anything detached can start, never in DllMain. The log line is
  `graphics module pinned=1 (path=...)`, or `graphics module pinned=0 (error N;
  ...)` when the loader refuses.
- Real-game exposure: Elite's exe imports `d3d11.dll` and `dxgi.dll` statically
  (`openvr_api.dll` is delay-loaded; read from the PE import table of the Frontier
  exe). A statically imported module's load count never reaches zero, so in the
  game DllMain's FreeLibrary branch cannot run. The pin makes that the DLL's own
  guarantee, for a host that does not import it. The branch stays for a host that
  loads the DLL and lets go of it before its first device creation.
- What F1 got right and wrong, measured against a DLL built from the real
  `journal_watch.cpp` and the real pin. A thread-pool callback and a `CreateThread`
  thread lose the image to an unpinned FreeLibrary. The journal worker does not.
- ruled out: "the journal worker is detached without a reference to the DLL",
  because it is a `std::thread`, and the static UCRT's `_beginthreadex` takes a
  `GetModuleHandleExW` reference on the module of the thread routine and ends the
  thread through `FreeLibraryAndExitThread` (Windows SDK 10.0.26100,
  `ucrt\startup\thread.cpp`). Unpinned, FreeLibrary during a file call leaves the
  image mapped, and the unload completes on the worker's own thread once it stops.
- ruled out: "DllMain's FreeLibrary branch runs in a session with logging on",
  because the log's flusher is a `std::thread` too, holds the DLL the same way,
  and is stopped only by that branch. The rig shows FreeLibrary leaving a DLL with
  its log open mapped, and DllMain(DETACH) not run, pinned or not.
- The one real instance in shipped code is `ui_surfaces.cpp`,
  `hmdRefreshOffThread`: `TrySubmitThreadpoolCallback(hmdRefresh, nullptr,
  nullptr)` every five seconds while `fix.ui_quality` is on (about to default to
  100). Nothing holds the DLL for a pool callback submitted that way; the pin does.
- Not changed, on purpose: the threads `shutdown()` joins (the log flusher, the
  menu ini writer, the panel raster worker). They finish before the module goes,
  but the join runs under the loader lock, and with the pin and the CRT's
  references that branch is unreachable in the game.
- Open: none for this arc. A thread made with `CreateThread`, or a pool callback,
  added later is covered by the pin.

## Threads and callbacks that run this DLL's code

    what                         started by                   ends                     kept mapped by
    log flusher                  std::thread, first note      joined in Log::close     CRT reference, pin
    menu ini writer              std::thread, first write     joined in stopWriter     CRT reference, pin
    panel raster worker          std::thread, first submit    joined in menuPanelShutdown  CRT reference, pin
    journal worker               std::thread, first tick      stop flag, never joined  CRT reference, pin
    HMD Quality refresh          TrySubmitThreadpoolCallback  one-shot callback         pin only
    debug-register helper (1)    CreateThread, awaited 2 s    by itself                pin only

The helper (`flat_camera_producer_probe.cpp`, a default-off probe; two more
went with the pose-reader and eye-base instruments, 2026-10-09) is awaited by a
thread already inside the DLL. The OpenXR runtime's threads (`frame_pacer.h`,
`owner_service.h`) are in `openvr_api.dll`, which pins itself at configure and is
not this DLL.

## Journal

### 2026-09-29: the pin, and what the rig found

F1 said the journal watcher's worker, a detached thread, runs the DLL's code with
nothing holding the DLL, so a FreeLibrary could unmap the image under it. The fix
the review proposed was a module reference for the worker's whole life, ended
through `FreeLibraryAndExitThread`. That reference would let the DLL unload only
after the worker stopped, and nothing stops the worker except the FreeLibrary
branch it would block; so the DLL would be pinned from the first frame by a
roundabout route. Pinning it outright is the same effect said plainly, and it
also covers the pool callback, which has no thread of ours to give a reference
to.

`tools\journal_unload_test` loads a DLL built from the real `journal_watch.cpp` and
the real pin, holds a thread inside it, calls FreeLibrary, and asks whether the
image is still mapped. Its controls run without the pin and must see the image go
(a `CreateThread` thread, a pool callback of `ui_surfaces.cpp`'s shape); a broken
pin fails the pinned runs and does not crash the rig, which was checked by
swapping the pin flag for `UNCHANGED_REFCOUNT`. It also loads the real
`build\d3d11.dll`, gives it a WARP device, and reads its own log for the pin line,
so the call in `initOnceCallback` is covered as shipped.
