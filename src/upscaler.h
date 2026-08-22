#pragma once

#include <d3d12.h>
#include <cstdint>

struct UpscalerInitParams {
    ID3D12Device* device = nullptr;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    const wchar_t* dlssDllPath = nullptr;
    uint32_t appId = 0;
    int perfQuality = 1;   // NVSDK_NGX_PerfQuality_Value: 0=MaxPerf 1=Balanced 2=MaxQuality 3=UltraPerf 4=UltraQuality
    bool mvJittered = true;
    bool autoExposure = true;
};

struct UpscalerEvaluateParams {
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Resource* color = nullptr;          // SRV-able input color (full-res resource, render rect = renderWidth/Height)
    ID3D12Resource* depth = nullptr;          // SRV-able depth (D24/D32, linear, same render rect)
    ID3D12Resource* motionVectors = nullptr;  // SRV-able velocity (R16G16_FLOAT, UV-space [0,1] deltas)
    ID3D12Resource* output = nullptr;         // UAV-able output (display res, R16G16B16A16_UNORM)
    float jitterX = 0.0f;                     // render pixels
    float jitterY = 0.0f;
    float mvScaleX = 0.0f;                    // velocity UV->pixel scale (motion vector buffer width)
    float mvScaleY = 0.0f;                    // velocity UV->pixel scale (motion vector buffer height)
    float sharpness = 0.0f;
};

class IUpscaler {
public:
    virtual ~IUpscaler() = default;

    virtual bool Init(const UpscalerInitParams& params) = 0;
    virtual bool Evaluate(const UpscalerEvaluateParams& params) = 0;
    virtual void Shutdown() = 0;

    virtual const char* GetName() const = 0;
    virtual bool IsEnabled() const = 0;
    virtual void SetEnabled(bool enabled) = 0;
    virtual bool IsReady() const = 0;

    // Re-configure for a new render/display size (e.g. the engine changed
    // its render target resolution). Default: no-op.
    virtual void UpdateSizes(unsigned int rw, unsigned int rh,
                             unsigned int dw, unsigned int dh) {}
};

enum UpscalerType {
    UPSCALER_NONE = 0,
    UPSCALER_DLSS = 1,
    UPSCALER_FSR2 = 2,
};

IUpscaler* CreateUpscaler(UpscalerType type);
void DestroyUpscaler(IUpscaler* upscaler);