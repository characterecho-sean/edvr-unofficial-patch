# Startup crash investigation

## Verified runs

The Steam launch at 14:50 local time on September 14 used
v0.16.2-119-gd160499, with graphics build 6AA85C83 and the matching
native OpenXR module. The separate installer verification passed for the
graphics/runtime pair, pinned loader, and startup configuration. The
user's edvr.ini was unchanged.

The graphics log is edvr_gfx_20260914_145029.log; the native trace is
edvr_openxr_20260914_145030_828_23128.log. SteamVR/OpenXR initialized
successfully and published 4980 by 4916 recommendations for both eyes.
The owner thread was 32184 and the render thread was 11192. No normal
eye submissions or DLSS evaluations were recorded before termination.

The matching final breadcrumb records an access violation, 0xC0000005,
at EliteDangerous64.exe RVA 0x5CBFC8. The game's watchdog identifies
thread 29604. The existing stack scan finds no return address.

The 14:36 launch on v0.16.2-17-ga84c2a1, before the native OpenXR
update, failed at the same RVA. Its recovery launch also failed, at RVA
0x5CB322. Both addresses were already present in the September 10
v0.15.0 reporter archives, as recorded in
review-opencomposite-startup-2026-09-10.md.

Ruled out: this crash signature first appearing with the native OpenXR
merge, because the earlier legacy launch and September 10 archives
contain the same addresses. This does not exonerate shared mod code.

At 15:16, another launch of the unchanged d160499 package entered
graphics sentinel recovery. That correctly refused graphics hooks, and
native OpenXR reported graphics_unavailable=80004002. At 15:18, the next
launch of the same package progressed to normal native submissions and
recorded live producer and XR timing samples. This confirms intermittent
startup failure, not a failure on every launch.

Ruled out: an observed DLSS evaluation causing this startup exception;
none had started in these failed runs.

## Offline inspection and remaining hypotheses

The local Elite executable reports build 332841, PE timestamp
0x6A989634. The fault lies inside the function from RVA 0x5CBC80 to
0x5CC187. It allocates 0x7330 bytes of stack scratch, beyond the old
crash handler's 4096-byte raw return-address scan.

The faulting instruction reads a dword at RSP + RCX * 8 + 0xEC. RCX is
three times an index formed from a group offset and a signed candidate
index. Each group has space for 64 records of 24 bytes. Candidate
indices start at RSP + 0x40 and limits at RSP + 0x90. The group count
comes from the object at R14 + 0x8D8.

The surrounding key-state calls and record layout suggest buffered input
processing. The disassembly alone does not establish the bad index's
value or its producer. Missing lower-bound checks do not prove that a
negative index occurred: initialization normally zeros the candidate
array. An excessive group count, an excessive record count, or
overwritten state require different fixes.

The other recurring fault, RVA 0x5CB322, reads a float from RAX + RBX
* 4 after checking RBX against a device object's count. It obtains that
object from the same group-table offset, 0x5B0, as the first function.
This supports investigating the shared input-device state, while still
leaving its corruption or invalid count unproven without crash context.

The native trace's unsupported property calls at IVRSystem slots 23 and
26 precede the fault but have no demonstrated causal link. Their
signatures and bounded output handling were inspected without finding a
matching failure. They are not changed on that evidence.

There is no saved minidump or current Windows Application Error report
in the inspected crash-report locations. The existing breadcrumbs omit
the fault address, access type, registers, and local stack values.

## Diagnostic capture

The next build records bounded exception context through the existing
crash handler. Registers distinguish the group and candidate that formed
the failing address; the first 256 stack bytes include the candidate and
limit arrays. Access parameters identify the actual invalid read
address. Unwinding through Windows' existing x64 metadata can recover
callers past the large local frame.

This is diagnostic work, not a claimed crash fix. It must preserve the
game's previous exception filter, the stack-overflow bypass, and the
reentry guard. It must not add work to normal rendering or change input
events, graphics settings, or runtime selection.

## Validation

The production helper's focused test passes 330 checks. It verifies all
integer registers, full-width values, each captured stack offset,
access-violation and in-page parameters, null and truncated records,
unreadable pages, address overflow, and the eight-frame unwind limit. A
real 32 KB stack frame captured with RtlCaptureContext must unwind to
its actual caller, checked against that caller's Windows function
metadata. Each output line fits the existing breadcrumb sink.

The old raw stack scan remains as additional evidence. The previous
filter chain, reentry guard, and stack-overflow bypass are unchanged.
The new report and unwind helpers cannot inline their larger local
frames into that bypass path.

The full worktree build and all gates passed after correcting Windows
quoting of the new test's object-file path. The completed-build NVIDIA
smoke test passed with live NGX evaluations. These checks validate the
diagnostic build, not the cause of Elite's intermittent exception.

The user subsequently reported that restarting Windows stopped the
crashing and supplied new rendering captures. No crash fix is claimed,
and the diagnostics have not been installed into that working session.
