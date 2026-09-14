#include "fixture.h"
static ExportFixtureStats g_stats;
// All five wrapped exports are deliberately absent, including Init and Generic.
extern "C" __declspec(dllexport) const ExportFixtureStats* __cdecl edvrExportFixtureStats() { return &g_stats; }
