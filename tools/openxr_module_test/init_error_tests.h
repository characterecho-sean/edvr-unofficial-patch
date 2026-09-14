#pragma once

#include "../../src/openxr/init_error.h"
#include <array>
#include <atomic>
#include <cstring>
#include <thread>

namespace edvr::openxr {

template<class Check>
void runInitErrorTests(Check&& check) {
  using Error = vr::EVRInitError;
  // This is the native module's emitted census, rather than a second table of
  // symbol/description mappings.  It keeps this test coupled to observable
  // initialization outcomes while the helper owns the complete enum mapping.
  constexpr std::array<Error, 11> emitted{{
    vr::VRInitError_None,
    vr::VRInitError_Init_InstallationCorrupt,
    vr::VRInitError_Init_Retry,
    vr::VRInitError_Init_ShuttingDown,
    vr::VRInitError_Init_TooManyObjects,
    vr::VRInitError_Init_HmdNotFound,
    vr::VRInitError_Init_Internal,
    vr::VRInitError_Init_NotSupportedWithCompositor,
    vr::VRInitError_Init_InterfaceNotFound,
    vr::VRInitError_Init_InvalidInterface,
    vr::VRInitError_Init_NotInitialized
  }};

  for (const Error error : emitted) {
    const char* symbol = initErrorSymbol(error);
    const char* description = initErrorDescription(error);
    check(symbol != nullptr && symbol[0] != '\0', "native init error symbol is nonempty");
    check(description != nullptr && description[0] != '\0', "native init error description is nonempty");
    check(symbol == initErrorSymbol(error), "native init error symbol pointer is stable");
    check(description == initErrorDescription(error), "native init error description pointer is stable");
    check(std::strcmp(symbol, "VRInitError_Unknown") != 0, "native init error has a known symbol");
  }
  check(std::strcmp(initErrorSymbol(vr::VRInitError_None), "VRInitError_None") == 0,
        "None has its exact init error symbol");
  check(std::strcmp(initErrorSymbol(vr::VRInitError_Init_InstallationCorrupt),
                    "VRInitError_Init_InstallationCorrupt") == 0,
        "InstallationCorrupt has its exact init error symbol");
  check(std::strcmp(initErrorSymbol(vr::VRInitError_Init_HmdNotFound),
                    "VRInitError_Init_HmdNotFound") == 0,
        "HmdNotFound has its exact init error symbol");
  check(std::strcmp(initErrorSymbol(vr::VRInitError_Init_Internal),
                    "VRInitError_Init_Internal") == 0,
        "Internal has its exact init error symbol");
  check(std::strcmp(initErrorSymbol(vr::VRInitError_Init_NotInitialized),
                    "VRInitError_Init_NotInitialized") == 0,
        "NotInitialized has its exact init error symbol");

  const Error positiveUnknown = static_cast<Error>(987654321);
  const Error negativeUnknown = static_cast<Error>(-987654321);
  const char* unknownSymbol = initErrorSymbol(positiveUnknown);
  const char* unknownDescription = initErrorDescription(positiveUnknown);
  check(std::strcmp(unknownSymbol, "VRInitError_Unknown") == 0,
        "unknown positive init error uses stable Unknown symbol");
  check(std::strcmp(initErrorSymbol(negativeUnknown), unknownSymbol) == 0,
        "unknown negative init error uses the same Unknown symbol");
  check(std::strcmp(unknownDescription, "Unknown native OpenXR initialization error") == 0,
        "unknown positive init error uses stable description");
  check(std::strcmp(initErrorDescription(negativeUnknown), unknownDescription) == 0,
        "unknown negative init error uses the same description");
  check(unknownSymbol == initErrorSymbol(positiveUnknown) &&
        unknownSymbol == initErrorSymbol(negativeUnknown),
        "unknown init error symbol pointer is stable");
  check(unknownDescription == initErrorDescription(positiveUnknown) &&
        unknownDescription == initErrorDescription(negativeUnknown),
        "unknown init error description pointer is stable");

  std::atomic<bool> concurrentOk{true};
  std::array<std::thread, 8> workers;
  for (size_t worker = 0; worker < workers.size(); ++worker) {
    workers[worker] = std::thread([&, worker] {
      for (unsigned iteration = 0; iteration < 256; ++iteration) {
        const Error error = emitted[(worker + iteration) % emitted.size()];
        const char* symbol = initErrorSymbol(error);
        const char* description = initErrorDescription(error);
        if (!symbol || !description || symbol[0] == '\0' || description[0] == '\0' ||
            std::strcmp(symbol, "VRInitError_Unknown") == 0 ||
            std::strcmp(description, "Unknown native OpenXR initialization error") == 0 ||
            symbol != initErrorSymbol(error) || description != initErrorDescription(error)) {
          concurrentOk.store(false, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();
  check(concurrentOk.load(std::memory_order_relaxed), "concurrent init error callers are stable");
}

} // namespace edvr::openxr
