# VDXR startup dimensions

The matching Frontier flight after system-runtime selection loaded
VirtualDesktopXR, enumerated nonzero stereo dimensions, created the separate
graphics device and completed native startup with valid geometry. Elite then
requested a zero-width, zero-height depth texture; D3D11 rejected it with
`E_INVALIDARG`, and the logs ended without normal shutdown. The subsequent
launch tripped the graphics crash sentinel, so the native provider's
`E_NOINTERFACE` on that attempt is a secondary failure.

Ruled out: continued selection of the Pimax runtime in this flight, because the
matching native trace explicitly identifies VirtualDesktopXR and reports
successful session startup.

The previous implementation exposed a concrete unsafe dependency:
`GetRecommendedRenderTargetSize` read dimensions only when the current
pose/geometry snapshot was valid. `SystemPublication::invalidate` clears that
snapshot on recenter/reference-space changes and failed geometry location. The
recommended dimensions themselves come from the session's validated view
configuration and are unchanged by those events. Losing a pose must not turn
those dimensions into zero.

The fix stores recommended per-eye dimensions as session metadata and uses them
for the System size getter and ExtendedDisplay virtual window/viewports. Frame
invalidation continues clearing current pose, projection, transforms and
temporal history; it preserves the size metadata. Session retirement clears the
metadata. This does not manufacture valid tracking or reuse a stale render
pose.

The flight log did not record the exact invalidation/getter that supplied
Elite's zero dimensions. Bounded traces now record System geometry queries,
ExtendedDisplay reads and origin invalidations, so the next flight can
distinguish recenter/reference changes, invalid tracking and unrelated
application dimensions. The desktop regression covers valid startup,
invalidated/failed geometry, stable nonzero dimensions, recovery and
retirement. The next manual flight must confirm that VDXR reaches the VR menu,
the new device metrics settle and shutdown completes; a passing desktop
regression alone does not prove that outcome.

Do not disable the crash sentinel to mask the second launch. The one-session
recovery behavior remains intact. No Oculus/LibOVR suppression is part of this
fix: this launch did reach EDVR's OpenXR facade and VirtualDesktopXR.

## Desktop regression

Running the expanded System fixture against the previous production getter
failed three checks: dimensions before the first located pose, dimensions after
explicit origin invalidation, and dimensions with invalid live geometry. The
corrected getter passes all 150 checks. The native-host fixture also exercises
the real auxiliary source and extended-display window/viewport APIs without
opening a runtime, verifying nonzero dimensions before tracking and after
invalidation, and zero dimensions after session retirement.

The full build passed with all 501 source hashes unchanged, including 150
System checks, 41 native-host checks, 234 auxiliary API checks and 63 shutdown
checks. Timing, shared transfer, stereo and module gates also pass. The
diagnostic pair is stamped `v0.16.2-84-ga8a0e57-dirty`; exact source hashes and
binaries remain in the local qualification archive. Its manual VDXR startup and
timing result remain pending.
