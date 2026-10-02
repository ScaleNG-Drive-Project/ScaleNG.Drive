#pragma once

#include "dxgi_hooks.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace ScaleNG {

enum class ResourceType {
    Unknown,
    DepthStencil,
    MotionVectors,
    ColorHDR,
    ColorLDR,
    BackBuffer
};

struct TrackedResource {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_DESC desc = {};
    ResourceType type = ResourceType::Unknown;
    uint64_t first_seen_frame = 0;
    uint64_t last_seen_frame = 0;
    bool is_persistent = false;
    std::vector<D3D12_TEXTURE_COPY_LOCATION> copy_sources;
    std::vector<D3D12_TEXTURE_COPY_LOCATION> copy_dests;
};

class ResourceTracker {
public:
    static ResourceTracker& Get() {
        static ResourceTracker instance;
        return instance;
    }

    void Initialize();

    bool OnInitResource(const void* event_data);
    bool OnCopyTextureRegion(const void* event_data);
    bool OnCopyBufferToTexture(const void* event_data);
    bool OnInitSwapchain(const void* event_data);
    bool OnPresent(const void* event_data);

    TrackedResource* GetDepthResource();
    TrackedResource* GetMotionVectorResource();
    TrackedResource* GetColorResource();
    TrackedResource* GetBackBufferResource();

    const std::vector<TrackedResource*>& GetAllResources() const;

    static ResourceType ClassifyResource(const D3D12_RESOURCE_DESC* desc);
    static bool IsDepthFormat(DXGI_FORMAT fmt);
    static bool IsMotionVectorFormat(DXGI_FORMAT fmt);
    static bool IsHDRColorFormat(DXGI_FORMAT fmt);
    static bool IsLDRColorFormat(DXGI_FORMAT fmt);
    static bool IsBackBufferFormat(DXGI_FORMAT fmt);

    void NextFrame() { current_frame_++; }
    uint64_t GetCurrentFrame() const { return current_frame_; }

private:
    ResourceTracker() = default;

    std::mutex mutex_;
    std::vector<TrackedResource> resources_;
    std::unordered_map<ID3D12Resource*, size_t> resource_index_;
    
    TrackedResource* depth_resource_ = nullptr;
    TrackedResource* motion_vector_resource_ = nullptr;
    TrackedResource* color_resource_ = nullptr;
    TrackedResource* back_buffer_resource_ = nullptr;

    uint64_t current_frame_ = 0;
    IDXGISwapChain3* swapchain_ = nullptr;
};

} // namespace ScaleNG