// The files this installer carries, embedded in its own executable.
//
// One .exe and nothing else: a zip that has to be extracted first is where
// most manual installs go wrong (Explorer will happily run a program from
// INSIDE a zip, in a temporary folder, with none of its siblings) and it is
// the step people skip. The payload is linked in as RCDATA by build.bat, which
// generates the .rc so that a build without the game's openvr_api.dll simply
// ships without that half rather than failing to compile.
#pragma once

#include <string>

#include "plan.h"

namespace edvr::installer {

// Resource ids, matched by the generated payload.rc.
#define IDR_EDVR_D3D11 101
#define IDR_EDVR_OPENVR 102
#define IDR_EDVR_INI 103

// "d3d11" | "openvr" | "ini". False when this build carries no such item.
bool payloadItem(const std::string& item, const void** data, size_t* size);

// Version, hashes and the shipped edvr.ini text, computed once.
const PayloadInfo& payloadInfo();

}  // namespace edvr::installer
