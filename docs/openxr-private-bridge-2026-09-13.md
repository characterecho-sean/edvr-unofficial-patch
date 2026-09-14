# Private compositor submissions through the graphics proxy

This checkpoint adds an explicit, versioned connection between the staged
OpenXR renderer and the actual graphics proxy. It addresses a concrete
dependency introduced by the `6de325e` main merge: the ExecuteCommandList hook
invalidates weapon, rigid-mesh and observed scene-pool history for unknown
resource writes. Restoring pipeline bindings alone does not preserve that
history. Native Frontier discovery and game-thread scheduling remain pending.

## Submission contract

The graphics proxy exports `edvrAcquireGraphicsBridge`. Version 1 uses fixed
request/table sizes, typed callbacks and an opaque lease. Discovery requires
the exact published device and its successfully hooked immediate context,
compared through canonical COM identity. A second device on the same adapter
does not qualify. Unsupported versions, missing hooks, SINGLETHREADED devices,
removed devices and a second active lease are rejected.

The consumer receives an explicit, already loaded module from its trusted host.
It retains that module by the export's address before calling it, rejects a
forwarded export from another module and releases its reference only after the
lease callback returns. It does not search by DLL basename or load an alternate
graphics implementation. Windows documents why retaining a module reference and
resolving ambiguous module names matter in
[GetModuleHandleExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw).
The existing proxy hooks and their registered first context still have process
lifetime; this lease does not add support for unloading an active graphics
proxy.

The caller must already own the immediate context exclusively. One lease is
admitted, and execution is restricted to its acquisition thread. Wrong-thread
calls fail before accessing the command list or device. Reentrant calls are
rejected, and scoped cleanup retires admission and the permit on failure,
including a C++ exception from a chained COM implementation. Release must be
serialized with lease operations. These checks do not establish exclusion
against arbitrary game rendering calls.

The renderer's explicit provider mode requires this capability before creating
XR swapchains. If binding fails, initialization fails without an unpaired
fallback. The omitted-provider mode remains for standalone diagnostic devices.
The native harness still uses that standalone mode; this checkpoint does not
claim a new headset qualification.

## Private versus unknown writes

Only the renderer's private lists may use the bridge. Every write must target a
renderer-owned resource or an acquired runtime image. This is a contract
between trusted modules, not an inspector that can prove what an arbitrary
command list contains.

The provider validates the list's device, then grants a thread-local, one-use
permit for the exact context, list pointer and `RestoreContextState == TRUE`.
It calls the normal context interface, preserving the installed hook chain. The
hook consumes that permit and skips just the two unknown-write invalidation
calls. Timing/census observation and the actual command-list execution still
run. A different list, missing permit, repeated consumption or FALSE restore
remains conservative. A list object is never permanently tagged private.

After execution, the provider flushes before returning, including when a later
hook bypassed the expected hook. A missing consumption reports failure; it does
not retry queued work. The renderer uses its existing sticky failure policy.
The provider also checks device removal before and after execution.

For ordinary game lists, `weaponStabilityResourceWritten(nullptr)` still
reaches animated-weapon and rigid-mesh history invalidation, and
`glitchFrameInvalidatePool(nullptr)` still retires observed pool writes. TRUE
restore preserves the binding shadow while those content histories are
invalidated; FALSE also forgets bindings. This follows the distinct state
semantics of
[ExecuteCommandList](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist).

## Desktop verification

The actual-DLL WARP fixture retains its existing binding, target-pixel and
renderer checks and adds bridge coverage:

- Invalid versions, structure sizes, null arguments and short-output canaries;
  same-adapter foreign devices, foreign contexts and deferred contexts.
- Missing-provider initialization before XR swapchains; duplicate leases on
  both the owner and another thread; wrong-thread rejection before COM access.
- Foreign lists and reentrant calls; exception cleanup followed by a valid
  submission that executes real GPU work.
- A previously private list replayed directly becomes unknown again.
- All eight lists from direct stereo, diagnostic eyes, copied-eye stereo and
  skybox rendering use the bridge. The 14 binding identities/generations and
  game target pixels survive, and no unknown-write branch is entered.
- An ordinary game list actually turns the game target green and enters
  unknown-write invalidation. TRUE preserves the binding shadow; FALSE clears
  it. Neither ordinary execution receives a private permit.

Read-only counters observe the actual DLL's private/unknown branch decisions.
They do not independently seed and read back live Frontier weapon/mesh history
payloads. Existing weapon and mesh GPU fixtures remain separate regression
gates; this test is not proof of in-game TAA/DLSS parity. Hook replacement
after acquisition and real device removal are guarded but not independently
injected by this fixture.

The full build passed with exit 0. The actual-proxy fixture passed 290 checks,
the stereo fixture 10,774, weapon motion 80,493 and mesh motion 1,242. Existing
OpenXR lifecycle, binding, copy, compositor and owner-service gates also
passed, as did the 254-key configuration contract. The complete output is
`build/openxr-private-bridge-build.log`; source and binary hashes are recorded
in `build/openxr-private-bridge-validation.json`. No native run was launched.
The build includes the new provider in the real graphics DLL and runs the
expanded fixture through the existing gate. Luna supplied the provider and hook
integration. Parent review tightened structure validation, ABI/export ownership
and exception cleanup, and added the consumer, renderer integration and
actual-DLL tests.

## Next integration boundary

Immediate-context ownership is still the blocker for a native game transport.
Microsoft requires serialized immediate execution in [Immediate and Deferred
Rendering](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-render).
An owner-worker mutex or this lease cannot make unsynchronized game calls safe.
The next step must keep game-device copies and playback on an explicitly owned
render-thread boundary, with a defined handoff to the XR service for loading,
frame completion and shutdown. This checkpoint neither schedules autonomous
loading on the game device nor changes ID3D11Multithread protection.

The installed Frontier pair remains `f3c205e`. No installed DLL, live INI,
registry runtime selection or saved runtime was changed. Full legacy exports,
shipping discovery, feature integration, native Frontier startup/flight/exit
and focus/device-loss qualification remain open. The previous `8ff82f2` headset
result remains evidence for its archived executable only.
