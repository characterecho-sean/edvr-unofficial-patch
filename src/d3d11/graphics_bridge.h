#pragma once

#include "../common/graphics_bridge.h"
#include <cstdint>

namespace edvr {

enum class HookMode : uint32_t;

// Called once, after the context vtable commit has succeeded. The first
// successful registration wins for the life of this DLL.
bool graphicsBridgeRegisterOwner(ID3D11Device* device, ID3D11DeviceContext* context);

// Optional-feature-disabled sessions still need the private ExecuteCommandList
// transport so native discovery can bind the exact immediate context. This is
// a single bounded install attempt; a failed attempt leaves discovery absent.
bool graphicsBridgeInstallTransport(ID3D11Device* device, HookMode mode);
void graphicsBridgeUninstallTransport();

// The ExecuteCommandList hook uses this to consume the one-shot private pass.
bool graphicsBridgeConsumePermit(ID3D11DeviceContext* context,
                                 ID3D11CommandList* list,
                                 BOOL restoreContextState);
void graphicsBridgeNoteUnknownExecution();

uint64_t graphicsBridgePrivateExecutionCount();
uint64_t graphicsBridgeUnknownExecutionCount();

}
