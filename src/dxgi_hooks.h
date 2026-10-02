#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <functional>
#include <mutex>
#include <vector>

// Forward declarations
struct IDXGIFactory1;
struct IDXGISwapChain3;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

// ============================================================================
// NGX Function Pointer Types (from NVSDK_NGX)
// ============================================================================

// Basic NGX types
typedef struct NVSDK_NGX_Handle NVSDK_NGX_Handle;
typedef struct NVSDK_NGX_Parameter NVSDK_NGX_Parameter;

typedef enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0,
    NVSDK_NGX_Result_Fail = 1,
    NVSDK_NGX_Result_InvalidParameter = 2,
    NVSDK_NGX_Result_OutOfMemory = 3,
    NVSDK_NGX_Result_NotSupported = 4,
    NVSDK_NGX_Result_NotInitialized = 5,
    NVSDK_NGX_Result_NotFound = 6,
    NVSDK_NGX_Result_AlreadyInitialized = 7,
    NVSDK_NGX_Result_DeviceNotSupported = 8,
    NVSDK_NGX_Result_DriverNotSupported = 9,
    NVSDK_NGX_Result_ModelNotFound = 10,
    NVSDK_NGX_Result_ModelWrongVersion = 11,
    NVSDK_NGX_Result_ModelCorrupted = 12,
    NVSDK_NGX_Result_FeatureNotSupported = 13,
    NVSDK_NGX_Result_FeatureNotEnabled = 14,
    NVSDK_NGX_Result_FeatureAlreadyEnabled = 15,
    NVSDK_NGX_Result_FeatureCreateFailed = 16,
    NVSDK_NGX_Result_DeviceLost = 17,
    NVSDK_NGX_Result_QueueNotSupported = 18,
    NVSDK_NGX_Result_UnsupportedFormat = 19,
} NVSDK_NGX_Result;

typedef enum NVSDK_NGX_Feature {
    NVSDK_NGX_Feature_ImageSuperResolution = 1,
    NVSDK_NGX_Feature_DepthSuperResolution = 2,
    NVSDK_NGX_Feature_RayReconstruction = 3,
    NVSDK_NGX_Feature_DLSS = 4,
    NVSDK_NGX_Feature_DLAA = 5,
} NVSDK_NGX_Feature;

// Function pointer types
typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_D3D12_CREATE_DLSS_EXT)(
    ID3D12Device*, ID3D12CommandQueue*, void*, NVSDK_NGX_Feature, unsigned int, void*);

typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_D3D12_EVALUATE_FEATURE)(
    ID3D12Device*, ID3D12CommandQueue*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);

typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_D3D12_GET_CAPABILITIES)(
    ID3D12Device*, NVSDK_NGX_Feature, void*);

typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_D3D12_GET_OPTIMAL_SETTINGS)(
    ID3D12Device*, NVSDK_NGX_Feature, void*);

typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_D3D12_SHUTDOWN)();

typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_ALLOCATE_PARAMETERS)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_DESTROY_PARAMETERS)(NVSDK_NGX_Parameter*);
typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_CREATE_FEATURE)(NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (WINAPI *PFN_NVSDK_NGX_RELEASE_FEATURE)(NVSDK_NGX_Handle*);

// ============================================================================
// Configuration
// ============================================================================

namespace ScaleNG {

struct Config {
    bool enabled = true;
    bool dlaa = true;
    float render_scale = 0.67f;
    float sharpness = 0.0f;
    int perf_quality = 1;
    bool mv_jittered = true;
    bool auto_exposure = true;
    UINT app_id = 0xE658700;
    wchar_t dlss_dll_path[MAX_PATH] = {};
    wchar_t log_path[MAX_PATH] = {};
};

// ============================================================================
// Global State (Singleton)
// ============================================================================

class State {
public:
    static State& Get() {
        static State instance;
        return instance;
    }

    Config config;
    std::mutex state_mutex;

    // Captured objects
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* graphics_queue = nullptr;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12Resource* back_buffer = nullptr;

    // Discovered resources
    ID3D12Resource* depth_resource = nullptr;
    ID3D12Resource* motion_vector_resource = nullptr;
    ID3D12Resource* color_resource = nullptr;

private:
    State() = default;
    ~State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
};

// ============================================================================
// Logging
// ============================================================================

void LogMessage(const char* format, ...);
void SetDllPath(HMODULE hModule);
void SetConfig(const Config& cfg);
Config GetConfig();

} // namespace ScaleNG

// Convenience macros
#define SNG_STATE ScaleNG::State::Get()
#define SNG_CONFIG ScaleNG::State::Get().config
#define SNG_LOCK std::lock_guard<std::mutex> _lock(ScaleNG::State::Get().state_mutex)
#define SNG_LOG ScaleNG::LogMessage

// ============================================================================
// Event System (Mirroring ReShade's addon_event)
// ============================================================================

namespace ScaleNG {

enum class EventType {
    InitDevice,
    InitSwapchain,
    InitResource,
    UpdateTextureRegion,
    CopyBufferToTexture,
    CopyTextureRegion,
    InitEffectRuntime,
    Present,
    DestroySwapchain,
    DestroyDevice
};

struct InitDeviceEvent {
    ID3D12Device* device;
};

struct InitSwapchainEvent {
    IDXGISwapChain3* swapchain;
    bool resize;
};

struct InitResourceEvent {
    ID3D12Device* device;
    const D3D12_RESOURCE_DESC* desc;
    const D3D12_SUBRESOURCE_DATA* initial_data;
    ID3D12Resource* resource;
};

struct UpdateTextureRegionEvent {
    ID3D12Device* device;
    const D3D12_SUBRESOURCE_DATA* data;
    ID3D12Resource* dest;
    UINT dest_subresource;
    const D3D12_BOX* dest_box;
};

struct CopyBufferToTextureEvent {
    ID3D12GraphicsCommandList* cmd_list;
    ID3D12Resource* src_buffer;
    UINT64 src_offset;
    ID3D12Resource* dest_texture;
    UINT dest_subresource;
    const D3D12_BOX* dest_box;
};

struct CopyTextureRegionEvent {
    ID3D12GraphicsCommandList* cmd_list;
    const D3D12_TEXTURE_COPY_LOCATION* dst;
    UINT dst_x, dst_y, dst_z;
    const D3D12_TEXTURE_COPY_LOCATION* src;
    const D3D12_BOX* src_box;
};

struct InitEffectRuntimeEvent {
    IDXGISwapChain3* swapchain;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
};

struct PresentEvent {
    IDXGISwapChain3* swapchain;
    UINT sync_interval;
    UINT flags;
};

struct DestroySwapchainEvent {
    IDXGISwapChain3* swapchain;
    bool resize;
};

struct DestroyDeviceEvent {
    ID3D12Device* device;
};

using EventCallback = std::function<bool(const void*)>;

struct EventRegistration {
    EventType type;
    EventCallback callback;
    int priority;
    void* user_data;
};

class EventSystem {
public:
    static EventSystem& Get() {
        static EventSystem instance;
        return instance;
    }

    void RegisterEvent(EventType type, EventCallback callback, int priority = 0, void* user_data = nullptr);
    void UnregisterEvent(EventType type, EventCallback callback);
    bool FireEvent(EventType type, const void* event_data);

    bool FireInitDevice(ID3D12Device* device);
    bool FireInitSwapchain(IDXGISwapChain3* swapchain, bool resize);
    bool FireInitResource(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc, 
                          const D3D12_SUBRESOURCE_DATA* initial_data, ID3D12Resource* resource);
    bool FireUpdateTextureRegion(ID3D12Device* device, const D3D12_SUBRESOURCE_DATA* data,
                                 ID3D12Resource* dest, UINT dest_subresource, const D3D12_BOX* dest_box);
    bool FireCopyBufferToTexture(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* src_buffer,
                                 UINT64 src_offset, ID3D12Resource* dest_texture, UINT dest_subresource,
                                 const D3D12_BOX* dest_box);
    bool FireCopyTextureRegion(ID3D12GraphicsCommandList* cmd_list,
                               const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dst_x, UINT dst_y, UINT dst_z,
                               const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* src_box);
    bool FireInitEffectRuntime(IDXGISwapChain3* swapchain, ID3D12Device* device, ID3D12CommandQueue* queue);
    bool FirePresent(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags);
    bool FireDestroySwapchain(IDXGISwapChain3* swapchain, bool resize);
    bool FireDestroyDevice(ID3D12Device* device);

    void Clear();

private:
    EventSystem() = default;
    std::mutex mutex_;
    std::vector<EventRegistration> registrations_[10];
};

} // namespace ScaleNG

// Event registration macros
#define SNG_REGISTER_EVENT(type, callback, priority) \
    ScaleNG::EventSystem::Get().RegisterEvent(ScaleNG::EventType::type, callback, priority)

#define SNG_FIRE_EVENT(type, ...) \
    ScaleNG::EventSystem::Get().Fire##type(__VA_ARGS__)

// ============================================================================
// DXGI Hook Functions
// ============================================================================

namespace ScaleNG {

bool InstallDXGIHooks();
void UninstallDXGIHooks();

// Hook function declarations
HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory);
HRESULT WINAPI Hook_CreateDXGIFactory2(REFIID riid, void** ppFactory);
HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void** ppFactory);

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain
);

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain3* This, UINT SyncInterval, UINT Flags);
HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain3* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

// Hook callbacks (set by main)
extern ScaleNG::EventCallback g_on_init_device;
extern ScaleNG::EventCallback g_on_init_swapchain;
extern ScaleNG::EventCallback g_on_init_resource;
extern ScaleNG::EventCallback g_on_update_texture_region;
extern ScaleNG::EventCallback g_on_copy_buffer_to_texture;
extern ScaleNG::EventCallback g_on_copy_texture_region;
extern ScaleNG::EventCallback g_on_init_effect_runtime;
extern ScaleNG::EventCallback g_on_present;
extern ScaleNG::EventCallback g_on_destroy_swapchain;

} // namespace ScaleNG