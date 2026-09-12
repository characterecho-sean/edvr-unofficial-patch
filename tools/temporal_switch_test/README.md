# Temporal resource-switch regression

Compile `temporal_switch_test.cpp` in a VS 2022 x64 developer shell with
`/std:c++17 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX`, linking `d3d11.lib`.
Pass the absolute path to the built EDVR `d3d11.dll` as its only argument.
The DLL directory needs an INI enabling `fix.temporal_aa=dlss` and the NVIDIA
runtime, and the test needs an RTX GPU.

The runner exercises 64 eye evaluations across native TAA, DLAA, DLSS, native
format fallback, same-size mode switches and resizes. It verifies owned output
dimensions/formats and restoration of caller SRV/UAV bindings.
