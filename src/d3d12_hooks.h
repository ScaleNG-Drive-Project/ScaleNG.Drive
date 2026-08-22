#pragma once
#include <windows.h>
#include <d3d12.h>

struct ScaleNgConfig {
    bool enabled = true;
    bool dlaa = false;
    bool passive = false;
    float renderScale = 0.67f;
    float sharpness = 0.0f;
    int perfQuality = 1;
    bool mvJittered = true;
    bool autoExposure = true;
    unsigned int appId = 1;
    wchar_t dlssDllPath[MAX_PATH] = {};
};

void HooksSetConfig(const ScaleNgConfig& config);

void HooksInstallCreateDeviceDetour();

void HooksGetDescriptorHeaps(UINT* count, ID3D12DescriptorHeap** heaps);

void HooksRestoreDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT count,
                                 ID3D12DescriptorHeap* const* heaps);