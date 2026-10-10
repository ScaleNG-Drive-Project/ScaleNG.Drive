#define NOMINMAX
#include "d3d12_hooks.h"
#include <tlhelp32.h>
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

void EnsureUpscalerInit(bool bypassQuietGate = false);
static bool s_creatingBridge = false;   // true while EnsureBridge creates its device
unsigned g_mvFirstValidFrame = 0;
unsigned g_depthFirstValidFrame = 0;
DXGI_FORMAT g_depthRealFmt = DXGI_FORMAT_UNKNOWN; // engine's actual depth format
bool g_depthMsaa = false;


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

volatile LONG g_smokeBusy = 0; // nonzero while smoke test owns the driver
volatile LONG g_presentSelfTestFired = 0;
IDXGISwapChain* g_egshDummySC = nullptr; // never adopt/pipeline this one
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
// A raw COM pointer can be recycled after its prior resource is released.
// Keep a bounded address-generation table so the old weak slot becomes stale
// on collision and only a later observed view/adoption can refresh its token.
// No COM references are retained. Overflow fails closed for engine inputs.
struct ResourceAddressGeneration { ID3D12Resource* resource; LONG64 generation; };
static ResourceAddressGeneration g_resourceAddressGenerations[256] = {};
static SRWLOCK g_resourceAddressGenerationLock = SRWLOCK_INIT;
static volatile LONG g_resourceAddressGenerationOverflow = 0;
static volatile LONG g_sceneAddressReuseDetected = 0;
static volatile LONG64 g_mvResourceGeneration = 0;
static volatile LONG64 g_mvResourceAltGeneration = 0;
static volatile LONG64 g_depthResourceGeneration = 0;
static volatile LONG64 g_sceneColorGeneration = 0;
static volatile LONG64 g_sceneColorAltGeneration = 0;
// True when the depth slot holds an SRV-sourced (depth-view) adoption rather
// than copy-heuristic guesswork. Copies must not overwrite real depth.
static bool g_depthSrvSourced = false;

// Frame stamps: when depth/MV were last (re)discovered. Feeding NGX a freed
// resource = InvalidParameter storms + driver instability, so injection
// requires fresh discoveries only.
unsigned int g_depthStamp = 0;
unsigned int g_mvStamp = 0;

// Present-serial stamps: last observed barrier or output-merger bind for each
// candidate. These are only recency hints: command recording can precede
// execution, and a bind does not prove a draw wrote the resource or that the
// contents correspond to the color frame submitted to NGX.
// g_frameCounter (camera clock) only advances when the engine patches
// camera CBs — it freezes during steady state, making the frame-counter
// age gate (10/20000 frames) certify week-old resources as "fresh".
// g_presentSerial advances on every Present, so the age bounds when a covered
// callback was observed. It does not prove the recorded list executed, that a
// draw wrote the resource, or that its contents align with the evaluated color.
// Zero = no observation since adoption (fail closed on first eval).
static unsigned long long g_mvLastTouchPresent = 0;
static unsigned long long g_depthLastTouchPresent = 0;
// Diagnostic-only per-interval touch-rate counters (observability). Incremented
// lock-free at the existing touch sites below; exchanged-to-zero by the sampled
// topo snapshot. Interval zeros vs nonzeros separate "engine produces nothing
// the hooks can see" from "produces but gates reject". Never affect gates.
static volatile LONG64 g_diagMvOm = 0, g_diagMvBar = 0, g_diagDepthDsv = 0, g_diagDepthBar = 0;
// Diagnostic-only per-resource touch records. A nonblocking writer lock keeps
// each pointer/generation/serial/source tuple coherent across recording threads;
// contended samples are dropped and counted. These records never affect gates.
struct ResourceTouchLedger {
    volatile LONG writer;
    ID3D12Resource* resource;
    LONG64 generation;
    LONG64 present;
    LONG64 ecl;
    LONG source;
    volatile LONG64 dropped;
};
static ResourceTouchLedger g_mvPrimaryTouchLedger = {};
static ResourceTouchLedger g_mvAltTouchLedger = {};
static ResourceTouchLedger g_depthTouchLedger = {};
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
DXGI_FORMAT g_brDepthFmt = DXGI_FORMAT_UNKNOWN;
bool g_brDepthFmtSet = false;
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
volatile LONG g_upscalerInitAttempted = 0;
IUpscaler* g_upscaler = nullptr;
bool g_evalDidBridge = false;
static unsigned g_brW = 0, g_brH = 0;
static DXGI_FORMAT g_brFmt = DXGI_FORMAT_UNKNOWN;
unsigned int g_lastCamPatchFrame = 0;

// Create/refresh the cross-device bridge for the given size+format.
ID3D12Fence* g_gameFence = nullptr; // game-device view of bridge shared fence
static bool EnsureBridge(unsigned W, unsigned H, DXGI_FORMAT fmt, ID3D12Device* gameDev)
{
    // Zero dims = display not yet adopted (hysteresis needs ~15 stable frames).
    // Creating 0-sized shared textures is E_INVALIDARG - retry later instead.
    if (W == 0 || H == 0) return false;
    if (fmt == DXGI_FORMAT_UNKNOWN) return false;

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
        // Log BRIDGE adapter identity alongside the game's (hybrid triage).
        {
            IDXGIDevice* bdxgi = nullptr;
            if (SUCCEEDED(g_bridgeDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&bdxgi))) {
                IDXGIAdapter* bad = nullptr;
                if (SUCCEEDED(bdxgi->GetAdapter(&bad))) {
                    DXGI_ADAPTER_DESC bdesc = {};
                    if (SUCCEEDED(bad->GetDesc(&bdesc))) {
                        Log("bridge: BRIDGE device adapter VendorId=0x%04X '%ls' LUID=%08X:%08X",
                            bdesc.VendorId, bdesc.Description,
                            (unsigned)bdesc.AdapterLuid.HighPart, (unsigned)bdesc.AdapterLuid.LowPart);
                    }
                    bad->Release();
                }
                bdxgi->Release();
            }
        }
        // ROOT-CAUSE TOOL: force DRED auto-breadcrumbs + page-fault reporting
        // so any device removal names its exact faulting operation instead of
        // leaving us inferring from log correlation.
        {
            typedef HRESULT(WINAPI* PFN_D12Dbg)(const IID&, void**);
            PFN_D12Dbg getDbg = (PFN_D12Dbg)(void*)GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12GetDebugInterface");
            if (getDbg) {
                void* dredSet = nullptr;
                if (SUCCEEDED(getDbg(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), &dredSet))) {
                    auto* ds = (ID3D12DeviceRemovedExtendedDataSettings*)dredSet;
                    ds->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    ds->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    ds->Release();
                    Log("bridge: DRED breadcrumbs+pagefault FORCED ON");
                } else {
                    Log("bridge: DRED settings unavailable (older runtime?)");
                }
            }
        }
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
        // CreateSharedHandle is a COM method on ID3D12Device - NOT a d3d12.dll
        // export (GetProcAddress returns NULL for it, which silently disabled
        // the whole shared-fence path and forced illegal cross-device signals).
        if (g_bridgeFence) {
            HANDLE hf = nullptr;
            HRESULT shr = g_bridgeDev->CreateSharedHandle(g_bridgeFence, nullptr, GENERIC_ALL, nullptr, &hf);
            if (SUCCEEDED(shr)) g_bridgeFenceShared = hf;
            else Log("bridge: fence CreateSharedHandle failed hr=0x%08X", (unsigned)shr);
        }
        g_bridgeFenceEv = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        // Open the game-device view of the shared fence NOW - the game queue
        // must never Signal/Wait the bridge-device fence instance directly.
        if (gameDev && g_bridgeFenceShared)
            gameDev->OpenSharedHandle(g_bridgeFenceShared, IID_PPV_ARGS(&g_gameFence));
        Log("bridge: our device/queue/fence ready (gameFence=%p)", (void*)g_gameFence);
    }

    auto mkShared = [&](ID3D12Resource** ours, HANDLE* hout, ID3D12Resource** theirs,
                        UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl) -> bool {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = f; d.SampleDesc.Count = 1;
        d.Flags = fl | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
                    | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Log("bridge: mkShared %ux%u fmt=%u baseFlags=%X - probing combinations...", (unsigned)w, (unsigned)h,
            (unsigned)f, (unsigned)fl);
        // ARGUMENT-SPACE PROBE: E_INVALIDARG without naming which constraint.
        // Try descending permissiveness; each attempt logged with its hr so
        // the exact failing requirement identifies itself.
        struct Combo { D3D12_RESOURCE_FLAGS rf; D3D12_HEAP_FLAGS hf; const char* name; };
        const Combo combos[] = {
            { fl | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
              D3D12_HEAP_FLAG_SHARED, "SHARED+SIMUL" },
            { fl | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER,
              D3D12_HEAP_FLAG_SHARED, "SHARED+CROSS+SIMUL" },
            { fl | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
              D3D12_HEAP_FLAG_NONE, "NOSHARE+SIMUL" },
        };
        HRESULT lastHr = E_FAIL;
        bool made = false;
        for (const auto& c : combos) {
            d.Flags = c.rf;
            HRESULT chr = g_bridgeDev->CreateCommittedResource(&hp, c.hf, &d,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(ours));
            Log("bridge: mkShared try %-18s heapFlag=%llX resFlags=%X hr=0x%08X",
                c.name, (unsigned long long)c.hf, (unsigned)c.rf, (unsigned)chr);
            if (SUCCEEDED(chr)) {
                lastHr = chr; made = true;
                HRESULT drr = g_bridgeDev->GetDeviceRemovedReason();
                Log("bridge: mkShared SUCCESS via %s (devRemoved=0x%08X)", c.name, (unsigned)drr);
                break;
            }
            lastHr = chr;
        }
        if (!made) {
            HRESULT drr = g_bridgeDev->GetDeviceRemovedReason();
            Log("bridge: mkShared ALL combos failed (last hr=0x%08X, devRemoved=0x%08X)",
                (unsigned)lastHr, (unsigned)drr);
            return false;
        }
        Log("bridge: mkShared resource ok %p - CreateSharedHandle...", (void*)*ours);
        if (FAILED(g_bridgeDev->CreateSharedHandle(*ours, nullptr, GENERIC_ALL, nullptr, hout))) {
            Log("bridge: mkShared CreateSharedHandle FAILED");
            return false;
        }
        Log("bridge: mkShared handle ok - OpenSharedHandle on game device...");
        HRESULT ohr = gameDev->OpenSharedHandle(*hout, IID_PPV_ARGS(theirs));
        Log("bridge: mkShared OpenSharedHandle hr=0x%08X", (unsigned)ohr);
        return SUCCEEDED(ohr);
    };

    bool ok = true;
    ok &= mkShared(&g_brColor, &g_hColor, &g_gameColor, W, H, fmt, D3D12_RESOURCE_FLAG_NONE);
    DXGI_FORMAT depthFmt = (g_depthRealFmt != DXGI_FORMAT_UNKNOWN) ? g_depthRealFmt : DXGI_FORMAT_R32_FLOAT;
    if (g_brDepthFmtSet && g_brDepthFmt != depthFmt) {
        g_bridgeReady = false;
        for (auto** p : { &g_gameColor, &g_gameDepth, &g_gameMv, &g_gameOut })
            if (*p) { (*p)->Release(); *p = nullptr; }
        for (auto** p : { &g_brColor, &g_brDepth, &g_brMv, &g_brOut })
            if (*p) { (*p)->Release(); *p = nullptr; }
        for (auto** h : { &g_hColor, &g_hDepth, &g_hMv, &g_hOut })
            if (*h) { CloseHandle(*h); *h = nullptr; }
        Log("bridge: depth format changed to %d - rebuilding shared", (int)depthFmt);
    }
    ok &= mkShared(&g_brDepth, &g_hDepth, &g_gameDepth, W, H, depthFmt, D3D12_RESOURCE_FLAG_NONE);
    g_brDepthFmt = depthFmt; g_brDepthFmtSet = true;
    ok &= mkShared(&g_brMv, &g_hMv, &g_gameMv, W, H, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    ok &= mkShared(&g_brOut, &g_hOut, &g_gameOut, W, H, fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!ok) {
        // Identify WHICH shared resource failed and why (cross-adapter
        // OpenSharedHandle/CreateSharedHandle failures look exactly like this).
        Log("bridge: shared resource creation failed (brColor=%p hColor=%p gameColor=%p brDepth=%p brMv=%p brOut=%p)",
            (void*)g_brColor, (void*)g_hColor, (void*)g_gameColor,
            (void*)g_brDepth, (void*)g_brMv, (void*)g_brOut);
        return false;
    }

    g_brW = W; g_brH = H; g_brFmt = fmt;
    g_bridgeReady = true;
    // Re-arm NGX init: it may have bound the wrapped game device before the
    // bridge existed. Next EnsureUpscalerInit re-runs on OUR clean device.
    InterlockedExchange(&g_upscalerInitAttempted, 0);
    Log("bridge: ready %ux%u fmt=%d (NGX re-bind armed)", W, H, (int)fmt);
    return true;
}


// Which scene color was actually rendered this frame (set by the viewport patch).
ID3D12Resource* g_activeSceneColor = nullptr;

ID3D12Resource* g_dlssOut = nullptr;
bool g_dlssOutValid = false;

unsigned int g_renderW = 0;
unsigned int g_renderH = 0;

Jitter2D g_currJitter = { 0.0f, 0.0f };
Jitter2D g_prevJitter = { 0.0f, 0.0f };
unsigned int g_frameCounter = 0;
static volatile LONG64 g_eclSerial = 0;
static volatile LONG64 g_presentSerial = 0;
// Renderer-transition quarantine: when the scene-color identity changes the
// engine is rebuilding its render graph - resource identities churn and
// adopting new candidates here races teardown (deterministic engine-side AV).
// All adoption + DLAA freezes until this frame.
volatile unsigned g_quietUntilFrame = 0;
// LOAD-PHASE SILENCE: during map load our hooks do NOTHING but forward -
// no GetDesc discovery, zero file logging - because even microsecond-scale
// perturbation on engine threads flips a timing coin-flip at the render-
// graph teardown (deterministic exe+0xD02EDA AV when lost). Armed once
// gameplay evidence exists (camera CB + MV + depth all seen).
volatile LONG g_loadPhase = 1;
volatile unsigned g_lastSceneChangeFrame = 0;
volatile unsigned g_lastDiscoveryChangeFrame = 0; // any tracked input swap
unsigned g_lastDlaaFrame = 0; // per-frame DLAA flow cap
volatile LONG g_settledOnce = 0; // session latch: all heavy init deferred until set (file-scope: set at submit, checked in gates)
bool g_frameStarted = false;
bool g_patchViewport = false;
bool g_patchAppliedThisFrame = false;
bool g_injectedThisFrame = false;

ID3D12Resource* g_grave[4] = {};
int g_graveN = 0;

// Legacy ownership scaffolding. Current engine-resource candidate slots and
// maps store weak pointers; they do not AddRef or own observed resources.
// CreatedRef_Put and the g_owned helpers below currently have no call sites.
// Do not infer resource lifetime from address-generation bookkeeping or from
// these unused helpers; ownership changes need a bounded acquire/release
// design and evidence that acquiring at the observation point is safe.
ID3D12Resource* g_createdRefs[256] = {};
int g_createdN = 0;
// Slots currently owning a transferred ref (graveyard/replace decisions).
ID3D12Resource* g_owned[16] = {};
int g_ownedN = 0;
// Composite-source persistence counters (scene-color promotion proof).
std::map<void*, int> g_copySrcCount;

static bool Owned_Remove(ID3D12Resource* res)
{
    for (int i = 0; i < g_ownedN; ++i)
        if (g_owned[i] == res) { g_owned[i] = g_owned[--g_ownedN]; return true; }
    return false;
}
static void Owned_Add(ID3D12Resource* res)
{
    if (g_ownedN < 16) g_owned[g_ownedN++] = res;
}

void CreatedRef_Put(ID3D12Resource* res)
{
    if (!res) return;
    for (int i = 0; i < g_createdN; ++i)
        if (g_createdRefs[i] == res) return; // already held
    if (g_createdN >= 256) return;           // table full: stay weak
    res->AddRef();
    g_createdRefs[g_createdN++] = res;
}

// Pure weak pointer swap - no transition bookkeeping (ping-pong path).
void StoreTracked_Weak(ID3D12Resource** slot, ID3D12Resource* res)
{
    *slot = res;
}

// Thread-safety lock for g_copySrcCount (see comment at definition site below).
static SRWLOCK g_copyMapLock = SRWLOCK_INIT;

// BOOKKEEPING LOCK: g_resourceStates/g_rtvMap are touched from engine ECL
// threads (creation/barrier/OMRT/copy hooks) AND the Present thread.
// Recursive critical section - safe for overlapping coarse scopes.
static CRITICAL_SECTION g_bookCS;
static INIT_ONCE g_bookInit = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK InitBookCS(PINIT_ONCE, PVOID, PVOID*)
{ InitializeCriticalSection(&g_bookCS); return TRUE; }
struct BookGuard {
    BookGuard() { InitOnceExecuteOnce(&g_bookInit, InitBookCS, nullptr, nullptr); EnterCriticalSection(&g_bookCS); }
    ~BookGuard() { LeaveCriticalSection(&g_bookCS); }
};

static LONG64 GetResourceAddressGeneration(ID3D12Resource* resource)
{
    if (!resource) return 0;
    LONG64 generation = 0;
    AcquireSRWLockShared(&g_resourceAddressGenerationLock);
    for (const auto& entry : g_resourceAddressGenerations) {
        if (entry.resource == resource) { generation = entry.generation; break; }
    }
    ReleaseSRWLockShared(&g_resourceAddressGenerationLock);
    return generation;
}

static bool ResourceGenerationMatches(ID3D12Resource* resource, LONG64 observedGeneration)
{
    // Generation zero means this address has never been observed through a
    // covered resource-creation path. Unknown is not a valid generation.
    return resource && observedGeneration > 0 &&
        InterlockedCompareExchange(&g_resourceAddressGenerationOverflow, 0, 0) == 0 &&
        GetResourceAddressGeneration(resource) == observedGeneration;
}

static bool EnsureResourceAddressGeneration(ID3D12Resource* resource);

static void SetTrackedResourceGeneration(ID3D12Resource** slot, ID3D12Resource* resource)
{
    volatile LONG64* generation = nullptr;
    if (slot == &g_mvResource) generation = &g_mvResourceGeneration;
    else if (slot == &g_mvResourceAlt) generation = &g_mvResourceAltGeneration;
    else if (slot == &g_depthResource) generation = &g_depthResourceGeneration;
    else if (slot == &g_sceneColor) generation = &g_sceneColorGeneration;
    else if (slot == &g_sceneColorAlt) generation = &g_sceneColorAltGeneration;
    if (generation && resource) {
        EnsureResourceAddressGeneration(resource);
        InterlockedExchange64(generation, resource ? GetResourceAddressGeneration(resource) : 0);
    } else if (generation) {
        InterlockedExchange64(generation, 0);
    }
}

static void InvalidateTrackedResourceGeneration(ID3D12Resource** slot)
{
    SetTrackedResourceGeneration(slot, nullptr);
}

static bool AdvanceResourceAddressGeneration(ID3D12Resource* resource,
                                              LONG64* generationOut)
{
    if (!resource) return false;
    bool found = false;
    AcquireSRWLockExclusive(&g_resourceAddressGenerationLock);
    ResourceAddressGeneration* freeEntry = nullptr;
    for (auto& entry : g_resourceAddressGenerations) {
        if (entry.resource == resource) { freeEntry = &entry; found = true; break; }
        if (!entry.resource && !freeEntry) freeEntry = &entry;
    }
    if (!freeEntry) {
        InterlockedExchange(&g_resourceAddressGenerationOverflow, 1);
        ReleaseSRWLockExclusive(&g_resourceAddressGenerationLock);
        return false;
    }
    if (!found) freeEntry->resource = resource;
    ++freeEntry->generation;
    if (generationOut) *generationOut = freeEntry->generation;
    ReleaseSRWLockExclusive(&g_resourceAddressGenerationLock);
    return true;
}

static bool EnsureResourceAddressGeneration(ID3D12Resource* resource)
{
    if (!resource) return false;
    AcquireSRWLockExclusive(&g_resourceAddressGenerationLock);
    ResourceAddressGeneration* freeEntry = nullptr;
    for (auto& entry : g_resourceAddressGenerations) {
        if (entry.resource == resource) {
            ReleaseSRWLockExclusive(&g_resourceAddressGenerationLock);
            return true;
        }
        if (!entry.resource && !freeEntry) freeEntry = &entry;
    }
    if (!freeEntry) {
        InterlockedExchange(&g_resourceAddressGenerationOverflow, 1);
        ReleaseSRWLockExclusive(&g_resourceAddressGenerationLock);
        return false;
    }
    freeEntry->resource = resource;
    freeEntry->generation = 1;
    ReleaseSRWLockExclusive(&g_resourceAddressGenerationLock);
    return true;
}

static bool AdvanceKnownResourceAddressGeneration(ID3D12Resource* resource,
                                                   LONG64* generationOut)
{
    if (!resource) return false;
    AcquireSRWLockExclusive(&g_resourceAddressGenerationLock);
    for (auto& entry : g_resourceAddressGenerations) {
        if (entry.resource == resource) {
            ++entry.generation;
            if (generationOut) *generationOut = entry.generation;
            ReleaseSRWLockExclusive(&g_resourceAddressGenerationLock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_resourceAddressGenerationLock);
    return false;
}

// Own-texture guard (forward): our textures must never be adopted as engine
// inputs (proven: NGX-created SRV on our g_shDepth adopted it into the depth
// slot — REAL depth was our own zeros). Defined after the owned globals.
static bool IsOwnResource(ID3D12Resource* res);

bool StoreTracked(ID3D12Resource** slot, ID3D12Resource* res)
// Returns true when the slot names res afterwards (stored, or already did).
// Returns false ONLY on self-adopt rejection. Callers MUST NOT update
// discovery metadata (valid bits, stamps, formats, state map, logs) on
// false: the slot still holds the previous engine resource and the old
// metadata describes it.
{
    if (*slot == res) return true; // same address is not fresh-lifetime evidence
    // SELF-ADOPTION GUARD: never track our own textures as engine inputs.
    // NGX (via the hooked device) creates views on the resources we hand it;
    // those view-creation hooks would otherwise adopt our placeholders back
    // into the discovery slots (observed: NGX SRV on our g_shDepth).
    if (res && IsOwnResource(res)) {
        static volatile LONG s_selfAdoptLogs = 0;
        if (InterlockedIncrement(&s_selfAdoptLogs) <= 3)
            Log("hooks: self-adopt rejected %p", (void*)res);
        return false;
    }
    bool mvSlot = (slot == (ID3D12Resource**)&g_mvResource) || (slot == (ID3D12Resource**)&g_mvResourceAlt);
    bool depSlot = (slot == (ID3D12Resource**)&g_depthResource);
    bool sceneSlot = (slot == (ID3D12Resource**)&g_sceneColor) || (slot == (ID3D12Resource**)&g_sceneColorAlt);
    if (sceneSlot && *slot && res) {
        // Routine double-buffer ping-pong (engine binds A,B,A,B...) must NOT
        // count as a change - otherwise the settle gate can never elapse.
        ID3D12Resource** other = (slot == (ID3D12Resource**)&g_sceneColor)
                                     ? &g_sceneColorAlt : &g_sceneColor;
        if (*other == res) {
            *slot = res;
            InvalidateTrackedResourceGeneration(slot);
            return true;
        } // pure reassignment
        // Only PERSISTENT composite sources are real scene changes - transient
        // post targets on recycled descriptors swap constantly during play.
        int persistNow = 0;
        { AcquireSRWLockShared(&g_copyMapLock); auto ci = g_copySrcCount.find((void*)res); if (ci != g_copySrcCount.end()) persistNow = ci->second; ReleaseSRWLockShared(&g_copyMapLock); }
        if (persistNow < 40) {
            *slot = res;
            InvalidateTrackedResourceGeneration(slot);
            return true;
        }
        g_lastSceneChangeFrame = g_frameCounter;
        g_lastDiscoveryChangeFrame = g_frameCounter;
        static unsigned s_changeFrames[8] = {};
        static int s_changeHead = 0;
        s_changeFrames[s_changeHead] = g_frameCounter;
        s_changeHead = (s_changeHead + 1) & 7;
        int recent = 0;
        for (int i = 0; i < 8; ++i)
            if ((int)(g_frameCounter - s_changeFrames[i]) >= 0 &&
                g_frameCounter - s_changeFrames[i] <= 90)
                ++recent;
        if (recent >= 5) {
            g_quietUntilFrame = g_frameCounter + 60;
            Log("hooks: scene churn %d/90f - quarantine until frame %u",
                recent, g_quietUntilFrame);
        }
    } else if (mvSlot) {
        ID3D12Resource** oMv = (slot == (ID3D12Resource**)&g_mvResource)
                                    ? &g_mvResourceAlt : &g_mvResource;
        if (*oMv == res) {
            *slot = res;
            InvalidateTrackedResourceGeneration(slot);
            return true;
        }
        g_lastDiscoveryChangeFrame = g_frameCounter;
    } else if (depSlot) {
        g_lastDiscoveryChangeFrame = g_frameCounter;
        // Rotation census (bounded, logging only): how fast does the depth
        // slot churn? Decides whether stability-gating can find usable
        // depth (MV proved stable; depth proved transient). Null clears
        // excluded; same-pointer swaps early-return above.
        if (res) {
            static ID3D12Resource* s_lastDepth = nullptr;
            if (res != s_lastDepth) {
                s_lastDepth = res;
                static volatile LONG s_depthChanges = 0;
                LONG dc = InterlockedIncrement(&s_depthChanges);
                if (dc <= 5 || (dc % 20) == 0)
                    Log("hooks: depth slot rotation #%ld -> %p (frame %u)", dc, (void*)res, g_frameCounter);
            }
        }
    }
    // STRICTLY WEAK: never hold refs on engine-owned resources. BeamNG's
    // lifecycle is refcount-exact - any extra ref (at observation OR at
    // creation) desyncs its teardown bookkeeping and corrupts its object
    // graph. Staleness stamps + bridge SEH handle freed pointers safely.
    *slot = res;
    // Generic discovery/copy/barrier observations are not lifetime proof.
    // Candidate eligibility is restored only by a fresh resource-view path.
    InvalidateTrackedResourceGeneration(slot);
    return true;
}
// Last-seen MV RTV descriptor key. After an invalidation the engine does NOT
// recreate its MV texture (same map/spawn - fixed content), so creation-based
// adoption never refires and MV stays 'absent' all session. This key lets us
// re-adopt the same texture from g_rtvMap once discovery is live again.
static unsigned long long g_mvLastRtvKey = 0;
// Rolling last-full-res-copy source - correlated with Present to name the
// terminal scene node (the texture that feeds Present = DLAA input target).
static unsigned g_lastNewChainFrame = 0;
// Sentinel correctness: 0 is both "never observed" and a valid frameCounter
// value (pre-first-camera). Without a separate observed flag, a chain first
// seen at frame 0 reads as never-observed forever (quiet stuck 0f).
static bool g_chainObserved = false;
// SHADOW PRESENT QUIET (measurement-only, no gating): stamped at the same
// newNode point as g_lastNewChainFrame. Thread model: written on engine
// recording threads inside CopyTexBody (concurrent ECL workers); read on
// engine threads and the Present thread inside EnsureUpscalerInit's defer log.
// Plain fields would race, so storage follows the established serial pattern:
// LONG64 + InterlockedExchange64 writes / InterlockedCompareExchange64 loads
// (same as g_presentSerial/g_eclSerial); observed flag is LONG + Interlocked
// (same as g_loadPhase/g_settledOnce). Width is 64-bit like g_presentSerial;
// quiet is (now - last) modulo 2^64, wrap-safe and practically nowrap.
static volatile LONG64 g_lastNewChainPresent = 0;
static volatile LONG g_chainPresentObserved = 0;
static unsigned long long HooksGetPresentQuietFrames() {
    if (InterlockedCompareExchange(&g_chainPresentObserved, 0, 0) == 0) return 0;
    unsigned long long now =
        (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    unsigned long long last =
        (unsigned long long)InterlockedCompareExchange64(&g_lastNewChainPresent, 0, 0);
    return now - last;
}
// Backbuffer-fetch circuit breaker: consecutive guarded faults on the cached
// swapchain mean it is stale; null it and let Present self-heal re-adopt.
static volatile long g_bbFetchFails = 0;
// THREAD SAFETY: Hook_CopyTextureRegion / OMSetRenderTargets run on the
// ENGINE'S SUBMISSION THREADS. The std::map below was mutated unsynchronized
// - concurrent inserts corrupt the heap and crash ANYWHERE later (driver,
// engine, us). Every access takes this lock.

// Swapchains that repeatedly faulted during backbuffer fetch - never touch
// these objects again (engine-guarded wrappers raise on our probes).
void* g_badSc[4] = { nullptr, nullptr, nullptr, nullptr };
ID3D12Resource* g_bbCached = nullptr; // captured from RTV creation - no GetBuffer probes needed
// Reviewer #16 isolation: copies-only bridge mode (no NGX eval).
static const bool g_diagBridge = GetEnvironmentVariableA("SCALENG_DIAG_BRIDGE", nullptr, 0) > 0;

static void* g_topoLastSrc = nullptr;
static unsigned g_topoLastFmt = 0;

// Bounded registry for cold-path native resource discovery.  This registry
// deliberately owns no COM references: it is evidence only, and every later
// descriptor read remains guarded because BeamNG can retire a resource after
// creation.  Roles: 1=RTV, 2=SRV.
struct NativeCandidate {
    ID3D12Resource* resource;
    UINT64 width;
    UINT height;
    UINT format;
    UINT flags;
    UINT samples;
    unsigned roles;
    unsigned rtvCreates;
    unsigned srvCreates;
    unsigned long long firstPresent;
    unsigned long long lastCreatePresent;
};
static NativeCandidate g_nativeCandidates[96] = {};
static unsigned g_nativeCandidateCount = 0;
static SRWLOCK g_nativeCandidateLock = SRWLOCK_INIT;

static void ObserveNativeCandidate(ID3D12Resource* resource,
                                   const D3D12_RESOURCE_DESC& desc,
                                   unsigned role)
{
    if (!resource || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.MipLevels != 1 || desc.Width < 1000 || desc.Height < 500 ||
        desc.SampleDesc.Count != 1)
        return;
    const unsigned fmt = (unsigned)desc.Format;
    if (fmt != (unsigned)DXGI_FORMAT_R16G16B16A16_FLOAT &&
        fmt != (unsigned)DXGI_FORMAT_R16G16B16A16_UNORM &&
        fmt != (unsigned)DXGI_FORMAT_R16G16_FLOAT &&
        fmt != (unsigned)DXGI_FORMAT_R32_TYPELESS &&
        fmt != (unsigned)DXGI_FORMAT_R32_FLOAT &&
        fmt != (unsigned)DXGI_FORMAT_R10G10B10A2_UNORM)
        return;
    const unsigned long long present =
        (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    AcquireSRWLockExclusive(&g_nativeCandidateLock);
    NativeCandidate* found = nullptr;
    for (unsigned i = 0; i < g_nativeCandidateCount; ++i) {
        if (g_nativeCandidates[i].resource == resource) {
            found = &g_nativeCandidates[i];
            break;
        }
    }
    bool evicted = false;
    if (!found) {
        if (g_nativeCandidateCount < 96) {
            found = &g_nativeCandidates[g_nativeCandidateCount++];
        } else {
            // Keep the registry bounded while following renderer churn. The
            // oldest creation-context entry is evidence we no longer need for
            // the active presentation window; replacing it lets long runs
            // continue to expose current candidates.
            unsigned oldest = 0;
            for (unsigned i = 1; i < g_nativeCandidateCount; ++i) {
                if (g_nativeCandidates[i].lastCreatePresent <
                    g_nativeCandidates[oldest].lastCreatePresent)
                    oldest = i;
            }
            found = &g_nativeCandidates[oldest];
            evicted = true;
            *found = {};
        }
        found->resource = resource;
        found->width = desc.Width;
        found->height = (UINT)desc.Height;
        found->format = fmt;
        found->flags = (UINT)desc.Flags;
        found->samples = (UINT)desc.SampleDesc.Count;
        found->firstPresent = present;
    }
    if (found) {
        const bool newRole = (found->roles & role) == 0;
        found->roles |= role;
        found->lastCreatePresent = present;
        if (role == 1) ++found->rtvCreates;
        if (role == 2) ++found->srvCreates;
        if (newRole) {
            if (evicted)
                Log("native-candidate: registry evicted oldest entry for resource=%p",
                    (void*)resource);
            Log("native-candidate: resource=%p role=%s size=%llux%u fmt=%u flags=%u present=%llu",
                (void*)resource, role == 1 ? "RTV" : "SRV",
                (unsigned long long)desc.Width, (unsigned)desc.Height,
                fmt, (unsigned)desc.Flags, present);
        }
    }
    ReleaseSRWLockExclusive(&g_nativeCandidateLock);
}

static void LogNativeCandidateSummary(unsigned long long presentSerial)
{
    static unsigned long long s_lastSummary = 0;
    if (presentSerial > 5 && presentSerial - s_lastSummary < 600)
        return;
    s_lastSummary = presentSerial;
    AcquireSRWLockShared(&g_nativeCandidateLock);
    bool used[96] = {};
    unsigned emitted = 0;
    while (emitted < 8) {
        unsigned best = 96;
        for (unsigned i = 0; i < g_nativeCandidateCount; ++i) {
            if (!used[i] && g_nativeCandidates[i].resource &&
                (best == 96 || g_nativeCandidates[i].lastCreatePresent >
                 g_nativeCandidates[best].lastCreatePresent))
                best = i;
        }
        if (best == 96) break;
        used[best] = true;
        const NativeCandidate& c = g_nativeCandidates[best];
        Log("native-candidate: summary resource=%p size=%llux%u fmt=%u roles=%u rtv=%u srv=%u firstPresent=%llu lastCreatePresent=%llu",
            (void*)c.resource, (unsigned long long)c.width, c.height,
            c.format, c.roles, c.rtvCreates, c.srvCreates,
            c.firstPresent, c.lastCreatePresent);
        ++emitted;
    }
    Log("native-candidate: registry count=%u emitted=%u present=%llu",
        g_nativeCandidateCount, emitted, presentSerial);
    ReleaseSRWLockShared(&g_nativeCandidateLock);
}

// Forward: guarded descriptor read (defined with the other safe helpers).
static bool SafeGetDesc(ID3D12Resource* r, D3D12_RESOURCE_DESC* out);

// SCENE-SET REGISTRY (legacy rotation tracking): bounded set of observed
// scene-color candidates used for MATCHING (SceneColorBound / isSceneSrc),
// replacing the single last-wins ALT slot for that purpose. Classification
// variables (g_sceneColor/g_sceneColorAlt/g_activeSceneColor) and all
// adoption/refresh logic are untouched. Identity is pointer-based;
// dimensions/format are insert filters only, never proof of scene.
// Weak pointers, no COM refs; every descriptor read guarded.
struct SceneSetEntry {
    ID3D12Resource* resource;
    unsigned viewW;
    unsigned viewH;
    unsigned viewFmt;
    unsigned resW;
    unsigned resH;
    unsigned resFmt;
    unsigned resFlags;
    unsigned long long lastUsePresent;
    unsigned uses;
};
static SceneSetEntry g_sceneSet[8] = {};
static unsigned g_sceneSetCount = 0;
static SRWLOCK g_sceneSetLock = SRWLOCK_INIT;

static unsigned long long SceneSetNow()
{ return (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0); }

enum ResourceTouchSource : LONG {
    TOUCH_SOURCE_NONE = 0,
    TOUCH_SOURCE_BARRIER = 1,
    TOUCH_SOURCE_BROAD_BARRIER = 2,
    TOUCH_SOURCE_OM_BIND = 3,
    TOUCH_SOURCE_DSV_BIND = 4
};

static void RecordTouchLedger(ResourceTouchLedger* ledger,
                              ID3D12Resource* resource, LONG64 generation,
                              LONG source)
{
    if (!ledger || !resource) return;
    if (InterlockedCompareExchange(&ledger->writer, 1, 0) != 0) {
        InterlockedIncrement64(&ledger->dropped);
        return;
    }
    ledger->resource = resource;
    ledger->generation = generation;
    ledger->present = (LONG64)SceneSetNow();
    ledger->ecl = InterlockedCompareExchange64(&g_eclSerial, 0, 0);
    ledger->source = source;
    InterlockedExchange(&ledger->writer, 0);
}

struct ResourceTouchSnapshot {
    bool valid;
    ID3D12Resource* resource;
    LONG64 generation;
    LONG64 present;
    LONG64 ecl;
    LONG source;
    LONG64 dropped;
};

static bool CaptureTouchLedger(ResourceTouchLedger* ledger,
                               ResourceTouchSnapshot* out)
{
    if (!ledger || !out) return false;
    if (InterlockedCompareExchange(&ledger->writer, 1, 0) != 0) return false;
    ResourceTouchSnapshot snapshot = {};
    snapshot.resource = ledger->resource;
    snapshot.generation = ledger->generation;
    snapshot.present = ledger->present;
    snapshot.ecl = ledger->ecl;
    snapshot.source = ledger->source;
    snapshot.dropped = InterlockedCompareExchange64(&ledger->dropped, 0, 0);
    snapshot.valid = snapshot.resource != nullptr;
    InterlockedExchange(&ledger->writer, 0);
    *out = snapshot;
    return true;
}

// Record the exact candidate identity behind a touch as diagnostic telemetry.
// The existing shared timestamp and all gate behavior remain unchanged.
static void NoteMvTouchIdentity(ID3D12Resource* resource, LONG source)
{
    if (!resource) return;
    LONG64 generation = 0;
    ResourceTouchLedger* ledger = nullptr;
    if (resource == g_mvResource) {
        generation = InterlockedCompareExchange64(&g_mvResourceGeneration, 0, 0);
        ledger = &g_mvPrimaryTouchLedger;
    } else if (resource == g_mvResourceAlt) {
        generation = InterlockedCompareExchange64(&g_mvResourceAltGeneration, 0, 0);
        ledger = &g_mvAltTouchLedger;
    }
    if (ledger) RecordTouchLedger(ledger, resource, generation, source);
}

static void NoteDepthTouchIdentity(ID3D12Resource* resource, LONG source)
{
    if (!resource) return;
    LONG64 generation = resource == g_depthResource
        ? InterlockedCompareExchange64(&g_depthResourceGeneration, 0, 0) : 0;
    if (resource == g_depthResource)
        RecordTouchLedger(&g_depthTouchLedger, resource, generation, source);
}

// Scene-color formats: the HDR pipeline renders linear HALF-float scene
// color (R16G16B16A16_FLOAT, verified live: 1920x1080 fmt-10 RTV-bound,
// barriered PSR->RT, and viewported on one list at present 192); UNORM is
// retained for compat with targets already adopted. Project assumption is now
// evidence-backed for FLOAT; NGX infers encoding from the resource desc and
// the IsHDR create flag (set in dlss_ngx.cpp).
static bool IsSceneColorFormat(unsigned fmt)
{
    return fmt == (unsigned)DXGI_FORMAT_R16G16B16A16_UNORM ||
           fmt == (unsigned)DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// Depth-family RESOURCE formats (SDK-verified numerics, not memory): NGX
// depth is single-channel; velocity (34) and LDR color (28) copy DESTs must
// never occupy the depth slot (observed: fmt-34 velocity adopted as depth,
// fmt-28 359x379 junk). Typeless D24 (44) and D24S8 view (45) resources are
// true depth; conversion to an NGX-usable representation is separate work.
static bool IsDepthFamilyFormat(unsigned fmt)
{
    return fmt == (unsigned)DXGI_FORMAT_R32_TYPELESS ||        // 39
           fmt == (unsigned)DXGI_FORMAT_D32_FLOAT ||          // 40
           fmt == (unsigned)DXGI_FORMAT_R32_FLOAT ||          // 41
           fmt == (unsigned)DXGI_FORMAT_R24G8_TYPELESS ||     // 44
           fmt == (unsigned)DXGI_FORMAT_D24_UNORM_S8_UINT ||  // 45
           fmt == (unsigned)DXGI_FORMAT_R24_UNORM_X8_TYPELESS;// 46
}

// Insert-capable observe: creation / OM-adoption paths pass already-fetched
// view evidence for a resource that is live-by-construction at that point
// (just created / just bound on this thread). MV (R16G16F) and UI (small /
// non-scene-color) targets are excluded by the scene-format + size filter by design.
static void SceneSetNote(ID3D12Resource* res, unsigned w, unsigned h, unsigned viewFmt)
{
    if (!res || w < 1000 || h < 500 || !IsSceneColorFormat(viewFmt))
        return;
    const unsigned long long now = SceneSetNow();
    AcquireSRWLockShared(&g_sceneSetLock);
    bool known = false;
    for (unsigned i = 0; i < g_sceneSetCount; ++i) {
        if (g_sceneSet[i].resource == res) { known = true; break; }
    }
    ReleaseSRWLockShared(&g_sceneSetLock);
    if (known) {
        AcquireSRWLockExclusive(&g_sceneSetLock);
        for (unsigned i = 0; i < g_sceneSetCount; ++i) {
            if (g_sceneSet[i].resource == res) {
                g_sceneSet[i].lastUsePresent = now;
                ++g_sceneSet[i].uses;
                g_sceneSet[i].viewW = w; g_sceneSet[i].viewH = h; g_sceneSet[i].viewFmt = viewFmt;
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_sceneSetLock);
        return;
    }
    // Reuse-baseline snapshot (guarded; a fault means teardown raced us).
    D3D12_RESOURCE_DESC rd = {};
    if (!SafeGetDesc(res, &rd)) return;
    AcquireSRWLockExclusive(&g_sceneSetLock);
    for (unsigned i = 0; i < g_sceneSetCount; ++i) {
        if (g_sceneSet[i].resource == res) {
            g_sceneSet[i].lastUsePresent = now;
            ++g_sceneSet[i].uses;
            ReleaseSRWLockExclusive(&g_sceneSetLock);
            return;
        }
    }
    SceneSetEntry* slot = nullptr;
    if (g_sceneSetCount < 8) {
        slot = &g_sceneSet[g_sceneSetCount++];
    } else {
        unsigned oldest = 0;
        for (unsigned i = 1; i < g_sceneSetCount; ++i) {
            if (g_sceneSet[i].lastUsePresent < g_sceneSet[oldest].lastUsePresent)
                oldest = i;
        }
        slot = &g_sceneSet[oldest];
        static int s_evictLogs = 0;
        if (++s_evictLogs <= 4)
            Log("hooks: scene-set evicted LRU %p for %p", (void*)slot->resource, (void*)res);
    }
    slot->resource = res;
    slot->viewW = w; slot->viewH = h; slot->viewFmt = viewFmt;
    slot->resW = (unsigned)rd.Width; slot->resH = (unsigned)rd.Height;
    slot->resFmt = (unsigned)rd.Format; slot->resFlags = (unsigned)rd.Flags;
    slot->lastUsePresent = now;
    slot->uses = 1;
    unsigned countNow = g_sceneSetCount;
    ReleaseSRWLockExclusive(&g_sceneSetLock);
    static int s_noteLogs = 0;
    if (++s_noteLogs <= 12)
        Log("hooks: scene-set note %p (%ux%u viewFmt=%u resFmt=%u) count=%u",
            (void*)res, w, h, viewFmt, (unsigned)rd.Format, countNow);
}

// Recency-only touch (no insert, no descriptor reads): OM-bind and copy
// paths refresh live usage of already-known members.
static void SceneSetTouch(ID3D12Resource* res)
{
    if (!res) return;
    const unsigned long long now = SceneSetNow();
    AcquireSRWLockExclusive(&g_sceneSetLock);
    for (unsigned i = 0; i < g_sceneSetCount; ++i) {
        if (g_sceneSet[i].resource == res) {
            g_sceneSet[i].lastUsePresent = now;
            ++g_sceneSet[i].uses;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_sceneSetLock);
}

// Pointer-identity membership (hot paths): pure compares, no dereference.
static bool SceneSetContains(ID3D12Resource* res)
{
    if (!res) return false;
    AcquireSRWLockShared(&g_sceneSetLock);
    bool hit = false;
    for (unsigned i = 0; i < g_sceneSetCount; ++i) {
        if (g_sceneSet[i].resource == res) { hit = true; break; }
    }
    ReleaseSRWLockShared(&g_sceneSetLock);
    return hit;
}

// Periodic sweep (Present cadence): guarded revalidation. A fault drops the
// entry (never dereferenced again). A descriptor change means the address
// was reused by a new allocation: refresh the baseline and reset usage so
// stale metadata never attaches to the new resource.
static void SceneSetSweep()
{
    AcquireSRWLockExclusive(&g_sceneSetLock);
    for (unsigned i = 0; i < g_sceneSetCount;) {
        ID3D12Resource* res = g_sceneSet[i].resource;
        D3D12_RESOURCE_DESC rd = {};
        ReleaseSRWLockExclusive(&g_sceneSetLock);
        bool ok = (res && SafeGetDesc(res, &rd));
        AcquireSRWLockExclusive(&g_sceneSetLock);
        unsigned idx = g_sceneSetCount;
        for (unsigned j = 0; j < g_sceneSetCount; ++j) {
            if (g_sceneSet[j].resource == res) { idx = j; break; }
        }
        if (idx >= g_sceneSetCount) continue;
        if (!ok) {
            static int s_invLogs = 0;
            if (++s_invLogs <= 6)
                Log("hooks: scene-set invalidated %p (guarded read fault)", (void*)res);
            g_sceneSet[idx] = g_sceneSet[--g_sceneSetCount];
            g_sceneSet[g_sceneSetCount] = {};
            continue;
        }
        if ((unsigned)rd.Width != g_sceneSet[idx].resW ||
            (unsigned)rd.Height != g_sceneSet[idx].resH ||
            (unsigned)rd.Format != g_sceneSet[idx].resFmt ||
            (unsigned)rd.Flags != g_sceneSet[idx].resFlags) {
            g_sceneSet[idx].resW = (unsigned)rd.Width;
            g_sceneSet[idx].resH = (unsigned)rd.Height;
            g_sceneSet[idx].resFmt = (unsigned)rd.Format;
            g_sceneSet[idx].resFlags = (unsigned)rd.Flags;
            g_sceneSet[idx].uses = 1;
            static int s_reuseLogs = 0;
            if (++s_reuseLogs <= 6)
                Log("hooks: scene-set address reused %p (%ux%u fmt=%u) - baseline refreshed",
                    (void*)res, (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)rd.Format);
        }
        ++i;
    }
    ReleaseSRWLockExclusive(&g_sceneSetLock);
}

// These objects are declared below with external linkage; forward declarations
// keep the diagnostic helpers adjacent to the topology state they describe.
extern ID3D12CommandQueue* g_graphicsQueue;
extern ID3D12Resource* g_bbCached;
extern ID3D12Resource* g_boundRtvResource;

// Compact, read-only topology evidence.  Resource pointers are weak because
// BeamNG retires render targets during resize/map transitions, so every
// descriptor read is guarded.  This is intentionally sampled from Present
// rather than emitted from hot command-list paths; it gives one useful record
// per render interval instead of megabytes of repetitive logging.
static void LogTopoResource(const char* label, ID3D12Resource* res)
{
    if (!res) {
        Log("topo-state: %s=null", label);
        return;
    }
    __try {
        D3D12_RESOURCE_DESC d = res->GetDesc();
        Log("topo-state: %s=%p %llux%llu fmt=%u flags=%u samples=%u",
            label, (void*)res,
            (unsigned long long)d.Width, (unsigned long long)d.Height,
            (unsigned)d.Format, (unsigned)d.Flags,
            (unsigned)d.SampleDesc.Count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("topo-state: %s=%p desc-fault code=%08X", label, (void*)res,
            (unsigned)GetExceptionCode());
    }
}

static void LogTopoSnapshot(unsigned long long presentSerial,
                            ID3D12Resource* bridgePresentBb,
                            ID3D12Resource* bridgeOutput,
                            bool bridgeReady,
                            int deferredPending)
{
    static unsigned long long s_lastSnapshot = 0;
    if (presentSerial > 5 && presentSerial - s_lastSnapshot < 120)
        return;
    s_lastSnapshot = presentSerial;
    Log("topo-state: snapshot present=%llu ecl=%llu queue=%p bb=%p scene=%p alt=%p bound=%p lastCopySrc=%p lastCopyFmt=%u bridgeReady=%d deferred=%d bridgeBb=%p bridgeOut=%p mvOm=%llu mvBar=%llu depDsv=%llu depBar=%llu",
        presentSerial,
        (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0),
        (void*)g_graphicsQueue, (void*)g_bbCached, (void*)g_sceneColor,
        (void*)g_sceneColorAlt, (void*)g_boundRtvResource, g_topoLastSrc,
        g_topoLastFmt, (int)bridgeReady, deferredPending,
        (void*)bridgePresentBb, (void*)bridgeOutput,
        (unsigned long long)InterlockedExchange64(&g_diagMvOm, 0),
        (unsigned long long)InterlockedExchange64(&g_diagMvBar, 0),
        (unsigned long long)InterlockedExchange64(&g_diagDepthDsv, 0),
        (unsigned long long)InterlockedExchange64(&g_diagDepthBar, 0));
    LogTopoResource("bb", g_bbCached);
    LogTopoResource("scene", g_sceneColor);
    LogTopoResource("sceneAlt", g_sceneColorAlt);
    LogTopoResource("bound", g_boundRtvResource);
    LogTopoResource("lastCopySrc", (ID3D12Resource*)g_topoLastSrc);
    LogTopoResource("bridgeBb", bridgePresentBb);
    LogTopoResource("bridgeOut", bridgeOutput);
    LogNativeCandidateSummary(presentSerial);
}
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
// STEADY-STATE SUBMIT QUEUE (single-writer latch): g_graphicsQueue has five
// writers (ECL-first, CreateCommandQueue capture, InjectAtPresent,
// IDENTITY probe, swapchain hook) and verified live to differ between
// capture time (CreateCommandQueue first-direct) and steady state (ECL
// observations, e.g. capture=804289D940 vs ECL=80005B3040 in one run).
// Own-list submissions must order against the RENDER stream, so the queue
// is latched once from validated GAME ECL observations and never rewritten.
// No consumers yet (ordering design pending); recording only.
ID3D12CommandQueue* g_gameSubmitQueue = nullptr;
ID3D12CommandAllocator* g_injAlloc = nullptr;
ID3D12GraphicsCommandList* g_injList = nullptr;
ID3D12DescriptorHeap* g_injHeap = nullptr;
ID3D12DescriptorHeap* g_injSamplerHeap = nullptr;
ID3D12Fence* g_injFence = nullptr;
HANDLE g_injEvent = nullptr;
UINT64 g_injFenceVal = 0;
bool g_injSubmitted = false;
IDXGIFactory* g_anyFactory = nullptr;

void EnsureGlobalSwapchainHookImpl();

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
// DLSS working output: HDR HALF-float to match the linear fmt-10 scene input
// (identical-format copy law for the dlssOut->scene result copy). DLAA mode
// re-forces this to the backbuffer format per-present (see below).
DXGI_FORMAT g_dlssOutFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

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
std::map<SIZE_T, ID3D12Resource*> g_dsvMap;
static std::map<SIZE_T, ID3D12Resource*> g_displayRTVMap; // persistent display-sized, not overwritten on handle reuse
// Last display-sized RTV creation for provenance
static D3D12_CPU_DESCRIPTOR_HANDLE g_lastDisplayRTVHandle = {};
static ID3D12Resource* g_lastDisplayRTVResource = nullptr;
static unsigned long long g_lastDisplayRTVPresent = 0;
struct DisplayRTVRecord { D3D12_CPU_DESCRIPTOR_HANDLE handle; ID3D12Resource* resource; unsigned width; unsigned height; unsigned format; unsigned long long present; };
static DisplayRTVRecord g_displayRTVHistory[8] = {};
static unsigned g_displayRTVHistoryNext = 0;

std::map<ID3D12Resource*, D3D12_RESOURCE_STATES> g_resourceStates;

// SRV CPU handle → resource map (parallels g_rtvMap for RTVs).
// Populated by Hook_CreateShaderResourceView and propagated by
// Hook_CopyDescriptors*/CopyDescriptorsSimple so that descriptors copied
// into shader-visible heaps can still be resolved back to resources.
std::map<SIZE_T, ID3D12Resource*> g_srvMap;

UINT g_setHeapCount = 0;
ID3D12DescriptorHeap* g_setHeaps[2] = { nullptr, nullptr };
// Heap-state snapshot lock: engine threads WRITE this state on every
// SetDescriptorHeaps while the Present-thread flow READS it to save/restore.
// Unsynced tearing during rotation churn handed the restore path freed heap
// pointers - the convicted killer of the 6s-class crashes (WER offsets
// 0x9d5d..0x9e3d, all inside Hook_SetDescriptorHeaps).
static SRWLOCK g_heapStateLock = SRWLOCK_INIT;

typedef void (STDMETHODCALLTYPE* PFN_CreateRenderTargetView)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void (STDMETHODCALLTYPE* PFN_CreateDepthStencilView)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void (STDMETHODCALLTYPE* PFN_CreateShaderResourceView)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateCommandQueue)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateCommandList)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
typedef void (STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef void (STDMETHODCALLTYPE* PFN_CopyBufferRegion)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
typedef void (STDMETHODCALLTYPE* PFN_CopyTextureRegion)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
typedef void (STDMETHODCALLTYPE* PFN_CopyResource)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
typedef void (STDMETHODCALLTYPE* PFN_SetGraphicsRootDescriptorTable)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
typedef void (STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
typedef void (STDMETHODCALLTYPE* PFN_RSSetScissorRects)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
typedef void (STDMETHODCALLTYPE* PFN_ResourceBarrier)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
typedef void (STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CommandListClose)(ID3D12GraphicsCommandList*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CommandListReset)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
typedef void (STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
static HRESULT STDMETHODCALLTYPE Shim_CommandListClose(ID3D12GraphicsCommandList*);
static HRESULT STDMETHODCALLTYPE Shim_CommandListReset(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
static void STDMETHODCALLTYPE Shim_DrawInstanced(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
static void STDMETHODCALLTYPE Shim_DrawIndexedInstanced(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
static void STDMETHODCALLTYPE Shim_Dispatch(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ResourceMap)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
typedef void (STDMETHODCALLTYPE* PFN_ResourceUnmap)(ID3D12Resource*, UINT, const D3D12_RANGE*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateCommittedResource)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreatePlacedResource)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateReservedResource)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
// ID3D12Device slots 23/24 (SDK 10.0.28000.0): CopyDescriptors / CopyDescriptorsSimple.
// These copy CPU-visible descriptors into shader-visible heaps — the handles
// used by OMSetRenderTargets / SetGraphicsRootDescriptorTable that are
// NOT in g_rtvMap/g_srvMap unless we track the copy here.
typedef void (STDMETHODCALLTYPE* PFN_CopyDescriptors)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
typedef void (STDMETHODCALLTYPE* PFN_CopyDescriptorsSimple)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
// Device4 '1'-variant creation signatures (SDK-verified slots 53/54/55; see
// slot audit - an earlier read misreported 56/57/58 from unfiltered header
// text with non-Windows preprocessor branches included).
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateCommittedResource1)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateHeap1)(ID3D12Device*, const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateReservedResource1)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
// Descriptor-heap creation signature (SDK-verified slot 14).
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateDescriptorHeap)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
// Device8 resource-creation signatures (slots verified: CommittedResource2=69,
// PlacedResource1=70). D3D12_RESOURCE_DESC1 shares the base descriptor prefix,
// so the shared texture filter can read it through a base pointer.
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateCommittedResource2)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreatePlacedResource1)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);

PFN_CreateRenderTargetView Real_CreateRenderTargetView = nullptr;
PFN_CreateDepthStencilView Real_CreateDepthStencilView = nullptr;
PFN_CreateShaderResourceView Real_CreateShaderResourceView = nullptr;
PFN_CopyDescriptors Real_CopyDescriptors = nullptr;
PFN_CopyDescriptorsSimple Real_CopyDescriptorsSimple = nullptr;
PFN_CreateCommandQueue Real_CreateCommandQueue = nullptr;
PFN_CreateCommandList Real_CreateCommandList = nullptr;
PFN_ExecuteCommandLists Real_ExecuteCommandLists = nullptr;
// Device QueryInterface original for the QI census. Same save-once pattern as
// the other device vtable entries: only the first observed table is ever
// swapped, so a single global stays correct (see the distinct-vtable guard;
// second tables are skipped, reported as a coverage limitation).
typedef HRESULT (STDMETHODCALLTYPE* PFN_DeviceQueryInterface)(IUnknown* self, REFIID riid, void** ppvObject);
PFN_DeviceQueryInterface Real_DeviceQI = nullptr;
static volatile LONG g_realQueueHookInstalled = 0;
static volatile LONG g_realListHookInstalled = 0;
static volatile LONG g_gameQueueObserved = 0;
PFN_CopyBufferRegion Real_CopyBufferRegion = nullptr;
PFN_CopyTextureRegion Real_CopyTextureRegion = nullptr;
PFN_RSSetViewports Real_RSSetViewports = nullptr;
PFN_RSSetScissorRects Real_RSSetScissorRects = nullptr;
PFN_ResourceBarrier Real_ResourceBarrier = nullptr;
PFN_OMSetRenderTargets Real_OMSetRenderTargets = nullptr;
PFN_CreateCommittedResource Real_CreateCommittedResource = nullptr;
PFN_CreatePlacedResource Real_CreatePlacedResource = nullptr;
PFN_CreateReservedResource Real_CreateReservedResource = nullptr;
// Device4 per-table originals (save-once, same rule as the base entries:
// only the first observed table is ever swapped).
PFN_CreateCommittedResource1 Real_CommittedResource1 = nullptr;
PFN_CreateHeap1 Real_Heap1 = nullptr;
PFN_CreateReservedResource1 Real_ReservedResource1 = nullptr;
// Descriptor-heap per-table original (save-once, same distinct-table rule).
PFN_CreateDescriptorHeap Real_CreateDescriptorHeap = nullptr;
// Device8 per-table originals (save-once, same rule as all device entries).
PFN_CreateCommittedResource2 Real_CommittedResource2 = nullptr;
PFN_CreatePlacedResource1 Real_PlacedResource1 = nullptr;
static void* g_hookedDeviceVtbl = nullptr;

// Per-object read-only command-list wrappers.  BeamNG exposes more than one
// command-list implementation/vtable (direct and copy lists), so one global
// MinHook trampoline is not sufficient.  We clone each real list's vtable and
// replace only the observation slots below; every other method and all
// intercepted methods are forwarded to the original vtable entry.
struct CommandListShim {
    ID3D12GraphicsCommandList* list;
    UINT listType;
    unsigned long long createdPresent;
    unsigned long long createdEcl;
    unsigned copyCount;
    void** originalVtbl;
    void** clonedVtbl;
    PFN_CopyTextureRegion copyTexture;
    PFN_CopyBufferRegion copyBufferRegion;
    PFN_ResourceBarrier resourceBarrier;
    PFN_OMSetRenderTargets omSetRenderTargets;
    // Viewport/scissor originals (SDK-verified slots 21/22). Added when the
    // legacy path was reconnected: the patch flag they set gates injection.
    PFN_RSSetViewports rsSetViewports;
    PFN_RSSetScissorRects rsSetScissorRects;
    // Whole-resource copy original (SDK-verified slot 17). Observe-only:
    // counts + endpoint identity, no analysis or forwarding changes.
    PFN_CopyResource copyResource;
    // Graphics root descriptor table original (SDK-verified slot 32, filtered
    // CINTERFACE order cross-checked). Observe-only bind TIMING: root parameter
    // + GPU handle value only; heap contents never read, referenced resource
    // never resolved here.
    PFN_SetGraphicsRootDescriptorTable setGraphicsRootDescriptorTable;
    // Diagnostic-only recording epoch and MV-target draw census. These fields
    // contain pointer identity/counts only; no engine COM reference is held.
    PFN_CommandListClose close;
    PFN_CommandListReset reset;
    PFN_DrawInstanced drawInstanced;
    PFN_DrawIndexedInstanced drawIndexedInstanced;
    PFN_Dispatch dispatch;
    volatile LONG64 recordingEpoch;
    volatile LONG recordingClosed;
    volatile LONG recordingResetInProgress;
    volatile LONG recordingResetFailed;
    ID3D12Resource* volatile currentOmMvTarget;
    ID3D12Resource* volatile recordedMvTarget;
    volatile LONG mvTargetWidth;
    volatile LONG mvTargetHeight;
    volatile LONG64 mvTargetOmCount;
    volatile LONG64 mvTargetDrawCount;
    volatile LONG64 mvTargetIndexedDrawCount;
    volatile LONG64 dispatchCount;
    volatile LONG64 mvTargetRecordPresent;
    volatile LONG64 mvTargetRecordEcl;
};
// Resource shim for Map/Unmap hooks on constant buffers
struct ResourceShim {
    ID3D12Resource* resource;
    void** originalVtbl;
    void** clonedVtbl;
    PFN_ResourceMap map;
    PFN_ResourceUnmap unmap;
    size_t cbSize;
};
static ResourceShim g_resourceShims[32] = {};
static SRWLOCK g_resourceShimLock = SRWLOCK_INIT;
static volatile LONG64 g_resourceMapCalls = 0;

static CommandListShim g_commandListShims[64] = {};
static SRWLOCK g_commandListShimLock = SRWLOCK_INIT;
// Exact diagnostic totals (atomic; snapshots reported on sampled log records).
static volatile LONG64 g_installCallsTotal = 0;
static volatile LONG64 g_installOkTotal = 0;
static volatile LONG64 g_installDedupTotal = 0;
static volatile LONG64 g_installFailTotal = 0;
static volatile LONG64 g_shimCopyCalls = 0;
static volatile LONG64 g_shimBufferCopyCalls = 0;
static volatile LONG64 g_shimOmCalls = 0;
static volatile LONG64 g_shimBarrierCalls = 0;
// Viewport/scissor invocation totals (exact; install vs invocation split).
static volatile LONG64 g_shimVpCalls = 0;
static volatile LONG64 g_shimScCalls = 0;
// Whole-resource copy invocation total (exact; observe-only diagnostic).
static volatile LONG64 g_shimCopyResCalls = 0;
// Graphics root descriptor table invocation total (exact; timing only).
static volatile LONG64 g_shimRootTableCalls = 0;
struct NativeCorrelation {
    void* copySrc;
    void* copyDst;
    UINT copySrcFormat;
    UINT copyDstFormat;
    void* omColor;
    UINT omColorFormat;
    void* barrierResource;
    UINT barrierFormat;
    UINT barrierBefore;
    UINT barrierAfter;
    unsigned long long present;
    unsigned long long ecl;
    unsigned long long generation;
    unsigned long long batchPresent;
    unsigned long long batchLastPresent;
    unsigned long long batchEcl;
    unsigned long long batchLastEcl;
    void* batchColor;
    UINT batchColorFormat;
    void* batchCopySrc;
    void* batchCopyDst;
    UINT batchCopySrcFormat;
    UINT batchCopyDstFormat;
    void* batchBarrierResource;
    UINT batchBarrierFormat;
    UINT batchBarrierBefore;
    UINT batchBarrierAfter;
    unsigned batchCopySeen;
    unsigned batchOmSeen;
    unsigned batchBarrierSeen;
    unsigned long long batchReportedGeneration;
};
static NativeCorrelation g_nativeCorrelation = {};
static SRWLOCK g_nativeCorrelationLock = SRWLOCK_INIT;

struct NativeBatchTargetHistory {
    void* resource;
    UINT format;
    unsigned hits;
    unsigned long long firstPresent;
    unsigned long long lastPresent;
};
static NativeBatchTargetHistory g_nativeBatchTargets[16] = {};

// Read-only lifetime evidence for the display-sized scene-color resource.
// This deliberately stores only weak identities: no COM reference is held and
// no command list or resource state is changed. The resource can therefore be
// observed safely while BeamNG rotates/rebuilds its render graph.
struct SceneColorCandidateHistory {
    void* resource;
    UINT format;
    UINT width;
    UINT height;
    unsigned samples;
    unsigned long long firstPresent;
    unsigned long long lastPresent;
    unsigned long long lastSamplePresent;
};
static SceneColorCandidateHistory g_sceneColorCandidates[8] = {};
static SRWLOCK g_sceneColorCandidateLock = SRWLOCK_INIT;
static volatile LONG64 g_sceneColorCandidateLastLog = 0;

static void BeginNativeBatchLocked(NativeCorrelation& c,
                                   unsigned long long present,
                                   unsigned long long ecl)
{
    // BeamNG records one renderer frame over several ECL submissions and the
    // observed native work can straddle a small Present boundary. Keep a
    // bounded four-Present window, then require recurrence separately.
    if (c.batchPresent != 0 && present >= c.batchPresent &&
        present - c.batchPresent <= 3) {
        if (present > c.batchLastPresent) c.batchLastPresent = present;
        c.batchLastEcl = ecl;
        return;
    }
    c.batchPresent = present;
    c.batchLastPresent = present;
    c.batchEcl = ecl;
    c.batchLastEcl = ecl;
    c.batchColor = nullptr;
    c.batchColorFormat = 0;
    c.batchCopySrc = nullptr;
    c.batchCopyDst = nullptr;
    c.batchCopySrcFormat = 0;
    c.batchCopyDstFormat = 0;
    c.batchBarrierResource = nullptr;
    c.batchBarrierFormat = 0;
    c.batchBarrierBefore = 0;
    c.batchBarrierAfter = 0;
    c.batchCopySeen = 0;
    c.batchOmSeen = 0;
    c.batchBarrierSeen = 0;
}

static void MaybeLogCoherentNativeBatchLocked(NativeCorrelation& c)
{
    // This is a diagnostic eligibility record only. It deliberately excludes
    // the cached swapchain backbuffer and never enables output mutation.
    if (!c.batchCopySeen || !c.batchOmSeen || !c.batchBarrierSeen ||
        !c.batchColor || c.batchColor == (void*)g_bbCached ||
        c.batchReportedGeneration == c.generation ||
        // Present 1 is BeamNG startup composition. Require a later gameplay
        // interval before allowing a target into the handoff candidate set.
        c.batchPresent < 120)
        return;
    if (c.batchColorFormat != (UINT)DXGI_FORMAT_R16G16B16A16_UNORM &&
        c.batchColorFormat != (UINT)DXGI_FORMAT_R16G16B16A16_FLOAT &&
        c.batchColorFormat != (UINT)DXGI_FORMAT_R10G10B10A2_UNORM)
        return;
    NativeBatchTargetHistory* history = nullptr;
    for (auto& h : g_nativeBatchTargets) {
        if (h.resource == c.batchColor && h.format == c.batchColorFormat) {
            history = &h;
            break;
        }
    }
    if (!history) {
        for (auto& h : g_nativeBatchTargets) {
            if (!h.resource) { history = &h; break; }
        }
    }
    if (!history)
        return;
    if (history->lastPresent != c.batchPresent) {
        if (!history->resource) {
            history->resource = c.batchColor;
            history->format = c.batchColorFormat;
            history->firstPresent = c.batchPresent;
        }
        history->lastPresent = c.batchPresent;
        ++history->hits;
    }
    c.batchReportedGeneration = c.generation;
    Log("native-batch: coherent=1 repeatable=%u hits=%u target=%p targetFmt=%u batchPresent=%llu-%llu batchEcl=%llu-%llu copySrc=%p copyDst=%p copyFmt=%u/%u barrier=%p barrierFmt=%u states=%u/%u generation=%llu",
        history->hits >= 3 ? 1u : 0u, history->hits,
        c.batchColor, c.batchColorFormat, c.batchPresent, c.batchLastPresent,
        c.batchEcl, c.batchLastEcl,
        c.batchCopySrc, c.batchCopyDst, c.batchCopySrcFormat, c.batchCopyDstFormat,
        c.batchBarrierResource, c.batchBarrierFormat,
        c.batchBarrierBefore, c.batchBarrierAfter, c.generation);
}

static void LogNativeCorrelation(const char* reason, unsigned long long value)
{
    NativeCorrelation c = {};
    AcquireSRWLockShared(&g_nativeCorrelationLock);
    c = g_nativeCorrelation;
    ReleaseSRWLockShared(&g_nativeCorrelationLock);
    unsigned long long currentPresent = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    unsigned long long currentEcl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
    unsigned long long presentAge = currentPresent >= c.present ? currentPresent - c.present : 0;
    unsigned long long eclAge = currentEcl >= c.ecl ? currentEcl - c.ecl : 0;
    bool fresh = c.generation != 0 && presentAge <= 120 && eclAge <= 1200;
    Log("native-correlation: reason=%s value=%llu fresh=%u generation=%llu presentAge=%llu eclAge=%llu copySrc=%p copyDst=%p copyFmt=%u/%u omColor=%p omFmt=%u barrier=%p barrierFmt=%u states=%u/%u nativePresent=%llu nativeEcl=%llu currentPresent=%llu currentEcl=%llu",
        reason, value, fresh ? 1u : 0u, c.generation, presentAge, eclAge,
        c.copySrc, c.copyDst, c.copySrcFormat, c.copyDstFormat,
        c.omColor, c.omColorFormat, c.barrierResource, c.barrierFormat,
        c.barrierBefore, c.barrierAfter, c.present, c.ecl,
        currentPresent, currentEcl);
}

static void LogCommandListShimCounter(const char* method, LONG64 count,
                                      ID3D12GraphicsCommandList* list,
                                      CommandListShim* shim)
{
    if (count <= 5 || (count % 100000) == 0)
        Log("native-usage: invocation method=%s count=%lld list=%p type=%u originalVtbl=%p clonedVtbl=%p present=%llu ecl=%llu",
            method, (long long)count, (void*)list,
            shim ? (unsigned)shim->listType : 0xFFFFFFFFu,
            shim ? (void*)shim->originalVtbl : nullptr,
            shim ? (void*)shim->clonedVtbl : nullptr,
            (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
            (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
}

static CommandListShim* FindCommandListShim(ID3D12GraphicsCommandList* list)
{
    if (!list) return nullptr;
    void** vtbl = nullptr;
    __try { vtbl = *(void***)list; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    AcquireSRWLockShared(&g_commandListShimLock);
    CommandListShim* result = nullptr;
    for (auto& shim : g_commandListShims) {
        if (shim.list == list && shim.clonedVtbl == vtbl) { result = &shim; break; }
    }
    ReleaseSRWLockShared(&g_commandListShimLock);
    return result;
}

// Seen-check for the ECL submission path (replaces the unlocked s_hookedLists
// vector gate, whose scan/push_back raced on concurrent submissions). True if
// any entry already names this list, REGARDLESS of vtable, so foreign tables
// are preserved and table use stays bounded exactly as the old gate behaved.
// Shared-lock scan released before InstallCommandListHooks takes exclusive:
// sequential, never nested, so no lock-order inversion. No COM ref retained.
static bool WasListHooked(ID3D12GraphicsCommandList* list)
{
    if (!list) return true;
    AcquireSRWLockShared(&g_commandListShimLock);
    bool seen = false;
    for (auto& existing : g_commandListShims) {
        if (existing.list == list) { seen = true; break; }
    }
    ReleaseSRWLockShared(&g_commandListShimLock);
    return seen;
}

// If a third party replaces a list's vtable after we installed our clone,
// wrappers must still forward exactly once. Prefer the currently installed
// method when it is not one of our wrappers; if it is our wrapper, recover the
// saved original by pointer identity. This helper never retains the COM object.
static void* ResolveCommandListForward(ID3D12GraphicsCommandList* list,
                                      UINT slot, void* ourShim)
{
    if (!list || slot >= 64) return nullptr;
    void** actual = nullptr;
    void* target = nullptr;
    __try {
        actual = *(void***)list;
        if (actual) target = actual[slot];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        actual = nullptr;
        target = nullptr;
    }
    if (target && target != ourShim) return target;

    void* saved = nullptr;
    AcquireSRWLockShared(&g_commandListShimLock);
    for (auto& shim : g_commandListShims) {
        if (shim.list != list || !shim.originalVtbl) continue;
        __try { saved = shim.originalVtbl[slot]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { saved = nullptr; }
        if (saved && saved != ourShim) break;
    }
    ReleaseSRWLockShared(&g_commandListShimLock);
    return saved;
}

struct MvRecordingSnapshot {
    bool found;
    bool closed;
    bool stable;
    bool resetInProgress;
    bool resetFailed;
    bool clonedVtblMatches;
    void** actualVtbl;
    void** expectedClonedVtbl;
    void** expectedOriginalVtbl;
    UINT listType;
    LONG64 epoch;
    ID3D12Resource* target;
    LONG width;
    LONG height;
    LONG64 omCount;
    LONG64 drawCount;
    LONG64 indexedDrawCount;
    LONG64 dispatchCount;
    LONG64 recordPresent;
    LONG64 recordEcl;
};

static bool CaptureMvRecordingSnapshot(ID3D12GraphicsCommandList* list,
                                       MvRecordingSnapshot* out)
{
    if (!list || !out) return false;
    MvRecordingSnapshot snapshot = {};
    void** actualVtbl = nullptr;
    __try { actualVtbl = *(void***)list; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    AcquireSRWLockShared(&g_commandListShimLock);
    CommandListShim* selectedShim = nullptr;
    CommandListShim* historicalShim = nullptr;
    for (auto& shim : g_commandListShims) {
        if (shim.list != list || !shim.clonedVtbl) continue;
        if (!historicalShim) historicalShim = &shim;
        if (actualVtbl == shim.clonedVtbl) {
            selectedShim = &shim;
            break;
        }
    }
    // Prefer the attachment generation that owns the current vtable. If a
    // third party has replaced it, retain the oldest available snapshot only
    // as explicitly historical mismatch evidence.
    CommandListShim* shim = selectedShim ? selectedShim : historicalShim;
    if (shim) {
        snapshot.found = true;
        LONG resetBefore = InterlockedCompareExchange(&shim->recordingResetInProgress, 0, 0);
        snapshot.resetInProgress = resetBefore != 0;
        snapshot.resetFailed = InterlockedCompareExchange(&shim->recordingResetFailed, 0, 0) != 0;
        snapshot.closed = InterlockedCompareExchange(&shim->recordingClosed, 0, 0) != 0;
        snapshot.clonedVtblMatches = actualVtbl == shim->clonedVtbl;
        snapshot.actualVtbl = actualVtbl;
        snapshot.expectedClonedVtbl = shim->clonedVtbl;
        snapshot.expectedOriginalVtbl = shim->originalVtbl;
        snapshot.listType = shim->listType;
        snapshot.epoch = InterlockedCompareExchange64(&shim->recordingEpoch, 0, 0);
        snapshot.target = (ID3D12Resource*)InterlockedCompareExchangePointer(
            (PVOID volatile*)&shim->recordedMvTarget, nullptr, nullptr);
        snapshot.width = InterlockedCompareExchange(&shim->mvTargetWidth, 0, 0);
        snapshot.height = InterlockedCompareExchange(&shim->mvTargetHeight, 0, 0);
        snapshot.omCount = InterlockedCompareExchange64(&shim->mvTargetOmCount, 0, 0);
        snapshot.drawCount = InterlockedCompareExchange64(&shim->mvTargetDrawCount, 0, 0);
        snapshot.indexedDrawCount = InterlockedCompareExchange64(&shim->mvTargetIndexedDrawCount, 0, 0);
        snapshot.dispatchCount = InterlockedCompareExchange64(&shim->dispatchCount, 0, 0);
        snapshot.recordPresent = InterlockedCompareExchange64(&shim->mvTargetRecordPresent, 0, 0);
        snapshot.recordEcl = InterlockedCompareExchange64(&shim->mvTargetRecordEcl, 0, 0);
        snapshot.stable = !snapshot.resetInProgress && !snapshot.resetFailed && snapshot.closed &&
            InterlockedCompareExchange(&shim->recordingResetInProgress, 0, 0) == 0 &&
            InterlockedCompareExchange(&shim->recordingResetFailed, 0, 0) == 0 &&
            InterlockedCompareExchange64(&shim->recordingEpoch, 0, 0) == snapshot.epoch &&
            InterlockedCompareExchange(&shim->recordingClosed, 0, 0) != 0;
    }
    ReleaseSRWLockShared(&g_commandListShimLock);
    *out = snapshot;
    return snapshot.found;
}

static volatile LONG64 g_mvRecordingSubmittedTotal = 0;
static volatile LONG64 g_mvRecordingSubmittedDetail = 0;
static volatile LONG64 g_mvRecordingDrawTotal = 0;
static volatile LONG64 g_mvRecordingOmSetTotal = 0;
static volatile LONG64 g_mvRecordingQualifyingOmTotal = 0;
static volatile LONG64 g_mvRecordingResetTotal = 0;
static volatile LONG64 g_mvRecordingResetSuccessTotal = 0;
static volatile LONG64 g_mvRecordingResetFailTotal = 0;
static volatile LONG64 g_mvRecordingCloseTotal = 0;
static volatile LONG64 g_mvRecordingCloseSuccessTotal = 0;
static volatile LONG64 g_mvRecordingCloseFailTotal = 0;
static volatile LONG64 g_mvRecordingCloneMismatchTotal = 0;

static void NoteMvOmTarget(ID3D12GraphicsCommandList* list,
                           CommandListShim* shim, UINT count,
                           const D3D12_CPU_DESCRIPTOR_HANDLE* handles)
{
    if (!shim) return;
    ID3D12Resource* candidate = nullptr;
    D3D12_RESOURCE_DESC candidateDesc = {};
    // Track the final qualifying RTV in the OM set. This only records a
    // requested bind; it does not infer draw output or resource contents.
    for (UINT i = 0; handles && i < count && i < 8; ++i) {
        if (!handles[i].ptr) continue;
        ID3D12Resource* resource = nullptr;
        {
            BookGuard guard;
            auto it = g_rtvMap.find(handles[i].ptr);
            if (it != g_rtvMap.end()) resource = it->second;
        }
        if (!resource) continue;
        D3D12_RESOURCE_DESC desc = {};
        if (!SafeGetDesc(resource, &desc)) continue;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.Format == DXGI_FORMAT_R16G16_FLOAT && desc.Width >= 1000 &&
            desc.Height >= 500 && desc.SampleDesc.Count == 1) {
            candidate = resource;
            candidateDesc = desc;
        }
    }

    InterlockedExchangePointer((PVOID volatile*)&shim->currentOmMvTarget,
                               candidate);
    if (!candidate) return;

    InterlockedExchangePointer((PVOID volatile*)&shim->recordedMvTarget,
                               candidate);
    InterlockedIncrement64(&g_mvRecordingQualifyingOmTotal);
    InterlockedExchange(&shim->mvTargetWidth, (LONG)candidateDesc.Width);
    InterlockedExchange(&shim->mvTargetHeight, (LONG)candidateDesc.Height);
    InterlockedIncrement64(&shim->mvTargetOmCount);
    InterlockedExchange64(&shim->mvTargetRecordPresent,
        InterlockedCompareExchange64(&g_presentSerial, 0, 0));
    InterlockedExchange64(&shim->mvTargetRecordEcl,
        InterlockedCompareExchange64(&g_eclSerial, 0, 0));

    static volatile LONG s_mvOmDetails = 0;
    LONG detail = InterlockedIncrement(&s_mvOmDetails);
    if (detail <= 12 || (detail % 1000) == 0)
        Log("mv-record: om-target list=%p type=%u target=%p size=%ux%u fmt=%u epoch=%lld present=%llu ecl=%llu detail=%ld",
            (void*)list, shim->listType, (void*)candidate,
            (unsigned)candidateDesc.Width, (unsigned)candidateDesc.Height,
            (unsigned)candidateDesc.Format,
            (long long)InterlockedCompareExchange64(&shim->recordingEpoch, 0, 0),
            (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
            (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0),
            detail);
}

// Diagnostic only: find a registry entry by list pointer alone, regardless of
// current vtable.Reports the actual vtable vs the expected cloned/original so a
// miss can be classified as vtable-restored vs never-registered. No behavior change.
static bool FindShimByListOnly(ID3D12GraphicsCommandList* list, void** actualVtblOut,
                               void** expectedClonedOut, void** expectedOriginalOut)
{
    if (!list) return false;
    void** vtbl = nullptr;
    __try { vtbl = *(void***)list; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (actualVtblOut) *actualVtblOut = vtbl;
    bool found = false;
    AcquireSRWLockShared(&g_commandListShimLock);
    for (auto& shim : g_commandListShims) {
        if (shim.list == list) {
            if (expectedClonedOut) *expectedClonedOut = shim.clonedVtbl;
            if (expectedOriginalOut) *expectedOriginalOut = shim.originalVtbl;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_commandListShimLock);
    return found;
}

// Diagnostic only: clone-integrity snapshot for a registered-but-mismatched list.
// Copies registry fields under the shared lock, releases it, then performs only
// guarded reads (no writes, no reinstall, no returned entry pointers):
// - whether our cloned table is still readable and slot 15 still equals our shim;
// - the actual vtable's memory region (VirtualQuery: base/size/protect/state/type);
// - the actual slot-15 target and which module it resides in (FROM_ADDRESS lookup,
//   refcount unchanged).Heap residency alone is reported as an observation.
static void STDMETHODCALLTYPE Shim_CopyBufferRegion(
    ID3D12GraphicsCommandList* list, ID3D12Resource* dst, UINT64 dstOffset,
    ID3D12Resource* src, UINT64 srcOffset, UINT64 numBytes);
struct ShimIntegritySnapshot {
    void* list;
    void* expectedOriginal;
    void* expectedCloned;
    void* actualVtbl;
    bool clonedReadable;
    bool cloneSlot15IsShim;
    void* cloneSlot15Value;
    bool actualRegionValid;
    void* actualRegionBase;
    SIZE_T actualRegionSize;
    DWORD actualProtect;
    DWORD actualState;
    DWORD actualType;
    bool actualSlot15Readable;
    void* actualSlot15Value;
    void* actualSlot15Module;
    // Submitted base-interface pointer (ID3D12CommandList*, no extra QI, no
    // refcount change, never retained): distinguishes which subobject the game
    // submits from the derived pointer we shimmed.
    void* baseList;
    bool baseVtblReadable;
    void* baseVtbl;
    bool baseRegionValid;
    void* baseRegionBase;
    SIZE_T baseRegionSize;
    DWORD baseProtect;
    // Slot-by-slot comparison across the three tables (verified SDK slots:
    // Close=9, Reset=10, CopyBufferRegion=15, CopyTextureRegion=16,
    // ResourceBarrier=26, OMSetRenderTargets=46). Module basename recorded
    // only for actual entries differing from both saved tables.
    bool slotsReadable;
    void* actualSlot[6];
    void* origSlot[6];
    void* clonedSlot[6];
    wchar_t slotModBase[6][40];
};

// Diagnostic only: classify one actual vtable entry against the saved tables.
// Returns a one-character string (Log has no %c): "B" matches both (untouched
// slot), "O" original only, "C" clone only, "X" differs from both (module
// basename logged alongside when available).
static const char* SlotClass(void* actual, void* orig, void* cloned)
{
    bool isO = (actual == orig);
    bool isC = (actual == cloned);
    if (isO && isC) return "B";
    if (isO) return "O";
    if (isC) return "C";
    return "X";
}

static bool CaptureShimIntegrity(ID3D12GraphicsCommandList* list, ID3D12CommandList* baseList,
                                 ShimIntegritySnapshot* out)
{
    if (!list || !out) return false;
    void** actual = nullptr;
    __try { actual = *(void***)list; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    void* cloned = nullptr;
    void* original = nullptr;
    AcquireSRWLockShared(&g_commandListShimLock);
    for (auto& shim : g_commandListShims) {
        if (shim.list == list) {
            cloned = shim.clonedVtbl;
            original = shim.originalVtbl;
            break;
        }
    }
    ReleaseSRWLockShared(&g_commandListShimLock);
    if (!cloned) return false;
    out->list = (void*)list;
    out->expectedOriginal = original;
    out->expectedCloned = cloned;
    out->actualVtbl = (void*)actual;
    out->clonedReadable = false;
    out->cloneSlot15IsShim = false;
    out->cloneSlot15Value = nullptr;
    out->actualRegionValid = false;
    out->actualRegionBase = nullptr;
    out->actualRegionSize = 0;
    out->actualProtect = 0;
    out->actualState = 0;
    out->actualType = 0;
    out->actualSlot15Readable = false;
    out->actualSlot15Value = nullptr;
    out->actualSlot15Module = nullptr;
    out->baseList = (void*)baseList;
    out->baseVtblReadable = false;
    out->baseVtbl = nullptr;
    out->baseRegionValid = false;
    out->baseRegionBase = nullptr;
    out->baseRegionSize = 0;
    out->baseProtect = 0;
    __try {
        void** ct = (void**)cloned;
        if (ct) {
            out->cloneSlot15Value = ct[15];
            out->clonedReadable = true;
            out->cloneSlot15IsShim = (ct[15] == (void*)&Shim_CopyBufferRegion);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (actual) {
        MEMORY_BASIC_INFORMATION mbi = {};
        SIZE_T qr = VirtualQuery((LPCVOID)actual, &mbi, sizeof(mbi));
        if (qr == sizeof(mbi)) {
            out->actualRegionValid = true;
            out->actualRegionBase = mbi.BaseAddress;
            out->actualRegionSize = mbi.RegionSize;
            out->actualProtect = mbi.Protect;
            out->actualState = mbi.State;
            out->actualType = mbi.Type;
        }
        __try {
            void** at = (void**)actual;
            if (at) {
                out->actualSlot15Value = at[15];
                out->actualSlot15Readable = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        if (out->actualSlot15Readable && out->actualSlot15Value) {
            HMODULE hm = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)out->actualSlot15Value, &hm) && hm) {
                out->actualSlot15Module = (void*)hm;
            }
        }
    }
    // Submitted base-interface pointer: guarded vtable read + region only.
    // No QI, no refcount change, never retained. A base address different from
    // the derived pointer proves distinct subobjects; equal addresses with
    // different vtables prove a per-subobject vtable split.
    if (baseList) {
        void** bt = nullptr;
        __try { bt = *(void***)baseList; } __except (EXCEPTION_EXECUTE_HANDLER) { bt = nullptr; }
        if (bt) {
            out->baseVtbl = (void*)bt;
            out->baseVtblReadable = true;
            MEMORY_BASIC_INFORMATION mbi = {};
            SIZE_T qr = VirtualQuery((LPCVOID)bt, &mbi, sizeof(mbi));
            if (qr == sizeof(mbi)) {
                out->baseRegionValid = true;
                out->baseRegionBase = mbi.BaseAddress;
                out->baseRegionSize = mbi.RegionSize;
                out->baseProtect = mbi.Protect;
            }
        }
    }
    // Slot-by-slot table comparison. All three reads guarded as one unit: if
    // any table faults, the comparison is reported unreadable rather than
    // crashing. Module basename uses only manual loops (no CRT string calls).
    static const int kCmpSlots[6] = { 9, 10, 15, 16, 26, 46 };
    out->slotsReadable = false;
    for (int i = 0; i < 6; ++i) {
        out->actualSlot[i] = nullptr;
        out->origSlot[i] = nullptr;
        out->clonedSlot[i] = nullptr;
        out->slotModBase[i][0] = L'\0';
    }
    if (actual && original && cloned) {
        __try {
            void** at = (void**)actual;
            void** ot = (void**)original;
            void** ct = (void**)cloned;
            for (int i = 0; i < 6; ++i) {
                out->actualSlot[i] = at[kCmpSlots[i]];
                out->origSlot[i] = ot[kCmpSlots[i]];
                out->clonedSlot[i] = ct[kCmpSlots[i]];
            }
            out->slotsReadable = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    if (out->slotsReadable) {
        for (int i = 0; i < 6; ++i) {
            void* av = out->actualSlot[i];
            if (!av || av == out->origSlot[i] || av == out->clonedSlot[i])
                continue;
            HMODULE hm = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCWSTR)av, &hm) || !hm)
                continue;
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameW(hm, path, MAX_PATH) == 0)
                continue;
            int lastSep = -1;
            for (int k = 0; path[k]; ++k) {
                if (path[k] == L'\\' || path[k] == L'/')
                    lastSep = k;
            }
            int o = 0;
            for (int k = lastSep + 1; path[k] && o < 39; ++k, ++o)
                out->slotModBase[i][o] = path[k];
            out->slotModBase[i][o] = L'\0';
        }
    }
    return true;
}

static bool IsNativeUsageSize(ID3D12Resource* resource, D3D12_RESOURCE_DESC* outDesc)
{
    if (!resource) return false;
    __try {
        D3D12_RESOURCE_DESC desc = resource->GetDesc();
        if (outDesc) *outDesc = desc;
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
               desc.Width >= 1000 && desc.Height >= 500;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeGetDesc(ID3D12Resource* r, D3D12_RESOURCE_DESC* out)
{
    if (!r || !out) return false;
    __try { *out = r->GetDesc(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static UINT64 SafeGetFenceCompleted(ID3D12Fence* f)
{
    if (!f) return 0;
    __try { return f->GetCompletedValue(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool SafeOverwriteRTV(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (!res || !g_device) return false;
    __try { g_device->CreateRenderTargetView(res, nullptr, handle); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void ObservePersistentSceneColor(unsigned long long presentSerial)
{
    // Sampling once per 60 Presents gives us lifetime/recurrence evidence
    // without adding a meaningful amount of work to the Present path.
    if (presentSerial == 0 || (presentSerial % 60) != 1)
        return;

    ID3D12Resource* resources[2] = { g_sceneColor, g_sceneColorAlt };
    for (ID3D12Resource* resource : resources) {
        if (!resource || resource == g_bbCached)
            continue;

        D3D12_RESOURCE_DESC desc = {};
        if (!IsNativeUsageSize(resource, &desc))
            continue;
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_UNORM &&
            desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
            continue;
        if (g_displayW && g_displayH &&
            (desc.Width != g_displayW || desc.Height != g_displayH))
            continue;

        AcquireSRWLockExclusive(&g_sceneColorCandidateLock);
        SceneColorCandidateHistory* history = nullptr;
        for (auto& candidate : g_sceneColorCandidates) {
            if (candidate.resource == resource && candidate.format == (UINT)desc.Format) {
                history = &candidate;
                break;
            }
        }
        if (!history) {
            for (auto& candidate : g_sceneColorCandidates) {
                if (!candidate.resource) {
                    history = &candidate;
                    candidate.resource = resource;
                    candidate.format = (UINT)desc.Format;
                    candidate.width = (UINT)desc.Width;
                    candidate.height = (UINT)desc.Height;
                    candidate.firstPresent = presentSerial;
                    break;
                }
            }
        }
        if (history && history->lastSamplePresent != presentSerial) {
            history->lastSamplePresent = presentSerial;
            history->lastPresent = presentSerial;
            ++history->samples;
            unsigned persistent = history->samples >= 3 ? 1u : 0u;
            LONG64 previous = InterlockedExchange64(&g_sceneColorCandidateLastLog,
                                                    (LONG64)presentSerial);
            // Always report the first observation and promotion to persistent;
            // afterward report at most once per 600 Presents globally.
            bool important = history->samples <= 3 || previous == 0 ||
                             presentSerial >= (unsigned long long)previous + 600;
            if (important) {
                Log("native-target: scene-color-observed persistent=%u samples=%u resource=%p size=%ux%u fmt=%u firstPresent=%llu lastPresent=%llu display=%ux%u backbuffer=%p",
                    persistent, history->samples, resource,
                    history->width, history->height, history->format,
                    history->firstPresent, history->lastPresent,
                    g_displayW, g_displayH, (void*)g_bbCached);
            }
        }
        ReleaseSRWLockExclusive(&g_sceneColorCandidateLock);
    }
}

static void ObserveNativeCopyUsage(ID3D12GraphicsCommandList* list,
                                   CommandListShim* shim,
                                   const D3D12_TEXTURE_COPY_LOCATION* dst,
                                   const D3D12_TEXTURE_COPY_LOCATION* src)
{
    if (!dst || !src || !dst->pResource || !src->pResource) return;
    D3D12_RESOURCE_DESC dd = {}, sd = {};
    bool dstLarge = IsNativeUsageSize(dst->pResource, &dd);
    bool srcLarge = IsNativeUsageSize(src->pResource, &sd);
    if (!dstLarge && !srcLarge) return;
    static LONG s_logs = 0;
    LONG n = InterlockedIncrement(&s_logs);
    unsigned copyOnList = shim ? ++shim->copyCount : 0;
    unsigned long long present = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    unsigned long long ecl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
    AcquireSRWLockExclusive(&g_nativeCorrelationLock);
    BeginNativeBatchLocked(g_nativeCorrelation, present, ecl);
    g_nativeCorrelation.copySrc = (void*)src->pResource;
    g_nativeCorrelation.copyDst = (void*)dst->pResource;
    g_nativeCorrelation.copySrcFormat = (UINT)sd.Format;
    g_nativeCorrelation.copyDstFormat = (UINT)dd.Format;
    g_nativeCorrelation.present = present;
    g_nativeCorrelation.ecl = ecl;
    ++g_nativeCorrelation.generation;
    g_nativeCorrelation.batchCopySrc = (void*)src->pResource;
    g_nativeCorrelation.batchCopyDst = (void*)dst->pResource;
    g_nativeCorrelation.batchCopySrcFormat = (UINT)sd.Format;
    g_nativeCorrelation.batchCopyDstFormat = (UINT)dd.Format;
    g_nativeCorrelation.batchCopySeen = 1;
    MaybeLogCoherentNativeBatchLocked(g_nativeCorrelation);
    ReleaseSRWLockExclusive(&g_nativeCorrelationLock);
    if (n <= 80 || (n % 600) == 0)
        Log("native-usage: copy #%d list=%p type=%u listCopy=%u createdPresent=%llu createdEcl=%llu src=%p %llux%u fmt=%u dst=%p %llux%u fmt=%u present=%llu ecl=%llu presentMinusCreate=%lld",
            (int)n, (void*)list,
            shim ? (unsigned)shim->listType : 0xFFFFFFFFu, copyOnList,
            shim ? shim->createdPresent : 0, shim ? shim->createdEcl : 0,
            (void*)src->pResource,
            (unsigned long long)sd.Width, (unsigned)sd.Height, (unsigned)sd.Format,
            (void*)dst->pResource, (unsigned long long)dd.Width, (unsigned)dd.Height,
            (unsigned)dd.Format,
            present, ecl, (long long)(present - (shim ? shim->createdPresent : present)));
}

static void ObserveNativeOmUsage(ID3D12GraphicsCommandList* list, UINT count,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                                 BOOL singleRange,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
{
    if (!handles || count == 0) return;
    for (UINT i = 0; i < count && i < 8; ++i) {
        ID3D12Resource* resource = nullptr;
        bool mapped = false;
        { BookGuard guard; auto it = g_rtvMap.find(handles[i].ptr); if (it != g_rtvMap.end()) { resource = it->second; mapped = true; } }
        static LONG s_rawLogs = 0;
        static LONG s_colorLogs = 0;
        LONG raw = InterlockedIncrement(&s_rawLogs);
        LONG color = handles[i].ptr ? InterlockedIncrement(&s_colorLogs) : 0;
        if (raw <= 80 || (handles[i].ptr && color <= 160))
            Log("native-usage: om-raw #%d colorSample=%d list=%p slot=%u count=%u singleRange=%u handle=%llX mapped=%u depth=%llX present=%llu ecl=%llu",
                (int)raw, (int)color, (void*)list, (unsigned)i, (unsigned)count,
                singleRange ? 1u : 0u,
                (unsigned long long)handles[i].ptr, mapped ? 1u : 0u,
                (unsigned long long)(depth ? depth->ptr : 0),
                (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
                (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
        D3D12_RESOURCE_DESC desc = {};
        if (!IsNativeUsageSize(resource, &desc)) continue;
        unsigned long long present = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
        unsigned long long ecl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
        AcquireSRWLockExclusive(&g_nativeCorrelationLock);
        BeginNativeBatchLocked(g_nativeCorrelation, present, ecl);
        g_nativeCorrelation.omColor = (void*)resource;
        g_nativeCorrelation.omColorFormat = (UINT)desc.Format;
        g_nativeCorrelation.present = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
        g_nativeCorrelation.ecl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
        ++g_nativeCorrelation.generation;
        g_nativeCorrelation.batchColor = (void*)resource;
        g_nativeCorrelation.batchColorFormat = (UINT)desc.Format;
        g_nativeCorrelation.batchOmSeen = 1;
        MaybeLogCoherentNativeBatchLocked(g_nativeCorrelation);
        ReleaseSRWLockExclusive(&g_nativeCorrelationLock);
        static LONG s_logs = 0;
        LONG n = InterlockedIncrement(&s_logs);
        if (n <= 80 || (n % 600) == 0)
            Log("native-usage: om #%d list=%p slot=%u handle=%llX resource=%p %llux%u fmt=%u present=%llu",
                (int)n, (void*)list, (unsigned)i,
                (unsigned long long)handles[i].ptr, (void*)resource,
                (unsigned long long)desc.Width, (unsigned)desc.Height,
                (unsigned)desc.Format,
                (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0));
    }
}

static void ObserveNativeBarrierUsage(ID3D12GraphicsCommandList* list, UINT count,
                                      const D3D12_RESOURCE_BARRIER* barriers)
{
    if (!barriers) return;
    for (UINT i = 0; i < count; ++i) {
        if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        ID3D12Resource* resource = barriers[i].Transition.pResource;
        D3D12_RESOURCE_DESC desc = {};
        if (!IsNativeUsageSize(resource, &desc)) continue;
        unsigned long long present = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
        unsigned long long ecl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
        AcquireSRWLockExclusive(&g_nativeCorrelationLock);
        BeginNativeBatchLocked(g_nativeCorrelation, present, ecl);
        g_nativeCorrelation.barrierResource = (void*)resource;
        g_nativeCorrelation.barrierFormat = (UINT)desc.Format;
        g_nativeCorrelation.barrierBefore = (UINT)barriers[i].Transition.StateBefore;
        g_nativeCorrelation.barrierAfter = (UINT)barriers[i].Transition.StateAfter;
        g_nativeCorrelation.present = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
        g_nativeCorrelation.ecl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
        ++g_nativeCorrelation.generation;
        g_nativeCorrelation.batchBarrierResource = (void*)resource;
        g_nativeCorrelation.batchBarrierFormat = (UINT)desc.Format;
        g_nativeCorrelation.batchBarrierBefore = (UINT)barriers[i].Transition.StateBefore;
        g_nativeCorrelation.batchBarrierAfter = (UINT)barriers[i].Transition.StateAfter;
        g_nativeCorrelation.batchBarrierSeen = 1;
        MaybeLogCoherentNativeBatchLocked(g_nativeCorrelation);
        ReleaseSRWLockExclusive(&g_nativeCorrelationLock);
        static LONG s_logs = 0;
        LONG n = InterlockedIncrement(&s_logs);
        if (n <= 80 || (n % 600) == 0)
            Log("native-usage: barrier #%d list=%p resource=%p %llux%u fmt=%u before=%u after=%u present=%llu",
                (int)n, (void*)list, (void*)resource,
                (unsigned long long)desc.Width, (unsigned)desc.Height,
                (unsigned)desc.Format,
                (unsigned)barriers[i].Transition.StateBefore,
                (unsigned)barriers[i].Transition.StateAfter,
                (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0));
    }
}

static bool TryVectorA(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY, UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* srcBox, CommandListShim* shim);
static bool TryVectorB(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL singleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* depth, CommandListShim* shim);
static void Hook_CopyBufferRegion(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset, UINT64 numBytes);
static void InstallResourceShim(ID3D12Resource* resource, size_t cbSize);
// Viewport/scissor hooks are analysis + single-forward via the passed-in
// per-list original (the Real_* globals are never assigned: two driver
// tables exist, so forwarding must stay per-list). Return value reports
// whether the engine call was recorded, so the Shim_ wrapper forwards at
// most once. Both functions are defined later in this TU.
bool Hook_RSSetViewports(ID3D12GraphicsCommandList* list, UINT numViewports,
    const D3D12_VIEWPORT* pViewports, PFN_RSSetViewports realFn);
bool Hook_RSSetScissorRects(ID3D12GraphicsCommandList* list, UINT numRects,
    const D3D12_RECT* pRects, PFN_RSSetScissorRects realFn);
// Legacy-path reconnect (no new interception mechanism): the installed
// command-list shims below route to these analysis bodies, which previously
// lived only in disconnected Hook_* functions.
static void CopyTexBody(ID3D12GraphicsCommandList* list,
    const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY,
    UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src,
    const D3D12_BOX* srcBox);
static void TrackResourceBarriers(UINT numBarriers, const D3D12_RESOURCE_BARRIER* pBarriers);
static ID3D12Resource* TrackOMBind(ID3D12GraphicsCommandList* list, UINT numRenderTargets,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargets,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencil);

static void STDMETHODCALLTYPE Shim_CopyTextureRegion(
    ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst,
    UINT dstX, UINT dstY, UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src,
    const D3D12_BOX* srcBox)
{
    CommandListShim* shim = FindCommandListShim(list);
    LONG64 invocation = InterlockedIncrement64(&g_shimCopyCalls);
    LogCommandListShimCounter("CopyTextureRegion", invocation, list, shim);
    ObserveNativeCopyUsage(list, shim, dst, src);
    // LEGACY RECONNECT: run the legacy trigger/scene/depth analysis on the
    // engine's intent BEFORE any vector substitution or forwarding. CopyTexBody
    // forwards only via null-guarded Real_* (no-ops here); the shim forwards
    // below exactly once, so recording behavior is unchanged.
    CopyTexBody(list, dst, dstX, dstY, dstZ, src, srcBox);
    if (TryVectorA(list, dst, dstX, dstY, dstZ, src, srcBox, shim)) return;
    if (shim && shim->copyTexture)
        shim->copyTexture(list, dst, dstX, dstY, dstZ, src, srcBox);
}

// Whole-resource copy shim (SDK-verified slot 17). OBSERVE-ONLY: counts the
// call, records endpoint identity, forwards exactly once through the
// per-list original. No camera validation, scene selection, injection, or
// patching - answers only whether CopyResource moves scene candidates.
static void STDMETHODCALLTYPE Shim_CopyResource(
    ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src)
{
    static unsigned s_entryDiag = 0;
    if ((++s_entryDiag % 600) == 1) {
        Log("hooks: Shim_CopyResource ENTRY list=%p dst=%p src=%p",
            (void*)list, (void*)dst, (void*)src);
    }
    CommandListShim* shim = FindCommandListShim(list);
    LONG64 invocation = InterlockedIncrement64(&g_shimCopyResCalls);
    LogCommandListShimCounter("CopyResource", invocation, list, shim);
    // Endpoint identity by pointer only (descriptor similarity never counts).
    // Guarded reads; a fault ends the record for that endpoint.
    {
        static volatile LONG s_detail = 0;
        LONG n = InterlockedIncrement(&s_detail);
        if (n <= 10 || (n % 500) == 0) {
            int srcScene = (src && (src == g_sceneColor ||
                (g_sceneColorAlt && src == g_sceneColorAlt))) ? 1 : 0;
            int dstScene = (dst && (dst == g_sceneColor ||
                (g_sceneColorAlt && dst == g_sceneColorAlt))) ? 1 : 0;
            int srcSet = SceneSetContains(src) ? 1 : 0;
            int dstSet = SceneSetContains(dst) ? 1 : 0;
            D3D12_RESOURCE_DESC sd = {}, dd = {};
            bool gotS = (src && SafeGetDesc(src, &sd));
            bool gotD = (dst && SafeGetDesc(dst, &dd));
            // Full descriptor record for creation-filter comparison
            // (dimension/mips/samples decide filter inclusion; W/H/fmt alone
            // cannot). Diagnostic only; no selection effect.
            Log("hooks: CopyResource #%d list=%p dst=%p src=%p dstScene=%d srcScene=%d dstSet=%d srcSet=%d dstDesc=%ux%u fmt=%u srcDesc=%ux%u fmt=%u",
                (int)n, (void*)list, (void*)dst, (void*)src,
                dstScene, srcScene, dstSet, srcSet,
                gotD ? (unsigned)dd.Width : 0u, gotD ? (unsigned)dd.Height : 0u,
                gotD ? (unsigned)dd.Format : 0u,
                gotS ? (unsigned)sd.Width : 0u, gotS ? (unsigned)sd.Height : 0u,
                gotS ? (unsigned)sd.Format : 0u);
            Log("hooks: CopyResource #%d fulldesc dst dim=%u depth=%u mips=%u samp=%u/%u layout=%u flags=0x%X src dim=%u depth=%u mips=%u samp=%u/%u layout=%u flags=0x%X",
                (int)n,
                gotD ? (unsigned)dd.Dimension : 0u,
                gotD ? (unsigned)dd.DepthOrArraySize : 0u, gotD ? (unsigned)dd.MipLevels : 0u,
                gotD ? (unsigned)dd.SampleDesc.Count : 0u, gotD ? (unsigned)dd.SampleDesc.Quality : 0u,
                gotD ? (unsigned)dd.Layout : 0u, gotD ? (unsigned)dd.Flags : 0u,
                gotS ? (unsigned)sd.Dimension : 0u,
                gotS ? (unsigned)sd.DepthOrArraySize : 0u, gotS ? (unsigned)sd.MipLevels : 0u,
                gotS ? (unsigned)sd.SampleDesc.Count : 0u, gotS ? (unsigned)sd.SampleDesc.Quality : 0u,
                gotS ? (unsigned)sd.Layout : 0u, gotS ? (unsigned)sd.Flags : 0u);
        }
    }
    if (shim && shim->copyResource)
        shim->copyResource(list, dst, src);
}

// Graphics root descriptor table shim (SDK-verified slot 32). OBSERVE-ONLY
// bind timing: which root parameter is bound with which GPU handle value, on
// which list, at which present/ECL serial - for offline correlation with OM,
// viewport, and barrier windows. The handle VALUE is logged; heap contents
// are never read and the referenced resource is never resolved here (that
// would require descriptor-heap content reads, explicitly out of scope).
// Forwards exactly once through the per-list original.
static void STDMETHODCALLTYPE Shim_SetGraphicsRootDescriptorTable(
    ID3D12GraphicsCommandList* list, UINT rootParam,
    D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
    static unsigned s_entryDiag = 0;
    if ((++s_entryDiag % 600) == 1) {
        Log("hooks: Shim_SetGraphicsRootDescriptorTable ENTRY list=%p root=%u gpu=%llX",
            (void*)list, rootParam, (unsigned long long)baseDescriptor.ptr);
    }
    CommandListShim* shim = FindCommandListShim(list);
    LONG64 invocation = InterlockedIncrement64(&g_shimRootTableCalls);
    LogCommandListShimCounter("SetGraphicsRootDescriptorTable", invocation, list, shim);
    {
        static volatile LONG s_detail = 0;
        LONG n = InterlockedIncrement(&s_detail);
        if (n <= 12 || (n % 500) == 0) {
            Log("hooks: roottable #%d list=%p type=%u root=%u gpu=%llX present=%llu ecl=%llu",
                (int)n, (void*)list,
                shim ? (unsigned)shim->listType : 0xFFFFFFFFu, rootParam,
                (unsigned long long)baseDescriptor.ptr,
                (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
                (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
        }
    }
    if (shim && shim->setGraphicsRootDescriptorTable)
        shim->setGraphicsRootDescriptorTable(list, rootParam, baseDescriptor);
}

static void STDMETHODCALLTYPE Shim_CopyBufferRegion(
    ID3D12GraphicsCommandList* list, ID3D12Resource* dst, UINT64 dstOffset,
    ID3D12Resource* src, UINT64 srcOffset, UINT64 numBytes)
{
    static unsigned s_entryDiag = 0;
    if ((++s_entryDiag % 600) == 1) {
        Log("hooks: Shim_CopyBufferRegion ENTRY list=%p dst=%p src=%p numBytes=%llu",
            (void*)list, (void*)dst, (void*)src, (unsigned long long)numBytes);
    }
    CommandListShim* shim = FindCommandListShim(list);
    LONG64 invocation = InterlockedIncrement64(&g_shimBufferCopyCalls);
    LogCommandListShimCounter("CopyBufferRegion", invocation, list, shim);
    // Delegate to the existing hook logic which validates/patches camera/velocity CBs
    Hook_CopyBufferRegion(list, dst, dstOffset, src, srcOffset, numBytes);
    if (shim && shim->copyBufferRegion)
        shim->copyBufferRegion(list, dst, dstOffset, src, srcOffset, numBytes);
}

// Resource Map/Unmap shim for constant buffer patching
static HRESULT STDMETHODCALLTYPE Shim_ResourceMap(ID3D12Resource* resource, UINT subresource,
                                                 const D3D12_RANGE* readRange, void** ppData)
{
    ResourceShim* shim = nullptr;
    {
        AcquireSRWLockShared(&g_resourceShimLock);
        for (auto& rs : g_resourceShims) {
            if (rs.resource == resource) { shim = &rs; break; }
        }
        ReleaseSRWLockShared(&g_resourceShimLock);
    }
    LONG64 invocation = InterlockedIncrement64(&g_resourceMapCalls);
    if (invocation <= 5 || (invocation % 1000) == 0)
        Log("hooks: Shim_ResourceMap resource=%p subresource=%u cbSize=%zu invocation=%lld",
            (void*)resource, subresource, shim ? shim->cbSize : 0, invocation);
    HRESULT hr = shim ? shim->map(resource, subresource, readRange, ppData) : E_FAIL;
    if (SUCCEEDED(hr) && ppData && *ppData && shim && shim->cbSize == 1616) {
        // Potential camera CB - validate and patch jitter
        float* cb = (float*)*ppData;
        if (ValidateCameraCb(cb, shim->cbSize)) {
            static int s_acceptLogs = 0;
            if (++s_acceptLogs <= 3)
                Log("hooks: camera CB via Map validated & patched resource=%p", (void*)resource);
            ApplyCameraCbJitter(cb, shim->cbSize, g_renderW, g_renderH, g_currJitter, g_prevJitter);
            std::memcpy(g_lastPatchedCameraCb, cb, 1616);
            g_cameraCbValid = true;
            g_lastCamPatchFrame = g_frameCounter;
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE Shim_ResourceUnmap(ID3D12Resource* resource, UINT subresource,
                                                const D3D12_RANGE* writtenRange)
{
    ResourceShim* shim = nullptr;
    {
        AcquireSRWLockShared(&g_resourceShimLock);
        for (auto& rs : g_resourceShims) {
            if (rs.resource == resource) { shim = &rs; break; }
        }
        ReleaseSRWLockShared(&g_resourceShimLock);
    }
    if (shim)
        shim->unmap(resource, subresource, writtenRange);
}

// Per-command-list OM/viewport correlation (diagnostic only): the global
// bound-RTV state (g_boundRtv*) is last-writer-wins across all lists, so a
// viewport cannot be paired with its list's OM bind from globals. This table
// remembers each list's latest OM binding alongside its viewport calls.
// Raw pointers only, never AddRef'd; entries persist like the shim registry
// itself (same address-reuse caveat, visible via the logged sequence counts).
// No selection/trigger/injection effect.
struct ListCorr {
    ID3D12GraphicsCommandList* list;
    void* omRes;
    unsigned long long omHandle;
    unsigned long long omEcl;
    unsigned omCount;
    unsigned vpCount;
};
static ListCorr g_listCorr[64] = {};
static SRWLOCK g_corrLock = SRWLOCK_INIT;
static void CorrNoteOm(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                       unsigned long long handle)
{
    unsigned long long ecl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
    AcquireSRWLockExclusive(&g_corrLock);
    ListCorr* s = nullptr;
    for (auto& e : g_listCorr) {
        if (e.list == list) { s = &e; break; }
    }
    if (!s) {
        for (auto& e : g_listCorr) {
            if (!e.list) { s = &e; s->list = list; break; }
        }
    }
    if (!s) {
        static int s_corrFullLogs = 0;
        if (++s_corrFullLogs <= 2)
            Log("hooks: list-corr table full, OM record dropped");
    } else {
        s->omRes = (void*)res;
        s->omHandle = handle;
        s->omEcl = ecl;
        ++s->omCount;
    }
    ReleaseSRWLockExclusive(&g_corrLock);
}
static void CorrNoteViewport(ID3D12GraphicsCommandList* list, UINT numViewports,
                             const D3D12_VIEWPORT* pViewports)
{
    float vw = (numViewports >= 1 && pViewports) ? pViewports[0].Width : -1.0f;
    float vh = (numViewports >= 1 && pViewports) ? pViewports[0].Height : -1.0f;
    unsigned long long ecl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
    void* omRes = nullptr;
    unsigned long long omHandle = 0;
    unsigned long long omEcl = 0;
    unsigned omCount = 0;
    unsigned vpCount = 0;
    AcquireSRWLockExclusive(&g_corrLock);
    ListCorr* s = nullptr;
    for (auto& e : g_listCorr) {
        if (e.list == list) { s = &e; break; }
    }
    if (!s) {
        for (auto& e : g_listCorr) {
            if (!e.list) { s = &e; s->list = list; break; }
        }
    }
    if (s) {
        omRes = s->omRes;
        omHandle = s->omHandle;
        omEcl = s->omEcl;
        omCount = s->omCount;
        ++s->vpCount;
        vpCount = s->vpCount;
    }
    ReleaseSRWLockExclusive(&g_corrLock);
    if (!s) return;
    // Lock released before descriptor reads. Pointer identity only.
    D3D12_RESOURCE_DESC bd = {};
    bool gotBd = (omRes && SafeGetDesc((ID3D12Resource*)omRes, &bd));
    static volatile LONG s_corrLogs = 0;
    LONG n = InterlockedIncrement(&s_corrLogs);
    if (n <= 24) {
        Log("hooks: list-corr list=%p om=%p handle=%llX omSeq=%u omEcl=%llu vp=%dx%d vpSeq=%u vpEcl=%llu inset=%d bdesc=%ux%u fmt=%u",
            (void*)list, omRes, omHandle, omCount, omEcl,
            (int)vw, (int)vh, vpCount, ecl,
            SceneSetContains((ID3D12Resource*)omRes) ? 1 : 0,
            gotBd ? (unsigned)bd.Width : 0u, gotBd ? (unsigned)bd.Height : 0u,
            gotBd ? (unsigned)bd.Format : 0u);
    }
}

static HRESULT STDMETHODCALLTYPE Shim_CommandListReset(
    ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* initialState)
{
    CommandListShim* shim = FindCommandListShim(list);
    PFN_CommandListReset original = shim ? shim->reset : nullptr;
    if (!original) original = (PFN_CommandListReset)ResolveCommandListForward(
        list, 10, (void*)&Shim_CommandListReset);
    if (!original) return E_POINTER;
    InterlockedIncrement64(&g_mvRecordingResetTotal);
    if (shim) InterlockedExchange(&shim->recordingResetInProgress, 1);
    HRESULT hr = original(list, allocator, initialState);
    if (SUCCEEDED(hr)) InterlockedIncrement64(&g_mvRecordingResetSuccessTotal);
    if (SUCCEEDED(hr) && shim) {
        InterlockedExchange(&shim->recordingResetFailed, 0);
        InterlockedExchange(&shim->recordingClosed, 0);
        InterlockedIncrement64(&shim->recordingEpoch);
        InterlockedExchangePointer((PVOID volatile*)&shim->currentOmMvTarget, nullptr);
        InterlockedExchangePointer((PVOID volatile*)&shim->recordedMvTarget, nullptr);
        InterlockedExchange(&shim->mvTargetWidth, 0);
        InterlockedExchange(&shim->mvTargetHeight, 0);
        InterlockedExchange64(&shim->mvTargetOmCount, 0);
        InterlockedExchange64(&shim->mvTargetDrawCount, 0);
        InterlockedExchange64(&shim->mvTargetIndexedDrawCount, 0);
        InterlockedExchange64(&shim->dispatchCount, 0);
        InterlockedExchange64(&shim->mvTargetRecordPresent, 0);
        InterlockedExchange64(&shim->mvTargetRecordEcl, 0);
    } else if (FAILED(hr)) {
        if (shim) {
            InterlockedExchange(&shim->recordingResetFailed, 1);
            InterlockedExchange(&shim->recordingResetInProgress, 0);
        }
        InterlockedIncrement64(&g_mvRecordingResetFailTotal);
    }
    if (SUCCEEDED(hr) && shim) InterlockedExchange(&shim->recordingResetInProgress, 0);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Shim_CommandListClose(ID3D12GraphicsCommandList* list)
{
    CommandListShim* shim = FindCommandListShim(list);
    PFN_CommandListClose original = shim ? shim->close : nullptr;
    if (!original) original = (PFN_CommandListClose)ResolveCommandListForward(
        list, 9, (void*)&Shim_CommandListClose);
    if (!original) return E_POINTER;
    InterlockedIncrement64(&g_mvRecordingCloseTotal);
    HRESULT hr = original(list);
    if (SUCCEEDED(hr)) {
        if (shim) InterlockedExchange(&shim->recordingClosed, 1);
        InterlockedIncrement64(&g_mvRecordingCloseSuccessTotal);
    } else {
        InterlockedIncrement64(&g_mvRecordingCloseFailTotal);
    }
    return hr;
}

static void STDMETHODCALLTYPE Shim_DrawInstanced(
    ID3D12GraphicsCommandList* list, UINT vertexCount, UINT instanceCount,
    UINT startVertex, UINT startInstance)
{
    CommandListShim* shim = FindCommandListShim(list);
    PFN_DrawInstanced original = shim ? shim->drawInstanced : nullptr;
    if (!original) original = (PFN_DrawInstanced)ResolveCommandListForward(
        list, 12, (void*)&Shim_DrawInstanced);
    if (!original) return;
    if (shim && InterlockedCompareExchangePointer((PVOID volatile*)&shim->currentOmMvTarget,
                                          nullptr, nullptr))
    {
        InterlockedIncrement64(&shim->mvTargetDrawCount);
        InterlockedIncrement64(&g_mvRecordingDrawTotal);
    }
    original(list, vertexCount, instanceCount, startVertex, startInstance);
}

static void STDMETHODCALLTYPE Shim_DrawIndexedInstanced(
    ID3D12GraphicsCommandList* list, UINT indexCount, UINT instanceCount,
    UINT startIndex, INT baseVertex, UINT startInstance)
{
    CommandListShim* shim = FindCommandListShim(list);
    PFN_DrawIndexedInstanced original = shim ? shim->drawIndexedInstanced : nullptr;
    if (!original) original = (PFN_DrawIndexedInstanced)ResolveCommandListForward(
        list, 13, (void*)&Shim_DrawIndexedInstanced);
    if (!original) return;
    if (shim && InterlockedCompareExchangePointer((PVOID volatile*)&shim->currentOmMvTarget,
                                          nullptr, nullptr))
    {
        InterlockedIncrement64(&shim->mvTargetIndexedDrawCount);
        InterlockedIncrement64(&g_mvRecordingDrawTotal);
    }
    original(list, indexCount, instanceCount, startIndex,
             baseVertex, startInstance);
}

static void STDMETHODCALLTYPE Shim_Dispatch(
    ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z)
{
    CommandListShim* shim = FindCommandListShim(list);
    PFN_Dispatch original = shim ? shim->dispatch : nullptr;
    if (!original) original = (PFN_Dispatch)ResolveCommandListForward(
        list, 14, (void*)&Shim_Dispatch);
    if (!original) return;
    if (shim) InterlockedIncrement64(&shim->dispatchCount);
    original(list, x, y, z);
}

static void STDMETHODCALLTYPE Shim_ResourceBarrier(ID3D12GraphicsCommandList* list,
                                                   UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    CommandListShim* shim = FindCommandListShim(list);
    LONG64 invocation = InterlockedIncrement64(&g_shimBarrierCalls);
    LogCommandListShimCounter("ResourceBarrier", invocation, list, shim);
    ObserveNativeBarrierUsage(list, count, barriers);
    // LEGACY RECONNECT: state tracking for DoInjection's Barrier() calls.
    // Forwarding below via shim->resourceBarrier is unchanged.
    TrackResourceBarriers(count, barriers);
    if (shim && shim->resourceBarrier)
        shim->resourceBarrier(list, count, barriers);
}

static void STDMETHODCALLTYPE Shim_OMSetRenderTargets(
    ID3D12GraphicsCommandList* list, UINT count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL singleRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
{
    CommandListShim* shim = FindCommandListShim(list);
    InterlockedIncrement64(&g_mvRecordingOmSetTotal);
    LONG64 invocation = InterlockedIncrement64(&g_shimOmCalls);
    LogCommandListShimCounter("OMSetRenderTargets", invocation, list, shim);
    ObserveNativeOmUsage(list, count, handles, singleRange, depth);
    // LEGACY RECONNECT: bound-RTV tracking for SceneColorBound() (the
    // viewport-patch prerequisite). Forwarding below is unchanged.
    ID3D12Resource* omRes = TrackOMBind(list, count, handles, depth);
    // Diagnostic only: note a display-sized R16G16_FLOAT RTV requested by
    // this command-list recording. The census does not infer writes/content.
    NoteMvOmTarget(list, shim, count, handles);
    // Targeted diagnostic: prove whether main gameplay OM is observed
    {
        bool isTargeted = false;
        for (UINT i = 0; i < count && i < 8; ++i) {
            if (!handles[i].ptr) continue;
            ID3D12Resource* res = nullptr;
            {
                BookGuard guard;
                auto it = g_rtvMap.find(handles[i].ptr);
                if (it != g_rtvMap.end()) res = it->second;
            }
            if (!res) continue;
            D3D12_RESOURCE_DESC d = {};
            if (!SafeGetDesc(res, &d)) continue;
            bool isDisplay = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_displayW && d.Height == g_displayH && (d.Format == DXGI_FORMAT_R16G16B16A16_UNORM || d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (!isDisplay) continue;
            bool isTracked = false;
            AcquireSRWLockShared(&g_sceneColorCandidateLock);
            for (auto &c : g_sceneColorCandidates) {
                if (c.resource == (void*)res && c.width == g_displayW && c.height == g_displayH) { isTracked = true; break; }
            }
            ReleaseSRWLockShared(&g_sceneColorCandidateLock);
            if (!isTracked) {
                for (unsigned h = 0; h < 8; ++h) {
                    if (g_displayRTVHistory[h].resource == res) { isTracked = true; break; }
                }
            }
            if (isTracked) { isTargeted = true; break; }
        }
        if (isTargeted) {
            Log("vectorB: targeted OM list=%p queue=%p count=%u present=%llu ecl=%llu", (void*)list, (void*)g_graphicsQueue, count, (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0), (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
            for (UINT i = 0; i < count && i < 8; ++i) {
                if (!handles[i].ptr) continue;
                ID3D12Resource* res = nullptr;
                {
                    BookGuard guard;
                    auto it = g_rtvMap.find(handles[i].ptr);
                    if (it != g_rtvMap.end()) res = it->second;
                }
                D3D12_RESOURCE_DESC d = {};
                bool gotDesc = res ? SafeGetDesc(res, &d) : false;
                bool isDisplay = gotDesc && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_displayW && d.Height == g_displayH && (d.Format == DXGI_FORMAT_R16G16B16A16_UNORM || d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
                unsigned samples = 0;
                if (res) {
                    AcquireSRWLockShared(&g_sceneColorCandidateLock);
                    for (auto &c : g_sceneColorCandidates) {
                        if (c.resource == (void*)res && c.width == g_displayW && c.height == g_displayH) { samples = c.samples; break; }
                    }
                    ReleaseSRWLockShared(&g_sceneColorCandidateLock);
                }
                Log("vectorB: targeted slot=%u handle=%llX resource=%p size=%ux%u fmt=%u samples=%u isDisplay=%u", i, (unsigned long long)handles[i].ptr, (void*)res, gotDesc ? (unsigned)d.Width : 0u, gotDesc ? (unsigned)d.Height : 0u, gotDesc ? (unsigned)d.Format : 0u, samples, isDisplay ? 1u : 0u);
            }
        }
        if (!shim) {
            Log("vectorB: miss shim null list=%p count=%u present=%llu", (void*)list, count, (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0));
        } else if (!shim->omSetRenderTargets) {
            Log("vectorB: miss shim->omSetRenderTargets null list=%p", (void*)list);
        }
    }
    if (TryVectorB(list, count, handles, singleRange, depth, shim)) return;
    if (shim && shim->omSetRenderTargets)
        shim->omSetRenderTargets(list, count, handles, singleRange, depth);
    // Per-list correlation record (diagnostic): this list's latest OM binding.
    // Placed after forwarding so engine recording is never delayed by logging.
    CorrNoteOm(list, omRes,
               (count >= 1 && handles) ? handles[0].ptr : 0ull);
}

// Viewport/scissor shims (SDK-verified slots 21/22). Same per-list clone
// mechanism as slots 15/16/26/46 (proven safe: dozens of installs, stable
// runs); no global hook. The Hook_ call performs analysis and records the
// single engine call via the per-list original; the wrapper forwards only
// if Hook_ reports it did not record (at most once, never skipped twice).
static void STDMETHODCALLTYPE Shim_RSSetViewports(
    ID3D12GraphicsCommandList* list, UINT numViewports,
    const D3D12_VIEWPORT* pViewports)
{
    // PRE-GATE BOUND-IDENTITY RECORD (diagnostic only): runs before any
    // patchViewport/camera/DLAA gate, so it fires even when the camera path
    // is dormant. Correlates the viewport-bound RTV against the scene-set
    // snapshot by pointer identity only (descriptor equality never counts).
    // Read-only: registry snapshotted under lock, lock released before any
    // descriptor read; a guarded-read fault is logged and that pointer is not
    // dereferenced further. Capped distinct events; own counters, separate
    // from invocation totals. No selection/trigger/injection effect.
    {
        struct VpSnap { void* res; unsigned w; unsigned h; unsigned fmt; unsigned uses; };
        static int s_vpIdLogs = 0;
        static void* s_vpIdLastBound = nullptr;
        static float s_vpIdLastW = -1.0f, s_vpIdLastH = -1.0f;
        float vw = (numViewports >= 1 && pViewports) ? pViewports[0].Width : -1.0f;
        float vh = (numViewports >= 1 && pViewports) ? pViewports[0].Height : -1.0f;
        ID3D12Resource* bound = g_boundRtvResource;
        bool changed = (bound != (ID3D12Resource*)s_vpIdLastBound) ||
                       (vw != s_vpIdLastW) || (vh != s_vpIdLastH);
        if (changed && s_vpIdLogs < 16) {
            VpSnap snap[8] = {};
            unsigned snapN = 0;
            { AcquireSRWLockShared(&g_sceneSetLock);
              for (unsigned i = 0; i < g_sceneSetCount && i < 8; ++i) {
                  snap[i].res = (void*)g_sceneSet[i].resource;
                  snap[i].w = g_sceneSet[i].resW; snap[i].h = g_sceneSet[i].resH;
                  snap[i].fmt = g_sceneSet[i].resFmt; snap[i].uses = g_sceneSet[i].uses;
              }
              snapN = g_sceneSetCount < 8 ? g_sceneSetCount : 8;
              ReleaseSRWLockShared(&g_sceneSetLock); }
            D3D12_RESOURCE_DESC bd = {};
            bool gotBd = (bound && SafeGetDesc(bound, &bd));
            int inset = 0;
            for (unsigned i = 0; i < snapN; ++i) {
                if (snap[i].res == (void*)bound) { inset = 1; break; }
            }
            ++s_vpIdLogs;
            s_vpIdLastBound = (void*)bound; s_vpIdLastW = vw; s_vpIdLastH = vh;
            if (!bound) {
                Log("hooks: vp-bind vp=%dx%d bound=null inset=0 setcount=%u (n=%d)",
                    (int)vw, (int)vh, snapN, s_vpIdLogs);
            } else if (!gotBd) {
                Log("hooks: vp-bind vp=%dx%d bound=%p DESC-FAULT inset=%d setcount=%u (n=%d)",
                    (int)vw, (int)vh, (void*)bound, inset, snapN, s_vpIdLogs);
            } else {
                Log("hooks: vp-bind vp=%dx%d bound=%p %ux%u fmt=%u inset=%d setcount=%u (n=%d)",
                    (int)vw, (int)vh, (void*)bound,
                    (unsigned)bd.Width, (unsigned)bd.Height, (unsigned)bd.Format,
                    inset, snapN, s_vpIdLogs);
            }
            for (unsigned i = 0; i < snapN; ++i) {
                Log("hooks: vp-bind   cand=%p %ux%u fmt=%u uses=%u%s",
                    snap[i].res, snap[i].w, snap[i].h, snap[i].fmt, snap[i].uses,
                    (snap[i].res == (void*)bound) ? " <= BOUND" : "");
            }
        }
    }
    static unsigned s_entryDiag = 0;
    if ((++s_entryDiag % 600) == 1) {
        Log("hooks: Shim_RSSetViewports ENTRY list=%p num=%u w=%.0f h=%.0f",
            (void*)list, numViewports,
            (numViewports >= 1 && pViewports) ? (double)pViewports[0].Width : -1.0,
            (numViewports >= 1 && pViewports) ? (double)pViewports[0].Height : -1.0);
    }
    CommandListShim* shim = FindCommandListShim(list);
    LONG64 invocation = InterlockedIncrement64(&g_shimVpCalls);
    LogCommandListShimCounter("RSSetViewports", invocation, list, shim);
    bool recorded = Hook_RSSetViewports(list, numViewports, pViewports,
                                        shim ? shim->rsSetViewports : nullptr);
    if (!recorded && shim && shim->rsSetViewports)
        shim->rsSetViewports(list, numViewports, pViewports);
    // Per-list correlation record (diagnostic): pairs this viewport with the
    // list's latest OM binding recorded above. Pre-gate identity logging in
    // the vp-bind block is unaffected.
    CorrNoteViewport(list, numViewports, pViewports);
}

static void STDMETHODCALLTYPE Shim_RSSetScissorRects(
    ID3D12GraphicsCommandList* list, UINT numRects,
    const D3D12_RECT* pRects)
{
    static unsigned s_entryDiag = 0;
    if ((++s_entryDiag % 600) == 1) {
        Log("hooks: Shim_RSSetScissorRects ENTRY list=%p num=%u",
            (void*)list, numRects);
    }
    CommandListShim* shim = FindCommandListShim(list);
    LONG64 invocation = InterlockedIncrement64(&g_shimScCalls);
    LogCommandListShimCounter("RSSetScissorRects", invocation, list, shim);
    bool recorded = Hook_RSSetScissorRects(list, numRects, pRects,
                                           shim ? shim->rsSetScissorRects : nullptr);
    if (!recorded && shim && shim->rsSetScissorRects)
        shim->rsSetScissorRects(list, numRects, pRects);
}



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

void InstallCommandListHooks(ID3D12GraphicsCommandList* list, UINT listType = 0xFFFFFFFFu);
static HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(
    ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
    ID3D12PipelineState*, REFIID, void**);

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

    // LEGACY ARMING (dlaa=0): viewport patch enabled unless aborted.
    // NOTE: bd4e94b added "&& g_legacyScale" as a bridge-era kill-switch
    // (the cross-device bridge needed full-res rendering). The bridge is
    // abandoned (EnsureBridge creation disabled -> g_bridgeReady is always
    // false), so the switch now only kills the documented dlaa=0 legacy
    // path. Restored to pre-flag semantics; legacyScale remains parsed
    // for config compat but no longer gates arming. DLAA mode still
    // disables the patch via !g_dlaaMode below.
    g_patchViewport = !g_patchAborted && !g_dlaaMode;
    // Gameplay evidence: accepted camera patches. Do NOT arm immediately -
    // flipping discovery on mid-session floods the creation hooks with
    // GetDesc/adoption/log work exactly while the engine's render-graph
    // burst is still draining (instant-crash signature). Delay 3s so late
    // arming lands on an already-built, quiet graph.
    if (g_cameraCbValid) {
        static DWORD s_firstCamTick = 0;
        DWORD now = GetTickCount();
        if (!s_firstCamTick) {
            s_firstCamTick = now;
        } else if (now - s_firstCamTick > 3000 &&
                   InterlockedCompareExchange(&g_loadPhase, 0, 1) == 1) {
            Log("hooks: gameplay settled - discovery armed");
        }
    }
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
    // Rotation tracking: the bound target may be a live ping-pong sibling
    // that is neither stored slot. Membership is pointer-based.
    if (SceneSetContains(g_boundRtvResource)) return g_boundRtvResource;
    return nullptr;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES after)
{
    if (!list || !res) return;
    // Per-list forwarding: the Real_* globals are never assigned (two driver
    // tables exist), so resolve the driver original from this list's shim
    // (shared-lock lookup + SEH, same as all shim paths). A list without a
    // shim (e.g. submit-time third-heap table) is skipped rather than
    // null-called, and the state map is left untouched to match.
    CommandListShim* fwd = FindCommandListShim(list);
    if (!fwd || !fwd->resourceBarrier) return;
    D3D12_RESOURCE_STATES before;
    bool tracked = false;
    { BookGuard _bg; auto it = g_resourceStates.find(res);
      if (it == g_resourceStates.end()) return; // untracked
      tracked = true; before = it->second; }
    if (!tracked || before == after) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    fwd->resourceBarrier(list, 1, &b);
    { BookGuard _bg; g_resourceStates[res] = after; }
    // Rate-limited: this fires per-transition inside the flow; unlogged it
    // was ~400 lines/sec and the synchronous log I/O contributed to freezes.
    static volatile long s_barrierLogs = 0;
    long n = InterlockedIncrement(&s_barrierLogs);
    if (n <= 25 || (n % 2000) == 0)
        Log("hooks: barrier %p %u -> %u (#%ld)", (void*)res, (unsigned int)before, (unsigned int)after, n);
}

// Locked lookup of tracked entry states for a resource pair. Lives outside
// any __try function (BookGuard requires unwinding; C2712 forbids __try in
// such frames), so Present-time code can query states legally.
static bool LookupTrackedStates(ID3D12Resource* a, D3D12_RESOURCE_STATES* aOut,
                                ID3D12Resource* b, D3D12_RESOURCE_STATES* bOut)
{
    if (!a || !b || !aOut || !bOut) return false;
    bool aTr = false, bTr = false;
    { BookGuard _bg;
      auto it = g_resourceStates.find(a);
      if (it != g_resourceStates.end()) { *aOut = it->second; aTr = true; }
      auto jt = g_resourceStates.find(b);
      if (jt != g_resourceStates.end()) { *bOut = jt->second; bTr = true; } }
    return aTr && bTr;
}

// C2712-safe single lookups for callers that own direct __try frames
// (notably InjectAtPresentImpl): they cannot hold a BookGuard local, so the
// lock lives here instead. Only copied VALUES escape - never an iterator,
// reference, or pointer into map storage. A copied state can be stale by the
// time the caller barriers, but Barrier() re-reads under guard and no-ops on
// mismatch; callers keep their existing fail-closed untracked branches.
static bool FindTrackedState(ID3D12Resource* res, D3D12_RESOURCE_STATES* out)
{
    if (!res || !out) return false;
    BookGuard _bg;
    auto it = g_resourceStates.find(res);
    if (it == g_resourceStates.end()) return false;
    *out = it->second;
    return true;
}

// Same pattern for the handle->resource map (MV registry re-adoption path).
static bool FindRtvResource(SIZE_T handle, ID3D12Resource** out)
{
    if (!handle || !out) return false;
    BookGuard _bg;
    auto it = g_rtvMap.find(handle);
    if (it == g_rtvMap.end()) return false;
    *out = it->second;
    return true;
}

// Locked state-map restore WITHOUT recording: when a command list is
// discarded unexecuted (e.g. NGX faulted mid-record and the list won't
// Close), the GPU never saw our transitions, so the map must return to
// entry values — otherwise the next frame's barriers no-op against a lie
// while the GPU sits in the old state. Lock lives here, never in __try
// frames (C2712: BookGuard requires unwinding).
static void NoteTrackedStates(ID3D12Resource* a, D3D12_RESOURCE_STATES aState,
                              ID3D12Resource* b, D3D12_RESOURCE_STATES bState)
{
    if (!a || !b) return;
    BookGuard _bg;
    g_resourceStates[a] = aState;
    g_resourceStates[b] = bState;
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
    { BookGuard _bgState; g_resourceStates[g_dlssOut] = D3D12_RESOURCE_STATE_COMMON; }
    Log("hooks: dlssOut %p allocated (%ux%u)", (void*)g_dlssOut, g_displayW, g_displayH);
}

// The engine renders at whatever its window/render-target resolution is; we
// cannot hardcode 1920x992. Adopt a display size from a trusted source (scene
// RTV creation, scene-slot bind, or the scene-bound viewport), recompute the
// render size, invalidate the DLSS output, and re-configure the upscaler.
void AdoptDisplaySize(unsigned int w, unsigned int h)
{
    if (w == 0 || h == 0) return;
    // MIN-DISPLAY FLOOR: UI/menu targets (768x400 etc.) must never define
    // display size - bridge shared textures built at their dims then collide
    // with the real scene inside nvwgf2umx (driver AV, WER-confirmed).
    if (w < 1000 || h < 700) return;

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
    // Candidate confirmed stable â€” commit only if different from current.
    if (w == g_displayW && h == g_displayH)
        return;

    g_displayW = w;
    g_displayH = h;
    if (g_dlaaMode) {
        // VISIBILITY MODE: honoring 'scale' inside DLAA renders the feature
        // below display res and lets NGX upscale - an unmistakable visual.
        // (DLAA purity is one keystroke away: set scale=1.0.)
        if (g_cfg.renderScale > 0.05f && g_cfg.renderScale < 0.999f) {
            g_renderW = (unsigned int)((float)w * g_cfg.renderScale);
            g_renderH = (unsigned int)((float)h * g_cfg.renderScale);
            Log("hooks: VISIBILITY MODE - render %ux%u -> display %ux%u (scale %.2f)",
                g_renderW, g_renderH, w, h, g_cfg.renderScale);
        } else {
            g_renderW = w;
            g_renderH = h;
        }
    } else if (g_cfg.renderScale > 0.0f && g_cfg.renderScale < 1.0f) {
        g_renderW = (unsigned int)((float)g_displayW * g_cfg.renderScale);
        g_renderH = (unsigned int)((float)g_displayH * g_cfg.renderScale);
    }
    g_patchAborted = false;
    g_patchFramesWithoutInject = 0;
    if (g_dlssOutValid && g_dlssOut) {
        // Park in the graveyard â€” released after GPU drain inside ECL flush.
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

void EnsureUpscalerInit(bool bypassQuietGate)
{
    // FULL NGX SEQUENCING (fix89 extension): nvngx/NVAPI *loading* also races
    // load churn. The 00:00 run died with nvngx loaded at +5s pre-CreateFeature.
    // No NVIDIA driver contact until the copy chain has been quiet 600 frames.
    // NOTE: must run BEFORE the atomic 'attempted' mark below - deferring is
    // not attempting, and the flag would otherwise block all retries forever
    // (seen live: exactly one defer line, then init never re-ran).
    // SINGLE-DEVICE: reduced sequencing requirement since no second device.
    // Still need some stability before NGX touches the driver.
    // PRESENT-CLOCK GATE (2026-10-04): the camera-frame clock is unpassable
    // (game issues ~3 camera copies per loading burst, then dormant: quiet
    // stuck at 1f/120f across 14k presents). Gate instead on 600 presents of
    // no-new-chain-nodes (~6s at ~100 presents/s) â€” restoring fix89/90's
    // original 600 count on the reachable clock, past the +5s nvngx-load
    // death window with margin. Same protective intent, measurable units.
    if (!bypassQuietGate && HooksGetPresentQuietFrames() < 600) {
        static int s_seqLogs = 0;
        if (++s_seqLogs <= 5)
            Log("hooks: NGX init deferred - chain quiet %llup/600p (cam %uf/120f)", HooksGetPresentQuietFrames(), HooksGetQuietFrames());
        return; // retried by later callers
    }
    // Atomic: only one thread may attempt NGX init
    if (InterlockedCompareExchange(&g_upscalerInitAttempted, 1, 0) != 0) return;
    // SINGLE-DEVICE ARCHITECTURE: abandon bridge. Use game's device directly.
    // Tested live (runs 182725Z+): NGX Init on the game device returns
    // 0xBAD00001/0xBAD00002, identically on pristine clean devices — see the
    // Init investigation in docs/STATUS.md. QI(IDXGIDevice) failure below is
    // normal D3D12 behavior (proven on clean devices), not wrapper blocking.
    if (!g_device) {
        InterlockedExchange(&g_upscalerInitAttempted, 0);
        return;
    }
    Log("hooks: SINGLE-DEVICE - attempting NGX init on game device %p", (void*)g_device);
    // Log adapter info if QI succeeds (diagnostic only, not a gate)
    {
        IDXGIDevice* dxgidev = nullptr;
        HRESULT qhr = g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgidev);
        Log("hooks: SINGLE-DEVICE QI(IDXGIDevice) hr=0x%08X", (unsigned)qhr);
        if (SUCCEEDED(qhr)) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxgidev->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC adesc = {};
                if (SUCCEEDED(ad->GetDesc(&adesc)))
                    Log("hooks: SINGLE-DEVICE adapter VendorId=0x%04X '%ls'",
                        adesc.VendorId, adesc.Description);
                ad->Release();
            }
            dxgidev->Release();
        } else {
            Log("hooks: SINGLE-DEVICE QI(IDXGIDevice) failed as on clean D3D12 devices - proceeding anyway");
        }
    }
    if (!g_upscaler) g_upscaler = CreateUpscaler(UPSCALER_DLSS);
    if (!g_upscaler) {
        Log("hooks: upscaler creation failed");
        // Auditor fix: deferral must not consume the single attempt (see NOTE
        // above). Any early return below re-arms so later callers retry.
        InterlockedExchange(&g_upscalerInitAttempted, 0);
        return;
    }
    UpscalerInitParams ip = {};
    ip.device = g_device;  // GAME'S DEVICE, not bridge
    ip.renderWidth = g_renderW;
    ip.renderHeight = g_renderH;
    ip.displayWidth = g_displayW;
    ip.displayHeight = g_displayH;
    ip.dlssDllPath = g_cfg.dlssDllPath;
    ip.appId = g_cfg.appId;
    ip.ngxApiVersion = g_cfg.ngxApiVersion;
    ip.perfQuality = g_cfg.perfQuality;
    ip.mvJittered = g_cfg.mvJittered;
    ip.autoExposure = g_cfg.autoExposure;
    ip.depthInverted = g_cfg.depthInverted;
    if (!g_upscaler->Init(ip)) {
        Log("hooks: DLSS init failed - upscaling disabled");
        g_upscaler->SetEnabled(false);
        // Auditor fix: a failed Init is not an attempt that may proceed — the
        // atomic flag stays clear so later callers retry (throttled by the
        // 1/60-present retry cadence and the CreateFeature 1/sec throttle, so
        // no spin). SetEnabled(false) keeps Evaluate gated until then.
        InterlockedExchange(&g_upscalerInitAttempted, 0);
    } else {
        Log("hooks: upscaler ready (render %ux%u display %ux%u)",
            g_renderW, g_renderH, g_displayW, g_displayH);
        // One-shot clean-device discriminator (read-only vs the game).
        // Interprets a game-device Init failure: success here implicates the
        // wrapper; identical failure implicates AppId/driver platform.
        int probeRc = g_upscaler->ProbeCleanDeviceInit();
        Log("hooks: clean-device probe result=%d (0x%08X)", probeRc, (unsigned)probeRc);
    }
}

void DoInjection(ID3D12GraphicsCommandList* list)
{
    // Bridge mode owns DLAA exclusively (Present-time flow). This legacy
    // path records NGX work into the ENGINE's command list - illegal when
    // the feature lives on the bridge device. Hard-disable in DLAA mode.
    if (g_dlaaMode) return;
    // LEGACY EVAL DISABLED (2026-10-08, run 20261008T144341Z): this path
    // records NGX Evaluate into the engine's own command list, which we can
    // neither discard nor repair — when NGX faults mid-record (observed:
    // EvaluateFeature AV then game AV death seconds later), the poisoned
    // engine list executes and kills the game. The shadow own-list path is
    // the live path (discard-safe); legacy has never recorded a success
    // (0 injection markers in every recent run). Early-out keeps discovery
    // (viewport patch, triggers, logs) intact for diagnosis.
    // Reversal: delete this block; rebuild; rerun.
    {
        static volatile LONG s_legacyOffLogs = 0;
        if (InterlockedIncrement(&s_legacyOffLogs) <= 3)
            Log("hooks: legacy DoInjection eval disabled - shadow path owns NGX (see STATUS)");
        (void)list;
        return;
    }
    EnsureUpscalerInit(false);
    if (!g_upscaler || !g_upscaler->IsReady()) return;

    if (!g_dlssOutValid) CreateDlssOut();
    if (!g_dlssOutValid) return;

    ID3D12Resource* scene = g_activeSceneColor ? g_activeSceneColor : g_sceneColor;
    if (!scene) return;

    // STALE-SCENE SAFETY (not a behavior change): the stored choice may
    // reference a retired target (guarded reads have faulted on tracked
    // scene pointers in vivo). Skip the frame rather than handing a freed
    // resource to the driver; rediscovery repopulates on later frames.
    {
        __try {
            D3D12_RESOURCE_DESC srd = scene->GetDesc();
            (void)srd;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static int s_freedLogs = 0;
            if (++s_freedLogs <= 3)
                Log("hooks: DLSS injection skipped - scene choice retired (frame %u)", g_frameCounter);
            return;
        }
    }

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
    // Per-list result-copy forwarding (same shim-struct originals the record
    // path itself forwards through). Never null-call and never claim an
    // injection that was not recorded: a list without a shim is skipped with
    // the established diagnostic.
    CommandListShim* cshim = FindCommandListShim(list);
    if (!cshim || !cshim->copyTexture) {
        static int s_noFwdLogs = 0;
        if (++s_noFwdLogs <= 3)
            Log("hooks: DLSS result copy skipped - no per-list forward for list %p (frame %u)", (void*)list, g_frameCounter);
        return;
    }
    cshim->copyTexture(list, &dst, 0, 0, 0, &src, 0);

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

// Display-sized candidate-texture filter shared by the three logging-only
// texture branches below. Mirrors the ObserveNativeCandidate format list so
// one consistent candidate definition applies; descriptors are evidence for
// correlation, never proof of role.
static bool CreateTexFilterMatch(const D3D12_RESOURCE_DESC* d)
{
    if (!d || d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        d->MipLevels != 1 || d->Width < 1000 || d->Height < 500)
        return false;
    DXGI_FORMAT f = d->Format;
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           f == DXGI_FORMAT_R16G16B16A16_UNORM ||
           f == DXGI_FORMAT_R16G16_FLOAT ||
           f == DXGI_FORMAT_R32_TYPELESS ||
           f == DXGI_FORMAT_R32_FLOAT ||
           f == DXGI_FORMAT_R10G10B10A2_UNORM;
}

static void EraseResourceMappings(std::map<SIZE_T, ID3D12Resource*>& mapping,
                                  ID3D12Resource* resource)
{
    for (auto it = mapping.begin(); it != mapping.end();) {
        if (it->second == resource) it = mapping.erase(it);
        else ++it;
    }
}

// A successful D3D12 resource-creation call returning an address already held
// in a weak candidate slot advances that address's generation. Existing slots
// then fail validation without being cleared concurrently; a later observed
// view can explicitly re-adopt the same address at its new generation. If the
// fixed generation table is exhausted, all REAL inputs fail closed. No COM
// refs are retained or released.
static void RecordTrackedAddressReuse(const char* api, ID3D12Resource* created,
                                      const D3D12_RESOURCE_DESC* desc,
                                      REFIID requestedIid)
{
    // The raw pointer comparison is meaningful only for the interface type
    // stored by our tracking code. Do not reinterpret some other supported
    // creation interface as ID3D12Resource.
    if (!created || !desc || !InlineIsEqualGUID(requestedIid, __uuidof(ID3D12Resource))) return;
    bool depthMatch = created == g_depthResource;
    bool mvMatch = created == g_mvResource || created == g_mvResourceAlt;
    bool sceneMatch = created == g_sceneColor || created == g_sceneColorAlt;
    LONG64 generation = 0;
    bool tracked = false;
    if (depthMatch || mvMatch || sceneMatch)
        tracked = AdvanceResourceAddressGeneration(created, &generation);
    else
        tracked = AdvanceKnownResourceAddressGeneration(created, &generation);
    if (!tracked) return;
    if (sceneMatch) InterlockedExchange(&g_sceneAddressReuseDetected, 1);
    static volatile LONG s_matches = 0;
    LONG n = InterlockedIncrement(&s_matches);
    if (n <= 24 || (n % 100) == 0)
        Log("hooks: tracked-address-reuse generation n=%ld api=%s res=%p generation=%lld tracked=%d depth=%d mv=%d scene=%d newDesc=%ux%u fmt=%u dim=%u mips=%u samples=%u present=%llu ecl=%llu",
            n, api, (void*)created, (long long)generation, tracked ? 1 : 0,
            depthMatch ? 1 : 0, mvMatch ? 1 : 0,
            sceneMatch ? 1 : 0, (unsigned)desc->Width, (unsigned)desc->Height,
            (unsigned)desc->Format, (unsigned)desc->Dimension,
            (unsigned)desc->MipLevels, (unsigned)desc->SampleDesc.Count,
            (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
            (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));

    // Drop pointer/handle lookups protected by their established locks. The
    // candidate slots themselves stay untouched; their per-slot generation
    // mismatch makes them fail closed until a fresh view/use observation.
    {
        BookGuard guard;
        EraseResourceMappings(g_rtvMap, created);
        EraseResourceMappings(g_dsvMap, created);
        EraseResourceMappings(g_srvMap, created);
        EraseResourceMappings(g_displayRTVMap, created);
        g_resourceStates.erase(created);
    }
    {
        AcquireSRWLockExclusive(&g_copyMapLock);
        g_copySrcCount.erase((void*)created);
        ReleaseSRWLockExclusive(&g_copyMapLock);
    }
    {
        AcquireSRWLockExclusive(&g_sceneSetLock);
        for (unsigned i = 0; i < g_sceneSetCount;) {
            if (g_sceneSet[i].resource == created) {
                g_sceneSet[i] = g_sceneSet[--g_sceneSetCount];
                g_sceneSet[g_sceneSetCount] = {};
            } else ++i;
        }
        ReleaseSRWLockExclusive(&g_sceneSetLock);
    }
    {
        AcquireSRWLockExclusive(&g_nativeCandidateLock);
        for (unsigned i = 0; i < g_nativeCandidateCount;) {
            if (g_nativeCandidates[i].resource == created) {
                g_nativeCandidates[i] = g_nativeCandidates[--g_nativeCandidateCount];
                g_nativeCandidates[g_nativeCandidateCount] = {};
            } else ++i;
        }
        ReleaseSRWLockExclusive(&g_nativeCandidateLock);
    }
    {
        AcquireSRWLockExclusive(&g_sceneColorCandidateLock);
        for (auto& candidate : g_sceneColorCandidates) {
            if (candidate.resource == created) candidate = {};
        }
        ReleaseSRWLockExclusive(&g_sceneColorCandidateLock);
    }

}

HRESULT WINAPI Hook_CreateCommittedResource(ID3D12Device* device,
    const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** ppResource)
{
    // Diagnostic-only forwarding hook. No shims, no patching.
    // Per-resource Map/Unmap shims stay dormant until a valid target is established.
    HRESULT hr = Real_CreateCommittedResource(device, heapProps, heapFlags, desc, initialState, clearValue, riid, ppResource);
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc) {
        RecordTrackedAddressReuse("Committed", (ID3D12Resource*)*ppResource, desc, riid);
        // Bounded log: large buffers only (>=1MB), first 20 then 1/100 sampling.
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            desc->Width >= 1048576ULL) {
            static volatile LONG s_largeCommittedLogs = 0;
            LONG n = InterlockedIncrement(&s_largeCommittedLogs);
            if (n <= 20 || (n % 100) == 1) {
                Log("hooks: CreateCommittedResource device=%p res=%p w=%llu h=%u dim=%u fmt=%u flags=0x%X initState=0x%X heapType=%u heapFlags=0x%X (n=%d)",
                    (void*)device, *ppResource,
                    (unsigned long long)desc->Width, (unsigned)desc->Height, (unsigned)desc->Dimension,
                    (unsigned)desc->Format, (unsigned)desc->Flags, (unsigned)initialState,
                    heapProps ? (unsigned)heapProps->Type : 0xFFFFFFFFu, (unsigned)heapFlags, (int)n);
            }
        }
        // Bounded log: display-sized candidate textures (the buffer filter
        // above omits all textures, hiding scene-sized targets that move only
        // through copy chains). First 128 then 1/100 sampling; separate counter.
        // 128 covers the observed loading burst plus the old 21-100 blind
        // window with headroom; any overflow still surfaces at n=201, 301.
        if (CreateTexFilterMatch(desc)) {
            static volatile LONG s_texCommittedLogs = 0;
            LONG nt = InterlockedIncrement(&s_texCommittedLogs);
            if (nt <= 128 || (nt % 100) == 1) {
                Log("hooks: CreateCommittedTexture device=%p res=%p w=%llu h=%u fmt=%u flags=0x%X mips=%u samples=%u initState=0x%X heapType=%u (n=%d)",
                    (void*)device, *ppResource,
                    (unsigned long long)desc->Width, (unsigned)desc->Height,
                    (unsigned)desc->Format, (unsigned)desc->Flags,
                    (unsigned)desc->MipLevels, (unsigned)desc->SampleDesc.Count,
                    (unsigned)initialState,
                    heapProps ? (unsigned)heapProps->Type : 0xFFFFFFFFu, (int)nt);
            }
        }
    }
    return hr;
}

HRESULT WINAPI Hook_CreatePlacedResource(ID3D12Device* device,
    ID3D12Heap* pHeap, UINT64 heapOffset,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** ppResource)
{
    // Diagnostic-only forwarding hook. Heap type is NOT inferred here;
    // log heap object pointer and offset only.
    HRESULT hr = Real_CreatePlacedResource(device, pHeap, heapOffset, desc, initialState, clearValue, riid, ppResource);
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc) {
        RecordTrackedAddressReuse("Placed", (ID3D12Resource*)*ppResource, desc, riid);
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            desc->Width >= 1048576ULL) {
            static volatile LONG s_largePlacedLogs = 0;
            LONG n = InterlockedIncrement(&s_largePlacedLogs);
            if (n <= 20 || (n % 100) == 1) {
                Log("hooks: CreatePlacedResource device=%p res=%p heap=%p heapOff=%llu w=%llu h=%u dim=%u fmt=%u flags=0x%X initState=0x%X (n=%d)",
                    (void*)device, *ppResource, (void*)pHeap, (unsigned long long)heapOffset,
                    (unsigned long long)desc->Width, (unsigned)desc->Height, (unsigned)desc->Dimension,
                    (unsigned)desc->Format, (unsigned)desc->Flags, (unsigned)initialState, (int)n);
            }
        }
        // Bounded log: display-sized candidate textures (see committed hook).
        // First 128 then 1/100 sampling (blind-window closure, same rationale).
        if (CreateTexFilterMatch(desc)) {
            static volatile LONG s_texPlacedLogs = 0;
            LONG nt = InterlockedIncrement(&s_texPlacedLogs);
            if (nt <= 128 || (nt % 100) == 1) {
                Log("hooks: CreatePlacedTexture device=%p res=%p heap=%p heapOff=%llu w=%llu h=%u fmt=%u flags=0x%X mips=%u samples=%u initState=0x%X (n=%d)",
                    (void*)device, *ppResource, (void*)pHeap, (unsigned long long)heapOffset,
                    (unsigned long long)desc->Width, (unsigned)desc->Height,
                    (unsigned)desc->Format, (unsigned)desc->Flags,
                    (unsigned)desc->MipLevels, (unsigned)desc->SampleDesc.Count,
                    (unsigned)initialState, (int)nt);
            }
        }
    }
    return hr;
}

HRESULT WINAPI Hook_CreateReservedResource(ID3D12Device* device,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** ppResource)
{
    // Diagnostic-only forwarding hook. No heap applies to reserved resources.
    HRESULT hr = Real_CreateReservedResource(device, desc, initialState, clearValue, riid, ppResource);
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc) {
        RecordTrackedAddressReuse("Reserved", (ID3D12Resource*)*ppResource, desc, riid);
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            desc->Width >= 1048576ULL) {
            static volatile LONG s_largeReservedLogs = 0;
            LONG n = InterlockedIncrement(&s_largeReservedLogs);
            if (n <= 20 || (n % 100) == 1) {
                Log("hooks: CreateReservedResource device=%p res=%p w=%llu h=%u dim=%u fmt=%u flags=0x%X initState=0x%X (n=%d)",
                    (void*)device, *ppResource,
                    (unsigned long long)desc->Width, (unsigned)desc->Height, (unsigned)desc->Dimension,
                    (unsigned)desc->Format, (unsigned)desc->Flags, (unsigned)initialState, (int)n);
            }
        }
        // Bounded log: display-sized candidate textures (see committed hook).
        // First 128 then 1/100 sampling (blind-window closure, same rationale).
        if (CreateTexFilterMatch(desc)) {
            static volatile LONG s_texReservedLogs = 0;
            LONG nt = InterlockedIncrement(&s_texReservedLogs);
            if (nt <= 128 || (nt % 100) == 1) {
                Log("hooks: CreateReservedTexture device=%p res=%p w=%llu h=%u fmt=%u flags=0x%X mips=%u samples=%u initState=0x%X (n=%d)",
                    (void*)device, *ppResource,
                    (unsigned long long)desc->Width, (unsigned)desc->Height,
                    (unsigned)desc->Format, (unsigned)desc->Flags,
                    (unsigned)desc->MipLevels, (unsigned)desc->SampleDesc.Count,
                    (unsigned)initialState, (int)nt);
            }
        }
    }
    return hr;
}

// Device4 '1'-variant creation hooks (observe-only, diagnostic). Installed once
// on the already-swapped table after the first successful Device4 QI (see
// Shim_DeviceQI). Same forwarding/diagnostic discipline as the base creation
// hooks: forward exactly once via the per-table original, log matches of the
// shared texture filter, no retention, no analysis, no patching.
HRESULT WINAPI Hook_CreateCommittedResource1(ID3D12Device* device,
    const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, ID3D12ProtectedResourceSession* session,
    REFIID riidRes, void** ppResource)
{
    HRESULT hr = Real_CommittedResource1 ?
        Real_CommittedResource1(device, heapProps, heapFlags, desc, initialState,
                                clearValue, session, riidRes, ppResource) : E_NOINTERFACE;
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc)
        RecordTrackedAddressReuse("Committed1", (ID3D12Resource*)*ppResource, desc, riidRes);
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc && CreateTexFilterMatch(desc)) {
        static volatile LONG s_texCommitted1Logs = 0;
        LONG nt = InterlockedIncrement(&s_texCommitted1Logs);
        if (nt <= 128 || (nt % 100) == 1) {
            Log("hooks: CreateCommittedResource1 device=%p res=%p w=%llu h=%u fmt=%u flags=0x%X mips=%u samples=%u initState=0x%X heapType=%u prot=%d (n=%d)",
                (void*)device, *ppResource,
                (unsigned long long)desc->Width, (unsigned)desc->Height,
                (unsigned)desc->Format, (unsigned)desc->Flags,
                (unsigned)desc->MipLevels, (unsigned)desc->SampleDesc.Count,
                (unsigned)initialState,
                heapProps ? (unsigned)heapProps->Type : 0xFFFFFFFFu,
                session ? 1 : 0, (int)nt);
        }
    }
    return hr;
}

HRESULT WINAPI Hook_CreateHeap1(ID3D12Device* device,
    const D3D12_HEAP_DESC* heapDesc, ID3D12ProtectedResourceSession* session,
    REFIID riid, void** ppHeap)
{
    HRESULT hr = Real_Heap1 ?
        Real_Heap1(device, heapDesc, session, riid, ppHeap) : E_NOINTERFACE;
    // Heap creation is logged AS heap creation (not resource creation): heap
    // timing only bounds placed-resource creation, it cannot attribute it.
    if (SUCCEEDED(hr) && ppHeap && *ppHeap && heapDesc) {
        static volatile LONG s_heap1Logs = 0;
        LONG n = InterlockedIncrement(&s_heap1Logs);
        if (n <= 20 || (n % 100) == 1) {
            Log("hooks: CreateHeap1 device=%p heap=%p size=%llu type=%u align=%llu flags=0x%X prot=%d (n=%d)",
                (void*)device, *ppHeap,
                (unsigned long long)heapDesc->SizeInBytes,
                (unsigned)heapDesc->Properties.Type,
                (unsigned long long)heapDesc->Alignment,
                (unsigned)heapDesc->Flags,
                session ? 1 : 0, (int)n);
        }
    }
    return hr;
}

HRESULT WINAPI Hook_CreateReservedResource1(ID3D12Device* device,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, ID3D12ProtectedResourceSession* session,
    REFIID riidRes, void** ppResource)
{
    HRESULT hr = Real_ReservedResource1 ?
        Real_ReservedResource1(device, desc, initialState,
                               clearValue, session, riidRes, ppResource) : E_NOINTERFACE;
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc)
        RecordTrackedAddressReuse("Reserved1", (ID3D12Resource*)*ppResource, desc, riidRes);
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc && CreateTexFilterMatch(desc)) {
        static volatile LONG s_texReserved1Logs = 0;
        LONG nt = InterlockedIncrement(&s_texReserved1Logs);
        if (nt <= 128 || (nt % 100) == 1) {
            Log("hooks: CreateReservedResource1 device=%p res=%p w=%llu h=%u fmt=%u flags=0x%X mips=%u samples=%u initState=0x%X prot=%d (n=%d)",
                (void*)device, *ppResource,
                (unsigned long long)desc->Width, (unsigned)desc->Height,
                (unsigned)desc->Format, (unsigned)desc->Flags,
                (unsigned)desc->MipLevels, (unsigned)desc->SampleDesc.Count,
                (unsigned)initialState,
                session ? 1 : 0, (int)nt);
        }
    }
    return hr;
}

// Device8 resource hooks (observe-only, diagnostic). Installed once on the
// already-swapped table after the first successful Device8 QI (see
// Shim_DeviceQI). D3D12_RESOURCE_DESC1 shares the base descriptor prefix, so
// the shared texture filter reads it through a base pointer. Same
// forward-once / caps / no-retention discipline as all creation hooks.
HRESULT WINAPI Hook_CreateCommittedResource2(ID3D12Device* device,
    const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, ID3D12ProtectedResourceSession* session,
    REFIID riidRes, void** ppResource)
{
    HRESULT hr = Real_CommittedResource2 ?
        Real_CommittedResource2(device, heapProps, heapFlags, desc, initialState,
                                clearValue, session, riidRes, ppResource) : E_NOINTERFACE;
    const D3D12_RESOURCE_DESC* bd = (const D3D12_RESOURCE_DESC*)desc;
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc)
        RecordTrackedAddressReuse("Committed2", (ID3D12Resource*)*ppResource, bd, riidRes);
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc && CreateTexFilterMatch(bd)) {
        static volatile LONG s_texCommitted2Logs = 0;
        LONG nt = InterlockedIncrement(&s_texCommitted2Logs);
        if (nt <= 128 || (nt % 100) == 1) {
            Log("hooks: CreateCommittedResource2 device=%p res=%p w=%llu h=%u fmt=%u flags=0x%X mips=%u samples=%u initState=0x%X heapType=%u prot=%d (n=%d)",
                (void*)device, *ppResource,
                (unsigned long long)bd->Width, (unsigned)bd->Height,
                (unsigned)bd->Format, (unsigned)bd->Flags,
                (unsigned)bd->MipLevels, (unsigned)bd->SampleDesc.Count,
                (unsigned)initialState,
                heapProps ? (unsigned)heapProps->Type : 0xFFFFFFFFu,
                session ? 1 : 0, (int)nt);
        }
    }
    return hr;
}

HRESULT WINAPI Hook_CreatePlacedResource1(ID3D12Device* device,
    ID3D12Heap* pHeap, UINT64 heapOffset,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** ppResource)
{
    HRESULT hr = Real_PlacedResource1 ?
        Real_PlacedResource1(device, pHeap, heapOffset, desc, initialState,
                             clearValue, riid, ppResource) : E_NOINTERFACE;
    const D3D12_RESOURCE_DESC* bd = (const D3D12_RESOURCE_DESC*)desc;
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc)
        RecordTrackedAddressReuse("Placed1", (ID3D12Resource*)*ppResource, bd, riid);
    if (SUCCEEDED(hr) && ppResource && *ppResource && desc && CreateTexFilterMatch(bd)) {
        static volatile LONG s_texPlaced1Logs = 0;
        LONG nt = InterlockedIncrement(&s_texPlaced1Logs);
        if (nt <= 128 || (nt % 100) == 1) {
            Log("hooks: CreatePlacedResource1 device=%p res=%p heap=%p heapOff=%llu w=%llu h=%u fmt=%u flags=0x%X mips=%u samples=%u initState=0x%X (n=%d)",
                (void*)device, *ppResource, (void*)pHeap, (unsigned long long)heapOffset,
                (unsigned long long)bd->Width, (unsigned)bd->Height,
                (unsigned)bd->Format, (unsigned)bd->Flags,
                (unsigned)bd->MipLevels, (unsigned)bd->SampleDesc.Count,
                (unsigned)initialState, (int)nt);
        }
    }
    return hr;
}

// Descriptor-heap inventory (observe-only, diagnostic). Records the numeric
// data needed for OFFLINE correlation of root-table GPU handles: heap object,
// type, descriptor count, flags, CPU/GPU start handles, and the per-type
// increment. The heap and all references are transient: nothing is stored
// beyond logged numbers, no COM reference is retained. Heap contents are
// never read. Same forward-once discipline as all creation hooks.
static volatile LONG64 g_heapInventoryTotal = 0;
HRESULT WINAPI Hook_CreateDescriptorHeap(ID3D12Device* device,
    const D3D12_DESCRIPTOR_HEAP_DESC* heapDesc, REFIID riid, void** ppHeap)
{
    HRESULT hr = Real_CreateDescriptorHeap ?
        Real_CreateDescriptorHeap(device, heapDesc, riid, ppHeap) : E_NOINTERFACE;
    LONG64 n = InterlockedIncrement64(&g_heapInventoryTotal);
    if (SUCCEEDED(hr) && ppHeap && *ppHeap && heapDesc) {
        static volatile LONG s_heapLogs = 0;
        LONG m = InterlockedIncrement(&s_heapLogs);
        if (m <= 24 || (m % 100) == 0) {
            ID3D12DescriptorHeap* heap = (ID3D12DescriptorHeap*)*ppHeap;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
            bool shaderVisible =
                (heapDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
            __try {
                cpu = heap->GetCPUDescriptorHandleForHeapStart();
                if (shaderVisible)
                    gpu = heap->GetGPUDescriptorHandleForHeapStart();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                cpu = {}; gpu = {};
            }
            unsigned inc = 0;
            if (device) {
                __try {
                    inc = device->GetDescriptorHandleIncrementSize(heapDesc->Type);
                } __except (EXCEPTION_EXECUTE_HANDLER) { inc = 0; }
            }
            Log("hooks: heap-inventory #%lld device=%p heap=%p type=%u count=%u flags=0x%X node=%u cpu=%llX gpu=%llX inc=%u vis=%d",
                n, (void*)device, (void*)heap,
                (unsigned)heapDesc->Type, (unsigned)heapDesc->NumDescriptors,
                (unsigned)heapDesc->Flags, (unsigned)heapDesc->NodeMask,
                (unsigned long long)cpu.ptr, (unsigned long long)gpu.ptr,
                inc, shaderVisible ? 1 : 0);
        }
    }
    return hr;
}

void InstallResourceShim(ID3D12Resource* resource, size_t cbSize)
{
    // DORMANT: do not call until a valid target resource and safe CPU-write path are established.
    // Slots verified SDK 10.0.28000.0: Map=8, Unmap=9 (10/11 are GetDesc/GetGPUVirtualAddress).
    if (!resource) return;
    AcquireSRWLockExclusive(&g_resourceShimLock);
    // Check if already shimmed
    for (auto& rs : g_resourceShims) {
        if (rs.resource == resource) {
            ReleaseSRWLockExclusive(&g_resourceShimLock);
            return;
        }
    }
    // Find empty slot
    ResourceShim* slot = nullptr;
    for (auto& rs : g_resourceShims) {
        if (!rs.resource) { slot = &rs; break; }
    }
    if (!slot) {
        ReleaseSRWLockExclusive(&g_resourceShimLock);
        return;
    }
    // Get original vtable
    void** original = nullptr;
    __try { original = *(void***)resource; } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (!original) {
        ReleaseSRWLockExclusive(&g_resourceShimLock);
        return;
    }
    // ID3D12Resource vtable slots (verified SDK 10.0.28000.0): Map=8, Unmap=9.
    // Slots 10/11 are GetDesc/GetGPUVirtualAddress and must not be hooked as Map/Unmap.
    slot->map = (PFN_ResourceMap)original[8];
    slot->unmap = (PFN_ResourceUnmap)original[9];
    // Clone vtable
    constexpr SIZE_T kResourceVtableEntries = 32;
    void** cloned = (void**)VirtualAlloc(nullptr, sizeof(void*) * kResourceVtableEntries, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!cloned) {
        ReleaseSRWLockExclusive(&g_resourceShimLock);
        return;
    }
    __try { std::memcpy(cloned, original, sizeof(void*) * kResourceVtableEntries); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    slot->resource = resource;
    slot->originalVtbl = original;
    slot->clonedVtbl = cloned;
    slot->cbSize = cbSize;
    cloned[8] = (void*)&Shim_ResourceMap;
    cloned[9] = (void*)&Shim_ResourceUnmap;
    // Swap vtable
    bool installed = true;
    __try { *(void***)resource = cloned; } __except (EXCEPTION_EXECUTE_HANDLER) { installed = false; }
    if (!installed) {
        VirtualFree(cloned, 0, MEM_RELEASE);
        slot->resource = nullptr;
        slot->originalVtbl = nullptr;
        slot->clonedVtbl = nullptr;
        ReleaseSRWLockExclusive(&g_resourceShimLock);
        return;
    }
    Log("hooks: Resource shim installed resource=%p cbSize=%zu map=%p unmap=%p",
        (void*)resource, cbSize, (void*)slot->map, (void*)slot->unmap);
    ReleaseSRWLockExclusive(&g_resourceShimLock);
}

void Hook_CreateRenderTargetView(ID3D12Device* device, ID3D12Resource* res,
                                 const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                 D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    static unsigned s_entryDiag = 0;
    if ((++s_entryDiag % 60) == 1) {
        D3D12_RESOURCE_DESC rd = {};
        SafeGetDesc(res, &rd);
        unsigned fmt = desc ? (unsigned)desc->Format : 0;
        Log("hooks: Hook_CreateRenderTargetView ENTRY device=%p res=%p desc=%p handle=%llX rd.Width=%u rd.Height=%u rd.Format=%u rd.Flags=%X desc.Format=%u",
            (void*)device, (void*)res, (void*)desc, (unsigned long long)handle.ptr,
            (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)rd.Format, (unsigned)rd.Flags, fmt);
    }
    // DISCOVERY MUST ALWAYS RUN: resources are discovered at creation time
    // BEFORE the bridge exists - gating on g_bridgeReady creates a
    // chicken-and-egg deadlock (need discovery to build bridge, need bridge
    // to enable discovery).
    // Track RTVs on ALL game devices - the game creates multiple devices.
    if (res && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        ObserveNativeCandidate(res, rd, 1);
        // Provenance diagnostic: log every qualifying display-sized RTV creation with exact handle
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1 && rd.SampleDesc.Count == 1) {
            Log("rtv-provenance: create handle=%llX resource=%p size=%ux%u fmt=%u flags=%u", (unsigned long long)handle.ptr, (void*)res, (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)rd.Format, (unsigned)rd.Flags);
        }
        // Track handle reuse and last display RTV for OM correlation
        {
            BookGuard _bg;
            auto it = g_rtvMap.find(handle.ptr);
            if (it != g_rtvMap.end() && it->second != res) {
                Log("rtv-provenance: handle reuse handle=%llX oldRes=%p newRes=%p", (unsigned long long)handle.ptr, (void*)it->second, (void*)res);
            }
        }
        // Format-neutral handle map (correlation infrastructure): record EVERY
        // display-sized mip1 RTV view so OM binds resolve regardless of pixel
        // format. LDR fmt-28 views previously missed: the outer display-sized
        // branch below traps all big-mip1 textures, so the catch-all at the end
        // never sees them. Pure handle->resource bookkeeping, refreshed on
        // every creation (handles recycle); every reader applies its own
        // format/role checks, and no adoption/classification logic is touched.
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1) {
            BookGuard _bgMap;
            g_rtvMap[handle.ptr] = res;
        }
        if (rd.Width == g_displayW && rd.Height == g_displayH && (rd.Format == DXGI_FORMAT_R16G16B16A16_UNORM || rd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            g_lastDisplayRTVHandle = handle;
            g_lastDisplayRTVResource = res;
            g_lastDisplayRTVPresent = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
            // Record in history for resource-level correlation
            DisplayRTVRecord &rec = g_displayRTVHistory[g_displayRTVHistoryNext % 8];
            rec.handle = handle;
            rec.resource = res;
            rec.width = (unsigned)rd.Width;
            rec.height = (unsigned)rd.Height;
            rec.format = (unsigned)rd.Format;
            rec.present = g_lastDisplayRTVPresent;
            g_displayRTVHistoryNext++;
            Log("rtv-provenance: display history idx=%u handle=%llX resource=%p present=%llu", (g_displayRTVHistoryNext-1)%8, (unsigned long long)handle.ptr, (void*)res, g_lastDisplayRTVPresent);
            // Persistent display map not overwritten on handle reuse
            {
                BookGuard _bg;
                auto it = g_displayRTVMap.find(handle.ptr);
                if (it == g_displayRTVMap.end()) g_displayRTVMap[handle.ptr] = res;
            }
        }
        // BACKBUFFER CAPTURE (polite): flip-model swapchain buffers carry
        // ALLOW_RENDER_TARGET + DISPLAY_SWAP? and are RTV'd right after swap
        // creation. Capture display-sized candidates here so the injection
        // path NEVER has to call GetBuffer on the engine's guarded wrapper.
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            rd.MipLevels == 1 && rd.SampleDesc.Count == 1 &&
            (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
            rd.Width >= 500 && rd.Height >= 300 &&
            (!g_bbCached || res != g_bbCached)) {
            bool isBbLike = (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) == 0;
            if (isBbLike && (!g_bbCached || g_bbFetchFails > 0)) {
                StoreTracked(&g_bbCached, res);
                Log("hooks: backbuffer candidate cached from RTV creation %p (%ux%u fmt %u flags %X)",
                    (void*)res, (unsigned)rd.Width, (unsigned)rd.Height,
                    (unsigned)rd.Format, (unsigned)rd.Flags);
            }
        }
        // Hold a creation-ref on any interesting target NOW - the object is
        // fully constructed and the engine holds its own ref, so ours is safe.
        // (AddRef-on-observation later is illegal and corrupted teardown.)
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1 &&
            IsSceneColorFormat((unsigned)desc->Format)) {
            // Display-sized HDR/UNORM color target - adopt as scene color. The size
            // is NOT hardcoded (the engine may render at e.g. 1920x1001).
            { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
            if (!g_sceneColorValid) {
                StoreTracked(&g_sceneColor, res);
                g_sceneColorRtv = handle;
                g_sceneColorValid = true;
                AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
                Log("hooks: scene color RTV %p (%ux%u fmt=%u)", (void*)res,
                    (unsigned int)rd.Width, (unsigned int)rd.Height, (unsigned)desc->Format);
            } else if (res == g_sceneColor) {
                // The game re-created the RTV view for the same resource
                // (e.g. renderer re-init). Refresh the stored handle.
                StoreTracked(&g_sceneColor, res);
                g_sceneColorRtv = handle;
                Log("hooks: scene color RTV handle refreshed %p", (void*)res);
            } else if (res == g_sceneColorAlt) {
                StoreTracked(&g_sceneColorAlt, res);
                g_sceneColorRtvAlt = handle;
            } else if (res != g_sceneColorAlt) {
                StoreTracked(&g_sceneColorAlt, res);
                g_sceneColorRtvAlt = handle;
                AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
                Log("hooks: scene color RTV %p (%ux%u fmt=%u) (ALT)", (void*)res,
                    (unsigned int)rd.Width, (unsigned int)rd.Height, (unsigned)desc->Format);
            }
            // Scene-set evidence (rotation tracking): every adopted UNORM view
            // enters the bounded set; matching stays pointer-based.
            SceneSetNote(res, (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)desc->Format);
    } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1) {
            if (IsSceneColorFormat((unsigned)desc->Format)) {
                g_displayW = (unsigned int)rd.Width;
                g_displayH = (unsigned int)rd.Height;
                { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
                if (!g_sceneColorValid) {
                    StoreTracked(&g_sceneColor, res);
                    g_sceneColorRtv = handle;
                    g_sceneColorValid = true;
                    Log("hooks: scene color RTV %p (%ux%u fmt=%u)", (void*)res,
                        (unsigned int)rd.Width, (unsigned int)rd.Height, (unsigned)desc->Format);
                } else if (res == g_sceneColor) {
                    // The game re-created the RTV view for the same resource
                    // (e.g. renderer re-init). Refresh the stored handle.
                    StoreTracked(&g_sceneColor, res);
                    g_sceneColorRtv = handle;
                    Log("hooks: scene color RTV handle refreshed %p", (void*)res);
                } else if (res == g_sceneColorAlt) {
                    StoreTracked(&g_sceneColorAlt, res);
                    g_sceneColorRtvAlt = handle;
                } else if (res != g_sceneColorAlt) {
                    StoreTracked(&g_sceneColorAlt, res);
                    g_sceneColorRtvAlt = handle;
                    Log("hooks: scene color RTV %p (%ux%u fmt=%u) (ALT)", (void*)res,
                        (unsigned int)rd.Width, (unsigned int)rd.Height, (unsigned)desc->Format);
                }
                // Scene-set evidence (rotation tracking); see above.
                SceneSetNote(res, (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)desc->Format);
            } else if (desc->Format == DXGI_FORMAT_R16G16_FLOAT) {
                g_mvW = (unsigned int)rd.Width;
                g_mvH = (unsigned int)rd.Height;
                { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
                if (!g_mvValid) {
                    if (!g_mvValid) g_mvFirstValidFrame = g_frameCounter;
                    if (!StoreTracked(&g_mvResource, res)) {
                        if (Real_CreateRenderTargetView)
                            Real_CreateRenderTargetView(device, res, desc, handle);
                        return;
                    }
                    SetTrackedResourceGeneration(&g_mvResource, res);
                    g_mvValid = true;
                    g_mvStamp = g_frameCounter;
                    g_mvLastRtvKey = handle.ptr;
                    Log("hooks: motion vector RTV %p (%ux%u R16G16_FLOAT)", (void*)res, g_mvW, g_mvH);
                } else if (res == g_mvResource || res == g_mvResourceAlt) {
                    if (res == g_mvResource) {
                        if (!g_mvValid) g_mvFirstValidFrame = g_frameCounter;
                        if (StoreTracked(&g_mvResource, res))
                            SetTrackedResourceGeneration(&g_mvResource, res);
                        Log("hooks: motion vector RTV handle refreshed %p", (void*)res);
                    } else {
                        if (StoreTracked(&g_mvResourceAlt, res))
                            SetTrackedResourceGeneration(&g_mvResourceAlt, res);
                    }
                } else if (res != g_mvResourceAlt) {
                    if (StoreTracked(&g_mvResourceAlt, res))
                        SetTrackedResourceGeneration(&g_mvResourceAlt, res);
                    Log("hooks: motion vector RTV %p (%ux%u R16G16_FLOAT) (ALT)", (void*)res, g_mvW, g_mvH);
                }
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1 &&
                   desc->Format == DXGI_FORMAT_R16G16_FLOAT) {
            { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
            if (!g_mvValid) {
                if (!g_mvValid) g_mvFirstValidFrame = g_frameCounter;
                if (!StoreTracked(&g_mvResource, res)) {
                    if (Real_CreateRenderTargetView)
                        Real_CreateRenderTargetView(device, res, desc, handle);
                    return;
                }
                SetTrackedResourceGeneration(&g_mvResource, res);
                g_mvValid = true;
                g_mvStamp = g_frameCounter;
                g_mvW = (unsigned int)rd.Width;
                g_mvH = (unsigned int)rd.Height;
                g_mvLastRtvKey = handle.ptr;
                Log("hooks: motion vector RTV %p (1920x1001 R16G16_FLOAT)", (void*)res);
            } else if (res == g_mvResource || res == g_mvResourceAlt) {
                if (res == g_mvResource) {
                    if (StoreTracked(&g_mvResource, res))
                        SetTrackedResourceGeneration(&g_mvResource, res);
                    Log("hooks: motion vector RTV handle refreshed %p", (void*)res);
                } else {
                    if (StoreTracked(&g_mvResourceAlt, res))
                        SetTrackedResourceGeneration(&g_mvResourceAlt, res);
                }
            } else if (res != g_mvResourceAlt) {
                if (StoreTracked(&g_mvResourceAlt, res))
                    SetTrackedResourceGeneration(&g_mvResourceAlt, res);
                g_mvStamp = g_frameCounter;
                g_mvW = (unsigned int)rd.Width;
                g_mvH = (unsigned int)rd.Height;
                Log("hooks: motion vector RTV %p (1920x1001 R16G16_FLOAT) (ALT)", (void*)res);
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   desc->Format == DXGI_FORMAT_R16G16_FLOAT && rd.MipLevels == 1 &&
                   rd.Width > 0 && rd.Height > 0) {
            { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
            static int s_otherMvRtvs = 0;
            if (s_otherMvRtvs < 10) {
                ++s_otherMvRtvs;
                Log("hooks: other R16G16_FLOAT RTV %p (%ux%u)", (void*)res,
                    (unsigned int)rd.Width, (unsigned int)rd.Height);
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   IsSceneColorFormat((unsigned)desc->Format) && rd.MipLevels == 1 &&
                   rd.Width >= 1000 && rd.Height >= 500) {
            // View re-created at any time: keep the handle map fresh so
            // OMSetRenderTargets can resolve the scene color even if the
            // resource itself was discovered earlier.
            { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
            if (res == g_sceneColor) {
                g_sceneColorRtv = handle;
                Log("hooks: scene color RTV handle refreshed (map) %p", (void*)res);
            } else if (res == g_sceneColorAlt) {
                g_sceneColorRtvAlt = handle;
                Log("hooks: scene color ALT RTV handle refreshed (map) %p", (void*)res);
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   rd.MipLevels == 1 && rd.Width > 0 && rd.Height > 0) {
            { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
        }
    }
    if (Real_CreateRenderTargetView)
        Real_CreateRenderTargetView(device, res, desc, handle);
}

void Hook_CreateShaderResourceView(ID3D12Device* device, ID3D12Resource* res,
                                   const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    // DISCOVERY MUST ALWAYS RUN (same reason as CreateRenderTargetView)
    if (device == g_device && res && desc && desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
        desc->Texture2D.MipLevels == 1) {
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        ObserveNativeCandidate(res, rd, 2);
        // Discovery only. Creating an SRV neither binds nor transitions the
        // resource, so it must not be used as state or content-freshness proof.
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.MipLevels == 1 &&
            rd.Width == g_displayW && rd.Height == g_displayH &&
            (desc->Format == DXGI_FORMAT_R32_FLOAT || desc->Format == DXGI_FORMAT_R32_TYPELESS ||
             desc->Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || desc->Format == DXGI_FORMAT_D32_FLOAT)) {
            // Metadata only on a real store: a rejected self-adopt must not
            // corrupt the slot's engine metadata (valid/stamp/fmt/srvSourced).
            // Forwarding below always runs regardless.
            ID3D12Resource* previousDepth = g_depthResource;
            if (!StoreTracked(&g_depthResource, res)) { /* rejected: keep prior engine metadata */ }
            else {
            // This SRV is a newly observed view of the currently returned
            // resource address, so it may refresh the slot's lifetime token.
            SetTrackedResourceGeneration(&g_depthResource, res);
            g_depthValid = true;
            g_depthSrvSourced = true;
            g_depthStamp = g_frameCounter;
            g_depthRealFmt = rd.Format;
            g_depthMsaa = rd.SampleDesc.Count != 1;
            static volatile LONG s_depthCandidateEvents = 0;
            LONG candidateEvent = InterlockedIncrement(&s_depthCandidateEvents);
            if (previousDepth != res || candidateEvent <= 16 || (candidateEvent % 200) == 0)
                Log("hooks: depth-candidate source=SRV event=%ld previous=%p current=%p res=%ux%u fmt=%u samples=%u viewFmt=%u view=%llX srvSource=1 present=%llu ecl=%llu",
                    candidateEvent, (void*)previousDepth, (void*)res,
                    (unsigned)rd.Width, (unsigned)rd.Height, (unsigned)rd.Format,
                    (unsigned)rd.SampleDesc.Count, (unsigned)desc->Format,
                    (unsigned long long)handle.ptr,
                    (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
                    (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
            }
        }
        // MV SRV discovery is not an actual sample: descriptors are often
        // created once and reused. Keep candidate discovery, but do not stamp
        // recency or invent a shader-resource state here.
        // NOTE: NOT gated by g_shadowRealInputs — the engine may create SRVs
        // before REAL mode is toggled on (config init timing is racy at
        // startup). Unconditional storage is safe: it only adds handle→resource
        // entries; the MV-specific touch stamp only fires on R16G16F match.
        if (res &&
            rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.MipLevels == 1 &&
            rd.Format == DXGI_FORMAT_R16G16_FLOAT &&
            (unsigned)rd.Width == g_displayW && rd.Height == g_displayH &&
            !IsOwnResource(res)) {
            if (res != g_mvResource && res != g_mvResourceAlt) {
                if (StoreTracked(&g_mvResourceAlt, res)) {
                    SetTrackedResourceGeneration(&g_mvResourceAlt, res);
                    g_mvStamp = g_frameCounter;
                    if (!g_mvFirstValidFrame) g_mvFirstValidFrame = g_frameCounter;
                    g_mvW = (unsigned int)rd.Width; g_mvH = (unsigned int)rd.Height;
                    static int s_mvSrvLog = 0;
                    if (s_mvSrvLog++ < 4)
                        Log("hooks: MV candidate SRV %p (%ux%u)", (void*)res, (unsigned)rd.Width, (unsigned)rd.Height);
                }
            } else if (res == g_mvResource) {
                if (StoreTracked(&g_mvResource, res))
                    SetTrackedResourceGeneration(&g_mvResource, res);
            } else {
                if (StoreTracked(&g_mvResourceAlt, res))
                    SetTrackedResourceGeneration(&g_mvResourceAlt, res);
            }
        }
        // Store handle→resource mapping for ALL 2D SRVs so CopyDescriptors
        // propagation can later resolve copied handles in shader-visible heaps.
        { BookGuard _bg2; g_srvMap[handle.ptr] = res; }
    }
    if (Real_CreateShaderResourceView)
        Real_CreateShaderResourceView(device, res, desc, handle);
}

void Hook_CreateDepthStencilView(ID3D12Device* device, ID3D12Resource* res,
                                 const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                 D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    // The handle map supports later OM depth-bind observation. View creation
    // is not evidence that the depth resource is currently bound or written.
    if (device == g_device && handle.ptr) {
        BookGuard guard;
        if (res) g_dsvMap[handle.ptr] = res;
        else g_dsvMap.erase(handle.ptr);
    }
    if (Real_CreateDepthStencilView)
        Real_CreateDepthStencilView(device, res, desc, handle);
}

void TryDeferredInject(ID3D12CommandQueue* injQueue);
void InjectAtPresentImpl(ID3D12CommandQueue* injQueue);
void TryQueueOutputCopy(ID3D12CommandQueue* queue);

// --------------------------------------------------------------------------
// CopyDescriptors / CopyDescriptorsSimple hooks
//
// Descriptor-copy hooks propagate CPU-handle bookkeeping only. Copying a
// descriptor does not execute GPU work, bind a resource, or prove freshness.
// The mappings are used to resolve later RTV/DSV binds; unknown overwrites
// invalidate prior entries rather than preserving stale identities.
// --------------------------------------------------------------------------
static void EraseDescriptorMappingRange(std::map<SIZE_T, ID3D12Resource*>& mapping,
                                        SIZE_T start, UINT count, UINT incrementSize);

void Hook_CopyDescriptorsSimple(ID3D12Device* device, UINT numDescriptors,
                                D3D12_CPU_DESCRIPTOR_HANDLE destStart,
                                D3D12_CPU_DESCRIPTOR_HANDLE srcStart,
                                D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeaps)
{
    if (Real_CopyDescriptorsSimple) Real_CopyDescriptorsSimple(device, numDescriptors,
        destStart, srcStart, DescriptorHeaps);

    if (numDescriptors == 0 || !device || device != g_device) return;

    // Only track RTV and CBV_SRV_UAV (SRV/UAV) heap copies — these are the
    // descriptor types used for render targets and shader resources.
    if (DescriptorHeaps != D3D12_DESCRIPTOR_HEAP_TYPE_RTV &&
        DescriptorHeaps != D3D12_DESCRIPTOR_HEAP_TYPE_DSV &&
        DescriptorHeaps != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return;

    // Resolve increment size to walk the descriptor range.
    UINT incrementSize = device->GetDescriptorHandleIncrementSize(DescriptorHeaps);
    if (incrementSize == 0) return;

    // Descriptor bookkeeping is intentionally NOT a resource freshness signal:
    // copying a CPU descriptor says nothing about GPU execution or contents.
    // Bound the snapshot to keep this hook's work predictable. For a larger
    // overwrite, invalidate any known destination mappings instead of risking
    // stale handle->resource associations.
    static const UINT kMaxTrackedCopy = 256;
    BookGuard _bg;
    std::map<SIZE_T, ID3D12Resource*>& mapping =
        DescriptorHeaps == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? g_rtvMap :
        DescriptorHeaps == D3D12_DESCRIPTOR_HEAP_TYPE_DSV ? g_dsvMap : g_srvMap;
    if (numDescriptors > kMaxTrackedCopy) {
        EraseDescriptorMappingRange(mapping, destStart.ptr, numDescriptors, incrementSize);
        return;
    }
    if ((SIZE_T)(numDescriptors - 1) > ((~(SIZE_T)0) - srcStart.ptr) / incrementSize ||
        (SIZE_T)(numDescriptors - 1) > ((~(SIZE_T)0) - destStart.ptr) / incrementSize)
        return;

    ID3D12Resource* copied[kMaxTrackedCopy] = {};
    for (UINT i = 0; i < numDescriptors; ++i) {
        SIZE_T srcPtr = srcStart.ptr + (SIZE_T)i * incrementSize;
        auto it = mapping.find(srcPtr);
        if (it != mapping.end()) copied[i] = it->second;
    }
    for (UINT i = 0; i < numDescriptors; ++i)
        mapping.erase(destStart.ptr + (SIZE_T)i * incrementSize);
    for (UINT i = 0; i < numDescriptors; ++i) {
        if (copied[i])
            mapping[destStart.ptr + (SIZE_T)i * incrementSize] = copied[i];
    }
}

static void EraseDescriptorMappingRange(std::map<SIZE_T, ID3D12Resource*>& mapping,
                                        SIZE_T start, UINT count, UINT incrementSize)
{
    if (!count || !incrementSize ||
        (SIZE_T)count > ((~(SIZE_T)0) - start) / incrementSize)
        return;
    const SIZE_T end = start + (SIZE_T)count * incrementSize;
    auto it = mapping.lower_bound(start);
    while (it != mapping.end() && it->first < end)
        it = mapping.erase(it);
}

void Hook_CopyDescriptors(ID3D12Device* device,
                          UINT numDestDescriptorRanges,
                          const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                          const UINT* pDestDescriptorCounts,
                          UINT numSrcDescriptorRanges,
                          const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                          const UINT* pSrcDescriptorCounts,
                          D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeaps)
{
    if (Real_CopyDescriptors) Real_CopyDescriptors(device, numDestDescriptorRanges,
        pDestDescriptorRangeStarts, pDestDescriptorCounts,
        numSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorCounts,
        DescriptorHeaps);

    if (!pDestDescriptorRangeStarts || !pSrcDescriptorRangeStarts ||
        !device || device != g_device) return;

    if (DescriptorHeaps != D3D12_DESCRIPTOR_HEAP_TYPE_RTV &&
        DescriptorHeaps != D3D12_DESCRIPTOR_HEAP_TYPE_DSV &&
        DescriptorHeaps != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return;

    UINT incrementSize = device->GetDescriptorHandleIncrementSize(DescriptorHeaps);
    if (incrementSize == 0) return;

    static const UINT kMaxTrackedCopy = 256;
    BookGuard _bg;
    std::map<SIZE_T, ID3D12Resource*>& mapping =
        DescriptorHeaps == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? g_rtvMap :
        DescriptorHeaps == D3D12_DESCRIPTOR_HEAP_TYPE_DSV ? g_dsvMap : g_srvMap;

    // CopyDescriptors concatenates source ranges and destination ranges; the
    // range indexes are NOT paired. Snapshot the flattened source sequence
    // before invalidating destinations so overlapping copies remain correct.
    UINT destTotal = 0, srcTotal = 0;
    bool bounded = true;
    for (UINT r = 0; r < numDestDescriptorRanges; ++r) {
        const UINT n = pDestDescriptorCounts ? pDestDescriptorCounts[r] : 1;
        if (n > kMaxTrackedCopy - destTotal) { bounded = false; break; }
        destTotal += n;
    }
    for (UINT r = 0; r < numSrcDescriptorRanges; ++r) {
        const UINT n = pSrcDescriptorCounts ? pSrcDescriptorCounts[r] : 1;
        if (n > kMaxTrackedCopy - srcTotal) { bounded = false; break; }
        srcTotal += n;
    }

    // Invalid or oversized operations cannot safely be resolved. Invalidate
    // known destination mappings so they cannot masquerade as current data.
    if (!bounded || destTotal != srcTotal) {
        for (UINT r = 0; r < numDestDescriptorRanges; ++r) {
            const UINT n = pDestDescriptorCounts ? pDestDescriptorCounts[r] : 1;
            EraseDescriptorMappingRange(mapping, pDestDescriptorRangeStarts[r].ptr, n, incrementSize);
        }
        return;
    }

    ID3D12Resource* copied[kMaxTrackedCopy] = {};
    UINT flat = 0;
    for (UINT r = 0; r < numSrcDescriptorRanges; ++r) {
        const UINT n = pSrcDescriptorCounts ? pSrcDescriptorCounts[r] : 1;
        for (UINT i = 0; i < n; ++i, ++flat) {
            if ((SIZE_T)i > ((~(SIZE_T)0) - pSrcDescriptorRangeStarts[r].ptr) / incrementSize)
                continue;
            const SIZE_T srcPtr = pSrcDescriptorRangeStarts[r].ptr + (SIZE_T)i * incrementSize;
            auto it = mapping.find(srcPtr);
            if (it != mapping.end()) copied[flat] = it->second;
        }
    }

    for (UINT r = 0; r < numDestDescriptorRanges; ++r) {
        const UINT n = pDestDescriptorCounts ? pDestDescriptorCounts[r] : 1;
        EraseDescriptorMappingRange(mapping, pDestDescriptorRangeStarts[r].ptr, n, incrementSize);
    }

    flat = 0;
    for (UINT r = 0; r < numDestDescriptorRanges; ++r) {
        const UINT n = pDestDescriptorCounts ? pDestDescriptorCounts[r] : 1;
        for (UINT i = 0; i < n; ++i, ++flat) {
            if (!copied[flat] ||
                (SIZE_T)i > ((~(SIZE_T)0) - pDestDescriptorRangeStarts[r].ptr) / incrementSize)
                continue;
            const SIZE_T destPtr = pDestDescriptorRangeStarts[r].ptr + (SIZE_T)i * incrementSize;
            mapping[destPtr] = copied[flat];
        }
    }
}
void InjectAtPresent();
bool g_inInject = false;

void EnsureGlobalSwapchainHookImpl();

void Hook_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists,
                              ID3D12CommandList* const* lists)
{
    // Keep the first real direct queue captured by CreateCommandQueue. BeamNG
    // exposes several direct queue interfaces; replacing this pointer on each
    // callback makes queue-to-Present correlation meaningless.
    if (queue && !g_graphicsQueue)
        g_graphicsQueue = queue;
    EnsureGlobalSwapchainHookImpl();
    static int s_execCalls = 0;
    if (s_execCalls < 5) {
        ++s_execCalls;
        Log("hooks: ExecuteCommandLists #%d (queue %p, %u lists)", s_execCalls, (void*)queue, numLists);
    }
    if (g_device && lists && numLists > 0) {
        // No s_hookedLists gate: WasListHooked + InstallCommandListHooks cover
        // dedup under g_commandListShimLock (see below).
        for (UINT i = 0; i < numLists; ++i) {
            ID3D12GraphicsCommandList* cl = nullptr;
            if (SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&cl))) && cl) {
                // Bounded producer census: an observed closed recording has
                // an OM-bound display-sized fmt-34 target and direct draws,
                // and this list pointer is present in the submitted batch.
                // This is not GPU-completion, contents, or frame-alignment
                // proof. Indirect draws/bundles and unshimmed records are gaps.
                MvRecordingSnapshot mvSnapshot = {};
                bool haveMvSnapshot = CaptureMvRecordingSnapshot(cl, &mvSnapshot);
                if (haveMvSnapshot && !mvSnapshot.clonedVtblMatches) {
                    InterlockedIncrement64(&g_mvRecordingCloneMismatchTotal);
                    // A mismatch means our recording hooks may have missed a
                    // later Reset/recording on this object. Report only the
                    // last MV-like state our shim observed, never label it as
                    // the current recording or as fresh GPU work.
                    if (mvSnapshot.target && mvSnapshot.omCount > 0 &&
                        (mvSnapshot.drawCount > 0 || mvSnapshot.indexedDrawCount > 0)) {
                        static volatile LONG64 s_lastObservedMvDetail = 0;
                        LONG64 detail = InterlockedIncrement64(&s_lastObservedMvDetail);
                        if (detail <= 8 || (detail % 5000) == 0) {
                            Log("mv-record: last-observed-on-mismatched-list detail=%lld list=%p queue=%p batchIndex=%u batchCount=%u actualVtbl=%p expectedClone=%p expectedOriginal=%p type=%u epoch=%lld closed=%u stableAtSnapshot=%u resetInProgress=%u resetFailed=%u target=%p size=%dx%d om=%lld draw=%lld indexed=%lld dispatch=%lld recordPresent=%lld recordEcl=%lld submitPresent=%lld submitEcl=%lld",
                                detail, (void*)cl, (void*)queue, i, numLists,
                                mvSnapshot.actualVtbl, mvSnapshot.expectedClonedVtbl,
                                mvSnapshot.expectedOriginalVtbl, mvSnapshot.listType,
                                (long long)mvSnapshot.epoch, mvSnapshot.closed ? 1u : 0u,
                                mvSnapshot.stable ? 1u : 0u,
                                mvSnapshot.resetInProgress ? 1u : 0u,
                                mvSnapshot.resetFailed ? 1u : 0u,
                                (void*)mvSnapshot.target, (int)mvSnapshot.width,
                                (int)mvSnapshot.height, (long long)mvSnapshot.omCount,
                                (long long)mvSnapshot.drawCount,
                                (long long)mvSnapshot.indexedDrawCount,
                                (long long)mvSnapshot.dispatchCount,
                                (long long)mvSnapshot.recordPresent,
                                (long long)mvSnapshot.recordEcl,
                                (long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
                                (long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
                        }
                    }
                }
                if (haveMvSnapshot && mvSnapshot.clonedVtblMatches &&
                    mvSnapshot.stable && mvSnapshot.omCount > 0 &&
                    (mvSnapshot.drawCount > 0 || mvSnapshot.indexedDrawCount > 0)) {
                    LONG64 submitted = InterlockedIncrement64(&g_mvRecordingSubmittedTotal);
                    LONG64 logged = InterlockedIncrement64(&g_mvRecordingSubmittedDetail);
                    if (logged <= 12 || (submitted % 1000) == 0) {
                        Log("mv-record: submitted count=%lld list=%p queue=%p batchIndex=%u batchCount=%u type=%u epoch=%lld cloneVtbl=%u target=%p size=%dx%d om=%lld draw=%lld indexed=%lld dispatch=%lld recordPresent=%lld recordEcl=%lld submitPresent=%lld submitEclBefore=%lld",
                            (long long)submitted, (void*)cl, (void*)queue, i, numLists,
                            mvSnapshot.listType, (long long)mvSnapshot.epoch,
                            mvSnapshot.clonedVtblMatches ? 1u : 0u,
                            (void*)mvSnapshot.target, (int)mvSnapshot.width,
                            (int)mvSnapshot.height, (long long)mvSnapshot.omCount,
                            (long long)mvSnapshot.drawCount,
                            (long long)mvSnapshot.indexedDrawCount,
                            (long long)mvSnapshot.dispatchCount,
                            (long long)mvSnapshot.recordPresent,
                            (long long)mvSnapshot.recordEcl,
                            (long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
                    }
                }
                // Diagnostic only: record shim coverage per submitted list.
                // Does not change install/forwarding behavior below.
                // Type comes from our own shim table when available; no guarded
                // vtable call here because __try is illegal in this function
                // (std::vector requires unwinding).
                // Exact totals (nd/hits/misses/ptrReg/ptrUnreg) count every
                // submission; sampled records only report snapshots of them.
                {
                    CommandListShim* preShim = FindCommandListShim(cl);
                    unsigned clType = preShim ? preShim->listType : 0xFFFFFFFFu;
                    static volatile LONG s_eclListDiag = 0;
                    static volatile LONG64 s_eclHitTotal = 0;
                    static volatile LONG64 s_eclMissTotal = 0;
                    static volatile LONG64 s_eclPtrRegTotal = 0;
                    static volatile LONG64 s_eclPtrUnregTotal = 0;
                    static volatile LONG64 s_eclDetailLogged = 0;
                    if (preShim) InterlockedIncrement64(&s_eclHitTotal);
                    else InterlockedIncrement64(&s_eclMissTotal);
                    LONG nd = InterlockedIncrement(&s_eclListDiag);
                    bool sampled = (nd <= 20 || (nd % 500) == 1);
                    bool ptrRegistered = false;
                    void* actual = nullptr;
                    void* expectedCloned = nullptr;
                    void* expectedOriginal = nullptr;
                    if (!preShim) {
                        ptrRegistered = FindShimByListOnly(cl, &actual, &expectedCloned, &expectedOriginal);
                        if (ptrRegistered) InterlockedIncrement64(&s_eclPtrRegTotal);
                        else InterlockedIncrement64(&s_eclPtrUnregTotal);
                    }
                    if (sampled) {
                        LONG64 hits = InterlockedCompareExchange64(&s_eclHitTotal, 0, 0);
                        LONG64 misses = InterlockedCompareExchange64(&s_eclMissTotal, 0, 0);
                        LONG64 ptrReg = InterlockedCompareExchange64(&s_eclPtrRegTotal, 0, 0);
                        LONG64 ptrUnreg = InterlockedCompareExchange64(&s_eclPtrUnregTotal, 0, 0);
                        LONG64 instOk = InterlockedCompareExchange64(&g_installOkTotal, 0, 0);
                        LONG64 instDedup = InterlockedCompareExchange64(&g_installDedupTotal, 0, 0);
                        LONG64 instFail = InterlockedCompareExchange64(&g_installFailTotal, 0, 0);
                        Log("hooks: ECL list list=%p shimHit=%d type=%u (n=%d hits=%lld misses=%lld ptrReg=%lld ptrUnreg=%lld instOk=%lld instDedup=%lld instFail=%lld)",
                            (void*)cl, preShim ? 1 : 0, clType, (int)nd, hits, misses,
                            ptrReg, ptrUnreg, instOk, instDedup, instFail);
                        Log("mv-record: coverage submitted=%lld om=%lld qualifyingOm=%lld draws=%lld resets=%lld/%lld/%lld closes=%lld/%lld/%lld cloneMismatch=%lld",
                            (long long)InterlockedCompareExchange64(&g_mvRecordingSubmittedTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingOmSetTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingQualifyingOmTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingDrawTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingResetTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingResetSuccessTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingResetFailTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingCloseTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingCloseSuccessTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingCloseFailTotal, 0, 0),
                            (long long)InterlockedCompareExchange64(&g_mvRecordingCloneMismatchTotal, 0, 0));
                    }
                    // Integrity detail only on sampled misses whose pointer is
                    // registered: snapshot states whether our clone is intact,
                    // what the actual vtable's memory looks like, and how the
                    // submitted base-interface pointer compares to the derived
                    // pointer we shimmed. A third heap value proves neither
                    // writer nor reuse by itself. Capped by its own emit
                    // counter (increments only on emit).
                    if (!preShim && sampled && ptrRegistered) {
                        ShimIntegritySnapshot snap = {};
                        if (CaptureShimIntegrity(cl, lists[i], &snap)) {
                            LONG64 logged = InterlockedIncrement64(&s_eclDetailLogged);
                            if (logged <= 10) {
                                Log("hooks: ECL list mismatch list=%p registered=1 actualVtbl=%p expectedCloned=%p expectedOriginal=%p fullHit=0 type=%u cloneReadable=%d cloneSlot15IsShim=%d cloneSlot15=%p actualRegionBase=%p actualRegionSize=%llu actualProtect=0x%X actualState=0x%X actualType=0x%X actualSlot15Readable=%d actualSlot15=%p actualSlot15Mod=%p baseList=%p baseVtblReadable=%d baseVtbl=%p baseSamePtr=%d baseVtblIsActual=%d baseVtblIsOriginal=%d baseVtblIsCloned=%d baseRegionBase=%p baseRegionSize=%llu baseProtect=0x%X (detail=%lld)",
                                    (void*)cl, snap.actualVtbl, snap.expectedCloned, snap.expectedOriginal, clType,
                                    snap.clonedReadable ? 1 : 0, snap.cloneSlot15IsShim ? 1 : 0, snap.cloneSlot15Value,
                                    snap.actualRegionBase, (unsigned long long)snap.actualRegionSize,
                                    (unsigned)snap.actualProtect, (unsigned)snap.actualState, (unsigned)snap.actualType,
                                    snap.actualSlot15Readable ? 1 : 0, snap.actualSlot15Value, snap.actualSlot15Module,
                                    snap.baseList, snap.baseVtblReadable ? 1 : 0, snap.baseVtbl,
                                    (snap.baseList == (void*)cl) ? 1 : 0,
                                    (snap.baseVtblReadable && snap.baseVtbl == snap.actualVtbl) ? 1 : 0,
                                    (snap.baseVtblReadable && snap.baseVtbl == snap.expectedOriginal) ? 1 : 0,
                                    (snap.baseVtblReadable && snap.baseVtbl == snap.expectedCloned) ? 1 : 0,
                                    snap.baseRegionBase, (unsigned long long)snap.baseRegionSize,
                                    (unsigned)snap.baseProtect, logged);
                                if (snap.slotsReadable) {
                                    Log("hooks: ECL list slots list=%p s9=%s/%ls s10=%s/%ls s15=%s/%ls s16=%s/%ls s26=%s/%ls s46=%s/%ls (detail=%lld)",
                                        (void*)cl,
                                        SlotClass(snap.actualSlot[0], snap.origSlot[0], snap.clonedSlot[0]), snap.slotModBase[0],
                                        SlotClass(snap.actualSlot[1], snap.origSlot[1], snap.clonedSlot[1]), snap.slotModBase[1],
                                        SlotClass(snap.actualSlot[2], snap.origSlot[2], snap.clonedSlot[2]), snap.slotModBase[2],
                                        SlotClass(snap.actualSlot[3], snap.origSlot[3], snap.clonedSlot[3]), snap.slotModBase[3],
                                        SlotClass(snap.actualSlot[4], snap.origSlot[4], snap.clonedSlot[4]), snap.slotModBase[4],
                                        SlotClass(snap.actualSlot[5], snap.origSlot[5], snap.clonedSlot[5]), snap.slotModBase[5],
                                        logged);
                                } else {
                                    Log("hooks: ECL list slots list=%p unreadable (detail=%lld)", (void*)cl, logged);
                                }
                            }
                        }
                    }
                }
                if (!WasListHooked(cl)) {
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
    // Discovery phase: the real queue hook is intentionally observation-only.
    // Injection remains Present-owned until the queue identity and submission
    // cadence are proven against BeamNG's actual graphics queue.
    if (!g_inInject && queue && queue == g_graphicsQueue &&
        InterlockedCompareExchange(&g_gameQueueObserved, 0, 0)) {
        g_inInject = true;
        static unsigned s_gameEcl = 0;
        ++s_gameEcl;
        const unsigned long long eclSerial = (unsigned long long)InterlockedIncrement64(&g_eclSerial);
        if (s_gameEcl <= 8 || (s_gameEcl % 600) == 0)
            Log("hooks: GAME ExecuteCommandLists observed #%u queue=%p lists=%u frame=%u",
                s_gameEcl, (void*)queue, numLists, g_frameCounter);
        if (s_gameEcl <= 8 || (s_gameEcl % 600) == 0)
            Log("hooks: ECL correlation serial=%llu queue=%p presentSerial=%llu",
                eclSerial, (void*)queue,
                (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0));
        // Latch the steady-state submit queue once (see declaration). Validated
        // GAME branch only; never rewritten, no behavior change by itself.
        if (!InterlockedCompareExchangePointer((void**)&g_gameSubmitQueue, queue, nullptr))
            Log("hooks: submit queue latched=%p (steady-state render stream)", (void*)queue);
        TryQueueOutputCopy(queue);
        // Observation-only for now. Calling the full pipeline from every ECL
        // submission enters BeamNG during render-graph construction and
        // crashes before the first helper acknowledgement. The confirmed
        // queue identity is retained for a later, narrowly gated copy pass.
        g_inInject = false;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue(
    ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc,
    REFIID riid, void** outQueue)
{
    if (!Real_CreateCommandQueue)
        return E_POINTER;
    HRESULT hr = Real_CreateCommandQueue(device, desc, riid, outQueue);
    if (SUCCEEDED(hr) && outQueue && *outQueue && device == g_device && desc &&
        desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12CommandQueue* q = nullptr;
        IUnknown* returned = reinterpret_cast<IUnknown*>(*outQueue);
        if (returned && SUCCEEDED(returned->QueryInterface(IID_PPV_ARGS(&q))) && q) {
            if (InterlockedCompareExchange(&g_gameQueueObserved, 1, 0) == 0) {
                g_graphicsQueue = q;
                Log("hooks: GAME direct queue captured=%p", (void*)q);
            }
            void** qv = *(void***)q;
            void* ecl = qv[10];
            if (InterlockedCompareExchange(&g_realQueueHookInstalled, 1, 0) == 0) {
                MH_STATUS st = MH_CreateHook(ecl, (void*)&Hook_ExecuteCommandLists,
                                             (void**)&Real_ExecuteCommandLists);
                if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
                    MH_STATUS en = MH_EnableHook(ecl);
                    Log("hooks: real queue ECL hook %s target=%p st=%d enable=%d",
                        en == MH_OK ? "INSTALLED" : "FAILED", ecl, (int)st, (int)en);
                } else {
                    InterlockedExchange(&g_realQueueHookInstalled, 0);
                    Log("hooks: real queue ECL hook create FAILED target=%p st=%d", ecl, (int)st);
                }
            }
            q->Release();
        }
    }
    return hr;
}

// Cold-path observer only.  Do not patch any command-list method here: the
// renderer's hot methods run through driver/wrapper state where MinHook caused
// artifacts and crashes in earlier experiments.  This hook only records that a
// real game command list was created and exposes its vtable identity for the
// next read-only observation step.
static HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(
    ID3D12Device* device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* initialState,
    REFIID riid, void** outList)
{
    if (!Real_CreateCommandList)
        return E_POINTER;
    HRESULT hr = Real_CreateCommandList(device, nodeMask, type, allocator,
                                        initialState, riid, outList);
    if (SUCCEEDED(hr) && outList && *outList) {
        static LONG s_created = 0;
        LONG n = InterlockedIncrement(&s_created);
        // ALL-DEVICE EXPERIMENT (camera-path audit): creation-shim lists from
        // every game device, not just g_device. Dormant runs create >=10
        // DIRECT+COPY lists on a second device that were never shimmed, while
        // active runs have zero skipped creations. Per-list originals make
        // forwarding table-agnostic, and submit-time install already touches
        // any device's lists, so this only moves the same safe op earlier
        // (before the game records into the list). Excluded after the first
        // NGX init attempt: NGX creates its own internal devices during Init,
        // whose lists must stay shim-free (Map/adopt analysis must never
        // touch NVIDIA-owned resources).
        bool postInit = (InterlockedCompareExchange(&g_upscalerInitAttempted, 0, 0) != 0);
        bool cover = (device == g_device) || !postInit;
        if (n <= 12 || (n % 500) == 0) {
            void* list = *outList;
            void** vtbl = nullptr;
            __try { vtbl = *(void***)list; } __except (EXCEPTION_EXECUTE_HANDLER) { }
            Log("hooks: GAME CreateCommandList #%d list=%p type=%u vtbl=%p device=%p primary=%d covered=%d allocator=%p riid=%08X:%04X:%04X",
                (int)n, list, (unsigned)type, (void*)vtbl, (void*)device,
                (device == g_device) ? 1 : 0, cover ? 1 : 0, (void*)allocator,
                (unsigned)riid.Data1,
                (unsigned)riid.Data2,
                (unsigned)riid.Data3);
        }
        // Bundle/compute census (diagnostic): the first-12 cap above can miss
        // late-created bundle lists, and UNORM scene traffic could hide in
        // bundle payloads (ExecuteBundle slot 27 is unhooked). Type-1/2 lists
        // have never appeared in any run's sampled logs; record the first 10
        // unconditionally to test that hypothesis with certainty.
        if ((type == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
             type == D3D12_COMMAND_LIST_TYPE_COMPUTE) &&
            outList && *outList) {
            static volatile LONG s_type12Logs = 0;
            LONG m12 = InterlockedIncrement(&s_type12Logs);
            if (m12 <= 10) {
                void* blist = *outList;
                void** bvtbl = nullptr;
                __try { bvtbl = *(void***)blist; } __except (EXCEPTION_EXECUTE_HANDLER) { }
                Log("hooks: CreateCommandList type12 #%d list=%p type=%u vtbl=%p device=%p primary=%d",
                    (int)m12, blist, (unsigned)type, (void*)bvtbl, (void*)device,
                    (device == g_device) ? 1 : 0);
            }
        }
        if (cover) {
            // Attach before BeamNG records any work into the newly-created list.
            // Installing at ExecuteCommandLists is too late: all Copy/OM/Barrier
            // calls for that submission have already happened by then.
            InstallCommandListHooks((ID3D12GraphicsCommandList*)*outList, (UINT)type);
        } else {
            // Post-NGX-init foreign device: quantify only (no behavior change).
            static volatile LONG s_skippedCreate = 0;
            LONG m = InterlockedIncrement(&s_skippedCreate);
            if (m <= 10 || (m % 500) == 1) {
                Log("hooks: CreateCommandList skipped (post-NGX-init device) device=%p g_device=%p list=%p type=%u (n=%d)",
                    (void*)device, (void*)g_device, *outList, (unsigned)type, (int)m);
            }
        }
    }
    return hr;
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
    // ISOLATION (reviewer #16 pattern): SCALENG_NO_INITRES=1 skips wrapped-
    // device resource creation on this background thread - prime suspect for
    // the nvwgf2umx freeze class (every freeze fires immediately after
    // 'present-injection resources created' from this thread).
    static const bool s_noInitRes = GetEnvironmentVariableA("SCALENG_NO_INITRES", nullptr, 0) > 0;
    for (;;) {
        WaitForSingleObject(g_initThreadEv, INFINITE);
        if (s_noInitRes) {
            Log("hooks: init thread - EnsureInjectionResources SKIPPED (isolation mode)");
            return 0;
        }
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

// ============================================================================
// SELF-CONTAINED NGX PIPELINE
// Everything created lazily on first successful Present. No hooks needed
// except Present itself. No bridge, no shared textures, no cross-device.
// ============================================================================

static ID3D12Resource* g_ngxColor = nullptr;     // our color input (copy of bb)
static ID3D12Resource* g_ngxDepth = nullptr;     // our depth (zeros = autoexposure)
static ID3D12Resource* g_ngxMv = nullptr;        // our MV (zeros = static frame)
static ID3D12Resource* g_ngxOut = nullptr;       // NGX output
static ID3D12CommandAllocator* g_ngxAlloc = nullptr;
static ID3D12GraphicsCommandList* g_ngxList = nullptr;
static ID3D12CommandQueue* g_ngxQueue = nullptr;    // our own queue
static bool g_ngxPipelineReady = false;
static unsigned g_ngxFrameCount = 0;

static void CreateNgxTextures(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
    // Release any previous set (size change / re-init) - prevents leak.
    if (g_ngxColor) { g_ngxColor->Release(); g_ngxColor = nullptr; }
    if (g_ngxDepth) { g_ngxDepth->Release(); g_ngxDepth = nullptr; }
    if (g_ngxMv)    { g_ngxMv->Release();    g_ngxMv = nullptr; }
    if (g_ngxOut)   { g_ngxOut->Release();   g_ngxOut = nullptr; }

    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;

    // ALL textures created in COMMON: the per-frame pipeline transitions from
    // COMMON explicitly and restores to COMMON at the end. Single source of
    // truth - no state drift (proven device-killer in the smoke test).
    rd.Format = fmt;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_ngxColor));

    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_ngxDepth));

    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // explicit: no stale carry-over
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_ngxMv));

    rd.Format = fmt;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_ngxOut));

    Log("ngx-pipe: textures created %ux%u COMMON (color=%p depth=%p mv=%p out=%p)",
        w, h, (void*)g_ngxColor, (void*)g_ngxDepth, (void*)g_ngxMv, (void*)g_ngxOut);
}

// ============================================================================
// ============================================================================
// FENCE-ORDERED SHARED-HANDLE BRIDGE (b2 - coexists with dormant legacy vars)
// NGX runs on OUR clean NVIDIA device (200/200 proven); the game's wrapped
// device only ever does copies. Simultaneous-access textures need no cross-
// device state tracking. GPU-side fences sequence the hops - no CPU blocking.
//
//   game queue : copy bb->shColor ; signal fIn(v)
//   our queue  : wait fIn(v) ; NGX shColor->shOut ; signal fOut(v)
//   game queue : wait fOut(v) ; copy shOut->bb
//
// FAILURE LAW: fOut is ALWAYS signaled (queue-level Signal, list-independent).
// A failed NGX frame degrades to stale-output passthrough - never deadlock.
// ============================================================================

static ID3D12Device*              g_b2Dev     = nullptr;
static ID3D12CommandQueue*        g_b2Q       = nullptr;
static ID3D12CommandAllocator*    g_b2Alloc   = nullptr;
static ID3D12GraphicsCommandList* g_b2List    = nullptr;
static ID3D12Resource*            g_b2ColorG  = nullptr;
static ID3D12Resource*            g_b2ColorO  = nullptr;
static ID3D12Resource*            g_b2OutG    = nullptr;
static ID3D12Resource*            g_b2OutO    = nullptr;
static ID3D12Fence*               g_b2FenceInG  = nullptr;
static ID3D12Fence*               g_b2FenceOutG = nullptr;
static ID3D12Fence*               g_b2FIO  = nullptr;
static ID3D12Fence*               g_b2FOO  = nullptr;
static UINT64                     g_b2Val  = 0;
static volatile LONG              g_b2DeferredPending = 0;
static UINT64                     g_b2DeferredVal = 0;
static ID3D12Resource*            g_b2PresentBb = nullptr; // weak, current Present backbuffer
static bool                       g_b2Ready = false;
static bool                       g_ngxBlocked = false; // wrapper detected -> bridge mode
static UINT                       g_b2W = 0, g_b2H = 0;
static DXGI_FORMAT                g_b2Fmt = DXGI_FORMAT_UNKNOWN;
static ID3D12Resource*            g_b2Depth = nullptr;
static ID3D12Resource*            g_b2Mv    = nullptr;
static ID3D12CommandAllocator*    g_b2CopyAlloc = nullptr;
static ID3D12GraphicsCommandList* g_b2CopyList = nullptr;
static bool                       g_b2QueueCopy = false;
static volatile LONG              g_vectorAOneShot = 0; // Vector A aggressive one-shot handoff
static bool                       g_vectorAEnabled = false; // disabled for provenance diagnostic
static volatile LONG              g_vectorBOneShot = 0; // Vector B OM hijack one-shot
static bool                       g_vectorBEnabled = true;
static volatile LONG              g_vectorBArmed = 0;
static ID3D12Resource*            g_vectorBArmedRes = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE g_vectorBArmedHandle = {};
static unsigned                   g_vectorBArmedSamples = 0;
static UINT64                     g_vectorBArmedPending = 0;
static bool                       g_vectorBTestActive = false;
static unsigned                   g_vectorBTestFrames = 0;
static const unsigned             g_vectorBTestTotal = 1000;
static unsigned long long         g_vectorBTestStartPresent = 0;
static D3D12_CPU_DESCRIPTOR_HANDLE g_vectorBTestHandle = {};
static ID3D12Resource*            g_vectorBTestResource = nullptr;

template <typename T>
static bool B2OpenShared(ID3D12Device* dev, ID3D12DeviceChild* obj, T** out)
{
    HANDLE h = nullptr;
    if (FAILED(g_device->CreateSharedHandle(obj, nullptr, GENERIC_ALL, nullptr, &h)) || !h)
        return false;
    HRESULT hr = dev->OpenSharedHandle(h, IID_PPV_ARGS(out));
    CloseHandle(h);
    return SUCCEEDED(hr) && *out;
}

static void B2ReleasePair()
{
    // NOTE: fences are intentionally NOT released here - they persist across
    // resizes so the helper never desyncs (dimension-independent objects).
    for (auto** p : { &g_b2ColorG, &g_b2ColorO, &g_b2OutG, &g_b2OutO,
                      &g_b2Depth, &g_b2Mv })
        if (*p) { (*p)->Release(); *p = nullptr; }
}

static void B2EnsureDummyInputs(UINT w, UINT h)
{
    static UINT s_w = 0, s_h = 0;
    if (s_w == w && s_h == h && g_b2Depth && g_b2Mv) return;
    if (g_b2Depth) { g_b2Depth->Release(); g_b2Depth = nullptr; }
    if (g_b2Mv)    { g_b2Mv->Release();    g_b2Mv = nullptr; }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    g_b2Dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_b2Depth));
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    g_b2Dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_b2Mv));
    s_w = w; s_h = h;
}

// ---- cross-process helper management --------------------------------------
static HANDLE g_b2Helper  = nullptr; // child process
static HANDLE g_b2Pipe    = nullptr;
static bool   g_b2UseHelper = false; // opt-in via ScaleNG.ini [bridge] helper=1
static bool   g_b2ReplaceOutput = true; // copy DLSS result back into game bb
static bool   g_b2DeferredOutput = false; // hand off through engine copy hook
static SRWLOCK g_b2PipeLock = SRWLOCK_INIT; // Present can enter concurrently
// The bridge reuses two allocators/lists and the current backbuffer. Present
// may be re-entered during queue submission (and can arrive from another
// render thread during resize), so only one bridge transaction may record or
// submit at a time. A skipped transaction leaves the engine's Present path
// untouched and the caller releases its GetBuffer reference.
static SRWLOCK g_b2FrameLock = SRWLOCK_INIT;
static HANDLE g_b2HColor  = nullptr, g_b2HOut = nullptr;
static HANDLE g_b2HFIn    = nullptr, g_b2HFOut = nullptr;
// These are handle values in the CURRENT helper process. They must be cleared
// whenever that process is replaced; numeric HANDLE values are not portable
// across processes or helper restarts.
static unsigned long long g_b2FInDup = 0, g_b2FOutDup = 0;
struct B2FrameAck { unsigned long long value; unsigned int recorded; unsigned int reserved; };
static bool g_b2FramePending = false;
static unsigned long long g_b2PendingValue = 0;

static void B2KillOrphans()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!lstrcmpiW(pe.szExeFile, L"ScaleNG_NGX_helper.exe") &&
                pe.th32ProcessID != GetCurrentProcessId()) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (h) {
                    Log("ngx-b2: terminating orphan helper pid=%lu", pe.th32ProcessID);
                    TerminateProcess(h, 0);
                    WaitForSingleObject(h, 2000);
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static void B2KillHelper()
{
    if (g_b2Pipe)    { CloseHandle(g_b2Pipe);   g_b2Pipe = nullptr; }
    if (g_b2Helper)  { TerminateProcess(g_b2Helper, 0); CloseHandle(g_b2Helper); g_b2Helper = nullptr; }
    g_b2FInDup = 0;
    g_b2FOutDup = 0;
}

static void B2CheckIniFlag()
{
    static bool s_checked = false;
    if (s_checked) return;
    s_checked = true;
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    wchar_t* sl = wcsrchr(p, L'\\');
    if (sl) *(sl + 1) = 0;
    lstrcatW(p, L"plugins\\ScaleNG.ini");
    wchar_t buf[16] = {};
    GetPrivateProfileStringW(L"bridge", L"helper", L"0", buf, 15, p);
    g_b2UseHelper = _wtoi(buf) != 0;
    GetPrivateProfileStringW(L"bridge", L"replaceOutput", L"1", buf, 15, p);
    g_b2ReplaceOutput = _wtoi(buf) != 0;
    GetPrivateProfileStringW(L"bridge", L"deferredOutput", L"0", buf, 15, p);
    g_b2DeferredOutput = _wtoi(buf) != 0;
    GetPrivateProfileStringW(L"bridge", L"queueCopy", L"0", buf, 15, p);
    g_b2QueueCopy = _wtoi(buf) != 0;
    Log("ngx-b2: helper mode %s (ini [bridge] helper), output replacement %s, deferred handoff %s, queue copy %s",
        g_b2UseHelper ? "ENABLED" : "off", g_b2ReplaceOutput ? "ENABLED" : "disabled",
        g_b2DeferredOutput ? "ENABLED" : "off", g_b2QueueCopy ? "ENABLED" : "off");
}

static bool TryVectorA(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY, UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* srcBox, CommandListShim* shim)
{
    if (!g_vectorAEnabled || !g_b2DeferredOutput || !g_b2DeferredPending || !list || !dst || !src ||
        !dst->pResource || !src->pResource ||
        dst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        src->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        dst->SubresourceIndex != 0 || src->SubresourceIndex != 0 ||
        dstX != 0 || dstY != 0 || dstZ != 0 ||
        dst->pResource == g_b2ColorG || dst->pResource == g_b2OutG ||
        src->pResource == g_b2ColorG || src->pResource == g_b2OutG ||
        !g_b2OutG || !g_b2FenceOutG)
        return false;
    unsigned long long presentSerial = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    if (presentSerial <= 300) return false;
    if (InterlockedCompareExchange(&g_vectorAOneShot, 0, 1) != 0) return false;
    bool doIt = false;
    D3D12_RESOURCE_DESC dd = {};
    D3D12_RESOURCE_DESC od = {};
    D3D12_RESOURCE_DESC sd = {};
    SafeGetDesc(dst->pResource, &dd);
    SafeGetDesc(g_b2OutG, &od);
    SafeGetDesc(src->pResource, &sd);
    const UINT copyW = srcBox ? (UINT)(srcBox->right - srcBox->left) : (UINT)sd.Width;
    const UINT copyH = srcBox ? (UINT)(srcBox->bottom - srcBox->top) : (UINT)sd.Height;
    const UINT64 pending = g_b2DeferredVal;
    UINT64 completed = SafeGetFenceCompleted(g_b2FenceOutG);
    bool dstIsDisplay = dd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                        dd.Width == od.Width && dd.Height == od.Height &&
                        dd.Format == od.Format && od.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                        dd.Width == g_displayW && dd.Height == g_displayH;
    bool srcIsDisplaySized = sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                             sd.Width == g_displayW && sd.Height == g_displayH &&
                             sd.Width >= 1000 && sd.Height >= 500;
    bool fullFrame = copyW == (UINT)dd.Width && copyH == (UINT)dd.Height && sd.Width == dd.Width && sd.Height == dd.Height;
    if (dstIsDisplay && srcIsDisplaySized && fullFrame && completed >= pending && pending != 0) {
        bool isPersistent = false;
        AcquireSRWLockShared(&g_sceneColorCandidateLock);
        for (auto &c : g_sceneColorCandidates) {
            if (c.resource == (void*)src->pResource && c.samples >= 3 && c.width == g_displayW && c.height == g_displayH) { isPersistent = true; break; }
        }
        ReleaseSRWLockShared(&g_sceneColorCandidateLock);
        doIt = true;
        Log("vectorA: attempting handoff dst=%p %ux%u fmt=%u src=%p %ux%u fmt=%u od=%ux%u fmt=%u pending=%llu completed=%llu persistent=%u present=%llu",
            (void*)dst->pResource, (unsigned)dd.Width, (unsigned)dd.Height, (unsigned)dd.Format,
            (void*)src->pResource, (unsigned)sd.Width, (unsigned)sd.Height, (unsigned)sd.Format,
            (unsigned)od.Width, (unsigned)od.Height, (unsigned)od.Format,
            (unsigned long long)pending, (unsigned long long)completed, isPersistent ? 1u : 0u, presentSerial);
    }
    if (doIt && InterlockedCompareExchange(&g_b2DeferredPending, 0, 1) == 1) {
        D3D12_TEXTURE_COPY_LOCATION replacement = { g_b2OutG, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_b2OutG;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (shim && shim->resourceBarrier) shim->resourceBarrier(list, 1, &b);
        else if (Real_ResourceBarrier) Real_ResourceBarrier(list, 1, &b);
        if (shim && shim->copyTexture) shim->copyTexture(list, dst, dstX, dstY, dstZ, &replacement, srcBox);
        else if (Real_CopyTextureRegion) Real_CopyTextureRegion(list, dst, dstX, dstY, dstZ, &replacement, srcBox);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        if (shim && shim->resourceBarrier) shim->resourceBarrier(list, 1, &b);
        else if (Real_ResourceBarrier) Real_ResourceBarrier(list, 1, &b);
        Log("vectorA: DLSS output substituted frame=%llu dst=%p srcOrig=%p persistent=%u", (unsigned long long)pending, (void*)dst->pResource, (void*)src->pResource, doIt ? 1u : 0u);
        return true;
    } else {
        InterlockedExchange(&g_vectorAOneShot, 0);
        return false;
    }
}

static bool TryVectorB(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL singleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* depth, CommandListShim* shim)
{
    if (!g_vectorBEnabled || !list || !handles || count == 0)
        return false;
    unsigned long long presentSerial = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    unsigned long long eclSerial = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
    UINT64 pending = g_b2DeferredVal;
    UINT64 completed = SafeGetFenceCompleted(g_b2FenceOutG);
    bool fenceReady = pending != 0 && completed >= pending && g_b2DeferredPending;
    bool sawDisplaySized = false;
    for (UINT i = 0; i < count && i < 8; ++i) {
        if (!handles[i].ptr) continue;
        ID3D12Resource* res = nullptr;
        {
            BookGuard guard;
            auto it = g_rtvMap.find(handles[i].ptr);
            if (it != g_rtvMap.end()) res = it->second;
        }
        if (!res) continue;
        D3D12_RESOURCE_DESC d = {};
        if (!SafeGetDesc(res, &d)) continue;
        bool isDisplay = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_displayW && d.Height == g_displayH && (d.Format == DXGI_FORMAT_R16G16B16A16_UNORM || d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (isDisplay) { sawDisplaySized = true; break; }
    }
    bool shouldLogDetails = sawDisplaySized || fenceReady;
    if (shouldLogDetails) {
        for (UINT i = 0; i < count && i < 8; ++i) {
            if (!handles[i].ptr) continue;
            ID3D12Resource* res = nullptr;
            bool foundInMap = false;
            {
                BookGuard guard;
                auto it = g_rtvMap.find(handles[i].ptr);
                if (it != g_rtvMap.end()) { res = it->second; foundInMap = true; }
            }
        D3D12_RESOURCE_DESC d = {};
        bool gotDesc = res ? SafeGetDesc(res, &d) : false;
        bool isDisplay = gotDesc && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_displayW && d.Height == g_displayH && (d.Format == DXGI_FORMAT_R16G16B16A16_UNORM || d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (handles[i].ptr == g_lastDisplayRTVHandle.ptr && handles[i].ptr != 0) {
            Log("vectorB: lastDisplay bound handle=%llX resource=%p present=%llu ecl=%llu", (unsigned long long)handles[i].ptr, (void*)res, presentSerial, eclSerial);
        }
        // Resource-level correlation: same resource even if handle recycled
        {
            bool isTrackedRes = false;
            D3D12_CPU_DESCRIPTOR_HANDLE creationHandle = {};
            unsigned long long creationPresent = 0;
            unsigned trackedSamples = 0;
            AcquireSRWLockShared(&g_sceneColorCandidateLock);
            for (auto &c : g_sceneColorCandidates) {
                if (c.resource == (void*)res && c.width == g_displayW && c.height == g_displayH) { isTrackedRes = true; trackedSamples = c.samples; break; }
            }
            ReleaseSRWLockShared(&g_sceneColorCandidateLock);
            if (!isTrackedRes) {
                for (unsigned h = 0; h < 8; ++h) {
                    if (g_displayRTVHistory[h].resource == res) {
                        isTrackedRes = true;
                        creationHandle = g_displayRTVHistory[h].handle;
                        creationPresent = g_displayRTVHistory[h].present;
                        trackedSamples = 1;
                        break;
                    }
                }
            } else {
                for (unsigned h = 0; h < 8; ++h) {
                    if (g_displayRTVHistory[h].resource == res) { creationHandle = g_displayRTVHistory[h].handle; creationPresent = g_displayRTVHistory[h].present; break; }
                }
                if (!creationHandle.ptr) { creationHandle = handles[i]; creationPresent = presentSerial; }
            }
            if (res && isTrackedRes) {
                bool isPersistent = trackedSamples >= 1;
                bool recycled = creationHandle.ptr != handles[i].ptr && creationHandle.ptr != 0;
                UINT64 pendingRes = g_b2DeferredVal;
                UINT64 completedRes = SafeGetFenceCompleted(g_b2FenceOutG);
                Log("vectorB: resource-match creationHandle=%llX creationRes=%p creationPresent=%llu omHandle=%llX omRes=%p omPresent=%llu size=%ux%u fmt=%u samples=%u pending=%llu completed=%llu persistent=%u recycled=%u", (unsigned long long)creationHandle.ptr, (void*)res, creationPresent, (unsigned long long)handles[i].ptr, (void*)res, presentSerial, gotDesc ? (unsigned)d.Width : 0, gotDesc ? (unsigned)d.Height : 0, gotDesc ? (unsigned)d.Format : 0, trackedSamples, (unsigned long long)pendingRes, (unsigned long long)completedRes, isPersistent ? 1u : 0u, recycled ? 1u : 0u);
            }
        }
        if (isDisplay) sawDisplaySized = true;
        unsigned samples = 0;
            if (foundInMap && gotDesc) {
                AcquireSRWLockShared(&g_sceneColorCandidateLock);
                for (auto &c : g_sceneColorCandidates) {
                    if (c.resource == (void*)res && c.width == g_displayW && c.height == g_displayH) { samples = c.samples; break; }
                }
                ReleaseSRWLockShared(&g_sceneColorCandidateLock);
            }
            Log("vectorB: slot=%u handle=%llX foundInMap=%u resource=%p size=%ux%u fmt=%u samples=%u present=%llu ecl=%llu pending=%llu completed=%llu isDisplay=%u", i, (unsigned long long)handles[i].ptr, foundInMap ? 1u : 0u, (void*)res, gotDesc ? (unsigned)d.Width : 0u, gotDesc ? (unsigned)d.Height : 0u, gotDesc ? (unsigned)d.Format : 0u, samples, presentSerial, eclSerial, (unsigned long long)pending, (unsigned long long)completed, isDisplay ? 1u : 0u);
        }
        if (sawDisplaySized) {
            Log("vectorB: omCall count=%u singleRange=%u present=%llu ecl=%llu handles=%llX %llX %llX %llX", count, singleRange ? 1u : 0u, presentSerial, eclSerial, count>0 ? (unsigned long long)handles[0].ptr : 0, count>1 ? (unsigned long long)handles[1].ptr : 0, count>2 ? (unsigned long long)handles[2].ptr : 0, count>3 ? (unsigned long long)handles[3].ptr : 0);
            BookGuard guard;
            unsigned dumped = 0;
            for (auto &kv : g_rtvMap) {
                if (dumped >= 16) break;
                D3D12_RESOURCE_DESC d = {};
                bool got = kv.second ? SafeGetDesc(kv.second, &d) : false;
                Log("vectorB: rtvMap handle=%llX resource=%p size=%ux%u fmt=%u", (unsigned long long)kv.first, (void*)kv.second, got ? (unsigned)d.Width : 0u, got ? (unsigned)d.Height : 0u, got ? (unsigned)d.Format : 0u);
                ++dumped;
            }
            if (dumped == 0) Log("vectorB: rtvMap empty at present=%llu", presentSerial);
        }
    }
    // Resource-level correlation for every OM that binds a tracked display resource â€” unconditional, handle-independent
    for (UINT i = 0; i < count && i < 8; ++i) {
        if (!handles[i].ptr) continue;
        ID3D12Resource* res = nullptr;
        {
            BookGuard guard;
            auto it = g_rtvMap.find(handles[i].ptr);
            if (it != g_rtvMap.end()) res = it->second;
            else {
                auto it2 = g_displayRTVMap.find(handles[i].ptr);
                if (it2 != g_displayRTVMap.end()) res = it2->second;
            }
        }
        if (!res) {
            // Also try reverse lookup via display history resource handle
            for (unsigned h = 0; h < 8; ++h) {
                if (g_displayRTVHistory[h].handle.ptr == handles[i].ptr) { res = g_displayRTVHistory[h].resource; break; }
            }
        }
        if (!res) continue;
        bool isTracked = false;
        D3D12_CPU_DESCRIPTOR_HANDLE creationHandle = {};
        unsigned long long creationPresent = 0;
        unsigned trackedSamples = 0;
        AcquireSRWLockShared(&g_sceneColorCandidateLock);
        for (auto &c : g_sceneColorCandidates) {
            if (c.resource == (void*)res && c.width == g_displayW && c.height == g_displayH) { isTracked = true; trackedSamples = c.samples; break; }
        }
        ReleaseSRWLockShared(&g_sceneColorCandidateLock);
        if (!isTracked) {
            for (unsigned h = 0; h < 8; ++h) {
                if (g_displayRTVHistory[h].resource == res) {
                    isTracked = true;
                    creationHandle = g_displayRTVHistory[h].handle;
                    creationPresent = g_displayRTVHistory[h].present;
                    trackedSamples = 1;
                    break;
                }
            }
        } else {
            for (unsigned h = 0; h < 8; ++h) {
                if (g_displayRTVHistory[h].resource == res) { creationHandle = g_displayRTVHistory[h].handle; creationPresent = g_displayRTVHistory[h].present; break; }
            }
            if (!creationHandle.ptr) { creationHandle = handles[i]; creationPresent = presentSerial; }
        }
        if (res && isTracked) {
            D3D12_RESOURCE_DESC d = {};
            bool gotDesc = SafeGetDesc(res, &d);
            bool isPersistent = trackedSamples >= 1;
            Log("vectorB: resource-match creationHandle=%llX creationRes=%p creationPresent=%llu omHandle=%llX omRes=%p omPresent=%llu size=%ux%u fmt=%u samples=%u pending=%llu completed=%llu persistent=%u recycled=%u", (unsigned long long)creationHandle.ptr, (void*)res, creationPresent, (unsigned long long)handles[i].ptr, (void*)res, presentSerial, gotDesc ? (unsigned)d.Width : 0, gotDesc ? (unsigned)d.Height : 0, gotDesc ? (unsigned)d.Format : 0, trackedSamples, (unsigned long long)pending, (unsigned long long)completed, isPersistent ? 1u : 0u);
        }
    }
    // 1000-frame test: if test active, handle continued substitution
    if (g_vectorBTestActive) {
        if (presentSerial > g_vectorBTestStartPresent + g_vectorBTestTotal) {
            Log("vectorB: TEST END after %u frames startPresent=%llu endPresent=%llu", g_vectorBTestFrames, g_vectorBTestStartPresent, presentSerial);
            g_vectorBTestActive = false;
            g_vectorBTestFrames = 0;
            InterlockedExchange(&g_vectorBArmed, 0);
            InterlockedExchange(&g_vectorBOneShot, 0);
            return false;
        }
        for (UINT i = 0; i < count && i < 8; ++i) {
            if (!handles[i].ptr) continue;
            if (handles[i].ptr != g_vectorBTestHandle.ptr) continue;
            ID3D12Resource* resCheck = nullptr;
            {
                BookGuard guard;
                auto it = g_rtvMap.find(handles[i].ptr);
                if (it != g_rtvMap.end()) resCheck = it->second;
                else {
                    auto it2 = g_displayRTVMap.find(handles[i].ptr);
                    if (it2 != g_displayRTVMap.end()) resCheck = it2->second;
                }
            }
            if (!resCheck || resCheck != g_vectorBTestResource) {
                Log("vectorB: TEST STOP resource mismatch handle=%llX", (unsigned long long)handles[i].ptr);
                g_vectorBTestActive = false;
                InterlockedExchange(&g_vectorBArmed, 0);
                return false;
            }
            if (!g_b2DeferredPending || g_b2DeferredVal == 0 || SafeGetFenceCompleted(g_b2FenceOutG) < g_b2DeferredVal) {
                Log("vectorB: TEST STOP fence not ready");
                g_vectorBTestActive = false;
                InterlockedExchange(&g_vectorBArmed, 0);
                return false;
            }
            if (InterlockedCompareExchange(&g_b2DeferredPending, 0, 1) != 1) {
                Log("vectorB: TEST skip pending race");
                return false;
            }
            bool ok = SafeOverwriteRTV(g_b2OutG, handles[i]);
            if (ok) {
                D3D12_RESOURCE_DESC d = {};
                SafeGetDesc(g_vectorBTestResource, &d);
                g_vectorBTestFrames++;
                if (g_vectorBTestFrames == 1 || (g_vectorBTestFrames % 60) == 0) {
                    Log("vectorB: TEST FRAME %u/%u handle=%llX resource=%p present=%llu ecl=%llu pending=%llu", g_vectorBTestFrames, g_vectorBTestTotal, (unsigned long long)handles[i].ptr, (void*)g_vectorBTestResource, presentSerial, eclSerial, (unsigned long long)g_b2DeferredVal);
                }
                if (shim && shim->omSetRenderTargets) shim->omSetRenderTargets(list, count, handles, singleRange, depth);
                return true;
            } else {
                Log("vectorB: TEST STOP CreateRTV failed");
                InterlockedExchange(&g_b2DeferredPending, 1);
                g_vectorBTestActive = false;
                InterlockedExchange(&g_vectorBArmed, 0);
                return false;
            }
        }
        return false;
    }
    if (presentSerial > 300 && g_b2DeferredPending && pending != 0 && completed >= pending) {
        ID3D12Resource* bestRes = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE bestHandle = {};
        unsigned bestSamples = 0;
        {
            AcquireSRWLockShared(&g_sceneColorCandidateLock);
            for (auto &c : g_sceneColorCandidates) {
                if (!c.resource || c.width != g_displayW || c.height != g_displayH) continue;
                if (c.samples < 1) continue;
                if (c.resource == (void*)g_bbCached) continue;
                BookGuard guard;
                for (auto &kv : g_rtvMap) {
                    if (kv.second == (ID3D12Resource*)c.resource) {
                        bestRes = (ID3D12Resource*)c.resource;
                        bestHandle.ptr = kv.first;
                        bestSamples = c.samples;
                        break;
                    }
                }
                if (bestRes) break;
            }
            ReleaseSRWLockShared(&g_sceneColorCandidateLock);
        }
        // Use actual OM resource that matches tracked display resource (resource identity authoritative, format 10/11 both accepted)
        ID3D12Resource* omMatchedRes = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE omMatchedHandle = {};
        unsigned omMatchedSamples = 0;
        for (UINT i2 = 0; i2 < count && i2 < 8; ++i2) {
            if (!handles[i2].ptr) continue;
            ID3D12Resource* resCheck2 = nullptr;
            {
                BookGuard guard2;
                auto it2 = g_rtvMap.find(handles[i2].ptr);
                if (it2 != g_rtvMap.end()) resCheck2 = it2->second;
                else {
                    auto it3 = g_displayRTVMap.find(handles[i2].ptr);
                    if (it3 != g_displayRTVMap.end()) resCheck2 = it3->second;
                }
            }
            if (!resCheck2) continue;
            D3D12_RESOURCE_DESC d2 = {};
            if (!SafeGetDesc(resCheck2, &d2)) continue;
            bool isDisplay2 = d2.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d2.Width == g_displayW && d2.Height == g_displayH && (d2.Format == DXGI_FORMAT_R16G16B16A16_UNORM || d2.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (!isDisplay2) continue;
            unsigned samples2 = 0;
            AcquireSRWLockShared(&g_sceneColorCandidateLock);
            for (auto &c2 : g_sceneColorCandidates) {
                if (c2.resource == (void*)resCheck2 && c2.width == g_displayW && c2.height == g_displayH) { samples2 = c2.samples; break; }
            }
            ReleaseSRWLockShared(&g_sceneColorCandidateLock);
            if (samples2 < 1) {
                for (unsigned h2 = 0; h2 < 8; ++h2) {
                    if (g_displayRTVHistory[h2].resource == resCheck2) { samples2 = 1; break; }
                }
            }
            if (samples2 < 1) continue;
            if (resCheck2 == g_bbCached) continue;
            omMatchedRes = resCheck2;
            omMatchedHandle = handles[i2];
            omMatchedSamples = samples2;
            break;
        }
        if (!omMatchedRes) {
            // No OM in this call matches a tracked display resource â€” keep armed for later OM if any bestRes exists
            if (bestRes && !g_vectorBArmed) {
                if (!g_vectorBArmed) {
                    g_vectorBArmedRes = bestRes;
                    g_vectorBArmedHandle = bestHandle;
                    g_vectorBArmedSamples = bestSamples;
                    g_vectorBArmedPending = pending;
                    InterlockedExchange(&g_vectorBArmed, 1);
                    Log("vectorB: armed handle=%llX resource=%p size=%ux%u fmt=%u samples=%u present=%llu pending=%llu completed=%llu", (unsigned long long)bestHandle.ptr, (void*)bestRes, g_displayW, g_displayH, 11, bestSamples, presentSerial, (unsigned long long)pending, (unsigned long long)completed);
                }
            } else {
                Log("vectorB: omMatched found handle=%llX resource=%p present=%llu", (unsigned long long)omMatchedHandle.ptr, (void*)omMatchedRes, presentSerial);
                // omRes == tracked display resource is authoritative â€” start test for this exact OM resource, not bestRes
                if (!g_vectorBTestActive && omMatchedHandle.ptr != 0 && omMatchedRes) {
                    g_vectorBTestActive = true;
                    g_vectorBTestFrames = 0;
                    g_vectorBTestStartPresent = presentSerial;
                    g_vectorBTestHandle = omMatchedHandle;
                    g_vectorBTestResource = omMatchedRes;
                    D3D12_CPU_DESCRIPTOR_HANDLE creationHandle2 = {};
                    unsigned long long creationPresent2 = 0;
                    for (unsigned h = 0; h < 8; ++h) {
                        if (g_displayRTVHistory[h].resource == omMatchedRes) { creationHandle2 = g_displayRTVHistory[h].handle; creationPresent2 = g_displayRTVHistory[h].present; break; }
                    }
                    if (!creationHandle2.ptr) { creationHandle2 = omMatchedHandle; creationPresent2 = presentSerial; }
                    bool recycled2 = creationHandle2.ptr != omMatchedHandle.ptr && creationHandle2.ptr != 0;
                    Log("vectorB: TEST START reason=OM_RESOURCE_MATCH handle=%llX resource=%p creationHandle=%llX creationRes=%p creationPresent=%llu omPresent=%llu ecl=%llu size=%ux%u fmt=%u samples=%u pending=%llu completed=%llu recycled=%u", (unsigned long long)omMatchedHandle.ptr, (void*)omMatchedRes, (unsigned long long)creationHandle2.ptr, (void*)omMatchedRes, creationPresent2, presentSerial, eclSerial, g_displayW, g_displayH, 11, omMatchedSamples, (unsigned long long)pending, (unsigned long long)completed, recycled2 ? 1u : 0u);
                    g_vectorBArmedRes = omMatchedRes;
                    g_vectorBArmedHandle = omMatchedHandle;
                    g_vectorBArmedSamples = omMatchedSamples;
                    g_vectorBArmedPending = pending;
                    InterlockedExchange(&g_vectorBArmed, 1);
                }
            }
        }
    }
    if (g_vectorBArmed && presentSerial > 300) {
        for (UINT i = 0; i < count && i < 8; ++i) {
            if (!handles[i].ptr) continue;
            if (handles[i].ptr != g_vectorBArmedHandle.ptr) continue;
            Log("vectorB: armed-match handle=%llX resource=%p samples=%u present=%llu ecl=%llu pending=%llu completed=%llu", (unsigned long long)handles[i].ptr, (void*)g_vectorBArmedRes, g_vectorBArmedSamples, presentSerial, eclSerial, (unsigned long long)g_vectorBArmedPending, (unsigned long long)SafeGetFenceCompleted(g_b2FenceOutG));
            if (InterlockedCompareExchange(&g_vectorBOneShot, 0, 1) != 0) {
                Log("vectorB: skipped one-shot already consumed");
                return false;
            }
            if (!g_b2DeferredPending || g_vectorBArmedPending != g_b2DeferredVal || SafeGetFenceCompleted(g_b2FenceOutG) < g_b2DeferredVal) {
                Log("vectorB: skipped fence not ready at match");
                InterlockedExchange(&g_vectorBOneShot, 0);
                return false;
            }
            if (InterlockedCompareExchange(&g_b2DeferredPending, 0, 1) != 1) {
                Log("vectorB: skipped pending race at match");
                InterlockedExchange(&g_vectorBOneShot, 0);
                return false;
            }
            bool ok = SafeOverwriteRTV(g_b2OutG, handles[i]);
            if (ok) {
                D3D12_RESOURCE_DESC d = {};
                SafeGetDesc(g_vectorBArmedRes, &d);
                Log("vectorB: substituted handle=%llX origRes=%p size=%ux%u fmt=%u present=%llu ecl=%llu pending=%llu repl=%p", (unsigned long long)handles[i].ptr, (void*)g_vectorBArmedRes, (unsigned)d.Width, (unsigned)d.Height, (unsigned)d.Format, presentSerial, eclSerial, (unsigned long long)g_vectorBArmedPending, (void*)g_b2OutG);
                if (shim && shim->omSetRenderTargets) shim->omSetRenderTargets(list, count, handles, singleRange, depth);
                InterlockedExchange(&g_vectorBArmed, 0);
                return true;
            } else {
                Log("vectorB: skipped CreateRTV failed at match handle=%llX", (unsigned long long)handles[i].ptr);
                InterlockedExchange(&g_b2DeferredPending, 1);
                InterlockedExchange(&g_vectorBOneShot, 0);
                InterlockedExchange(&g_vectorBArmed, 0);
                return false;
            }
        }
    }
    return false;
}


static bool B2StartHelper()
{
    if (!g_b2UseHelper) return false;
    if (g_b2Pipe) return true;
    wchar_t self[MAX_PATH] = {};
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&B2StartHelper, &mod);
    if (mod) GetModuleFileNameW(mod, self, MAX_PATH);
    wchar_t* slash = wcsrchr(self, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wchar_t exe[MAX_PATH];
    lstrcpyW(exe, self); lstrcatW(exe, L"ScaleNG_NGX_helper.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        Log("ngx-b2: helper exe missing at %ls", exe);
        g_b2UseHelper = false;
        return false;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    char pipeName[128] = {};
    // Include a launch nonce. A PID-only endpoint can remain occupied by a
    // stale server instance or collide with a second module instance during
    // renderer restart, producing ERROR_PIPE_BUSY forever.
    _snprintf_s(pipeName, _TRUNCATE, "\\\\.\\pipe\\ScaleNG_NGX_%lu_%lu",
                GetCurrentProcessId(), GetTickCount());
    g_b2Pipe = CreateNamedPipeA(pipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, sizeof(unsigned long long) * 8, sizeof(unsigned long long) * 8, 0, &sa);
    if (g_b2Pipe == INVALID_HANDLE_VALUE) {
        Log("ngx-b2: CreateNamedPipe FAILED err=%lu", GetLastError());
        // Helper mode is an explicit safety requirement for BeamNG's wrapped
        // device. Keep it enabled so a transient endpoint collision cannot
        // send the frame into the incompatible in-process fallback.
        g_b2Pipe = nullptr;
        return false;
    }

    wchar_t cmd[MAX_PATH * 2];
    _snwprintf_s(cmd, _TRUNCATE, L"\"%ls\" %hs %lu", exe, pipeName, GetCurrentProcessId());
    PROCESS_INFORMATION pi = {};
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log("ngx-b2: spawn FAILED err=%lu", GetLastError());
        CloseHandle(g_b2Pipe); g_b2Pipe = nullptr;
        return false;
    }
    g_b2Helper = pi.hProcess; CloseHandle(pi.hThread);

    if (!ConnectNamedPipe(g_b2Pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        Log("ngx-b2: ConnectNamedPipe FAILED err=%lu", GetLastError());
        B2KillHelper();
        return false;
    }

    unsigned int hello[2] = { 0x58474E53, 1 }; // 'SNGX'
    unsigned int ack[2] = {};
    DWORD wr = 0, rd = 0;
    if (!WriteFile(g_b2Pipe, hello, sizeof(hello), &wr, nullptr) ||
        !ReadFile(g_b2Pipe, ack, sizeof(ack), &rd, nullptr) ||
        ack[0] != 0x58474E48) {
        Log("ngx-b2: handshake FAILED (ack=%08X)", ack[0]);
        B2KillHelper();
        return false;
    }
Log("ngx-b2: helper connected pid=%lu", GetProcessId(g_b2Helper));
    return true;
}

#pragma pack(push, 1)
struct B2SetupMsg {
    unsigned long long hColor, hOut, hFIn, hFOut;
    unsigned int w, h, fmt;
    unsigned int pad = 0;
    unsigned long long startVal = 0; // epoch-sync for helper fence loop
};
static_assert(sizeof(B2SetupMsg) == 56, "B2SetupMsg size mismatch - must be 56 bytes");
#pragma pack(pop)

static bool B2SendSetup(UINT w, UINT h, DXGI_FORMAT fmt)
{
    AcquireSRWLockExclusive(&g_b2PipeLock);
    struct PipeUnlock { SRWLOCK* lock; ~PipeUnlock() { ReleaseSRWLockExclusive(lock); } } pipeUnlock{ &g_b2PipeLock };
    if (!B2StartHelper()) return false;

    // A resize setup message must not overtake the acknowledgement for the
    // one frame currently in flight. The pipe is byte-oriented; if setup is
    // sent first, its four-byte response can consume the first four bytes of
    // a pending frame acknowledgement and permanently desynchronize the
    // protocol. Wait only for this single outstanding frame, never for a
    // backlog (the frame path enforces one-frame backpressure).
    {
        const DWORD deadline = GetTickCount() + 5000;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(g_b2Pipe, nullptr, 0, nullptr, &avail, nullptr))
                return false;
            if (avail >= sizeof(B2FrameAck)) {
                B2FrameAck pendingAck = {};
                DWORD got = 0;
                if (!ReadFile(g_b2Pipe, &pendingAck, sizeof(pendingAck), &got, nullptr) ||
                    got != sizeof(pendingAck)) return false;
                if (g_b2FramePending && pendingAck.value == g_b2PendingValue)
                    g_b2FramePending = false;
                else if (pendingAck.value != g_b2PendingValue)
                    Log("ngx-b2: resize drained unexpected ack v=%llu expected=%llu",
                        (unsigned long long)pendingAck.value,
                        (unsigned long long)g_b2PendingValue);
                // Drain every complete ACK already queued. This also covers
                // a stale ACK left behind when the pending flag was cleared
                // on a previous Present re-entry.
                continue;
            }
            if (avail != 0) {
                // A partial ACK must be completed before setup is written;
                // otherwise the setup response can consume its remaining
                // bytes and become a value such as 0x000005B9.
                if (avail < sizeof(B2FrameAck)) {
                    if ((LONG)(GetTickCount() - deadline) >= 0) return false;
                    Sleep(1);
                    continue;
                }
            }
            if (g_b2Helper && WaitForSingleObject(g_b2Helper, 0) == WAIT_OBJECT_0)
                return false;
            if (g_b2FramePending) {
                if ((LONG)(GetTickCount() - deadline) >= 0) {
                    Log("ngx-b2: resize waited too long for frame ack v=%llu",
                        (unsigned long long)g_b2PendingValue);
                    return false;
                }
                Sleep(1);
                continue;
            }
            if ((LONG)(GetTickCount() - deadline) >= 0) {
                break;
            }
            break;
        }
    }

    auto dupInto = [](HANDLE nt, unsigned long long* outVal) -> bool {
        if (!nt) return false;
        HANDLE val = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), nt, g_b2Helper, &val, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return false;
        *outVal = (unsigned long long)(uintptr_t)val;
        return true;
    };
    static bool s_valsLogged = false;
    B2SetupMsg m = {};
    if (!dupInto(g_b2HColor, &m.hColor) || !dupInto(g_b2HOut, &m.hOut)) {
        Log("ngx-b2: DuplicateHandle(color/out) FAILED err=%lu", GetLastError());
        return false;
    }
    // Fences persist across resizes: duplicate ONCE, reuse values after.
    if (g_b2FInDup && g_b2FOutDup) {
        m.hFIn = g_b2FInDup; m.hFOut = g_b2FOutDup;
    } else if (!dupInto(g_b2HFIn, &m.hFIn) || !dupInto(g_b2HFOut, &m.hFOut)) {
        Log("ngx-b2: DuplicateHandle(fences) FAILED err=%lu", GetLastError());
        return false;
    } else { g_b2FInDup = m.hFIn; g_b2FOutDup = m.hFOut; }
    m.w = w; m.h = h; m.fmt = (unsigned)fmt;
    m.startVal = g_b2Val + 1; // next frame index helper should expect
    const char tagS = 0x53; // 'S'
    DWORD wa = 0, wb = 0, wr = 0, rd = 0;
    if (!WriteFile(g_b2Pipe, &tagS, 1, &wa, nullptr) ||
        !WriteFile(g_b2Pipe, &m, sizeof(m), &wb, nullptr)) return false;
    unsigned int resp = 0;
    // Never let a broken/dead helper block the render thread during a resize.
    // The helper acknowledges setup synchronously, so poll the byte count with
    // a bounded deadline before doing the final read.
    const DWORD deadline = GetTickCount() + 5000;
    bool responseReady = false;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_b2Pipe, nullptr, 0, nullptr, &avail, nullptr))
            break;
        if (avail >= sizeof(resp)) {
            responseReady = true;
            break;
        }
        if (g_b2Helper && WaitForSingleObject(g_b2Helper, 0) == WAIT_OBJECT_0)
            break;
        if ((LONG)(GetTickCount() - deadline) >= 0) {
            Log("ngx-b2: setup acknowledgement timeout");
            break;
        }
        Sleep(1);
    }
    if (!responseReady || !ReadFile(g_b2Pipe, &resp, sizeof(resp), &rd, nullptr) || resp != 0x59414B4F) {
        Log("ngx-b2: setup rejected by helper (%08X)", resp);
        return false;
    }
    Log("ngx-b2: helper owns NGX now (%ux%u)", w, h);
    return true;
}static bool EnsureNgxBridgeB2(UINT w, UINT h, DXGI_FORMAT fmt)
{
    B2CheckIniFlag(); // must precede local-fallback branching
    if (g_b2Ready && g_b2W == w && g_b2H == h && g_b2Fmt == fmt)
        return true;
    // THROTTLE before any logging - failed init retried every frame once (9k lines).
    {
        static DWORD s_lastFailMs = 0;
        DWORD nowMs = GetTickCount();
        if (!g_b2Dev && s_lastFailMs && (nowMs - s_lastFailMs) < 3000) return false;
        if (!g_b2Dev) s_lastFailMs = nowMs; // arm on first attempt of this burst
    }
    Log("ngx-b2: init %ux%u fmt=%u", w, h, (unsigned)fmt);
    if (!g_b2UseHelper && !g_b2Ready && !g_b2Dev) {
        // THROTTLE: failed init retried every frame spammed 9k lines once.
        static DWORD s_lastFailMs = 0;
        DWORD nowMs = GetTickCount();
        if (s_lastFailMs && (nowMs - s_lastFailMs) < 3000) return false;

        typedef HRESULT(WINAPI* PFN_DC)(IUnknown*, D3D_FEATURE_LEVEL, const IID&, void**);
        HMODULE d3dMod = GetModuleHandleA("d3d12.dll");
        PFN_DC mkDev = d3dMod ? (PFN_DC)GetProcAddress(d3dMod, "D3D12CreateDevice") : nullptr;
        if (!mkDev) { Log("ngx-b2: no D3D12CreateDevice export"); s_lastFailMs = nowMs; return false; }

        // PRIVATE d3d12 COPY: defeats module-instance/IAT wrappers that alias
        // every in-process device to the game's object.
        typedef HRESULT(WINAPI* PFN_CF1)(const IID&, void**);
        PFN_CF1 mkF = nullptr;
        {
            HMODULE dxgiMod = GetModuleHandleA("dxgi.dll");
            mkF = dxgiMod ? (PFN_CF1)GetProcAddress(dxgiMod, "CreateDXGIFactory1") : nullptr;
        }
        IDXGIFactory1* fac = nullptr;
        if (mkF) mkF(__uuidof(IDXGIFactory1), (void**)&fac);

        auto tryCreate = [&](PFN_DC dc, const char* tag) -> bool {
            if (!dc || !fac) return false;
            IDXGIAdapter1* ad = nullptr;
            for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d = {};
                if (SUCCEEDED(ad->GetDesc1(&d)) && d.VendorId == 0x10DE) {
                    HRESULT hr = dc(ad, D3D_FEATURE_LEVEL_11_0,
                                    __uuidof(ID3D12Device), (void**)&g_b2Dev);
                    Log("ngx-b2: [%s] NVIDIA create hr=0x%08X dev=%p",
                        tag, (unsigned)hr, (void*)g_b2Dev);
                    bool ok = SUCCEEDED(hr) && g_b2Dev && g_b2Dev != g_device;
                    ad->Release();
                    if (!ok) g_b2Dev = nullptr;
                    return ok;
                }
                ad->Release();
            }
            return false;
        };

        bool created = tryCreate(mkDev, "system");
        if (!created) {
            // private copy attempt
            static HMODULE s_priv = nullptr;
            if (!s_priv) {
                wchar_t src[MAX_PATH], dst[MAX_PATH];
                GetSystemDirectoryW(src, MAX_PATH); lstrcatW(src, L"\\d3d12.dll");
                GetTempPathW(MAX_PATH, dst); lstrcatW(dst, L"ScaleNG_priv_d3d12.dll");
                if (CopyFileW(src, dst, FALSE))
                    s_priv = LoadLibraryW(dst);
                Log("ngx-b2: private d3d12 copy -> %ls (%p)", dst, (void*)s_priv);
            }
            if (s_priv) {
                PFN_DC privDev = (PFN_DC)GetProcAddress(s_priv, "D3D12CreateDevice");
                created = tryCreate(privDev, "private");
            }
        }
        if (fac) fac->Release();
        if (!created) {
            Log("ngx-b2: NO independent device available"); s_lastFailMs = nowMs;
            return false;
        }
        IDXGIDevice* probe = nullptr;
        HRESULT qhr = g_b2Dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&probe);
        if (probe) probe->Release();
        if (FAILED(qhr)) {
            Log("ngx-b2: device STILL wrapper-blocked (QI hr=0x%08X)", (unsigned)qhr);
            g_b2Dev->Release(); g_b2Dev = nullptr; s_lastFailMs = nowMs;
            return false;
        }
        Log("ngx-b2: INDEPENDENT device %p", (void*)g_b2Dev);
        D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(g_b2Dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_b2Q))) ||
            FAILED(g_b2Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_b2Alloc))) ||
            FAILED(g_b2Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_b2Alloc, nullptr,
                                              IID_PPV_ARGS(&g_b2List)))) {
            Log("ngx-b2: own queue/list FAILED"); return false;
        }
        g_b2List->Close();
    }

    // shared texture pair on GAME device -> opened on ours
    if (g_b2HColor) { CloseHandle(g_b2HColor); g_b2HColor = nullptr; }
    if (g_b2HOut)   { CloseHandle(g_b2HOut);   g_b2HOut = nullptr; }
    // The source shared-fence handles belong to the persistent fences and must
    // survive resize/setup retries. Only the per-helper duplicates are reset
    // when the helper process is replaced (inside B2KillHelper).
    B2ReleasePair();
    {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Format = fmt;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                   D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_SHARED;
        if (FAILED(g_device->CreateCommittedResource(&hp, hf, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_b2ColorG))) ||
            FAILED(g_device->CreateCommittedResource(&hp, hf, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_b2OutG)))) {
            Log("ngx-b2: shared tex creation FAILED"); return false;
        }
        if (!g_b2UseHelper && (!B2OpenShared(g_b2Dev, g_b2ColorG, &g_b2ColorO) ||
            !B2OpenShared(g_b2Dev, g_b2OutG, &g_b2OutO))) {
            Log("ngx-b2: OpenSharedHandle FAILED"); return false;
        }
        // NT handles for cross-process duplication (helper mode)
        if (FAILED(g_device->CreateSharedHandle(g_b2ColorG, nullptr, GENERIC_ALL, nullptr, &g_b2HColor)) ||
            FAILED(g_device->CreateSharedHandle(g_b2OutG,   nullptr, GENERIC_ALL, nullptr, &g_b2HOut))) {
            Log("ngx-b2: CreateSharedHandle(color/out) FAILED"); return false;
        }
        Log("ngx-b2: created NT handles: color=%p out=%p", (void*)g_b2HColor, (void*)g_b2HOut);
        // shared fences: DIMENSION-INDEPENDENT -> create ONCE, never on resize.
        // Rebuilding desyncs the helper (it waits on the old object).
        if (!g_b2FenceInG) {
            ID3D12Fence *fiG=nullptr,*foG=nullptr,*fiO=nullptr,*foO=nullptr;
            bool ok =
                SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fiG))) &&
                SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&foG)));
            if (ok && !g_b2UseHelper)
                ok = B2OpenShared(g_b2Dev, fiG, &fiO) && B2OpenShared(g_b2Dev, foG, &foO);
            if (ok)
                ok = SUCCEEDED(g_device->CreateSharedHandle(fiG, nullptr, GENERIC_ALL, nullptr, &g_b2HFIn)) &&
                     SUCCEEDED(g_device->CreateSharedHandle(foG, nullptr, GENERIC_ALL, nullptr, &g_b2HFOut));
            if (!ok) {
                Log("ngx-b2: shared fence FAILED");
                if(fiG)fiG->Release(); if(foG)foG->Release(); if(fiO)fiO->Release(); if(foO)foO->Release();
                return false;
            }
            g_b2FenceInG = fiG; g_b2FenceOutG = foG; g_b2FIO = fiO; g_b2FOO = foO;
            Log("ngx-b2: fences created once (persistent across resizes)");
        }
    }

    if (g_b2UseHelper) {
        // CROSS-PROCESS MODE: helper owns device + NGX entirely.
        if (!B2SendSetup(w, h, fmt)) {
            // A setup failure can be transient (stale helper, deployment while
            // the game is still running, or a helper startup race). Tear down
            // the failed instance and keep helper mode enabled so the existing
            // retry throttle can start a clean worker on a later frame. The old
            // behavior permanently forced the unavailable local-device fallback
            // after one failed attempt, making recovery impossible in-session.
            Log("ngx-b2: helper setup FAILED - restarting helper on a later frame");
            B2KillHelper();
            return false;
        }
        g_b2W = w; g_b2H = h; g_b2Fmt = fmt;
        g_b2Ready = true;
        Log("ngx-b2: READY via HELPER");
        return true;
    }
    // LOCAL-MODE fallback (helper unavailable): NGX on our own clean device.
    if (!g_upscaler) g_upscaler = CreateUpscaler(UPSCALER_DLSS);    // NGX upscaler on OUR device
    if (!g_upscaler) g_upscaler = CreateUpscaler(UPSCALER_DLSS);
    if (!g_upscaler) { Log("ngx-b2: upscaler create FAILED"); return false; }
    UpscalerInitParams ip = {};
    ip.device = g_b2Dev;
    ip.renderWidth = w;  ip.renderHeight = h;
    ip.displayWidth = w; ip.displayHeight = h;
    ip.appId = g_cfg.appId;
    ip.ngxApiVersion = g_cfg.ngxApiVersion;
    ip.perfQuality = g_cfg.perfQuality;
    ip.mvJittered = g_cfg.mvJittered != 0;
    ip.autoExposure = g_cfg.autoExposure != 0;
    ip.depthInverted = g_cfg.depthInverted;
    {
        static wchar_t dllPath[MAX_PATH];
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&EnsureNgxBridgeB2, &self);
        wchar_t selfPath[MAX_PATH] = {};
        if (self) GetModuleFileNameW(self, selfPath, MAX_PATH);
        wchar_t* slash = wcsrchr(selfPath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        lstrcpyW(dllPath, selfPath); lstrcatW(dllPath, L"nvngx_dlss.dll");
        ip.dlssDllPath = dllPath;
    }
    if (!g_upscaler->Init(ip) || !g_upscaler->IsReady()) {
        Log("ngx-b2: NGX Init on own device FAILED"); return false;
    }
    g_upscaler->UpdateSizes(w, h, w, h);

    g_b2W = w; g_b2H = h; g_b2Fmt = fmt;
    g_b2Ready = true;
    Log("ngx-b2: READY (own=%p game=%p)", (void*)g_b2Dev, (void*)g_device);
    return true;
}

static void NgxBridgeFrameB2(ID3D12Resource* bb, UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (!TryAcquireSRWLockExclusive(&g_b2FrameLock)) {
        static unsigned s_busySkips = 0;
        if ((++s_busySkips % 120) == 1)
            Log("ngx-b2: frame skipped - bridge transaction already active");
        return;
    }
    struct FrameUnlock { SRWLOCK* lock; ~FrameUnlock() { ReleaseSRWLockExclusive(lock); } } frameUnlock{ &g_b2FrameLock };

    if (!EnsureNgxBridgeB2(w, h, fmt)) return;
    if (!g_b2UseHelper) B2EnsureDummyInputs(w, h);

    ++g_b2Val;
    UINT64 v = g_b2Val;

    static ID3D12CommandAllocator*    s_alA = nullptr; static ID3D12GraphicsCommandList* s_clA = nullptr;
    static ID3D12CommandQueue*        s_gq  = nullptr;
    static bool        s_gqTried = false;
    if (!s_gq && !s_gqTried) {
        s_gqTried = true;
        // PREFER the game's own direct queue: same-queue submission makes our
        // backbuffer transitions ordered against the game's rendering (a
        // private queue racing the game on the same bb = device removal).
        if (g_graphicsQueue) {
            s_gq = g_graphicsQueue;
            Log("ngx-b2: stages on GAME queue %p", (void*)s_gq);
        } else {
            D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&s_gq)))) return;
            Log("ngx-b2: WARNING game queue unavailable - private queue (race risk)");
        }
    }
    if (!s_gq) return;
    // Allocators/lists are queue-agnostic; create once.
    static bool s_listsTried = false;
    if (!s_listsTried) {
        s_listsTried = true;
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alA))) ||
            FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alA, nullptr, IID_PPV_ARGS(&s_clA))) ||
            FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_b2CopyAlloc))) ||
            FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_b2CopyAlloc, nullptr, IID_PPV_ARGS(&g_b2CopyList)))) {
            Log("ngx-b2: alloc/list creation FAILED");
            s_gq = nullptr;
            return;
        }
        s_clA->Close(); g_b2CopyList->Close();
    }

    // STAGE 1 (game): bb -> sharedColor ; signal fIn
    s_clA->Reset(s_alA, nullptr);
    D3D12_RESOURCE_BARRIER b1 = {};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = bb;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_clA->ResourceBarrier(1, &b1);
    D3D12_TEXTURE_COPY_LOCATION d1 = { g_b2ColorG, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION s1 = { bb,         D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    s_clA->CopyTextureRegion(&d1, 0, 0, 0, &s1, nullptr);
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b1.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    s_clA->ResourceBarrier(1, &b1);
    s_clA->Close();
    ID3D12CommandList* l1[] = { s_clA };
    s_gq->ExecuteCommandLists(1, l1);

    // STAGE 2 (protocol v2): tell helper this frame is ready; it evaluates and
    // signals fOut, then acks. We never read fence values cross-process.
    bool helperAcked = false;
    if (g_b2UseHelper && g_b2Pipe) {
        AcquireSRWLockExclusive(&g_b2PipeLock);
        struct PipeUnlock { SRWLOCK* lock; ~PipeUnlock() { ReleaseSRWLockExclusive(lock); } } pipeUnlock{ &g_b2PipeLock };
        static unsigned s_wDiag = 0;
        B2FrameAck ack = {};
        DWORD rd = 0, have = 0;

        // Drain the one acknowledgement that may already be queued. The
        // helper can take longer than one frame during NGX initialization, so
        // this must remain non-blocking.
        for (int spin = 0; spin < 2 && have < sizeof(ack); ++spin) {
            if (!PeekNamedPipe(g_b2Pipe, nullptr, 0, nullptr, &rd, nullptr)) break;
            if (rd) {
                DWORD want = rd;
                if (want > sizeof(ack) - have) want = sizeof(ack) - have;
                if (!ReadFile(g_b2Pipe, ((BYTE*)&ack) + have, want, &rd, nullptr)) break;
                have += rd;
                if (have >= sizeof(ack)) break;
            }
        }
if (have == sizeof(ack) && ack.value == g_b2PendingValue) {
            g_b2FramePending = false;
            // The ACK is for the pending frame (g_b2PendingValue). Its recorded
            // status tells us whether NGX produced a valid result for that frame.
            // Do NOT compare with current frame v - the ACK arrives one frame late.
            helperAcked = (ack.recorded != 0);
            if (ack.recorded && g_b2DeferredOutput) {
                g_b2DeferredVal = ack.value;
                InterlockedExchange(&g_b2DeferredPending, 1);
                static unsigned s_deferredAcks = 0;
                if (++s_deferredAcks <= 5 || (s_deferredAcks % 600) == 0)
                    Log("ngx-b2: helper acknowledged v=%llu; deferred output armed",
                        (unsigned long long)ack.value);
                if (s_deferredAcks <= 5 || (s_deferredAcks % 600) == 0)
                    LogNativeCorrelation("helper-ack", (unsigned long long)ack.value);
            }
            if (!ack.recorded) {
                Log("ngx-b2: helper acknowledged v=%llu but NGX rejected the frame", (unsigned long long)ack.value);
                static unsigned s_rejectCorrelation = 0;
                if (++s_rejectCorrelation <= 3 || (s_rejectCorrelation % 600) == 0)
                    LogNativeCorrelation("helper-reject", (unsigned long long)ack.value);
            }
        } else if (have == sizeof(ack) && ack.value != g_b2PendingValue) {
            Log("ngx-b2: helper acknowledgement out of sequence (got=%llu expected=%llu)",
                (unsigned long long)ack.value, (unsigned long long)g_b2PendingValue);
        }

        // Apply backpressure. Without this guard the ASI can enqueue thousands
        // of frame messages while NGX is still evaluating the first one. A
        // later resize setup then sits behind that backlog and appears to
        // freeze the game while waiting for its setup acknowledgement.
        if (!g_b2FramePending) {
            const char tagF = 0x46; // 'F'
            DWORD wa = 0, wb = 0;
            bool wrOK = WriteFile(g_b2Pipe, &tagF, 1, &wa, nullptr) &&
                        WriteFile(g_b2Pipe, &v, sizeof(v), &wb, nullptr);
            if (++s_wDiag <= 3)
                Log("ngx-b2: frame msg write v=%llu ok=%d err=%lu",
                    (unsigned long long)v, (int)wrOK, wrOK ? 0UL : GetLastError());
            if (!wrOK) {
                Log("ngx-b2: helper pipe write failed at v=%llu", (unsigned long long)v);
                B2KillHelper();
                g_b2Ready = false;
            } else {
                g_b2FramePending = true;
                g_b2PendingValue = v;
            }
        } else {
            static unsigned s_ackWaitDiag = 0;
            if ((++s_ackWaitDiag % 120) == 1)
                Log("ngx-b2: helper busy at v=%llu; frame skipped (pending=%llu)",
                    (unsigned long long)v, (unsigned long long)g_b2PendingValue);
        }

    }

    // STAGE 3 (game): only blit when helper CONFIRMED fOut signal enqueue.
    // The helper does not return a command-list recording result to this
    // process; its acknowledgement means it enqueued the fence signal after
    // its NGX attempt.  Treat that acknowledgement as the successful stage-2
    // result so stage 3 can wait for and copy the helper's output.
    bool recorded = helperAcked;
    if (!g_b2UseHelper) {
        // LOCAL-MODE fallback: evaluate on our own clean device.
        if (g_b2Q && SUCCEEDED(g_b2List->Reset(g_b2Alloc, nullptr))) {
            UpscalerEvaluateParams ep = {};
            ep.commandList = g_b2List;
            ep.color = g_b2ColorO;
            ep.depth = g_b2Depth;
            ep.motionVectors = g_b2Mv;
            ep.output = g_b2OutO;
            ep.jitterX = 0; ep.jitterY = 0;
            ep.mvScaleX = (float)w; ep.mvScaleY = (float)h;
            ep.sharpness = g_cfg.sharpness;
            recorded = g_upscaler->Evaluate(ep);
            if (recorded && SUCCEEDED(g_b2List->Close())) {
                ID3D12CommandList* l2[] = { g_b2List };
                g_b2Q->ExecuteCommandLists(1, l2);
                g_b2Q->Signal(g_b2FOO, v);
            } else recorded = false;
        }
    }

if (!recorded) {
        // Helper didn't ack for the previous frame yet. In deferred output
        // mode this is normal - the previous frame's deferred output was
        // already armed when its ACK arrived. Skip stage-3 copy (not needed
        // in deferred mode) and let the engine copy hook consume the output.
        if (g_b2DeferredOutput && g_b2DeferredPending) {
            static unsigned s_deferredFrames = 0;
            if ((++s_deferredFrames % 120) == 1)
                Log("ngx-b2: frame %llu deferred (pending=%llu)",
                    (unsigned long long)v, (unsigned long long)g_b2DeferredVal);
        } else {
            static unsigned s_skips = 0;
            if ((++s_skips % 120) == 1)
                Log("ngx-b2: frame %llu skipped (no ack/eval) x%u",
                    (unsigned long long)v, s_skips);
        }
        bb->Release();
        return;
    }
    // The helper acknowledgement only proves that its Signal was enqueued.
    // Calling ID3D12CommandQueue::Wait on the game's queue from inside its
    // Present callback is unsafe on this renderer and caused an immediate
    // black-window crash after the first successful NGX frame.  Complete the
    // shared fence on the CPU first, then submit the copy with no cross-queue
    // GPU wait while Present is active.
    static HANDLE s_outEvent = nullptr;
    if (!s_outEvent) s_outEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_outEvent || FAILED(g_b2FenceOutG->SetEventOnCompletion(v, s_outEvent)) ||
        WaitForSingleObject(s_outEvent, 5000) != WAIT_OBJECT_0) {
        static unsigned s_waitFails = 0;
        if ((++s_waitFails % 60) == 1)
            Log("ngx-b2: frame %llu skipped (helper fence wait failed)", (unsigned long long)v);
        bb->Release();
        return;
    }
    // Evaluation is useful for validating the helper/NGX path, but copying
    // the cross-process output into BeamNG's wrapped backbuffer is currently
    // the only operation that still causes the game to terminate. Keep this
    // gate configurable so the helper can run in evaluation-only mode while
    // the game remains on its native presentation path.
if (!g_b2ReplaceOutput) {
        if (g_b2DeferredOutput && recorded) {
            // Deferred output was already armed when the previous frame's ACK
            // arrived. Do not re-arm here with the current frame value.
            static unsigned s_deferredArmedLog = 0;
            if ((++s_deferredArmedLog % 600) == 1)
                Log("ngx-b2: frame %llu eval=ok (deferred output already pending=%llu) queue=%p presentBb=%p fenceDone=%llu",
                    (unsigned long long)v, (unsigned long long)g_b2DeferredVal,
                (void*)g_graphicsQueue, (void*)g_b2PresentBb,
                    (unsigned long long)(g_b2FenceOutG ? g_b2FenceOutG->GetCompletedValue() : 0));
        }
        static unsigned s_evalOnly = 0;
        if (!g_b2DeferredOutput && (++s_evalOnly <= 5 || (s_evalOnly % 600) == 0))
            Log("ngx-b2: frame %llu eval=ok (output replacement disabled)",
                (unsigned long long)v);
        bb->Release();
        return;
    }
    g_b2CopyList->Reset(g_b2CopyAlloc, nullptr);
    D3D12_RESOURCE_BARRIER b3 = {};
    b3.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b3.Transition.pResource = bb;
    b3.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b3.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b3.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_b2CopyList->ResourceBarrier(1, &b3);
    D3D12_TEXTURE_COPY_LOCATION d3 = { bb,       D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION s3 = { g_b2OutG, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    g_b2CopyList->CopyTextureRegion(&d3, 0, 0, 0, &s3, nullptr);
    b3.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b3.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_b2CopyList->ResourceBarrier(1, &b3);
    g_b2CopyList->Close();
    ID3D12CommandList* l3[] = { g_b2CopyList };
    s_gq->ExecuteCommandLists(1, l3);

    static unsigned s_brFrames = 0;
    if (++s_brFrames <= 5 || (s_brFrames % 600) == 0)
        Log("ngx-b2: frame %u eval=%s", s_brFrames, recorded ? "ok" : "SKIP");
    bb->Release();
}
void TryQueueOutputCopy(ID3D12CommandQueue* queue)
{
    if (!g_b2QueueCopy || !queue || queue != g_graphicsQueue ||
        !g_b2Ready || !g_b2DeferredOutput || !g_b2PresentBb ||
        !g_b2OutG || !g_b2FenceOutG || !g_b2CopyAlloc || !g_b2CopyList)
        return;

    // Never touch the swapchain during render-graph construction. The first
    // queue-copy attempt happened during startup and crashed immediately after
    // the first Present. Require a warmed-up Present stream before arming.
    const unsigned long long presentSerial =
        (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    if (presentSerial < 300) {
        static unsigned s_warmupLog = 0;
        if (++s_warmupLog == 1)
            Log("ngx-b2: queue copy held for Present warmup (serial=%llu)", presentSerial);
        return;
    }

    const UINT64 pending = g_b2DeferredVal;
    if (!pending)
        return;
    if (g_device && g_device->GetDeviceRemovedReason() != S_OK) {
        Log("ngx-b2: queue copy disabled - game device removed");
        g_b2QueueCopy = false;
        return;
    }
    const UINT64 completed = g_b2FenceOutG->GetCompletedValue();
    if (completed == UINT64_MAX) {
        Log("ngx-b2: queue copy disabled - invalid output fence completion UINT64_MAX");
        g_b2QueueCopy = false;
        return;
    }
    if (completed < pending)
        return;
    if (InterlockedCompareExchange(&g_b2DeferredPending, 0, 1) != 1)
        return;

    D3D12_RESOURCE_DESC outDesc = g_b2OutG->GetDesc();
    D3D12_RESOURCE_DESC bbDesc = g_b2PresentBb->GetDesc();
    if (outDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        bbDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        outDesc.Width != bbDesc.Width || outDesc.Height != bbDesc.Height ||
        outDesc.Format != bbDesc.Format) {
        Log("ngx-b2: queue copy rejected shape out=%llux%u fmt=%u bb=%llux%u fmt=%u present=%llu",
            (unsigned long long)outDesc.Width, (unsigned)outDesc.Height,
            (unsigned)outDesc.Format, (unsigned long long)bbDesc.Width,
            (unsigned)bbDesc.Height, (unsigned)bbDesc.Format, presentSerial);
        InterlockedExchange(&g_b2DeferredPending, 1);
        return;
    }

    HRESULT whr = queue->Wait(g_b2FenceOutG, pending);
    HRESULT rhr = whr;
    if (SUCCEEDED(whr)) rhr = g_b2CopyAlloc->Reset();
    if (SUCCEEDED(rhr)) rhr = g_b2CopyList->Reset(g_b2CopyAlloc, nullptr);
    if (SUCCEEDED(rhr)) {
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (int i = 0; i < 2; ++i) {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        b[0].Transition.pResource = g_b2OutG;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Transition.pResource = g_b2PresentBb;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_b2CopyList->ResourceBarrier(2, b);
        D3D12_TEXTURE_COPY_LOCATION dst = { g_b2PresentBb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION src = { g_b2OutG, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        g_b2CopyList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_b2CopyList->ResourceBarrier(2, b);
        rhr = g_b2CopyList->Close();
    }
    if (SUCCEEDED(rhr)) {
        ID3D12CommandList* lists[] = { g_b2CopyList };
        queue->ExecuteCommandLists(1, lists);
        static unsigned s_queueCopies = 0;
        if (++s_queueCopies <= 5 || (s_queueCopies % 600) == 0)
            Log("ngx-b2: QUEUE COPY submitted #%u frame=%u fence=%llu queue=%p bb=%p",
                s_queueCopies, g_frameCounter, (unsigned long long)pending,
                (void*)queue, (void*)g_b2PresentBb);
    } else {
        Log("ngx-b2: queue copy failed hr=0x%08X wait=0x%08X fence=%llu",
            (unsigned)rhr, (unsigned)whr, (unsigned long long)pending);
        InterlockedExchange(&g_b2DeferredPending, 1);
    }
}

static void NgxSelfContainedPipeline(IDXGISwapChain* sc, ID3D12GraphicsCommandList* cmdList,
                                      ID3D12CommandQueue* queue)
{
    if (!g_swapchain) return;

    static int s_enterLogs = 0;
    if (++s_enterLogs <= 3 || (s_enterLogs % 600) == 0) Log("ngx-pipe: enter #%d", s_enterLogs);

    // Get backbuffer - GUARDED: sc may be a wrapped/garbage pointer
    ID3D12Resource* bb = nullptr;
    __try {
        if (FAILED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) {
            Log("ngx-pipe: GetBuffer FAILED");
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ngx-pipe: GetBuffer AV");
        return;
    }
    static int s_bbLogs = 0; if (++s_bbLogs <= 3) Log("ngx-pipe: bb=%p", (void*)bb);
    g_b2PresentBb = bb; // weak identity used by deferred output handoff
    {
        static unsigned s_b2BbDiag = 0;
        if (++s_b2BbDiag <= 5 || (s_b2BbDiag % 600) == 0) {
            D3D12_RESOURCE_DESC bd = bb->GetDesc();
            Log("ngx-b2: Present backbuffer #%u bb=%p size=%ux%u fmt=%u frame=%u queue=%p ready=%d pending=%d",
                s_b2BbDiag, (void*)bb, (unsigned)bd.Width, (unsigned)bd.Height,
                (unsigned)bd.Format, g_frameCounter, (void*)g_graphicsQueue,
                g_b2Ready ? 1 : 0, (int)g_b2DeferredPending);
            LogNativeCorrelation("present",
                (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0));
        }
    }

    D3D12_RESOURCE_DESC bbd = {};
    __try {
        bbd = bb->GetDesc();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ngx-pipe: bb GetDesc AV");
        bb->Release();
        return;
    }
    UINT w = (UINT)bbd.Width, h = (UINT)bbd.Height;
    if (w < 256 || h < 128) { bb->Release(); return; } // EGSH dummy / tiny targets
    static int s_dimLogs = 0; if (++s_dimLogs <= 3) Log("ngx-pipe: bb %ux%u fmt=%u", w, h, (unsigned)bbd.Format);

    // Lazy-create everything on first valid frame
    if (!g_ngxPipelineReady) {
        // Get device from swapchain (always works - raw COM call)
        if (!g_device) {
            Log("ngx-pipe: getting device from swapchain...");
            IDXGIDevice* dxgidev = nullptr;
            __try {
                if (SUCCEEDED(g_swapchain->GetDevice(__uuidof(IDXGIDevice), (void**)&dxgidev))) {
                    dxgidev->QueryInterface(__uuidof(ID3D12Device), (void**)&g_device);
                    dxgidev->Release();
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
            if (!g_device) {
                Log("ngx-pipe: FAILED to get device from swapchain");
                bb->Release();
                return;
            }
        }
        static int s_devLogs = 0; if (++s_devLogs <= 3) Log("ngx-pipe: device=%p", (void*)g_device);

        // GAME-WRAPPER TRIPWIRE -> BRIDGE DISPATCH:
        // BeamNG's device wrapper fails IDXGIDevice QI; NGX dispatch recording
        // on this wrapped device hangs/crashes (proven twice). When detected,
        // run the fence-ordered shared-handle bridge: NGX on OUR clean device,
        // copies-only on the game device.
        if (!g_ngxBlocked) {
            IDXGIDevice* probe = nullptr;
            HRESULT qhr = g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&probe);
            if (probe) probe->Release();
            if (FAILED(qhr)) {
                g_ngxBlocked = true;
                Log("ngx-pipe: game-wrapped device (QI hr=0x%08X) - enabling shared-handle bridge", (unsigned)qhr);
            }
        }
        if (g_ngxBlocked) {
            NgxBridgeFrameB2(bb, w, h, bbd.Format);
            bb->Release();
            return;
        }

        if (!g_upscaler) {
            EnsureUpscalerInit(true /*Present pipeline: bypass legacy quiet gate*/);
            if (!g_upscaler || !g_upscaler->IsReady()) { bb->Release(); return; }
        }

        CreateNgxTextures(g_device, w, h, bbd.Format);
        // Sync feature dims to real backbuffer (smoke test may have left a
        // 512x512 feature behind). DestroyFeature happens inside; next
        // Evaluate re-creates at these sizes.
        g_upscaler->UpdateSizes(w, h, w, h);
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_ngxAlloc));
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ngxAlloc, nullptr,
                                    IID_PPV_ARGS(&g_ngxList));
        g_ngxList->Close();

        // Our OWN command queue on the game's device
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_ngxQueue));

        g_ngxPipelineReady = true;
        Log("ngx-pipe: pipeline ready (dev=%p queue=%p)", (void*)g_device, (void*)g_ngxQueue);
    }

    if (!g_ngxColor || !g_ngxDepth || !g_ngxMv || !g_ngxOut) { bb->Release(); return; }

    // Resolution change (menu -> game -> resize): recreate textures + feature.
    // Allocator/list/queue are size-agnostic - only textures and the NGX
    // feature need refreshing.
    {
        static UINT s_ngxW = 0, s_ngxH = 0;
        if (s_ngxW != w || s_ngxH != h) {
            Log("ngx-pipe: size change %ux%u -> %ux%u", s_ngxW, s_ngxH, w, h);
            s_ngxW = w; s_ngxH = h;
            if (g_device && g_upscaler && g_upscaler->IsReady()) {
                CreateNgxTextures(g_device, w, h, bbd.Format);
                g_upscaler->UpdateSizes(w, h, w, h);
            }
        }
    }

    ++g_ngxFrameCount;
    if (g_ngxFrameCount < 30 || (g_ngxFrameCount % 300) == 0)
        Log("ngx-pipe: frame %u evaluating", g_ngxFrameCount);

    // Reset and record NGX work
    g_ngxList->Reset(g_ngxAlloc, nullptr);

    // All NGX textures live in COMMON at frame boundaries (creation + restore).
    D3D12_RESOURCE_BARRIER bars[4] = {};
    for (int i = 0; i < 4; ++i) { bars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
    bars[0].Transition.pResource = g_ngxColor;
    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.pResource = g_ngxDepth;
    bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bars[2].Transition.pResource = g_ngxMv;
    bars[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bars[2].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bars[3].Transition.pResource = g_ngxOut;
    bars[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bars[3].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_ngxList->ResourceBarrier(4, bars);

    // Backbuffer PRESENT -> COPY_SOURCE, copy into color input
    D3D12_RESOURCE_BARRIER bar = {};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = bb;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_ngxList->ResourceBarrier(1, &bar);

    D3D12_TEXTURE_COPY_LOCATION cdst = {}; cdst.pResource = g_ngxColor; cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION csrc = {}; csrc.pResource = bb; csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_ngxList->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, nullptr);

    // color COPY_DEST -> SRV for NGX read
    D3D12_RESOURCE_BARRIER cbar = {};
    cbar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    cbar.Transition.pResource = g_ngxColor;
    cbar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    cbar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cbar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_ngxList->ResourceBarrier(1, &cbar);

    // Restore backbuffer to PRESENT before evaluate
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_ngxList->ResourceBarrier(1, &bar);

    // NGX Evaluate (descriptor heap bound internally by dlss_ngx.cpp)
    UpscalerEvaluateParams ep = {};
    ep.commandList = g_ngxList;
    ep.color = g_ngxColor;
    ep.depth = g_ngxDepth;
    ep.motionVectors = g_ngxMv;
    ep.output = g_ngxOut;
    ep.jitterX = 0.0f; ep.jitterY = 0.0f;
    ep.mvScaleX = (float)w; ep.mvScaleY = (float)h;
    ep.sharpness = g_cfg.sharpness;
    bool ok = g_upscaler->Evaluate(ep);
    if (!ok) {
        // NEVER submit a list NGX recorded garbage into (proven device-killer).
        // Nothing executed -> all textures still COMMON -> next frame is clean.
        if (g_ngxFrameCount <= 30 || (g_ngxFrameCount % 300) == 0)
            Log("ngx-pipe: Evaluate FAILED frame %u - discarding cmd list", g_ngxFrameCount);
        bb->Release();
        return;
    }

    // out UAV -> COPY_SOURCE; backbuffer PRESENT -> COPY_DEST; copy result up
    D3D12_RESOURCE_BARRIER ob[2] = {};
    for (int i = 0; i < 2; ++i) { ob[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; ob[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
    ob[0].Transition.pResource = g_ngxOut;
    ob[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ob[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ob[1].Transition.pResource = bb;
    ob[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    ob[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_ngxList->ResourceBarrier(2, ob);

    D3D12_TEXTURE_COPY_LOCATION odst = {}; odst.pResource = bb; odst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION osrc = {}; osrc.pResource = g_ngxOut; osrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_ngxList->CopyTextureRegion(&odst, 0, 0, 0, &osrc, nullptr);

    // Restore EVERYTHING for next frame: out/color/depth/mv -> COMMON,
    // backbuffer COPY_DEST -> PRESENT.
    D3D12_RESOURCE_BARRIER rs[5] = {};
    for (int i = 0; i < 5; ++i) { rs[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; rs[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
    rs[0].Transition.pResource = g_ngxOut;
    rs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    rs[1].Transition.pResource = g_ngxColor;
    rs[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rs[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    rs[2].Transition.pResource = g_ngxDepth;
    rs[2].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rs[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    rs[3].Transition.pResource = g_ngxMv;
    rs[3].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rs[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    rs[4].Transition.pResource = bb;
    rs[4].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    rs[4].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_ngxList->ResourceBarrier(5, rs);

    g_ngxList->Close();

    ID3D12CommandList* cls[] = { g_ngxList };
    g_ngxQueue->ExecuteCommandLists(1, cls);

    bb->Release();
}

// ============================================================================

void InjectAtPresentImpl(ID3D12CommandQueue* injQueue)
{
    if (injQueue) g_graphicsQueue = injQueue;

    g_injStep = "gate";
    if (g_passiveMode) return;

    // SELF-CONTAINED NGX PIPELINE: runs UNCONDITIONALLY.
    // Gets everything from swapchain internally. No other hooks needed.
    if (g_dlaaMode && g_swapchain && !g_ngxPipelineReady) {
        static int s_firstPresLogs = 0;
        if (++s_firstPresLogs <= 3)
            Log("ngx-pipe: first Present - starting self-contained pipeline");
    }
    if (g_dlaaMode && g_swapchain) {
        __try {
            NgxSelfContainedPipeline(g_swapchain, nullptr, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static int s_ngxStorm = 0;
            if (++s_ngxStorm <= 5)
                Log("ngx-pipe: guarded fault");
        }
        return; // self-contained pipeline handles everything
    }
    unsigned int fc = g_frameCounter;
    bool gameplayActive = (g_lastCamPatchFrame != 0 &&
        fc >= g_lastCamPatchFrame && (fc - g_lastCamPatchFrame) < 120) ||
        (g_sceneColorValid && g_displayW > 0);
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

    // SELF-CONTAINED NGX PIPELINE: bypasses ALL legacy architecture.
    // No bridge, no shared resources, no cross-device anything.
    // Gets device + queue from swapchain internally. Only needs g_swapchain.
    if (g_dlaaMode && g_swapchain) {
        __try {
            NgxSelfContainedPipeline(g_swapchain, nullptr, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static int s_ngxStorm = 0;
            if (++s_ngxStorm <= 5)
                Log("ngx-pipe: guarded fault at frame %u", fc);
        }
        return; // self-contained pipeline handles everything
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
    // Nothing to fetch from and nothing cached -> nothing to do here.
    if (!g_bbCached && !g_swapchain && !g_sceneColorValid) return;
    // POLITE PATH: use RTV-captured backbuffer when available - never probe
    // the engine's guarded wrapper via GetBuffer (software-AV storm class).
    bool usedCachedBb = false;
    if (g_bbCached) {
        bb = g_bbCached;
        usedCachedBb = true;
        g_injStep = "bb-cached";
        // Guarded GetDesc: cached resource may be freed by the engine before
        // we use it. On fault, drop the cache and fall through to scene-based
        // display sizing instead of blocking the pipeline.
        __try {
            D3D12_RESOURCE_DESC bbd = bb->GetDesc();
            if ((unsigned int)bbd.Width >= 1000 && bbd.Height >= 700 &&
                ((unsigned int)bbd.Width != g_displayW || (unsigned int)bbd.Height != g_displayH)) {
                g_displayW = (unsigned int)bbd.Width;
                g_displayH = (unsigned int)bbd.Height;
                Log("hooks: display committed from cached backbuffer %ux%u", g_displayW, g_displayH);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: bb-cached faulted (freed) - clearing cache");
            g_bbCached = nullptr;
            bb = nullptr;
            usedCachedBb = false;
        }
    }
    if (!usedCachedBb) {
    // Blacklist: a swapchain that faulted repeatedly is skipped BY IDENTITY.
    // Nulling g_swapchain alone just re-adopts the same poisoned object.
    {
        void* cur = (void*)g_swapchain;
        for (int bi = 0; bi < 4; ++bi) {
            extern void* g_badSc[4]; // defined below near g_bbFetchFails
            if (g_badSc[bi] && g_badSc[bi] == cur) return;
        }
    }
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
        // Dead-swapchain circuit breaker: three consecutive faults means the
        // cached g_swapchain is stale (engine re-init/rotation). Null it so
        // Hook_Present's self-heal re-adopts the REAL swapchain instead of
        // retrying into the same AV every frame (freeze class).
        if (InterlockedIncrement(&g_bbFetchFails) >= 3) {
            // Blacklist THIS object so self-heal doesn't re-adopt poison.
            void* bad = (void*)g_swapchain;
            g_swapchain = nullptr;
            g_bbFetchFails = 0;
            if (bad) {
                for (int bi = 0; bi < 4; ++bi) {
                    if (g_badSc[bi] == bad) break;
                    if (!g_badSc[bi]) { InterlockedExchangePointer(&g_badSc[bi], bad); break; }
                }
                Log("hooks: swapchain %p blacklisted after repeated fetch faults - self-heal armed", bad);
            }
        }
    }
    g_injStep = "bb-fetched";
    // FALLBACK: commit display from scene color when no backbuffer available.
    // Then build bridge BEFORE the bb check - NGX on bridge device doesn't
    // need the game's backbuffer, only display dims + shared textures.
    if (!bb && g_sceneColorValid && g_sceneColor) {
        __try {
            D3D12_RESOURCE_DESC scd = g_sceneColor->GetDesc();
            if ((unsigned int)scd.Width >= 1000 && scd.Height >= 700 &&
                g_displayW == 0) {
                g_displayW = (unsigned int)scd.Width;
                g_displayH = (unsigned int)scd.Height;
                Log("hooks: display committed from scene color %ux%u", g_displayW, g_displayH);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }

    // SINGLE-DEVICE: bridge creation DISABLED - NGX runs on game's device
    // if (g_dlaaMode && g_displayW > 0 && g_bbFormat != DXGI_FORMAT_UNKNOWN) {
    //     EnsureBridge(g_displayW, g_displayH, g_bbFormat, g_device);
    // }

    if (!bb) return; // no backbuffer = skip injection, but bridge may now exist

    // The backbuffer IS the true display surface: adopt its dimensions so the
    // DLAA feature (render==display) always matches what we feed it. Without
    // this, a stale loading-screen adoption (e.g. 1902x954) leaves the feature
    // smaller than the 1920x1001 backbuffer -> EvaluateFeature 0xBAD00005.
    D3D12_RESOURCE_DESC bbd = bb->GetDesc();
    if ((unsigned int)bbd.Width != g_displayW || (unsigned int)bbd.Height != g_displayH)
        AdoptDisplaySize((unsigned int)bbd.Width, (unsigned int)bbd.Height);
    } // end legacy probe (only when nothing cached)

    // STEP MARKERS: the repeating fault at 'adopted' jumps into non-module
    // memory (MinHook trampoline pool of another copy?). These name the exact
    // call that transfers control there.
    static int s_stepLogs = 0;
    bool doStepLog = s_stepLogs < 15;
    if (doStepLog) { ++s_stepLogs; Log("step: adopted bb=%p display=%ux%u fmt=%u", (void*)bb, g_displayW, g_displayH, g_bbFormat); }

    g_injStep = "adopted";
    // Guarded single lookup (value copy; no iterator escapes). bb's own entry
    // state is unused downstream - only tracked/untracked matters here.
    D3D12_RESOURCE_STATES bbProbe = D3D12_RESOURCE_STATE_COMMON;
    if (!FindTrackedState(bb, &bbProbe)) {
        Log("hooks: backbuffer %p untracked - present injection skipped", (void*)bb);
        bb->Release();
        return;
    }

    if (g_bbFormat == DXGI_FORMAT_UNKNOWN)
        g_bbFormat = bb->GetDesc().Format;

    // SESSION SETTLE LATCH - computed every present from cheap counters.
    // Scene identity unchanged 90f + quarantine expired. Until latched, ALL
    // heavy work (bridge/NGX/PSOs/heaps) stays deferred so the load window
    // stays passive-mode-light (protection against the teardown coin-flip).
    if (!g_settledOnce &&
        (int)(g_frameCounter - g_lastSceneChangeFrame) > 90 &&
        (int)(g_frameCounter - g_quietUntilFrame) >= 0 &&
        InterlockedCompareExchange(&g_settledOnce, 1, 0) == 0) {
        Log("hooks: render graph settled - DLAA armed for session");
    }

    // Bridge device + shared textures: SAFE during load (every stable run
    // SINGLE-DEVICE: bridge creation DISABLED
    // if (g_dlaaMode) {
    //     if (doStepLog) Log("step: calling EnsureBridge");
    //     if (!EnsureBridge(g_displayW, g_displayH, g_bbFormat, g_device)) {
    //         static int s_brFail = 0;
    //         if (++s_brFail <= 3) Log("hooks: bridge unavailable - DLAA disabled this session");
    //     }
    // }
    // NGX runtime load + upscaler + injection PSOs/heaps: DEFERRED to settle
    // (these were the load-window perturbations worth avoiding).
    if (g_dlaaMode && g_settledOnce) {
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
        EnsureUpscalerInit(false);
        if (!g_dlssOutValid) CreateDlssOut();
    }

    // One-time init runs on a DEDICATED THREAD, never inside the ECL callback.
    // The creation burst (PSOs/resources mid-callback) correlated with every
    // loading-phase crash of the fix20-22 era.
    if (!InterlockedCompareExchange(&g_injResourcesReady, 0, 0)) {
        bb->Release();
        if (g_settledOnce)
            KickInitThread(); // heavy PSO/heap creation only after settle
        return;
    }
    if (!g_injAlloc || !g_injList || !g_injHeap || !g_injSamplerHeap) {
        bb->Release();
        return;
    }

    g_injStep = "gated";
    bool bridgeOk = true;
    bool doDlss = g_dlaaMode && !g_dlaaHalted && g_upscaler && g_upscaler->IsReady() && g_dlssOutValid;
    if (doDlss && InterlockedCompareExchange(&g_sceneAddressReuseDetected, 0, 0)) {
        static volatile LONG s_sceneReuseGateLogs = 0;
        if (InterlockedIncrement(&s_sceneReuseGateLogs) <= 3)
            Log("hooks: DLAA blocked - scene resource address generation changed; legacy scene identity is unproven");
        doDlss = false;
    }
    if (doDlss &&
        (!ResourceGenerationMatches(g_mvResource,
             InterlockedCompareExchange64(&g_mvResourceGeneration, 0, 0)) ||
         !ResourceGenerationMatches(g_depthResource,
             InterlockedCompareExchange64(&g_depthResourceGeneration, 0, 0)))) {
        static volatile LONG s_reuseGateLogs = 0;
        if (InterlockedIncrement(&s_reuseGateLogs) <= 3)
            Log("hooks: DLAA blocked - MV/depth resource generation is stale or unknown");
        doDlss = false;
    }
    if (doDlss) {
        // STALENESS INVALIDATION: if depth/MV stamps are too old, the tracked
        // pointers likely reference freed engine resources. Null them out so
        // the bridge flow never touches them. Re-discovery will repopulate.
        // 3-frame max age: during gameplay depth/MV render EVERY frame. A gap
        // means a renderer transition (map load, resize) - the old pointers
        // may be freed with heap memory reused, which passes null checks but
        // hands the driver garbage (TDR). Tight threshold trades rediscovery
        // cost for safety.
        // Sticky settle: scene identity must be UNCHANGED for 90 consecutive
        // frames AND no active quarantine before we inject. Prevents slipping
        // into gaps between churn re-arms while the graph is still rebuilding.
        // Once settled for the session, scene-slot swaps are NORMAL gameplay
        // (engine rotates 3+ composite sources). Stamps+SEH are the safety net
        // (28-min stable run proof). Quarantine remains for pre-settle only.
        // Latch itself is computed early in the present path (before heavy
        // init) - see SESSION SETTLE LATCH above.
        if (!g_settledOnce) {
            doDlss = false;
        }
        // ONE DLAA flow per ENGINE frame. Multiple swapchain Present paths
        // (Present + Present1 + child windows) otherwise run full NGX
        // evaluates back-to-back within one frame (7x seen at frame 338)
        // and overwhelm the driver.
        if (doDlss && g_lastDlaaFrame == g_frameCounter) {
            doDlss = false;
        }
        // Depth must be single-sample with a KNOWN format before we can build
        // a matching shared texture - MSAA or unknown formats made the copy
        // an illegal operation (silent GPU fault, instant death).
        // Veteran-input gate: never inject on freshly-discovered inputs -
        // every observed death burst was fresh discovery + immediate activity.
        if (g_mvValid && (int)(g_frameCounter - g_mvFirstValidFrame) <= 120) doDlss = false;
        if (g_depthValid && (int)(g_frameCounter - g_depthFirstValidFrame) <= 60) doDlss = false;
        if (g_depthMsaa || g_depthRealFmt == DXGI_FORMAT_UNKNOWN) {
            static int s_depthGateLogs = 0;
            if (++s_depthGateLogs <= 3)
                Log("hooks: DLAA blocked - depth msaa=%d fmt=%d", g_depthMsaa ? 1 : 0, (int)g_depthRealFmt);
            doDlss = false;
        }
        // Settle = scene identity stable 90f + no active quarantine.
        // Depth/MV deliberately EXCLUDED from this gate: transient depth
        // candidates rotate every ~2s during NORMAL gameplay (shadow/composite
        // passes) - requiring them frozen forever blocks arming entirely.
        // Their safety is the 3-frame liveness stamps + bridge SEH instead.

        unsigned int fc2 = g_frameCounter;
        // MV and depth are both adopted once and reused for thousands of
        // frames WITHOUT barrier traffic in this engine (ages 6052+ observed),
        // so tight staleness caps invalidated them every single frame.
        // Trust the veteran gate + weak pointers + bridge SEH instead.
        bool depthStale = g_depthValid && (fc2 < g_depthStamp || fc2 - g_depthStamp > 20000);
        // MV textures here are TRANSIENT (created, rendered briefly, freed).
        // Using one older than ~5 frames = use-after-free = mv-barrier fault
        // (proven: re-adopted-from-registry texture faulted again in 1.1s).
        bool mvStale = g_mvValid && (fc2 < g_mvStamp || fc2 - g_mvStamp > 5);
        if (depthStale || mvStale) {
            static int s_staleLogs = 0;
            if (++s_staleLogs <= 5)
                Log("hooks: DLAA invalidated stale inputs (depth age %u, mv age %u)",
                    depthStale ? fc2 - g_depthStamp : 0,
                    mvStale ? fc2 - g_mvStamp : 0);
            doDlss = false;
            // Null the stale pointers so the null guard catches them next frame
            if (depthStale) { g_depthResource = nullptr; SetTrackedResourceGeneration(&g_depthResource, nullptr); g_depthValid = false; g_depthSrvSourced = false; g_depthLastTouchPresent = 0; }
            if (mvStale) { g_mvResource = nullptr; SetTrackedResourceGeneration(&g_mvResource, nullptr); g_mvValid = false; g_mvLastTouchPresent = 0; }
        }
    }
    if (doDlss && g_dlssOut) {
        // Never hand NGX mismatched sizes even if something above failed to
        // re-create the feature/output - 0xBAD00005 storms destabilize drivers.
        D3D12_RESOURCE_DESC od = g_dlssOut->GetDesc();
        if (od.Width != g_displayW || od.Height != g_displayH) {
            static int s_dimSkips = 0;
            if (++s_dimSkips <= 5)
                Log("hooks: DLAA skipped - output %ux%u != backbuffer %ux%u",
                    (unsigned)od.Width, (unsigned)od.Height,
                    (unsigned)g_displayW, (unsigned)g_displayH);
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

    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mvState = D3D12_RESOURCE_STATE_COMMON;
    FindTrackedState(g_depthResource, &depthState);
    FindTrackedState(g_mvResource, &mvState);

    if (doDlss) {
        // P2#10: device health gates EVERYTHING - an adapter killed during
        // load must not enter the bridge flow at all (barriers on a removed
        // device = instant hard crash, observed 18:48).
        if (doDlss && g_device) {
            HRESULT drr = g_device->GetDeviceRemovedReason();
            if (FAILED(drr)) {
                static int s_flowDrrLogs = 0;
                if (++s_flowDrrLogs <= 3) {
                    Log("hooks: DLAA flow skipped - device removed 0x%08X", (unsigned)drr);
                    HooksDumpDRED("flow-gate");
                }
                doDlss = false;
            }
        }
        // TOPOLOGY-QUIET GATE: a new display-sized node entering the copy
        // chain means the render graph is still forming (load churn). The
        // 6-7s crash class died mid-flow on textures that were being born/
        // freed around us. Flow only after the chain has been stable 120f.
        if (doDlss && g_frameCounter < g_lastNewChainFrame + 120) {
            static int s_quietLogs = 0;
            if (++s_quietLogs <= 3)
                Log("hooks: DLAA flow held - chain churned %u frames ago",
                    g_frameCounter - g_lastNewChainFrame);
            doDlss = false;
        }
        if (!g_bridgeReady || !g_gameColor || !g_gameDepth || !g_gameMv || !g_gameOut
            || !g_depthResource || !g_mvResource) {
            doDlss = false;
            // MV RE-ADOPTION from registry: invalidation dropped a texture the
            // engine will never recreate (fixed map/spawn). If its descriptor
            // key is still mapped, adopt it back - bridge SEH covers frees.
            if (!g_mvResource && g_mvLastRtvKey) {
                // Guarded single lookup (value copy; no iterator escapes).
                ID3D12Resource* riRes = nullptr;
                if (FindRtvResource((SIZE_T)g_mvLastRtvKey, &riRes) && riRes) {
                    // Format guard: the handle slot may have been recycled for a
                    // non-MV view since the key was recorded (the handle map is
                    // format-neutral by design). Never adopt a non-motion-vector
                    // resource as MV; valid re-adoptions pass unchanged.
                    D3D12_RESOURCE_DESC mrd = {};
                    if (!SafeGetDesc(riRes, &mrd) ||
                        mrd.Format != DXGI_FORMAT_R16G16_FLOAT) {
                        static int s_mvKeyMismatchLogs = 0;
                        if (++s_mvKeyMismatchLogs <= 3)
                            Log("hooks: MV re-adopt skipped - key now resolves to fmt=%u (not MV)",
                                (unsigned)mrd.Format);
                    } else {
                        // Metadata only on a real store (self-adopt guard).
                        if (!StoreTracked(&g_mvResource, riRes)) { /* keep prior MV metadata */ }
                        else {
                        g_mvValid = true;
                        g_mvStamp = g_frameCounter;
                        if (!g_mvFirstValidFrame) g_mvFirstValidFrame = g_frameCounter;
                        Log("hooks: MV re-adopted from registry %p", (void*)riRes);
                        }
                    }
                }
            }
            static int s_nullSkip = 0;
            if (++s_nullSkip <= 5)
                Log("hooks: DLAA skipped - null ptr: brC=%p brD=%p brM=%p brO=%p dep=%p mv=%p",
                    (void*)g_gameColor, (void*)g_gameDepth, (void*)g_gameMv, (void*)g_gameOut,
                    (void*)g_depthResource, (void*)g_mvResource);
            // Item #27: MV absence is a capability state, not an error. Report
            // once per session; temporal path stays disabled until MV appears.
            static bool s_mvCapAbsentLogged = false;
            if (!s_mvCapAbsentLogged && !g_mvResource && g_settledOnce && s_nullSkip > 5) {
                s_mvCapAbsentLogged = true;
                Log("DLSS: MV capability ABSENT this configuration - DLAA idle until an MV RTV binds");
            }
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
            if (g_injFence) injQueue->Signal(g_injFence, ++g_injFenceVal);
            g_injSubmitted = true;
            g_injStep = "bridge:signal-v1";
            // Game queue may ONLY signal its own device's fence view. If the
            // open failed, skip the signal - bridge eval will CPU-timeout on
            // its Wait rather than corrupting driver state with an illegal
            // cross-device fence op.
            if (g_gameFence) {
                injQueue->Signal(g_gameFence, g_bridgeVal);
            } else {
                static int s_noGameFence = 0;
                if (++s_noGameFence <= 3)
                    Log("hooks: gameFence MISSING - copy-in signal skipped");
            }

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
            bool evalOk = true;
            if (g_diagBridge) {
                static int s_diagLogs = 0;
                if (++s_diagLogs <= 10)
                    Log("DIAG#16: bridge copies done - Evaluate SKIPPED (isolation mode)");
            } else {
                evalOk = g_upscaler->Evaluate(ep);
            }
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
                StoreTracked(&g_depthResource, nullptr); g_depthValid = false; g_depthSrvSourced = false; g_depthStamp = 0; g_depthLastTouchPresent = 0;
                StoreTracked(&g_mvResource, nullptr); g_mvValid = false; g_mvStamp = 0; g_mvLastTouchPresent = 0;
                // One-strike rule (P5 crash handling): an in-engine AV means the
                // engine cmd list may be left inconsistent by the faulting call.
                // Retrying next frame re-enters the same hazard; every observed
                // hard crash followed a recovered fault within seconds. Retire
                // DLAA for this session - stability outranks coverage.
                static long s_faultStrikes = 0;
                if (InterlockedIncrement(&s_faultStrikes) >= 2) {
                    g_dlaaHalted = true;
                    Log("hooks: DLAA HALTED - %ld bridge faults this session (one-strike rule)",
                        s_faultStrikes);
                }
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
    // Copy the DLAA result from the shared texture into the backbuffer.
    // NOTE: the copy-in list was already Closed+submitted inside the bridge
    // flow. Before reusing g_injList here we must CPU-wait until that
    // submission has RETIRED - resetting an in-flight list is illegal.
    if (g_evalDidBridge) {
        if (g_injSubmitted && g_injFence && g_injEvent) {
            if (g_injFence->GetCompletedValue() < g_injFenceVal) {
                g_injFence->SetEventOnCompletion(g_injFenceVal, g_injEvent);
                WaitForSingleObject(g_injEvent, 5000);
            }
        }
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
        // Restore both touched resources - without these, next frame records
        // the same transitions again against stale states (invalid barrier,
        // driver dies after N frames).
        D3D12_RESOURCE_BARRIER bro = {};
        bro.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bro.Transition.pResource = g_gameOut;
        bro.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bro.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        bro.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_injList->ResourceBarrier(1, &bro);
        D3D12_RESOURCE_BARRIER brb = {};
        brb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        brb.Transition.pResource = bb;
        brb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        brb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        brb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_injList->ResourceBarrier(1, &brb);
        g_injList->Close();
        ID3D12CommandList* rcls[] = { g_injList };
        Real_ExecuteCommandLists(injQueue, 1, rcls);
    }
    if (doDlss) {
        Log("hooks: DLAA injection at present (frame %u)", g_frameCounter);
        g_lastDlaaFrame = g_frameCounter; // consumed this frame's DLAA slot
    }
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
static IDXGISwapChain* g_startupCandidate = nullptr;
static unsigned g_startupCandidatePresents = 0;
static IDXGISwapChain* g_startupCandidates[16] = {};
static unsigned g_startupCandidateCounts[16] = {};

// BeamNG creates several probe/UI/renderer swapchains before the display
// swapchain is stable. Do not touch a candidate's backbuffer or device until
// the same object has presented repeatedly; probing transient surfaces here
// can stall startup or leave the game with no visible window.
static bool ObserveStableSwapchain(IDXGISwapChain* sc)
{
    if (!sc || sc == g_egshDummySC) return false;
    int slot = -1;
    for (int i = 0; i < 16; ++i) {
        if (g_startupCandidates[i] == sc) { slot = i; break; }
        if (slot < 0 && !g_startupCandidates[i]) slot = i;
    }
    if (slot < 0) return false;
    if (!g_startupCandidates[slot]) {
        g_startupCandidates[slot] = sc;
        g_startupCandidateCounts[slot] = 0;
        Log("hooks: swapchain candidate %p observed (stabilizing)", (void*)sc);
    }
    if (g_startupCandidateCounts[slot] < 8) ++g_startupCandidateCounts[slot];
    g_startupCandidate = sc;
    g_startupCandidatePresents = g_startupCandidateCounts[slot];
    return g_startupCandidateCounts[slot] >= 8;
}

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

static void NgxSelfContainedPipeline(IDXGISwapChain* sc, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* queue);

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
            bool stableSwapchain = ObserveStableSwapchain((IDXGISwapChain*)sc);
            if (stableSwapchain && sc != g_swapchain) {
                g_swapchain = sc;
                Log("hooks: present on real swapchain %p (format %d)", (void*)sc, (int)g_bbFormat);
                // COMMIT A: capture and log ALL swapchain-derived identities
                static long s_identityLogged = 0;
                if (InterlockedCompareExchange(&s_identityLogged, 1, 0) == 0) {
                    __try {
                        // A1: swapchain->GetDevice(IDXGIDevice)
                        IDXGIDevice* dxgiDev = nullptr;
                        HRESULT qhr = sc->GetDevice(__uuidof(IDXGIDevice), (void**)&dxgiDev);
                        Log("IDENTITY-A: sc->GetDevice(IDXGIDevice) hr=0x%08X ptr=%p",
                            (unsigned)qhr, (void*)dxgiDev);
                        if (SUCCEEDED(qhr) && dxgiDev) {
                            // A2: adapter info from that device
                            IDXGIAdapter* pad = nullptr;
                            if (SUCCEEDED(dxgiDev->GetAdapter(&pad))) {
                                DXGI_ADAPTER_DESC pdesc = {};
                                if (SUCCEEDED(pad->GetDesc(&pdesc)))
                                    Log("IDENTITY-B: adapter VendorId=0x%04X '%ls' LUID=%08X:%08X",
                                        pdesc.VendorId, pdesc.Description,
                                        (unsigned)pdesc.AdapterLuid.HighPart,
                                        (unsigned)pdesc.AdapterLuid.LowPart);
                                pad->Release();
                            }
                            // A3: QI to ID3D12Device
                            ID3D12Device* d3ddev = nullptr;
                            HRESULT d3hr = dxgiDev->QueryInterface(__uuidof(ID3D12Device), (void**)&d3ddev);
                            Log("IDENTITY-C: QI(ID3D12Device) hr=0x%08X ptr=%p",
                                (unsigned)d3hr, (void*)d3ddev);
                            if (SUCCEEDED(d3hr) && d3ddev) {
                                // A4: compare with captured g_device
                                Log("IDENTITY-D: g_device(from detour)=%p match=%d",
                                    (void*)g_device, (g_device == d3ddev) ? 1 : 0);
                                // A5: create our own queue on this device
                                D3D12_COMMAND_QUEUE_DESC qd = {};
                                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                                ID3D12CommandQueue* q = nullptr;
                                HRESULT qhr2 = d3ddev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
                                Log("IDENTITY-E: CreateCommandQueue hr=0x%08X queue=%p",
                                    (unsigned)qhr2, (void*)q);
                                if (q) { g_graphicsQueue = q; Log("IDENTITY-F: queue stored as g_graphicsQueue"); }
                                if (d3ddev != g_device) { d3ddev->Release(); }
                                else { /* same object, don't double-release */ d3ddev->Release(); }
                            }
                            dxgiDev->Release();
                        } else {
                            Log("IDENTITY-A FAILED - swapchain GetDevice rejected");
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        Log("IDENTITY: probe guarded (exception)");
                    }
                }
                // BISECT STAGE 2: attempt NGX init on game device at first adoption
                static long s_ngxAttempted = 0;
                if (InterlockedCompareExchange(&s_ngxAttempted, 1, 0) == 0) {
                    Log("BISECT STAGE 2: attempting NGX init on game device");
                    EnsureUpscalerInit(false);
                }
            }
            // SELF-CONTAINED NGX PIPELINE: runs every frame from Present.
            // Gets device/queue/textures internally via swapchain.
            if (stableSwapchain && g_dlaaMode && !g_passiveMode) {
                __try {
                    NgxSelfContainedPipeline(sc, nullptr, nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    static int s_ngxStorm = 0;
                    if (++s_ngxStorm <= 3)
                        Log("ngx-pipe: guarded fault");
                }
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

// SHADOW EVAL (mechanics-only NGX proof, zero visible impact): evaluate NGX
// once per Present on an OWN list submitted to the steady-state game queue,
// using the just-presented backbuffer as LDR color input plus fully-owned
// zero MV/depth/output. See ShadowEvalAtPresent body below for hypothesis,
// state reasoning, and limits. Globals here; function follows LogInjectFault.
static ID3D12CommandAllocator* g_shAlloc[2] = {};
static ID3D12GraphicsCommandList* g_shList[2] = {};
static ID3D12Resource* g_shMv = nullptr;
static ID3D12Resource* g_shDepth = nullptr;
static ID3D12Resource* g_shMvUp = nullptr;
static ID3D12Resource* g_shDepthUp = nullptr;
static ID3D12Resource* g_shOut = nullptr;
static ID3D12Fence* g_shFence = nullptr;
static UINT64 g_shFenceNext = 1;
static UINT64 g_shFenceDone[2] = {};
static UINT64 g_shUpFenceVal = 0;
static unsigned g_shFlip = 0;
// Visible-handoff master switch (F8 toggles live; logged). Default ON: this
// VM exists to verify DLSS visuals, and any corruption is diagnosable
// evidence with a one-key revert.
static bool g_shadowHandoff = true;
// HDR/LDR A/B switch for the shadow path's LDR backbuffer input (F7 toggles
// live + recreates the feature; logged). File-local toggle state; the NGX
// side goes through IUpscaler::SetHDR (anonymous-namespace globals have no
// external linkage by design).
static bool g_dlssForceLDR = false;
// A/B auto-alternation window in presents (0 = steady manual-only control).
// Nonzero alternates handoff ON/OFF per window for bot captures.
static unsigned g_abWindowPresents = 0;
// Real-input switch for the shadow path (F9 toggles live + logged; INI
// `realInputs`, default OFF). OFF = owned zero MV/depth (known-good).
// ON = engine MV/depth after fail-closed validation, restored post-eval.
static bool g_shadowRealInputs = false;

// Own-texture registry check: every texture ScaleNG creates itself. The
// discovery hooks (RTV/SRV creation, OM bind, copy DEST, registry re-adopt)
// all funnel through StoreTracked, so one comparison list here protects
// every slot (scene/MV/depth) against self-adoption feedback loops.
static bool IsOwnResource(ID3D12Resource* res)
{
    if (!res) return false;
    return res == g_shMv || res == g_shDepth || res == g_shOut ||
           res == g_shMvUp || res == g_shDepthUp ||
           res == g_hudAtlas || res == g_hudVb ||
           res == g_ngxColor || res == g_ngxDepth || res == g_ngxMv || res == g_ngxOut ||
           res == g_dlssOut ||
           res == g_brColor || res == g_brDepth || res == g_brMv || res == g_brOut ||
           res == g_gameColor || res == g_gameDepth || res == g_gameMv || res == g_gameOut ||
           res == g_b2ColorG || res == g_b2ColorO || res == g_b2OutG || res == g_b2OutO ||
           res == g_b2Depth || res == g_b2Mv;
}

static void ShadowEvalAtPresent(IDXGISwapChain* sc, unsigned long long presentSerial)
{
    if (g_dlaaMode || !g_upscaler || !g_upscaler->IsReady()) return;
    // Eval outcome counters (declared up-front: the halt check below needs
    // them; POD statics are __try-safe anywhere in this frame).
    static volatile LONG s_shOk = 0, s_shFail = 0;
    static volatile LONG s_shConsecFail = 0;
    static volatile LONG s_shHalted = 0;
    // Reset-fail streak for the heartbeat below (declared up-front: both
    // the reset-fail early return and the ok path below touch it).
    static volatile LONG s_shResetStreak = 0;
    if (InterlockedCompareExchange(&s_shHalted, 0, 0)) {
        // Bounded heartbeat: halt is otherwise fully silent, making
        // breaker-passthrough indistinguishable from F8-OFF, fence-skip,
        // or silent eval-fail stretches. Logs the CURRENT handoff setting
        // so the passthrough cause is explicit. POD-only (__try below).
        static volatile LONG s_shHaltLogs = 0;
        LONG hn = InterlockedIncrement(&s_shHaltLogs);
        if (hn <= 3 || (presentSerial % 600) == 0)
            Log("hooks: shadow-eval passthrough src=HALTED handoffSetting=%d (present %llu)",
                g_shadowHandoff ? 1 : 0, presentSerial);
        return;
    }
    // F8 edge-triggered handoff toggle for live A/B comparison.
    // F7 edge-triggered HDR/LDR mode toggle (recreates feature).
    // F9 edge-triggered real/zero MV+depth input toggle (logged).
    {
        static bool s_f8Prev = false, s_f7Prev = false, s_f9Prev = false;
        bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8 && !s_f8Prev) {
            g_shadowHandoff = !g_shadowHandoff;
            Log("hooks: shadow-eval handoff %s (F8)", g_shadowHandoff ? "ON" : "OFF");
        }
        s_f8Prev = f8;
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !s_f9Prev) {
            g_shadowRealInputs = !g_shadowRealInputs;
            Log("hooks: shadow-eval inputs %s (F9)", g_shadowRealInputs ? "REAL MV/depth" : "zero placeholders");
        }
        s_f9Prev = f9;
        bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !s_f7Prev && g_upscaler) {
            g_dlssForceLDR = !g_dlssForceLDR;
            g_upscaler->SetHDR(!g_dlssForceLDR);
            g_upscaler->ResetFeature();
            Log("hooks: shadow-eval color mode %s (F7, feature recreates next eval)",
                g_dlssForceLDR ? "LDR" : "HDR");
        }
        s_f7Prev = f7;
    }
    if (!g_device || g_displayW == 0 || g_displayH == 0) return;
    ID3D12CommandQueue* queue = g_gameSubmitQueue ? g_gameSubmitQueue : g_graphicsQueue;
    if (!queue) return;
    ID3D12Resource* bb = nullptr;
    // Use the CURRENT backbuffer index, never hardcoded 0: with 2+ buffers,
    // index 0 may be the frame the engine is already acquiring next, and
    // transitioning/copying it corrupts engine state (prime suspect in the
    // 165729Z GPU crash: 2 handoffs then device loss). QI IDXGISwapChain3;
    // logged fallback to 0 only if the interface is unavailable.
    {
        UINT bbIndex = 0;
        IDXGISwapChain3* sc3 = nullptr;
        __try {
            if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) && sc3) {
                bbIndex = sc3->GetCurrentBackBufferIndex();
                sc3->Release();
            } else {
                static volatile LONG s_bbIdxLogs = 0;
                if (InterlockedIncrement(&s_bbIdxLogs) <= 2)
                    Log("hooks: shadow-eval no IDXGISwapChain3 - using buffer 0");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        __try {
            if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb))) || !bb) return;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        static UINT s_bbLastIndex = 0xFFFFFFFFu;
        if (bbIndex != s_bbLastIndex) {
            s_bbLastIndex = bbIndex;
            Log("hooks: shadow-eval backbuffer index=%u", bbIndex);
        }
    }
    D3D12_RESOURCE_DESC bbd = {};
    __try { bbd = bb->GetDesc(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static volatile LONG s_shDescFaults = 0;
        if (InterlockedIncrement(&s_shDescFaults) <= 3)
            Log("hooks: shadow-eval backbuffer desc fault");
        bb->Release(); return;
    }
    if (bbd.Width != g_displayW || bbd.Height != g_displayH ||
        bbd.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        static volatile LONG s_shSizeMiss = 0;
        if (InterlockedIncrement(&s_shSizeMiss) <= 3)
            Log("hooks: shadow-eval size mismatch bb=%ux%u fmt=%u vs display=%ux%u",
                (unsigned)bbd.Width, (unsigned)bbd.Height, (unsigned)bbd.Format,
                g_displayW, g_displayH);
        bb->Release(); return;
    }

    // Lazy one-time infra (own fence/allocators/lists/inputs/output).
    static LONG s_shInit = 0;
    if (!g_shFence || !g_shAlloc[0] || !g_shList[0] || !g_shMv || !g_shDepth || !g_shOut) {
        if (InterlockedCompareExchange(&s_shInit, 1, 0) != 0) { bb->Release(); return; }
        bool ok = true;
        // Per-step HRESULTs (first-failure diagnosis, bounded log below).
        // f=fence a=allocator l=list m=MV-zero d=depth-zero o=output.
        HRESULT fhr = S_OK, ahr = S_OK, lhr = S_OK, mhr = S_OK, dhr = S_OK, ohr = S_OK;
        HRESULT mhrMap = S_OK, dhrMap = S_OK;
        if (!g_shFence) fhr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_shFence));
        if (FAILED(fhr)) ok = false;
        for (int i = 0; ok && i < 2; ++i) {
            if (!g_shAlloc[i]) ahr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_shAlloc[i]));
            if (FAILED(ahr)) { ok = false; break; }
            if (!g_shList[i]) lhr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_shAlloc[i], nullptr, IID_PPV_ARGS(&g_shList[i]));
            if (FAILED(lhr)) { ok = false; break; }
            if (g_shList[i]) g_shList[i]->Close();
        }
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = g_displayW; td.Height = g_displayH;
        td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES up = {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES hpDef = {};
        hpDef.Type = D3D12_HEAP_TYPE_DEFAULT;
        // MV/depth live in DEFAULT heaps (UPLOAD float TEXTURES fail on this
        // device with E_INVALIDARG), filled once from UPLOAD staging BUFFERS
        // via CopyTextureRegion on our own list below, then parked permanently
        // in PSR (never transitioned again — no per-frame state tracking).
        // Staging buffers release once the upload fence completes (see eval
        // path); releasing earlier would free GPU-read memory mid-flight.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT mvFoot = {};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT dFoot = {};
        UINT64 mvBytes = 0, dBytes = 0;
        if (ok && !g_shMv) {
            td.Format = DXGI_FORMAT_R16G16_FLOAT;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            mhr = g_device->CreateCommittedResource(&hpDef, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_shMv));
            if (SUCCEEDED(mhr) && g_shMv) {
                g_device->GetCopyableFootprints(&td, 0, 1, 0, &mvFoot, nullptr, nullptr, &mvBytes);
                D3D12_RESOURCE_DESC btd = {};
                btd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                btd.Width = mvBytes ? mvBytes : 1;
                btd.Height = 1; btd.DepthOrArraySize = 1; btd.MipLevels = 1;
                btd.SampleDesc.Count = 1; btd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                mhrMap = g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &btd,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_shMvUp));
                if (SUCCEEDED(mhrMap) && g_shMvUp) {
                    void* mem = nullptr; D3D12_RANGE r = { 0, 0 };
                    mhrMap = g_shMvUp->Map(0, &r, &mem);
                    if (SUCCEEDED(mhrMap) && mem) {
                        memset(mem, 0, (size_t)mvBytes);
                        D3D12_RANGE w = { 0, (SIZE_T)mvBytes };
                        g_shMvUp->Unmap(0, &w);
                    } else mhrMap = E_FAIL;
                }
                if (FAILED(mhrMap)) { mhr = mhrMap; ok = false; }
            } else { mhr = FAILED(mhr) ? mhr : E_FAIL; ok = false; }
        }
        if (ok && !g_shDepth) {
            td.Format = DXGI_FORMAT_R32_FLOAT;
            dhr = g_device->CreateCommittedResource(&hpDef, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_shDepth));
            if (SUCCEEDED(dhr) && g_shDepth) {
                g_device->GetCopyableFootprints(&td, 0, 1, 0, &dFoot, nullptr, nullptr, &dBytes);
                D3D12_RESOURCE_DESC btd = {};
                btd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                btd.Width = dBytes ? dBytes : 1;
                btd.Height = 1; btd.DepthOrArraySize = 1; btd.MipLevels = 1;
                btd.SampleDesc.Count = 1; btd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                dhrMap = g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &btd,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_shDepthUp));
                if (SUCCEEDED(dhrMap) && g_shDepthUp) {
                    void* mem = nullptr; D3D12_RANGE r = { 0, 0 };
                    dhrMap = g_shDepthUp->Map(0, &r, &mem);
                    if (SUCCEEDED(dhrMap) && mem) {
                        memset(mem, 0, (size_t)dBytes);
                        D3D12_RANGE w = { 0, (SIZE_T)dBytes };
                        g_shDepthUp->Unmap(0, &w);
                    } else dhrMap = E_FAIL;
                }
                if (FAILED(dhrMap)) { dhr = dhrMap; ok = false; }
            } else { dhr = FAILED(dhr) ? dhr : E_FAIL; ok = false; }
        }
        if (ok && !g_shOut) {
            D3D12_RESOURCE_DESC od = td;
            od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            ohr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &od,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_shOut));
            if (FAILED(ohr)) ok = false;
        }
        // One-time upload: staging zeros -> DEFAULT MV/depth, parked in PSR.
        // Recorded on our own list[0] (fresh allocator, never submitted) and
        // executed on the game queue; both eval slots stay fence-gated until
        // this upload completes, so later presents skip non-blockingly.
        // Staging buffers release in the eval path once fenced (below).
        HRESULT uhr = S_OK;
        if (ok && (g_shMvUp || g_shDepthUp) && g_shAlloc[0] && g_shList[0]) {
            HRESULT uploadAllocResetHr = g_shAlloc[0]->Reset();
            HRESULT uploadListResetHr = SUCCEEDED(uploadAllocResetHr)
                ? g_shList[0]->Reset(g_shAlloc[0], nullptr) : E_FAIL;
            if (FAILED(uploadAllocResetHr) || FAILED(uploadListResetHr)) {
                // Initialization has its own allocator/list Reset site, before
                // the normal eval Reset guard below. Fail closed here too:
                // never let partially initialized zero inputs reach Evaluate
                // on a later Present after this private recorder is unusable.
                LONG resetStreak = InterlockedIncrement(&s_shResetStreak);
                InterlockedExchange(&s_shHalted, 1);
                Log("hooks: shadow-eval HALTED after upload allocator/list Reset failure (streak=%ld alloc=0x%08X list=0x%08X present %llu) - presenting unmodified",
                    resetStreak, (unsigned)uploadAllocResetHr, (unsigned)uploadListResetHr,
                    presentSerial);
                uhr = E_FAIL;
            }
            if (SUCCEEDED(uhr)) {
                D3D12_TEXTURE_COPY_LOCATION udst = {};
                udst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                udst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION usrc = {};
                usrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                if (g_shMvUp && mvBytes > 0) {
                    udst.pResource = g_shMv;
                    usrc.pResource = g_shMvUp;
                    usrc.PlacedFootprint = mvFoot;
                    g_shList[0]->CopyTextureRegion(&udst, 0, 0, 0, &usrc, nullptr);
                }
                if (g_shDepthUp && dBytes > 0) {
                    udst.pResource = g_shDepth;
                    usrc.pResource = g_shDepthUp;
                    usrc.PlacedFootprint = dFoot;
                    g_shList[0]->CopyTextureRegion(&udst, 0, 0, 0, &usrc, nullptr);
                }
                D3D12_RESOURCE_BARRIER ub = {};
                ub.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                ub.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                UINT ubN = 0;
                D3D12_RESOURCE_BARRIER ubs[2] = {};
                if (g_shMvUp) {
                    ubs[ubN].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    ubs[ubN].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    ubs[ubN].Transition.pResource = g_shMv;
                    ubs[ubN].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    ubs[ubN].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    ++ubN;
                }
                if (g_shDepthUp) {
                    ubs[ubN].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    ubs[ubN].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    ubs[ubN].Transition.pResource = g_shDepth;
                    ubs[ubN].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    ubs[ubN].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    ++ubN;
                }
                if (ubN) g_shList[0]->ResourceBarrier(ubN, ubs);
                if (FAILED(g_shList[0]->Close())) uhr = E_FAIL;
            }
            if (SUCCEEDED(uhr)) {
                ID3D12CommandList* ul[1] = { g_shList[0] };
                queue->ExecuteCommandLists(1, ul);
                UINT64 ufv = g_shFenceNext++;
                queue->Signal(g_shFence, ufv);
                g_shFenceDone[0] = ufv;
                g_shFenceDone[1] = ufv;
                g_shUpFenceVal = ufv;
            } else ok = false;
        }
        InterlockedExchange(&s_shInit, 0);
        {
            static volatile LONG s_shInfraLogs = 0;
            LONG il = InterlockedIncrement(&s_shInfraLogs);
            if ((!ok && il <= 3) || (ok && il == 1))
                Log("hooks: shadow-eval infra %s f=0x%X a=0x%X l=0x%X m=0x%X(mm=0x%X) d=0x%X(dm=0x%X) o=0x%X %ux%u",
                    ok ? "ready" : "FAILED", (unsigned)fhr, (unsigned)ahr, (unsigned)lhr,
                    (unsigned)mhr, (unsigned)mhrMap, (unsigned)dhr, (unsigned)dhrMap,
                    (unsigned)ohr, g_displayW, g_displayH);
        }
        if (!ok) { bb->Release(); return; }
    }

    // Dimension honesty (2026-10-07): the engine renders full-res (viewport
    // patch is replay-blocked), so the only coherent eval is DLAA at native
    // size. Re-target the feature render==display once per display size;
    // UpdateSizes destroys + lazily recreates the feature at next Evaluate.
    // Legacy render-scale params stay untouched for a future engine input.
    {
        static unsigned s_shAppliedW = 0, s_shAppliedH = 0;
        if (g_displayW != 0 && (s_shAppliedW != g_displayW || s_shAppliedH != g_displayH)) {
            s_shAppliedW = g_displayW; s_shAppliedH = g_displayH;
            g_upscaler->UpdateSizes(g_displayW, g_displayH, g_displayW, g_displayH);
            Log("hooks: shadow-eval DLAA sizing render==display %ux%u (was %ux%u)",
                g_displayW, g_displayH, g_renderW, g_renderH);
        }
    }

    // A/B auto-alternation (default STEADY for human testing): when
    // g_abWindowPresents is nonzero, handoff alternates ON/OFF per window
    // for bot captures. At 0 (default) there is no alternation — manual
    // F8/INI control only, so driving/orbiting never flickers. Transitions
    // logged; per-eval handoff bit in ok log for exact ON-frame counting.
    bool handoffNow = false;
    {
        static int s_lastWindow = -1;
        int window = 0;
        if (g_abWindowPresents > 0)
            window = (int)((presentSerial / g_abWindowPresents) % 2);
        handoffNow = g_shadowHandoff && (window == 0);
        if (window != s_lastWindow) {
            s_lastWindow = window;
            Log("hooks: shadow-eval window handoff %s (present %llu)",
                handoffNow ? "ON" : "OFF", presentSerial);
        }
    }

    unsigned slot = (g_shFlip++) & 1;
    // Fence + reset diagnostics: both early exits below used to go quiet
    // after 3 logs, hiding stall-vs-poisoned-allocator ambiguity forever.
    // completed<expected = GPU never finished our work (signal skipped or
    // queue wedged); reset-fail with completed>=expected = allocator/list
    // state poisoned (e.g. unclosable list after an NGX fault).
    unsigned long long shCompleted = SafeGetFenceCompleted(g_shFence);
    if (shCompleted < g_shFenceDone[slot]) {
        static volatile LONG s_shSkips = 0;
        if (InterlockedIncrement(&s_shSkips) <= 3)
            Log("hooks: shadow-eval skipped - prior frame still in flight (completed=%llu expected=%llu)",
                shCompleted, (unsigned long long)g_shFenceDone[slot]);
        bb->Release();
        return;
    }
    HRESULT shAllocHr = g_shAlloc[slot]->Reset();
    HRESULT shListHr = FAILED(shAllocHr) ? E_FAIL : g_shList[slot]->Reset(g_shAlloc[slot], nullptr);
    if (FAILED(shAllocHr) || FAILED(shListHr)) {
        static volatile LONG s_shResetFails = 0;
        // A failed Reset means this ScaleNG-owned allocator/list is unusable.
        // Repeated Reset/recreate attempts previously flooded E_FAIL and could
        // fault again while the game continued presenting. Stop the shadow
        // path immediately; Hook_Present then forwards the untouched game frame.
        LONG rc = InterlockedIncrement(&s_shResetStreak);
        if (InterlockedIncrement(&s_shResetFails) <= 5 || (rc % 600) == 0)
            Log("hooks: shadow-eval allocator/list reset failed (src=RESET-DEAD streak=%ld alloc=0x%08X list=0x%08X completed=%llu expected=%llu)",
                rc, (unsigned)shAllocHr, (unsigned)shListHr,
                shCompleted, (unsigned long long)g_shFenceDone[slot]);
        InterlockedExchange(&s_shHalted, 1);
        Log("hooks: shadow-eval HALTED after ScaleNG-owned allocator/list Reset failure (present %llu) - presenting unmodified",
            presentSerial);
        bb->Release();
        return;
    }
    ID3D12GraphicsCommandList* list = g_shList[slot];
    // Upload staging is GPU-consumed once the upload fence value completes.
    // MV/depth themselves stay parked in PSR forever.
    if ((g_shMvUp || g_shDepthUp) && g_shUpFenceVal != 0 &&
        SafeGetFenceCompleted(g_shFence) >= g_shUpFenceVal) {
        if (g_shMvUp) { g_shMvUp->Release(); g_shMvUp = nullptr; }
        if (g_shDepthUp) { g_shDepthUp->Release(); g_shDepthUp = nullptr; }
    }
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = bb;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    b.Transition.pResource = g_shOut;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &b);

    // REAL-INPUT PATH (default OFF, F9 toggles live): feed the engine's own
    // MV/depth to NGX instead of the zero placeholders. Fail-closed: ANY
    // doubt falls back to the owned zeros (behavior below is then exactly
    // the proven path). Engine resources are transitioned via the
    // tracked-state Barrier() helper and restored to entry states in the
    // same list, so FIFO queue order keeps engine use safe.
    // Known unknowns (logged per eval batch, NOT solved here): MV value
    // range/direction/jitter inclusion (assumed UV [0,1] prev-minus-cur,
    // same as the legacy path's mvScale=W/H); depth convention
    // (DepthInverted NOT set); jitter stays 0 (render is unjittered).
    ID3D12Resource* inMv = g_shMv;
    ID3D12Resource* inDepth = g_shDepth;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    bool useReal = false;
    const char* realWhy = "off";
    D3D12_RESOURCE_STATES mvBefore = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES depthBefore = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* mvCand = nullptr;   // selected engine MV candidate (scope for diagnostics)
    ID3D12Resource* depCand = nullptr;  // selected engine depth candidate (scope for diagnostics)
    LONG64 mvGeneration = 0;
    LONG64 depthGeneration = 0;
    ResourceTouchSnapshot mvTouchDiag = {}, depthTouchDiag = {};
    bool mvTouchDiagRead = false, depthTouchDiagRead = false;
    bool mvTouchDiagMatch = false, depthTouchDiagMatch = false;
    unsigned long long mvTouchDiagAge = 9999, depthTouchDiagAge = 9999;
    const char* mvTouchDiagSlot = "none";
    ResourceTouchLedger* mvTouchDiagLedger = nullptr;
    ResourceTouchLedger* depthTouchDiagLedger = nullptr;
        ID3D12Resource* mvAltDiag = g_mvResourceAlt;
        LONG64 mvAltGenerationDiag = InterlockedCompareExchange64(&g_mvResourceAltGeneration, 0, 0);
        bool mvAltGenerationCurrent = false;
        bool mvAltDescRead = false;
        D3D12_RESOURCE_DESC mvAltDesc = {};
    if (g_shadowRealInputs) {
        realWhy = "unchecked";
        ID3D12Resource* mv = g_mvResource;
        ID3D12Resource* dep = g_depthResource;
        mvGeneration = InterlockedCompareExchange64(&g_mvResourceGeneration, 0, 0);
        depthGeneration = InterlockedCompareExchange64(&g_depthResourceGeneration, 0, 0);
        bool mvGenerationCurrent = ResourceGenerationMatches(mv, mvGeneration);
        bool depthGenerationCurrent = ResourceGenerationMatches(dep, depthGeneration);
        D3D12_RESOURCE_DESC mvd = {}, depd = {};
        bool mvAlive = mvGenerationCurrent && mv && SafeGetDesc(mv, &mvd);
        // MV rotation: the engine renders into a fresh R16G16F target most
        // frames (primary + ALT pattern in every run). A retired OR
        // wrong-sized primary must not pin us to zeros while a live,
        // display-sized ALT exists (observed: stable 1902x1033 primary vs
        // live 1920x1080 ALTs all run).
        bool considerMvAlt = !mvAlive || mvd.Format != DXGI_FORMAT_R16G16_FLOAT ||
                             mvd.Width != g_displayW || mvd.Height != g_displayH;
        if (considerMvAlt && mvAltDiag) {
            mvAltGenerationCurrent = ResourceGenerationMatches(mvAltDiag, mvAltGenerationDiag);
            if (mvAltGenerationCurrent) mvAltDescRead = SafeGetDesc(mvAltDiag, &mvAltDesc);
        }
        if (considerMvAlt && mvAltDiag && mvAltGenerationCurrent && mvAltDescRead) {
            mv = mvAltDiag;
            mvd = mvAltDesc;
            mvGeneration = mvAltGenerationDiag;
            mvGenerationCurrent = true;
            mvAlive = true;
        }
        mvCand = mv; depCand = dep;
        if (mv == g_mvResource) {
            mvTouchDiagSlot = "primary";
            mvTouchDiagLedger = &g_mvPrimaryTouchLedger;
            mvTouchDiagRead = CaptureTouchLedger(mvTouchDiagLedger, &mvTouchDiag);
        } else if (mv == g_mvResourceAlt) {
            mvTouchDiagSlot = "alt";
            mvTouchDiagLedger = &g_mvAltTouchLedger;
            mvTouchDiagRead = CaptureTouchLedger(mvTouchDiagLedger, &mvTouchDiag);
        }
        if (dep == g_depthResource) {
            depthTouchDiagLedger = &g_depthTouchLedger;
            depthTouchDiagRead = CaptureTouchLedger(depthTouchDiagLedger, &depthTouchDiag);
        }
        mvTouchDiagMatch = mvTouchDiagRead && mvTouchDiag.valid &&
            mvTouchDiag.resource == mv && mvTouchDiag.generation == mvGeneration;
        depthTouchDiagMatch = depthTouchDiagRead && depthTouchDiag.valid &&
            depthTouchDiag.resource == dep && depthTouchDiag.generation == depthGeneration;
        if (mvTouchDiagMatch && (LONG64)presentSerial >= mvTouchDiag.present)
            mvTouchDiagAge = presentSerial - (unsigned long long)mvTouchDiag.present;
        if (depthTouchDiagMatch && (LONG64)presentSerial >= depthTouchDiag.present)
            depthTouchDiagAge = presentSerial - (unsigned long long)depthTouchDiag.present;
        bool depAlive = depthGenerationCurrent && dep && SafeGetDesc(dep, &depd);
        if (InterlockedCompareExchange(&g_resourceAddressGenerationOverflow, 0, 0))
            realWhy = "resource-generation-overflow";
        else if (!g_mvValid || !mv) realWhy = "no-mv";
        else if (!mvGenerationCurrent) realWhy = "mv-generation-stale";
        else if (!mvAlive) realWhy = "mv-retired";
        else if (mvd.Format != DXGI_FORMAT_R16G16_FLOAT) realWhy = "mv-format";
        else if (IsOwnResource(mv) || IsOwnResource(dep)) realWhy = "self-input";
        else if (mvd.Width != g_displayW || mvd.Height != g_displayH) realWhy = "mv-size";
        else if (!g_depthValid || !dep) realWhy = "no-depth";
        else if (!depthGenerationCurrent) realWhy = "depth-generation-stale";
        else if (!depAlive) realWhy = "depth-retired";
        else if (g_depthRealFmt == DXGI_FORMAT_UNKNOWN || g_depthMsaa) realWhy = "depth-fmt";
        else if (depd.Width != g_displayW || depd.Height != g_displayH) realWhy = "depth-size";
        else if (g_frameCounter < g_mvStamp || g_frameCounter - g_mvStamp > 10) realWhy = "mv-stale";
        else if (g_frameCounter < g_depthStamp || g_frameCounter - g_depthStamp > 20000) realWhy = "depth-stale";
        // Present-serial observation-age heuristic. The observations are made
        // while command lists are recorded; they can precede execution and do
        // not prove a write or associate the resource contents with this color
        // frame. Threshold 3 only bounds how recently a hook saw a qualifying
        // bind/barrier; it is NOT a same-frame guarantee. Zero means no such
        // observation was seen and remains fail-closed.
        else if (g_mvLastTouchPresent == 0) realWhy = "mv-no-observation";
        else if (presentSerial - g_mvLastTouchPresent > 3) realWhy = "mv-stale-present";
        else if (g_depthLastTouchPresent == 0) realWhy = "depth-no-observation";
        else if (presentSerial - g_depthLastTouchPresent > 3) realWhy = "depth-stale-present";
        else {
            // Tracked entry states must exist: without a known StateBefore
            // there is no legal transition (fail closed to zeros). The lookup
            // helper owns the lock; this __try frame must not (C2712).
            mvCand = mv; depCand = dep;
            if (!LookupTrackedStates(mv, &mvBefore, dep, &depthBefore)) realWhy = "untracked-state";
            else {
                useReal = true; realWhy = "real";
                inMv = mv; inDepth = dep;
                mvScaleX = (float)mvd.Width; mvScaleY = (float)mvd.Height;
                Barrier(list, inMv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(list, inDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }
    }
    // Re-check after candidate validation. A changed per-address generation
    // means this slot was retired/reused; restore the entry states and fall
    // back instead of submitting ambiguous inputs.
    if (useReal && (!ResourceGenerationMatches(inMv, mvGeneration) ||
                    !ResourceGenerationMatches(inDepth, depthGeneration))) {
        Barrier(list, inMv, mvBefore);
        Barrier(list, inDepth, depthBefore);
        inMv = g_shMv;
        inDepth = g_shDepth;
        mvCand = nullptr;
        depCand = nullptr;
        mvScaleX = 1.0f;
        mvScaleY = 1.0f;
        useReal = false;
        realWhy = "resource-generation-changed";
    }
    // Source-class evidence (logging only): pointer identity of the SELECTED
    // inputs against the owned zero placeholders (g_shMv/g_shDepth). Fallback
    // and toggle-off both leave inMv/inDepth on the placeholders, so classes
    // read PLACEHOLDER/PLACEHOLDER with the fallback reason by construction.
    const char* shMvClass = (inMv == g_shMv) ? "PLACEHOLDER" : "ENGINE_MV";
    const char* shDepthClass = (inDepth == g_shDepth) ? "PLACEHOLDER" : "ENGINE_DEPTH";
    int shDepthSrvSrc = g_depthSrvSourced ? 1 : 0;
    unsigned shDepthFmt = 0;
    D3D12_RESOURCE_DESC shDepthDesc = {};
    if (inDepth && SafeGetDesc(inDepth, &shDepthDesc))
        shDepthFmt = (unsigned)shDepthDesc.Format;
    {
        // Bounded source diagnostics: transitions always; periodic identity
        // below with the ok counter. Never per-frame. Prints the CANDIDATE
        // engine resources (mv/dep), not the selected inputs: on fallback the
        // inputs are the owned placeholders and would mislead.
        static int s_lastSrc = -1;
        int src = useReal ? 1 : 0;
        if (src != s_lastSrc) {
            s_lastSrc = src;
            // Print the SELECTED candidates (mvCand/depCand), not slot
            // re-reads: engine frees MV textures mid-frame, so a re-read can
            // disagree with the validation microseconds earlier (observed).
            // When the toggle is off the candidates are null and the owned
            // placeholders (inMv/inDepth) are printed instead.
            D3D12_RESOURCE_DESC mvd2 = {}, depd2 = {};
            ID3D12Resource* mvP = mvCand ? mvCand : inMv;
            ID3D12Resource* depP = depCand ? depCand : inDepth;
            bool mOk = mvP && SafeGetDesc(mvP, &mvd2);
            bool dOk = depP && SafeGetDesc(depP, &depd2);
            Log("hooks: shadow-eval inputs %s why=%s candMv=%p(%u %ux%u%s) altMv=%p altGen=%lld altGenOK=%u altDescOK=%u altFmt=%u altSize=%ux%u candDepth=%p(%u %ux%u) mvScale=%.0fx%.0f mvClass=%s depthClass=%s frame=%u mvAge=%u depthAge=%u mvStateB=%u depthStateB=%u present=%llu mvTouchAge=%llu depthTouchAge=%llu",
                useReal ? "REAL" : "ZERO", realWhy, (void*)mvP,
                mOk ? (unsigned)mvd2.Format : 0, mOk ? (unsigned)mvd2.Width : 0, mOk ? (unsigned)mvd2.Height : 0,
                (mvP == g_mvResourceAlt) ? " ALT" : "",
                (void*)mvAltDiag, (long long)mvAltGenerationDiag,
                mvAltGenerationCurrent ? 1u : 0u, mvAltDescRead ? 1u : 0u,
                mvAltDescRead ? (unsigned)mvAltDesc.Format : 0u,
                mvAltDescRead ? (unsigned)mvAltDesc.Width : 0u,
                mvAltDescRead ? (unsigned)mvAltDesc.Height : 0u,
                (void*)depP,
                dOk ? (unsigned)depd2.Format : 0, dOk ? (unsigned)depd2.Width : 0, dOk ? (unsigned)depd2.Height : 0,
                mvScaleX, mvScaleY, shMvClass, shDepthClass,
                g_frameCounter,
                g_mvValid ? (g_frameCounter - g_mvStamp) : 9999,
                g_depthValid ? (g_frameCounter - g_depthStamp) : 9999,
                (unsigned)mvBefore, (unsigned)depthBefore, presentSerial,
                g_mvLastTouchPresent ? (presentSerial - g_mvLastTouchPresent) : 9999,
                g_depthLastTouchPresent ? (presentSerial - g_depthLastTouchPresent) : 9999);
        }
    }

    // Diagnostic: log validation chain outcome unconditionally when REAL inputs
    // are active, BEFORE the NGX evaluation call — so the result survives a fault.
    if (g_shadowRealInputs) {
        Log("hooks: shadow-eval real-inputs why=%s useReal=%d mvTouchAge=%llu depthTouchAge=%llu "
            "mvTouch=%llu depthTouch=%llu present=%llu mvCandidate=%p mvGeneration=%lld "
            "mvSlot=%s mvLedgerRead=%d mvLedgerMatch=%d mvLedgerResource=%p mvLedgerGeneration=%lld "
            "mvLedgerAge=%llu mvLedgerPresent=%lld mvLedgerEcl=%lld mvLedgerSource=%ld mvLedgerDrops=%lld "
            "depthCandidate=%p depthGeneration=%lld depthLedgerRead=%d depthLedgerMatch=%d "
            "depthLedgerResource=%p depthLedgerGeneration=%lld depthLedgerAge=%llu "
            "depthLedgerPresent=%lld depthLedgerEcl=%lld depthLedgerSource=%ld depthLedgerDrops=%lld",
            realWhy, useReal ? 1 : 0,
            g_mvLastTouchPresent ? (presentSerial - g_mvLastTouchPresent) : 9999,
            g_depthLastTouchPresent ? (presentSerial - g_depthLastTouchPresent) : 9999,
            (unsigned long long)g_mvLastTouchPresent,
            (unsigned long long)g_depthLastTouchPresent,
            (unsigned long long)presentSerial,
            (void*)mvCand, (long long)mvGeneration,
            mvTouchDiagSlot, mvTouchDiagRead ? 1 : 0, mvTouchDiagMatch ? 1 : 0,
            (void*)mvTouchDiag.resource, (long long)mvTouchDiag.generation,
            mvTouchDiagAge, (long long)mvTouchDiag.present, (long long)mvTouchDiag.ecl,
            (long)mvTouchDiag.source,
            (long long)(mvTouchDiagRead ? mvTouchDiag.dropped :
                (mvTouchDiagLedger ? InterlockedCompareExchange64(&mvTouchDiagLedger->dropped, 0, 0) : 0)),
            (void*)depCand, (long long)depthGeneration,
            depthTouchDiagRead ? 1 : 0, depthTouchDiagMatch ? 1 : 0,
            (void*)depthTouchDiag.resource, (long long)depthTouchDiag.generation,
            depthTouchDiagAge, (long long)depthTouchDiag.present, (long long)depthTouchDiag.ecl,
            (long)depthTouchDiag.source,
            (long long)(depthTouchDiagRead ? depthTouchDiag.dropped :
                (depthTouchDiagLedger ? InterlockedCompareExchange64(&depthTouchDiagLedger->dropped, 0, 0) : 0)));
    }

    UpscalerEvaluateParams ep = {};
    ep.commandList = list;
    ep.color = bb;
    ep.depth = inDepth;
    ep.motionVectors = inMv;
    ep.output = g_shOut;
    ep.jitterX = 0.0f; ep.jitterY = 0.0f;
    ep.mvScaleX = mvScaleX; ep.mvScaleY = mvScaleY;
    ep.sharpness = g_cfg.sharpness;
    // Final fail-closed check closes the interval between candidate logging
    // and the NGX call. If a collision arrived after the earlier check, undo
    // the real-input transitions and change the submitted parameters to the
    // proven placeholder pair.
    if (useReal && (!ResourceGenerationMatches(inMv, mvGeneration) ||
                    !ResourceGenerationMatches(inDepth, depthGeneration))) {
        Barrier(list, inMv, mvBefore);
        Barrier(list, inDepth, depthBefore);
        inMv = g_shMv;
        inDepth = g_shDepth;
        ep.depth = inDepth;
        ep.motionVectors = inMv;
        ep.mvScaleX = 1.0f;
        ep.mvScaleY = 1.0f;
        useReal = false;
        realWhy = "resource-generation-changed";
        Log("hooks: shadow-eval inputs ZERO why=resource-generation-changed mvClass=PLACEHOLDER depthClass=PLACEHOLDER present=%llu (forced immediately before NGX call)",
            presentSerial);
    }
    bool ok = g_upscaler->Evaluate(ep);
    // All post-eval driver calls (restore, handoff record, Close, submit)
    // run under ONE guard: a faulted NGX record poisons recorder state and
    // any of these calls can AV — previously only the submit tail was
    // guarded, so an AV during restore/handoff recording unwound past
    // discard, abandoning the list open and wedging the allocator forever
    // (run 154828Z: fault → outer-guard AV → permanent reset E_FAIL).
    // Healthy paths byte-identical. POD locals only (C2712).
    bool submitted = false;
    __try {
    if (useReal) {
        // Restore engine resources to entry states in the same list.
        Barrier(list, inMv, mvBefore);
        Barrier(list, inDepth, depthBefore);
    }

    // HANDOFF (visible DLSS): on success, write the evaluated output back
    // into the backbuffer before Real_Present runs. All states self-owned:
    // bb is PSR (set above), shOut is UAV (set above). F8 toggles live for
    // A/B comparison (logged). On eval failure the original frame presents
    // untouched (prior behavior).
    static bool s_handoffLogged = false;
    if (ok && handoffNow) {
        b.Transition.pResource = bb;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
        b.Transition.pResource = g_shOut;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION hdst = {};
        hdst.pResource = bb;
        hdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        hdst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION hsrc = {};
        hsrc.pResource = g_shOut;
        hsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        hsrc.SubresourceIndex = 0;
        list->CopyTextureRegion(&hdst, 0, 0, 0, &hsrc, nullptr);
        b.Transition.pResource = g_shOut;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &b);
        b.Transition.pResource = bb;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &b);
        if (!s_handoffLogged) {
            s_handoffLogged = true;
            Log("hooks: shadow-eval HANDOFF armed (F8 toggles)");
        }
    } else {
        b.Transition.pResource = bb;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &b);
        b.Transition.pResource = g_shOut;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &b);
    }
    // Submit, unless the list is unusable: a faulted NGX record can leave
    // the list unclosable, and submitting it (or leaving the allocator
    // stuck) kills every later frame silently — observed as permanent
    // reset-failure after a single CreateFeature AV. On failure the GPU
    // never saw this list, so discarding + resetting is state-neutral;
    // CPU-side tracked states are restored to entry values to match.
    // Submit runs under the single outer guard above (record + submit
    // share one __except; a separate inner guard would double-handle).
        HRESULT chr = list->Close();
        if (SUCCEEDED(chr)) {
            ID3D12CommandList* lists[1] = { list };
            queue->ExecuteCommandLists(1, lists);
            UINT64 fv = g_shFenceNext++;
            queue->Signal(g_shFence, fv);
            g_shFenceDone[slot] = fv;
            submitted = true;
        } else {
            static volatile LONG s_shCloseFails = 0;
            if (InterlockedIncrement(&s_shCloseFails) <= 5)
                Log("hooks: shadow-eval list Close failed hr=0x%08X (present %llu)", (unsigned)chr, presentSerial);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static volatile LONG s_shSubmitFaults = 0;
        if (InterlockedIncrement(&s_shSubmitFaults) <= 5)
            Log("hooks: shadow-eval record/submit faulted (code 0x%08X present %llu)", (unsigned)GetExceptionCode(), presentSerial);
    }
    if (!submitted) {
        if (useReal) NoteTrackedStates(inMv, mvBefore, inDepth, depthBefore);
        __try {
            list->Close();
            HRESULT discardAllocHr = g_shAlloc[slot]->Reset();
            HRESULT discardListHr = FAILED(discardAllocHr) ? E_FAIL :
                g_shList[slot]->Reset(g_shAlloc[slot], nullptr);
            if (FAILED(discardAllocHr) || FAILED(discardListHr)) {
                LONG resetStreak = InterlockedIncrement(&s_shResetStreak);
                InterlockedExchange(&s_shHalted, 1);
                Log("hooks: shadow-eval HALTED after discard allocator/list Reset failure (streak=%ld alloc=0x%08X list=0x%08X present %llu) - presenting unmodified",
                    resetStreak, (unsigned)discardAllocHr, (unsigned)discardListHr,
                    presentSerial);
            } else {
                HRESULT discardCloseHr = g_shList[slot]->Close();
                if (FAILED(discardCloseHr)) {
                    InterlockedExchange(&s_shHalted, 1);
                    Log("hooks: shadow-eval HALTED after discard list Close failure hr=0x%08X (present %llu) - presenting unmodified",
                        (unsigned)discardCloseHr, presentSerial);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static volatile LONG s_shDiscardFaults = 0;
            InterlockedExchange(&s_shHalted, 1);
            if (InterlockedIncrement(&s_shDiscardFaults) <= 3)
                Log("hooks: shadow-eval HALTED after discard fault (code 0x%08X present %llu) - presenting unmodified",
                    (unsigned)GetExceptionCode(), presentSerial);
        }
    }
    bb->Release();

    if (ok) {
        LONG was = InterlockedExchange(&s_shConsecFail, 0);
        InterlockedExchange(&s_shResetStreak, 0);
        LONG n = InterlockedIncrement(&s_shOk);
        if (n <= 10 || (n % 600) == 0) {
            Log("hooks: shadow-eval ok #%ld (present %llu handoff %d)", n, presentSerial, handoffNow ? 1 : 0);
            Log("hooks: shadow-eval inputs %s why=%s mv=%p depth=%p mvScale=%.0fx%.0f frame=%u mvAge=%u depthAge=%u mvClass=%s depthClass=%s depthFmt=%u srvSrc=%d present=%llu mvStateB=%u depthStateB=%u",
                useReal ? "REAL" : "ZERO", realWhy, (void*)inMv, (void*)inDepth,
                mvScaleX, mvScaleY, g_frameCounter,
                g_mvValid ? (g_frameCounter - g_mvStamp) : 9999,
                g_depthValid ? (g_frameCounter - g_depthStamp) : 9999,
                shMvClass, shDepthClass, shDepthFmt, shDepthSrvSrc,
                presentSerial, (unsigned)mvBefore, (unsigned)depthBefore);
        }
        if (was >= 30)
            Log("hooks: shadow-eval recovered after %ld-fault streak (ok #%ld)", was, n);
    } else {
        LONG n = InterlockedIncrement(&s_shFail);
        LONG cf = InterlockedIncrement(&s_shConsecFail);
        if (n <= 10)
            Log("hooks: shadow-eval FAILED #%ld (present %llu)", n, presentSerial);
        // Circuit breaker: faulting every present helps nothing and risks
        // driver state. At 30 consecutive faults, drop the NGX feature so the
        // next eval recreates it with fresh history (poisoned-history
        // hypothesis, falsifiable: recovery proves transient NGX state). At
        // 120, halt shadow evals for the session — the game then presents
        // unmodified frames (safe fallback, logged once).
        if (cf == 30 && g_upscaler) {
            g_upscaler->ResetFeature();
            Log("hooks: shadow-eval 30 consecutive faults - feature reset (present %llu)", presentSerial);
        } else if (cf == 120) {
            InterlockedExchange(&s_shHalted, 1);
            Log("hooks: shadow-eval HALTED after 120 consecutive faults (present %llu) - presenting unmodified", presentSerial);
        }
    }
}


HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    const unsigned long long presentSerial =
        (unsigned long long)InterlockedIncrement64(&g_presentSerial);
    ObservePersistentSceneColor(presentSerial);
    // LEGACY INIT RETRY (dlaa=0 only): camera accepts arrive in loading
    // bursts, then stop â€” so the camera-path Ensure call never re-fires once
    // present-quiet matures. Retry here at 1/60 presents while the upscaler
    // is not ready; the 600p gate inside Ensure still decides. Atomic
    // single-attempt guard makes double-init impossible across threads.
    // No rendering, selection, or forwarding effect until init succeeds.
    if (!g_dlaaMode && (!g_upscaler || !g_upscaler->IsReady()) && (presentSerial % 60) == 0)
        EnsureUpscalerInit(false);
    LogTopoSnapshot(presentSerial, g_b2PresentBb, g_b2OutG, g_b2Ready,
                    (int)InterlockedCompareExchange(&g_b2DeferredPending, 0, 0));
    // Unconditional first-call proof: if THIS never logs, nothing on earth
    // is calling dxgi's public Present in this process.
    {
        static volatile LONG s_first = 0;
        if (InterlockedCompareExchange(&s_first, 1, 0) == 0)
            Log("Hook_Present ENTER sc=%p sync=%u flags=%u serial=%llu",
                (void*)sc, syncInterval, flags, presentSerial);
    }
    if (sc) {
        __try {
            // Terminal-node correlation (unconditional entry): what the copy
            // chain last wrote, sampled at Present. The recurring ptr here IS
            // the DLAA input target.
            static unsigned s_presFeedCount = 0;
            if (++s_presFeedCount % 60 == 1 && g_topoLastSrc) {
                Log("present-feed: last full-res src %p fmt %u",
                    g_topoLastSrc, g_topoLastFmt);
            }
            // Install-vs-invocation split (bounded: same 1/60 cadence as the
            // feed above): exact atomic totals distinguishing shim-table
            // installs from actual per-method callback invocations.
            {
                static unsigned s_hookTotals = 0;
                if (++s_hookTotals % 60 == 1) {
                    Log("hooks: shim totals instOk=%lld instDedup=%lld instFail=%lld buf=%lld tex=%lld res=%lld rt=%lld barrier=%lld om=%lld vp=%lld sc=%lld",
                        (long long)InterlockedCompareExchange64(&g_installOkTotal, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_installDedupTotal, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_installFailTotal, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_shimBufferCopyCalls, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_shimCopyCalls, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_shimCopyResCalls, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_shimRootTableCalls, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_shimBarrierCalls, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_shimOmCalls, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_shimVpCalls, 0, 0),
                        (long long)InterlockedCompareExchange64(&g_shimScCalls, 0, 0));
                    // Scene-set hygiene (bounded: <=8 guarded reads per sweep).
                    // Retired targets fault here and are dropped, so the
                    // matching set cannot accumulate freed pointers.
                    static unsigned s_sceneSweep = 0;
                    if ((++s_sceneSweep % 10) == 1) SceneSetSweep();
                }
            }
            if (sc == g_egshDummySC) { /* EGSH self-test present: never adopt */ }
            bool stableSwapchain = ObserveStableSwapchain(sc);
            if (stableSwapchain && sc != g_swapchain) {
                g_swapchain = sc;
                Log("hooks: present on real swapchain %p (format %d)", (void*)sc, (int)g_bbFormat);
            }
            // SELF-CONTAINED NGX PIPELINE ENTRY: the ECL hook is build-disabled,
            // so Present IS the per-frame driver.
            if (stableSwapchain && g_dlaaMode && !g_passiveMode && g_swapchain) {
                __try {
                    InjectAtPresentImpl(nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    static int s_presFault = 0;
                    if (++s_presFault <= 5)
                        Log("ngx-pipe: present-path guarded fault #%d", s_presFault);
                }
            }
            // Shadow eval (legacy only): mechanics proof with zero visible
            // impact; runs inside the same guarded region, original presents.
            if (!g_dlaaMode && !g_passiveMode)
                ShadowEvalAtPresent(sc, presentSerial);
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
    const unsigned long long presentSerial =
        (unsigned long long)InterlockedIncrement64(&g_presentSerial);
    ObservePersistentSceneColor(presentSerial);
    // Same legacy init retry as Hook_Present (whichever Present the engine
    // drives; shared serial keeps the combined cadence bounded).
    if (!g_dlaaMode && (!g_upscaler || !g_upscaler->IsReady()) && (presentSerial % 60) == 0)
        EnsureUpscalerInit(false);
    if (sc) {
        __try {
            static unsigned s_present1Diag = 0;
            if (++s_present1Diag <= 3)
                Log("Hook_Present1 observed serial=%llu sc=%p", presentSerial, (void*)sc);
            if (sc == (IDXGISwapChain1*)g_egshDummySC) { /* EGSH self-test present */ }
            bool stableSwapchain = ObserveStableSwapchain((IDXGISwapChain*)sc);
            if (stableSwapchain && sc != g_swapchain) {
                g_swapchain = sc;
                Log("hooks: present on real swapchain %p (format %d)", (void*)sc, (int)g_bbFormat);
            }
            // SELF-CONTAINED NGX PIPELINE ENTRY (Present1 variant)
            if (stableSwapchain && g_dlaaMode && !g_passiveMode && g_swapchain) {
                __try {
                    InjectAtPresentImpl(nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    static int s_pres1Fault = 0;
                    if (++s_pres1Fault <= 5)
                        Log("ngx-pipe: present1-path guarded fault #%d", s_pres1Fault);
                }
            }
            // Shadow eval, Present1 variant (this is the path the engine
            // drives during gameplay; Hook_Present goes quiet after loading).
            // Same guarded region; original presents on all outcomes.
            if (!g_dlaaMode && !g_passiveMode)
                ShadowEvalAtPresent((IDXGISwapChain*)sc, presentSerial);
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
    Log("hooks: InstallSwapchainHooks called sc=%p", (void*)sc);
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

void EnsureGlobalSwapchainHookImpl()
{
    // RE-ENTRANCY GUARD: Hook_D3D12CreateDevice tail-calls us; step 1 below
    // calls D3D12CreateDevice -> detour -> back into here. Without this ICAS
    // the nested pass faults (observed C0000005 right after "EGSH device ok").
    static volatile LONG s_inEGSH = 0;
    if (InterlockedCompareExchange(&s_inEGSH, 1, 0) != 0) return;
    // Defer while the smoke test owns the driver: its device churn + our
    // factory hooks racing game-thread factory creation faulted here once.
    if (g_smokeBusy != 0 || g_device == nullptr) { InterlockedExchange(&s_inEGSH, 0); return; }
    static int s_tries = 0;
    if (g_scanDone || s_tries >= 10) { InterlockedExchange(&s_inEGSH, 0); return; }
    ++s_tries;

    __try {
        // ================================================================
        // STEP 1: Create dummy device + swapchain CLEANLY (no hooks yet!)
        // Each sub-step individually guarded: one fault must not prevent
        // the Present-hook installation stage.
        // ================================================================
        HWND dummyWnd = EnsureDummyWindow();
        ID3D12Device* ddev = nullptr;
        HRESULT dhr = E_FAIL;
        __try {
            typedef HRESULT(WINAPI* PFN_D3D12Create)(IUnknown*, D3D_FEATURE_LEVEL, const IID&, void**);
            HMODULE d3dMod = GetModuleHandleA("d3d12.dll");
            PFN_D3D12Create mkDev = d3dMod ? (PFN_D3D12Create)GetProcAddress(d3dMod, "D3D12CreateDevice") : nullptr;
            if (mkDev) dhr = mkDev(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&ddev);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: EGSH mkDev FAULTED");
            dhr = E_FAIL;
        }
        if (FAILED(dhr) || !ddev) {
            Log("hooks: EGSH fresh device FAILED hr=%08X", (unsigned)dhr);
            InterlockedExchange(&s_inEGSH, 0);
            return;
        }
        Log("hooks: EGSH device ok %p", (void*)ddev);

        ID3D12CommandQueue* dq = nullptr;
        __try {
            D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ddev->CreateCommandQueue(&qd, IID_PPV_ARGS(&dq));
        } __except (EXCEPTION_EXECUTE_HANDLER) { Log("hooks: EGSH mkQueue FAULTED"); }
        if (!dq) { ddev->Release(); InterlockedExchange(&s_inEGSH, 0); return; }

        // Create swapchain DIRECTLY via export (no hooks active yet!)
        IDXGIFactory4* f4 = nullptr;
        IDXGISwapChain1* dummy = nullptr;
        __try {
            typedef HRESULT(WINAPI* PFN_CreateDXGI)(const IID&, void**);
            HMODULE dxgiMod = GetModuleHandleA("dxgi.dll");
            PFN_CreateDXGI mkF = dxgiMod ? (PFN_CreateDXGI)GetProcAddress(dxgiMod, "CreateDXGIFactory1") : nullptr;
            if (mkF) mkF(__uuidof(IDXGIFactory4), (void**)&f4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { Log("hooks: EGSH mkFactory FAULTED"); }
        if (!f4) { dq->Release(); ddev->Release(); InterlockedExchange(&s_inEGSH, 0); return; }

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.BufferCount = 2; sd.Width = 8; sd.Height = 8;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SampleDesc.Count = 1; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        __try {
            dhr = f4->CreateSwapChainForHwnd(dq, dummyWnd, &sd, nullptr, nullptr, &dummy);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: EGSH CreateSwapChainForHwnd FAULTED");
            dhr = E_FAIL;
        }
        Log("hooks: EGSH clean swapchain hr=0x%08X sc=%p", (unsigned)dhr, (void*)dummy);
        g_egshDummySC = dummy;

        // ================================================================
        // STEP 2: Hook PRESENT at FUNCTION LEVEL via the dummy swapchain.
        //
        // Every real DXGI swapchain shares ONE static vtable inside dxgi.dll,
        // so the function address in vt[8]/vt[22] is THE implementation every
        // game Present funnels through. MinHook patches that function's
        // prologue once - no vtable writes, no per-object state. The old
        // artifacts/freezes came from the cmdlist-hook era (since removed);
        // a single cold Present detour is the standard Reshade-style model.
        //
        // Our pipeline REQUIRES Present-time execution: its barriers assume
        // the backbuffer sits in PRESENT state at entry.
        // ================================================================
        if (SUCCEEDED(dhr) && dummy) {
            void** dvt = *(void***)dummy;
            void* pPresent = dvt[8];
            void* pPresent1 = dvt[22];
            Log("hooks: EGSH dummy sc=%p vt8=%p vt22=%p", (void*)dummy, pPresent, pPresent1);
            // Module ownership diagnostic: if these targets are NOT in
            // dxgi.dll, the game routes Presents elsewhere and we need to know.
            {
                HMODULE mod = nullptr;
                DWORD modNameLen = 0;
                wchar_t modName[64] = L"?";
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)pPresent, &mod) && mod)
                    modNameLen = GetModuleFileNameW(mod, modName, 64);
                wchar_t* base = wcsrchr(modName, L'\\');
                Log("hooks: PRESENT target module: %ls", base ? base + 1 : modName);
            }
            if (pPresent && IsExecutableImagePtr(pPresent)) {
                MH_STATUS st = MH_CreateHook(pPresent, &Hook_Present, (void**)&Real_Present);
                if (st == MH_OK && MH_EnableHook(pPresent) == MH_OK) {
                    void* targets[1] = { (void*)Hook_Present };
                    CfgMarkValid(targets, 1);
                    Log("hooks: PRESENT fn-level hook INSTALLED (%p)", pPresent);
                } else if (st != MH_ERROR_ALREADY_CREATED) {
                    Log("hooks: PRESENT fn-level hook FAILED st=%d", (int)st);
                } else {
                    Log("hooks: PRESENT fn-level hook already created");
                    Real_Present = (PFN_Present)pPresent; // not exact trampoline but non-null sentinel
                }
            }
            if (!Real_Present1 && pPresent1 && IsExecutableImagePtr(pPresent1)) {
                MH_STATUS st1 = MH_CreateHook(pPresent1, &Hook_Present1, (void**)&Real_Present1);
                if (st1 == MH_OK && MH_EnableHook(pPresent1) == MH_OK) {
                    void* t1[1] = { (void*)Hook_Present1 };
                    CfgMarkValid(t1, 1);
                    Log("hooks: PRESENT1 fn-level hook INSTALLED (%p)", pPresent1);
                } else if (st1 != MH_ERROR_ALREADY_CREATED) {
                    Log("hooks: PRESENT1 fn-level hook FAILED st=%d", (int)st1);
                }
            }

            // SELF-TEST: present our own dummy swapchain. If Hook_Present
            // fires (flag flips), the patch is live and ANY real-dxgi
            // presenter would be caught. If it does NOT fire, the game's
            // silence is explained differently (patch ineffective).
            {
                extern volatile LONG g_presentSelfTestFired;
                InterlockedExchange(&g_presentSelfTestFired, 0);
                HRESULT shr = E_FAIL;
                __try { shr = dummy->Present(0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Sleep(150);
                Log("hooks: SELF-TEST dummy->Present hr=0x%08X fired=%s",
                    (unsigned)shr, g_presentSelfTestFired ? "YES" : "NO");
            }
        }

        // Cleanup temp objects
        g_egshDummySC = nullptr;
        if (dummy) { dummy->Release(); dummy = nullptr; }
        if (f4) f4->Release();
        if (dq) dq->Release();
        ddev->Release();

        // ================================================================
        // STEP 3: Factory vtable swaps for future swapchain creation
        // ================================================================
        IDXGIFactory* factory = nullptr;
        {
            typedef HRESULT(WINAPI* PFN_CreateDXGI)(const IID&, void**);
            HMODULE dxgiMod = GetModuleHandleA("dxgi.dll");
            PFN_CreateDXGI mkF = dxgiMod ? (PFN_CreateDXGI)GetProcAddress(dxgiMod, "CreateDXGIFactory1") : nullptr;
            if (mkF) mkF(__uuidof(IDXGIFactory), (void**)&factory);
        }
        if (factory) {
            void** fvt = *(void***)factory;
            MEMORY_BASIC_INFORMATION fmbi = {};
            VirtualQuery(fvt, &fmbi, sizeof(fmbi));
            DWORD foldProt = 0;
            if (VirtualProtect(fmbi.BaseAddress, fmbi.RegionSize, PAGE_READWRITE, &foldProt)) {
                if (!Real_CreateSwapChainForHwnd && fvt[15]) {
                    Real_CreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd)fvt[15];
                    fvt[15] = (void*)&Hook_CreateSwapChainForHwnd;
                    Log("hooks: factory slot15 SWAPPED");
                }
                if (!Real_CreateSwapChain && fvt[10]) {
                    Real_CreateSwapChain = (PFN_CreateSwapChain)fvt[10];
                    fvt[10] = (void*)&Hook_CreateSwapChain;
                    Log("hooks: factory slot10 SWAPPED");
                }
                VirtualProtect(fmbi.BaseAddress, fmbi.RegionSize, foldProt, &foldProt);
            }
            factory->Release();
        }

        g_swapchain = nullptr; // real one adopted at first present via Hook_Present self-heal
        g_scanDone = true;
        InterlockedExchange(&s_inEGSH, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("hooks: EGSH guarded (code %08X)", (unsigned)GetExceptionCode());
        InterlockedExchange(&s_inEGSH, 0);
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

        // ================================================================
        // SINGLE-DEVICE CAPTURE: the IUnknown* device parameter IS the
        // game's D3D12 command queue. Capture it and derive the device.
        // This is the ONLY place we can reliably get both in one shot.
        // ================================================================
        static long s_captured = 0;
        if (InterlockedCompareExchange(&s_captured, 1, 0) == 0 && device) {
            // QI to ID3D12CommandQueue
            ID3D12CommandQueue* q = nullptr;
            HRESULT qhr = device->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&q);
            Log("SINGLE-DEV: QI(ID3D12CommandQueue) hr=0x%08X ptr=%p", (unsigned)qhr, (void*)q);
            if (SUCCEEDED(qhr) && q) {
                g_graphicsQueue = q;
                Log("SINGLE-DEV: GAME QUEUE CAPTURED %p", (void*)q);

                // GetDevice from the queue â†’ real ID3D12Device
                ID3D12Device* dev = nullptr;
                HRESULT dhr = q->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
                Log("SINGLE-DEV: queue->GetDevice(ID3D12Device) hr=0x%08X ptr=%p",
                    (unsigned)dhr, (void*)dev);
                if (SUCCEEDED(dhr) && dev) {
                    if (!g_device || g_device != dev) {
                        Log("SINGLE-DEV: DEVICE CAPTURED %p (matches g_device=%d)",
                            (void*)dev, (g_device == dev) ? 1 : 0);
                        g_device = dev; // use this for everything
                    }
                    // Adapter identity from the real device
                    IDXGIDevice* dxgidev = nullptr;
                    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgidev))) {
                        IDXGIAdapter* ad = nullptr;
                        if (SUCCEEDED(dxgidev->GetAdapter(&ad))) {
                            DXGI_ADAPTER_DESC adesc = {};
                            if (SUCCEEDED(ad->GetDesc(&adesc)))
                                Log("SINGLE-DEV: adapter VendorId=0x%04X '%ls' LUID=%08X:%08X",
                                    adesc.VendorId, adesc.Description,
                                    (unsigned)adesc.AdapterLuid.HighPart,
                                    (unsigned)adesc.AdapterLuid.LowPart);
                            ad->Release();
                        }
                        dxgidev->Release();
                    }
                }
            } else {
                Log("SINGLE-DEV: device param is NOT a command queue (wrapped?)");
            }
        }
        // END SINGLE-DEVICE CAPTURE
        // ADAPTER IDENTITY via the swapchain itself: QI on the RAW swapchain
        // (not the wrapped game device) always works and names the physical
        // adapter that owns PRESENT - the ground truth for hybrid triage.
        // GUARDED: EGSH's dummy path can return S_OK with a garbage out-param
        // (observed sc=CCCCCC..), so validate + SEH before any deref.
        __try {
            if (!IsReadablePtr(sc, sizeof(void*))) {
                Log("hooks: SWAPCHAIN ptr not readable - skipping adapter identity");
            } else {
            IDXGIDevice* sdev = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(IDXGIDevice), (void**)&sdev))) {
                IDXGIAdapter* sad = nullptr;
                if (SUCCEEDED(sdev->GetAdapter(&sad))) {
                    DXGI_ADAPTER_DESC sdsc = {};
                    if (SUCCEEDED(sad->GetDesc(&sdsc))) {
                        Log("hooks: SWAPCHAIN adapter VendorId=0x%04X '%ls' LUID=%08X:%08X",
                            sdsc.VendorId, sdsc.Description,
                            (unsigned)sdsc.AdapterLuid.HighPart, (unsigned)sdsc.AdapterLuid.LowPart);
                    }
                    sad->Release();
                }
                sdev->Release();
            } else {
                Log("hooks: SWAPCHAIN GetDevice failed - cannot name present adapter");
            }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("hooks: SWAPCHAIN adapter identity guarded (code %08X)", (unsigned)GetExceptionCode());
        }
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
        // PRESENT-ADAPTER PROBE (once per session, guarded) - UNCONDITIONAL:
        // it must fire even when InstallSwapchainHooks rejects the object,
        // because hybrid-vs-single is the open architectural question.
        {
            static long s_presAdapterProbed = 0;
            if (InterlockedCompareExchange(&s_presAdapterProbed, 1, 0) == 0) {
                __try {
                    IDXGIDevice* pdxgi = nullptr;
                    if (SUCCEEDED(sc->GetDevice(__uuidof(IDXGIDevice), (void**)&pdxgi))) {
                        IDXGIAdapter* pad = nullptr;
                        if (SUCCEEDED(pdxgi->GetAdapter(&pad))) {
                            DXGI_ADAPTER_DESC pdesc = {};
                            if (SUCCEEDED(pad->GetDesc(&pdesc)))
                                Log("hooks: PRESENT adapter VendorId=0x%04X '%ls' LUID=%08X:%08X",
                                    pdesc.VendorId, pdesc.Description,
                                    (unsigned)pdesc.AdapterLuid.HighPart, (unsigned)pdesc.AdapterLuid.LowPart);
                            pad->Release();
                        }
                        pdxgi->Release();
                    } else {
                        Log("hooks: PRESENT swapchain GetDevice failed");
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("hooks: PRESENT adapter probe guarded");
                }
            }
        }
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
    Log("hooks: HookFactoryObject called factory=%p", (void*)factory);
    if (!g_anyFactory) g_anyFactory = factory;
    // Periodic EGSH kick: guarantees the Present-hook installation eventually
    // runs even if its first opportunities were skipped (smoke busy, early
    // device creation). Cheap no-op once g_scanDone is set.
    {
        static volatile LONG s_kick = 0;
        if (InterlockedCompareExchange(&s_kick, 1, 0) == 0) {
            static int s_kickCount = 0;
            if ((++s_kickCount % 8) == 1 && !g_scanDone)
                HooksKickEGSH();
            InterlockedExchange(&s_kick, 0);
        }
    }
    void** vt = *(void***)factory;
    bool any = false;
    if (!Real_CreateSwapChainForHwnd &&
        MH_CreateHook(vt[15], &Hook_CreateSwapChainForHwnd, (void**)&Real_CreateSwapChainForHwnd) == MH_OK) {
        if (MH_EnableHook(vt[15]) == MH_OK) { any = true; Log("hooks: factory slot15 hooked OK"); }
        else { Real_CreateSwapChainForHwnd = nullptr; }
    } else if (Real_CreateSwapChainForHwnd) {
        Log("hooks: factory slot15 already hooked");
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
    // Diagnostic: bounded copy log for placed-buffer correlation.
    // Log first 50 copies >=32 bytes unconditionally (captures 96-byte updates),
    // then 1/1000 sampling plus always-log >=1024/camera/velocity sizes.
    static unsigned s_copyDiag = 0;
    unsigned nCopy = 0;
    if (numBytes >= 32) { nCopy = ++s_copyDiag; }
    if (numBytes >= 32 && (nCopy <= 50 || (nCopy % 1000) == 1 || numBytes >= 1024 || numBytes == kCameraCbSize || numBytes == kVelocityCbSize)) {
        D3D12_RESOURCE_DESC srcDesc = {}, dstDesc = {};
        unsigned srcFmt = 0, dstFmt = 0;
        unsigned long long srcW = 0, dstW = 0;
        __try { if (src) { srcDesc = src->GetDesc(); srcFmt = (unsigned)srcDesc.Format; srcW = (unsigned long long)srcDesc.Width; } } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { if (dst) { dstDesc = dst->GetDesc(); dstFmt = (unsigned)dstDesc.Format; dstW = (unsigned long long)dstDesc.Width; } } __except (EXCEPTION_EXECUTE_HANDLER) {}
        unsigned listType = 0;
        __try { if (list) listType = list->GetType(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("hooks: CopyBufferRegion src=%p dst=%p srcOff=%llu dstOff=%llu bytes=%llu list=%p type=%u srcFmt=%u srcW=%llu dstFmt=%u dstW=%llu",
            (void*)src, (void*)dst, (unsigned long long)srcOffset, (unsigned long long)dstOffset, (unsigned long long)numBytes,
            (void*)list, listType, srcFmt, srcW, dstFmt, dstW);
    }
    static int s_otherSizes = 0;
    if (numBytes >= 1024 && numBytes != kCameraCbSize && numBytes != kVelocityCbSize &&
        srcOffset == 0 && dstOffset == 0 && s_otherSizes < 10) {
        ++s_otherSizes;
        Log("hooks: CopyBufferRegion size=%llu (src %p dst %p)", (unsigned long long)numBytes, (void*)src, (void*)dst);
    }
    if (src && dst && dstOffset == 0) {
        // Camera CB validation: accept copies where the source region contains a valid camera CB.
        // This handles both exact 1616-byte copies and larger copies from a known offset in a big upload buffer.
        if (numBytes >= kCameraCbSize) {
            if (!g_cameraRing) g_cameraRing = src;
            if (g_cameraRing == src || !g_cameraCbValid) {
                void* mapped = nullptr;
                if (SUCCEEDED(src->Map(0, nullptr, &mapped)) && mapped) {
                    float* cb = (float*)((char*)mapped + srcOffset);
                    if (ValidateCameraCb(cb, kCameraCbSize)) {
                        if (g_cameraRing != src) {
                            g_cameraRing = src;
                            Log("hooks: camera CB ring re-discovered %p at srcOff=%llu", (void*)src, (unsigned long long)srcOffset);
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
                        // LEGACY INIT TRIGGER (dlaa=0 only): a validated camera
                        // copy is the earliest proof of live gameplay rendering,
                        // so the single NGX init attempt is sequenced from here.
                        // Safe despite running on the engine ECL thread: (1) the
                        // quiet gate defers with NO driver contact until the copy
                        // chain is quiet 120f; (2) the atomic attempt-gate makes
                        // double-init impossible, superseding the fix89-era race
                        // fear (no Present-thread init exists in legacy mode at
                        // all); (3) DLAA mode is excluded, preserving its
                        // settled-once init sequencing.
                        // ISOLATION (reviewer #16 pattern): SCALENG_NO_JITTER=1
                        // disables the CB patch entirely - single-variable test
                        // for whether jitter writing triggers nvwgf2umx AVs.
                        if (!g_dlaaMode)
                            EnsureUpscalerInit(false);
                        if (g_dlaaMode && !GetEnvironmentVariableA("SCALENG_NO_JITTER", nullptr, 0))
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
        }
        // Velocity CB validation (permissive: any copy >= kVelocityCbSize)
        if (numBytes >= kVelocityCbSize) {
            void* mapped = nullptr;
            if (SUCCEEDED(src->Map(0, nullptr, &mapped)) && mapped) {
                float* cb = (float*)((char*)mapped + srcOffset);
                if (ValidateVelocityCb(cb, kVelocityCbSize, g_mvW, g_mvH)) {
                    PatchVelocityCb(cb, kVelocityCbSize, g_lastPatchedCameraCb);
                    g_velocityCbPatched = true;
                    Log("hooks: velocity CB patched in place (dst %p srcOff %llu uTexSize %.4f %.4f)",
                        (void*)dst, (unsigned long long)srcOffset, cb[0], cb[1]);
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

static void CopyTexBody(ID3D12GraphicsCommandList* list,
                        const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY,
                        UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src,
                        const D3D12_BOX* srcBox)
{
    // Diagnostic: unconditional entry log to verify hook is called
    {
        static unsigned s_entryDiag = 0;
        if ((++s_entryDiag % 600) == 1) {
            Log("hooks: CopyTexBody called list=%p dst=%p src=%p deferredOutput=%d pending=%d",
                (void*)list, (void*)dst, (void*)src, (int)g_b2DeferredOutput, (int)g_b2DeferredPending);
        }
    }
    // Diagnostic: log entry when deferred output is pending
    if (g_b2DeferredOutput) {
        static unsigned s_entryDiag2 = 0;
        if ((++s_entryDiag2 % 240) == 1) {
            Log("hooks: CopyTexBody entry deferredOutput=%d pending=%d pendingVal=%llu",
                (int)g_b2DeferredOutput, (int)g_b2DeferredPending, (unsigned long long)g_b2DeferredVal);
        }
    }
    // Deferred output handoff: the helper has already finished NGX on the
    // previous Present, but Present itself never submits a second list. When
    // BeamNG records its normal full-frame copy into the current backbuffer,
    // replace only that copy's source with the shared DLSS result. The copy,
    // destination state, and queue submission remain part of BeamNG's list.
    if (g_b2DeferredOutput && g_b2DeferredPending && list && dst && src &&
        dst->pResource && src->pResource && dst->pResource == g_b2PresentBb &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst->SubresourceIndex == 0 && src->SubresourceIndex == 0 &&
        dstX == 0 && dstY == 0 && dstZ == 0 && g_b2OutG && g_b2FenceOutG) {
        D3D12_RESOURCE_DESC od = {};
        D3D12_RESOURCE_DESC dd = {};
        D3D12_RESOURCE_DESC sd = {};
        od = g_b2OutG->GetDesc();
        dd = dst->pResource->GetDesc();
        sd = src->pResource->GetDesc();
        const UINT64 pending = g_b2DeferredVal;
        const UINT copyW = srcBox ? (UINT)(srcBox->right - srcBox->left) : (UINT)sd.Width;
        const UINT copyH = srcBox ? (UINT)(srcBox->bottom - srcBox->top) : (UINT)sd.Height;
        if (od.Width == dd.Width && od.Height == dd.Height &&
            od.Format == dd.Format && sd.Width == dd.Width && sd.Height == dd.Height &&
            copyW == (UINT)dd.Width && copyH == (UINT)dd.Height &&
            g_b2FenceOutG->GetCompletedValue() >= pending &&
            InterlockedCompareExchange(&g_b2DeferredPending, 0, 1) == 1) {
            D3D12_TEXTURE_COPY_LOCATION replacement = { g_b2OutG, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_b2OutG;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (Real_ResourceBarrier) Real_ResourceBarrier(list, 1, &b);
            if (Real_CopyTextureRegion) Real_CopyTextureRegion(list, dst, dstX, dstY, dstZ, &replacement, srcBox);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            if (Real_ResourceBarrier) Real_ResourceBarrier(list, 1, &b);
            static unsigned s_deferredCopies = 0;
            if (++s_deferredCopies <= 5 || (s_deferredCopies % 600) == 0)
                Log("hooks: deferred DLSS output copied in engine list frame=%llu count=%u",
                    (unsigned long long)pending, s_deferredCopies);
            return;
        }
    }
    // BeamNG may rotate or wrap the presentation resource between its engine
    // copy and Present, so exact pointer identity is not always observable in
    // this hook. Permit a tightly constrained full-frame candidate instead:
    // matching active display dimensions/format, zero offset, complete source
    // extent, and a completed helper fence. Never replace copies involving our
    // bridge resources themselves.
    if (g_b2DeferredOutput && g_b2DeferredPending && list && dst && src &&
        dst->pResource && src->pResource &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst->SubresourceIndex == 0 && src->SubresourceIndex == 0 &&
        dstX == 0 && dstY == 0 && dstZ == 0 &&
        dst->pResource != g_b2ColorG && dst->pResource != g_b2OutG &&
        src->pResource != g_b2ColorG && src->pResource != g_b2OutG &&
        g_b2OutG && g_b2FenceOutG) {
        D3D12_RESOURCE_DESC od = g_b2OutG->GetDesc();
        D3D12_RESOURCE_DESC dd = dst->pResource->GetDesc();
        D3D12_RESOURCE_DESC sd = src->pResource->GetDesc();
        const UINT copyW = srcBox ? (UINT)(srcBox->right - srcBox->left) : (UINT)sd.Width;
        const UINT copyH = srcBox ? (UINT)(srcBox->bottom - srcBox->top) : (UINT)sd.Height;
        const UINT64 pending = g_b2DeferredVal;
        const bool fullFrame = dd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            dd.Width == od.Width && dd.Height == od.Height &&
            sd.Width == dd.Width && sd.Height == dd.Height &&
            dd.Format == od.Format && sd.Format == dd.Format &&
            copyW == (UINT)dd.Width && copyH == (UINT)dd.Height &&
            (g_b2W == 0 || dd.Width == g_b2W) && (g_b2H == 0 || dd.Height == g_b2H) &&
            g_b2FenceOutG->GetCompletedValue() >= pending;
        if (fullFrame && InterlockedCompareExchange(&g_b2DeferredPending, 0, 1) == 1) {
            D3D12_TEXTURE_COPY_LOCATION replacement = { g_b2OutG, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_b2OutG;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (Real_ResourceBarrier) Real_ResourceBarrier(list, 1, &b);
            if (Real_CopyTextureRegion) Real_CopyTextureRegion(list, dst, dstX, dstY, dstZ, &replacement, srcBox);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            if (Real_ResourceBarrier) Real_ResourceBarrier(list, 1, &b);
            static unsigned s_candidateCopies = 0;
            if (++s_candidateCopies <= 8 || (s_candidateCopies % 600) == 0)
                Log("hooks: deferred DLSS candidate copied frame=%llu count=%u dst=%p src=%p fmt=%u size=%ux%u",
                    (unsigned long long)pending, s_candidateCopies,
                    (void*)dst->pResource, (void*)src->pResource,
                    (unsigned)dd.Format, (unsigned)dd.Width, (unsigned)dd.Height);
            return;
        }
    }
// The deferred result is only substituted when the destination/source
    // shape is proven safe. Record throttled near-misses so the next run can
    // identify which BeamNG copy condition differs, without modifying the
    // native command list.
    if (g_b2DeferredOutput && g_b2DeferredPending && dst && src &&
        dst->pResource && dst->pResource == g_b2PresentBb) {
        static unsigned s_deferredNearMiss = 0;
        if ((++s_deferredNearMiss % 300) == 1) {
            UINT64 completed = g_b2FenceOutG ? g_b2FenceOutG->GetCompletedValue() : 0;
            Log("hooks: deferred output near-miss dstType=%u srcType=%u dstSub=%u srcSub=%u xyz=%u/%u/%u pending=%llu completed=%llu src=%p dst=%p",
                (unsigned)dst->Type, (unsigned)src->Type,
                (unsigned)dst->SubresourceIndex, (unsigned)src->SubresourceIndex,
                (unsigned)dstX, (unsigned)dstY, (unsigned)dstZ,
                (unsigned long long)g_b2DeferredVal,
                (unsigned long long)completed,
                (void*)src->pResource, (void*)dst->pResource);
        }
    }
    // Broad diagnostics: log ALL full-frame copies when deferred output is
    // pending, to correlate with the engine's actual presentation copy chain.
    if (g_b2DeferredOutput && g_b2DeferredPending && list && dst && src &&
        dst->pResource && src->pResource &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst->SubresourceIndex == 0 && src->SubresourceIndex == 0 &&
        dstX == 0 && dstY == 0 && dstZ == 0) {
        D3D12_RESOURCE_DESC dd = dst->pResource->GetDesc();
        D3D12_RESOURCE_DESC sd = src->pResource->GetDesc();
        const UINT copyW = srcBox ? (UINT)(srcBox->right - srcBox->left) : (UINT)sd.Width;
        const UINT copyH = srcBox ? (UINT)(srcBox->bottom - srcBox->top) : (UINT)sd.Height;
        const bool isFullFrame = dd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            copyW == (UINT)dd.Width && copyH == (UINT)dd.Height &&
            sd.Width == dd.Width && sd.Height == dd.Height;
        if (isFullFrame) {
            static unsigned s_broadDiag = 0;
            if ((++s_broadDiag % 60) == 1) {
                UINT64 completed = g_b2FenceOutG ? g_b2FenceOutG->GetCompletedValue() : 0;
                D3D12_RESOURCE_DESC od = g_b2OutG ? g_b2OutG->GetDesc() : D3D12_RESOURCE_DESC{};
                Log("hooks: deferred diag full-frame copy dst=%p(%ux%u fmt=%u) src=%p(%ux%u fmt=%u) pending=%llu fence=%llu od=%ux%u fmt=%u",
                    (void*)dst->pResource, (unsigned)dd.Width, (unsigned)dd.Height, (unsigned)dd.Format,
                    (void*)src->pResource, (unsigned)sd.Width, (unsigned)sd.Height, (unsigned)sd.Format,
                    (unsigned long long)g_b2DeferredVal,
                    (unsigned long long)completed,
                    (unsigned)od.Width, (unsigned)od.Height, (unsigned)od.Format);
            }
        }
    }
    // Diagnostic: log deferred pending state at every copy to understand timing
    if (g_b2DeferredOutput && list && dst && src && dst->pResource && src->pResource) {
        static unsigned s_pendingDiag = 0;
        if ((++s_pendingDiag % 120) == 1) {
            Log("hooks: deferred pending state=%d pendingVal=%llu fenceDone=%llu",
                (int)g_b2DeferredPending, (unsigned long long)g_b2DeferredVal,
                (unsigned long long)(g_b2FenceOutG ? g_b2FenceOutG->GetCompletedValue() : 0));
        }
    }
    // LEGACY-COMPAT GUARD: bridge analysis needs the bridge; the legacy
    // (!dlaa) trigger/scene/depth analysis below needs no bridge (the
    // legacy trigger at PRIMARY TRIGGER explicitly requires !g_bridgeReady).
    // Old guard "if (!g_bridgeReady || !g_dlaaMode) return" made the legacy
    // trigger statically dead. DLAA-without-bridge still skips (as before).
    if (g_dlaaMode && !g_bridgeReady) return;
    // SEH helper kept out-of-line so CopyTexBody can own C++ objects.
    struct Local {
        static bool AltIsPairHalf(ID3D12Resource* alt) {
            __try {
                D3D12_RESOURCE_DESC ad = alt->GetDesc();
                return ad.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    };
    bool inject = false;
    bool injectBefore = false;
    if (dst && src && src->pResource != dst->pResource &&
        src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst->SubresourceIndex == 0 && src->SubresourceIndex == 0 &&
        srcBox && dstX == 0 && dstY == 0) {
        long w = srcBox->right - srcBox->left;
        long h = srcBox->bottom - srcBox->top;
        AcquireSRWLockExclusive(&g_copyMapLock);
        bool newNode = (++g_copySrcCount[(void*)src->pResource] == 1);
        ReleaseSRWLockExclusive(&g_copyMapLock);
        if (newNode) {
            g_lastNewChainFrame = g_frameCounter; // new node entered the chain
            g_chainObserved = true;
            // Shadow stamp: same event, Present-clock units. No gate effect.
            InterlockedExchange64(&g_lastNewChainPresent,
                InterlockedCompareExchange64(&g_presentSerial, 0, 0));
            InterlockedExchange(&g_chainPresentObserved, 1);
        }
        bool isMvDst = (dst->pResource == g_mvResource || dst->pResource == g_mvResourceAlt);
        bool isSceneSrc = (src->pResource == g_sceneColor ||
                           (g_sceneColorAlt && src->pResource == g_sceneColorAlt));
        if (g_displayW > 0 && !g_injectedThisFrame &&
            (unsigned long)w == g_displayW && (unsigned long)h == g_displayH) {
            D3D12_RESOURCE_DESC sd = src->pResource->GetDesc();
            // Evidence instrumentation (reviewer #21 style): the engine rotates
            // its scene target ACROSS FORMATS (28->34 observed pre-crash).
            // A display-sized src we do NOT track means rotation happened and
            // our bridge color (fixed fmt) is now stale. Log once per format.
            static unsigned s_lastUntrackedFmt = 0;
            // Topology mapping (one-shot, 20s window): log copy PAIRS to find
            // the terminal scene node that actually reaches Present.
            static ULONGLONG s_topoStart = 0;
            static int s_topoLogs = 0;
            if (s_topoLogs < 400) {
                ULONGLONG tnow = GetTickCount64();
                if (!s_topoStart) s_topoStart = tnow;
                if (tnow - s_topoStart < 20000) {
                    s_topoLogs++;
                    D3D12_RESOURCE_DESC dd = dst->pResource ? dst->pResource->GetDesc() : sd;
                    Log("topo: %ux%u f%u -> f%u (src %p dst %p)",
                        (unsigned)sd.Width, (unsigned)sd.Height, (unsigned)sd.Format,
                        (unsigned)dd.Format, (void*)src->pResource, (void*)dst->pResource);
                }
            }
            g_topoLastSrc = src->pResource;
            g_topoLastFmt = (unsigned)sd.Format;
            // TERMINAL PAIR ADOPTION: f10->f10 display-sized copies are the
            // final ping-pong stage whose output feeds Present (proven by
            // present-feed correlation). Track BOTH halves so the alternating
            // last-written node is always a known, stamped resource.
            if (!isSceneSrc &&
                sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                dst->pResource && dst->pResource != src->pResource) {
                D3D12_RESOURCE_DESC dd2 = dst->pResource->GetDesc();
                if (dd2.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                    (unsigned)dd2.Width == g_displayW && dd2.Height == g_displayH &&
                    (unsigned)sd.Width == g_displayW && sd.Height == g_displayH) {
                    bool srcIsTracked = (src->pResource == g_sceneColor || src->pResource == g_sceneColorAlt);
                    bool dstIsTracked = (dst->pResource == g_sceneColor || dst->pResource == g_sceneColorAlt);
                    // ALT slot may hold a stale non-pair texture from earlier
                    // adoptions; a genuine f10 pair node has priority. Replace
                    // it unless it already IS a pair half.
                    bool altIsPairHalf = false;
                    if (g_sceneColorAlt) {
                        // Weak pointer: may be freed since adoption. A fault
                        // here means dead ALT - clear it for replacement.
                        if (!Local::AltIsPairHalf(g_sceneColorAlt)) {
                            g_sceneColorAlt = nullptr;
                            SetTrackedResourceGeneration(&g_sceneColorAlt, nullptr);
                        }
                    }
                    if (!srcIsTracked && !g_sceneColorAlt) {
                        // Metadata only on a real store (self-adopt guard).
                        if (StoreTracked(&g_sceneColorAlt, src->pResource)) {
                        { BookGuard _bgState; g_resourceStates[g_sceneColorAlt] = D3D12_RESOURCE_STATE_COMMON; }
                        Log("hooks: terminal pair node adopted as ALT %p (f10)", (void*)src->pResource);
                        }
                    } else if (!dstIsTracked && !g_sceneColorAlt) {
                        if (StoreTracked(&g_sceneColorAlt, dst->pResource)) {
                        { BookGuard _bgState; g_resourceStates[g_sceneColorAlt] = D3D12_RESOURCE_STATE_COMMON; }
                        Log("hooks: terminal pair node adopted as ALT %p (f10 dst)", (void*)dst->pResource);
                        }
                    } else if (!altIsPairHalf && g_sceneColorAlt &&
                               g_sceneColorAlt != src->pResource && g_sceneColorAlt != dst->pResource) {
                        ID3D12Resource* oldAlt = g_sceneColorAlt;
                        (void)oldAlt;
                        ID3D12Resource* cand = srcIsTracked ? dst->pResource : src->pResource;
                        if (StoreTracked(&g_sceneColorAlt, cand)) {
                        { BookGuard _bgState; g_resourceStates[g_sceneColorAlt] = D3D12_RESOURCE_STATE_COMMON; }
                        Log("hooks: terminal pair REPLACED non-pair ALT -> %p (f10)", (void*)cand);
                        }
                    }
                }
            }
            if (!isSceneSrc && sd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
                sd.Format != s_lastUntrackedFmt) {
                s_lastUntrackedFmt = sd.Format;
                Log("hooks: UNTRACKED display-sized copy src fmt %u %ux%u - scene rotated off bridge fmt?",
                    (unsigned int)sd.Format, (unsigned int)sd.Width, (unsigned int)sd.Height);
            }
            // Post-reload fallback: if the scene color was never discovered
            // (plugin re-init after the game created its render targets), the
            // engine still copies the scene color at full-res every frame.
            // A display-sized scene-format src here is the scene color - adopt it.
            if (!g_sceneColorValid && !isMvDst) {
                // Guarded desc read: src is a weak engine pointer (same class
                // as the old copy-depth unguarded GetDesc). No early return:
                // the engine copy below must always be forwarded.
                D3D12_RESOURCE_DESC sd = {};
                if (src->pResource && SafeGetDesc(src->pResource, &sd) &&
                    sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    sd.MipLevels == 1 &&
                    IsSceneColorFormat((unsigned)sd.Format)) {
                    // Metadata only on a real store (self-adopt guard).
                    if (!StoreTracked(&g_sceneColor, src->pResource)) { /* keep prior */ }
                    else {
                    g_sceneColorValid = true;
                    { BookGuard _bgState; g_resourceStates[g_sceneColor] = D3D12_RESOURCE_STATE_COPY_SOURCE; }
                    AdoptDisplaySize((unsigned int)sd.Width, (unsigned int)sd.Height);
                    Log("hooks: scene color adopted from copy source %p", (void*)g_sceneColor);
                    }
                }
            }
            isSceneSrc = (src->pResource == g_sceneColor ||
                          (g_sceneColorAlt && src->pResource == g_sceneColorAlt) ||
                          SceneSetContains(src->pResource));
            // Copy-endpoint recency for rotation tracking (members only).
            SceneSetTouch(src->pResource);
            SceneSetTouch(dst->pResource);
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
            // PRIMARY TRIGGER - LEGACY PATH ONLY.
            // In DLAA/bridge mode the Present-time bridge flow owns all NGX
            // work; recording evaluate into the ENGINE's list here crosses
            // devices (NGX feature lives on the bridge) and faults the GPU.
            if (!g_dlaaMode && isSceneSrc && dst->pResource != g_dlssOut &&
                (g_patchViewport) && g_mvValid &&
                (g_patchAppliedThisFrame) && g_depthValid) {
                if (g_upscaler && g_upscaler->IsReady() && !g_bridgeReady) {
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
            // Gated two ways: (1) SRV-sourced true depth is never overwritten
            // by copy guesswork; (2) only depth-family RESOURCE formats adopt
            // (velocity fmt-34 and LDR junk proved the unfiltered heuristic
            // pollutes the slot). Guarded desc read: the DST pointer is weak.
            bool quietNow = ((int)(g_frameCounter - g_quietUntilFrame) < 0);
            if (!quietNow && !isSceneSrc && !isMvDst && dst->pResource != g_dlssOut) {
                D3D12_RESOURCE_DESC ddg = {};
                bool ddOk = dst->pResource && SafeGetDesc(dst->pResource, &ddg);
                if (!ddOk || !IsDepthFamilyFormat((unsigned)ddg.Format)) {
                    static int s_depthRejects = 0;
                    if (++s_depthRejects <= 4)
                        Log("hooks: depth copy rejected (fmt=%u %ux%u srvDepth=%d)",
                            ddOk ? (unsigned)ddg.Format : 0,
                            ddOk ? (unsigned)ddg.Width : 0, ddOk ? (unsigned)ddg.Height : 0,
                            g_depthSrvSourced ? 1 : 0);
                } else if (g_depthSrvSourced && g_depthResource && g_depthResource != dst->pResource) {
                    static int s_depthKeeps = 0;
                    if (++s_depthKeeps <= 2)
                        Log("hooks: depth copy skipped - SRV depth kept %p", (void*)g_depthResource);
                } else {
                if (!g_depthValid) g_depthFirstValidFrame = g_frameCounter;
                // Metadata only on a real store (self-adopt guard).
                if (!StoreTracked(&g_depthResource, dst->pResource)) { /* keep prior depth metadata */ }
                else {
                g_depthValid = true;
                g_depthStamp = g_frameCounter;
                {
                    g_depthRealFmt = ddg.Format;
                    g_depthMsaa = ddg.SampleDesc.Count != 1;
                }
                static int s_depthCandidates = 0;
                if (s_depthCandidates < 8) {
                    ++s_depthCandidates;
                    Log("hooks: depth candidate %p (full-res copy)", (void*)dst->pResource);
                }
                }
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
}

void Hook_CopyTextureRegion(ID3D12GraphicsCommandList* list,
                            const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY,
                            UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src,
                            const D3D12_BOX* srcBox)
{
    __try {
        CopyTexBody(list, dst, dstX, dstY, dstZ, src, srcBox);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("hooks: CopyTextureRegion guarded (code %08X)", (unsigned)GetExceptionCode());
        if (Real_CopyTextureRegion)
            Real_CopyTextureRegion(list, dst, dstX, dstY, dstZ, src, srcBox);
    }
}

bool Hook_RSSetViewports(ID3D12GraphicsCommandList* list, UINT numViewports,
                          const D3D12_VIEWPORT* pViewports, PFN_RSSetViewports realFn)
{
    // LEGACY-COMPAT: the viewport patch below is the legacy (!dlaa) render-
    // scale mechanism (TECHNICAL_REFERENCE 7.2.1) and needs no bridge; its
    // inner blocks are already gated on g_patchViewport (false in DLAA
    // mode), so DLAA behavior is unchanged (passthrough either way).
    // Old guard "bridgeReady && dlaa" made g_patchAppliedThisFrame
    // permanently false under the shipped dlaa=0 config.
    if (g_dlaaMode && !g_bridgeReady) {
        if (realFn) realFn(list, numViewports, pViewports);
        return true;
    }
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
        } else {
            ++s_vpDiag;
            // Bound-identity correlation (the first downstream gate): when the
            // bound scene is null, record WHICH resource was bound, its live
            // descriptor, and whether the set knows it. Distinguishes an
            // unobserved target (pre-hook creation / unknown handle) from a
            // non-scene target from a set-lookup bug. Same cadence as the
            // scene branch so gameplay viewports are captured, not just the
            // first 30 loading-phase ones.
            if (s_vpDiag <= 40 || (s_vpDiag % 500) == 0) {
                D3D12_RESOURCE_DESC bd = {};
                bool gotBd = (g_boundRtvResource && SafeGetDesc(g_boundRtvResource, &bd));
                unsigned setCount = 0;
                { AcquireSRWLockShared(&g_sceneSetLock); setCount = g_sceneSetCount; ReleaseSRWLockShared(&g_sceneSetLock); }
                Log("hooks: vp diag %dx%d boundScene=%p ready=%d mvValid=%d rtvValid=%d bound=%p bdesc=%ux%u fmt=%u inset=%d setcount=%u",
                    (int)pViewports[0].Width, (int)pViewports[0].Height, (void*)boundScene,
                    (g_upscaler && g_upscaler->IsReady()) ? 1 : 0, g_mvValid ? 1 : 0,
                    g_boundRtvValid ? 1 : 0, (void*)g_boundRtvResource,
                    gotBd ? (unsigned)bd.Width : 0u, gotBd ? (unsigned)bd.Height : 0u,
                    gotBd ? (unsigned)bd.Format : 0u,
                    SceneSetContains(g_boundRtvResource) ? 1 : 0, setCount);
            }
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
        if (realFn) realFn(list, 1, &v);
        g_activeSceneColor = boundScene;
        g_patchAppliedThisFrame = true;
        Log("hooks: viewport patched to %ux%u (scene %p)", g_renderW, g_renderH, (void*)boundScene);
        ++g_patchFramesWithoutInject;
        if (g_patchFramesWithoutInject > 1800) {
            g_patchAborted = true;
            g_patchViewport = false;
            Log("hooks: viewport patch aborted - no injection within %u frames", g_patchFramesWithoutInject);
        }
        return true;
    }
    if (realFn) realFn(list, numViewports, pViewports);
    return true;
}

bool Hook_RSSetScissorRects(ID3D12GraphicsCommandList* list, UINT numRects,
                             const D3D12_RECT* pRects, PFN_RSSetScissorRects realFn)
{
    if (numRects >= 1 && pRects && g_patchViewport && SceneColorBound() &&
        (pRects[0].right - pRects[0].left) == (LONG)g_displayW &&
        (pRects[0].bottom - pRects[0].top) == (LONG)g_displayH) {
        D3D12_RECT r = pRects[0];
        r.right = r.left + (LONG)g_renderW;
        r.bottom = r.top + (LONG)g_renderH;
        if (realFn) realFn(list, 1, &r);
        return true;
    }
    if (realFn) realFn(list, numRects, pRects);
    return true;
}

// Pure-observation state tracking extracted for the live shim path.
// No bridge dependency (map writes only); called from Shim_ResourceBarrier
// on every invocation and from Hook_ResourceBarrier below.
static void TrackResourceBarriers(UINT numBarriers, const D3D12_RESOURCE_BARRIER* pBarriers)
{
    if (pBarriers && numBarriers > 0) {
        for (UINT i = 0; i < numBarriers; ++i) {
            if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                ID3D12Resource* res = pBarriers[i].Transition.pResource;
                if (res) {
                    { BookGuard _bgRb; g_resourceStates[res] = pBarriers[i].Transition.StateAfter; }
                    // Observation recency only: a recorded transition is not
                    // proof the list executed, nor that a draw wrote this
                    // resource. Never interpret these stamps as same-frame data.
                    if (res == g_depthResource)
                        g_depthStamp = g_frameCounter;
                    if (res == g_mvResource || res == g_mvResourceAlt)
                        g_mvStamp = g_frameCounter;
                    // Present serial is only the time at which this hook saw
                    // the recorded barrier; the command list may be replayed.
                    if (res == g_depthResource)
                    {
                        g_depthLastTouchPresent = SceneSetNow();
                        InterlockedIncrement64(&g_diagDepthBar);
                    }
                    if (res == g_depthResource)
                        NoteDepthTouchIdentity(res, TOUCH_SOURCE_BARRIER);
                    if (res == g_mvResource || res == g_mvResourceAlt)
                    {
                        g_mvLastTouchPresent = SceneSetNow();
                        InterlockedIncrement64(&g_diagMvBar);
                    }
                    if (res == g_mvResource || res == g_mvResourceAlt)
                        NoteMvTouchIdentity(res, TOUCH_SOURCE_BARRIER);
                    else {
                        // Broader MV touch: engines using implicit COMMON-state
                        // transitions never call ResourceBarrier on the MV texture,
                        // so the pointer-match check above never fires. Catch any
                        // display-sized R16G16F resource transition as a touch.
                        D3D12_RESOURCE_DESC brd = {};
                        if (res && SafeGetDesc(res, &brd) &&
                            brd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                            (unsigned)brd.Width == g_displayW && brd.Height == g_displayH &&
                            !IsOwnResource(res)) {
                            if (brd.Format == DXGI_FORMAT_R16G16_FLOAT) {
                                if (res != g_mvResource && res != g_mvResourceAlt) {
                                    if (StoreTracked(&g_mvResourceAlt, res)) {
                                        g_mvStamp = g_frameCounter;
                                        if (!g_mvFirstValidFrame) g_mvFirstValidFrame = g_frameCounter;
                                        g_mvW = (unsigned int)brd.Width; g_mvH = (unsigned int)brd.Height;
                                        static int s_broadMvLog = 0;
                                        if (s_broadMvLog++ < 4)
                                            Log("hooks: MV broad-adopt on barrier %p (%ux%u)",
                                                (void*)res, (unsigned)brd.Width, (unsigned)brd.Height);
                                    }
                                }
                                g_mvLastTouchPresent = SceneSetNow();
                                InterlockedIncrement64(&g_diagMvBar);
                                NoteMvTouchIdentity(res, TOUCH_SOURCE_BROAD_BARRIER);
                                g_mvStamp = g_frameCounter;
                            }
                            if (IsDepthFamilyFormat((unsigned)brd.Format)) {
                                if (res != g_depthResource) {
                                    if (StoreTracked(&g_depthResource, res)) {
                                        g_depthStamp = g_frameCounter;
                                        g_depthValid = true;
                                        g_depthRealFmt = brd.Format;
                                        static int s_broadDepLog = 0;
                                        if (s_broadDepLog++ < 4)
                                            Log("hooks: depth broad-adopt on barrier %p (%ux%u fmt=%u)",
                                                (void*)res, (unsigned)brd.Width, (unsigned)brd.Height, (unsigned)brd.Format);
                                    }
                                }
                                g_depthLastTouchPresent = SceneSetNow();
                                InterlockedIncrement64(&g_diagDepthBar);
                                NoteDepthTouchIdentity(res, TOUCH_SOURCE_BROAD_BARRIER);
                                g_depthStamp = g_frameCounter;
                            }
                        }
                    }
                }
            }
        }
    }
}

void Hook_ResourceBarrier(ID3D12GraphicsCommandList* list, UINT numBarriers,
                          const D3D12_RESOURCE_BARRIER* pBarriers)
{
    // LEGACY-COMPAT: state tracking is a pure map write with no bridge
    // dependency; DoInjection's Barrier() calls need it in legacy (!dlaa)
    // mode (without it every barrier silently no-ops). DLAA-without-bridge
    // still skips (as before); DLAA-with-bridge still tracks (as before).
    if (g_dlaaMode && !g_bridgeReady) {
        if (Real_ResourceBarrier) Real_ResourceBarrier(list, numBarriers, pBarriers);
        return;
    }
    TrackResourceBarriers(numBarriers, pBarriers);
    if (Real_ResourceBarrier) Real_ResourceBarrier(list, numBarriers, pBarriers);
}

void Hook_SetDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT numHeaps,
                             ID3D12DescriptorHeap* const* heaps)
{
    // PASSTHROUGH: heap save/restore causes solid-color artifacts when the
    // saved state from one command list is restored onto another.
    Real_SetDescriptorHeaps(list, numHeaps, heaps);
}

// Pure-observation bind tracking extracted for the live shim path.
// SceneColorBound() (the viewport-patch prerequisite) has no other writer,
// so without this the patch flag can never be set. No bridge-resource
// dependency (map/adoption writes only); called from Shim_OMSetRenderTargets
// on every invocation and from Hook_OMSetRenderTargets below.
// Newly-live GetDesc calls use the guarded form (previously ran only under
// the bridge-era gate, now reachable on the record path).
static ID3D12Resource* TrackOMBind(ID3D12GraphicsCommandList* list, UINT numRenderTargets,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargets,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencil)
{
    if (numRenderTargets >= 1 && pRenderTargets) {
        g_boundRtv = pRenderTargets[0];
        g_boundRtvValid = true;
        g_boundRtvResource = nullptr;
        BookGuard _bgOmrt;
        auto it = g_rtvMap.find(pRenderTargets[0].ptr);
        if (it != g_rtvMap.end()) {
            g_boundRtvResource = it->second;
            // BIND SURVEY (measurement-only, format-agnostic): which display-sized
            // targets does the engine actually bind as RTV during gameplay, in any
            // format. Existing adoption below is UNORM-only, so HDR (fmt-10) or LDR
            // (fmt-28) binds would otherwise pass unseen. Read-only: no adoption,
            // no map writes, no gating effect. Same guarded-GetDesc pattern and
            // lock scope as the surrounding code. Budget is per (w,h,fmt) class
            // (first 3 each, 32 classes max) so loading-phase binds cannot consume
            // the gameplay window's quota. Class counters are plain statics like
            // the other log-budget counters (racy-benign: at most a duplicate
            // line, never a behavior change).
            {
                struct BindClass { unsigned w; unsigned h; unsigned fmt; unsigned logged; };
                static BindClass s_bindClasses[32] = {};
                static unsigned s_bindClassN = 0;
                D3D12_RESOURCE_DESC brd = {};
                if (g_boundRtvResource && SafeGetDesc(g_boundRtvResource, &brd) &&
                    brd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    brd.Width >= 1000 && brd.Height >= 500 && brd.MipLevels == 1) {
                    unsigned cw = (unsigned)brd.Width, ch = (unsigned)brd.Height, cf = (unsigned)brd.Format;
                    BindClass* cls = nullptr;
                    for (unsigned i = 0; i < s_bindClassN && i < 32; ++i) {
                        if (s_bindClasses[i].w == cw && s_bindClasses[i].h == ch && s_bindClasses[i].fmt == cf) {
                            cls = &s_bindClasses[i];
                            break;
                        }
                    }
                    if (!cls && s_bindClassN < 32) {
                        cls = &s_bindClasses[s_bindClassN++];
                        cls->w = cw; cls->h = ch; cls->fmt = cf; cls->logged = 0;
                    }
                    if (cls && cls->logged < 3) {
                        ++cls->logged;
                        Log("hooks: bind-survey %ux%u fmt=%u res=%p handle=%llX scene=%d present=%llu ecl=%llu",
                            cw, ch, cf,
                            (void*)g_boundRtvResource, (unsigned long long)pRenderTargets[0].ptr,
                            SceneSetContains(g_boundRtvResource) ? 1 : 0,
                            (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
                            (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
                    }
                }
            }
            // Dynamic adoption: if a display-sized scene-format target gets bound as
            // an RTV and we have never seen it as the scene color, remember it.
            // Covers renderer re-inits that re-create views after our hook
            // (or even the whole plugin) was installed.
            if (!g_sceneColorValid && g_boundRtvResource) {
                D3D12_RESOURCE_DESC rd = {};
                if (SafeGetDesc(g_boundRtvResource, &rd) && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1 &&
                    IsSceneColorFormat((unsigned)rd.Format)) {
                    StoreTracked(&g_sceneColor, g_boundRtvResource);
                    g_sceneColorRtv = pRenderTargets[0];
                    g_sceneColorValid = true;
                    AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
                    Log("hooks: scene color adopted from RTV bind %p (%ux%u)", (void*)g_sceneColor,
                        (unsigned int)rd.Width, (unsigned int)rd.Height);
                    SceneSetNote(g_sceneColor, (unsigned int)rd.Width, (unsigned int)rd.Height,
                                 (unsigned)rd.Format);
                }
            }
            // SET NOTATION ON BIND (no adoption change): a display-sized
            // scene-format target bound as RTV joins the rotation set even when
            // slots are already valid (verified live: gameplay fmt-10 binds
            // with setcount=8/inset=0 because creation-time views predated the
            // filter). Membership lets SceneColorBound/trigger resolve the
            // live target; classification slots are untouched.
            if (g_boundRtvResource && !SceneSetContains(g_boundRtvResource)) {
                D3D12_RESOURCE_DESC nrd = {};
                if (SafeGetDesc(g_boundRtvResource, &nrd) &&
                    nrd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    nrd.Width >= 1000 && nrd.Height >= 500 && nrd.MipLevels == 1 &&
                    IsSceneColorFormat((unsigned)nrd.Format))
                    SceneSetNote(g_boundRtvResource, (unsigned)nrd.Width,
                                 (unsigned)nrd.Height, (unsigned)nrd.Format);
            }
            // MV TRACK-BY-BIND: the engine binds MV as an RTV every frame it
            // renders it. Adopting here always holds the CURRENT texture -
            // immune to the rotation that caused repeated stale-MV faults.
            if (g_boundRtvResource && g_loadPhase == 0 &&
                g_boundRtvResource != g_mvResource && g_boundRtvResource != g_mvResourceAlt) {
                BookGuard _bgOmrt2;
            auto ri = g_rtvMap.find(pRenderTargets[0].ptr);
                if (ri != g_rtvMap.end() && ri->second == g_boundRtvResource) {
                    D3D12_RESOURCE_DESC mrd = {};
                    if (SafeGetDesc(g_boundRtvResource, &mrd) && mrd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                        mrd.MipLevels == 1 && mrd.SampleDesc.Count == 1 &&
                        (unsigned)mrd.Width == g_displayW && mrd.Height == g_displayH &&
                        mrd.Format == DXGI_FORMAT_R16G16_FLOAT) {
                        bool first = !g_mvValid;
                        // Metadata only on a real store (self-adopt guard).
                        // No log on this path by design (per-frame bind frequency).
                        if (!StoreTracked(&g_mvResource, g_boundRtvResource)) { /* keep prior MV metadata */ }
                        else {
                        // A current RTV mapping was resolved from this bind;
                        // that concrete view observation refreshes the token.
                        SetTrackedResourceGeneration(&g_mvResource, g_boundRtvResource);
                        g_mvValid = true;
                        g_mvStamp = g_frameCounter;
                        g_mvLastTouchPresent = SceneSetNow();
                        InterlockedIncrement64(&g_diagMvOm);
                        NoteMvTouchIdentity(g_boundRtvResource, TOUCH_SOURCE_OM_BIND);
                        if (first) g_mvFirstValidFrame = g_frameCounter;
                        }
                    }
                }
            }
            // Same-list bind observation for an already-known MV. This is
            // useful for candidate recency, but does NOT prove a draw occurred
            // or that a replayed command list belongs to the current Present.
            if (g_boundRtvResource == g_mvResource || g_boundRtvResource == g_mvResourceAlt) {
                bool currentViewMapping = false;
                {
                    BookGuard _bgCurrentView;
                    auto currentView = g_rtvMap.find(g_boundRtv.ptr);
                    currentViewMapping = currentView != g_rtvMap.end() &&
                                         currentView->second == g_boundRtvResource;
                }
                if (currentViewMapping) {
                    if (g_boundRtvResource == g_mvResource)
                        SetTrackedResourceGeneration(&g_mvResource, g_boundRtvResource);
                    else
                        SetTrackedResourceGeneration(&g_mvResourceAlt, g_boundRtvResource);
                }
                g_mvLastTouchPresent = SceneSetNow();
                InterlockedIncrement64(&g_diagMvOm);
                NoteMvTouchIdentity(g_boundRtvResource, TOUCH_SOURCE_OM_BIND);
                static volatile LONG64 s_mvOmBindCount = 0;
                LONG64 observed = InterlockedIncrement64(&s_mvOmBindCount);
                if (observed <= 10 || (observed % 500) == 0)
                    Log("hooks: mv-om-bind observed=%llu list=%p res=%p rtv=%llX present=%llu ecl=%llu",
                        (unsigned long long)observed, (void*)list, (void*)g_boundRtvResource,
                        (unsigned long long)pRenderTargets[0].ptr,
                        (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
                        (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
            }
            // The engine re-creates its scene target from time to time but keeps
            // reusing the same CPU descriptor slot. Refresh the tracked scene
            // color to the CURRENT resource bound at that slot, otherwise we
            // keep injecting with a stale (freed/recycled) resource.
            // Persistence gate: only refresh the scene slot to a resource that
            // has fed the full-res composite copy repeatedly - transient post/
            // bloom targets bound at recycled descriptors would otherwise churn
            // the identity every frame and keep the churn-quarantine armed.
            int persist = 0; { AcquireSRWLockShared(&g_copyMapLock); auto ci = g_copySrcCount.find((void*)g_boundRtvResource); if (ci != g_copySrcCount.end()) persist = ci->second; ReleaseSRWLockShared(&g_copyMapLock); }
            if (g_sceneColorValid && persist >= 40 && g_boundRtvResource &&
                g_boundRtv.ptr == g_sceneColorRtv.ptr &&
                g_boundRtvResource != g_sceneColor) {
                StoreTracked(&g_sceneColor, g_boundRtvResource);
                Log("hooks: scene color refreshed on bind %p", (void*)g_sceneColor);
                D3D12_RESOURCE_DESC rd = {};
                if (SafeGetDesc(g_sceneColor, &rd) && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    rd.Width >= 1000 && rd.Height >= 500)
                    AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
            }
            int persistA = 0; { AcquireSRWLockShared(&g_copyMapLock); auto ci = g_copySrcCount.find((void*)g_boundRtvResource); if (ci != g_copySrcCount.end()) persistA = ci->second; ReleaseSRWLockShared(&g_copyMapLock); }
            if (g_sceneColorAlt && persistA >= 40 && g_boundRtvResource &&
                g_boundRtv.ptr == g_sceneColorRtvAlt.ptr &&
                g_boundRtvResource != g_sceneColorAlt) {
                StoreTracked(&g_sceneColorAlt, g_boundRtvResource);
                Log("hooks: scene color ALT refreshed on bind %p", (void*)g_sceneColorAlt);
                // Scene-target churn = renderer re-init / map load: the old
                // MV/depth pointers are almost certainly dead now. Reset the
                // stamps so injection waits for fresh discoveries instead of
                // dereferencing freed resources.
                g_mvStamp = 0;
                g_depthStamp = 0;
                g_mvLastTouchPresent = 0;
                g_depthLastTouchPresent = 0;
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
    if (pDepthStencil && pDepthStencil->ptr) {
        ID3D12Resource* boundDepth = nullptr;
        {
            BookGuard depthGuard;
            auto depthIt = g_dsvMap.find(pDepthStencil->ptr);
            if (depthIt != g_dsvMap.end()) boundDepth = depthIt->second;
        }
        // A DSV bind is stronger identity evidence than SRV creation, but still
        // does not prove that a depth-writing draw executed in this Present.
        if (boundDepth) {
            const bool selectedDepth = boundDepth == g_depthResource;
            if (selectedDepth) {
                g_depthLastTouchPresent = SceneSetNow();
                InterlockedIncrement64(&g_diagDepthDsv);
                NoteDepthTouchIdentity(boundDepth, TOUCH_SOURCE_DSV_BIND);
            }
            static volatile LONG64 s_depthOmBindCount = 0;
            LONG64 observed = InterlockedIncrement64(&s_depthOmBindCount);
            if (observed <= 10 || (observed % 500) == 0)
            {
                D3D12_RESOURCE_DESC boundDesc = {};
                const bool descOk = SafeGetDesc(boundDepth, &boundDesc);
                Log("hooks: depth-om-bind observed=%llu list=%p res=%p selected=%d selectedRes=%p dsv=%llX descOk=%d size=%ux%u fmt=%u samples=%u present=%llu ecl=%llu",
                    (unsigned long long)observed, (void*)list, (void*)boundDepth,
                    selectedDepth ? 1 : 0, (void*)g_depthResource,
                    (unsigned long long)pDepthStencil->ptr, descOk ? 1 : 0,
                    descOk ? (unsigned)boundDesc.Width : 0,
                    descOk ? (unsigned)boundDesc.Height : 0,
                    descOk ? (unsigned)boundDesc.Format : 0,
                    descOk ? (unsigned)boundDesc.SampleDesc.Count : 0,
                    (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0),
                    (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0));
            }
        }
    }
    // Bind recency for rotation tracking (members only; unknown pointers are
    // ignored here - creation/adoption paths are the entry gates).
    SceneSetTouch(g_boundRtvResource);
    return g_boundRtvResource;
}

void Hook_OMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT numRenderTargets,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargets,
                             BOOL RTsSingleHandleToDescriptorRange,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    // LEGACY-COMPAT (same pattern as the other guards): bind tracking is
    // bridge-independent discovery. DLAA-without-bridge still skips.
    if (g_dlaaMode && !g_bridgeReady) {
        if (Real_OMSetRenderTargets) Real_OMSetRenderTargets(list, numRenderTargets, pRenderTargets,
                                RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
        return;
    }
    TrackOMBind(list, numRenderTargets, pRenderTargets, pDepthStencilDescriptor);
    if (Real_OMSetRenderTargets) Real_OMSetRenderTargets(list, numRenderTargets, pRenderTargets,
                            RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
}

// Device QueryInterface census (diagnostic): which ID3D12Device versions are
// requested on the observed device table. Decides whether Device4+/Device8
// '1'/'2'-variant creation methods (unhooked higher slots) are a live hypothesis
// for resources missing from the slot-27/29/30 logs. Watchlist covers base
// Device (owns the shared-handle open methods) through Device8 (owns
// CommittedResource2/PlacedResource1; Device5-7 add no resource creation).
// Forwards exactly once via the per-table original;
// no extra QIs, no retained references, identical results to the game.
// Exact per-IID counts (atomic) are separate from emitted records (first
// sighting + every 1000th).
static bool SameGUID(REFIID a, const GUID* b)
{
    if (a.Data1 != b->Data1 || a.Data2 != b->Data2 || a.Data3 != b->Data3)
        return false;
    for (int i = 0; i < 8; ++i) {
        if (a.Data4[i] != b->Data4[i]) return false;
    }
    return true;
}
// IIDs verified against 10.0.28000.0 d3d12.h MIDL_INTERFACE declarations.
static const GUID kQIDevice = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const GUID kQIDevice1 = { 0x77acce80, 0x638e, 0x4e65, { 0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e } };
static const GUID kQIDevice2 = { 0x30baa41e, 0xb15b, 0x475c, { 0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28 } };
static const GUID kQIDevice3 = { 0x81dadc15, 0x2bad, 0x4392, { 0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98 } };
static const GUID kQIDevice4 = { 0xe865df17, 0xa9ee, 0x46f9, { 0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5 } };
static const GUID kQIDevice5 = { 0x8b4f173b, 0x2fea, 0x4b80, { 0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d } };
static const GUID kQIDevice6 = { 0xc70b221b, 0x40e4, 0x4a17, { 0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc } };
static const GUID kQIDevice7 = { 0x5c014b53, 0x68a1, 0x4b9b, { 0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b } };
static const GUID kQIDevice8 = { 0x9218e6bb, 0xf944, 0x4f7e, { 0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3 } };
struct QIWatchEntry { const GUID* iid; const char* name; };
static const QIWatchEntry kQIWatch[] = {
    { &kQIDevice, "Device" }, { &kQIDevice1, "Device1" }, { &kQIDevice2, "Device2" },
    { &kQIDevice3, "Device3" }, { &kQIDevice4, "Device4" }, { &kQIDevice5, "Device5" },
    { &kQIDevice6, "Device6" }, { &kQIDevice7, "Device7" }, { &kQIDevice8, "Device8" },
};
static volatile LONG64 g_qiTotal = 0;
static volatile LONG64 g_qiWatchCounts[9] = {};
// Device4 '1'-variant hooks (defined with the other creation hooks above).
HRESULT WINAPI Hook_CreateCommittedResource1(ID3D12Device* device,
    const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, ID3D12ProtectedResourceSession* session,
    REFIID riidRes, void** ppResource);
HRESULT WINAPI Hook_CreateHeap1(ID3D12Device* device,
    const D3D12_HEAP_DESC* heapDesc, ID3D12ProtectedResourceSession* session,
    REFIID riid, void** ppHeap);
HRESULT WINAPI Hook_CreateReservedResource1(ID3D12Device* device,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, ID3D12ProtectedResourceSession* session,
    REFIID riidRes, void** ppResource);
static volatile LONG s_d4HookState = 0;
// Device8 hooks (defined with the other creation hooks above); installed only
// after a successful Device8 QI on the same table (see Shim_DeviceQI).
HRESULT WINAPI Hook_CreateCommittedResource2(ID3D12Device* device,
    const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_FLAGS heapFlags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, ID3D12ProtectedResourceSession* session,
    REFIID riidRes, void** ppResource);
HRESULT WINAPI Hook_CreatePlacedResource1(ID3D12Device* device,
    ID3D12Heap* pHeap, UINT64 heapOffset,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue, REFIID riid, void** ppResource);
static volatile LONG s_d8HookState = 0;
static HRESULT STDMETHODCALLTYPE Shim_DeviceQI(IUnknown* self, REFIID riid, void** ppvObject)
{
    InterlockedIncrement64(&g_qiTotal);
    int idx = -1;
    for (int i = 0; i < 9; ++i) {
        if (SameGUID(riid, kQIWatch[i].iid)) { idx = i; break; }
    }
    LONG64 c = 0;
    if (idx >= 0)
        c = InterlockedIncrement64(&g_qiWatchCounts[idx]);
    HRESULT hr = E_NOINTERFACE;
    void* out = nullptr;
    if (Real_DeviceQI)
        hr = Real_DeviceQI(self, riid, ppvObject);
    else {
        static int s_qiFaultLogs = 0;
        if (++s_qiFaultLogs <= 2)
            Log("hooks: QI census forwarding missing (unreachable path)");
    }
    if (ppvObject) out = *ppvObject;
    if (idx >= 0 && (c == 1 || (c % 1000) == 0))
        Log("hooks: QI census %s #%lld self=%p hr=0x%08X out=%p",
            kQIWatch[idx].name, c, (void*)self, (unsigned)hr, out);
    // Device4 '1'-variant observation: install once, on the SAME table only,
    // after a successful Device4 QI and before returning the interface.
    // A distinct table is left untouched per the existing distinct-vtable rule
    // (state resets so a later same-table QI can still install).
    if (idx == 4 && SUCCEEDED(hr) && ppvObject && *ppvObject) {
        if (InterlockedCompareExchange(&s_d4HookState, 1, 0) == 0) {
            void* d4obj = *ppvObject;
            void** d4vt = nullptr;
            __try { d4vt = *(void***)d4obj; } __except (EXCEPTION_EXECUTE_HANDLER) { d4vt = nullptr; }
            if (d4vt && g_hookedDeviceVtbl && d4vt == (void**)g_hookedDeviceVtbl) {
                MEMORY_BASIC_INFORMATION dmbi = {};
                VirtualQuery(d4vt, &dmbi, sizeof(dmbi));
                DWORD doldProt = 0;
                if (VirtualProtect(dmbi.BaseAddress, dmbi.RegionSize, PAGE_READWRITE, &doldProt)) {
                    if (!Real_CommittedResource1)
                        Real_CommittedResource1 = (PFN_CreateCommittedResource1)d4vt[53];
                    if (!Real_Heap1)
                        Real_Heap1 = (PFN_CreateHeap1)d4vt[54];
                    if (!Real_ReservedResource1)
                        Real_ReservedResource1 = (PFN_CreateReservedResource1)d4vt[55];
                    d4vt[53] = (void*)&Hook_CreateCommittedResource1;
                    d4vt[54] = (void*)&Hook_CreateHeap1;
                    d4vt[55] = (void*)&Hook_CreateReservedResource1;
                    VirtualProtect(dmbi.BaseAddress, dmbi.RegionSize, doldProt, &doldProt);
                    void* d4t[3] = { (void*)&Hook_CreateCommittedResource1,
                                     (void*)&Hook_CreateHeap1,
                                     (void*)&Hook_CreateReservedResource1 };
                    CfgMarkValid(d4t, 3);
                    Log("hooks: Device4 '1'-variant hooks installed (53/54/55) vtbl=%p", (void*)d4vt);
                } else {
                    Log("hooks: Device4 table not writable - '1'-variant hooks skipped");
                    InterlockedExchange(&s_d4HookState, 0);
                }
            } else {
                Log("hooks: Device4 distinct table %p (hooked %p) - '1'-variant hooks skipped (coverage limitation)",
                    (void*)d4vt, g_hookedDeviceVtbl);
                InterlockedExchange(&s_d4HookState, 0);
            }
        }
    }
    // Device8 resource hooks: install once, on the SAME table only, after a
    // successful Device8 QI and before returning the interface. Same
    // distinct-table rule and one-time state discipline as Device4 above.
    // (PlacedResource1 lives on Device8, not Device4 - verified in-header.)
    if (idx == 8 && SUCCEEDED(hr) && ppvObject && *ppvObject) {
        if (InterlockedCompareExchange(&s_d8HookState, 1, 0) == 0) {
            void* d8obj = *ppvObject;
            void** d8vt = nullptr;
            __try { d8vt = *(void***)d8obj; } __except (EXCEPTION_EXECUTE_HANDLER) { d8vt = nullptr; }
            if (d8vt && g_hookedDeviceVtbl && d8vt == (void**)g_hookedDeviceVtbl) {
                MEMORY_BASIC_INFORMATION d8mbi = {};
                VirtualQuery(d8vt, &d8mbi, sizeof(d8mbi));
                DWORD d8oldProt = 0;
                if (VirtualProtect(d8mbi.BaseAddress, d8mbi.RegionSize, PAGE_READWRITE, &d8oldProt)) {
                    if (!Real_CommittedResource2)
                        Real_CommittedResource2 = (PFN_CreateCommittedResource2)d8vt[69];
                    if (!Real_PlacedResource1)
                        Real_PlacedResource1 = (PFN_CreatePlacedResource1)d8vt[70];
                    d8vt[69] = (void*)&Hook_CreateCommittedResource2;
                    d8vt[70] = (void*)&Hook_CreatePlacedResource1;
                    VirtualProtect(d8mbi.BaseAddress, d8mbi.RegionSize, d8oldProt, &d8oldProt);
                    void* d8t[2] = { (void*)&Hook_CreateCommittedResource2,
                                     (void*)&Hook_CreatePlacedResource1 };
                    CfgMarkValid(d8t, 2);
                    Log("hooks: Device8 resource hooks installed (69/70) vtbl=%p", (void*)d8vt);
                } else {
                    Log("hooks: Device8 table not writable - resource hooks skipped");
                    InterlockedExchange(&s_d8HookState, 0);
                }
            } else {
                Log("hooks: Device8 distinct table %p (hooked %p) - resource hooks skipped (coverage limitation)",
                    (void*)d8vt, g_hookedDeviceVtbl);
                InterlockedExchange(&s_d8HookState, 0);
            }
        }
    }
    return hr;
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
    // Once the GAME's device is captured, later creators are INTERNAL
    // libraries (the NGX core calls D3D12CreateDevice during Init). Those
    // must be passthrough too - capturing NGX's internal device as
    // g_device poisoned every game-device assumption and cascaded into
    // DEVICE_REMOVED after each successful pInit.
    if (g_device) {
        static volatile LONG s_skippedDevices = 0;
        LONG n = InterlockedIncrement(&s_skippedDevices);
        if (n <= 5) {
            // Interface census covers every call including early-returns: a newer
            // riid here would implicate post-capture devices directly.
            Log("hooks: D3D12CreateDevice skipped (already have g_device=%p, n=%d riid=%08X:%04X:%04X:%02X%02X%02X%02X%02X%02X%02X%02X)",
                (void*)g_device, (int)n,
                (unsigned)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
                riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
        }
        return Real_D3D12CreateDevice_Tramp(adapter, minLevel, riid, ppDevice);
    }
    if (s_createCalls < 5) {
        ++s_createCalls;
        // Device-interface census (diagnostic): which ID3D12Device version the
        // game requests decides whether Device4+ '1'-variant creation methods
        // (unhooked higher vtable slots) are a live hypothesis for resources
        // missing from the slot-27/29/30 creation logs. Bounded (first 5).
        Log("hooks: D3D12CreateDevice called #%d (g_device=%p riid=%08X:%04X:%04X:%02X%02X%02X%02X%02X%02X%02X%02X)",
            s_createCalls, (void*)g_device,
            (unsigned)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
            riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
            riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
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
            if (dxgidev) {
                // ADAPTER IDENTITY (hybrid-laptop triage): which physical GPU
                // owns the GAME device? If AMD iGPU while our bridge sits on
                // the NVIDIA dGPU, every shared-resource transfer crosses
                // adapters - prime suspect for un-TDR'd DEVICE_REMOVED.
                IDXGIAdapter* ad = nullptr;
                if (SUCCEEDED(dxgidev->GetAdapter(&ad))) {
                    DXGI_ADAPTER_DESC adesc = {};
                    if (SUCCEEDED(ad->GetDesc(&adesc))) {
                        Log("hooks: GAME device adapter VendorId=0x%04X '%ls' LUID=%08X:%08X",
                            adesc.VendorId, adesc.Description,
                            (unsigned)adesc.AdapterLuid.HighPart, (unsigned)adesc.AdapterLuid.LowPart);
                    }
                    ad->Release();
                }
                dxgidev->Release();
            }
        }
        bool reinstallHooks = (g_device != newDev);
        // ALWAYS install RTV/SRV vtable hooks on every game device - the game
        // creates multiple devices and we need to observe RTV creation on all.
        // But only set g_device and install queue/list hooks on the first device.
        bool isFirstDevice = (g_device == nullptr);
        if (isFirstDevice) {
            g_device = newDev;
        }
        // Hook the cold device factory method so we can observe BeamNG's real
        // direct graphics queue. The old code only modified a temporary queue
        // created by ScaleNG, which could never see the game's submissions.
        if (isFirstDevice && InterlockedCompareExchange(&g_realQueueHookInstalled, 0, 0) == 0 &&
            newDev) {
            void** dv = *(void***)newDev;
            void* ccq = dv[8]; // ID3D12Device::CreateCommandQueue
            MH_STATUS qst = MH_CreateHook(ccq, (void*)&Hook_CreateCommandQueue,
                                          (void**)&Real_CreateCommandQueue);
            if (qst == MH_OK || qst == MH_ERROR_ALREADY_CREATED) {
                MH_STATUS qen = MH_EnableHook(ccq);
                Log("hooks: device CreateCommandQueue hook %s target=%p st=%d enable=%d",
                    qen == MH_OK ? "INSTALLED" : "FAILED", ccq, (int)qst, (int)qen);
            } else {
                Log("hooks: device CreateCommandQueue hook create FAILED target=%p st=%d",
                    ccq, (int)qst);
            }
        }
        if (isFirstDevice && InterlockedCompareExchange(&g_realListHookInstalled, 0, 0) == 0 &&
            newDev) {
            void** dv = *(void***)newDev;
            void* ccl = dv[12]; // ID3D12Device::CreateCommandList
            MH_STATUS lst = MH_CreateHook(ccl, (void*)&Hook_CreateCommandList,
                                           (void**)&Real_CreateCommandList);
            if (lst == MH_OK || lst == MH_ERROR_ALREADY_CREATED) {
                MH_STATUS len = MH_EnableHook(ccl);
                Log("hooks: device CreateCommandList hook %s target=%p st=%d enable=%d",
                    len == MH_OK ? "INSTALLED" : "FAILED", ccl, (int)lst, (int)len);
                if (len == MH_OK)
                    InterlockedExchange(&g_realListHookInstalled, 1);
            } else {
                Log("hooks: device CreateCommandList hook create FAILED target=%p st=%d",
                    ccl, (int)lst);
            }
        }
        // Install RTV/SRV/resource-creation hooks. Diagnostic-only forwarding.
        {
            // Cold device-level hooks are observation-only. The
            // hot command-list methods remain untouched below.
            // PURE VTABLE SWAP: never patch driver code bytes. Just redirect
            // the vtable pointer to our hook and save the original for
            // forwarding. The original function code is untouched.
            void** vtbl = *(void***)newDev;

            // Shared-vtable safety: only one distinct device vtable is supported.
            // Overwriting globals for a second distinct vtable would corrupt forwarding.
            if (g_hookedDeviceVtbl && g_hookedDeviceVtbl != (void*)vtbl) {
                Log("hooks: distinct device vtable %p already hooked %p - skipping second vtable (coverage limitation)",
                    (void*)vtbl, g_hookedDeviceVtbl);
            } else {
            // Make vtable page writable (it may be read-only)
            MEMORY_BASIC_INFORMATION mbi = {};
            VirtualQuery(vtbl, &mbi, sizeof(mbi));
            DWORD oldProt = 0;
            if (VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_READWRITE, &oldProt)) {
                // Save originals and swap
                // ID3D12Device vtable (verified SDK 10.0.28000.0): QI=0, SRV=18, RTV=20,
                // Committed=27, Placed=29, Reserved=30. Slot 26 is GetCustomHeapProperties.
                // DescriptorHeap=14 (heap inventory, observe-only).
                if (!Real_DeviceQI)
                    Real_DeviceQI = (PFN_DeviceQueryInterface)vtbl[0];
                if (!Real_CreateDescriptorHeap)
                    Real_CreateDescriptorHeap = (PFN_CreateDescriptorHeap)vtbl[14];
                if (!Real_CreateShaderResourceView)
                    Real_CreateShaderResourceView = (PFN_CreateShaderResourceView)vtbl[18];
                if (!Real_CreateRenderTargetView)
                    Real_CreateRenderTargetView = (PFN_CreateRenderTargetView)vtbl[20];
                if (!Real_CreateDepthStencilView)
                    Real_CreateDepthStencilView = (PFN_CreateDepthStencilView)vtbl[21];
                if (!Real_CreateCommittedResource)
                    Real_CreateCommittedResource = (PFN_CreateCommittedResource)vtbl[27];
                if (!Real_CreatePlacedResource)
                    Real_CreatePlacedResource = (PFN_CreatePlacedResource)vtbl[29];
                if (!Real_CreateReservedResource)
                    Real_CreateReservedResource = (PFN_CreateReservedResource)vtbl[30];
                // CopyDescriptors (23) / CopyDescriptorsSimple (24):
                // SDK-verified slots for ID3D12Device. These copy descriptor
                // handles between CPU-visible heaps. Tracking them lets us
                // propagate g_rtvMap/g_srvMap handle→resource entries to
                // shader-visible heaps, closing the gap where the engine
                // copies RTV/SRV handles before binding them.
                if (!Real_CopyDescriptors)
                    Real_CopyDescriptors = (PFN_CopyDescriptors)vtbl[23];
                if (!Real_CopyDescriptorsSimple)
                    Real_CopyDescriptorsSimple = (PFN_CopyDescriptorsSimple)vtbl[24];
                vtbl[18] = (void*)&Hook_CreateShaderResourceView;
                vtbl[20] = (void*)&Hook_CreateRenderTargetView;
                vtbl[21] = (void*)&Hook_CreateDepthStencilView;
                vtbl[27] = (void*)&Hook_CreateCommittedResource;
                vtbl[29] = (void*)&Hook_CreatePlacedResource;
                vtbl[30] = (void*)&Hook_CreateReservedResource;
                // CopyDescriptors (slot 23) / CopyDescriptorsSimple (slot 24)
                vtbl[23] = (void*)&Hook_CopyDescriptors;
                vtbl[24] = (void*)&Hook_CopyDescriptorsSimple;
                vtbl[14] = (void*)&Hook_CreateDescriptorHeap;
                vtbl[0] = (void*)&Shim_DeviceQI;
                VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProt, &oldProt);
                g_hookedDeviceVtbl = (void*)vtbl;
                Log("hooks: device vtable SWAPPED (18, 20, 21, 23, 24, 27, 29, 30) on device %p vtbl=%p", (void*)newDev, (void*)vtbl);
                Log("hooks: device QI census installed (slot 0) vtbl=%p", (void*)vtbl);
                Log("hooks: descriptor-heap inventory installed (slot 14) vtbl=%p", (void*)vtbl);
            } else {
                Log("hooks: VirtualProtect on vtable failed - device hooks not installed on device %p", (void*)newDev);
            }
            {
                void* targets[10] = { (void*)Hook_CreateRenderTargetView, (void*)Hook_CreateDepthStencilView, (void*)Hook_CreateShaderResourceView, (void*)Hook_CreateCommittedResource, (void*)Hook_CreatePlacedResource, (void*)Hook_CreateReservedResource, (void*)&Shim_DeviceQI, (void*)&Hook_CreateDescriptorHeap, (void*)Hook_CopyDescriptors, (void*)Hook_CopyDescriptorsSimple };
                CfgMarkValid(targets, 10);
            }
            }
        }

        ID3D12CommandQueue* queue = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // Do not create a ScaleNG-owned queue here. The real queue factory
        // hook below must capture BeamNG's first direct queue, not this
        // temporary queue; otherwise ECL observation is attached to the wrong
        // submission stream.
        if (false && SUCCEEDED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue) {
            void** qv = *(void***)queue;
            // DISABLED FOR BISECTION
            if (false) {
            // PURE VTABLE SWAP for queue too
            MEMORY_BASIC_INFORMATION qmbi = {};
            VirtualQuery(qv, &qmbi, sizeof(qmbi));
            DWORD qoldProt = 0;
            if (VirtualProtect(qmbi.BaseAddress, qmbi.RegionSize, PAGE_READWRITE, &qoldProt)) {
                Real_ExecuteCommandLists = (PFN_ExecuteCommandLists)qv[10];
                qv[10] = (void*)&Hook_ExecuteCommandLists;
                VirtualProtect(qmbi.BaseAddress, qmbi.RegionSize, qoldProt, &qoldProt);
                Log("hooks: queue vtable SWAPPED (slot 10)");
            } // end disabled queue swap
            } else {
                Log("hooks: queue ECL hook disabled by build (pipeline uses Present entry)");
            }
        }
        if (queue) g_graphicsQueue = queue;
        EnsureGlobalSwapchainHookImpl();
    }
    return hr;
}

void InstallCommandListHooks(ID3D12GraphicsCommandList* list, UINT listType)
{
    if (!list) return;
    InterlockedIncrement64(&g_installCallsTotal);

    void** original = nullptr;
    __try { original = *(void***)list; } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(&g_installFailTotal);
        return;
    }
    if (!original) { InterlockedIncrement64(&g_installFailTotal); return; }

    AcquireSRWLockExclusive(&g_commandListShimLock);
    for (auto& existing : g_commandListShims) {
        if (existing.list == list &&
            (existing.originalVtbl == original || existing.clonedVtbl == original)) {
            InterlockedIncrement64(&g_installDedupTotal);
            ReleaseSRWLockExclusive(&g_commandListShimLock);
            return;
        }
    }

    CommandListShim* slot = nullptr;
    for (auto& candidate : g_commandListShims) {
        if (!candidate.clonedVtbl) { slot = &candidate; break; }
    }
    if (!slot) {
        InterlockedIncrement64(&g_installFailTotal);
        ReleaseSRWLockExclusive(&g_commandListShimLock);
        // Capped: without the old once-per-pointer vector gate, a full table
        // would otherwise log on every submission of every unhookable list.
        static volatile LONG s_tableFullLogs = 0;
        if (InterlockedIncrement(&s_tableFullLogs) <= 8 || (s_tableFullLogs % 5000) == 0)
            Log("hooks: command-list shim table full");
        return;
    }

    constexpr SIZE_T kCommandListVtableEntries = 64;
    void** cloned = (void**)VirtualAlloc(nullptr,
        sizeof(void*) * kCommandListVtableEntries,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!cloned) {
        InterlockedIncrement64(&g_installFailTotal);
        ReleaseSRWLockExclusive(&g_commandListShimLock);
        Log("hooks: command-list shim allocation failed");
        return;
    }

    bool copied = true;
    __try {
        std::memcpy(cloned, original, sizeof(void*) * kCommandListVtableEntries);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        copied = false;
    }
    if (!copied) {
        VirtualFree(cloned, 0, MEM_RELEASE);
        InterlockedIncrement64(&g_installFailTotal);
        ReleaseSRWLockExclusive(&g_commandListShimLock);
        Log("hooks: command-list shim vtable copy faulted list=%p", (void*)list);
        return;
    }

    slot->list = list;
    slot->listType = listType;
    slot->createdPresent = (unsigned long long)InterlockedCompareExchange64(&g_presentSerial, 0, 0);
    slot->createdEcl = (unsigned long long)InterlockedCompareExchange64(&g_eclSerial, 0, 0);
    slot->copyCount = 0;
    slot->originalVtbl = original;
    slot->clonedVtbl = cloned;
    slot->close = (PFN_CommandListClose)original[9];
    slot->reset = (PFN_CommandListReset)original[10];
    slot->drawInstanced = (PFN_DrawInstanced)original[12];
    slot->drawIndexedInstanced = (PFN_DrawIndexedInstanced)original[13];
    slot->dispatch = (PFN_Dispatch)original[14];
    InterlockedExchange64(&slot->recordingEpoch, 1); // New lists begin recording.
    InterlockedExchange(&slot->recordingClosed, 0);
    InterlockedExchange(&slot->recordingResetInProgress, 0);
    InterlockedExchange(&slot->recordingResetFailed, 0);
    InterlockedExchangePointer((PVOID volatile*)&slot->currentOmMvTarget, nullptr);
    InterlockedExchangePointer((PVOID volatile*)&slot->recordedMvTarget, nullptr);
    InterlockedExchange(&slot->mvTargetWidth, 0);
    InterlockedExchange(&slot->mvTargetHeight, 0);
    InterlockedExchange64(&slot->mvTargetOmCount, 0);
    InterlockedExchange64(&slot->mvTargetDrawCount, 0);
    InterlockedExchange64(&slot->mvTargetIndexedDrawCount, 0);
    InterlockedExchange64(&slot->dispatchCount, 0);
    InterlockedExchange64(&slot->mvTargetRecordPresent, 0);
    InterlockedExchange64(&slot->mvTargetRecordEcl, 0);
    // ID3D12GraphicsCommandList vtable slots from the Windows SDK
    // (10.0.28000.0 DECLSPEC_XFGVIRT order cross-checked):
    // CopyBufferRegion=15, CopyTextureRegion=16, CopyResource=17,
    // RSSetViewports=21, RSSetScissorRects=22, ResourceBarrier=26,
    // OMSetRenderTargets=46, SetGraphicsRootDescriptorTable=32.
    slot->copyBufferRegion = (PFN_CopyBufferRegion)original[15];
    slot->copyTexture = (PFN_CopyTextureRegion)original[16];
    slot->copyResource = (PFN_CopyResource)original[17];
    slot->setGraphicsRootDescriptorTable = (PFN_SetGraphicsRootDescriptorTable)original[32];
    slot->rsSetViewports = (PFN_RSSetViewports)original[21];
    slot->rsSetScissorRects = (PFN_RSSetScissorRects)original[22];
    slot->omSetRenderTargets = (PFN_OMSetRenderTargets)original[46];
    slot->resourceBarrier = (PFN_ResourceBarrier)original[26];
    static unsigned s_slotDiag = 0;
    if (++s_slotDiag <= 3) {
        Log("hooks: shim vtable slots copyBufferRegion=%p copyTexture=%p copyResource=%p resourceBarrier=%p omSetRenderTargets=%p rsSetViewports=%p rsSetScissorRects=%p rootDescTable=%p",
            (void*)slot->copyBufferRegion, (void*)slot->copyTexture,
            (void*)slot->copyResource,
            (void*)slot->resourceBarrier, (void*)slot->omSetRenderTargets,
            (void*)slot->rsSetViewports, (void*)slot->rsSetScissorRects,
            (void*)slot->setGraphicsRootDescriptorTable);
    }
    cloned[15] = (void*)&Shim_CopyBufferRegion;
    cloned[16] = (void*)&Shim_CopyTextureRegion;
    cloned[17] = (void*)&Shim_CopyResource;
    cloned[32] = (void*)&Shim_SetGraphicsRootDescriptorTable;
    cloned[21] = (void*)&Shim_RSSetViewports;
    cloned[22] = (void*)&Shim_RSSetScissorRects;
    cloned[46] = (void*)&Shim_OMSetRenderTargets;
    cloned[26] = (void*)&Shim_ResourceBarrier;
    cloned[9] = (void*)&Shim_CommandListClose;
    cloned[10] = (void*)&Shim_CommandListReset;
    cloned[12] = (void*)&Shim_DrawInstanced;
    cloned[13] = (void*)&Shim_DrawIndexedInstanced;
    cloned[14] = (void*)&Shim_Dispatch;

    bool installed = true;
    __try { *(void***)list = cloned; }
    __except (EXCEPTION_EXECUTE_HANDLER) { installed = false; }
    if (!installed) {
        slot->list = nullptr;
        slot->listType = 0xFFFFFFFFu;
        slot->createdPresent = 0;
        slot->createdEcl = 0;
        slot->copyCount = 0;
        slot->originalVtbl = nullptr;
        slot->clonedVtbl = nullptr;
        slot->copyTexture = nullptr;
        slot->copyBufferRegion = nullptr;
        slot->copyResource = nullptr;
        slot->setGraphicsRootDescriptorTable = nullptr;
        slot->rsSetViewports = nullptr;
        slot->rsSetScissorRects = nullptr;
        slot->omSetRenderTargets = nullptr;
        slot->resourceBarrier = nullptr;
        slot->close = nullptr;
        slot->reset = nullptr;
        slot->drawInstanced = nullptr;
        slot->drawIndexedInstanced = nullptr;
        slot->dispatch = nullptr;
        InterlockedExchange(&slot->recordingClosed, 1);
        InterlockedExchange(&slot->recordingResetInProgress, 0);
        InterlockedExchange(&slot->recordingResetFailed, 1);
        InterlockedExchangePointer((PVOID volatile*)&slot->currentOmMvTarget, nullptr);
        InterlockedExchangePointer((PVOID volatile*)&slot->recordedMvTarget, nullptr);
        VirtualFree(cloned, 0, MEM_RELEASE);
        InterlockedIncrement64(&g_installFailTotal);
        ReleaseSRWLockExclusive(&g_commandListShimLock);
        Log("hooks: command-list shim install faulted list=%p", (void*)list);
        return;
    }

    void* targets[13] = { (void*)&Shim_CopyBufferRegion,
                         (void*)&Shim_CopyTextureRegion,
                         (void*)&Shim_CopyResource,
                         (void*)&Shim_SetGraphicsRootDescriptorTable,
                         (void*)&Shim_RSSetViewports,
                         (void*)&Shim_RSSetScissorRects,
                         (void*)&Shim_OMSetRenderTargets,
                         (void*)&Shim_ResourceBarrier,
                         (void*)&Shim_CommandListClose,
                         (void*)&Shim_CommandListReset,
                         (void*)&Shim_DrawInstanced,
                         (void*)&Shim_DrawIndexedInstanced,
                         (void*)&Shim_Dispatch };
    CfgMarkValid(targets, 13);
    InterlockedIncrement64(&g_installOkTotal);
    ReleaseSRWLockExclusive(&g_commandListShimLock);
    Log("hooks: command-list read-only shim installed list=%p original=%p cloned=%p",
        (void*)list, (void*)original, (void*)cloned);
    return;
/*
    // COMMAND-LIST HOOKS REMOVED: MinHook patches on hot ID3D12GraphicsCommandList
    // methods (SetDescriptorHeaps, ResourceBarrier, CopyTextureRegion, etc.) cause
    // solid-color rendering artifacts. These functions run thousands of times per
    // frame inside nvwgf2umx.dll; the trampoline overhead + instruction relocation
    // corrupts driver-internal state.
    //
    // Discovery now uses ONLY cold-path hooks:
    //   - CreateRenderTargetView / CreateShaderResourceView (device vtable)
    //   - OMSetRenderTargets (called via vtable from ECL, but tracked via
    //     the depth-stencil descriptor parameter instead of hooking the list)
    //   - ExecuteCommandLists (queue vtable) for frame trigger
    //
    // If per-list hooks become necessary in the future, use a different
    // interception method (e.g., wrapping the command list interface).
    Log("hooks: cmdlist hooks DISABLED (artifact fix - using device-level discovery only)");
*/
}

} // namespace

void EnsureGlobalSwapchainHookEx() { EnsureGlobalSwapchainHookImpl(); }

// ============================================================================
// SYNTHETIC NGX SMOKE TEST
// Self-contained test: creates own device textures, initializes NGX on the
// game's captured device, runs one evaluate. No engine resources touched.
// Proves NGX can execute on the game's device. Zero hooks required.
// ============================================================================

void RunNgxSyntheticTest()
{
    // PROCESS-WIDE once-guard via named mapping: survives multiple ASI
    // copies / loader re-entries where per-copy statics do not.
    {
        HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, L"Local\\ScaleNG_SmokeRan");
        long* pShared = hMap ? (long*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 64) : nullptr;
        bool alreadyRan = true;
        if (pShared) {
            alreadyRan = (InterlockedCompareExchange(pShared, 1, 0) != 0);
            UnmapViewOfFile(pShared);
        } else {
            static volatile long s_fallback = 0;
            alreadyRan = (InterlockedCompareExchange(&s_fallback, 1, 0) != 0);
        }
        if (hMap) CloseHandle(hMap);
        if (alreadyRan) return;
    }
    InterlockedExchange(&g_smokeBusy, 1);

    Log("SMOKE: starting synthetic NGX test");
    // If game device not captured (ASI loaded after device creation), create our own
    if (!g_device) {
        typedef HRESULT(WINAPI* PFN_DC)(IUnknown*, D3D_FEATURE_LEVEL, const IID&, void**);
        HMODULE dm = GetModuleHandleA("d3d12.dll");
        auto mkD = dm ? (PFN_DC)GetProcAddress(dm, "D3D12CreateDevice") : nullptr;
        IDXGIFactory1* fct = nullptr;
        { typedef HRESULT(WINAPI* PFN_CDXGI)(const IID&, void**);
          HMODULE dxgi = GetModuleHandleA("dxgi.dll");
          auto mkF = dxgi ? (PFN_CDXGI)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
          if (mkF) mkF(__uuidof(IDXGIFactory1), (void**)&fct); }
        IDXGIAdapter1* nva = nullptr;
        if (fct) { for(UINT i=0; fct->EnumAdapters1(i,&nva)==S_OK;++i){
            DXGI_ADAPTER_DESC1 d={};nva->GetDesc1(&d);
            if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&d.VendorId==0x10DE)break; nva=nullptr;} }
        if (nva && mkD)
            mkD(nva,D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),(void**)&g_device);
        if (fct) fct->Release();
        if (!g_device) { Log("SMOKE: FAIL - could not create fallback device"); return; }
        Log("SMOKE: created OWN NVIDIA device for isolated NGX test");
    }
    Log("SMOKE: device=%p", (void*)g_device);

    // UNWRAP: find real ID3D12Device inside BeamNG's wrapper by vtable match.
    // Create a clean device to discover the true D3D12Device vtable address,
    // then scan g_device's memory for embedded objects with same vtable.
    ID3D12Device* ngxDev = g_device;
    {
        typedef HRESULT(WINAPI* PFN_DC)(IUnknown*, D3D_FEATURE_LEVEL, const IID&, void**);
        HMODULE dm = GetModuleHandleA("d3d12.dll");
        auto mk = dm ? (PFN_DC)GetProcAddress(dm, "D3D12CreateDevice") : nullptr;
        if (mk) {
            ID3D12Device* td = nullptr;
            if (SUCCEEDED(mk(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&td)) && td) {
                void* rvt = *(void**)td;
                Log("SMOKE-UNWRAP: real vtable=%p", rvt);
                td->Release();
                __try {
                    auto mem = (void**)g_device;
                    for (int off = 0; off < 64; ++off) {
                        void* cand = mem[off];
                        if (!cand) continue;
                        MEMORY_BASIC_INFORMATION m2 = {};
                        if (!VirtualQuery(cand, &m2, sizeof(m2)) || m2.State != MEM_COMMIT) continue;
                        if (*(void**)cand == rvt) {
                            auto cd = (ID3D12Device*)cand;
                            __try {
                                if (SUCCEEDED(cd->GetDeviceRemovedReason())) {
                                    ngxDev = cd;
                                    Log("SMOKE-UNWRAP: FOUND real dev at +0x%x ptr=%p", off*8, (void*)ngxDev);
                                    break;
                                }
                            } __except(EXCEPTION_EXECUTE_HANDLER) {}
                        }
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
                if (ngxDev == g_device) Log("SMOKE-UNWRAP: no separate dev found - using g_device directly");
            }
        }
    }
    Log("SMOKE: using %s device %p", ngxDev == g_device ? "g_device" : "UNWRAPPED", (void*)ngxDev);

    // Create 512x512 test textures on unwrapped device
    UINT w = 512, h = 512;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;

    ID3D12Resource* color=nullptr,*depth=nullptr,*mv=nullptr,*out=nullptr;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = ngxDev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&color));
    Log("SMOKE: color hr=0x%08X",(unsigned)hr);
    rd.Format = DXGI_FORMAT_R32_FLOAT; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = ngxDev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&depth));
    Log("SMOKE: depth hr=0x%08X",(unsigned)hr);
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    hr = ngxDev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&mv));
    Log("SMOKE: mv hr=0x%08X",(unsigned)hr);
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = ngxDev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&out));
    Log("SMOKE: out hr=0x%08X",(unsigned)hr);
    if(!color||!depth||!mv||!out){Log("SMOKE: FAIL tex");return;}

    ID3D12CommandQueue* q=nullptr;
    {D3D12_COMMAND_QUEUE_DESC qd={};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ngxDev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q));}
    ID3D12CommandAllocator* al=nullptr;
    ngxDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&al));
    ID3D12GraphicsCommandList* cl=nullptr;
    ngxDev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,al,nullptr,IID_PPV_ARGS(&cl));
    if(!q||!al||!cl){Log("SMOKE: FAIL cmd infra");return;}
    Log("SMOKE: q=%p al=%p cl=%p",(void*)q,(void*)al,(void*)cl);

    // NGX init on unwrapped game device
    if(!g_upscaler) g_upscaler = CreateUpscaler(UPSCALER_DLSS);
    if(!g_upscaler){Log("SMOKE: FAIL upscaler");return;}
    UpscalerInitParams ip={};
    ip.device=ngxDev; ip.renderWidth=w; ip.renderHeight=h;
    ip.displayWidth=w; ip.displayHeight=h;
    ip.dlssDllPath=g_cfg.dlssDllPath; ip.appId=g_cfg.appId;
    ip.perfQuality=0; ip.mvJittered=false; ip.autoExposure=true;
    bool iok=g_upscaler->Init(ip);
    Log("SMOKE: NGX Init %s",iok?"SUCCESS":"FAILED");
    if(!iok){Log("SMOKE: RESULT - init failed");return;}

    // Evaluate
    cl->Reset(al,nullptr);
    UpscalerEvaluateParams ep={};
    ep.commandList=cl; ep.color=color; ep.depth=depth;
    ep.motionVectors=mv; ep.output=out;
    ep.jitterX=0; ep.jitterY=0;
    ep.mvScaleX=(float)w; ep.mvScaleY=(float)h;
    ep.sharpness=0.0f;
    // CONTINUOUS EVALUATION: 100 iterations proving sustained NGX operation.
    int pass = 0, fail = 0, evalFails = 0;
    ID3D12Fence* fence = nullptr;
    ngxDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE evt = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    for (int iter = 0; iter < 100; ++iter) {
        // Device health check EVERY iteration - abort immediately on removal
        HRESULT drr = ngxDev->GetDeviceRemovedReason();
        if (FAILED(drr)) {
            Log("SMOKE: iter %d DEVICE REMOVED hr=0x%08X - aborting", iter, (unsigned)drr);
            break;
        }

        cl->Reset(al, nullptr);

        D3D12_RESOURCE_BARRIER bars[4] = {};
        for (int i = 0; i < 4; ++i) { bars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
        bars[0].Transition.pResource = color;
        bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bars[1].Transition.pResource = depth;
        bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bars[2].Transition.pResource = mv;
        bars[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bars[2].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bars[3].Transition.pResource = out;
        bars[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bars[3].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cl->ResourceBarrier(4, bars);

        UpscalerEvaluateParams ep = {};
        ep.commandList = cl;
        ep.color = color;
        ep.depth = depth;
        ep.motionVectors = mv;
        ep.output = out;
        ep.jitterX = 0.0f; ep.jitterY = 0.0f;
        ep.mvScaleX = 1.0f; ep.mvScaleY = 1.0f;
        ep.sharpness = 0.0f;

        bool eok = g_upscaler->Evaluate(ep);
        if (!eok) {
            // EvaluateFeature failed - NGX may have recorded partial/garbage
            // commands. DO NOT submit - discard and abort.
            Log("SMOKE: iter %d Evaluate FAILED - discarding command list, aborting", iter);
            ++evalFails;
            break;
        }
        HRESULT chr = cl->Close();
        if (FAILED(chr)) {
            Log("SMOKE: iter %d Close FAILED hr=0x%08X", iter, (unsigned)chr);
            break;
        }

        q->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList*const*>(&cl));

        fence->SetEventOnCompletion(iter + 1, evt);
        q->Signal(fence, iter + 1);
        DWORD wr = WaitForSingleObject(evt, 10000);

        if (wr == WAIT_OBJECT_0) { ++pass; }
        else { ++fail; if (fail <= 3) Log("SMOKE: iter %d GPU TIMEOUT", iter); }

        // Pace: don't starve the game's GPU work (prevents TDR during map load)
        Sleep(16);

        if ((iter + 1) % 25 == 0)
            Log("SMOKE: %d/100 evaluates done (pass=%d fail=%d)", iter + 1, pass, fail);
    }

    HRESULT postDrr = ngxDev->GetDeviceRemovedReason();
    Log("SMOKE: RESULT - %d/100 PASS, %d FAIL, devRemoved=0x%08X",
        pass, fail, (unsigned)postDrr);

    // Cleanup
    CloseHandle(evt); if(fence)fence->Release();
    if(color)color->Release(); if(depth)depth->Release();
    if(mv)mv->Release(); if(out)out->Release();
    if(cl)cl->Release(); if(al)al->Release(); if(q)q->Release();
}

void HooksSetConfig(const ScaleNgConfig& config)
{
    g_cfg = config;
    g_cfgSet = true;
    g_dlaaMode = config.dlaa;
    g_hudIniOn = config.hud;
    g_legacyScale = config.legacyScale;
    g_passiveMode = config.passive;
    g_shadowHandoff = config.shadowHandoff;
    g_abWindowPresents = config.abWindow;
    g_shadowRealInputs = config.realInputs;
    Log("hooks: config applied (dlaa=%d legacyScale=%d; viewport arming is dlaa-gated, legacyScale retained for compat)",
        g_dlaaMode ? 1 : 0, g_legacyScale ? 1 : 0);
}

void HooksGetDescriptorHeaps(UINT* count, ID3D12DescriptorHeap** heaps)
{
    if (!count) return;
    AcquireSRWLockShared(&g_heapStateLock);
    *count = g_setHeapCount;
    if (heaps) {
        for (UINT i = 0; i < g_setHeapCount; ++i)
            heaps[i] = g_setHeaps[i];
    }
    ReleaseSRWLockShared(&g_heapStateLock);
}

void HooksRestoreDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT count,
                                 ID3D12DescriptorHeap* const* heaps)
{
    if (list && Real_SetDescriptorHeaps)
        Real_SetDescriptorHeaps(list, count, heaps);
}

// Externally-linked getter (dlss_ngx.cpp uses it for init sequencing).
// Defined OUTSIDE the anonymous namespace so the header decl binds; it may
// still read anon-namespace globals since this is the same TU.
unsigned HooksGetQuietFrames() {
    // Chain never observed yet => NOT quiet. (frameCounter grows through the
    // loading screen before any display-sized copy exists, which made the
    // 600f gate pass instantly and let nvngx load mid-churn.)
    // Correctness: use the observed flag, not last==0, because 0 is also a
    // valid frameCounter (first chain node seen pre-first-camera). Threshold
    // and units unchanged (camera-frames, 120).
    return g_chainObserved ? (g_frameCounter - g_lastNewChainFrame) : 0;
}
void HooksSetSmokeBusy(int v) { InterlockedExchange(&g_smokeBusy, (LONG)v); }
void HooksKickEGSH() { EnsureGlobalSwapchainHookImpl(); }

// CPU-side microscope: VEH logs EVERY first-chance AV with module+offset as
// it happens. Rotation-burst deaths show no SEH catch and no device-removed,
// so they may be AVs outside our try regions - this names them live.
static LONG CALLBACK SngVectoredHandler(PEXCEPTION_POINTERS ep)
{
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        static volatile long s_vehLogs = 0;
        if (InterlockedCompareExchange(&s_vehLogs, 0, 0) < 20 &&
            !InterlockedIncrement(&s_vehLogs)) {
            HMODULE mod = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod);
            char mName[MAX_PATH] = "?";
            if (mod) GetModuleFileNameA(mod, mName, MAX_PATH);
            const char* base = strrchr(mName, '\\'); base = base ? base + 1 : mName;
            Log("VEH: AV at %s+0x%p addr=0x%p flags=%u",
                base,
                (void*)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)mod),
                ep->ExceptionRecord->ExceptionInformation[1],
                (unsigned)ep->ExceptionRecord->ExceptionInformation[0]);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void HooksInstallVEH()
{
    AddVectoredExceptionHandler(1, SngVectoredHandler);
    Log("VEH: installed (first-chance AV logger)");
}

// Dump Device Removed Extended Data from the bridge device: auto-breadcrumb
// command history + page-fault VA for whatever op killed the adapter.
void HooksDumpDRED(const char* why)
{
    if (!g_bridgeDev) return;
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(g_bridgeDev->QueryInterface(IID_PPV_ARGS(&dred)))) {
        Log("DRED[%s]: not available on bridge device", why);
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)) && bc.pHeadAutoBreadcrumbNode) {
        int dumped = 0;
        for (auto n = bc.pHeadAutoBreadcrumbNode; n && dumped < 8; n = n->pNext, ++dumped) {
            unsigned lastOp = n->BreadcrumbCount ? (unsigned)n->pCommandHistory[n->BreadcrumbCount - 1] : 0;
            Log("DRED[%s]: list '%ls' q '%ls' ops=%u lastOp=%u",
                why,
                n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"?",
                n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"?",
                (unsigned)n->BreadcrumbCount, lastOp);
        }
    } else {
        Log("DRED[%s]: no breadcrumb nodes recorded", why);
    }
    dred->Release();
}

void HooksInstallCreateDeviceDetour()
{
    // P0#3 idempotency: raw byte patch must NEVER be applied twice (double
    // patch = corrupted prologue = instant crash on first device creation).
    static long s_installState = 0; // 0=NOT_INSTALLED 1=INSTALLED 2=FAILED
    if (InterlockedCompareExchange(&s_installState, 0, 0) != 0) {
        Log("hooks: CreateDevice detour already state=%ld - skipping re-install",
            s_installState);
        return;
    }
    if (!g_cfgSet) {
        Log("hooks: install called before config - ignoring");
        return;
    }
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    if (!d3d12) {
        Log("hooks: d3d12.dll not loaded yet - ScaleNG inactive");
        InterlockedExchange(&s_installState, 2);
        return;
    }
    void* pCreateDevice = (void*)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!pCreateDevice) {
        Log("hooks: D3D12CreateDevice export not found - ScaleNG inactive");
        InterlockedExchange(&s_installState, 2);
        return;
    }
    unsigned char pre[16] = {};
    memcpy(pre, pCreateDevice, 16);
    Log("hooks: D3D12CreateDevice first bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
        pre[0], pre[1], pre[2], pre[3], pre[4], pre[5], pre[6], pre[7],
        pre[8], pre[9], pre[10], pre[11], pre[12], pre[13], pre[14], pre[15]);
    if (MH_Initialize() != MH_OK) {
        Log("hooks: MinHook initialize failed - ScaleNG inactive");
        InterlockedExchange(&s_installState, 2);
        return;
    }
    MH_STATUS st = MH_CreateHook(pCreateDevice, &Hook_D3D12CreateDevice, (void**)&Real_D3D12CreateDevice_Tramp);
    if (st != MH_OK) {
        Log("hooks: D3D12CreateDevice hook failed (%d) - ScaleNG inactive", (int)st);
        InterlockedExchange(&s_installState, 2);
        return;
    }
    if (MH_EnableHook(pCreateDevice) != MH_OK) {
        Log("hooks: D3D12CreateDevice enable failed - ScaleNG inactive");
        InterlockedExchange(&s_installState, 2);
        return;
    }
    InterlockedExchange(&s_installState, 1);
    Log("hooks: D3D12CreateDevice detour installed");

    // Global Map/Unmap hooks for constant buffer discovery (camera CB, velocity CB)
    // These catch Map calls on already-created resources
    if (g_device) {
        void** devVtbl = *(void***)g_device;
        // Note: Map/Unmap are on ID3D12Resource vtable, not device vtable.
        // We need to hook them on the resource vtable. Since resources are created
        // dynamically, we'll use a different approach: hook CreateCommittedResource
        // to install per-resource shims (already done above).
        // For resources created BEFORE our hook, we need another approach.
        Log("hooks: Resource Map/Unmap shim infrastructure ready");
    }

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
