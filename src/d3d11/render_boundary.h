#pragma once

#include <d3d11.h>

namespace edvr {

bool renderBoundaryRegisterOwner(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;
void renderBoundaryNoteOwnedPresent(ID3D11Device* device) noexcept;
void renderBoundaryPresent(ID3D11Device* device) noexcept;

}
