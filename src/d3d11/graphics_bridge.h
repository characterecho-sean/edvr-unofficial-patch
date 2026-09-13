#pragma once

#include "../common/graphics_bridge.h"

namespace edvr {

// Called once, after the context vtable commit has succeeded. The first
// successful registration wins for the life of this DLL.
bool graphicsBridgeRegisterOwner(ID3D11Device* device, ID3D11DeviceContext* context);

// The ExecuteCommandList hook uses this to consume the one-shot private pass.
bool graphicsBridgeConsumePermit(ID3D11DeviceContext* context,
                                 ID3D11CommandList* list,
                                 BOOL restoreContextState);
void graphicsBridgeNoteUnknownExecution();

uint64_t graphicsBridgePrivateExecutionCount();
uint64_t graphicsBridgeUnknownExecutionCount();

}
