#pragma once
// Isolate the complete historical types from openvr_min.h translation units.
namespace edvr {
void configureCallCensus();
void* wrapCensusInterface(void* original, const char* version);
}
