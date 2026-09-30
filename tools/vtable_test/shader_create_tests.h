// Calls the live precompiled creation helpers through a minimal COM ABI fixture.
// No GPU: published output ownership and SEH faults are the contract under test.
#pragma once
#include "../../src/d3d11/shader_swap.h"
#include <d3d11.h>
#include <cstdint>
namespace edvr { void perfMonitorNoteEvent(uint32_t,double) {} }
namespace shaderCreateFixture {
struct Object { void** table; unsigned releases = 0; };
inline Object* device = nullptr;
inline Object* shader = nullptr;
inline unsigned mode = 0; // 1 GetDevice fault, 2 create fault, 3 failed HRESULT, 4 device Release fault
inline ULONG STDMETHODCALLTYPE release(Object* object) {
    ++object->releases;
    if (object == device && mode == 4) RaiseException(0xE042ED61u,0,0,nullptr);
    return 1;
}
inline void STDMETHODCALLTYPE getDevice(Object*, ID3D11Device** out) {
    *out = reinterpret_cast<ID3D11Device*>(device);
    if (mode == 1) RaiseException(0xE042ED62u,0,0,nullptr);
}
inline HRESULT STDMETHODCALLTYPE create(Object*, const void*, SIZE_T, ID3D11ClassLinkage*, void** out) {
    *out = shader;
    if (mode == 2) RaiseException(0xE042ED63u,0,0,nullptr);
    return mode == 3 ? E_FAIL : S_OK;
}
inline void run() {
    void* contextTable[4]{}; contextTable[3]=reinterpret_cast<void*>(&getDevice);
    void* deviceTable[19]{};
    deviceTable[2]=reinterpret_cast<void*>(&release);
    deviceTable[12]=deviceTable[15]=deviceTable[18]=reinterpret_cast<void*>(&create);
    void* shaderTable[3]{}; shaderTable[2]=reinterpret_cast<void*>(&release);
    Object context{contextTable}, dev{deviceTable}, value{shaderTable};
    device=&dev; shader=&value;
    const unsigned char bytes[] = {1};
    for(unsigned stage=0;stage<3;++stage) {
        // Successful, failed-HRESULT, and post-create device-release fault.
        // The first two stages also inject four charged faults in total,
        // below the shared production budget of five.
        for(unsigned cause : {0u,3u,4u,1u,2u}) {
            if(stage==2 && cause==1) continue; // keep five charged faults within shared budget
            dev.releases=value.releases=0; mode=cause;
            IUnknown* out=stage==2
                ? static_cast<IUnknown*>(edvr::shaderSwapCreateVs(reinterpret_cast<ID3D11DeviceContext*>(&context),bytes,sizeof(bytes),"fixture","fixture"))
                : stage==1
                ? static_cast<IUnknown*>(edvr::shaderSwapCreatePs(reinterpret_cast<ID3D11DeviceContext*>(&context),bytes,sizeof(bytes),"fixture","fixture"))
                : static_cast<IUnknown*>(edvr::shaderSwapCreateCs(reinterpret_cast<ID3D11DeviceContext*>(&context),bytes,sizeof(bytes),"fixture","fixture"));
            check((out!=nullptr)==(cause==0),"precompiled creation rejects faults and failed HRESULTs","partial shader escaped or valid creation refused");
            check(dev.releases==1,"precompiled creation releases published device once","device leaked or Release was retried after a fault");
            check(value.releases==(cause==0 || cause==1 ? 0u : 1u),"precompiled creation retires partial output once","partial shader leaked or successful output released");
            if(out) { out->Release(); check(value.releases==1,"successful output remains caller-owned","ownership was not transferred"); }
        }
    }
    mode=0; device=shader=nullptr;
}
} // namespace shaderCreateFixture
