// Carrying out a plan, and undoing it if a step fails.
//
// The dangerous half of this installer is three steps long: rename the game's
// openvr_api.dll out of the way, write ours in its place, tell the user it
// worked. Fail in the middle and the folder has no openvr_api.dll at all --
// worse than before we started, and in a way the user cannot see or name.
//
// So every step records how to undo itself, and a failure walks that list
// backwards before reporting. Backups are still taken (an undo cannot recover a
// file that was replaced rather than moved), but the ordinary interrupted run
// -- out of disk, antivirus, a file that turned out to be locked, permission
// refused -- ends with the folder exactly as it was found.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "plan.h"

namespace edvr::installer {

// Hands back the bytes of an embedded payload item ("d3d11", "openvr").
using PayloadProvider = std::function<bool(const std::string& item, const void** data, size_t* size)>;

struct ApplyResult {
    bool ok = false;
    bool needsElevation = false;  // a write was refused; the same run as admin would work
    bool rolledBack = false;
    // A file was replaced before the failure. Renames come back; replaced bytes
    // do not, so a run that overwrote something cannot be called fully undone
    // -- the copy in edvr_backup\ is the way back, and saying otherwise stops
    // anybody looking there.
    bool overwrote = false;
    std::vector<std::string> done;   // one line per completed step, for the report
    std::string error;               // empty when ok
};

ApplyResult applyPlan(const Plan& plan, const PayloadProvider& payload);

// Can this process create a file in that folder? Asked before anything is
// touched, so that "you need to run this as administrator" is offered up front
// rather than discovered halfway through a rename.
bool canWriteInto(const std::wstring& dir);

}  // namespace edvr::installer
