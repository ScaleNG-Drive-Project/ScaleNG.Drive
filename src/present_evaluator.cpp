#include "present_evaluator.h"
#include "dxgi_hooks.h"
#include "resource_tracker.h"
#include "ngx_evaluator.h"
#include <chrono>

namespace ScaleNG {

bool PresentEvaluator::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapchain) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (ready_) {
        SNG_LOG("[Present] Already initialized");
        return true;
    }

    device_ = device;
    graphics_queue_ = queue;
    swapchain_ = swapchain;
    
    if (!device_ || !graphics_queue_ || !swapchain_) {
        SNG_LOG("[Present] Invalid parameters");
        return false;
    }

    device_->AddRef();
    graphics_queue_->AddRef();
    swapchain_->AddRef();

    DXGI_SWAP_CHAIN_DESC1 desc;
    swapchain_->GetDesc1(&desc);
    back_buffer_count_ = desc.BufferCount;
    back_buffer_format_ = desc.Format;
    display_width_ = desc.Width;
    display_height_ = desc.Height;

    const auto& config = SNG_STATE.config;
    if (config.dlaa) {
        render_width_ = display_width_;
        render_height_ = display_height_;
    } else {
        NgxEvaluator::GetOptimalRenderDimensions(display_width_, display_height_, config.perf_quality, 
                                                  render_width_, render_height_);
    }

    SNG_LOG("[Present] Initializing: display=%ux%u, render=%ux%u, format=%u, buffers=%u",
        display_width_, display_height_, render_width_, render_height_, back_buffer_format_, back_buffer_count_);

    if (!CreateSyncObjects()) return false;
    if (!CreateCommandObjects()) return false;
    if (!CreateRenderResources()) return false;

    NgxInitParams ngx_params;
    ngx_params.device = device_;
    ngx_params.queue = graphics_queue_;
    ngx_params.app_id = config.app_id;
    wcscpy_s(ngx_params.dll_path, config.dlss_dll_path);
    ngx_params.dlaa = config.dlaa;
    ngx_params.perf_quality = config.perf_quality;

    if (!NgxEvaluator::Get().Initialize(ngx_params)) {
        SNG_LOG("[Present] NGX initialization failed");
        return false;
    }

    if (!NgxEvaluator::Get().CreateFeature(render_width_, render_height_, display_width_, display_height_)) {
        SNG_LOG("[Present] NGX feature creation failed");
        return false;
    }

    ready_ = true;
    SNG_LOG("[Present] Initialized successfully");
    return true;
}

bool PresentEvaluator::CreateSyncObjects() {
    HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence_);
    if (FAILED(hr)) {
        SNG_LOG("[Present] Failed to create fence: 0x%08X", hr);
        return false;
    }
    fence_value_ = 1;

    fence_event_ = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!fence_event_) {
        SNG_LOG("[Present] Failed to create fence event");
        return false;
    }

    return true;
}

bool PresentEvaluator::CreateCommandObjects() {
    HRESULT hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, 
                                                  __uuidof(ID3D12CommandAllocator), (void**)&cmd_allocator_);
    if (FAILED(hr)) {
        SNG_LOG("[Present] Failed to create command allocator: 0x%08X", hr);
        return false;
    }

    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocator_, nullptr,
                                    __uuidof(ID3D12GraphicsCommandList), (void**)&cmd_list_);
    if (FAILED(hr)) {
        SNG_LOG("[Present] Failed to create command list: 0x%08X", hr);
        return false;
    }

    cmd_list_->Close();
    return true;
}

bool PresentEvaluator::CreateRenderResources() {
    D3D12_RESOURCE_DESC color_desc = {};
    color_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    color_desc.Width = render_width_;
    color_desc.Height = render_height_;
    color_desc.DepthOrArraySize = 1;
    color_desc.MipLevels = 1;
    color_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    color_desc.SampleDesc.Count = 1;
    color_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    color_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clear_value.Color[0] = 0.0f;
    clear_value.Color[1] = 0.0f;
    clear_value.Color[2] = 0.0f;
    clear_value.Color[3] = 1.0f;

    HRESULT hr = device_->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE,
                                                   &color_desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                   &clear_value, __uuidof(ID3D12Resource), (void**)&color_input_);
    if (FAILED(hr)) {
        SNG_LOG("[Present] Failed to create color input: 0x%08X", hr);
        return false;
    }
    color_input_->SetName(L"ScaleNG_ColorInput");

    D3D12_RESOURCE_DESC output_desc = color_desc;
    output_desc.Width = display_width_;
    output_desc.Height = display_height_;
    output_desc.Format = back_buffer_format_;
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE output_clear = {};
    output_clear.Format = back_buffer_format_;
    output_clear.Color[0] = 0.0f;
    output_clear.Color[1] = 0.0f;
    output_clear.Color[2] = 0.0f;
    output_clear.Color[3] = 1.0f;

    hr = device_->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE,
                                           &output_desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                           &output_clear, __uuidof(ID3D12Resource), (void**)&ngx_output_);
    if (FAILED(hr)) {
        SNG_LOG("[Present] Failed to create NGX output: 0x%08X", hr);
        return false;
    }
    ngx_output_->SetName(L"ScaleNG_NGXOutput");

    SNG_LOG("[Present] Created render resources: color_input=%p, ngx_output=%p", color_input_, ngx_output_);
    return true;
}

void PresentEvaluator::DestroyRenderResources() {
    if (color_input_) { color_input_->Release(); color_input_ = nullptr; }
    if (ngx_output_) { ngx_output_->Release(); ngx_output_ = nullptr; }
}

void PresentEvaluator::DestroyCommandObjects() {
    if (cmd_list_) { cmd_list_->Release(); cmd_list_ = nullptr; }
    if (cmd_allocator_) { cmd_allocator_->Release(); cmd_allocator_ = nullptr; }
}

void PresentEvaluator::DestroySyncObjects() {
    if (fence_event_) { CloseHandle(fence_event_); fence_event_ = nullptr; }
    if (fence_) { fence_->Release(); fence_ = nullptr; }
}

void PresentEvaluator::OnResize(IDXGISwapChain3* swapchain) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!ready_) return;
    
    SNG_LOG("[Present] Resize detected");
    resizing_ = true;

    WaitForFence(fence_value_);

    if (swapchain_) swapchain_->Release();
    swapchain_ = swapchain;
    swapchain_->AddRef();

    DXGI_SWAP_CHAIN_DESC1 desc;
    swapchain_->GetDesc1(&desc);
    display_width_ = desc.Width;
    display_height_ = desc.Height;
    back_buffer_format_ = desc.Format;
    back_buffer_count_ = desc.BufferCount;

    const auto& config = SNG_STATE.config;
    if (config.dlaa) {
        render_width_ = display_width_;
        render_height_ = display_height_;
    } else {
        NgxEvaluator::GetOptimalRenderDimensions(display_width_, display_height_, config.perf_quality, 
                                                  render_width_, render_height_);
    }

    SNG_LOG("[Present] Resized: display=%ux%u, render=%ux%u", 
        display_width_, display_height_, render_width_, render_height_);

    DestroyRenderResources();
    if (!CreateRenderResources()) {
        SNG_LOG("[Present] Failed to recreate render resources after resize");
        ready_ = false;
        return;
    }

    NgxEvaluator::Get().Shutdown();
    
    NgxInitParams ngx_params;
    ngx_params.device = device_;
    ngx_params.queue = graphics_queue_;
    ngx_params.app_id = SNG_STATE.config.app_id;
    wcscpy_s(ngx_params.dll_path, SNG_STATE.config.dlss_dll_path);
    ngx_params.dlaa = config.dlaa;
    ngx_params.perf_quality = config.perf_quality;

    if (!NgxEvaluator::Get().Initialize(ngx_params) ||
        !NgxEvaluator::Get().CreateFeature(render_width_, render_height_, display_width_, display_height_)) {
        SNG_LOG("[Present] Failed to recreate NGX feature after resize");
        ready_ = false;
        return;
    }

    resizing_ = false;
}

void PresentEvaluator::OnPresent(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!ready_ || resizing_) return;
    
    if (swapchain != swapchain_) {
        SNG_LOG("[Present] Swapchain mismatch: expected %p, got %p", swapchain_, swapchain);
        return;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    ID3D12Resource* back_buffer = GetCurrentBackBuffer();
    if (!back_buffer) {
        SNG_LOG("[Present] Failed to get current back buffer");
        stats_.frames_failed++;
        return;
    }

    ExecutePresentPipeline(back_buffer);
    back_buffer->Release();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    stats_.avg_eval_time_ms = stats_.avg_eval_time_ms * 0.9f + (duration.count() / 1000.0f) * 0.1f;
    stats_.frames_evaluated++;
    stats_.last_present_time = GetTickCount64();
}

ID3D12Resource* PresentEvaluator::GetCurrentBackBuffer() {
    UINT current_back_buffer_index = swapchain_->GetCurrentBackBufferIndex();
    ID3D12Resource* back_buffer = nullptr;
    HRESULT hr = swapchain_->GetBuffer(current_back_buffer_index, __uuidof(ID3D12Resource), (void**)&back_buffer);
    if (FAILED(hr)) {
        SNG_LOG("[Present] GetBuffer failed: 0x%08X", hr);
        return nullptr;
    }
    return back_buffer;
}

void PresentEvaluator::ExecutePresentPipeline(ID3D12Resource* back_buffer) {
    HRESULT hr = cmd_allocator_->Reset();
    if (FAILED(hr)) {
        SNG_LOG("[Present] CmdAllocator reset failed: 0x%08X", hr);
        return;
    }

    hr = cmd_list_->Reset(cmd_allocator_, nullptr);
    if (FAILED(hr)) {
        SNG_LOG("[Present] CmdList reset failed: 0x%08X", hr);
        return;
    }

    // Step 1: Copy backbuffer (PRESENT) -> color_input (COPY_SOURCE)
    Barrier(cmd_list_, back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd_list_, color_input_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    
    cmd_list_->CopyResource(color_input_, back_buffer);
    
    Barrier(cmd_list_, color_input_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd_list_, back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);

    // Step 2: Get depth and motion vector resources
    auto& tracker = ResourceTracker::Get();
    TrackedResource* depth_res = tracker.GetDepthResource();
    TrackedResource* mv_res = tracker.GetMotionVectorResource();

    if (!depth_res || !depth_res->resource || !mv_res || !mv_res->resource) {
        SNG_LOG("[Present] Missing depth or MV resources (depth=%p, mv=%p)", 
            depth_res ? depth_res->resource : nullptr, mv_res ? mv_res->resource : nullptr);
    }

    if (depth_res && depth_res->resource) {
        Barrier(cmd_list_, depth_res->resource, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (mv_res && mv_res->resource) {
        Barrier(cmd_list_, mv_res->resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Step 3: NGX Evaluate
    NgxParameters ngx_params;
    ngx_params.color = color_input_;
    ngx_params.depth = depth_res ? depth_res->resource : nullptr;
    ngx_params.motion_vectors = mv_res ? mv_res->resource : nullptr;
    ngx_params.output = ngx_output_;
    ngx_params.jitter_x = 0.0f;
    ngx_params.jitter_y = 0.0f;
    ngx_params.sharpness = SNG_STATE.config.sharpness;
    ngx_params.auto_exposure = SNG_STATE.config.auto_exposure;
    ngx_params.render_width = render_width_;
    ngx_params.render_height = render_height_;
    ngx_params.display_width = display_width_;
    ngx_params.display_height = display_height_;

    bool ngx_success = NgxEvaluator::Get().Evaluate(ngx_params);

    // Step 4: Copy NGX output -> backbuffer
    if (ngx_success) {
        Barrier(cmd_list_, ngx_output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd_list_, back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        
        cmd_list_->CopyResource(back_buffer, ngx_output_);
        
        Barrier(cmd_list_, back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        Barrier(cmd_list_, ngx_output_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    } else {
        stats_.frames_failed++;
        Barrier(cmd_list_, back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
    }

    hr = cmd_list_->Close();
    if (FAILED(hr)) {
        SNG_LOG("[Present] CmdList close failed: 0x%08X", hr);
        return;
    }

    ID3D12CommandList* cmd_lists[] = { cmd_list_ };
    graphics_queue_->ExecuteCommandLists(1, cmd_lists);

    SignalFence(fence_value_);
}

void PresentEvaluator::Barrier(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* resource,
                               D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    cmd_list->ResourceBarrier(1, &barrier);
}

void PresentEvaluator::BarrierUAV(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;
    cmd_list->ResourceBarrier(1, &barrier);
}

void PresentEvaluator::WaitForFence(UINT64 value) {
    if (fence_->GetCompletedValue() >= value) return;
    
    HRESULT hr = fence_->SetEventOnCompletion(value, fence_event_);
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(fence_event_, 1000);
    }
}

void PresentEvaluator::SignalFence(UINT64 value) {
    graphics_queue_->Signal(fence_, value);
    fence_value_ = value + 1;
}

void PresentEvaluator::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!ready_) return;
    
    WaitForFence(fence_value_);
    
    NgxEvaluator::Get().Shutdown();
    
    DestroyRenderResources();
    DestroyCommandObjects();
    DestroySyncObjects();
    
    if (swapchain_) { swapchain_->Release(); swapchain_ = nullptr; }
    if (graphics_queue_) { graphics_queue_->Release(); graphics_queue_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    
    ready_ = false;
    SNG_LOG("[Present] Shutdown complete");
}

PresentEvaluator::Stats PresentEvaluator::GetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} // namespace ScaleNG