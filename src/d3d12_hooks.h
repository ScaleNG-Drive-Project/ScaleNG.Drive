#pragma once
#include <windows.h>
#include <d3d12.h>

struct ScaleNgConfig {
    bool enabled = true;
    bool dlaa = false;
    bool hud = true;
    bool legacyScale = false;
    bool passive = false;
    float renderScale = 0.67f;
    float sharpness = 0.0f;
    int perfQuality = 1;
    bool mvJittered = true;
    bool autoExposure = true;
    unsigned int appId = 0xE658700;
    wchar_t dlssDllPath[MAX_PATH] = {};
};

unsigned HooksGetQuietFrames();
void HooksDumpDRED(const char* why);
void HooksInstallVEH();
void HooksSetConfig(const ScaleNgConfig& config);

void HooksInstallCreateDeviceDetour();

void HooksGetDescriptorHeaps(UINT* count, ID3D12DescriptorHeap** heaps);

void HooksRestoreDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT count,
                                 ID3D12DescriptorHeap* const* heaps);

// Shared trampoline for genuine D3D12CreateDevice (bypasses our detour)
typedef HRESULT(WINAPI* PFN_ScaleNG_CreateDevice)(void*, unsigned, const IID&, void**);
extern PFN_ScaleNG_CreateDevice Real_D3D12CreateDevice_Tramp;
