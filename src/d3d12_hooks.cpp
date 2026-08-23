#define NOMINMAX
#include "d3d12_hooks.h"
#include "log.h"
#include "camera_cb.h"
#include "dlss_ngx.h"

#include <dxgi1_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <MinHook.h>

extern "C" WINBASEAPI DWORD WINAPI K32GetModuleBaseNameW(HANDLE, HMODULE, LPWSTR, DWORD);
#include <map>
#include <vector>
#include <cstring>

#include <cmath>

PFN_ScaleNG_CreateDevice Real_D3D12CreateDevice_Tramp = nullptr;

void EnsureUpscalerInit();
static bool s_creatingBridge = false;   // true while EnsureBridge creates its device


static unsigned F2U(float v)
{
    unsigned u = 0;
    std::memcpy(&u, &v, 4);
    return u;
}

namespace {

constexpr size_t kCameraCbSize = 1616;
constexpr size_t kVelocityCbSize = 176;

ScaleNgConfig g_cfg;
bool g_cfgSet = false;

ID3D12Device* g_device = nullptr;
ID3D12Resource* g_cameraRing = nullptr;

ID3D12Resource* g_sceneColor = nullptr;
bool g_sceneColorValid = false;
D3D12_CPU_DESCRIPTOR_HANDLE g_sceneColorRtv = {};
ID3D12Resource* g_sceneColorAlt = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_sceneColorRtvAlt = {};
unsigned int g_displayW = 0;
unsigned int g_displayH = 0;

ID3D12Resource* g_mvResource = nullptr;
bool g_mvValid = false;
ID3D12Resource* g_mvResourceAlt = nullptr;
unsigned int g_mvW = 0;
unsigned int g_mvH = 0;

ID3D12Resource* g_depthResource = nullptr;
bool g_depthValid = false;

// Frame stamps: when depth/MV were last (re)discovered. Feeding NGX a freed
// resource = InvalidParameter storms + driver instability, so injection
// requires fresh discoveries only.
unsigned int g_depthStamp = 0;
unsigned int g_mvStamp = 0;
unsigned int g_evalFailStreak = 0;
bool g_dlaaHalted = false;
// Frame stamp of the last successful camera-CB patch: our "gameplay is
// actually rendering" signal. Loading screens/menus don't patch camera CBs,
// so we suppress ALL present-time activity there.
// ---------------------------------------------------------------------------
// Cross-device NGX bridge: NGX runs on OUR clean device (the game's wrapped
// device lacks IDXGIDevice and crashes NVIDIA's driver during evaluate).
// Per frame: game queue copies scene/depth/mv into SHARED textures, our
// device evaluates DLSS, game queue copies the result back. All sync via a
// single shared fence.
// ---------------------------------------------------------------------------
ID3D12Device* g_bridgeDev = nullptr;
ID3D12CommandQueue* g_bridgeQueue = nullptr;
ID3D12GraphicsCommandList* g_bridgeList = nullptr;
ID3D12CommandAllocator* g_bridgeAlloc = nullptr;
ID3D12Fence* g_bridgeFence = nullptr;
HANDLE g_bridgeFenceEv = nullptr;
HANDLE g_bridgeFenceShared = nullptr;
UINT64 g_bridgeVal = 0;
UINT64 g_bridgeLastSubmit = 0;
ID3D12Resource* g_brColor = nullptr;  HANDLE g_hColor = nullptr;
ID3D12Resource* g_brDepth = nullptr;  HANDLE g_hDepth = nullptr;
ID3D12Resource* g_brMv = nullptr;     HANDLE g_hMv = nullptr;
ID3D12Resource* g_brOut = nullptr;    HANDLE g_hOut = nullptr;
ID3D12Resource* g_gameColor = nullptr;
ID3D12Resource* g_gameDepth = nullptr;
ID3D12Resource* g_gameMv = nullptr;
ID3D12Resource* g_gameOut = nullptr;
bool g_passiveMode = false;
bool g_bridgeReady = false;
bool g_upscalerInitAttempted = false;
IUpscaler* g_upscaler = nullptr;
bool g_evalDidBridge = false;
static unsigned g_brW = 0, g_brH = 0;
static DXGI_FORMAT g_brFmt = DXGI_FORMAT_UNKNOWN;
unsigned int g_lastCamPatchFrame = 0;

// Create/refresh the cross-device bridge for the given size+format.
static bool EnsureBridge(unsigned W, unsigned H, DXGI_FORMAT fmt, ID3D12Device* gameDev)
{
    
    if (g_bridgeReady && g_brW == W && g_brH == H && g_brFmt == fmt)
        return true;

    // Tear down previous
    g_bridgeReady = false;
    for (auto** p : { &g_gameColor, &g_gameDepth, &g_gameMv, &g_gameOut })
        if (*p) { (*p)->Release(); *p = nullptr; }
    for (auto** p : { &g_brColor, &g_brDepth, &g_brMv, &g_brOut })
        if (*p) { (*p)->Release(); *p = nullptr; }
    for (auto** h : { &g_hColor, &g_hDepth, &g_hMv, &g_hOut })
        if (*h) { CloseHandle(*h); *h = nullptr; }

    if (!g_bridgeDev) {
        typedef HRESULT(WINAPI* PFN_MkDev)(void*, unsigned, const IID&, void**);
        PFN_MkDev mk = (PFN_MkDev)GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12CreateDevice");
        s_creatingBridge = true;   // keep Hook_D3D12CreateDevice from hijacking g_device
        // Enumerate adapters to find NVIDIA (hybrid laptops have AMD iGPU)
        IDXGIFactory4* brFactory = nullptr;
        typedef HRESULT(WINAPI* PFN_CreateDXGI)(const IID&, void**);
        PFN_CreateDXGI mkFactory = (PFN_CreateDXGI)(void*)GetProcAddress(GetModuleHandleA("dxgi.dll"), "CreateDXGIFactory1");
        if (mkFactory) mkFactory(__uuidof(IDXGIFactory4), (void**)&brFactory);
        IDXGIAdapter1* brAdapter = nullptr;
        if (brFactory) {
            for (UINT i = 0; brFactory->EnumAdapters1(i, &brAdapter) == S_OK; ++i) {
                DXGI_ADAPTER_DESC1 d; brAdapter->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId == 0x10DE) break;
                brAdapter = nullptr;
            }
        }
        HRESULT bdevHr;
        if (!mk || FAILED(bdevHr = mk(brAdapter, 0xb000, __uuidof(ID3D12Device), (void**)&g_bridgeDev))) {
            s_creatingBridge = false;
            Log("bridge: clean device create failed");
            return false;
        }
        s_creatingBridge = false;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_bridgeDev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_bridgeQueue));
        g_bridgeDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_bridgeAlloc));
        g_bridgeDev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bridgeAlloc, nullptr, IID_PPV_ARGS(&g_bridgeList));
        g_bridgeList->Close();
        g_bridgeDev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_bridgeFence));
                {
            typedef HRESULT(STDMETHODCALLTYPE* PFN_FenceSH)(ID3D12Fence*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*);
            PFN_FenceSH fsh = (PFN_FenceSH)(void*)GetProcAddress(GetModuleHandleA("d3d12.dll"), "CreateSharedHandle");
            HANDLE hf = nullptr;
            if (fsh && SUCCEEDED(fsh(g_bridgeFence, nullptr, GENERIC_ALL, nullptr, &hf))) {
                g_bridgeFenceShared = hf;
            }
        }
        g_bridgeFenceEv = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        Log("bridge: our device/queue/fence ready");
    }

    auto mkShared = [&](ID3D12Resource** ours, HANDLE* hout, ID3D12Resource** theirs,
                        UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl) -> bool {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = f; d.SampleDesc.Count = 1;
        d.Flags = fl | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g_bridgeDev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(ours))))
            return false;
        if (FAILED(g_bridgeDev->CreateSharedHandle(*ours, nullptr, GENERIC_ALL, nullptr, hout)))
            return false;
        return SUCCEEDED(gameDev->OpenSharedHandle(*hout, IID_PPV_ARGS(theirs)));
    };

    bool ok = true;
    ok &= mkShared(&g_brColor, &g_hColor, &g_gameColor, W, H, fmt, D3D12_RESOURCE_FLAG_NONE);
    ok &= mkShared(&g_brDepth, &g_hDepth, &g_gameDepth, W, H, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    ok &= mkShared(&g_brMv, &g_hMv, &g_gameMv, W, H, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    ok &= mkShared(&g_brOut, &g_hOut, &g_gameOut, W, H, fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!ok) { Log("bridge: shared resource creation failed"); return false; }

    g_brW = W; g_brH = H; g_brFmt = fmt;
    g_bridgeReady = true;
    // Re-arm NGX init: it may have bound the wrapped game device before the
    // bridge existed. Next EnsureUpscalerInit re-runs on OUR clean device.
    g_upscalerInitAttempted = false;
    Log("bridge: ready %ux%u fmt=%d (NGX re-bind armed)", W, H, (int)fmt);
    return true;
}

static ID3D12Resource* g_grave[4] = {};
static int g_graveN = 0;

// Which scene color was actually rendered this frame (set by the viewport patch).
ID3D12Resource* g_activeSceneColor = nullptr;

ID3D12Resource* g_dlssOut = nullptr;
bool g_dlssOutValid = false;

ID3D12Fence* g_gameFence = nullptr; // game-device view of bridge shared fence
unsigned int g_renderW = 0;
unsigned int g_renderH = 0;

Jitter2D g_currJitter = { 0.0f, 0.0f };
Jitter2D g_prevJitter = { 0.0f, 0.0f };
unsigned int g_frameCounter = 0;
bool g_frameStarted = false;
bool g_patchViewport = false;
bool g_patchAppliedThisFrame = false;
bool g_injectedThisFrame = false;
// Patching self-limits: if the engine never produces a scene copy (e.g. the game
// is backgrounded and only renders the menu), patching the viewport to the render
// size forever leaves the frame stretched/black and has been observed alongside
// GPU-driver faults. Abort after a budget of patched frames with no injection, and
// re-arm when a scene copy appears again or a new display size is adopted.
bool g_patchAborted = false;
unsigned int g_patchFramesWithoutInject = 0;
// DLAA mode: DLSS feature created at render==display size so the display-sized
// scene/depth/MV resources we can see satisfy the driver's size validation.
// The viewport patch is not armed (nothing should shrink the render).
bool g_dlaaMode = false;
bool g_hudIniOn = true;
bool g_legacyScale = false;

// Swapchain Present-time injection (DLAA mode only): the game renders every
// frame DIRECTLY into its swapchain backbuffers (no copy chain), so the only
// reliable injection point is right before Present. We record DLSS work on our
// own command list and execute it on the game's graphics queue before the
// present is forwarded, so the backbuffer contains the DLSS result.
IDXGISwapChain* g_swapchain = nullptr;
IDXGIAdapter* g_adapter = nullptr;
ID3D12CommandQueue* g_graphicsQueue = nullptr;
ID3D12CommandAllocator* g_injAlloc = nullptr;
ID3D12GraphicsCommandList* g_injList = nullptr;
ID3D12DescriptorHeap* g_injHeap = nullptr;
ID3D12DescriptorHeap* g_injSamplerHeap = nullptr;
ID3D12Fence* g_injFence = nullptr;
HANDLE g_injEvent = nullptr;
UINT64 g_injFenceVal = 0;
bool g_injSubmitted = false;
IDXGIFactory* g_anyFactory = nullptr;

void EnsureGlobalSwapchainHook();

// ---- On-screen HUD (drawn into the backbuffer at Present) ----
bool g_showHud = true;
bool g_hudReady = false;
DXGI_FORMAT g_bbFormat = DXGI_FORMAT_UNKNOWN;
ID3D12RootSignature* g_hudRootSig = nullptr;
ID3D12PipelineState* g_hudTextPso = nullptr;
ID3D12PipelineState* g_hudSolidPso = nullptr;
ID3DBlob* g_hudVsBlob = nullptr;ID3DBlob* g_hudTextPsBlob = nullptr;
ID3DBlob* g_hudSolidPsBlob = nullptr;
ID3D12Resource* g_hudAtlas = nullptr;
ID3D12Resource* g_hudVb = nullptr;
void* g_hudVbMap = nullptr;
ID3D12DescriptorHeap* g_hudRtvHeap = nullptr;
ID3D12DescriptorHeap* g_hudSrvHeap = nullptr;
ID3D12Resource* g_hudLastBb = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_hudBbRtv = {};
long long g_hudLastTick = 0;
unsigned int g_hudFrames = 0;
unsigned int g_hudFps = 0;
unsigned int g_evalOkCount = 0;
unsigned int g_evalFailCount = 0;
DXGI_FORMAT g_dlssOutFormat = DXGI_FORMAT_R16G16B16A16_UNORM;

unsigned char g_lastPatchedCameraCb[kCameraCbSize] = {};
bool g_cameraCbValid = false;
bool g_velocityCbPatched = false;


D3D12_CPU_DESCRIPTOR_HANDLE g_boundRtv = {};
bool g_boundRtvValid = false;
ID3D12Resource* g_boundRtvResource = nullptr;

// Every RTV CPU handle we have ever seen, mapped to its resource. Lets us
// resolve "what is currently bound as an RTV" to a resource even when the
// view was created at a moment we later lost (plugin re-init, view re-creation).
std::map<SIZE_T, ID3D12Resource*> g_rtvMap;

std::map<ID3D12Resource*, D3D12_RESOURCE_STATES> g_resourceStates;

UINT g_setHeapCount = 0;
ID3D12DescriptorHeap* g_setHeaps[2] = { nullptr, nullptr };

typedef void (STDMETHODCALLTYPE* PFN_CreateRenderTargetView)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void (STDMETHODCALLTYPE* PFN_CreateShaderResourceView)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void (STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef void (STDMETHODCALLTYPE* PFN_CopyBufferRegion)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
typedef void (STDMETHODCALLTYPE* PFN_CopyTextureRegion)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
typedef void (STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
typedef void (STDMETHODCALLTYPE* PFN_RSSetScissorRects)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
typedef void (STDMETHODCALLTYPE* PFN_ResourceBarrier)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
typedef void (STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);

PFN_CreateRenderTargetView Real_CreateRenderTargetView = nullptr;
PFN_CreateShaderResourceView Real_CreateShaderResourceView = nullptr;
PFN_ExecuteCommandLists Real_ExecuteCommandLists = nullptr;
PFN_CopyBufferRegion Real_CopyBufferRegion = nullptr;
PFN_CopyTextureRegion Real_CopyTextureRegion = nullptr;
PFN_RSSetViewports Real_RSSetViewports = nullptr;
PFN_RSSetScissorRects Real_RSSetScissorRects = nullptr;
PFN_ResourceBarrier Real_ResourceBarrier = nullptr;
PFN_OMSetRenderTargets Real_OMSetRenderTargets = nullptr;



struct CfgCallTargetInfo { ULONG_PTR Offset; ULONG_PTR Flags; };

// Register our hook entry points as valid Control Flow Guard call targets.
// Without this, a CFG-enabled game fast-fails (0xC0000409) on the first
// indirect call through a hooked vtable slot.
void CfgMarkValid(void* const* targets, size_t count)
{
    typedef BOOL (WINAPI* PFN_SetProcessValidCallTargets)(HANDLE, PVOID, SIZE_T, ULONG, CfgCallTargetInfo*);
    static PFN_SetProcessValidCallTargets pfn =
        (PFN_SetProcessValidCallTargets)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                              "SetProcessValidCallTargets");
    if (!pfn || !targets || count == 0) return;
    if (count > 16) count = 16;
    CfgCallTargetInfo info[16] = {};
    ULONG_PTR base = (ULONG_PTR)targets[0];
    ULONG_PTR maxOff = 0;
    for (size_t i = 0; i < count; ++i) {
        info[i].Offset = (ULONG_PTR)targets[i] - base;
        if (info[i].Offset > maxOff) maxOff = info[i].Offset;
    }
    pfn(GetCurrentProcess(), (PVOID)base, (SIZE_T)(maxOff + 1), (ULONG)count, info);
}

void InstallCommandListHooks(ID3D12GraphicsCommandList* list);

void StartFrame()
{
    if (g_frameStarted) return;
    g_frameStarted = true;
    g_patchAppliedThisFrame = false;
    g_velocityCbPatched = false;
    g_injectedThisFrame = false;

    ++g_frameCounter;
    g_prevJitter = g_currJitter;
    g_currJitter = ComputeJitter(g_frameCounter);

    if (g_dlaaMode) {
        g_renderW = g_displayW;
        g_renderH = g_displayH;
    } else if (g_displayW > 0 && g_cfg.renderScale > 0.0f && g_cfg.renderScale < 1.0f) {
        g_renderW = (unsigned int)((float)g_displayW * g_cfg.renderScale);
        g_renderH = (unsigned int)((float)g_displayH * g_cfg.renderScale);
    }

    g_patchViewport = !g_patchAborted && !g_dlaaMode && g_legacyScale;
    static int s_frameLogs = 0;
    ++s_frameLogs;
    if (s_frameLogs <= 20 || (s_frameLogs % 5000) == 0)
        Log("hooks: frame %u started (render %ux%u, jitter %.2f/%.2f)",
            g_frameCounter, g_renderW, g_renderH, g_currJitter.x, g_currJitter.y);
}

// Returns the scene color resource currently bound as an RTV, or nullptr.
ID3D12Resource* SceneColorBound()
{
    if (!g_sceneColorValid) return nullptr;
    if (g_boundRtvResource) {
        if (g_boundRtvResource == g_sceneColor) return g_sceneColor;
        if (g_sceneColorAlt && g_boundRtvResource == g_sceneColorAlt) return g_sceneColorAlt;
    }
    if (!g_boundRtvValid) return nullptr;
    if (g_boundRtv.ptr == g_sceneColorRtv.ptr) return g_sceneColor;
    if (g_sceneColorAlt && g_boundRtv.ptr == g_sceneColorRtvAlt.ptr) return g_sceneColorAlt;
    return nullptr;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES after)
{
    if (!list || !res) return;
    auto it = g_resourceStates.find(res);
    if (it == g_resourceStates.end()) {
        Log("hooks: skip barrier for untracked resource %p", (void*)res);
        return;
    }
    D3D12_RESOURCE_STATES before = it->second;
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Real_ResourceBarrier(list, 1, &b);
    it->second = after;
    Log("hooks: barrier %p %u -> %u", (void*)res, (unsigned int)before, (unsigned int)after);
}

void CreateDlssOut()
{
    if (!g_device || g_displayW == 0 || g_displayH == 0) return;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = g_displayW;
    rd.Height = g_displayH;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = g_dlssOutFormat;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_dlssOut)))) {
        Log("hooks: dlssOut allocation failed");
        return;
    }
    g_dlssOutValid = true;
    g_resourceStates[g_dlssOut] = D3D12_RESOURCE_STATE_COMMON;
    Log("hooks: dlssOut %p allocated (%ux%u)", (void*)g_dlssOut, g_displayW, g_displayH);
}

// The engine renders at whatever its window/render-target resolution is; we
// cannot hardcode 1920x992. Adopt a display size from a trusted source (scene
// RTV creation, scene-slot bind, or the scene-bound viewport), recompute the
// render size, invalidate the DLSS output, and re-configure the upscaler.
void AdoptDisplaySize(unsigned int w, unsigned int h)
{
    if (w == 0 || h == 0) return;

    // Proper hysteresis: track a candidate separately from accepted globals.
    // Only commit after 15 consecutive identical observations. This prevents
    // loading-screen RTVs (1902x954 etc.) from thrashing the DLSS feature.
    static unsigned int candW = 0, candH = 0;
    static int candStable = 0;
    if (w == candW && h == candH) {
        if (candStable < 1000) ++candStable;
    } else {
        candW = w; candH = h; candStable = 1;
        return; // new candidate: wait for confirmation next call
    }
    // Candidate confirmed stable — commit only if different from current.
    if (w == g_displayW && h == g_displayH)
        return;

    g_displayW = w;
    g_displayH = h;
    if (g_dlaaMode) {
        g_renderW = w;
        g_renderH = h;
    } else if (g_cfg.renderScale > 0.0f && g_cfg.renderScale < 1.0f) {
        g_renderW = (unsigned int)((float)g_displayW * g_cfg.renderScale);
        g_renderH = (unsigned int)((float)g_displayH * g_cfg.renderScale);
    }
    g_patchAborted = false;
    g_patchFramesWithoutInject = 0;
    if (g_dlssOutValid && g_dlssOut) {
        // Park in the graveyard — released after GPU drain inside ECL flush.
        if (g_graveN < 4) {
            g_grave[g_graveN++] = g_dlssOut;
        } else {
            g_dlssOut->Release();
        }
        g_dlssOut = nullptr;
        g_dlssOutValid = false;
    }
    if (g_upscaler)
        g_upscaler->UpdateSizes(g_renderW, g_renderH, g_displayW, g_displayH);
    Log("hooks: display size adopted %ux%u (render %ux%u)", g_displayW, g_displayH,
        g_renderW, g_renderH);
}

void EnsureUpscalerInit()
{
    if (g_upscalerInitAttempted) return;
    // Wait for the bridge device - initializing NGX on the game's wrapper
    // device crashes the driver. If bridge isn't ready yet, don't mark
    // attempted; a later call will retry once it exists.
    extern ID3D12Device* g_bridgeDev;
    if (!g_bridgeDev) return;
    g_upscalerInitAttempted = true;
    if (!g_upscaler) g_upscaler = CreateUpscaler(UPSCALER_DLSS);
    if (!g_upscaler) {
        Log("hooks: upscaler creation failed");
        return;
    }
    UpscalerInitParams ip = {};
    ip.device = g_bridgeDev;
    ip.renderWidth = g_renderW;
    ip.renderHeight = g_renderH;
    ip.displayWidth = g_displayW;
    ip.displayHeight = g_displayH;
    ip.dlssDllPath = g_cfg.dlssDllPath;
    ip.appId = g_cfg.appId;
    ip.perfQuality = g_cfg.perfQuality;
    ip.mvJittered = g_cfg.mvJittered;
    ip.autoExposure = g_cfg.autoExposure;
    if (!g_upscaler->Init(ip)) {
        Log("hooks: DLSS init failed - upscaling disabled");
        g_upscaler->SetEnabled(false);
    }
}

void DoInjection(ID3D12GraphicsCommandList* list)
{
    EnsureUpscalerInit();
    if (!g_upscaler || !g_upscaler->IsReady()) return;

    if (!g_dlssOutValid) CreateDlssOut();
    if (!g_dlssOutValid) return;

    ID3D12Resource* scene = g_activeSceneColor ? g_activeSceneColor : g_sceneColor;
    if (!scene) return;

    // DLSS reads scene/depth/mv as SRV and writes dlssOut as UAV.
    Barrier(list, g_mvResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, g_depthResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, g_dlssOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    UpscalerEvaluateParams ep = {};
    ep.commandList = list;
    ep.color = scene;
    ep.depth = g_depthResource;
    ep.motionVectors = g_mvResource;
    ep.output = g_dlssOut;
    ep.jitterX = g_currJitter.x;
    ep.jitterY = g_currJitter.y;
    ep.mvScaleX = (float)g_mvW;
    ep.mvScaleY = (float)g_mvH;
    ep.sharpness = g_cfg.sharpness;

    bool ok = g_upscaler->Evaluate(ep);
    if (!ok) {
        // Do NOT copy dlssOut (never written / stale) into the scene - that
        // corrupts the frame and has caused GPU faults. Restore states and let
        // the engine composite the low-res render as-is; retry next frame.
        static int s_evalFailLogs = 0;
        ++s_evalFailLogs;
        if (s_evalFailLogs <= 10 || (s_evalFailLogs % 500) == 0)
            Log("hooks: DLSS evaluate failed - injection skipped for frame %u", g_frameCounter);
        if (s_evalFailLogs <= 3)
            Log("hooks: eval inputs scene=%p depth=%p mv=%p out=%p jitter=%.3f/%.3f mvScale=%.0fx%.0f sharp=%.2f render=%ux%u display=%ux%u",
                (void*)scene, (void*)g_depthResource, (void*)g_mvResource, (void*)g_dlssOut,
                ep.jitterX, ep.jitterY, ep.mvScaleX, ep.mvScaleY, ep.sharpness,
                g_renderW, g_renderH, g_displayW, g_displayH);
        Barrier(list, scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, g_dlssOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(list, g_mvResource, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(list, g_depthResource, D3D12_RESOURCE_STATE_COPY_DEST);
        return;
    }

    // Write the upscaled result into the scene target in place: scene becomes
    // COPY_DEST, dlssOut becomes COPY_SOURCE for the copy.
    Barrier(list, scene, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, g_dlssOut, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = scene;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = g_dlssOut;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    Real_CopyTextureRegion(list, &dst, 0, 0, 0, &src, 0);

    Barrier(list, scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, g_dlssOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, g_mvResource, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, g_depthResource, D3D12_RESOURCE_STATE_COPY_DEST);

    g_injectedThisFrame = true;
    g_patchViewport = false;
    g_patchAborted = false;
    g_patchFramesWithoutInject = 0;
    Log("hooks: DLSS injection recorded for frame %u", g_frameCounter);
}

void Hook_CreateRenderTargetView(ID3D12Device* device, ID3D12Resource* res,
                                 const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                 D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (device == g_device && res && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1 &&
            desc->Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
            // Display-sized UNORM color target - adopt as scene color. The size
            // is NOT hardcoded (the engine may render at e.g. 1920x1001).
            g_resourceStates[res] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_rtvMap[handle.ptr] = res;
            if (!g_sceneColorValid) {
                g_sceneColor = res;
                g_sceneColorRtv = handle;
                g_sceneColorValid = true;
                AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
                Log("hooks: scene color RTV %p (%ux%u R16G16B16A16_UNORM)", (void*)res,
                    (unsigned int)rd.Width, (unsigned int)rd.Height);
            } else if (res == g_sceneColor) {
                // The game re-created the RTV view for the same resource
                // (e.g. renderer re-init). Refresh the stored handle.
                g_sceneColorRtv = handle;
                Log("hooks: scene color RTV handle refreshed %p", (void*)res);
            } else if (res != g_sceneColorAlt) {
                g_sceneColorAlt = res;
                g_sceneColorRtvAlt = handle;
                Log("hooks: scene color RTV %p (%ux%u R16G16B16A16_UNORM) (ALT)", (void*)res,
                    (unsigned int)rd.Width, (unsigned int)rd.Height);
            }
    } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1) {
            if (desc->Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
                g_displayW = (unsigned int)rd.Width;
                g_displayH = (unsigned int)rd.Height;
                g_resourceStates[res] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                g_rtvMap[handle.ptr] = res;
                if (!g_sceneColorValid) {
                    g_sceneColor = res;
                    g_sceneColorRtv = handle;
                    g_sceneColorValid = true;
                    Log("hooks: scene color RTV %p (1920x992 R16G16B16A16_UNORM)", (void*)res);
                } else if (res == g_sceneColor) {
                    // The game re-created the RTV view for the same resource
                    // (e.g. renderer re-init). Refresh the stored handle.
                    g_sceneColorRtv = handle;
                    Log("hooks: scene color RTV handle refreshed %p", (void*)res);
                } else if (res != g_sceneColorAlt) {
                    g_sceneColorAlt = res;
                    g_sceneColorRtvAlt = handle;
                    Log("hooks: scene color RTV %p (1920x992 R16G16B16A16_UNORM) (ALT)", (void*)res);
                }
            } else if (desc->Format == DXGI_FORMAT_R16G16_FLOAT) {
                g_mvW = (unsigned int)rd.Width;
                g_mvH = (unsigned int)rd.Height;
                g_resourceStates[res] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                g_rtvMap[handle.ptr] = res;
                if (!g_mvValid) {
                    g_mvResource = res;
                    g_mvValid = true;
                    g_mvStamp = g_frameCounter;
                    Log("hooks: motion vector RTV %p (%ux%u R16G16_FLOAT)", (void*)res, g_mvW, g_mvH);
                } else if (res == g_mvResource || res == g_mvResourceAlt) {
                    if (res == g_mvResource) {
                        g_mvResource = res;
                        Log("hooks: motion vector RTV handle refreshed %p", (void*)res);
                    }
                } else if (res != g_mvResourceAlt) {
                    g_mvResourceAlt = res;
                    Log("hooks: motion vector RTV %p (%ux%u R16G16_FLOAT) (ALT)", (void*)res, g_mvW, g_mvH);
                }
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1 &&
                   desc->Format == DXGI_FORMAT_R16G16_FLOAT) {
            g_resourceStates[res] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_rtvMap[handle.ptr] = res;
            if (!g_mvValid) {
                g_mvResource = res;
                g_mvValid = true;
                g_mvStamp = g_frameCounter;
                g_mvW = (unsigned int)rd.Width;
                g_mvH = (unsigned int)rd.Height;
                Log("hooks: motion vector RTV %p (1920x1001 R16G16_FLOAT)", (void*)res);
            } else if (res == g_mvResource || res == g_mvResourceAlt) {
                if (res == g_mvResource)
                    Log("hooks: motion vector RTV handle refreshed %p", (void*)res);
            } else if (res != g_mvResourceAlt) {
                g_mvResourceAlt = res;
                g_mvW = (unsigned int)rd.Width;
                g_mvH = (unsigned int)rd.Height;
                Log("hooks: motion vector RTV %p (1920x1001 R16G16_FLOAT) (ALT)", (void*)res);
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   desc->Format == DXGI_FORMAT_R16G16_FLOAT && rd.MipLevels == 1 &&
                   rd.Width > 0 && rd.Height > 0) {
            g_rtvMap[handle.ptr] = res;
            static int s_otherMvRtvs = 0;
            if (s_otherMvRtvs < 10) {
                ++s_otherMvRtvs;
                Log("hooks: other R16G16_FLOAT RTV %p (%ux%u)", (void*)res,
                    (unsigned int)rd.Width, (unsigned int)rd.Height);
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   desc->Format == DXGI_FORMAT_R16G16B16A16_UNORM && rd.MipLevels == 1 &&
                   rd.Width >= 1000 && rd.Height >= 500) {
            // View re-created at any time: keep the handle map fresh so
            // OMSetRenderTargets can resolve the scene color even if the
            // resource itself was discovered earlier.
            g_rtvMap[handle.ptr] = res;
            if (res == g_sceneColor) {
                g_sceneColorRtv = handle;
                Log("hooks: scene color RTV handle refreshed (map) %p", (void*)res);
            } else if (res == g_sceneColorAlt) {
                g_sceneColorRtvAlt = handle;
                Log("hooks: scene color ALT RTV handle refreshed (map) %p", (void*)res);
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   rd.MipLevels == 1 && rd.Width > 0 && rd.Height > 0) {
            g_rtvMap[handle.ptr] = res;
        }
    }
    if (Real_CreateRenderTargetView)
        Real_CreateRenderTargetView(device, res, desc, handle);
}

void Hook_CreateShaderResourceView(ID3D12Device* device, ID3D12Resource* res,
                                   const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (device == g_device && res && desc && desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
        desc->Texture2D.MipLevels == 1) {
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.MipLevels == 1 &&
            rd.Width == g_displayW && rd.Height == g_displayH &&
            (desc->Format == DXGI_FORMAT_R32_FLOAT || desc->Format == DXGI_FORMAT_R32_TYPELESS ||
             desc->Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || desc->Format == DXGI_FORMAT_D32_FLOAT)) {
            g_depthResource = res;
            g_depthValid = true;
            g_depthStamp = g_frameCounter;
            g_resourceStates[res] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Log("hooks: depth candidate SRV %p", (void*)res);
        }
    }
    if (Real_CreateShaderResourceView)
        Real_CreateShaderResourceView(device, res, desc, handle);
}

void TryDeferredInject(ID3D12CommandQueue* injQueue);
void InjectAtPresentImpl(ID3D12CommandQueue* injQueue);
void InjectAtPresent();
bool g_inInject = false;

void Hook_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists,
                              ID3D12CommandList* const* lists)
{
    if (queue)
        g_graphicsQueue = queue;
    EnsureGlobalSwapchainHook();
    static int s_execCalls = 0;
    if (s_execCalls < 5) {
        ++s_execCalls;
        Log("hooks: ExecuteCommandLists #%d (queue %p, %u lists)", s_execCalls, (void*)queue, numLists);
    }
    if (g_device && lists && numLists > 0) {
        static std::vector<ID3D12GraphicsCommandList*> s_hookedLists;
        for (UINT i = 0; i < numLists; ++i) {
            ID3D12GraphicsCommandList* cl = nullptr;
            if (SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&cl))) && cl) {
                bool seen = false;
                for (auto* p : s_hookedLists)
                    if (p == cl) { seen = true; break; }
                if (!seen) {
                    s_hookedLists.push_back(cl);
                    InstallCommandListHooks(cl);
                }
                cl->Release();
            }
        }
    }
    g_frameStarted = false;
    if (Real_ExecuteCommandLists)
        Real_ExecuteCommandLists(queue, numLists, lists);

    // Deferred injection: run our DLAA+HUD work on THE GAME'S OWN QUEUE,
    // immediately after its lists - perfect ordering, zero cross-queue races.
    if (!g_inInject && queue) {
        g_inInject = true;
        TryDeferredInject(queue);
        g_inInject = false;
    }
}

// ---------------- Swapchain Present injection (DLAA mode) ----------------

typedef void (STDMETHODCALLTYPE* PFN_SetDescriptorHeaps)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
PFN_SetDescriptorHeaps Real_SetDescriptorHeaps = nullptr;

void EnsureInjectionResources()
{
    if (g_injList || !g_device) return;
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_injAlloc))))
        return;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_injAlloc, nullptr,
                                           IID_PPV_ARGS(&g_injList)))) {
        g_injAlloc = nullptr;
        return;
    }
    g_injList->Close();
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1024;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_injHeap));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = 16;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_injSamplerHeap));
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_injFence));
    g_injEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Log("hooks: present-injection resources created (list %p heap %p sampler %p)",
        (void*)g_injList, (void*)g_injHeap, (void*)g_injSamplerHeap);
}

// ---------------- On-screen HUD ----------------

// 5x7 bitmap font, rows top->bottom, bit4 = leftmost column. ASCII 32..126.
static const unsigned char s_font5x7[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, //   !
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, {0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00}, // " #
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, {0x12,0x13,0x02,0x04,0x08,0x19,0x09}, // $ %
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, {0x04,0x04,0x08,0x00,0x00,0x00,0x00}, // & '
    {0x02,0x04,0x04,0x04,0x04,0x04,0x02}, {0x08,0x04,0x04,0x04,0x04,0x04,0x08}, // ( )
    {0x00,0x0A,0x04,0x0E,0x04,0x0A,0x00}, {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // * +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x08}, {0x00,0x00,0x00,0x0E,0x00,0x00,0x00}, // , -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, {0x10,0x08,0x04,0x02,0x01,0x00,0x00}, // . /
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 0 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 2 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 4 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 6 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 8 9
    {0x00,0x00,0x0C,0x00,0x0C,0x00,0x00}, {0x00,0x00,0x0C,0x00,0x0C,0x08,0x00}, // : ;
    {0x00,0x02,0x04,0x08,0x04,0x02,0x00}, {0x00,0x00,0x0E,0x00,0x0E,0x00,0x00}, // < =
    {0x00,0x08,0x04,0x02,0x04,0x08,0x00}, {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // > ?
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // @ A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // B C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // D E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // F G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, // H I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // J K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // L M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // N O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // P Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // R S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // T U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // V W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // X Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // Z [
    {0x01,0x02,0x04,0x08,0x10,0x00,0x00}, {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // \ ]
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // ^ _
    {0x08,0x04,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, // ` a
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E}, // b c
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, // d e
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}, // f g
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, // h i
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, // j k
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x00,0x00,0x1A,0x15,0x15,0x15,0x15}, // l m
    {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, // n o
    {0x00,0x00,0x1E,0x11,0x11,0x1E,0x10}, {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01}, // p q
    {0x00,0x00,0x0E,0x11,0x10,0x10,0x10}, {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, // r s
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, // t u
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, // v w
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, // x y
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, // z {
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, // | }
    {0x00,0x08,0x15,0x02,0x00,0x00,0x00}  // ~
};

static inline unsigned int HudColor(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    return (unsigned int)r | ((unsigned int)g << 8) | ((unsigned int)b << 16) | ((unsigned int)a << 24);
}

void HudEnsurePso()
{
    if (g_hudTextPso && g_hudSolidPso) return;
    if (!g_hudRootSig || !g_hudVsBlob || !g_hudTextPsBlob || !g_hudSolidPsBlob) return;

    struct Variant { const char* name; bool noInputLayout; bool forceR8; bool noBlend; };
    static const Variant variants[] = {
        { "A-full",    false, false, false },
        { "B-fmt-r8",  false, true,  false },
        { "C-noblend", false, false, true  },
        { "D-noIA",    true,  false, false },
        { "E-min",     true,  true,  true  },
    };

    auto build = [&](const Variant& v, ID3DBlob* ps, ID3D12PipelineState** out) -> HRESULT {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_hudRootSig;
        D3D12_INPUT_ELEMENT_DESC elems[3] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        if (!v.noInputLayout) {
            pd.InputLayout.NumElements = 3;
            pd.InputLayout.pInputElementDescs = elems;
        }
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        if (!v.noBlend) {
            pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
            pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.NumRenderTargets = 1;
        DXGI_FORMAT f = v.forceR8 ? DXGI_FORMAT_R8G8B8A8_UNORM
                      : (g_bbFormat != DXGI_FORMAT_UNKNOWN ? g_bbFormat : DXGI_FORMAT_R8G8B8A8_UNORM);
        pd.RTVFormats[0] = f;
        pd.SampleDesc.Count = 1;
        pd.SampleMask = 0xFFFFFFFF;
        pd.VS = { g_hudVsBlob->GetBufferPointer(), g_hudVsBlob->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        return g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(out));
    };

    // Find the first variant the device accepts (text PS), reuse it for both.
    for (const Variant& v : variants) {
        ID3D12PipelineState* t = nullptr;
        HRESULT hr = build(v, g_hudTextPsBlob, &t);
        if (FAILED(hr)) {
            Log("hud: PSO variant %s hr=0x%08X", v.name, (unsigned)hr);
            continue;
        }
        ID3D12PipelineState* s = nullptr;
        hr = build(v, g_hudSolidPsBlob, &s);
        if (FAILED(hr)) {
            Log("hud: PSO variant %s solid hr=0x%08X", v.name, (unsigned)hr);
            t->Release();
            continue;
        }
        g_hudTextPso = t;
        g_hudSolidPso = s;
        Log("hud: PSOs built with variant %s", v.name);
        return;
    }
}

void HudInitCompile()
{
    if (g_hudReady || !g_device) return;

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!dc) {
        Log("hud: d3dcompiler_47.dll not found - overlay disabled");
        return;
    }
    typedef HRESULT (WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                             LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    PFN_D3DCompile pCompile = (PFN_D3DCompile)(void*)GetProcAddress(dc, "D3DCompile");
    if (!pCompile) {
        Log("hud: D3DCompile not found - overlay disabled");
        return;
    }

    static const char kVs[] =
        "struct Hv { float2 pos : POSITION; float2 uv : TEXCOORD; float4 color : COLOR; };\n"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 color : COLOR; };\n"
        "cbuffer HudCb : register(b0) { float2 ScreenSize; };\n"
        "PSIn main(Hv v) {\n"
        "  PSIn o;\n"
        "  float2 ndc = float2(v.pos.x / (ScreenSize.x * 0.5) - 1.0, 1.0 - v.pos.y / (ScreenSize.y * 0.5));\n"
        "  o.pos = float4(ndc, 0.0, 1.0);\n"
        "  o.uv = v.uv; o.color = v.color;\n"
        "  return o;\n"
        "}\n";
    static const char kTextPs[] =
        "Texture2D Atlas : register(t0);\n"
        "SamplerState Samp : register(s0);\n"
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 color : COLOR; };\n"
        "float4 main(PSIn i) : SV_Target {\n"
        "  float cov = Atlas.Sample(Samp, i.uv).r;\n"
        "  return float4(i.color.rgb * cov, cov);\n"
        "}\n";
    static const char kSolidPs[] =
        "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 color : COLOR; };\n"
        "float4 main(PSIn i) : SV_Target { return i.color; }\n";

    auto compile = [&](const char* src, const char* target, ID3DBlob** out) -> bool {
        ID3DBlob* err = nullptr;
        HRESULT hr = pCompile(src, strlen(src), "ScaleNG.hud", nullptr, nullptr, "main", target, 0, 0, out, &err);
        if (FAILED(hr)) {
            if (err) Log("hud: shader %s error: %.200s", target, (char*)err->GetBufferPointer());
            else Log("hud: shader %s failed hr=0x%08X", target, hr);
            return false;
        }
        return true;
    };
    if (!compile(kVs, "vs_5_0", &g_hudVsBlob)) return;
    if (!compile(kTextPs, "ps_5_0", &g_hudTextPsBlob)) return;
    if (!compile(kSolidPs, "ps_5_0", &g_hudSolidPsBlob)) return;

    HMODULE d3d12m = GetModuleHandleA("d3d12.dll");
    typedef HRESULT (WINAPI* PFN_SerializeRootSig)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                   ID3DBlob**, ID3DBlob**);
    PFN_SerializeRootSig pSerialize = d3d12m ? (PFN_SerializeRootSig)(void*)GetProcAddress(d3d12m, "D3D12SerializeRootSignature") : nullptr;
    if (!pSerialize) {
        Log("hud: D3D12SerializeRootSignature not found - overlay disabled");
        return;
    }

    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.RegisterSpace = 0;
    rp[0].Constants.Num32BitValues = 2;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // The atlas is a Texture2D - root SRV descriptors only support raw/structured
    // buffers, so the texture must come from a DESCRIPTOR TABLE instead.
    D3D12_DESCRIPTOR_RANGE atlasRange = {};
    atlasRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    atlasRange.NumDescriptors = 1;
    atlasRange.BaseShaderRegister = 0;
    atlasRange.RegisterSpace = 0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &atlasRange;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MipLODBias = 0;
    ss.MaxAnisotropy = 1;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.MinLOD = 0;
    ss.MaxLOD = 0;
    ss.ShaderRegister = 0;
    ss.RegisterSpace = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &ss;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;
    if (FAILED(pSerialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
        Log("hud: root signature serialize failed - overlay disabled");
        return;
    }
    if (FAILED(g_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                             IID_PPV_ARGS(&g_hudRootSig)))) {
        Log("hud: root signature create failed - overlay disabled");
        return;
    }

    // Font atlas: 570x8 R8, 6px cells (5 wide + 1 spacing), 8 tall (7 + 1).
    static const int kAtlasW = 570, kAtlasH = 8;
    std::vector<unsigned char> atlas((size_t)kAtlasW * kAtlasH, 0);
    for (int g = 0; g < 95; ++g)
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c)
                if (s_font5x7[g][r] & (1u << (4 - c)))
                    atlas[(size_t)r * kAtlasW + g * 6 + c] = 255;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kAtlasW;
    td.Height = kAtlasH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&g_hudAtlas)))) {
        Log("hud: atlas create failed - overlay disabled");
        return;
    }
    g_hudAtlas->WriteToSubresource(0, nullptr, atlas.data(), kAtlasW, kAtlasW);

    // Shader-visible SRV heap holding the atlas descriptor (the root signature
    // exposes the atlas via a descriptor TABLE - root descriptors can't be
    // Texture2D).
    D3D12_DESCRIPTOR_HEAP_DESC shd = {};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shd.NumDescriptors = 1;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT shr = g_device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&g_hudSrvHeap));
    if (FAILED(shr)) {
        Log("hud: SRV heap create failed hr=0x%08X - overlay disabled", (unsigned)shr);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_R8_UNORM;
    svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    svd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_hudAtlas, &svd,
                                       g_hudSrvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_DESC vbd = {};
    vbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbd.Width = 1024 * 6 * 20;  // 1024 chars max, 6 verts each, 20 bytes per vertex
    vbd.Height = 1;
    vbd.DepthOrArraySize = 1;
    vbd.MipLevels = 1;
    vbd.SampleDesc.Count = 1;
    vbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hpU = {};
    hpU.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(g_device->CreateCommittedResource(&hpU, D3D12_HEAP_FLAG_NONE, &vbd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&g_hudVb)))) {
        Log("hud: vertex buffer create failed - overlay disabled");
        return;
    }
    g_hudVb->Map(0, nullptr, &g_hudVbMap);

    D3D12_DESCRIPTOR_HEAP_DESC rhd = {};
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rhd.NumDescriptors = 16;
    HRESULT hhr = g_device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&g_hudRtvHeap));
    if (FAILED(hhr)) {
        Log("hud: RTV heap create failed hr=0x%08X - overlay disabled", (unsigned)hhr);
        return;
    }

    HudEnsurePso();
    g_hudReady = g_hudTextPso && g_hudSolidPso;
    Log("hud: overlay %s (format %d)", g_hudReady ? "ready" : "failed", (int)g_bbFormat);
}

void HudEnsureRtv(ID3D12Resource* bb)
{
    if (!g_hudRtvHeap || !bb) return;
    if (bb == g_hudLastBb) return;
    g_hudLastBb = bb;
    g_hudBbRtv = g_hudRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rvd = {};
    rvd.Format = g_bbFormat;  // UNKNOWN -> the resource's own format
    rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(bb, &rvd, g_hudBbRtv);
}

struct HudVert { float x, y, u, v; unsigned int color; };

void HudDrawQuads(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const HudVert* verts, UINT count)
{
    if (!g_hudVb || !g_hudVbMap || count == 0) return;
    memcpy(g_hudVbMap, verts, (size_t)count * sizeof(HudVert));
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = g_hudVb->GetGPUVirtualAddress();
    vbv.SizeInBytes = (UINT)((size_t)count * sizeof(HudVert));
    vbv.StrideInBytes = sizeof(HudVert);
    list->IASetVertexBuffers(0, 1, &vbv);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetGraphicsRootSignature(g_hudRootSig);
    list->SetPipelineState(pso);
    ID3D12DescriptorHeap* hudHeaps[] = { g_hudSrvHeap };
    list->SetDescriptorHeaps(1, hudHeaps);
    float screen[2] = { (float)g_displayW, (float)g_displayH };
    list->SetGraphicsRoot32BitConstants(0, 2, screen, 0);
    list->SetGraphicsRootDescriptorTable(1, g_hudSrvHeap->GetGPUDescriptorHandleForHeapStart());
    list->DrawInstanced(count, 1, 0, 0);
}

void HudDrawBar(ID3D12GraphicsCommandList* list, float x, float y, float w, float h, unsigned int color)
{
    HudVert v[6] = {
        { x, y, 0, 0, color }, { x + w, y, 0, 0, color }, { x, y + h, 0, 0, color },
        { x + w, y, 0, 0, color }, { x + w, y + h, 0, 0, color }, { x, y + h, 0, 0, color },
    };
    HudDrawQuads(list, g_hudSolidPso, v, 6);
}

void HudDrawText(ID3D12GraphicsCommandList* list, const char* text, float x, float y, unsigned int color)
{
    if (!text) return;
    size_t len = strlen(text);
    if (len == 0) return;
    static const int kAtlasW = 570;
    static const int kCellW = 6, kCellH = 8, kGlyphW = 5, kGlyphH = 7;
    HudVert q[6 * 512];
    size_t n = 0;
    float xx = x;
    for (size_t i = 0; i < len && n < 6 * 512; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c > 126) c = '?';
        int gi = c - 32;
        float u0 = (float)(gi * kCellW) / (float)kAtlasW;
        float u1 = (float)(gi * kCellW + kGlyphW) / (float)kAtlasW;
        float v0 = 0.0f;
        float v1 = (float)kGlyphH / (float)kCellH;
        HudVert* v = q + n;
        n += 6;
        v[0] = { xx, y, u0, v0, color };
        v[1] = { xx + kGlyphW, y, u1, v0, color };
        v[2] = { xx, y + kGlyphH, u0, v1, color };
        v[3] = { xx + kGlyphW, y, u1, v0, color };
        v[4] = { xx + kGlyphW, y + kGlyphH, u1, v1, color };
        v[5] = { xx, y + kGlyphH, u0, v1, color };
        xx += (float)kCellW;
    }
    HudDrawQuads(list, g_hudTextPso, q, (UINT)n);
}

void DrawHud(ID3D12GraphicsCommandList* list)
{
    if (!g_hudTextPso || !g_hudSolidPso || !g_hudBbRtv.ptr) return;

    LARGE_INTEGER now = {}, freq = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    ++g_hudFrames;
    if (g_hudFrames % 20 == 0) {
        if (g_hudLastTick) {
            double dt = (double)(now.QuadPart - g_hudLastTick) / (double)freq.QuadPart;
            if (dt > 0.0) g_hudFps = (unsigned int)(20.0 / dt + 0.5);
        }
        g_hudLastTick = now.QuadPart;
    }

    const char* st;
    unsigned int stColor;
    if (!g_dlaaMode) {
        st = "DLSS OFF";
        stColor = HudColor(255, 200, 0, 255);
    } else if (g_evalFailCount > 0 && g_evalOkCount == 0) {
        st = "DLSS FAIL";
        stColor = HudColor(255, 60, 60, 255);
    } else if (g_upscaler && g_upscaler->IsReady()) {
        st = "DLSS ACTIVE";
        stColor = HudColor(80, 255, 80, 255);
    } else {
        st = "DLSS INIT";
        stColor = HudColor(255, 200, 0, 255);
    }

    char l1[128], l2[128];
    snprintf(l1, sizeof l1, "ScaleNG  %s  %u fps", st, g_hudFps);
    snprintf(l2, sizeof l2, "render %ux%u -> %ux%u  eval ok %u fail %u",
             g_renderW, g_renderH, g_displayW, g_displayH, g_evalOkCount, g_evalFailCount);

    float w1 = (float)strlen(l1) * 6.0f + 6.0f;
    float w2 = (float)strlen(l2) * 6.0f + 6.0f;
    float bw = w1 > w2 ? w1 : w2;
    float x = (float)g_displayW - bw - 13.0f;
    float y = 10.0f;

    list->OMSetRenderTargets(1, &g_hudBbRtv, FALSE, nullptr);
    HudDrawBar(list, x - 3.0f, y - 3.0f, bw + 6.0f, 24.0f, HudColor(0, 0, 0, 160));
    HudDrawText(list, l1, x, y, HudColor(255, 255, 255, 255));
    HudDrawText(list, l2, x, y + 11.0f, stColor);
}


void* g_faultAddr = nullptr;
const char* g_injStep = "init";
CONTEXT g_faultCtx = {};
int g_faultCount = 0;
static void LogInjectFault(unsigned code);
static bool g_catchFaults = true;


// Dedicated init thread: builds injection resources + HUD pipeline OFF the
// ECL hot path. Signaled by KickInitThread once gameplay is active.
static HANDLE g_initThreadEv = nullptr;
static HANDLE g_initThreadH = nullptr;
static volatile long g_initThreadKick = 0;
volatile long g_injResourcesReady = 0; // atomic: 0=not ready, 1=ready

static DWORD WINAPI InitThreadProc(LPVOID)
{
    for (;;) {
        WaitForSingleObject(g_initThreadEv, INFINITE);
        if (!InterlockedCompareExchange(&g_injResourcesReady, 0, 0)) {
            EnsureInjectionResources();
            // HUD REMOVED from init: its creation surface (d3dcompiler,
            // PSOs, heaps on the wrapped device) is the prime crash suspect.
            // Activity signal moved to window title instead.
            InterlockedExchange(&g_injResourcesReady, (g_injAlloc && g_injList) ? 1 : 0);
            Log("hooks: init thread done (resources %s)",
                g_injAlloc ? "ok" : "FAIL");
        }
    }
    return 0;
}

static void KickInitThread()
{
    // Thread-safe: only create once even under concurrent ECL callbacks
    static volatile long s_initThreadCreated = 0;
    if (InterlockedCompareExchange(&s_initThreadCreated, 1, 0) == 0) {
        g_initThreadEv = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_initThreadH = CreateThread(nullptr, 0, InitThreadProc, nullptr, 0, nullptr);
    }
    if (!g_initThreadKick) {
        g_initThreadKick = 1;
        SetEvent(g_initThreadEv);
    }
}
void TryDeferredInject(ID3D12CommandQueue* injQueue)
{
    if (g_catchFaults) {
        __try {
            InjectAtPresentImpl(injQueue);
        } __except (g_faultAddr = (void*)GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                    g_faultCtx = *GetExceptionInformation()->ContextRecord,
                    EXCEPTION_EXECUTE_HANDLER) {
            LogInjectFault(GetExceptionCode());
        }
        return;
    }
    InjectAtPresentImpl(injQueue);
}

void InjectAtPresentImpl(ID3D12CommandQueue* injQueue)
{
    if (injQueue) g_graphicsQueue = injQueue;

    g_injStep = "gate";
    if (g_passiveMode) return;
    // All our present-time activity (HUD draw, NGX eval) is suppressed there -
    // touching the backbuffer while the engine slams loading frames is how we
    // crashed inside map loads.
    unsigned int fc = g_frameCounter;
    bool gameplayActive = g_lastCamPatchFrame != 0 &&
        fc >= g_lastCamPatchFrame && (fc - g_lastCamPatchFrame) < 120;
    static bool s_wasActive = false;
    if (!gameplayActive) {
        if (s_wasActive)
            Log("hooks: gameplay inactive - present-time activity suppressed");
        s_wasActive = false;
        return;
    }
    if (!s_wasActive) {
        Log("hooks: gameplay active - resuming present-time activity");
        s_wasActive = true;
    }

    // Delayed init: require 300 stable gameplay frames after load completes.
    // This ensures the volatile loading phase is completely over before we
    // create ANY D3D12 objects (the creation burst caused all crashes).
    static unsigned int s_stableFrames = 0;
    ++s_stableFrames;
    if (!g_injResourcesReady && s_stableFrames < 300)
        return;

    // Hotkeys (edge-triggered on keydown): F9 = toggle overlay, F10 = toggle DLAA.
    if (GetAsyncKeyState(VK_F9) & 1) {
        g_showHud = !g_showHud;
        Log("hud: overlay %s", g_showHud ? "on" : "off");
    }
    if (GetAsyncKeyState(VK_F10) & 1) {
        g_dlaaMode = !g_dlaaMode;
        g_dlaaHalted = false;
        g_evalFailStreak = 0;
        Log("hud: DLAA injection %s", g_dlaaMode ? "on" : "off");
    }
    if (!g_showHud && !g_dlaaMode) return;

    ID3D12Resource* bb = nullptr;
    IDXGISwapChain3* sc3 = nullptr;
    __try {
        if (SUCCEEDED(g_swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
            UINT idx = sc3->GetCurrentBackBufferIndex();
            if (FAILED(sc3->GetBuffer(idx, IID_PPV_ARGS(&bb)))) bb = nullptr;
            sc3->Release();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("hooks: backbuffer fetch guarded (code %08X)", (unsigned)GetExceptionCode());
        if (sc3) sc3->Release();
        bb = nullptr;
    }
    g_injStep = "bb-fetched";
    if (!bb) return;

    // The backbuffer IS the true display surface: adopt its dimensions so the
    // DLAA feature (render==display) always matches what we feed it. Without
    // this, a stale loading-screen adoption (e.g. 1902x954) leaves the feature
    // smaller than the 1920x1001 backbuffer -> EvaluateFeature 0xBAD00005.
    D3D12_RESOURCE_DESC bbd = bb->GetDesc();
    if ((unsigned int)bbd.Width != g_displayW || (unsigned int)bbd.Height != g_displayH)
        AdoptDisplaySize((unsigned int)bbd.Width, (unsigned int)bbd.Height);

    g_injStep = "adopted";
    auto it = g_resourceStates.find(bb);
    if (it == g_resourceStates.end()) {
        Log("hooks: backbuffer %p untracked - present injection skipped", (void*)bb);
        bb->Release();
        return;
    }

    if (g_bbFormat == DXGI_FORMAT_UNKNOWN)
        g_bbFormat = bb->GetDesc().Format;

    if (g_dlaaMode) {
        // Cross-device bridge: NGX lives on OUR clean device (the game's
        // wrapped device lacks IDXGIDevice and crashes the driver in-eval).
        if (!EnsureBridge((unsigned int)bbd.Width, (unsigned int)bbd.Height, bbd.Format, g_device)) {
            static int s_brFail = 0;
            if (++s_brFail <= 3) Log("hooks: bridge unavailable - DLAA disabled this session");
        }
        // The DLSS output must be in the backbuffer's format (copies require
        // identical formats), so force it before the feature is created.
        if (g_dlssOutFormat != g_bbFormat) {
            if (g_dlssOutValid) {
                g_dlssOut->Release();
                g_dlssOut = nullptr;
                g_dlssOutValid = false;
            }
            g_dlssOutFormat = g_bbFormat;
            Log("hooks: DLSS output format -> %d (backbuffer)", (int)g_bbFormat);
        }
        EnsureUpscalerInit();
        if (!g_dlssOutValid) CreateDlssOut();
    }

    // One-time init runs on a DEDICATED THREAD, never inside the ECL callback.
    // The creation burst (PSOs/resources mid-callback) correlated with every
    // loading-phase crash of the fix20-22 era.
    if (!InterlockedCompareExchange(&g_injResourcesReady, 0, 0)) {
        bb->Release();
        KickInitThread();
        return;
    }
    if (!g_injAlloc || !g_injList || !g_injHeap || !g_injSamplerHeap) {
        bb->Release();
        return;
    }

    g_injStep = "gated";
    bool bridgeOk = true;
    bool doDlss = g_dlaaMode && !g_dlaaHalted && g_upscaler && g_upscaler->IsReady() && g_dlssOutValid;
    if (doDlss) {
        // STALENESS INVALIDATION: if depth/MV stamps are too old, the tracked
        // pointers likely reference freed engine resources. Null them out so
        // the bridge flow never touches them. Re-discovery will repopulate.
        // 3-frame max age: during gameplay depth/MV render EVERY frame. A gap
        // means a renderer transition (map load, resize) - the old pointers
        // may be freed with heap memory reused, which passes null checks but
        // hands the driver garbage (TDR). Tight threshold trades rediscovery
        // cost for safety.
        unsigned int fc2 = g_frameCounter;
        bool depthStale = g_depthValid && (fc2 < g_depthStamp || fc2 - g_depthStamp > 3);
        bool mvStale = g_mvValid && (fc2 < g_mvStamp || fc2 - g_mvStamp > 3);
        if (depthStale || mvStale) {
            static int s_staleLogs = 0;
            if (++s_staleLogs <= 5)
                Log("hooks: DLAA invalidated stale inputs (depth age %u, mv age %u)",
                    depthStale ? fc2 - g_depthStamp : 0,
                    mvStale ? fc2 - g_mvStamp : 0);
            doDlss = false;
            // Null the stale pointers so the null guard catches them next frame
            if (depthStale) { g_depthResource = nullptr; g_depthValid = false; }
            if (mvStale) { g_mvResource = nullptr; g_mvValid = false; }
        }
    }
    if (doDlss && g_dlssOut) {
        // Never hand NGX mismatched sizes even if something above failed to
        // re-create the feature/output - 0xBAD00005 storms destabilize drivers.
        D3D12_RESOURCE_DESC od = g_dlssOut->GetDesc();
        if (od.Width != bbd.Width || od.Height != bbd.Height) {
            static int s_dimSkips = 0;
            if (++s_dimSkips <= 5)
                Log("hooks: DLAA skipped - output %ux%u != backbuffer %ux%u",
                    (unsigned)od.Width, (unsigned)od.Height,
                    (unsigned)bbd.Width, (unsigned)bbd.Height);
            doDlss = false;
        }
    }
    // DANGER REMOVED: the old SEH-guarded GetDesc probe called methods on
    // possibly-freed COM objects - UB that can corrupt state even when caught.
    // Staleness is now handled purely via discovery stamps + scene-refresh
    // invalidation (map load resets stamps to force re-discovery).
    bool doHud = false; // HUD removed: creation surface was the crash source
    if (!doDlss && !doHud) {
        bb->Release();
        return;
        bb->Release();
        return;
    }
    g_injStep = "resources-ready";

    // The previous injected list must be finished before reusing the allocator.
    if (g_injSubmitted && g_injFence && g_injEvent) {
        if (g_injFence->GetCompletedValue() < g_injFenceVal) {
            g_injFence->SetEventOnCompletion(g_injFenceVal, g_injEvent);
            WaitForSingleObject(g_injEvent, INFINITE);
        }
    }

    // Graveyard flush: the fence wait above proved ALL queued GPU work has
    // drained, so anything parked earlier is safe to release now.
    if (g_graveN > 0) {
        for (int i = 0; i < g_graveN; ++i)
            if (g_grave[i]) { g_grave[i]->Release(); g_grave[i] = nullptr; }
        g_graveN = 0;
    }

    g_injAlloc->Reset();
    g_injList->Reset(g_injAlloc, nullptr);

    D3D12_RESOURCE_STATES bbState = it->second;
    auto dit = g_resourceStates.find(g_depthResource);
    auto mit = g_resourceStates.find(g_mvResource);
    D3D12_RESOURCE_STATES depthState = dit != g_resourceStates.end() ? dit->second : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mvState = mit != g_resourceStates.end() ? mit->second : D3D12_RESOURCE_STATE_COMMON;

    if (doDlss) {
        if (!g_bridgeReady || !g_gameColor || !g_gameDepth || !g_gameMv || !g_gameOut
            || !g_depthResource || !g_mvResource) {
            doDlss = false;
            static int s_nullSkip = 0;
            if (++s_nullSkip <= 5)
                Log("hooks: DLAA skipped - null ptr: brC=%p brD=%p brM=%p brO=%p dep=%p mv=%p",
                    (void*)g_gameColor, (void*)g_gameDepth, (void*)g_gameMv, (void*)g_gameOut,
                    (void*)g_depthResource, (void*)g_mvResource);
            bb->Release(); return;
        } else {
            // Single outer SEH: stale engine resources pass null checks but
            // fault inside the driver when used. On fault we abandon the list,
            // invalidate all tracked inputs, and skip DLAA safely.
            __try {
            // ---- BRIDGE FLOW (game queue -> our device -> game queue) ----
            // Per-call instrumentation: g_injStep updated between EVERY D3D12
            // call so the fault handler pinpoints the exact crash point.
            g_injStep = "bridge:alloc-reset";
            g_injAlloc->Reset();
            g_injStep = "bridge:list-reset";
            g_injList->Reset(g_injAlloc, nullptr);
            g_injStep = "bridge:bb-barrier";
            Barrier(g_injList, bb, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g_injStep = "bridge:depth-barrier";
            Barrier(g_injList, g_depthResource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g_injStep = "bridge:mv-barrier";
            Barrier(g_injList, g_mvResource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (!g_gameColor || !g_gameDepth || !g_gameMv) {
                Log("hooks: bridge SKIP - null shared resource (c=%p d=%p m=%p)",
                    (void*)g_gameColor, (void*)g_gameDepth, (void*)g_gameMv);
                bb->Release();
                return;
            }
            g_injStep = "bridge:copy-color";
            D3D12_TEXTURE_COPY_LOCATION cd = {}; cd.pResource = g_gameColor;
            cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; cd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION cs = {}; cs.pResource = bb;
            cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; cs.SubresourceIndex = 0;
            Real_CopyTextureRegion(g_injList, &cd, 0, 0, 0, &cs, 0);
            g_injStep = "bridge:copy-depth";
            D3D12_TEXTURE_COPY_LOCATION dd2 = {}; dd2.pResource = g_gameDepth;
            dd2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dd2.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ds = {}; ds.pResource = g_depthResource;
            ds.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; ds.SubresourceIndex = 0;
            Real_CopyTextureRegion(g_injList, &dd2, 0, 0, 0, &ds, 0);
            g_injStep = "bridge:copy-mv";
            D3D12_TEXTURE_COPY_LOCATION md2 = {}; md2.pResource = g_gameMv;
            md2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; md2.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ms = {}; ms.pResource = g_mvResource;
            ms.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; ms.SubresourceIndex = 0;
            Real_CopyTextureRegion(g_injList, &md2, 0, 0, 0, &ms, 0);
            g_injStep = "bridge:restore-bb";
            Barrier(g_injList, bb, D3D12_RESOURCE_STATE_PRESENT);
            g_injStep = "bridge:restore-depth";
            Barrier(g_injList, g_depthResource, depthState);
            g_injStep = "bridge:restore-mv";
            Barrier(g_injList, g_mvResource, mvState);
            g_injStep = "bridge:close-ecl";
            g_injList->Close();
            ++g_bridgeVal;
            ID3D12CommandList* cl1[] = { g_injList };
            Real_ExecuteCommandLists(injQueue, 1, cl1);
            g_injStep = "bridge:signal-v1";
            injQueue->Signal(g_bridgeFence, g_bridgeVal);

            // Our device: wait for inputs, evaluate DLAA.
            UINT64 v1 = g_bridgeVal;
            g_bridgeQueue->Wait(g_bridgeFence, v1);
            // CPU-side safety: prove GPU finished prior bridge work before Reset
            if (g_bridgeFence && g_bridgeLastSubmit > 0 && g_bridgeFenceEv) {
                if (g_bridgeFence->GetCompletedValue() < g_bridgeLastSubmit) {
                    g_bridgeFence->SetEventOnCompletion(g_bridgeLastSubmit, g_bridgeFenceEv);
                    WaitForSingleObject(g_bridgeFenceEv, 5000);
                }
            }
            g_bridgeAlloc->Reset();
            g_bridgeList->Reset(g_bridgeAlloc, nullptr);
            g_injStep = "pre-evaluate";
            D3D12_RESOURCE_BARRIER bi[3] = {};
            for (int i = 0; i < 3; ++i) {
                bi[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bi[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bi[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                bi[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            bi[0].Transition.pResource = g_brColor;
            bi[1].Transition.pResource = g_brDepth;
            bi[2].Transition.pResource = g_brMv;
            g_bridgeList->ResourceBarrier(3, bi);
            UpscalerEvaluateParams ep = {};
            ep.commandList = g_bridgeList;
            ep.color = g_brColor;
            ep.depth = g_brDepth;
            ep.motionVectors = g_brMv;
            ep.output = g_brOut;
            ep.jitterX = g_currJitter.x;
            ep.jitterY = g_currJitter.y;
            ep.mvScaleX = (float)g_displayW;
            ep.mvScaleY = (float)g_displayH;
            ep.sharpness = g_cfg.sharpness;
            bool evalOk = g_upscaler->Evaluate(ep);
            for (int i = 0; i < 3; ++i) { bi[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON; bi[i].Transition.StateBefore = bi[i].Transition.StateAfter == D3D12_RESOURCE_STATE_COMMON ? D3D12_RESOURCE_STATE_COMMON : bi[i].Transition.StateAfter; }
            for (int i = 0; i < 3; ++i) {
                bi[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                bi[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            }
            g_bridgeList->ResourceBarrier(3, bi);
            D3D12_RESOURCE_BARRIER bo = {};
            bo.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bo.Transition.pResource = g_brOut;
            bo.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            bo.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            bo.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            (void)bo;
            g_bridgeList->Close();
            ++g_bridgeVal;
            UINT64 v2 = g_bridgeVal;
            ID3D12CommandList* cl2[] = { g_bridgeList };
            g_bridgeQueue->ExecuteCommandLists(1, cl2);
            g_bridgeQueue->Signal(g_bridgeFence, v2);
            g_bridgeLastSubmit = v2;

            // Cross-queue sync: open the shared fence on the game device ONCE,
            // then use it for both evalOk Wait and copy-back Wait.
            if (!g_gameFence && g_bridgeFenceShared && g_device) {
                HRESULT fhr = g_device->OpenSharedHandle(g_bridgeFenceShared, IID_PPV_ARGS(&g_gameFence));
                Log("bridge: opened game-side fence hr=0x%08X ptr=%p", (unsigned)fhr, (void*)g_gameFence);
            }
            if (evalOk) {
                // GPU-side wait: game queue blocks until bridge signals v2
                if (g_gameFence) injQueue->Wait(g_gameFence, g_bridgeVal);
                ++g_evalOkCount;
                g_evalFailStreak = 0;
                g_evalDidBridge = true;
                doDlss = true;
            } else {
                ++g_evalFailCount;
                if (++g_evalFailStreak >= 30 && !g_dlaaHalted) {
                    g_dlaaHalted = true;
                    Log("hooks: DLAA HALTED after %u consecutive eval failures", g_evalFailStreak);
                }
                doDlss = false;g_evalDidBridge = false;
            }            } // end __try
            __except (EXCEPTION_EXECUTE_HANDLER) {
                bridgeOk = false;
                Log("hooks: bridge FAULTED at %s - invalidating all inputs", g_injStep);
                g_depthResource = nullptr; g_depthValid = false; g_depthStamp = 0;
                g_mvResource = nullptr; g_mvValid = false; g_mvStamp = 0;
            }

        }
    }


                if (!bridgeOk) {
                // Abandoned list - do not submit. bb was AddRef'd at entry;
                // release it and return without touching D3D12 further.
                Log("hooks: bridge fault path - skipping frame submit");
                bb->Release();
                return;
            }
// Cross-queue sync for copy-back: game queue waits on the SAME fence.
    if (g_evalDidBridge && g_gameFence) {
        injQueue->Wait(g_gameFence, g_bridgeVal);
    }
    g_injStep = "submit";
    g_injList->Close();
    ID3D12CommandList* cls[] = { g_injList };
    Real_ExecuteCommandLists(injQueue, 1, cls);
    if (g_injFence) injQueue->Signal(g_injFence, ++g_injFenceVal);
    g_injSubmitted = true;
    // Copy the DLAA result from the shared texture into the backbuffer.
    if (g_evalDidBridge) {
        D3D12_RESOURCE_BARRIER bwo = {};
        bwo.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bwo.Transition.pResource = g_gameOut;
        bwo.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bwo.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bwo.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_injList->Reset(g_injAlloc, nullptr);
        g_injList->ResourceBarrier(1, &bwo);
        D3D12_RESOURCE_BARRIER bbc = {};
        bbc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bbc.Transition.pResource = bb;
        bbc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        bbc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        bbc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_injList->ResourceBarrier(1, &bbc);
        D3D12_TEXTURE_COPY_LOCATION dsto = {}; dsto.pResource = bb;
        dsto.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dsto.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srco = {}; srco.pResource = g_gameOut;
        srco.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srco.SubresourceIndex = 0;
        Real_CopyTextureRegion(g_injList, &dsto, 0, 0, 0, &srco, 0);
        g_injList->Close();
        ID3D12CommandList* rcls[] = { g_injList };
        Real_ExecuteCommandLists(injQueue, 1, rcls);
    }
    if (doDlss)
        Log("hooks: DLAA injection at present (frame %u)", g_frameCounter);
    bb->Release();
}

typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
PFN_Present Real_Present = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT,
    const DXGI_PRESENT_PARAMETERS*);
PFN_Present1 Real_Present1 = nullptr;

// Per-candidate Present stubs: the scan fn-hooks each vtable candidate's slot 8
// with a DISTINCT stub so the log tells us WHICH table the game actually
// presents through. Each stub forwards to the captured original of its own
// target, so non-swapchain tables (if ever called) are passed through intact.
#define SCALENG_MAX_PRESENT_CANDS 16
static bool IsReadablePtr(const void* p, size_t len);
static PFN_Present RealPresentCands[SCALENG_MAX_PRESENT_CANDS] = {};
static bool s_candFired[SCALENG_MAX_PRESENT_CANDS] = {};
static int s_nextCandIdx = 0;
static bool g_scanDone = false;

static bool g_inPresent = false;
static void* s_presentExAddr = nullptr;

static void LogPresentGuardDetails(void* exAddr)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(exAddr, &mbi, sizeof(mbi))) {
        Log("guard: region base %p size %llX protect %X type %X",
            mbi.BaseAddress, (unsigned long long)mbi.RegionSize, (unsigned)mbi.Protect,
            (unsigned)mbi.Type);
        char mod[MAX_PATH] = {};
        if (mbi.BaseAddress && GetModuleFileNameA((HMODULE)mbi.BaseAddress, mod, MAX_PATH))
            Log("guard: module %s", mod);
    }
    if (IsReadablePtr(exAddr, 32)) {
        unsigned char* p = (unsigned char*)exAddr;
        Log("guard: bytes @ %p: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            exAddr, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    } else {
        Log("guard: bytes @ %p: unreadable", exAddr);
    }
    void* region = mbi.BaseAddress;
    if (IsReadablePtr(region, 64)) {
        unsigned char* p = (unsigned char*)region;
        Log("guard: region head @ %p: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            region, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }
}

static HRESULT STDMETHODCALLTYPE PresentCore(IDXGISwapChain* sc, UINT syncInterval,
                                             UINT flags, int candIdx)
{
    if (sc) {
        __try {
            if (!s_candFired[candIdx]) {
                s_candFired[candIdx] = true;
                void* vt = nullptr;
                if (IsReadablePtr(sc, sizeof(void*)))
                    vt = *(void**)sc;
                Log("hooks: PRESENT fires via candidate %d (sc %p, vt %p)", candIdx, (void*)sc, vt);
                if (IsReadablePtr(vt, 8 * 9)) {
                    void* s2 = ((void**)vt)[2];
                    void* s8 = ((void**)vt)[8];
                    Log("hooks: vt slot2 (GetDesc) %p slot8 (Present) %p", s2, s8);
                    if (IsReadablePtr(s2, 16)) {
                        unsigned char* q = (unsigned char*)s2;
                        Log("hooks: slot2 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                            q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
                            q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
                    }
                }
            }
if (sc != g_swapchain) {
                g_swapchain = sc;
                Log("hooks: present1 on real swapchain %p (format %d, GetDesc skipped - trapped)", (void*)sc, (int)g_bbFormat);
            }
            if (!g_inPresent) {
                g_inPresent = true;
                if (s_candFired[candIdx]) {
                    static int s_skipCount = 0;
                    if ((++s_skipCount % 300) == 0)
                        Log("hooks: present #%d observed (inject disabled)", s_skipCount);
                } else {
                    Log("hooks: inject skipped (disabled) - sc %p", (void*)sc);
                }
                g_inPresent = false;
            }
        }
        __except (s_presentExAddr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                  EXCEPTION_EXECUTE_HANDLER) {
            g_inPresent = false;
            Log("hooks: present handling guarded (code %08X @ %p)", (unsigned)GetExceptionCode(),
                s_presentExAddr);
            LogPresentGuardDetails(s_presentExAddr);
        }
    }
    if (RealPresentCands[candIdx])
        return RealPresentCands[candIdx](sc, syncInterval, flags);
    return DXGI_ERROR_INVALID_CALL;
}

#define DEFINE_PRESENT_STUB(N)                                                     \
    HRESULT STDMETHODCALLTYPE PresentStub_##N(IDXGISwapChain* sc, UINT syncInterval, \
                                              UINT flags)                           \
    {                                                                               \
        return PresentCore(sc, syncInterval, flags, N);                             \
    }

DEFINE_PRESENT_STUB(0)
DEFINE_PRESENT_STUB(1)
DEFINE_PRESENT_STUB(2)
DEFINE_PRESENT_STUB(3)
DEFINE_PRESENT_STUB(4)
DEFINE_PRESENT_STUB(5)
DEFINE_PRESENT_STUB(6)
DEFINE_PRESENT_STUB(7)
DEFINE_PRESENT_STUB(8)
DEFINE_PRESENT_STUB(9)
DEFINE_PRESENT_STUB(10)
DEFINE_PRESENT_STUB(11)
DEFINE_PRESENT_STUB(12)
DEFINE_PRESENT_STUB(13)
DEFINE_PRESENT_STUB(14)
DEFINE_PRESENT_STUB(15)
static PFN_Present PresentStubs[SCALENG_MAX_PRESENT_CANDS] = {
    PresentStub_0, PresentStub_1, PresentStub_2, PresentStub_3,
    PresentStub_4, PresentStub_5, PresentStub_6, PresentStub_7,
    PresentStub_8, PresentStub_9, PresentStub_10, PresentStub_11,
    PresentStub_12, PresentStub_13, PresentStub_14, PresentStub_15,
};
static void LogInjectFault(unsigned code)
{
    ++g_faultCount;
    void* ea = g_faultAddr;
    HMODULE fmod = NULL;
    char where[96];
    snprintf(where, sizeof where, "non-module");
    if (GetModuleHandleExA(2, (const char*)ea, &fmod) && fmod) {
        char mp[MAX_PATH] = {};
        GetModuleFileNameA(fmod, mp, MAX_PATH);
        const char* fn = strrchr(mp, 92); fn = fn ? fn + 1 : mp;
        snprintf(where, sizeof where, "%s+0x%llX", fn, (unsigned long long)((uintptr_t)ea - (uintptr_t)fmod));
    }
    if (g_faultCount <= 3)
        Log("hooks: InjectAtPresent faulted at step: %s (code %08X @ %p [%s]) RIP=%llX RAX=%llX RCX=%llX",
            g_injStep, code, ea, where,
            (unsigned long long)g_faultCtx.Rip, (unsigned long long)g_faultCtx.Rax,
            (unsigned long long)g_faultCtx.Rcx);
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    if (sc) {
        __try {
            if (sc != g_swapchain) {
                g_swapchain = sc;
                Log("hooks: present on real swapchain %p (format %d)", (void*)sc, (int)g_bbFormat);
            }
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: present handling guarded (code %08X)", (unsigned)GetExceptionCode());
        }
    }
    if (Real_Present)
        return Real_Present(sc, syncInterval, flags);
    return DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* sc, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* params)
{
    if (sc) {
        __try {
            if (sc != g_swapchain) {
                g_swapchain = sc;
                Log("hooks: present on real swapchain %p (format %d)", (void*)sc, (int)g_bbFormat);
            }
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
                // Heavy work moved to ExecuteCommandLists (game-queue ordering).
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: present1 handling guarded (code %08X)", (unsigned)GetExceptionCode());
        }
    }
    if (Real_Present1)
        return Real_Present1(sc, syncInterval, flags, params);
    return DXGI_ERROR_INVALID_CALL;
}

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, REFIID, void**);
PFN_CreateSwapChainForHwnd Real_CreateSwapChainForHwnd = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChainForCoreWindow)(IDXGIFactory2*, IUnknown*,
    IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, REFIID, void**);
PFN_CreateSwapChainForCoreWindow Real_CreateSwapChainForCoreWindow = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*,
    DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
PFN_CreateSwapChain Real_CreateSwapChain = nullptr;

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* restrictToOutput, REFIID riid, void** ppSwapChain);
HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppSwapChain);

static bool IsReadablePtr(const void* p, size_t len)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

static bool IsExecutableImagePtr(const void* p)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
    return (mbi.Protect & 0xF0) != 0; // any PAGE_EXECUTE_* bit
}

// The object returned by the game's swapchain-creation calls is NOT a real
// DXGI swapchain (it points into game memory / is ASCII garbage) and must
// never be dereferenced blindly - the game crashes. Returns true only when
// the Present hook was actually installed on a sane target.
bool InstallSwapchainHooks(IDXGISwapChain* sc)
{
    if (!sc) return false;
    void** vt = nullptr;
    __try {
        if (!IsReadablePtr(sc, sizeof(void*))) {
            Log("hooks: swapchain %p rejected (not readable memory)", (void*)sc);
            return false;
        }
        vt = *(void***)sc;
        if (!IsReadablePtr(vt, sizeof(void*) * 23)) {
            Log("hooks: swapchain %p rejected (vtable not readable)", (void*)sc);
            return false;
        }
        if (!IsExecutableImagePtr(vt[8])) {
            Log("hooks: swapchain %p rejected (vt[8]=%p not exec image)", (void*)sc, (void*)vt[8]);
            return false;
        }
        MH_STATUS st = MH_CreateHook(vt[8], &Hook_Present, (void**)&Real_Present);
        if (st == MH_OK) {
            if (MH_EnableHook(vt[8]) == MH_OK) {
                void* targets[1] = { (void*)Hook_Present };
                CfgMarkValid(targets, 1);
                Log("hooks: swapchain %p Present hooked", (void*)sc);
                if (vt[22] && IsExecutableImagePtr(vt[22])) {
                    MH_STATUS st1 = MH_CreateHook(vt[22], &Hook_Present1, (void**)&Real_Present1);
                    if (st1 == MH_OK && MH_EnableHook(vt[22]) == MH_OK) {
                        Log("hooks: swapchain %p Present1 hooked", (void*)sc);
                    } else if (st1 != MH_ERROR_ALREADY_CREATED) {
                        Log("hooks: swapchain %p Present1 hook failed (st=%d)", (void*)sc, (int)st1);
                    }
                }
                return true;
            }
            Log("hooks: swapchain %p Present enable failed", (void*)sc);
            return false;
        }
        if (st == MH_ERROR_ALREADY_CREATED) {
            Log("hooks: swapchain %p Present already hooked (shared vtable)", (void*)sc);
            if (vt[22] && IsExecutableImagePtr(vt[22])) {
                MH_STATUS st1 = MH_CreateHook(vt[22], &Hook_Present1, (void**)&Real_Present1);
                if (st1 == MH_OK && MH_EnableHook(vt[22]) == MH_OK) {
                    Log("hooks: swapchain %p Present1 hooked", (void*)sc);
                } else if (st1 != MH_ERROR_ALREADY_CREATED) {
                    Log("hooks: swapchain %p Present1 hook failed (st=%d)", (void*)sc, (int)st1);
                }
            }
            return true;
        }
        char mod1[64] = "?", mod2[64] = "?", mod3[64] = "?";
        wchar_t wm1[64] = {}, wm2[64] = {}, wm3[64] = {};
        HMODULE m1 = nullptr, m2 = nullptr, m3 = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(ULONG_PTR)sc, &m1);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(ULONG_PTR)vt, &m2);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(ULONG_PTR)vt[8], &m3);
        if (m1) K32GetModuleBaseNameW(GetCurrentProcess(), m1, wm1, 64);
        if (m2) K32GetModuleBaseNameW(GetCurrentProcess(), m2, wm2, 64);
        if (m3) K32GetModuleBaseNameW(GetCurrentProcess(), m3, wm3, 64);
        Log("hooks: swapchain %p Present hook FAILED st=%d sc_mod=%ls(%p) vt_mod=%ls(%p) vt8=%p mod=%ls(%p)",
            (void*)sc, (int)st, wm1, (void*)m1, wm2, (void*)m2, (void*)vt[8], wm3, (void*)m3);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("hooks: swapchain %p install guarded (code %08X)", (void*)sc, (unsigned)GetExceptionCode());
    }
    return false;
}

// The game's real swapchain never shows up through any factory hook (the
// object returned by CreateSwapChainForHwnd lives inside the game's own image
// and is not a usable DXGI object). Every real DXGI swapchain shares ONE static
// vtable inside dxgi.dll, so we create a throwaway composition swapchain
// through the raw factory vtable and hook ITS Present slot - that lands the
// hook on the shared table and intercepts the game's real Present regardless
// of where its swapchain came from.
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChainForComposition)(IDXGIFactory2*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, REFIID, void**);

static HWND s_dummyHwnd = nullptr;

static HWND EnsureDummyWindow()
{
    if (s_dummyHwnd) return s_dummyHwnd;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ScaleNGDummyWnd";
    RegisterClassExW(&wc);
    s_dummyHwnd = CreateWindowExW(0, L"ScaleNGDummyWnd", L"ScaleNG", WS_OVERLAPPED,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 2, 2, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    return s_dummyHwnd;
}

void EnsureGlobalSwapchainHook()
{
    static int s_tries = 0;
    if (g_scanDone || s_tries >= 10) return;
    if (!g_device || !g_graphicsQueue) return;
    ++s_tries;

    __try {
        // Get a REAL dxgi factory (OptiScaler wraps the factories returned by
        // CreateDXGIFactory* with broken vtables). The game's device is created
        // with DISABLE_IMPLICIT_DXGI so QI(IDXGIDevice) fails - instead use the
        // adapter the game passed to D3D12CreateDevice: its parent IS the real
        // dxgi factory that enumerated it.
        IDXGIFactory* factory = nullptr;
        HRESULT hr = E_FAIL;
        if (g_adapter) {
            hr = g_adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
        }
        if (FAILED(hr) || !factory) {
            Log("hooks: EGSH adapter GetParent failed (hr=%08X)", (unsigned)hr);
            return;
        }
        Log("hooks: EGSH real factory %p", (void*)factory);

        void** fvt = *(void***)factory;
        static bool s_slotsLogged = false;
        if (!s_slotsLogged) {
            s_slotsLogged = true;
            Log("hooks: EGSH factory slots 10..17: %p %p %p %p %p %p %p %p",
                (void*)fvt[10], (void*)fvt[11], (void*)fvt[12], (void*)fvt[13],
                (void*)fvt[14], (void*)fvt[15], (void*)fvt[16], (void*)fvt[17]);
        }
        if (!IsReadablePtr(fvt, sizeof(void*) * 14) || !fvt[10]) {
            Log("hooks: EGSH real factory vtable bad (slot10 %p)", (void*)fvt[10]);
            factory->Release();
            return;
        }
        // All real dxgi factory instances share this static vtable, so hooking
        // slots 10/15 here covers EVERY swapchain the game creates through any
        // real factory. Chained on top of any OptiScaler detour already in the
        // slot (MH reads the current slot value).
        if (!Real_CreateSwapChainForHwnd && fvt[15]) {
            MH_STATUS st15 = MH_CreateHook(fvt[15], &Hook_CreateSwapChainForHwnd,
                                           (void**)&Real_CreateSwapChainForHwnd);
            if (st15 == MH_OK || st15 == MH_ERROR_ALREADY_CREATED) {
                if (MH_EnableHook(fvt[15]) == MH_OK) {
                    void* targets[1] = { (void*)Hook_CreateSwapChainForHwnd };
                    CfgMarkValid(targets, 1);
                    Log("hooks: EGSH real factory slot15 hooked");
                }
            } else {
                Log("hooks: EGSH real factory slot15 hook failed (st=%d)", (int)st15);
            }
        }
        if (!Real_CreateSwapChain && fvt[10]) {
            MH_STATUS st10 = MH_CreateHook(fvt[10], &Hook_CreateSwapChain,
                                           (void**)&Real_CreateSwapChain);
            if (st10 == MH_OK || st10 == MH_ERROR_ALREADY_CREATED) {
                if (MH_EnableHook(fvt[10]) == MH_OK) {
                    void* targets[1] = { (void*)Hook_CreateSwapChain };
                    CfgMarkValid(targets, 1);
                    Log("hooks: EGSH real factory slot10 hooked");
                }
            } else {
                Log("hooks: EGSH real factory slot10 hook failed (st=%d)", (int)st10);
            }
        }
        // SELF-SUFFICIENT DUMMY PATH: the game's device rejects IDXGIDevice QI
        // (DISABLE_IMPLICIT_DXGI-style), so swapchain creation on ITS queues
        // fails (887A0001) or AVs inside dxgi. Create a FRESH device + queue +
        // factory instead - the resulting swapchain uses the SAME shared static
        // dxgi vtable as every other swapchain, so InstallSwapchainHooks on it
        // lands MinHook on the shared Present/Present1 functions and covers the
        // game's real Present no matter how its own swapchain behaves.
        {
            HWND dummyWnd = EnsureDummyWindow();
            Log("hooks: EGSH dummy window %p", (void*)dummyWnd);
            ID3D12Device* ddev = nullptr;
            HRESULT dhr = E_FAIL;
            if (Real_D3D12CreateDevice_Tramp)
                dhr = Real_D3D12CreateDevice_Tramp(nullptr, D3D_FEATURE_LEVEL_11_0,
                                             __uuidof(ID3D12Device), (void**)&ddev);
            Log("hooks: EGSH fresh device hr=%08X dev=%p", (unsigned)dhr, (void*)ddev);
            IDXGISwapChain1* dummy = nullptr;
            if (SUCCEEDED(dhr) && ddev) {
                ID3D12CommandQueue* dq = nullptr;
                D3D12_COMMAND_QUEUE_DESC qd = {};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                if (SUCCEEDED(ddev->CreateCommandQueue(&qd, IID_PPV_ARGS(&dq))) && dq) {
                    typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1Raw)(REFIID, void**);
                    HMODULE dxgiMod = GetModuleHandleA("dxgi.dll");
                    PFN_CreateDXGIFactory1Raw mkFactory =
                        dxgiMod ? (PFN_CreateDXGIFactory1Raw)GetProcAddress(dxgiMod, "CreateDXGIFactory1")
                                : nullptr;
                    IDXGIFactory4* f4 = nullptr;
                    HRESULT fhr = mkFactory ? mkFactory(__uuidof(IDXGIFactory4), (void**)&f4) : E_FAIL;
                    Log("hooks: EGSH fresh factory hr=%08X f=%p", (unsigned)fhr, (void*)f4);
                    if (SUCCEEDED(fhr) && f4) {
                        DXGI_SWAP_CHAIN_DESC1 sd = {};
                        sd.BufferCount = 2;
                        sd.Width = 8;
                        sd.Height = 8;
                        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                        sd.SampleDesc.Count = 1;
                        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                        dhr = f4->CreateSwapChainForHwnd(dq, dummyWnd, &sd, nullptr, nullptr, &dummy);
                        Log("hooks: EGSH fresh ForHwnd hr=%08X sc=%p", (unsigned)dhr, (void*)dummy);
                        if (SUCCEEDED(dhr) && dummy && !Real_Present)
                            InstallSwapchainHooks((IDXGISwapChain*)dummy);
                        if (dummy) { dummy->Release(); dummy = nullptr; }
                        f4->Release();
                    }
                    dq->Release();
                }
                ddev->Release();
            }
            Log("hooks: EGSH dummy result - Real_Present %s",
                Real_Present ? "HOOKED" : "NOT HOOKED");
            g_swapchain = nullptr; // real one adopted at first present via Hook_Present self-heal
        }
        factory->Release();
        g_scanDone = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("hooks: EGSH guarded (code %08X)", (unsigned)GetExceptionCode());
    }
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* restrictToOutput, REFIID riid, void** ppSwapChain)
{
    HRESULT hr = Real_CreateSwapChainForHwnd(factory, device, hwnd, desc, fullscreen,
                                             restrictToOutput, riid, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain* sc = (IDXGISwapChain*)*ppSwapChain;
        if (desc) g_bbFormat = desc->Format;
        Log("hooks: CreateSwapChainForHwnd returned %p (hwnd %p, format %d, hr=%08X)",
            (void*)sc, (void*)hwnd, (int)g_bbFormat, (unsigned)hr);
        __try {
            bool rd = IsReadablePtr(sc, sizeof(void*));
            if (rd) {
                void* vt = *(void**)sc;
                bool vtRd = IsReadablePtr(vt, sizeof(void*) * 24);
                Log("hooks: ForHwnd sc readable, vtable %p readable=%d vt8=%p exec=%d vt22=%p",
                    vt, vtRd ? 1 : 0,
                    (void*)(vtRd ? ((void**)vt)[8] : 0),
                    (vtRd && IsExecutableImagePtr(((void**)vt)[8])) ? 1 : 0,
                    (void*)(vtRd ? ((void**)vt)[22] : 0));
            } else {
                Log("hooks: ForHwnd sc %p NOT readable", (void*)sc);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: ForHwnd sc analysis guarded (code %08X)", (unsigned)GetExceptionCode());
        }
        void** svt = nullptr;
        __try {
            if (IsReadablePtr(sc, sizeof(void*)))
                svt = *(void***)sc;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: swapchain result %p guarded (code %08X)", (void*)sc, (unsigned)GetExceptionCode());
        }
        if (!svt) return hr;
        if (InstallSwapchainHooks(sc))
            g_swapchain = sc;
        // Function-level hooks: the game may present through OptiScaler's
        // hijacked vtable which forwards to dxgi's Present function directly -
        // hook the function targets so presents are seen regardless of table.
        // (Only when the slot still holds the ORIGINAL function, not our hook.)
        if (!Real_Present && svt[8] && svt[8] != (void*)Hook_Present && IsExecutableImagePtr(svt[8])) {
            MH_STATUS st = MH_CreateHook(svt[8], &Hook_Present, (void**)&Real_Present);
            if (st == MH_OK && MH_EnableHook(svt[8]) == MH_OK)
                Log("hooks: present fn %p hooked from ForHwnd result", (void*)svt[8]);
        }
        if (!Real_Present1 && svt[22] && svt[22] != (void*)Hook_Present1 && IsExecutableImagePtr(svt[22])) {
            MH_STATUS st = MH_CreateHook(svt[22], &Hook_Present1, (void**)&Real_Present1);
            if (st == MH_OK && MH_EnableHook(svt[22]) == MH_OK)
                Log("hooks: present1 fn %p hooked from ForHwnd result", (void*)svt[22]);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device,
    IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput,
    REFIID riid, void** ppSwapChain)
{
    HRESULT hr = Real_CreateSwapChainForCoreWindow(factory, device, window, desc,
                                                   restrictToOutput, riid, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain* sc = (IDXGISwapChain*)*ppSwapChain;
        if (desc) g_bbFormat = desc->Format;
        Log("hooks: CreateSwapChainForCoreWindow returned %p (format %d)", (void*)sc, (int)g_bbFormat);
        void** svt = nullptr;
        __try {
            if (IsReadablePtr(sc, sizeof(void*)))
                svt = *(void***)sc;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: swapchain result %p guarded (code %08X)", (void*)sc, (unsigned)GetExceptionCode());
        }
        if (!svt) return hr;
        if (InstallSwapchainHooks(sc))
            g_swapchain = sc;
        if (!Real_Present && svt[8] && svt[8] != (void*)Hook_Present && IsExecutableImagePtr(svt[8])) {
            MH_STATUS st = MH_CreateHook(svt[8], &Hook_Present, (void**)&Real_Present);
            if (st == MH_OK && MH_EnableHook(svt[8]) == MH_OK)
                Log("hooks: present fn %p hooked from CoreWindow result", (void*)svt[8]);
        }
        if (!Real_Present1 && svt[22] && svt[22] != (void*)Hook_Present1 && IsExecutableImagePtr(svt[22])) {
            MH_STATUS st = MH_CreateHook(svt[22], &Hook_Present1, (void**)&Real_Present1);
            if (st == MH_OK && MH_EnableHook(svt[22]) == MH_OK)
                Log("hooks: present1 fn %p hooked from CoreWindow result", (void*)svt[22]);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppSwapChain)
{
    HRESULT hr = Real_CreateSwapChain(factory, device, desc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain* sc = *ppSwapChain;
        if (desc) g_bbFormat = desc->BufferDesc.Format;
        Log("hooks: CreateSwapChain returned %p (format %d)", (void*)sc, (int)g_bbFormat);
        void** svt = nullptr;
        __try {
            if (IsReadablePtr(sc, sizeof(void*)))
                svt = *(void***)sc;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: swapchain result %p guarded (code %08X)", (void*)sc, (unsigned)GetExceptionCode());
        }
        if (!svt) return hr;
        if (InstallSwapchainHooks(sc))
            g_swapchain = sc;
        if (!Real_Present && svt[8] && svt[8] != (void*)Hook_Present && IsExecutableImagePtr(svt[8])) {
            MH_STATUS st = MH_CreateHook(svt[8], &Hook_Present, (void**)&Real_Present);
            if (st == MH_OK && MH_EnableHook(svt[8]) == MH_OK)
                Log("hooks: present fn %p hooked from legacy result", (void*)svt[8]);
        }
        if (!Real_Present1 && svt[22] && svt[22] != (void*)Hook_Present1 && IsExecutableImagePtr(svt[22])) {
            MH_STATUS st = MH_CreateHook(svt[22], &Hook_Present1, (void**)&Real_Present1);
            if (st == MH_OK && MH_EnableHook(svt[22]) == MH_OK)
                Log("hooks: present1 fn %p hooked from legacy result", (void*)svt[22]);
        }
    }
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
PFN_CreateDXGIFactory2 Real_CreateDXGIFactory2 = nullptr;
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateDXGIFactory1)(REFIID, void**);
PFN_CreateDXGIFactory1 Real_CreateDXGIFactory1 = nullptr;
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateDXGIFactory)(REFIID, void**);
PFN_CreateDXGIFactory Real_CreateDXGIFactory = nullptr;

// DXGI factory objects always expose the full IDXGIFactory2+ vtable regardless
// of the riid the game requested, so hook the swapchain-creation slots on every
// factory instance we see (slot 10 legacy CreateSwapChain, 15 ForHwnd, 16
// ForCoreWindow). Without this, a factory requested as IDXGIFactory1 would
// never get its CreateSwapChainForHwnd hooked - the game's main swapchain
// slipped through exactly this way.
void HookFactoryObject(IDXGIFactory* factory)
{
    if (!factory) return;
    if (!g_anyFactory) g_anyFactory = factory;
    void** vt = *(void***)factory;
    bool any = false;
    if (!Real_CreateSwapChainForHwnd &&
        MH_CreateHook(vt[15], &Hook_CreateSwapChainForHwnd, (void**)&Real_CreateSwapChainForHwnd) == MH_OK) {
        if (MH_EnableHook(vt[15]) == MH_OK) { any = true; }
        else { Real_CreateSwapChainForHwnd = nullptr; }
    }
    if (!Real_CreateSwapChainForCoreWindow &&
        MH_CreateHook(vt[16], &Hook_CreateSwapChainForCoreWindow, (void**)&Real_CreateSwapChainForCoreWindow) == MH_OK) {
        if (MH_EnableHook(vt[16]) == MH_OK) { any = true; }
        else { Real_CreateSwapChainForCoreWindow = nullptr; }
    }
    if (!Real_CreateSwapChain &&
        MH_CreateHook(vt[10], &Hook_CreateSwapChain, (void**)&Real_CreateSwapChain) == MH_OK) {
        if (MH_EnableHook(vt[10]) == MH_OK) { any = true; }
        else { Real_CreateSwapChain = nullptr; }
    }
    if (any) {
        void* targets[3] = { (void*)Hook_CreateSwapChainForHwnd,
                             (void*)Hook_CreateSwapChainForCoreWindow,
                             (void*)Hook_CreateSwapChain };
        CfgMarkValid(targets, 3);
        Log("hooks: factory %p swapchain creation hooks installed", (void*)factory);
    }
}

HRESULT STDMETHODCALLTYPE Hook_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    HRESULT hr = Real_CreateDXGIFactory2(flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactoryObject((IDXGIFactory*)*ppFactory);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    HRESULT hr = Real_CreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactoryObject((IDXGIFactory*)*ppFactory);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    HRESULT hr = Real_CreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactoryObject((IDXGIFactory*)*ppFactory);
    return hr;
}

void Hook_CopyBufferRegion(ID3D12GraphicsCommandList* list, ID3D12Resource* dst,
                           UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset,
                           UINT64 numBytes)
{
    __try {
    static int s_otherSizes = 0;
    if (numBytes >= 1024 && numBytes != kCameraCbSize && numBytes != kVelocityCbSize &&
        srcOffset == 0 && dstOffset == 0 && s_otherSizes < 10) {
        ++s_otherSizes;
        Log("hooks: CopyBufferRegion size=%llu (src %p dst %p)", (unsigned long long)numBytes, (void*)src, (void*)dst);
    }
    if (src && dst && dstOffset == 0) {
        if (numBytes == kCameraCbSize) {
            if (!g_cameraRing) g_cameraRing = src;
            if (g_cameraRing == src || !g_cameraCbValid) {
                void* mapped = nullptr;
                if (SUCCEEDED(src->Map(0, nullptr, &mapped)) && mapped) {
                    float* cb = (float*)((char*)mapped + srcOffset);
                    if (ValidateCameraCb(cb, numBytes)) {
                        if (g_cameraRing != src) {
                            g_cameraRing = src;
                            Log("hooks: camera CB ring re-discovered %p", (void*)src);
                        }
                        static int s_acceptDumps = 0;
                        if (s_acceptDumps < 1) {
                            ++s_acceptDumps;
                            const float* ac = cb;
                            Log("hooks: ACCEPT f0..7=%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f jit=%.2f/%.2f",
                                ac[0], ac[1], ac[2], ac[3], ac[4], ac[5], ac[6], ac[7],
                                g_currJitter.x, g_currJitter.y);
                            Log("hooks: ACCEPT w2c=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                                F2U(ac[56]), F2U(ac[57]), F2U(ac[58]), F2U(ac[59]),
                                F2U(ac[60]), F2U(ac[61]), F2U(ac[62]), F2U(ac[63]),
                                F2U(ac[64]), F2U(ac[65]), F2U(ac[66]), F2U(ac[67]),
                                F2U(ac[68]), F2U(ac[69]), F2U(ac[70]), F2U(ac[71]));
                            Log("hooks: ACCEPT w2s=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                                F2U(ac[88]), F2U(ac[89]), F2U(ac[90]), F2U(ac[91]),
                                F2U(ac[92]), F2U(ac[93]), F2U(ac[94]), F2U(ac[95]),
                                F2U(ac[96]), F2U(ac[97]), F2U(ac[98]), F2U(ac[99]),
                                F2U(ac[100]), F2U(ac[101]), F2U(ac[102]), F2U(ac[103]));
                            Log("hooks: ACCEPT c2s=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                                F2U(ac[108]), F2U(ac[109]), F2U(ac[110]), F2U(ac[111]),
                                F2U(ac[112]), F2U(ac[113]), F2U(ac[114]), F2U(ac[115]),
                                F2U(ac[116]), F2U(ac[117]), F2U(ac[118]), F2U(ac[119]),
                                F2U(ac[120]), F2U(ac[121]), F2U(ac[122]), F2U(ac[123]));
                            Log("hooks: ACCEPT vp =%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                                F2U(ac[124]), F2U(ac[125]), F2U(ac[126]), F2U(ac[127]),
                                F2U(ac[128]), F2U(ac[129]), F2U(ac[130]), F2U(ac[131]),
                                F2U(ac[132]), F2U(ac[133]), F2U(ac[134]), F2U(ac[135]),
                                F2U(ac[136]), F2U(ac[137]), F2U(ac[138]), F2U(ac[139]));
                            Log("hooks: ACCEPT proj=%08X %08X %08X %08X",
                                F2U(ac[172]), F2U(ac[173]), F2U(ac[174]), F2U(ac[175]));
                        }
                        StartFrame();
                        EnsureUpscalerInit();
                        if (g_dlaaMode)
                            ApplyCameraCbJitter(cb, numBytes, g_renderW, g_renderH,
                                                g_currJitter, g_prevJitter);
                        std::memcpy(g_lastPatchedCameraCb, cb, kCameraCbSize);
                        g_cameraCbValid = true;
                        g_lastCamPatchFrame = g_frameCounter;
                        static int s_patchLogs = 0;
                        ++s_patchLogs;
                        if (s_patchLogs <= 5 || (s_patchLogs % 1000) == 0)
                            Log("hooks: camera CB patched in place (dst %p srcOff %llu far %.1f pos %.1f %.1f %.1f w2s11 %.4f)",
                                (void*)dst, (unsigned long long)srcOffset, cb[173],
                                cb[0], cb[1], cb[2], cb[99]);
                    } else if (g_cameraRing == src) {
                        static int s_rejectDumps = 0;
                        if (s_rejectDumps < 5) {
                            ++s_rejectDumps;
                            const float* cf = cb;
                            Log("hooks: camera CB reject: f0..7=%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f "
                                "w2c15=%.2f w2s11=%.2f c2s7=%.2f vp11=%.2f vpp11=%.2f w2sp11=%.2f "
                                "proj=%.2f %.2f %.2f %.2f (srcOff %llu)",
                                cf[0], cf[1], cf[2], cf[3], cf[4], cf[5], cf[6], cf[7],
                                cf[71], cf[99], cf[115], cf[135], cf[307], cf[323],
                                cf[172], cf[173], cf[174], cf[175],
                                (unsigned long long)srcOffset);
                        Log("hooks: reject hex w2c=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                            F2U(cf[56]), F2U(cf[57]), F2U(cf[58]), F2U(cf[59]),
                            F2U(cf[60]), F2U(cf[61]), F2U(cf[62]), F2U(cf[63]),
                            F2U(cf[64]), F2U(cf[65]), F2U(cf[66]), F2U(cf[67]),
                            F2U(cf[68]), F2U(cf[69]), F2U(cf[70]), F2U(cf[71]));
                        Log("hooks: reject hex w2s=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                            F2U(cf[88]), F2U(cf[89]), F2U(cf[90]), F2U(cf[91]),
                            F2U(cf[92]), F2U(cf[93]), F2U(cf[94]), F2U(cf[95]),
                            F2U(cf[96]), F2U(cf[97]), F2U(cf[98]), F2U(cf[99]),
                            F2U(cf[100]), F2U(cf[101]), F2U(cf[102]), F2U(cf[103]));
                        Log("hooks: reject hex c2s=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                            F2U(cf[108]), F2U(cf[109]), F2U(cf[110]), F2U(cf[111]),
                            F2U(cf[112]), F2U(cf[113]), F2U(cf[114]), F2U(cf[115]),
                            F2U(cf[116]), F2U(cf[117]), F2U(cf[118]), F2U(cf[119]),
                            F2U(cf[120]), F2U(cf[121]), F2U(cf[122]), F2U(cf[123]));
                        Log("hooks: reject hex vp =%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                            F2U(cf[124]), F2U(cf[125]), F2U(cf[126]), F2U(cf[127]),
                            F2U(cf[128]), F2U(cf[129]), F2U(cf[130]), F2U(cf[131]),
                            F2U(cf[132]), F2U(cf[133]), F2U(cf[134]), F2U(cf[135]),
                            F2U(cf[136]), F2U(cf[137]), F2U(cf[138]), F2U(cf[139]));
                        Log("hooks: reject hex vpp=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                            F2U(cf[296]), F2U(cf[297]), F2U(cf[298]), F2U(cf[299]),
                            F2U(cf[300]), F2U(cf[301]), F2U(cf[302]), F2U(cf[303]),
                            F2U(cf[304]), F2U(cf[305]), F2U(cf[306]), F2U(cf[307]),
                            F2U(cf[308]), F2U(cf[309]), F2U(cf[310]), F2U(cf[311]));
                        Log("hooks: reject hex w2sp=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                            F2U(cf[312]), F2U(cf[313]), F2U(cf[314]), F2U(cf[315]),
                            F2U(cf[316]), F2U(cf[317]), F2U(cf[318]), F2U(cf[319]),
                            F2U(cf[320]), F2U(cf[321]), F2U(cf[322]), F2U(cf[323]),
                            F2U(cf[324]), F2U(cf[325]), F2U(cf[326]), F2U(cf[327]));
                        Log("hooks: reject hex proj=%08X %08X %08X %08X",
                            F2U(cf[172]), F2U(cf[173]), F2U(cf[174]), F2U(cf[175]));
                        }
                    }
                    src->Unmap(0, nullptr);
                }
            }
        } else if (numBytes == kVelocityCbSize && g_patchViewport && g_cameraCbValid && g_mvValid) {
            void* mapped = nullptr;
            if (SUCCEEDED(src->Map(0, nullptr, &mapped)) && mapped) {
                float* cb = (float*)((char*)mapped + srcOffset);
                if (ValidateVelocityCb(cb, numBytes, g_mvW, g_mvH)) {
                    PatchVelocityCb(cb, numBytes, g_lastPatchedCameraCb);
                    g_velocityCbPatched = true;
                    Log("hooks: velocity CB patched in place (dst %p srcOff %llu uTexSize %.4f %.4f)",
                        (void*)dst, (unsigned long long)srcOffset, cb[0], cb[1]);
                } else {
                    static int s_vRejects = 0;
                    if (s_vRejects < 5) {
                        ++s_vRejects;
                        bool texOk = (std::fabs(cb[0] * (float)g_mvW - 1.0f) < 0.02f) ||
                                     (std::fabs(cb[0] - (float)g_mvW) < 0.5f);
                        Log("hooks: velocity CB copy not validated (dst %p srcOff %llu uTexSize %.6f %.6f "
                            "texOk %d stw15 %.4f stw12..14 %.4f %.4f %.4f mv %ux%u)",
                            (void*)dst, (unsigned long long)srcOffset, cb[0], cb[1],
                            texOk ? 1 : 0, cb[19], cb[16], cb[17], cb[18], g_mvW, g_mvH);
                        if (s_vRejects <= 2) {
                            Log("hooks: velocity CB f0..15: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                                cb[0], cb[1], cb[2], cb[3], cb[4], cb[5], cb[6], cb[7],
                                cb[8], cb[9], cb[10], cb[11], cb[12], cb[13], cb[14], cb[15]);
                            Log("hooks: velocity CB f16..43: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                                cb[16], cb[17], cb[18], cb[19], cb[20], cb[21], cb[22], cb[23],
                                cb[24], cb[25], cb[26], cb[27], cb[28], cb[29], cb[30], cb[31],
                                cb[32], cb[33], cb[34], cb[35], cb[36], cb[37], cb[38], cb[39],
                                cb[40], cb[41], cb[42], cb[43]);
                        }
                    }
                }
                src->Unmap(0, nullptr);
            }
        }
    }
    if (Real_CopyBufferRegion)
        Real_CopyBufferRegion(list, dst, dstOffset, src, srcOffset, numBytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("hooks: CopyBufferRegion guarded (code %08X)", (unsigned)GetExceptionCode());
        if (Real_CopyBufferRegion)
            Real_CopyBufferRegion(list, dst, dstOffset, src, srcOffset, numBytes);
    }
}

void Hook_CopyTextureRegion(ID3D12GraphicsCommandList* list,
                            const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY,
                            UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src,
                            const D3D12_BOX* srcBox)
{
    __try {
    bool inject = false;
    bool injectBefore = false;
    if (dst && src && src->pResource != dst->pResource &&
        src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst->SubresourceIndex == 0 && src->SubresourceIndex == 0 &&
        srcBox && dstX == 0 && dstY == 0) {
        long w = srcBox->right - srcBox->left;
        long h = srcBox->bottom - srcBox->top;
        bool isMvDst = (dst->pResource == g_mvResource || dst->pResource == g_mvResourceAlt);
        bool isSceneSrc = (src->pResource == g_sceneColor ||
                           (g_sceneColorAlt && src->pResource == g_sceneColorAlt));
        if (g_displayW > 0 && !g_injectedThisFrame &&
            (unsigned long)w == g_displayW && (unsigned long)h == g_displayH) {
            // Post-reload fallback: if the scene color was never discovered
            // (plugin re-init after the game created its render targets), the
            // engine still copies the scene color at full-res every frame.
            // A display-sized UNORM src here is the scene color - adopt it.
            if (!g_sceneColorValid && !isMvDst) {
                D3D12_RESOURCE_DESC sd = src->pResource->GetDesc();
                if (sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    sd.MipLevels == 1 &&
                    sd.Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
                    g_sceneColor = src->pResource;
                    g_sceneColorValid = true;
                    g_resourceStates[g_sceneColor] = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    AdoptDisplaySize((unsigned int)sd.Width, (unsigned int)sd.Height);
                    Log("hooks: scene color adopted from copy source %p", (void*)g_sceneColor);
                }
            }
            isSceneSrc = (src->pResource == g_sceneColor ||
                          (g_sceneColorAlt && src->pResource == g_sceneColorAlt));
            if (isSceneSrc && g_patchAborted) {
                g_patchAborted = false;
                Log("hooks: viewport patch re-armed by scene copy");
            }
            static int s_fullresLogs = 0;
            ++s_fullresLogs;
            if (s_fullresLogs <= 25 || (s_fullresLogs % 500) == 0) {
                D3D12_RESOURCE_DESC sd = src->pResource->GetDesc();
                Log("hooks: full-res copy dst %p mv=%d scene=%d w %ld h %ld patchVp %d depth %d mvV %d (src fmt %u %ux%u)",
                    (void*)dst->pResource, isMvDst ? 1 : 0, isSceneSrc ? 1 : 0, w, h,
                    g_patchAppliedThisFrame ? 1 : 0, g_depthValid ? 1 : 0, g_mvValid ? 1 : 0,
                    (unsigned int)sd.Format, (unsigned int)sd.Width, (unsigned int)sd.Height);
            }
            // PRIMARY TRIGGER: the engine copies the (low-res rendered) scene color at
            // display size every frame - this is the composite step. Replace it with the
            // DLSS upscaled image: run DLSS first, then let the engine's copy proceed
            // (it will now copy the full-res content).
            if (isSceneSrc && dst->pResource != g_dlssOut &&
                (g_patchViewport || g_dlaaMode) && g_mvValid &&
                (g_patchAppliedThisFrame || g_dlaaMode) && g_depthValid) {
                EnsureUpscalerInit();
                if (g_upscaler && g_upscaler->IsReady()) {
                    inject = true;
                    injectBefore = true;
                } else {
                    static int s_sceneSkips = 0;
                    ++s_sceneSkips;
                    if (s_sceneSkips <= 10)
                        Log("hooks: scene-copy DLSS not ready (init %d)",
                            g_upscaler ? 1 : 0);
                }
            }
            // Depth candidate heuristic (copies NOT involving the scene color or MV).
            if (!isSceneSrc && !isMvDst && dst->pResource != g_dlssOut) {
                g_depthResource = dst->pResource;
                g_depthValid = true;
                g_depthStamp = g_frameCounter;
                auto it = g_resourceStates.find(dst->pResource);
                if (it == g_resourceStates.end())
                    g_resourceStates[dst->pResource] = D3D12_RESOURCE_STATE_COPY_DEST;
                static int s_depthCandidates = 0;
                if (s_depthCandidates < 8) {
                    ++s_depthCandidates;
                    Log("hooks: depth candidate %p (full-res copy)", (void*)dst->pResource);
                }
            }
        }
        // FALLBACK trigger: full-res copy into the MV resource (pool re-fill events).
        // Almost never usable (patchApplied is false in that context); kept as a safety.
        if (g_mvValid && isMvDst &&
            (unsigned long)w == g_mvW && (unsigned long)h == g_mvH &&
            g_patchViewport && !g_injectedThisFrame) {
            if (g_patchAppliedThisFrame && g_depthValid &&
                g_upscaler && g_upscaler->IsReady()) {
                inject = true;
            } else {
                static int s_injSkips = 0;
                ++s_injSkips;
                if (s_injSkips <= 10 || (s_injSkips % 1000) == 0)
                    Log("hooks: injection skipped dst %p (viewport patch %d, depth %d, dlss %d)",
                        (void*)dst->pResource,
                        g_patchAppliedThisFrame ? 1 : 0, g_depthValid ? 1 : 0,
                        (g_upscaler && g_upscaler->IsReady()) ? 1 : 0);
            }
        }
    }
    if (inject && injectBefore) {
        // DLSS first, then the engine's copy picks up the full-res scene color.
        DoInjection(list);
        if (g_activeSceneColor)
            Barrier(list, g_activeSceneColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (Real_CopyTextureRegion)
        Real_CopyTextureRegion(list, dst, dstX, dstY, dstZ, src, srcBox);
    if (inject && !injectBefore)
        DoInjection(list);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("hooks: CopyTextureRegion guarded (code %08X)", (unsigned)GetExceptionCode());
        if (Real_CopyTextureRegion)
            Real_CopyTextureRegion(list, dst, dstX, dstY, dstZ, src, srcBox);
    }
}

void Hook_RSSetViewports(ID3D12GraphicsCommandList* list, UINT numViewports,
                         const D3D12_VIEWPORT* pViewports)
{
    ID3D12Resource* boundScene = SceneColorBound();
    static int s_vpDiag = 0;
    static int s_sceneVpDiag = 0;
    if (g_patchViewport && numViewports >= 1 && pViewports) {
        if (boundScene) {
            ++s_sceneVpDiag;
            if (s_sceneVpDiag <= 40 || (s_sceneVpDiag % 500) == 0)
                Log("hooks: vp diag %dx%d scene=%p ready=%d mvValid=%d (scene render viewport)",
                    (int)pViewports[0].Width, (int)pViewports[0].Height, (void*)boundScene,
                    (g_upscaler && g_upscaler->IsReady()) ? 1 : 0, g_mvValid ? 1 : 0);
        } else if (s_vpDiag < 30) {
            ++s_vpDiag;
            Log("hooks: vp diag %dx%d boundScene=%p ready=%d mvValid=%d rtvValid=%d",
                (int)pViewports[0].Width, (int)pViewports[0].Height, (void*)boundScene,
                (g_upscaler && g_upscaler->IsReady()) ? 1 : 0, g_mvValid ? 1 : 0,
                g_boundRtvValid ? 1 : 0);
        }
    }
    if (numViewports >= 1 && pViewports && g_patchViewport && boundScene) {
        int vw = (int)pViewports[0].Width;
        int vh = (int)pViewports[0].Height;
        if (vw >= 1000 && vh >= 500 && (vw != (int)g_displayW || vh != (int)g_displayH))
            AdoptDisplaySize((unsigned int)vw, (unsigned int)vh);
    }
    if (numViewports >= 1 && pViewports && g_patchViewport && boundScene &&
        g_upscaler && g_upscaler->IsReady() &&
        (int)pViewports[0].Width == (int)g_displayW &&
        (int)pViewports[0].Height == (int)g_displayH) {
        D3D12_VIEWPORT v = pViewports[0];
        v.Width = (float)g_renderW;
        v.Height = (float)g_renderH;
        Real_RSSetViewports(list, 1, &v);
        g_activeSceneColor = boundScene;
        g_patchAppliedThisFrame = true;
        Log("hooks: viewport patched to %ux%u (scene %p)", g_renderW, g_renderH, (void*)boundScene);
        ++g_patchFramesWithoutInject;
        if (g_patchFramesWithoutInject > 1800) {
            g_patchAborted = true;
            g_patchViewport = false;
            Log("hooks: viewport patch aborted - no injection within %u frames", g_patchFramesWithoutInject);
        }
        return;
    }
    Real_RSSetViewports(list, numViewports, pViewports);
}

void Hook_RSSetScissorRects(ID3D12GraphicsCommandList* list, UINT numRects,
                            const D3D12_RECT* pRects)
{
    if (numRects >= 1 && pRects && g_patchViewport && SceneColorBound() &&
        (pRects[0].right - pRects[0].left) == (LONG)g_displayW &&
        (pRects[0].bottom - pRects[0].top) == (LONG)g_displayH) {
        D3D12_RECT r = pRects[0];
        r.right = r.left + (LONG)g_renderW;
        r.bottom = r.top + (LONG)g_renderH;
        Real_RSSetScissorRects(list, 1, &r);
        return;
    }
    Real_RSSetScissorRects(list, numRects, pRects);
}

void Hook_ResourceBarrier(ID3D12GraphicsCommandList* list, UINT numBarriers,
                          const D3D12_RESOURCE_BARRIER* pBarriers)
{
    if (pBarriers && numBarriers > 0) {
        for (UINT i = 0; i < numBarriers; ++i) {
            if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                ID3D12Resource* res = pBarriers[i].Transition.pResource;
                if (res)
                    g_resourceStates[res] = pBarriers[i].Transition.StateAfter;
            }
        }
    }
    Real_ResourceBarrier(list, numBarriers, pBarriers);
}

void Hook_SetDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT numHeaps,
                             ID3D12DescriptorHeap* const* heaps)
{
    g_setHeapCount = numHeaps > 2 ? 2 : numHeaps;
    for (UINT i = 0; i < g_setHeapCount; ++i)
        g_setHeaps[i] = heaps[i];
    Real_SetDescriptorHeaps(list, numHeaps, heaps);
}

void Hook_OMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT numRenderTargets,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargets,
                             BOOL RTsSingleHandleToDescriptorRange,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    if (numRenderTargets >= 1 && pRenderTargets) {
        g_boundRtv = pRenderTargets[0];
        g_boundRtvValid = true;
        g_boundRtvResource = nullptr;
        auto it = g_rtvMap.find(pRenderTargets[0].ptr);
        if (it != g_rtvMap.end()) {
            g_boundRtvResource = it->second;
            // Dynamic adoption: if a display-sized UNORM target gets bound as
            // an RTV and we have never seen it as the scene color, remember it.
            // Covers renderer re-inits that re-create views after our hook
            // (or even the whole plugin) was installed.
            if (!g_sceneColorValid && g_boundRtvResource) {
                D3D12_RESOURCE_DESC rd = g_boundRtvResource->GetDesc();
                if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1 &&
                    rd.Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
                    g_sceneColor = g_boundRtvResource;
                    g_sceneColorRtv = pRenderTargets[0];
                    g_sceneColorValid = true;
                    AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
                    g_resourceStates[g_sceneColor] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    Log("hooks: scene color adopted from RTV bind %p (%ux%u)", (void*)g_sceneColor,
                        (unsigned int)rd.Width, (unsigned int)rd.Height);
                }
            }
            // The engine re-creates its scene target from time to time but keeps
            // reusing the same CPU descriptor slot. Refresh the tracked scene
            // color to the CURRENT resource bound at that slot, otherwise we
            // keep injecting with a stale (freed/recycled) resource.
            if (g_sceneColorValid && g_boundRtvResource &&
                g_boundRtv.ptr == g_sceneColorRtv.ptr &&
                g_boundRtvResource != g_sceneColor) {
                g_sceneColor = g_boundRtvResource;
                g_resourceStates[g_sceneColor] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                Log("hooks: scene color refreshed on bind %p", (void*)g_sceneColor);
                D3D12_RESOURCE_DESC rd = g_sceneColor->GetDesc();
                if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    rd.Width >= 1000 && rd.Height >= 500)
                    AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
            }
            if (g_sceneColorAlt && g_boundRtvResource &&
                g_boundRtv.ptr == g_sceneColorRtvAlt.ptr &&
                g_boundRtvResource != g_sceneColorAlt) {
                g_sceneColorAlt = g_boundRtvResource;
                Log("hooks: scene color ALT refreshed on bind %p", (void*)g_sceneColorAlt);
                // Scene-target churn = renderer re-init / map load: the old
                // MV/depth pointers are almost certainly dead now. Reset the
                // stamps so injection waits for fresh discoveries instead of
                // dereferencing freed resources.
                g_mvStamp = 0;
                g_depthStamp = 0;
            }
        }
        if (g_sceneColorValid && SceneColorBound()) {
            static int s_rtvDiag = 0;
            ++s_rtvDiag;
            if (s_rtvDiag <= 20 || (s_rtvDiag % 1000) == 0)
                Log("hooks: scene RTV bound via OMSetRenderTargets (%u RTs) res=%p",
                    numRenderTargets, (void*)g_boundRtvResource);
        }
    } else {
        g_boundRtvValid = false;
        g_boundRtvResource = nullptr;
    }
    Real_OMSetRenderTargets(list, numRenderTargets, pRenderTargets,
                            RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
}

HRESULT WINAPI Hook_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minLevel,
                                      REFIID riid, void** ppDevice)
{
    static int s_createCalls = 0;
    if (s_creatingBridge) {
        // Bridge device creation: pure passthrough. Do NOT touch g_device,
        // g_graphicsQueue, or reinstall vtable hooks - those must stay bound
        // to the GAME's device.
        return Real_D3D12CreateDevice_Tramp(adapter, minLevel, riid, ppDevice);
    }
    if (s_createCalls < 5) {
        ++s_createCalls;
        Log("hooks: D3D12CreateDevice called #%d (g_device=%p)", s_createCalls, (void*)g_device);
    }
    HRESULT hr = Real_D3D12CreateDevice_Tramp(adapter, minLevel, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        if (!g_adapter && adapter) {
            adapter->QueryInterface(__uuidof(IDXGIAdapter), (void**)&g_adapter);
            Log("hooks: adapter %p captured (for factory lookup)", (void*)g_adapter);
        }
        // The game creates MULTIPLE devices (probe + real). Track the LATEST:
        // the first one may be a throwaway, and NGX needs the device that
        // actually owns the render resources. QI-test each for DXGI interop
        // (NGX requires it; a device without IDXGIDevice gives PlatformError).
        ID3D12Device* newDev = (ID3D12Device*)*ppDevice;
        {
            IDXGIDevice* dxgidev = nullptr;
            HRESULT qhr = newDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgidev);
            Log("hooks: device %p created (QI IDXGIDevice hr=0x%08X)",
                (void*)newDev, (unsigned)qhr);
            if (dxgidev) dxgidev->Release();
        }
        bool reinstallHooks = (g_device != newDev);
        g_device = newDev;
        if (reinstallHooks) {
            // All ID3D12Device instances share one vtable; MinHook reports
            // ALREADY_CREATED on repeats and leaves the trampolines intact.
            void** vtbl = *(void***)g_device;
            MH_STATUS s20 = MH_CreateHook(vtbl[20], &Hook_CreateRenderTargetView, (void**)&Real_CreateRenderTargetView);
            MH_STATUS s18 = MH_CreateHook(vtbl[18], &Hook_CreateShaderResourceView, (void**)&Real_CreateShaderResourceView);
            MH_EnableHook(vtbl[20]);
            MH_EnableHook(vtbl[18]);
            {
                void* targets[2] = { (void*)Hook_CreateRenderTargetView, (void*)Hook_CreateShaderResourceView };
                CfgMarkValid(targets, 2);
            }
            Log("hooks: device vtable hooks (20:%d 18:%d)", (int)s20, (int)s18);
        }

        ID3D12CommandQueue* queue = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue) {
            void** qv = *(void***)queue;
            if (MH_CreateHook(qv[10], &Hook_ExecuteCommandLists, (void**)&Real_ExecuteCommandLists) == MH_OK) {
                MH_EnableHook(qv[10]);
                {
                    void* targets[1] = { (void*)Hook_ExecuteCommandLists };
                    CfgMarkValid(targets, 1);
                }
                Log("hooks: queue slot 10 hooked");
            } else {
                Log("hooks: queue slot 10 already hooked (shared vtable)");
            }
        }
        if (queue) g_graphicsQueue = queue;
        EnsureGlobalSwapchainHook();
    }
    return hr;
}

void InstallCommandListHooks(ID3D12GraphicsCommandList* list)
{
    Log("hooks: installing cmdlist hooks on %p", (void*)list);
    void** vtbl = *(void***)list;
    MH_CreateHook(vtbl[15], &Hook_CopyBufferRegion, (void**)&Real_CopyBufferRegion);
    MH_CreateHook(vtbl[16], &Hook_CopyTextureRegion, (void**)&Real_CopyTextureRegion);
    MH_CreateHook(vtbl[21], &Hook_RSSetViewports, (void**)&Real_RSSetViewports);
    MH_CreateHook(vtbl[22], &Hook_RSSetScissorRects, (void**)&Real_RSSetScissorRects);
    MH_CreateHook(vtbl[26], &Hook_ResourceBarrier, (void**)&Real_ResourceBarrier);
    MH_CreateHook(vtbl[28], &Hook_SetDescriptorHeaps, (void**)&Real_SetDescriptorHeaps);
    MH_CreateHook(vtbl[46], &Hook_OMSetRenderTargets, (void**)&Real_OMSetRenderTargets);
    MH_EnableHook(vtbl[15]);
    MH_EnableHook(vtbl[16]);
    MH_EnableHook(vtbl[21]);
    MH_EnableHook(vtbl[22]);
    MH_EnableHook(vtbl[26]);
    MH_EnableHook(vtbl[28]);
    MH_EnableHook(vtbl[46]);
    {
        void* targets[7] = { (void*)Hook_CopyBufferRegion, (void*)Hook_CopyTextureRegion,
                             (void*)Hook_RSSetViewports, (void*)Hook_RSSetScissorRects,
                             (void*)Hook_ResourceBarrier, (void*)Hook_SetDescriptorHeaps,
                             (void*)Hook_OMSetRenderTargets };
        CfgMarkValid(targets, 7);
    }
    Log("hooks: command list slots 15/16/21/22/26/28/46 hooked");
}

} // namespace

void HooksSetConfig(const ScaleNgConfig& config)
{
    g_cfg = config;
    g_cfgSet = true;
    g_dlaaMode = config.dlaa;
    g_hudIniOn = config.hud;
    g_legacyScale = config.legacyScale;
    g_passiveMode = config.passive;
    Log("hooks: config applied (dlaa=%d)", g_dlaaMode ? 1 : 0);
}

void HooksGetDescriptorHeaps(UINT* count, ID3D12DescriptorHeap** heaps)
{
    if (!count) return;
    *count = g_setHeapCount;
    if (heaps) {
        for (UINT i = 0; i < g_setHeapCount; ++i)
            heaps[i] = g_setHeaps[i];
    }
}

void HooksRestoreDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT count,
                                 ID3D12DescriptorHeap* const* heaps)
{
    if (list && Real_SetDescriptorHeaps)
        Real_SetDescriptorHeaps(list, count, heaps);
}

void HooksInstallCreateDeviceDetour()
{
    if (!g_cfgSet) {
        Log("hooks: install called before config - ignoring");
        return;
    }
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    if (!d3d12) {
        Log("hooks: d3d12.dll not loaded yet - ScaleNG inactive");
        return;
    }
    void* pCreateDevice = (void*)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!pCreateDevice) {
        Log("hooks: D3D12CreateDevice export not found - ScaleNG inactive");
        return;
    }
    unsigned char pre[16] = {};
    memcpy(pre, pCreateDevice, 16);
    Log("hooks: D3D12CreateDevice first bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
        pre[0], pre[1], pre[2], pre[3], pre[4], pre[5], pre[6], pre[7],
        pre[8], pre[9], pre[10], pre[11], pre[12], pre[13], pre[14], pre[15]);
    if (MH_Initialize() != MH_OK) {
        Log("hooks: MinHook initialize failed - ScaleNG inactive");
        return;
    }
    MH_STATUS st = MH_CreateHook(pCreateDevice, &Hook_D3D12CreateDevice, (void**)&Real_D3D12CreateDevice_Tramp);
    if (st != MH_OK) {
        Log("hooks: D3D12CreateDevice hook failed (%d) - ScaleNG inactive", (int)st);
        return;
    }
    if (MH_EnableHook(pCreateDevice) != MH_OK) {
        Log("hooks: D3D12CreateDevice enable failed - ScaleNG inactive");
        return;
    }
    Log("hooks: D3D12CreateDevice detour installed");

    // DXGI factory detours to capture the swapchain for Present-time injection.
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) {
        Log("hooks: dxgi.dll not loaded yet - swapchain injection inactive");
        return;
    }
    void* pF2 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory2");
    if (pF2) {
        if (MH_CreateHook(pF2, &Hook_CreateDXGIFactory2, (void**)&Real_CreateDXGIFactory2) == MH_OK &&
            MH_EnableHook(pF2) == MH_OK)
            Log("hooks: CreateDXGIFactory2 detour installed");
        else
            Log("hooks: CreateDXGIFactory2 hook failed");
    }
    void* pF1 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (pF1) {
        if (MH_CreateHook(pF1, &Hook_CreateDXGIFactory1, (void**)&Real_CreateDXGIFactory1) == MH_OK &&
            MH_EnableHook(pF1) == MH_OK)
            Log("hooks: CreateDXGIFactory1 detour installed");
        else
            Log("hooks: CreateDXGIFactory1 hook failed");
    }
    void* pF0 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory");
    if (pF0) {
        if (MH_CreateHook(pF0, &Hook_CreateDXGIFactory, (void**)&Real_CreateDXGIFactory) == MH_OK &&
            MH_EnableHook(pF0) == MH_OK)
            Log("hooks: CreateDXGIFactory detour installed");
        else
            Log("hooks: CreateDXGIFactory hook failed");
    }
}
