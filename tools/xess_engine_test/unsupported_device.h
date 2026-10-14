// A D3D11 device that supports nothing: QueryInterface answers only IUnknown and ID3D11Device, so xessAvailable's first probe step
// (ID3D11Device5, the shared fences) fails on it, deterministically and without a GPU. It counts the QueryInterface calls and
// the references held on it, which is how the rig tells a fresh probe from a cached failure and sees the backend release it.
// Every other method is an unused stub. Generated from the Windows SDK's ID3D11Device; nothing here is XeSS-specific.
#pragma once
#include <windows.h>
#include <d3d11.h>

#pragma warning(push)
#pragma warning(disable : 4100)   // the stubs name the SDK's parameters and ignore them
struct UnsupportedD3D11Device final : ID3D11Device {
    unsigned queries = 0;   // QueryInterface calls: one per probe step that reaches the device
    LONG refs = 1;          // the test's own reference

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        ++queries;
        if (out) *out = nullptr;
        if (!out || (!IsEqualIID(iid, __uuidof(IUnknown)) && !IsEqualIID(iid, __uuidof(ID3D11Device)))) return E_NOINTERFACE;
        *out = static_cast<ID3D11Device*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs)); }   // stack object: never freed
    virtual HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Buffer **ppBuffer) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture1D **ppTexture1D) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture3D **ppTexture3D) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc, ID3D11ShaderResourceView **ppSRView) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc, ID3D11UnorderedAccessView **ppUAView) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc, ID3D11RenderTargetView **ppRTView) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements, const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout **ppInputLayout) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void *pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY *pSODeclaration, UINT NumEntries, const UINT *pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11HullShader **ppHullShader) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11DomainShader **ppDomainShader) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11ComputeShader **ppComputeShader) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage **ppLinkage) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc, ID3D11BlendState **ppBlendState) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *pDepthStencilDesc, ID3D11DepthStencilState **ppDepthStencilState) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc, ID3D11RasterizerState **ppRasterizerState) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC *pQueryDesc, ID3D11Query **ppQuery) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC *pPredicateDesc, ID3D11Predicate **ppPredicate) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC *pCounterDesc, ID3D11Counter **ppCounter) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT ContextFlags, ID3D11DeviceContext **ppDeferredContext) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT *pNumQualityLevels) override { return E_NOTIMPL; }
    virtual void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO *pCounterInfo) override {}
    virtual HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC *pDesc, D3D11_COUNTER_TYPE *pType, UINT *pActiveCounters, LPSTR szName, UINT *pNameLength, LPSTR szUnits, UINT *pUnitsLength, LPSTR szDescription, UINT *pDescriptionLength) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override { return E_NOTIMPL; }
    virtual D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel(void) override { return D3D_FEATURE_LEVEL_11_0; }
    virtual UINT STDMETHODCALLTYPE GetCreationFlags(void) override { return 0; }
    virtual HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason(void) override { return E_NOTIMPL; }
    virtual void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) override {}
    virtual HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override { return E_NOTIMPL; }
    virtual UINT STDMETHODCALLTYPE GetExceptionMode(void) override { return 0; }
};
#pragma warning(pop)
