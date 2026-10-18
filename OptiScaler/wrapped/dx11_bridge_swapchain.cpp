#include "pch.h"
#include "dx11_bridge_swapchain.h"

#include <Config.h>

#include <proxies/D3D12_Proxy.h>

#define SAFE_RELEASE(p)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (p && p != nullptr)                                                                                         \
        {                                                                                                              \
            (p)->Release();                                                                                            \
            (p) = nullptr;                                                                                             \
        }                                                                                                              \
    } while ((void) 0, 0)

// Flip model swapchains don't accept sRGB formats
static DXGI_FORMAT ToFlipFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

static void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

HRESULT Dx11BridgeSwapChain::Create(ID3D11Device* device, const DXGI_SWAP_CHAIN_DESC* desc,
                                    const CreateDx12SwapChainFn& createSwapChain, IDXGISwapChain** ppSwapChain)
{
    *ppSwapChain = nullptr;

    auto bridge = new Dx11BridgeSwapChain(device);
    auto result = bridge->Init(desc, createSwapChain);

    if (result != S_OK)
    {
        LOG_ERROR("Bridge init failed: {:X}", (UINT) result);
        bridge->Release();
        return result;
    }

    LOG_INFO("Dx11 bridge swapchain created: {:X}, Dx12 swapchain: {:X}", (size_t) bridge, (size_t) bridge->_sc);
    *ppSwapChain = bridge;
    return S_OK;
}

Dx11BridgeSwapChain::Dx11BridgeSwapChain(ID3D11Device* device) : _d11(device) { _d11->AddRef(); }

Dx11BridgeSwapChain::~Dx11BridgeSwapChain()
{
    if (_queue != nullptr && _fence != nullptr)
        WaitForGpu();

    ReleaseProxyTexture();

    SAFE_RELEASE(_sc);
    SAFE_RELEASE(_cmdList);

    for (auto& allocator : _allocators)
        SAFE_RELEASE(allocator);

    SAFE_RELEASE(_fence);
    SAFE_RELEASE(_d11Fence);

    if (_fenceEvent != nullptr)
    {
        CloseHandle(_fenceEvent);
        _fenceEvent = nullptr;
    }

    SAFE_RELEASE(_queue);
    SAFE_RELEASE(_d12);
    SAFE_RELEASE(_ctx);
    SAFE_RELEASE(_d11_5);
    SAFE_RELEASE(_d11);
}

HRESULT Dx11BridgeSwapChain::Init(const DXGI_SWAP_CHAIN_DESC* desc, const CreateDx12SwapChainFn& createSwapChain)
{
    // Shared fences need D3D11.4
    auto result = _d11->QueryInterface(IID_PPV_ARGS(&_d11_5));
    if (result != S_OK)
    {
        LOG_ERROR("ID3D11Device5 not supported: {:X}", (UINT) result);
        return result;
    }

    ID3D11DeviceContext* context = nullptr;
    _d11->GetImmediateContext(&context);
    result = context->QueryInterface(IID_PPV_ARGS(&_ctx));
    context->Release();

    if (result != S_OK)
    {
        LOG_ERROR("ID3D11DeviceContext4 not supported: {:X}", (UINT) result);
        return result;
    }

    // D3D12 device on the same adapter as the game
    IDXGIAdapter* adapter = nullptr;
    IDXGIDevice* dxgiDevice = nullptr;

    if (_d11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
    {
        dxgiDevice->GetAdapter(&adapter);
        dxgiDevice->Release();
    }

    {
        ScopedSkipSpoofing skipSpoofing {};

        if (D3d12Proxy::Module() == nullptr)
            result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_d12));
        else
            result = D3d12Proxy::D3D12CreateDevice_()(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_d12));
    }

    if (adapter != nullptr)
        adapter->Release();

    if (result != S_OK)
    {
        LOG_ERROR("D3D12CreateDevice error: {:X}", (UINT) result);
        return result;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = _d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_queue));

    if (result != S_OK)
    {
        LOG_ERROR("CreateCommandQueue error: {:X}", (UINT) result);
        return result;
    }

    result = _d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_allocators[0]));

    if (result != S_OK)
    {
        LOG_ERROR("CreateCommandAllocator error: {:X}", (UINT) result);
        return result;
    }

    result = _d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0], nullptr,
                                     IID_PPV_ARGS(&_cmdList));

    if (result != S_OK)
    {
        LOG_ERROR("CreateCommandList error: {:X}", (UINT) result);
        return result;
    }

    _cmdList->Close();

    // Shared fence, created on D3D11 side like IFeature_Dx11wDx12 does
    result = _d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_d11Fence));

    if (result != S_OK)
    {
        LOG_ERROR("D3D11 CreateFence error: {:X}", (UINT) result);
        return result;
    }

    HANDLE fenceHandle = nullptr;
    result = _d11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);

    if (result != S_OK)
    {
        LOG_ERROR("Fence CreateSharedHandle error: {:X}", (UINT) result);
        return result;
    }

    result = _d12->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&_fence));
    CloseHandle(fenceHandle);

    if (result != S_OK)
    {
        LOG_ERROR("Fence OpenSharedHandle error: {:X}", (UINT) result);
        return result;
    }

    _fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // D3D12 swapchain for the game window
    DXGI_SWAP_CHAIN_DESC scDesc = *desc;
    _bufferUsage = desc->BufferUsage;
    _requestedFormat = desc->BufferDesc.Format;

    scDesc.BufferDesc.Format = ToFlipFormat(desc->BufferDesc.Format);
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;

    if (scDesc.BufferCount < 2)
        scDesc.BufferCount = 2;

    if (scDesc.BufferDesc.Format != _requestedFormat)
        LOG_WARN("Requested format {} is not flip compatible, using {}", (UINT) _requestedFormat,
                 (UINT) scDesc.BufferDesc.Format);

    LOG_INFO("Creating Dx12 swapchain {}x{}, Format: {}, Count: {}, Flags: {:X}, Windowed: {}",
             scDesc.BufferDesc.Width, scDesc.BufferDesc.Height, (UINT) scDesc.BufferDesc.Format, scDesc.BufferCount,
             scDesc.Flags, scDesc.Windowed);

    IDXGISwapChain* sc = nullptr;
    result = createSwapChain(_queue, &scDesc, &sc);

    if (result != S_OK || sc == nullptr)
    {
        LOG_ERROR("Dx12 CreateSwapChain error: {:X}", (UINT) result);
        return result == S_OK ? E_FAIL : result;
    }

    result = sc->QueryInterface(IID_PPV_ARGS(&_sc));
    sc->Release();

    if (result != S_OK)
    {
        LOG_ERROR("IDXGISwapChain4 not supported: {:X}", (UINT) result);
        return result;
    }

    return CreateProxyTexture();
}

HRESULT Dx11BridgeSwapChain::CreateProxyTexture()
{
    DXGI_SWAP_CHAIN_DESC1 scDesc {};
    auto result = _sc->GetDesc1(&scDesc);

    if (result != S_OK)
        return result;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = scDesc.Width;
    desc.Height = scDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = scDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    if (_bufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
        desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    result = _d11->CreateTexture2D(&desc, nullptr, &_proxy11);

    if (result != S_OK)
    {
        LOG_ERROR("Proxy CreateTexture2D error: {:X}", (UINT) result);
        return result;
    }

    IDXGIResource1* resource = nullptr;
    result = _proxy11->QueryInterface(IID_PPV_ARGS(&resource));

    if (result != S_OK)
    {
        LOG_ERROR("Proxy QueryInterface(IDXGIResource1) error: {:X}", (UINT) result);
        return result;
    }

    HANDLE handle = nullptr;
    result = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                          &handle);
    resource->Release();

    if (result != S_OK)
    {
        LOG_ERROR("Proxy CreateSharedHandle error: {:X}", (UINT) result);
        return result;
    }

    result = _d12->OpenSharedHandle(handle, IID_PPV_ARGS(&_proxy12));
    CloseHandle(handle);

    if (result != S_OK)
    {
        LOG_ERROR("Proxy OpenSharedHandle error: {:X}", (UINT) result);
        return result;
    }

    LOG_INFO("Proxy backbuffer created {}x{}, Format: {}", desc.Width, desc.Height, (UINT) desc.Format);
    return S_OK;
}

void Dx11BridgeSwapChain::ReleaseProxyTexture()
{
    SAFE_RELEASE(_proxy12);
    SAFE_RELEASE(_proxy11);
}

void Dx11BridgeSwapChain::WaitForFenceValue(UINT64 value)
{
    if (_fence->GetCompletedValue() >= value)
        return;

    _fence->SetEventOnCompletion(value, _fenceEvent);

    // Max 5 sec, avoid locking the game if something goes wrong
    if (WaitForSingleObject(_fenceEvent, 5000) != WAIT_OBJECT_0)
        LOG_WARN("Timeout while waiting fence value: {}", value);
}

void Dx11BridgeSwapChain::WaitForGpu()
{
    auto value = ++_fenceValue;
    _queue->Signal(_fence, value);
    WaitForFenceValue(value);
}

HRESULT Dx11BridgeSwapChain::CopyAndPresent(UINT SyncInterval, UINT Flags,
                                            const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (Flags & DXGI_PRESENT_TEST)
    {
        if (pPresentParameters == nullptr)
            return _sc->Present(SyncInterval, Flags);

        return _sc->Present1(SyncInterval, Flags, pPresentParameters);
    }

    if (_proxy12 == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    // D3D11 finished rendering into proxy when fence reaches this value
    auto renderValue = ++_fenceValue;
    auto result = _ctx->Signal(_d11Fence, renderValue);

    if (result != S_OK)
    {
        LOG_ERROR("D3D11 Signal error: {:X}", (UINT) result);
        return result;
    }

    _ctx->Flush();

    auto index = _sc->GetCurrentBackBufferIndex();

    if (_allocators[index] == nullptr)
    {
        result = _d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_allocators[index]));

        if (result != S_OK)
        {
            LOG_ERROR("CreateCommandAllocator error: {:X}", (UINT) result);
            return result;
        }
    }

    WaitForFenceValue(_allocatorFenceValues[index]);

    _allocators[index]->Reset();
    _cmdList->Reset(_allocators[index], nullptr);

    ID3D12Resource* backBuffer = nullptr;
    result = _sc->GetBuffer(index, IID_PPV_ARGS(&backBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("Dx12 GetBuffer({}) error: {:X}", index, (UINT) result);
        _cmdList->Close();
        return result;
    }

    Transition(_cmdList, _proxy12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(_cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    _cmdList->CopyResource(backBuffer, _proxy12);
    Transition(_cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Transition(_cmdList, _proxy12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    _cmdList->Close();

    backBuffer->Release();

    _queue->Wait(_fence, renderValue);

    ID3D12CommandList* lists[] = { _cmdList };
    _queue->ExecuteCommandLists(1, lists);

    auto copyValue = ++_fenceValue;
    _queue->Signal(_fence, copyValue);
    _allocatorFenceValues[index] = copyValue;

    // D3D11 must not render into proxy before copy is done
    _ctx->Wait(_d11Fence, copyValue);

    if (pPresentParameters == nullptr)
        result = _sc->Present(SyncInterval, Flags);
    else
        result = _sc->Present1(SyncInterval, Flags, pPresentParameters);

    if (result != S_OK)
        LOG_ERROR("Dx12 Present error: {:X}", (UINT) result);

    return result;
}

//
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr)
        return E_POINTER;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4) ||
        riid == __uuidof(Dx11BridgeSwapChain))
    {
        AddRef();
        *ppvObject = (IDXGISwapChain4*) this;
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dx11BridgeSwapChain::AddRef() { return InterlockedIncrement(&_refcount); }

ULONG STDMETHODCALLTYPE Dx11BridgeSwapChain::Release()
{
    auto ret = InterlockedDecrement(&_refcount);

    if (ret == 0)
    {
        LOG_INFO("Releasing Dx11 bridge swapchain: {:X}", (size_t) this);
        delete this;
    }

    return ret;
}

//
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _sc->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _sc->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _sc->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetParent(REFIID riid, void** ppParent)
{
    return _sc->GetParent(riid, ppParent);
}

// Game must see its own D3D11 device
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetDevice(REFIID riid, void** ppDevice)
{
    return _d11->QueryInterface(riid, ppDevice);
}

//
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::Present(UINT SyncInterval, UINT Flags)
{
    return CopyAndPresent(SyncInterval, Flags, nullptr);
}

// Like D3D11 flip model swapchains only buffer 0 is accessible and it's always the current one
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    if (Buffer != 0 || _proxy11 == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    return _proxy11->QueryInterface(riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Fullscreen: {}", Fullscreen);
    return _sc->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return _sc->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    auto result = _sc->GetDesc(pDesc);

    if (result == S_OK)
    {
        pDesc->BufferUsage = _bufferUsage;
        pDesc->BufferDesc.Format = _requestedFormat;
    }

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                             DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    LOG_INFO("BufferCount: {}, Width: {}, Height: {}, NewFormat: {}, SwapChainFlags: {:X}", BufferCount, Width, Height,
             (UINT) NewFormat, SwapChainFlags);

    // D3D11 side must be done with proxy and D3D12 side with the backbuffers
    _ctx->Flush();
    WaitForGpu();
    ReleaseProxyTexture();

    if (BufferCount == 1)
        BufferCount = 2;

    auto result = _sc->ResizeBuffers(BufferCount, Width, Height, ToFlipFormat(NewFormat), SwapChainFlags);

    if (result != S_OK)
        LOG_ERROR("Dx12 ResizeBuffers error: {:X}", (UINT) result);
    else if (NewFormat != DXGI_FORMAT_UNKNOWN)
        _requestedFormat = NewFormat;

    // Recreate proxy even on failure, swapchain keeps the old buffers then
    auto proxyResult = CreateProxyTexture();

    return result != S_OK ? result : proxyResult;
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _sc->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _sc->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _sc->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _sc->GetLastPresentCount(pLastPresentCount);
}

//
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    auto result = _sc->GetDesc1(pDesc);

    if (result == S_OK)
    {
        pDesc->BufferUsage = _bufferUsage;
        pDesc->Format = _requestedFormat;
    }

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return _sc->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetHwnd(HWND* pHwnd) { return _sc->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _sc->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::Present1(UINT SyncInterval, UINT PresentFlags,
                                                        const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    return CopyAndPresent(SyncInterval, PresentFlags, pPresentParameters);
}

BOOL STDMETHODCALLTYPE Dx11BridgeSwapChain::IsTemporaryMonoSupported(void) { return _sc->IsTemporaryMonoSupported(); }

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _sc->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _sc->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _sc->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _sc->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _sc->GetRotation(pRotation);
}

//
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetSourceSize(UINT Width, UINT Height)
{
    return _sc->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _sc->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetMaximumFrameLatency(UINT MaxLatency)
{
    return _sc->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    return _sc->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE Dx11BridgeSwapChain::GetFrameLatencyWaitableObject(void)
{
    return _sc->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _sc->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _sc->GetMatrixTransform(pMatrix);
}

//
UINT STDMETHODCALLTYPE Dx11BridgeSwapChain::GetCurrentBackBufferIndex(void) { return 0; }

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                      UINT* pColorSpaceSupport)
{
    return _sc->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    LOG_INFO("ColorSpace: {}", (UINT) ColorSpace);
    return _sc->SetColorSpace1(ColorSpace);
}

// D3D11 has no present queues, node masks are ignored
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                              DXGI_FORMAT Format, UINT SwapChainFlags,
                                                              const UINT* pCreationNodeMask,
                                                              IUnknown* const* ppPresentQueue)
{
    return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
}

//
HRESULT STDMETHODCALLTYPE Dx11BridgeSwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size,
                                                              void* pMetaData)
{
    return _sc->SetHDRMetaData(Type, Size, pMetaData);
}
