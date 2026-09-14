#include "published_session.h"
#include "../common/frame_flag.h"
namespace edvr::openxr {
XrResult initializePublishedSession(SessionBinding& binding,const BindingDispatch& api,XrInstance instance,XrSystemId system) {
  // One immediate read, no polling, fallback device or temporary session.
  return binding.initialize(api,instance,system,static_cast<ID3D11Device*>(edvr::gameDevice()));
}
}
