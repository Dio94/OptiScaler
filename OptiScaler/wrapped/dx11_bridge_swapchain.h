#pragma once

#include "SysUtils.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <functional>

// Presents a D3D11 game through a D3D12 swapchain.
// The game renders into a shared D3D11 texture which is copied to the D3D12 backbuffer on Present,
// so D3D12 only features (like frame generation) can sit on the presentation side.
class DECLSPEC_UUID("6c1f4a2e-93b5-4f0e-8d7a-2b9e5c3d1a77") Dx11BridgeSwapChain final : public IDXGISwapChain4
{
  public:
    using CreateDx12SwapChainFn =
        std::function<HRESULT(IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain)>;

    static HRESULT Create(ID3D11Device* device, const DXGI_SWAP_CHAIN_DESC* desc,
                          const CreateDx12SwapChainFn& createSwapChain, IDXGISwapChain** ppSwapChain);

    // implement IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // implement IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // implement IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    // implement IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                            UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

    // implement IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override;

    // implement IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override;

    // implement IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                     UINT* pColorSpaceSupport) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override;

    // implement IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override;

    ID3D12Device* Dx12Device() const { return _d12; }
    ID3D12CommandQueue* Dx12Queue() const { return _queue; }

  private:
    explicit Dx11BridgeSwapChain(ID3D11Device* device);
    ~Dx11BridgeSwapChain();

    HRESULT Init(const DXGI_SWAP_CHAIN_DESC* desc, const CreateDx12SwapChainFn& createSwapChain);
    HRESULT CreateProxyTexture();
    void ReleaseProxyTexture();
    void WaitForFenceValue(UINT64 value);
    void WaitForGpu();
    HRESULT CopyAndPresent(UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);

    LONG _refcount = 1;

    ID3D11Device* _d11 = nullptr;
    ID3D11Device5* _d11_5 = nullptr;
    ID3D11DeviceContext4* _ctx = nullptr;

    ID3D12Device* _d12 = nullptr;
    ID3D12CommandQueue* _queue = nullptr;
    ID3D12GraphicsCommandList* _cmdList = nullptr;
    ID3D12CommandAllocator* _allocators[DXGI_MAX_SWAP_CHAIN_BUFFERS] {};
    UINT64 _allocatorFenceValues[DXGI_MAX_SWAP_CHAIN_BUFFERS] {};

    // Same fence opened on both APIs, D3D11 signals render done, D3D12 signals copy done
    ID3D11Fence* _d11Fence = nullptr;
    ID3D12Fence* _fence = nullptr;
    HANDLE _fenceEvent = nullptr;
    UINT64 _fenceValue = 0;

    IDXGISwapChain4* _sc = nullptr;
    DXGI_USAGE _bufferUsage = 0;
    DXGI_FORMAT _requestedFormat = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D* _proxy11 = nullptr;
    ID3D12Resource* _proxy12 = nullptr;
};
