# Elite's legacy Oculus selection

The requested behavior is for Elite to reach EDVR's OpenXR backend on Meta and
older Oculus headsets, with Windows selecting the OpenXR runtime. System OpenXR
runtime selection and Elite's choice between LibOVR and OpenVR are separate
decisions. Changing the former does not force the latter.

## Evidence and limits

The installed Frontier executable's five OpenVR delay imports are
`VR_ShutdownInternal`, `VR_GetGenericInterface`, `VR_IsInterfaceVersionValid`,
`VR_GetInitToken` and `VR_InitInternal`. EDVR's facade implements this entry
path. Replacing `Openvr/win64/openvr_api.dll` only controls calls that reach
OpenVR; it cannot itself stop an earlier LibOVR choice.

The executable has no static LibOVR DLL import. Its strings include `LibOVR`,
`ovr_Initialize`, `ovr_Create`, `LIBOVR_DLL_DIR`, `%lsLibOVRRT%hs_%d.dll` and
`(Unable to load LibOVR)`. These support the existence of dynamic LibOVR
discovery. Strings alone do not prove the order of selection, the branch taken
by a particular launch, or that failing discovery safely falls back to OpenVR.

The existing [runtime detector](../src/d3d11/vr_runtime.cpp) recognizes loaded
`LibOVRRT` modules; it does not suppress them. The current executable contract
check in [openxr_pe.py](../tools/openxr_pe.py) validates OpenVR imports and
cannot establish the dynamic LibOVR decision. No LibOVR suppression, global
runtime modification or Oculus library replacement has been implemented in this
checkpoint.

## Next evidence gate

Locate the dynamic loading/initialization call site and establish whether EDVR
has a reliable earlier per-game interception point. Record module name, caller
address and selected path during a controlled launch. First look for an
existing Elite runtime-selection switch or configuration setting, verified
against the executable. Do not assume a similarly named option from another
engine works in Elite.

If interception is necessary, scope it to Elite's own legacy LibOVR probe and
verify fallback into EDVR's OpenVR facade. A blanket process-wide ban on
`LibOVRRT` is unsafe as a design assumption: the selected Meta OpenXR runtime
could itself depend on legacy runtime components. A loader hook would need
caller filtering, reentrancy and loader-lock analysis, and positive evidence
that OpenVR initialization follows refusal. Do not rename or remove system
Oculus libraries.

Qualification must include absence of the legacy SDK, its presence with a
connected Meta/Oculus headset, startup failure and fallback, rendering and
clean shutdown. Windows' selected runtime must actually support the connected
headset; forcing the entry path alone cannot establish compatibility for every
older Oculus device. Keep detailed executable offsets and local launch evidence
in the private build archive, with confirmed behavior summarized here.
