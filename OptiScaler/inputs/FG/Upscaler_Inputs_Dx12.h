#pragma once
#include "SysUtils.h"
#include <NVNGX_Parameter.h>
#include <upscalers/IFeature_Dx12.h>

class UpscalerInputsDx12
{
  private:
    inline static ID3D12Device* _device = nullptr;

    static void UpscaleStartImpl(ID3D12Device* device, ID3D12GraphicsCommandList* InCmdList,
                                 NVSDK_NGX_Parameter* InParameters, IFeature* feature, ID3D12Resource* paramVelocity,
                                 ID3D12Resource* paramDepth, bool fromDx11);

  public:
    static void Init(ID3D12Device* device);
    static void Reset();
    static void UpscaleStart(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                             IFeature_Dx12* feature);

    // For Dx11 features running on Dx12 with the Dx11 swapchain bridge,
    // resources are the Dx12 copies of Dx11 inputs and in common state
    static void UpscaleStartDx11wDx12(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                                      NVSDK_NGX_Parameter* InParameters, IFeature* feature, ID3D12Resource* velocity,
                                      ID3D12Resource* depth);
    static void UpscaleEnd(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                           IFeature_Dx12* feature);
};
