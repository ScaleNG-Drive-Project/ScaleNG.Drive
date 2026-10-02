#include "dxgi_hooks.h"
#include <algorithm>

namespace ScaleNG {

void EventSystem::RegisterEvent(EventType type, EventCallback callback, int priority, void* user_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    int idx = static_cast<int>(type);
    EventRegistration reg{type, callback, priority, user_data};
    
    auto& vec = registrations_[idx];
    auto it = std::find_if(vec.begin(), vec.end(), 
        [priority](const EventRegistration& r) { return r.priority < priority; });
    vec.insert(it, std::move(reg));
    
    SNG_LOG("[Event] Registered callback for type %d, priority %d", idx, priority);
}

void EventSystem::UnregisterEvent(EventType type, EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    int idx = static_cast<int>(type);
    auto& vec = registrations_[idx];
    // Note: std::function comparison is not straightforward
}

bool EventSystem::FireEvent(EventType type, const void* event_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    int idx = static_cast<int>(type);
    auto& vec = registrations_[idx];
    
    for (auto& reg : vec) {
        if (reg.callback && reg.callback(event_data)) {
            return true;
        }
    }
    return false;
}

bool EventSystem::FireInitDevice(ID3D12Device* device) {
    InitDeviceEvent evt{device};
    return FireEvent(EventType::InitDevice, &evt);
}

bool EventSystem::FireInitSwapchain(IDXGISwapChain3* swapchain, bool resize) {
    InitSwapchainEvent evt{swapchain, resize};
    return FireEvent(EventType::InitSwapchain, &evt);
}

bool EventSystem::FireInitResource(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc, 
                                   const D3D12_SUBRESOURCE_DATA* initial_data, ID3D12Resource* resource) {
    InitResourceEvent evt{device, desc, initial_data, resource};
    return FireEvent(EventType::InitResource, &evt);
}

bool EventSystem::FireUpdateTextureRegion(ID3D12Device* device, const D3D12_SUBRESOURCE_DATA* data,
                                          ID3D12Resource* dest, UINT dest_subresource, const D3D12_BOX* dest_box) {
    UpdateTextureRegionEvent evt{device, data, dest, dest_subresource, dest_box};
    return FireEvent(EventType::UpdateTextureRegion, &evt);
}

bool EventSystem::FireCopyBufferToTexture(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* src_buffer,
                                          UINT64 src_offset, ID3D12Resource* dest_texture, UINT dest_subresource,
                                          const D3D12_BOX* dest_box) {
    CopyBufferToTextureEvent evt{cmd_list, src_buffer, src_offset, dest_texture, dest_subresource, dest_box};
    return FireEvent(EventType::CopyBufferToTexture, &evt);
}

bool EventSystem::FireCopyTextureRegion(ID3D12GraphicsCommandList* cmd_list,
                                        const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dst_x, UINT dst_y, UINT dst_z,
                                        const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* src_box) {
    CopyTextureRegionEvent evt{cmd_list, dst, dst_x, dst_y, dst_z, src, src_box};
    return FireEvent(EventType::CopyTextureRegion, &evt);
}

bool EventSystem::FireInitEffectRuntime(IDXGISwapChain3* swapchain, ID3D12Device* device, ID3D12CommandQueue* queue) {
    InitEffectRuntimeEvent evt{swapchain, device, queue};
    return FireEvent(EventType::InitEffectRuntime, &evt);
}

bool EventSystem::FirePresent(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags) {
    PresentEvent evt{swapchain, sync_interval, flags};
    return FireEvent(EventType::Present, &evt);
}

bool EventSystem::FireDestroySwapchain(IDXGISwapChain3* swapchain, bool resize) {
    DestroySwapchainEvent evt{swapchain, resize};
    return FireEvent(EventType::DestroySwapchain, &evt);
}

bool EventSystem::FireDestroyDevice(ID3D12Device* device) {
    DestroyDeviceEvent evt{device};
    return FireEvent(EventType::DestroyDevice, &evt);
}

void EventSystem::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& vec : registrations_) {
        vec.clear();
    }
}

} // namespace ScaleNG