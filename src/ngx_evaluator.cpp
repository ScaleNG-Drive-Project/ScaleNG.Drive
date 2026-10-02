#include "ngx_evaluator.h"
#include "dxgi_hooks.h"
#include <windows.h>
#include <d3d12.h>
#include <vector>
#include <cmath>
#include <algorithm>

namespace ScaleNG {

// ============================================================================
// DLL Loading
// ============================================================================

bool NgxEvaluator::LoadNgxDll() {
    if (ngx_dll_) return true;

    wchar_t dll_path[MAX_PATH] = {};
    if (init_params_.dll_path[0]) {
        wcscpy_s(dll_path, init_params_.dll_path);
    } else {
        wchar_t module_path[MAX_PATH];
        GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        wchar_t* slash = wcsrchr(module_path, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat_s(module_path, L"nvngx_dlss.dll");
            wcscpy_s(dll_path, module_path);
        }
    }

    if (dll_path[0] == L'\0') {
        SNG_LOG("[NGX] No DLL path specified and couldn't auto-detect");
        return false;
    }

    ngx_dll_ = LoadLibraryExW(dll_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ngx_dll_) {
        DWORD err = GetLastError();
        SNG_LOG("[NGX] Failed to load nvngx_dlss.dll from %ls: error %lu", dll_path, err);
        return false;
    }

    nvngx_create_dlss_ext_ = (PFN_NVSDK_NGX_D3D12_CREATE_DLSS_EXT)GetProcAddress(ngx_dll_, "NVSDK_NGX_D3D12_CreateDLSS_Ext");
    nvngx_evaluate_feature_ = (PFN_NVSDK_NGX_D3D12_EVALUATE_FEATURE)GetProcAddress(ngx_dll_, "NVSDK_NGX_D3D12_EvaluateFeature");
    nvngx_get_capabilities_ = (PFN_NVSDK_NGX_D3D12_GET_CAPABILITIES)GetProcAddress(ngx_dll_, "NVSDK_NGX_D3D12_GetCapabilities");
    nvngx_get_optimal_settings_ = (PFN_NVSDK_NGX_D3D12_GET_OPTIMAL_SETTINGS)GetProcAddress(ngx_dll_, "NVSDK_NGX_D3D12_GetOptimalSettings");
    nvngx_shutdown_ = (PFN_NVSDK_NGX_D3D12_SHUTDOWN)GetProcAddress(ngx_dll_, "NVSDK_NGX_D3D12_Shutdown");
    nvngx_allocate_parameters_ = (PFN_NVSDK_NGX_ALLOCATE_PARAMETERS)GetProcAddress(ngx_dll_, "NVSDK_NGX_AllocateParameters");
    nvngx_destroy_parameters_ = (PFN_NVSDK_NGX_DESTROY_PARAMETERS)GetProcAddress(ngx_dll_, "NVSDK_NGX_DestroyParameters");
    nvngx_create_feature_ = (PFN_NVSDK_NGX_CREATE_FEATURE)GetProcAddress(ngx_dll_, "NVSDK_NGX_CreateFeature");
    nvngx_release_feature_ = (PFN_NVSDK_NGX_RELEASE_FEATURE)GetProcAddress(ngx_dll_, "NVSDK_NGX_ReleaseFeature");

    if (!nvngx_create_dlss_ext_ || !nvngx_evaluate_feature_ || !nvngx_allocate_parameters_ || 
        !nvngx_destroy_parameters_ || !nvngx_create_feature_ || !nvngx_release_feature_) {
        SNG_LOG("[NGX] Failed to get required function pointers");
        UnloadNgxDll();
        return false;
    }

    SNG_LOG("[NGX] Loaded nvngx_dlss.dll from %ls", dll_path);
    return true;
}

void NgxEvaluator::UnloadNgxDll() {
    if (ngx_dll_) {
        FreeLibrary(ngx_dll_);
        ngx_dll_ = nullptr;
    }
    nvngx_create_dlss_ext_ = nullptr;
    nvngx_evaluate_feature_ = nullptr;
    nvngx_get_capabilities_ = nullptr;
    nvngx_get_optimal_settings_ = nullptr;
    nvngx_shutdown_ = nullptr;
    nvngx_allocate_parameters_ = nullptr;
    nvngx_destroy_parameters_ = nullptr;
    nvngx_create_feature_ = nullptr;
    nvngx_release_feature_ = nullptr;
}

// ============================================================================
// Parameter Management
// ============================================================================

bool NgxEvaluator::CreateParameters() {
    if (!nvngx_allocate_parameters_) return false;
    
    int result = nvngx_allocate_parameters_((NVSDK_NGX_Parameter**)&parameters_);
    if (result != NVSDK_NGX_Result_Success) {
        SNG_LOG("[NGX] Failed to allocate parameters: %s", GetErrorString(result));
        return false;
    }
    return true;
}

void NgxEvaluator::DestroyParameters() {
    if (parameters_ && nvngx_destroy_parameters_) {
        nvngx_destroy_parameters_((NVSDK_NGX_Parameter*)parameters_);
        parameters_ = nullptr;
    }
}

// ============================================================================
// Jitter Sequence Generation
// ============================================================================

void NgxEvaluator::GenerateJitterSequence() {
    static const std::pair<float, float> halton_2_3[] = {
        {0.5f, 0.333333333f},
        {0.25f, 0.666666667f},
        {0.75f, 0.111111111f},
        {0.125f, 0.444444444f},
        {0.625f, 0.777777778f},
        {0.375f, 0.222222222f},
        {0.875f, 0.555555556f},
        {0.0625f, 0.888888889f}
    };
    
    jitter_sequence_.clear();
    for (const auto& j : halton_2_3) {
        jitter_sequence_.push_back(j);
    }
    jitter_index_ = 0;
}

std::pair<float, float> NgxEvaluator::GetNextJitter() {
    if (jitter_sequence_.empty()) {
        GenerateJitterSequence();
    }
    auto jitter = jitter_sequence_[jitter_index_];
    jitter_index_ = (jitter_index_ + 1) % jitter_sequence_.size();
    return jitter;
}

// ============================================================================
// Initialization
// ============================================================================

bool NgxEvaluator::Initialize(const NgxInitParams& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_) {
        SNG_LOG("[NGX] Already initialized");
        return true;
    }

    init_params_ = params;

    if (!LoadNgxDll()) {
        return false;
    }

    // NVSDK_NGX_FeatureCommonInfo structure
    struct FeatureCommonInfo {
        unsigned int AppId;
        wchar_t AppDataPath[260];
    } common_info = {};
    common_info.AppId = params.app_id;
    common_info.AppDataPath[0] = L'\0';
    
    int result = nvngx_create_dlss_ext_(
        params.device,
        params.queue,
        &common_info,
        NVSDK_NGX_Feature_DLSS,
        0,  // NVSDK_NGX_DLSS_Hint_Default
        nullptr
    );

    if (result != NVSDK_NGX_Result_Success) {
        SNG_LOG("[NGX] NVSDK_NGX_D3D12_CreateDLSS_Ext failed: %s", GetErrorString(result));
        UnloadNgxDll();
        return false;
    }

    if (!CreateParameters()) {
        if (nvngx_shutdown_) nvngx_shutdown_();
        UnloadNgxDll();
        return false;
    }

    initialized_ = true;
    frame_count_ = 0;
    GenerateJitterSequence();
    
    SNG_LOG("[NGX] Initialized successfully (AppId=%u, DLAA=%d, Quality=%d)", 
        params.app_id, params.dlaa ? 1 : 0, params.perf_quality);
    return true;
}

// ============================================================================
// Feature Creation
// ============================================================================

bool NgxEvaluator::CreateFeature(UINT render_width, UINT render_height, 
                                  UINT display_width, UINT display_height) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_) {
        SNG_LOG("[NGX] Not initialized");
        return false;
    }
    
    if (feature_created_) {
        SNG_LOG("[NGX] Feature already created");
        return true;
    }

    if (!parameters_) {
        SNG_LOG("[NGX] Parameters not allocated");
        return false;
    }

    int result = nvngx_create_feature_(
        NVSDK_NGX_Feature_DLSS,
        (NVSDK_NGX_Parameter*)parameters_,
        (NVSDK_NGX_Handle**)&feature_handle_
    );

    if (result != NVSDK_NGX_Result_Success) {
        SNG_LOG("[NGX] NVSDK_NGX_CreateFeature failed: %s", GetErrorString(result));
        return false;
    }

    feature_created_ = true;
    SNG_LOG("[NGX] Feature created: %ux%u -> %ux%u (DLAA=%d, Quality=%d)",
        render_width, render_height, display_width, display_height,
        init_params_.dlaa ? 1 : 0, init_params_.perf_quality);
    return true;
}

// ============================================================================
// Evaluation
// ============================================================================

bool NgxEvaluator::Evaluate(const NgxParameters& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_ || !feature_created_) {
        SNG_LOG("[NGX] Not ready for evaluation (init=%d, feature=%d)", 
            initialized_, feature_created_);
        return false;
    }

    if (!params.color || !params.depth || !params.motion_vectors || !params.output) {
        SNG_LOG("[NGX] Missing required resources: color=%p, depth=%p, mv=%p, output=%p",
            params.color, params.depth, params.motion_vectors, params.output);
        return false;
    }

    if (!parameters_) {
        SNG_LOG("[NGX] Parameters not available");
        return false;
    }

    auto jitter = GetNextJitter();
    float jitter_x = params.jitter_x != 0.0f ? params.jitter_x : jitter.first;
    float jitter_y = params.jitter_y != 0.0f ? params.jitter_y : jitter.second;

    int result = nvngx_evaluate_feature_(
        init_params_.device,
        init_params_.queue,
        (NVSDK_NGX_Handle*)feature_handle_,
        (NVSDK_NGX_Parameter*)parameters_,
        nullptr
    );

    if (result != NVSDK_NGX_Result_Success) {
        SNG_LOG("[NGX] Evaluate failed (frame %llu): %s", frame_count_, GetErrorString(result));
        return false;
    }

    frame_count_++;
    
    if (frame_count_ % 60 == 1) {
        SNG_LOG("[NGX] Evaluated frame %llu: jitter=(%.4f,%.4f), sharpness=%.2f", 
            frame_count_, jitter_x, jitter_y, params.sharpness);
    }

    return true;
}

// ============================================================================
// Shutdown
// ============================================================================

void NgxEvaluator::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (feature_created_ && feature_handle_ && nvngx_release_feature_) {
        nvngx_release_feature_((NVSDK_NGX_Handle*)feature_handle_);
        feature_handle_ = nullptr;
        feature_created_ = false;
    }
    
    DestroyParameters();
    
    if (initialized_ && nvngx_shutdown_) {
        nvngx_shutdown_();
        initialized_ = false;
    }
    
    UnloadNgxDll();
    
    SNG_LOG("[NGX] Shutdown complete");
}

// ============================================================================
// Utility Functions
// ============================================================================

void NgxEvaluator::GetOptimalRenderDimensions(UINT display_width, UINT display_height, int quality,
                                               UINT& out_render_width, UINT& out_render_height) {
    static const float quality_scales[] = {
        0.5f,   // 0 = Ultra Performance
        0.59f,  // 1 = Performance / Balanced
        0.67f,  // 2 = Quality
        0.77f,  // 3 = Ultra Quality
        1.0f    // 4 = DLAA
    };
    
    int idx = std::clamp(quality, 0, 4);
    float scale = quality_scales[idx];
    
    out_render_width = static_cast<UINT>(display_width * scale);
    out_render_height = static_cast<UINT>(display_height * scale);
    
    out_render_width = (out_render_width + 7) & ~7u;
    out_render_height = (out_render_height + 7) & ~7u;
}

const char* NgxEvaluator::GetErrorString(int result) {
    switch (result) {
        case NVSDK_NGX_Result_Success: return "Success";
        case NVSDK_NGX_Result_Fail: return "Fail";
        case NVSDK_NGX_Result_InvalidParameter: return "Invalid Parameter";
        case NVSDK_NGX_Result_OutOfMemory: return "Out of Memory";
        case NVSDK_NGX_Result_NotSupported: return "Not Supported";
        case NVSDK_NGX_Result_NotInitialized: return "Not Initialized";
        case NVSDK_NGX_Result_NotFound: return "Not Found";
        case NVSDK_NGX_Result_AlreadyInitialized: return "Already Initialized";
        case NVSDK_NGX_Result_DeviceNotSupported: return "Device Not Supported";
        case NVSDK_NGX_Result_DriverNotSupported: return "Driver Not Supported";
        case NVSDK_NGX_Result_ModelNotFound: return "Model Not Found";
        case NVSDK_NGX_Result_ModelWrongVersion: return "Model Wrong Version";
        case NVSDK_NGX_Result_ModelCorrupted: return "Model Corrupted";
        case NVSDK_NGX_Result_FeatureNotSupported: return "Feature Not Supported";
        case NVSDK_NGX_Result_FeatureNotEnabled: return "Feature Not Enabled";
        case NVSDK_NGX_Result_FeatureAlreadyEnabled: return "Feature Already Enabled";
        case NVSDK_NGX_Result_FeatureCreateFailed: return "Feature Create Failed";
        case NVSDK_NGX_Result_DeviceLost: return "Device Lost";
        case NVSDK_NGX_Result_QueueNotSupported: return "Queue Not Supported";
        case NVSDK_NGX_Result_UnsupportedFormat: return "Unsupported Format";
        default: return "Unknown Error";
    }
}

} // namespace ScaleNG