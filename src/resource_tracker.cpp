#include "resource_tracker.h"
#include "dxgi_hooks.h"
#include <algorithm>

namespace ScaleNG {

void ResourceTracker::Initialize() {
    SNG_REGISTER_EVENT(InitResource, [this](const void* data) { return OnInitResource(data); }, 100);
    SNG_REGISTER_EVENT(CopyTextureRegion, [this](const void* data) { return OnCopyTextureRegion(data); }, 100);
    SNG_REGISTER_EVENT(CopyBufferToTexture, [this](const void* data) { return OnCopyBufferToTexture(data); }, 100);
    SNG_REGISTER_EVENT(InitSwapchain, [this](const void* data) { return OnInitSwapchain(data); }, 100);
    SNG_REGISTER_EVENT(Present, [this](const void* data) { return OnPresent(data); }, 100);
}

bool ResourceTracker::OnInitResource(const void* event_data) {
    const auto* evt = static_cast<const InitResourceEvent*>(event_data);
    if (!evt || !evt->resource || !evt->desc) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resource_index_.find(evt->resource);
    if (it != resource_index_.end()) {
        resources_[it->second].last_seen_frame = current_frame_;
        return false;
    }

    ResourceType type = ClassifyResource(evt->desc);
    
    if (evt->desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        evt->desc->SampleDesc.Count > 1 ||
        evt->desc->Width < 100 || evt->desc->Height < 100) {
        return false;
    }

    TrackedResource tracked;
    tracked.resource = evt->resource;
    tracked.desc = *evt->desc;
    tracked.type = type;
    tracked.first_seen_frame = current_frame_;
    tracked.last_seen_frame = current_frame_;
    tracked.is_persistent = (evt->initial_data != nullptr);
    
    evt->resource->AddRef();

    size_t index = resources_.size();
    resources_.push_back(std::move(tracked));
    resource_index_[evt->resource] = index;

    TrackedResource* res = &resources_[index];

    if (type == ResourceType::DepthStencil && !depth_resource_) {
        depth_resource_ = res;
        SNG_LOG("[Resource] Found DEPTH resource: %p, %ux%u, fmt=%u", 
            res->resource, res->desc.Width, res->desc.Height, res->desc.Format);
    }
    else if (type == ResourceType::MotionVectors && !motion_vector_resource_) {
        motion_vector_resource_ = res;
        SNG_LOG("[Resource] Found MOTION VECTOR resource: %p, %ux%u, fmt=%u",
            res->resource, res->desc.Width, res->desc.Height, res->desc.Format);
    }
    else if ((type == ResourceType::ColorHDR || type == ResourceType::ColorLDR) && !color_resource_) {
        color_resource_ = res;
        SNG_LOG("[Resource] Found COLOR resource: %p, %ux%u, fmt=%u, type=%s",
            res->resource, res->desc.Width, res->desc.Height, res->desc.Format,
            type == ResourceType::ColorHDR ? "HDR" : "LDR");
    }
    else if (type == ResourceType::BackBuffer) {
        back_buffer_resource_ = res;
        SNG_LOG("[Resource] Found BACK BUFFER: %p, %ux%u, fmt=%u",
            res->resource, res->desc.Width, res->desc.Height, res->desc.Format);
    }
    else {
        SNG_LOG("[Resource] Found OTHER resource: %p, %ux%u, fmt=%u, type=%d",
            res->resource, res->desc.Width, res->desc.Height, res->desc.Format, static_cast<int>(type));
    }

    return false;
}

bool ResourceTracker::OnCopyTextureRegion(const void* event_data) {
    const auto* evt = static_cast<const CopyTextureRegionEvent*>(event_data);
    if (!evt || !evt->cmd_list) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    ID3D12Resource* src_resource = evt->src->pResource;
    ID3D12Resource* dst_resource = evt->dst->pResource;

    if (!src_resource || !dst_resource) return false;

    auto src_it = resource_index_.find(src_resource);
    auto dst_it = resource_index_.find(dst_resource);

    if (src_it != resource_index_.end()) {
        resources_[src_it->second].copy_dests.push_back(*evt->dst);
        resources_[src_it->second].last_seen_frame = current_frame_;
    }

    if (dst_it != resource_index_.end()) {
        resources_[dst_it->second].copy_sources.push_back(*evt->src);
        resources_[dst_it->second].last_seen_frame = current_frame_;
    }

    if (dst_it != resource_index_.end()) {
        TrackedResource* dst_res = &resources_[dst_it->second];
        if (dst_res == depth_resource_ || dst_res == motion_vector_resource_ || dst_res == color_resource_) {
            D3D12_RESOURCE_DESC src_desc = src_resource->GetDesc();
            SNG_LOG("[Resource] COPY to %s: src=%p (%ux%u, fmt=%u) -> dst=%p (%ux%u)",
                dst_res == depth_resource_ ? "DEPTH" : (dst_res == motion_vector_resource_ ? "MV" : "COLOR"),
                src_resource, src_desc.Width, src_desc.Height, src_desc.Format,
                dst_resource, dst_res->desc.Width, dst_res->desc.Height);
        }
    }

    return false;
}

bool ResourceTracker::OnCopyBufferToTexture(const void* event_data) {
    const auto* evt = static_cast<const CopyBufferToTextureEvent*>(event_data);
    if (!evt || !evt->cmd_list || !evt->src_buffer || !evt->dest_texture) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resource_index_.find(evt->dest_texture);
    if (it != resource_index_.end()) {
        resources_[it->second].last_seen_frame = current_frame_;
        D3D12_RESOURCE_DESC src_desc = evt->src_buffer->GetDesc();
        SNG_LOG("[Resource] UPLOAD to %p: src_buffer=%p (size=%llu) -> dst=%p (%ux%u, fmt=%u)",
            evt->dest_texture, evt->src_buffer, src_desc.Width, 
            evt->dest_texture, resources_[it->second].desc.Width, resources_[it->second].desc.Height);
    }

    return false;
}

bool ResourceTracker::OnInitSwapchain(const void* event_data) {
    const auto* evt = static_cast<const InitSwapchainEvent*>(event_data);
    if (!evt || !evt->swapchain) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    
    swapchain_ = evt->swapchain;
    swapchain_->AddRef();

    DXGI_SWAP_CHAIN_DESC1 desc;
    swapchain_->GetDesc1(&desc);
    
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(swapchain_->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&bb))) {
            D3D12_RESOURCE_DESC bb_desc = bb->GetDesc();
            TrackedResource tracked;
            tracked.resource = bb;
            tracked.desc = bb_desc;
            tracked.type = ResourceType::BackBuffer;
            tracked.first_seen_frame = current_frame_;
            tracked.last_seen_frame = current_frame_;
            tracked.is_persistent = true;
            
            size_t index = resources_.size();
            resources_.push_back(std::move(tracked));
            resource_index_[bb] = index;
            
            if (!back_buffer_resource_) {
                back_buffer_resource_ = &resources_[index];
            }
            
            SNG_LOG("[Resource] Back buffer %u: %p, %ux%u, fmt=%u", 
                i, bb, bb_desc.Width, bb_desc.Height, bb_desc.Format);
            
            bb->Release();
        }
    }

    return false;
}

bool ResourceTracker::OnPresent(const void* event_data) {
    NextFrame();
    return false;
}

TrackedResource* ResourceTracker::GetDepthResource() {
    std::lock_guard<std::mutex> lock(mutex_);
    return depth_resource_;
}

TrackedResource* ResourceTracker::GetMotionVectorResource() {
    std::lock_guard<std::mutex> lock(mutex_);
    return motion_vector_resource_;
}

TrackedResource* ResourceTracker::GetColorResource() {
    std::lock_guard<std::mutex> lock(mutex_);
    return color_resource_;
}

TrackedResource* ResourceTracker::GetBackBufferResource() {
    std::lock_guard<std::mutex> lock(mutex_);
    return back_buffer_resource_;
}

const std::vector<TrackedResource*>& ResourceTracker::GetAllResources() const {
    static std::vector<TrackedResource*> ptrs;
    ptrs.clear();
    for (const auto& res : resources_) {
        ptrs.push_back(const_cast<TrackedResource*>(&res));
    }
    return ptrs;
}

ResourceType ResourceTracker::ClassifyResource(const D3D12_RESOURCE_DESC* desc) {
    if (!desc) return ResourceType::Unknown;

    DXGI_FORMAT fmt = desc->Format;

    if (IsDepthFormat(fmt)) return ResourceType::DepthStencil;
    if (IsMotionVectorFormat(fmt)) return ResourceType::MotionVectors;
    if (IsHDRColorFormat(fmt)) return ResourceType::ColorHDR;
    if (IsLDRColorFormat(fmt)) return ResourceType::ColorLDR;
    
    if (IsBackBufferFormat(fmt) && (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
        return ResourceType::BackBuffer;
    }

    return ResourceType::Unknown;
}

bool ResourceTracker::IsDepthFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            return true;
        default:
            return false;
    }
}

bool ResourceTracker::IsMotionVectorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
            return true;
        default:
            return false;
    }
}

bool ResourceTracker::IsHDRColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
            return true;
        default:
            return false;
    }
}

bool ResourceTracker::IsLDRColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return true;
        default:
            return false;
    }
}

bool ResourceTracker::IsBackBufferFormat(DXGI_FORMAT fmt) {
    return IsLDRColorFormat(fmt) || fmt == DXGI_FORMAT_R10G10B10A2_UNORM;
}

} // namespace ScaleNG