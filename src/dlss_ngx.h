#pragma once

#include "upscaler.h"
#include <d3d12.h>
#include <cstdint>
#include <windows.h>

// NVSDK_NGX declarations (subset needed for DLSS), matching the vendored headers
// in vendor/nvngx/ (nvsdk_ngx.h / nvsdk_ngx_defs.h / nvsdk_ngx_params.h).

typedef int NVSDK_NGX_Result;
// Driver core 596.49 returns 0x1 on success (verified in the
// research/dls_sdk_370 harness: Init -> 0x1 SUCCESS, CreateFeature -> 0x1).
// Some SDK builds use 0, so accept both.
#define NVSDK_NGX_OK 0x1
#define NVSDK_NGX_SUCCEEDED(r) ((r) == 0x1 || (r) == 0)

typedef void NVSDK_NGX_Parameter;
typedef void NVSDK_NGX_Handle;

typedef enum NVSDK_NGX_Feature {
    NVSDK_NGX_Feature_SuperSampling = 1,
} NVSDK_NGX_Feature;

typedef enum NVSDK_NGX_PerfQuality_Value {
    NVSDK_NGX_PerfQuality_Value_MaxPerf,
    NVSDK_NGX_PerfQuality_Value_Balanced,
    NVSDK_NGX_PerfQuality_Value_MaxQuality,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance,
    NVSDK_NGX_PerfQuality_Value_UltraQuality,
    NVSDK_NGX_PerfQuality_Value_DLAA,
} NVSDK_NGX_PerfQuality_Value;

typedef struct NVSDK_NGX_PathListInfo {
    const wchar_t* const* Path;
    unsigned int Length;
} NVSDK_NGX_PathListInfo;

typedef struct NVSDK_NGX_FeatureCommonInfo {
    NVSDK_NGX_PathListInfo PathListInfo;
    void* InternalData;
} NVSDK_NGX_FeatureCommonInfo;

#define NVSDK_NGX_VERSION_API_MACRO 0x0000015
typedef enum NVSDK_NGX_Version { NVSDK_NGX_Version_API = NVSDK_NGX_VERSION_API_MACRO } NVSDK_NGX_Version;

// DLSS feature create flags
#define NVSDK_NGX_DLSS_Feature_Flags_IsHDR         (1 << 0)
#define NVSDK_NGX_DLSS_Feature_Flags_MVLowRes      (1 << 1)
#define NVSDK_NGX_DLSS_Feature_Flags_MVJittered    (1 << 2)
#define NVSDK_NGX_DLSS_Feature_Flags_DepthInverted (1 << 3)
#define NVSDK_NGX_DLSS_Feature_Flags_AutoExposure  (1 << 6)

// Parameter names
#define NVSDK_NGX_Parameter_Width                    "Width"
#define NVSDK_NGX_Parameter_Height                   "Height"
#define NVSDK_NGX_Parameter_OutWidth                 "OutWidth"
#define NVSDK_NGX_Parameter_OutHeight                "OutHeight"
#define NVSDK_NGX_Parameter_PerfQualityValue         "PerfQualityValue"
#define NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags "DLSS.Feature.Create.Flags"
#define NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects "DLSS.Enable.Output.Subrects"
#define NVSDK_NGX_Parameter_Color                    "Color"
#define NVSDK_NGX_Parameter_Output                   "Output"
#define NVSDK_NGX_Parameter_Depth                    "Depth"
#define NVSDK_NGX_Parameter_MotionVectors            "MotionVectors"
#define NVSDK_NGX_Parameter_Jitter_Offset_X          "Jitter.Offset.X"
#define NVSDK_NGX_Parameter_Jitter_Offset_Y          "Jitter.Offset.Y"
#define NVSDK_NGX_Parameter_Sharpness                "Sharpness"
#define NVSDK_NGX_Parameter_Reset                    "Reset"
#define NVSDK_NGX_Parameter_MV_Scale_X               "MV.Scale.X"
#define NVSDK_NGX_Parameter_MV_Scale_Y               "MV.Scale.Y"
#define NVSDK_NGX_Parameter_InvViewProjectionMatrix  "InvViewProjectionMatrix"
#define NVSDK_NGX_Parameter_ClipToPrevClipMatrix     "ClipToPrevClipMatrix"
#define NVSDK_NGX_Parameter_ExposureTexture          "ExposureTexture"

typedef void (__cdecl* PFN_NVSDK_NGX_Parameter_SetUI)(NVSDK_NGX_Parameter*, const char*, unsigned int);
typedef void (__cdecl* PFN_NVSDK_NGX_Parameter_SetI)(NVSDK_NGX_Parameter*, const char*, int);
typedef void (__cdecl* PFN_NVSDK_NGX_Parameter_SetF)(NVSDK_NGX_Parameter*, const char*, float);
typedef void (__cdecl* PFN_NVSDK_NGX_Parameter_SetULL)(NVSDK_NGX_Parameter*, const char*, unsigned long long);
typedef void (__cdecl* PFN_NVSDK_NGX_Parameter_SetD3d12Resource)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
typedef void (__cdecl* PFN_NVSDK_NGX_Parameter_SetVoidPointer)(NVSDK_NGX_Parameter*, const char*, void*);
typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_Parameter_GetUI)(NVSDK_NGX_Parameter*, const char*, unsigned int*);
typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_Parameter_GetF)(NVSDK_NGX_Parameter*, const char*, float*);
typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_Parameter_GetVoidPointer)(NVSDK_NGX_Parameter*, const char*, void**);

typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_D3D12_Init)(unsigned long long InApplicationId, const wchar_t* InApplicationDataPath, ID3D12Device* InDevice, NVSDK_NGX_Version InSDKVersion);
typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_D3D12_AllocateParameters)(NVSDK_NGX_Parameter** OutParameters);
typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_D3D12_CreateFeature)(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID, const NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle);
typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_D3D12_EvaluateFeature)(ID3D12GraphicsCommandList* InCmdList, const NVSDK_NGX_Handle* InFeatureHandle, const NVSDK_NGX_Parameter* InParameters, void* InCallback);
typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_D3D12_Shutdown)(void);
typedef NVSDK_NGX_Result (__cdecl* PFN_NVSDK_NGX_D3D12_GetParameters)(NVSDK_NGX_Parameter** OutParameters);

// NVSDK_NGX_Parameter is a classic 17-slot vtable object (returned by
// AllocateParameters on the driver core). Slot indices verified against the
// driver 596.49 core: 0=SetULL 1=SetF 2=SetD 3=SetUI 4=SetI 5=SetD3D11Res
// 6=SetD3D12Res 7=SetVoid 8..16=Get variants.
#define NGX_PARAM_SLOT_SET_ULL 0
#define NGX_PARAM_SLOT_SET_F   1
#define NGX_PARAM_SLOT_SET_UI  3
#define NGX_PARAM_SLOT_SET_I   4
#define NGX_PARAM_SLOT_SET_RES 6
#define NGX_PARAM_SLOT_SET_PTR 7
#define NGX_PARAM_SLOT_GET_UI  11
#define NGX_PARAM_SLOT_GET_F   9
#define NGX_PARAM_SLOT_GET_PTR 15

class NgxParamStore;

class NvDlssUpscaler : public IUpscaler {
public:
    NvDlssUpscaler();
    ~NvDlssUpscaler() override;

    bool Init(const UpscalerInitParams& params) override;
    bool Evaluate(const UpscalerEvaluateParams& params) override;
    void Shutdown() override;

    const char* GetName() const override { return "DLSS"; }
    bool IsEnabled() const override { return m_enabled; }
    void SetEnabled(bool enabled) override { m_enabled = enabled; }
    bool IsReady() const override { return m_initialized && m_enabled; }
    void UpdateSizes(unsigned int rw, unsigned int rh,
                     unsigned int dw, unsigned int dh) override;

private:
    bool LoadNGX(const wchar_t* dllPath);
    void UnloadNGX();
    bool CreateFeature(ID3D12GraphicsCommandList* cmdList);
    void DestroyFeature();
    void ResolveParamSlots();

    HMODULE m_ngxDll = nullptr;
    PFN_NVSDK_NGX_D3D12_Init pInit = nullptr;
    PFN_NVSDK_NGX_D3D12_AllocateParameters pAllocateParameters = nullptr;
    PFN_NVSDK_NGX_D3D12_CreateFeature pCreateFeature = nullptr;
    PFN_NVSDK_NGX_D3D12_EvaluateFeature pEvaluateFeature = nullptr;
    PFN_NVSDK_NGX_D3D12_Shutdown pShutdown = nullptr;
    PFN_NVSDK_NGX_D3D12_GetParameters pGetParameters = nullptr;
    PFN_NVSDK_NGX_Parameter_SetUI pSetUI = nullptr;
    PFN_NVSDK_NGX_Parameter_SetI pSetI = nullptr;
    PFN_NVSDK_NGX_Parameter_SetF pSetF = nullptr;
    PFN_NVSDK_NGX_Parameter_SetULL pSetULL = nullptr;
    PFN_NVSDK_NGX_Parameter_SetD3d12Resource pSetResource = nullptr;
    PFN_NVSDK_NGX_Parameter_SetVoidPointer pSetPtr = nullptr;
    PFN_NVSDK_NGX_Parameter_GetUI pGetUI = nullptr;
    bool m_paramSlotsResolved = false;

    ID3D12Device* m_device = nullptr;
    NVSDK_NGX_Parameter* m_parameters = nullptr;
    NgxParamStore* m_paramStore = nullptr;
    NVSDK_NGX_Handle* m_feature = nullptr;
    ID3D12DescriptorHeap* m_evalHeap = nullptr;

    uint32_t m_renderWidth = 0;
    uint32_t m_renderHeight = 0;
    uint32_t m_displayWidth = 0;
    uint32_t m_displayHeight = 0;
    uint32_t m_appId = 0;
    int m_perfQuality = 1;
    bool m_mvJittered = true;
    bool m_autoExposure = true;
    bool m_enabled = true;
    bool m_initialized = false;
    bool m_featureCreated = false;
    bool m_firstEvaluate = true;

    wchar_t m_ngxDataPath[MAX_PATH] = {};
    wchar_t m_dlssPath[MAX_PATH] = {};
    const wchar_t* m_pathList[2] = { nullptr, nullptr };
};