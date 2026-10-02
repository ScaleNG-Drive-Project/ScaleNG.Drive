#include "dxgi_hooks.h"
#include <MinHook.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>
#include <algorithm>

namespace ScaleNG {

// ============================================================================
// Global callback pointers (set by main)
// ============================================================================

EventCallback g_on_init_device = nullptr;
EventCallback g_on_init_swapchain = nullptr;
EventCallback g_on_init_resource = nullptr;
EventCallback g_on_update_texture_region = nullptr;
EventCallback g_on_copy_buffer_to_texture = nullptr;
EventCallback g_on_copy_texture_region = nullptr;
EventCallback g_on_init_effect_runtime = nullptr;
EventCallback g_on_present = nullptr;
EventCallback g_on_destroy_swapchain = nullptr;

// ============================================================================
// Original function pointers
// ============================================================================

using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);

PFN_CreateDXGIFactory1 g_orig_CreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 g_orig_CreateDXGIFactory2 = nullptr;
PFN_CreateDXGIFactory g_orig_CreateDXGIFactory = nullptr;

using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, 
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
PFN_CreateSwapChainForHwnd g_orig_CreateSwapChainForHwnd = nullptr;

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
PFN_Present g_orig_Present = nullptr;

using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PFN_ResizeBuffers g_orig_ResizeBuffers = nullptr;

// ============================================================================
// Hook function forward declarations
// ============================================================================

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

// ============================================================================
// Factory hooking
// ============================================================================

static std::mutex g_factory_hook_mutex;
static std::vector<IDXGIFactory2*> g_hooked_factories;

bool HookFactoryVTable(IDXGIFactory2* factory) {
    std::lock_guard<std::mutex> lock(g_factory_hook_mutex);
    
    if (std::find(g_hooked_factories.begin(), g_hooked_factories.end(), factory) != g_hooked_factories.end()) {
        return true;
    }
    
    void** vtable = *(void***)factory;
    
    // IDXGIFactory::CreateSwapChainForHwnd is at index 15
    const int SWAPCHAIN_FOR_HWND_INDEX = 15;
    
    void* target = vtable[SWAPCHAIN_FOR_HWND_INDEX];
    if (!g_orig_CreateSwapChainForHwnd) {
        g_orig_CreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd)target;
    }
    
    MH_STATUS status = MH_CreateHook(target, Hook_CreateSwapChainForHwnd, (void**)&g_orig_CreateSwapChainForHwnd);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) {
        SNG_LOG("[DXGI] Failed to hook CreateSwapChainForHwnd: %d", status);
        return false;
    }
    
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        SNG_LOG("[DXGI] Failed to enable hook: %d", status);
        return false;
    }
    
    g_hooked_factories.push_back(factory);
    factory->AddRef();
    
    SNG_LOG("[DXGI] Hooked factory %p vtable[%d]", factory, SWAPCHAIN_FOR_HWND_INDEX);
    return true;
}

// ============================================================================
// Hook implementations
// ============================================================================

HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    HRESULT hr = g_orig_CreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        SNG_LOG("[DXGI] CreateDXGIFactory1 succeeded, factory=%p", *ppFactory);
        
        // Try to hook this factory's vtable
        IDXGIFactory2* factory = nullptr;
        IUnknown* unknown = static_cast<IUnknown*>(*ppFactory);
        if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory))) {
            HookFactoryVTable(factory);
            factory->Release();
        }
    }
    return hr;
}

HRESULT WINAPI Hook_CreateDXGIFactory2(REFIID riid, void** ppFactory) {
    HRESULT hr = g_orig_CreateDXGIFactory2(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        SNG_LOG("[DXGI] CreateDXGIFactory2 succeeded, factory=%p", *ppFactory);
        
        IDXGIFactory2* factory = nullptr;
        IUnknown* unknown = static_cast<IUnknown*>(*ppFactory);
        if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory))) {
            HookFactoryVTable(factory);
            factory->Release();
        }
    }
    return hr;
}

HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    HRESULT hr = g_orig_CreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        SNG_LOG("[DXGI] CreateDXGIFactory succeeded, factory=%p", *ppFactory);
        
        IDXGIFactory2* factory = nullptr;
        IUnknown* unknown = static_cast<IUnknown*>(*ppFactory);
        if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory))) {
            HookFactoryVTable(factory);
            factory->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain
) {
    HRESULT hr = g_orig_CreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain3* swapchain = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swapchain))) {
            SNG_LOG("[DXGI] CreateSwapChainForHwnd succeeded, swapchain=%p, hwnd=%p", swapchain, hWnd);
            
            DXGI_SWAP_CHAIN_DESC1 desc;
            swapchain->GetDesc1(&desc);
            SNG_LOG("[DXGI] Swapchain desc: %ux%u, format=%u, buffer_count=%u, swap_effect=%u, flags=0x%X",
                desc.Width, desc.Height, desc.Format, desc.BufferCount, desc.SwapEffect, desc.Flags);
            
            // Fire init_swapchain event
            if (g_on_init_swapchain) {
                InitSwapchainEvent evt{swapchain, false};
                g_on_init_swapchain(&evt);
            }
            
            // Also hook Present and ResizeBuffers on this swapchain
            void** vtable = *(void***)swapchain;
            
            // Present is at index 8, ResizeBuffers at index 13 for IDXGISwapChain3
            if (!g_orig_Present) {
                g_orig_Present = (PFN_Present)vtable[8];
                MH_CreateHook(vtable[8], Hook_Present, (void**)&g_orig_Present);
                MH_EnableHook(vtable[8]);
            }
            
            if (!g_orig_ResizeBuffers) {
                g_orig_ResizeBuffers = (PFN_ResizeBuffers)vtable[13];
                MH_CreateHook(vtable[13], Hook_ResizeBuffers, (void**)&g_orig_ResizeBuffers);
                MH_EnableHook(vtable[13]);
            }
            
            swapchain->Release();
        }
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain3* This, UINT SyncInterval, UINT Flags) {
    if (g_on_present) {
        PresentEvent evt{This, SyncInterval, Flags};
        g_on_present(&evt);
    }
    
    return g_orig_Present(This, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain3* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    SNG_LOG("[DXGI] ResizeBuffers: %ux%u, buffer_count=%u, format=%u, flags=0x%X",
        Width, Height, BufferCount, NewFormat, SwapChainFlags);
    
    if (g_on_destroy_swapchain) {
        DestroySwapchainEvent evt{This, true};
        g_on_destroy_swapchain(&evt);
    }
    
    HRESULT hr = g_orig_ResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    
    if (SUCCEEDED(hr) && g_on_init_swapchain) {
        InitSwapchainEvent evt{This, true};
        g_on_init_swapchain(&evt);
    }
    
    return hr;
}

// ============================================================================
// Hook installation
// ============================================================================

bool InstallDXGIHooks() {
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        SNG_LOG("[DXGI] MinHook initialization failed: %d", status);
        return false;
    }
    
    // Get SYSTEM dxgi.dll from System32, NOT our proxy
    wchar_t sys_path[MAX_PATH];
    GetSystemDirectoryW(sys_path, MAX_PATH);
    wcscat_s(sys_path, L"\\dxgi.dll");
    
    HMODULE dxgi = LoadLibraryExW(sys_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dxgi) {
        SNG_LOG("[DXGI] Failed to load system dxgi.dll from %ls", sys_path);
        return false;
    }
    
    SNG_LOG("[DXGI] Loaded system dxgi.dll from %ls", sys_path);
    
    g_orig_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
    g_orig_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(dxgi, "CreateDXGIFactory2");
    g_orig_CreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(dxgi, "CreateDXGIFactory");
    
    if (g_orig_CreateDXGIFactory1) {
        MH_CreateHook(g_orig_CreateDXGIFactory1, Hook_CreateDXGIFactory1, (void**)&g_orig_CreateDXGIFactory1);
        MH_EnableHook(g_orig_CreateDXGIFactory1);
    }
    if (g_orig_CreateDXGIFactory2) {
        MH_CreateHook(g_orig_CreateDXGIFactory2, Hook_CreateDXGIFactory2, (void**)&g_orig_CreateDXGIFactory2);
        MH_EnableHook(g_orig_CreateDXGIFactory2);
    }
    if (g_orig_CreateDXGIFactory) {
        MH_CreateHook(g_orig_CreateDXGIFactory, Hook_CreateDXGIFactory, (void**)&g_orig_CreateDXGIFactory);
        MH_EnableHook(g_orig_CreateDXGIFactory);
    }
    
    SNG_LOG("[DXGI] DXGI hooks installed");
    return true;
}

void UninstallDXGIHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    
    std::lock_guard<std::mutex> lock(g_factory_hook_mutex);
    for (auto* factory : g_hooked_factories) {
        factory->Release();
    }
    g_hooked_factories.clear();
    
    SNG_LOG("[DXGI] DXGI hooks uninstalled");
}

// ============================================================================
// Logging implementation - simple, robust
// ============================================================================

static FILE* g_log_file = nullptr;
static std::mutex g_log_mutex;
static bool g_log_initialized = false;
static wchar_t g_dll_path[MAX_PATH] = {};

void InitializeLogFile() {
    if (g_log_initialized) return;
    g_log_initialized = true;
    
    // Use our DLL's path (set by DllMain)
    wchar_t log_path[MAX_PATH];
    if (g_dll_path[0]) {
        wcscpy_s(log_path, g_dll_path);
        wchar_t* slash = wcsrchr(log_path, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat_s(log_path, L"ScaleNG.log");
        }
    } else {
        // Fallback: use game directory
        GetModuleFileNameW(nullptr, log_path, MAX_PATH);
        wchar_t* slash = wcsrchr(log_path, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat_s(log_path, L"ScaleNG.log");
        } else {
            wcscpy_s(log_path, L"C:\\ScaleNG.log");
        }
    }
    
    g_log_file = _wfopen(log_path, L"a");
    if (g_log_file) {
        // Write a startup marker
        SYSTEMTIME st;
        GetLocalTime(&st);
        char marker[256];
        sprintf_s(marker, "\n\n=== ScaleNG.Drive Started: %02d:%02d:%02d.%03d ===\n", 
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        fwrite(marker, 1, strlen(marker), g_log_file);
        fflush(g_log_file);
    }
}

void SetDllPath(HMODULE hModule) {
    GetModuleFileNameW(hModule, g_dll_path, MAX_PATH);
}

void LogMessage(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    
    if (!g_log_initialized) {
        InitializeLogFile();
    }
    
    char buffer[2048];
    va_list args;
    va_start(args, format);
    int len = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);
    
    if (len < 0) return;
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[64];
    sprintf_s(timestamp, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    
    char output[2112];
    sprintf_s(output, "%s%s\n", timestamp, buffer);
    
    OutputDebugStringA(output);
    
    if (g_log_file) {
        fwrite(output, 1, strlen(output), g_log_file);
        fflush(g_log_file);
    }
}

void SetConfig(const Config& cfg) {
    SNG_LOCK;
    SNG_STATE.config = cfg;
}

Config GetConfig() {
    SNG_LOCK;
    return SNG_STATE.config;
}

} // namespace ScaleNG