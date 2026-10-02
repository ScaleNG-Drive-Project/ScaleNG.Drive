#pragma once

#include "dxgi_hooks.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <mutex>
#include <vector>

// ============================================================================
// NGX Evaluator - In-process DLSS/DLAA evaluation
// ============================================================================

namespace ScaleNG {

struct NgxParameters {
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion_vectors = nullptr;
    ID3D12Resource* exposure = nullptr;
    ID3D12Resource* bias_color = nullptr;
    ID3D12Resource* output = nullptr;
    
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float sharpness = 0.0f;
    bool mv_low_res = false;
    bool reset = false;
    bool auto_exposure = true;
    float pre_exposure = 1.0f;
    
    UINT render_width = 0;
    UINT render_height = 0;
    UINT display_width = 0;
    UINT display_height = 0;
};

struct NgxInitParams {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    UINT app_id = 0xE658700;
    wchar_t dll_path[MAX_PATH] = {};
    bool dlaa = true;
    int perf_quality = 1;
};

class NgxEvaluator {
public:
    static NgxEvaluator& Get() {
        static NgxEvaluator instance;
        return instance;
    }

    bool Initialize(const NgxInitParams& params);
    bool CreateFeature(UINT render_width, UINT render_height, UINT display_width, UINT display_height);
    bool Evaluate(const NgxParameters& params);
    void Shutdown();

    bool IsInitialized() const { return initialized_; }
    bool IsFeatureCreated() const { return feature_created_; }

    static void GetOptimalRenderDimensions(UINT display_width, UINT display_height, int quality, 
                                           UINT& out_render_width, UINT& out_render_height);
    static const char* GetErrorString(int result);

private:
    NgxEvaluator() = default;
    ~NgxEvaluator() { Shutdown(); }

    std::mutex mutex_;
    bool initialized_ = false;
    bool feature_created_ = false;
    
    NgxInitParams init_params_;
    
    void* feature_handle_ = nullptr;
    void* parameters_ = nullptr;
    
    std::vector<std::pair<float, float>> jitter_sequence_;
    size_t jitter_index_ = 0;
    uint64_t frame_count_ = 0;
    
    HMODULE ngx_dll_ = nullptr;
    
    // Function pointers
    PFN_NVSDK_NGX_D3D12_CREATE_DLSS_EXT nvngx_create_dlss_ext_ = nullptr;
    PFN_NVSDK_NGX_D3D12_EVALUATE_FEATURE nvngx_evaluate_feature_ = nullptr;
    PFN_NVSDK_NGX_D3D12_GET_CAPABILITIES nvngx_get_capabilities_ = nullptr;
    PFN_NVSDK_NGX_D3D12_GET_OPTIMAL_SETTINGS nvngx_get_optimal_settings_ = nullptr;
    PFN_NVSDK_NGX_D3D12_SHUTDOWN nvngx_shutdown_ = nullptr;
    PFN_NVSDK_NGX_ALLOCATE_PARAMETERS nvngx_allocate_parameters_ = nullptr;
    PFN_NVSDK_NGX_DESTROY_PARAMETERS nvngx_destroy_parameters_ = nullptr;
    PFN_NVSDK_NGX_CREATE_FEATURE nvngx_create_feature_ = nullptr;
    PFN_NVSDK_NGX_RELEASE_FEATURE nvngx_release_feature_ = nullptr;

    bool LoadNgxDll();
    void UnloadNgxDll();
    bool CreateParameters();
    void DestroyParameters();
    void GenerateJitterSequence();
    std::pair<float, float> GetNextJitter();
};

} // namespace ScaleNG