#pragma once
#include "session_binding.h"
namespace edvr::openxr {
// First-device candidate from the existing paired EDVR mapping. The graphics
// publisher retains a process-lifetime COM reference. This is a binding helper,
// not the future backend's complete paired-feature/capability handshake.
XrResult initializePublishedSession(SessionBinding&,const BindingDispatch&,XrInstance,XrSystemId);
}
