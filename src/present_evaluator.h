#pragma once

#include "dxgi_hooks.h"
#include "ngx_evaluator.h"
#include "resource_tracker.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mutex>

namespace ScaleNG {

class PresentEvaluator {
public:
    static PresentEvaluator& Get() {
        static PresentEvaluator instance;
        return instance;
    }

    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapchain);
    void OnPresent(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags);
    void OnResize(IDXGISwapChain3* swapchain);
    void Shutdown();

    bool IsReady() const { return ready_; }

    struct Stats {
        uint64_t frames_evaluated = 0;
        uint64_t frames_failed = 0;
        uint64_t last_present_time = 0;
        float avg_eval_time_ms = 0.0f;
    };
    Stats GetStats();

private:
    PresentEvaluator() = default;

    std::mutex mutex_;
    bool ready_ = false;
    bool resizing_ = false;

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* graphics_queue_ = nullptr;
    IDXGISwapChain3* swapchain_ = nullptr;

    ID3D12Resource* color_input_ = nullptr;
    ID3D12Resource* ngx_output_ = nullptr;
    
    ID3D12CommandAllocator* cmd_allocator_ = nullptr;
    ID3D12GraphicsCommandList* cmd_list_ = nullptr;

    ID3D12Fence* fence_ = nullptr;
    UINT64 fence_value_ = 0;
    HANDLE fence_event_ = nullptr;

    UINT render_width_ = 0;
    UINT render_height_ = 0;
    UINT display_width_ = 0;
    UINT display_height_ = 0;
    UINT back_buffer_count_ = 0;
    DXGI_FORMAT back_buffer_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;

    Stats stats_;

    bool CreateRenderResources();
    void DestroyRenderResources();
    bool CreateCommandObjects();
    void DestroyCommandObjects();
    bool CreateSyncObjects();
    void DestroySyncObjects();

    void ExecutePresentPipeline(ID3D12Resource* current_back_buffer);
    
    void Barrier(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* resource,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void BarrierUAV(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* resource);

    void WaitForFence(UINT64 value);
    void SignalFence(UINT64 value);

    ID3D12Resource* GetCurrentBackBuffer();
};

} // namespace ScaleNG