// ScaleNG.Drive - DLSS/DLAA Upscaler for BeamNG.drive
// DXGI Proxy Pattern (ReShade-style)
// Built as dxgi.dll, placed in game Bin64 folder

#include "dxgi_hooks.h"
#include "resource_tracker.h"
#include "ngx_evaluator.h"
#include "present_evaluator.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <filesystem>

namespace ScaleNG {

// Global initialization state
static bool g_initialized = false;
static bool g_dll_attached = false;

// Configuration loading
void LoadConfig() {
    wchar_t module_path[MAX_PATH];
    GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    
    wchar_t config_path[MAX_PATH];
    wcscpy_s(config_path, module_path);
    wchar_t* slash = wcsrchr(config_path, L'\\');
    if (slash) {
        *(slash + 1) = L'\0';
        wcscat_s(config_path, L"dxgi.ini");
    } else {
        wcscpy_s(config_path, L"dxgi.ini");
    }
    
    SNG_LOG("[Config] Loading from: %ls", config_path);
    
    Config cfg = {};
    cfg.enabled = true;
    cfg.dlaa = true;
    cfg.render_scale = 0.67f;
    cfg.sharpness = 0.0f;
    cfg.perf_quality = 1;
    cfg.mv_jittered = true;
    cfg.auto_exposure = true;
    cfg.app_id = 0xE658700;
    
    wcscpy_s(cfg.log_path, module_path);
    slash = wcsrchr(cfg.log_path, L'\\');
    if (slash) {
        *(slash + 1) = L'\0';
        wcscat_s(cfg.log_path, L"ScaleNG.log");
    }
    
    wcscpy_s(cfg.dlss_dll_path, module_path);
    slash = wcsrchr(cfg.dlss_dll_path, L'\\');
    if (slash) {
        *(slash + 1) = L'\0';
        wcscat_s(cfg.dlss_dll_path, L"nvngx_dlss.dll");
    }

    if (PathFileExistsW(config_path)) {
        cfg.enabled = GetPrivateProfileIntW(L"ScaleNG", L"enabled", 1, config_path) != 0;
        cfg.dlaa = GetPrivateProfileIntW(L"ScaleNG", L"dlaa", 1, config_path) != 0;
        cfg.render_scale = (float)GetPrivateProfileIntW(L"ScaleNG", L"render_scale", 67, config_path) / 100.0f;
        cfg.sharpness = (float)GetPrivateProfileIntW(L"ScaleNG", L"sharpness", 0, config_path) / 100.0f;
        cfg.perf_quality = GetPrivateProfileIntW(L"ScaleNG", L"perf_quality", 1, config_path);
        cfg.mv_jittered = GetPrivateProfileIntW(L"ScaleNG", L"mv_jittered", 1, config_path) != 0;
        cfg.auto_exposure = GetPrivateProfileIntW(L"ScaleNG", L"auto_exposure", 1, config_path) != 0;
        cfg.app_id = GetPrivateProfileIntW(L"ScaleNG", L"app_id", 0xE658700, config_path);
        
        wchar_t dll_path[MAX_PATH];
        GetPrivateProfileStringW(L"ScaleNG", L"dlss_dll_path", L"", dll_path, MAX_PATH, config_path);
        if (dll_path[0]) wcscpy_s(cfg.dlss_dll_path, dll_path);
        
        wchar_t log_path[MAX_PATH];
        GetPrivateProfileStringW(L"ScaleNG", L"log_path", L"", log_path, MAX_PATH, config_path);
        if (log_path[0]) wcscpy_s(cfg.log_path, log_path);
    }
    
    SNG_STATE.config = cfg;
    
    SNG_LOG("[Config] enabled=%d, dlaa=%d, render_scale=%.2f, sharpness=%.2f, quality=%d, app_id=0x%X",
        cfg.enabled, cfg.dlaa, cfg.render_scale, cfg.sharpness,
        cfg.perf_quality, cfg.app_id);
    SNG_LOG("[Config] dlss_dll=%ls", cfg.dlss_dll_path);
    SNG_LOG("[Config] log_path=%ls", cfg.log_path);
}

// Main initialization - called when swapchain+device+queue are ready
void OnInitEffectRuntime(IDXGISwapChain3* swapchain, ID3D12Device* device, ID3D12CommandQueue* queue) {
    SNG_LOG("[Main] InitEffectRuntime: swapchain=%p, device=%p, queue=%p", swapchain, device, queue);
    
    if (!SNG_STATE.config.enabled) {
        SNG_LOG("[Main] ScaleNG disabled in config");
        return;
    }
    
    // Initialize resource tracker
    ResourceTracker::Get().Initialize();
    
    // Initialize present evaluator (which initializes NGX)
    if (!PresentEvaluator::Get().Initialize(device, queue, swapchain)) {
        SNG_LOG("[Main] Present evaluator initialization failed");
        return;
    }
    
    g_initialized = true;
    SNG_LOG("[Main] ScaleNG fully initialized and ready");
}

// Present callback
void OnPresent(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags) {
    if (!g_initialized) return;
    PresentEvaluator::Get().OnPresent(swapchain, sync_interval, flags);
}

// Resize callback
void OnResize(IDXGISwapChain3* swapchain, bool resize) {
    if (!g_initialized) return;
    if (resize) {
        PresentEvaluator::Get().OnResize(swapchain);
    }
}

// DllMain
BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hModule);
            g_dll_attached = true;
            
            LoadConfig();
            
            if (!SNG_STATE.config.enabled) {
                SNG_LOG("[Main] ScaleNG disabled, skipping hook installation");
                return TRUE;
            }
            
            SNG_LOG("[Main] ScaleNG.Drive DXGI Proxy starting...");
            SNG_LOG("[Main] Module: %p", hModule);
            
            // Set up DXGI hook callbacks
            g_on_init_effect_runtime = [](const void* data) {
                auto* evt = static_cast<const InitEffectRuntimeEvent*>(data);
                OnInitEffectRuntime(evt->swapchain, evt->device, evt->queue);
                return false;
            };
            
            g_on_present = [](const void* data) {
                auto* evt = static_cast<const PresentEvent*>(data);
                OnPresent(evt->swapchain, evt->sync_interval, evt->flags);
                return false;
            };
            
            g_on_destroy_swapchain = [](const void* data) {
                auto* evt = static_cast<const DestroySwapchainEvent*>(data);
                OnResize(evt->swapchain, evt->resize);
                return false;
            };
            
            // Install DXGI hooks
            if (!InstallDXGIHooks()) {
                SNG_LOG("[Main] Failed to install DXGI hooks");
                return FALSE;
            }
            
            SNG_LOG("[Main] ScaleNG.Drive DXGI Proxy initialized successfully");
            break;
        }
        case DLL_PROCESS_DETACH: {
            if (!g_dll_attached) break;
            
            SNG_LOG("[Main] ScaleNG.Drive shutting down...");
            
            PresentEvaluator::Get().Shutdown();
            
            UninstallDXGIHooks();
            
            EventSystem::Get().Clear();
            
            g_initialized = false;
            g_dll_attached = false;
            
            SNG_LOG("[Main] ScaleNG.Drive shutdown complete");
            break;
        }
    }
    return TRUE;
}

} // namespace ScaleNG

// ============================================================================
// DXGI Proxy Exports - These are the functions Windows will call instead of system DXGI
// ============================================================================

extern "C" {

// CreateDXGIFactory
HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    return ScaleNG::Hook_CreateDXGIFactory(riid, ppFactory);
}

// CreateDXGIFactory1
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    return ScaleNG::Hook_CreateDXGIFactory1(riid, ppFactory);
}

// Note: We don't export CreateDXGIFactory2 to avoid conflict with system dxgi.dll

} // extern "C"