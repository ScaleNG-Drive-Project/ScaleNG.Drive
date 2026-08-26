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

// Tracked-resource ownership: discovery AddRefs every engine resource it
// adopts so the pointer can never dangle if the engine releases its own ref
// between our frames (use-after-free was the mv-barrier TDR source). The
// previous occupant is parked in the graveyard and released after the next
// injection fence proves the GPU is done with it.
// Created-table: refs taken INSIDE creation hooks (object provably alive,
// engine holds its own ref alongside ours). Adoption later TRANSFERS that
// ref into the tracked slot. Observing an unknown resource adopts it as a
// WEAK pointer - AddRef-on-observation is illegal (the object may be mid-
// destruction; resurrecting it corrupted the engine's object graph and
// caused deterministic teardown AVs).
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

void StoreTracked(ID3D12Resource** slot, ID3D12Resource* res)
{
    if (*slot == res) return;
    bool mvSlot = (slot == (ID3D12Resource**)&g_mvResource) || (slot == (ID3D12Resource**)&g_mvResourceAlt);
    bool depSlot = (slot == (ID3D12Resource**)&g_depthResource);
    bool sceneSlot = (slot == (ID3D12Resource**)&g_sceneColor) || (slot == (ID3D12Resource**)&g_sceneColorAlt);
    if (sceneSlot && *slot && res) {
        // Routine double-buffer ping-pong (engine binds A,B,A,B...) must NOT
        // count as a change - otherwise the settle gate can never elapse.
        ID3D12Resource** other = (slot == (ID3D12Resource**)&g_sceneColor)
                                     ? &g_sceneColorAlt : &g_sceneColor;
        if (*other == res) { *slot = res; return; } // pure reassignment
        // Only PERSISTENT composite sources are real scene changes - transient
        // post targets on recycled descriptors swap constantly during play.
        int persistNow = 0;
        { AcquireSRWLockShared(&g_copyMapLock); auto ci = g_copySrcCount.find((void*)res); if (ci != g_copySrcCount.end()) persistNow = ci->second; ReleaseSRWLockShared(&g_copyMapLock); }
        if (persistNow < 40) { *slot = res; return; }
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
        if (*oMv == res) { *slot = res; return; }
        g_lastDiscoveryChangeFrame = g_frameCounter;
    } else if (depSlot) {
        g_lastDiscoveryChangeFrame = g_frameCounter;
    }
    // STRICTLY WEAK: never hold refs on engine-owned resources. BeamNG's
    // lifecycle is refcount-exact - any extra ref (at observation OR at
    // creation) desyncs its teardown bookkeeping and corrupts its object
    // graph. Staleness stamps + bridge SEH handle freed pointers safely.
    *slot = res;
}
// Last-seen MV RTV descriptor key. After an invalidation the engine does NOT
// recreate its MV texture (same map/spawn - fixed content), so creation-based
// adoption never refires and MV stays 'absent' all session. This key lets us
// re-adopt the same texture from g_rtvMap once discovery is live again.
static unsigned long long g_mvLastRtvKey = 0;
// Rolling last-full-res-copy source - correlated with Present to name the
// terminal scene node (the texture that feeds Present = DLAA input target).
static unsigned g_lastNewChainFrame = 0;
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
// Heap-state snapshot lock: engine threads WRITE this state on every
// SetDescriptorHeaps while the Present-thread flow READS it to save/restore.
// Unsynced tearing during rotation churn handed the restore path freed heap
// pointers - the convicted killer of the 6s-class crashes (WER offsets
// 0x9d5d..0x9e3d, all inside Hook_SetDescriptorHeaps).
static SRWLOCK g_heapStateLock = SRWLOCK_INIT;

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
    return nullptr;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES after)
{
    if (!list || !res) return;
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
    Real_ResourceBarrier(list, 1, &b);
    { BookGuard _bg; g_resourceStates[res] = after; }
    // Rate-limited: this fires per-transition inside the flow; unlogged it
    // was ~400 lines/sec and the synchronous log I/O contributed to freezes.
    static volatile long s_barrierLogs = 0;
    long n = InterlockedIncrement(&s_barrierLogs);
    if (n <= 25 || (n % 2000) == 0)
        Log("hooks: barrier %p %u -> %u (#%ld)", (void*)res, (unsigned int)before, (unsigned int)after, n);
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
    // Candidate confirmed stable — commit only if different from current.
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
    if (!bypassQuietGate && HooksGetQuietFrames() < 120) {
        static int s_seqLogs = 0;
        if (++s_seqLogs <= 5)
            Log("hooks: NGX init deferred - chain quiet %uf/120f", HooksGetQuietFrames());
        return; // retried by later callers
    }
    // Atomic: only one thread may attempt NGX init
    if (InterlockedCompareExchange(&g_upscalerInitAttempted, 1, 0) != 0) return;
    // SINGLE-DEVICE ARCHITECTURE: abandon bridge. Use game's device directly.
    // The old assumption was that NGX needs IDXGIDevice (which the wrapper
    // blocks). But we never actually TESTED NGX on the wrapped device - we
    // assumed failure. Test it now.
    if (!g_device) return;
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
            Log("hooks: SINGLE-DEVICE wrapper blocks IDXGIDevice - proceeding anyway");
        }
    }
    if (!g_upscaler) g_upscaler = CreateUpscaler(UPSCALER_DLSS);
    if (!g_upscaler) {
        Log("hooks: upscaler creation failed");
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
    // Bridge mode owns DLAA exclusively (Present-time flow). This legacy
    // path records NGX work into the ENGINE's command list - illegal when
    // the feature lives on the bridge device. Hard-disable in DLAA mode.
    if (g_dlaaMode) return;
    EnsureUpscalerInit(false);
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
    // DISCOVERY MUST ALWAYS RUN: resources are discovered at creation time
    // BEFORE the bridge exists - gating on g_bridgeReady creates a
    // chicken-and-egg deadlock (need discovery to build bridge, need bridge
    // to enable discovery).
    if (device == g_device && res && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
        D3D12_RESOURCE_DESC rd = res->GetDesc();
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
                { BookGuard _bg; g_resourceStates[res] = D3D12_RESOURCE_STATE_COMMON; }
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
            desc->Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
            // Display-sized UNORM color target - adopt as scene color. The size
            // is NOT hardcoded (the engine may render at e.g. 1920x1001).
            { BookGuard _bg;
                { BookGuard _bg;
                    g_resourceStates[res] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
                }
            }
            if (!g_sceneColorValid) {
                StoreTracked(&g_sceneColor, res);
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
                StoreTracked(&g_sceneColorAlt, res);
                g_sceneColorRtvAlt = handle;
                AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
                Log("hooks: scene color RTV %p (%ux%u R16G16B16A16_UNORM) (ALT)", (void*)res,
                    (unsigned int)rd.Width, (unsigned int)rd.Height);
            }
    } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1) {
            if (desc->Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
                g_displayW = (unsigned int)rd.Width;
                g_displayH = (unsigned int)rd.Height;
                { BookGuard _bg;
                    g_resourceStates[res] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
                }
                if (!g_sceneColorValid) {
                    StoreTracked(&g_sceneColor, res);
                    g_sceneColorRtv = handle;
                    g_sceneColorValid = true;
                    Log("hooks: scene color RTV %p (1920x992 R16G16B16A16_UNORM)", (void*)res);
                } else if (res == g_sceneColor) {
                    // The game re-created the RTV view for the same resource
                    // (e.g. renderer re-init). Refresh the stored handle.
                    g_sceneColorRtv = handle;
                    Log("hooks: scene color RTV handle refreshed %p", (void*)res);
                } else if (res != g_sceneColorAlt) {
                    StoreTracked(&g_sceneColorAlt, res);
                    g_sceneColorRtvAlt = handle;
                    Log("hooks: scene color RTV %p (1920x992 R16G16B16A16_UNORM) (ALT)", (void*)res);
                }
            } else if (desc->Format == DXGI_FORMAT_R16G16_FLOAT) {
                g_mvW = (unsigned int)rd.Width;
                g_mvH = (unsigned int)rd.Height;
                { BookGuard _bg;
                    g_resourceStates[res] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
                }
                if (!g_mvValid) {
                    if (!g_mvValid) g_mvFirstValidFrame = g_frameCounter;
            StoreTracked(&g_mvResource, res);
                    g_mvValid = true;
                    g_mvStamp = g_frameCounter;
                    g_mvLastRtvKey = handle.ptr;
                    Log("hooks: motion vector RTV %p (%ux%u R16G16_FLOAT)", (void*)res, g_mvW, g_mvH);
                } else if (res == g_mvResource || res == g_mvResourceAlt) {
                    if (res == g_mvResource) {
                        if (!g_mvValid) g_mvFirstValidFrame = g_frameCounter;
            StoreTracked(&g_mvResource, res);
                        Log("hooks: motion vector RTV handle refreshed %p", (void*)res);
                    }
                } else if (res != g_mvResourceAlt) {
                    StoreTracked(&g_mvResourceAlt, res);
                    Log("hooks: motion vector RTV %p (%ux%u R16G16_FLOAT) (ALT)", (void*)res, g_mvW, g_mvH);
                }
            }
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   rd.Width >= 1000 && rd.Height >= 500 && rd.MipLevels == 1 &&
                   desc->Format == DXGI_FORMAT_R16G16_FLOAT) {
            { BookGuard _bg;
                { BookGuard _bg;
                    g_resourceStates[res] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    { BookGuard _bg; g_rtvMap[handle.ptr] = res; }
                }
            }
            if (!g_mvValid) {
                if (!g_mvValid) g_mvFirstValidFrame = g_frameCounter;
            StoreTracked(&g_mvResource, res);
                g_mvValid = true;
                g_mvStamp = g_frameCounter;
                g_mvW = (unsigned int)rd.Width;
                g_mvH = (unsigned int)rd.Height;
                g_mvLastRtvKey = handle.ptr;
                Log("hooks: motion vector RTV %p (1920x1001 R16G16_FLOAT)", (void*)res);
            } else if (res == g_mvResource || res == g_mvResourceAlt) {
                if (res == g_mvResource)
                    Log("hooks: motion vector RTV handle refreshed %p", (void*)res);
            } else if (res != g_mvResourceAlt) {
                StoreTracked(&g_mvResourceAlt, res);
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
                   desc->Format == DXGI_FORMAT_R16G16B16A16_UNORM && rd.MipLevels == 1 &&
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
        // Creation-time ref for depth-family targets (same safety rationale).
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.MipLevels == 1 &&
            rd.Width == g_displayW && rd.Height == g_displayH &&
            (desc->Format == DXGI_FORMAT_R32_FLOAT || desc->Format == DXGI_FORMAT_R32_TYPELESS ||
             desc->Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || desc->Format == DXGI_FORMAT_D32_FLOAT)) {
            StoreTracked(&g_depthResource, res);
            g_depthValid = true;
            g_depthStamp = g_frameCounter;
            g_depthRealFmt = rd.Format;
            g_depthMsaa = rd.SampleDesc.Count != 1;
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

void EnsureGlobalSwapchainHookImpl();

void Hook_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists,
                              ID3D12CommandList* const* lists)
{
    if (queue)
        g_graphicsQueue = queue;
    EnsureGlobalSwapchainHookImpl();
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
static bool                       g_b2Ready = false;
static bool                       g_ngxBlocked = false; // wrapper detected -> bridge mode
static UINT                       g_b2W = 0, g_b2H = 0;
static DXGI_FORMAT                g_b2Fmt = DXGI_FORMAT_UNKNOWN;
static ID3D12Resource*            g_b2Depth = nullptr;
static ID3D12Resource*            g_b2Mv    = nullptr;

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
static HANDLE g_b2HColor  = nullptr, g_b2HOut = nullptr;
static HANDLE g_b2HFIn    = nullptr, g_b2HFOut = nullptr;

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
    Log("ngx-b2: helper mode %s (ini [bridge] helper)", g_b2UseHelper ? "ENABLED" : "off");
}

static bool B2StartHelper()
{
    if (!g_b2UseHelper) return false;
    if (g_b2Pipe) return true;
    B2KillOrphans(); // stale helpers hold the pipe name -> ERROR_PIPE_BUSY

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
    g_b2Pipe = CreateNamedPipeA("\\\\.\\pipe\\ScaleNG_NGX",
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, sizeof(unsigned long long) * 8, sizeof(unsigned long long) * 8, 0, &sa);
    if (g_b2Pipe == INVALID_HANDLE_VALUE) {
        Log("ngx-b2: CreateNamedPipe FAILED err=%lu", GetLastError());
        g_b2Pipe = nullptr; g_b2UseHelper = false;
        return false;
    }

    wchar_t cmd[MAX_PATH * 2];
    _snwprintf_s(cmd, _TRUNCATE, L"\"%ls\" %lu", exe, GetCurrentProcessId());
    PROCESS_INFORMATION pi = {};
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log("ngx-b2: spawn FAILED err=%lu", GetLastError());
        CloseHandle(g_b2Pipe); g_b2Pipe = nullptr; g_b2UseHelper = false;
        return false;
    }
    g_b2Helper = pi.hProcess; CloseHandle(pi.hThread);

    if (!ConnectNamedPipe(g_b2Pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        Log("ngx-b2: ConnectNamedPipe FAILED err=%lu", GetLastError());
        B2KillHelper(); g_b2UseHelper = false;
        return false;
    }

    unsigned int hello[2] = { 0x58474E53, 1 }; // 'SNGX'
    unsigned int ack[2] = {};
    DWORD wr = 0, rd = 0;
    if (!WriteFile(g_b2Pipe, hello, sizeof(hello), &wr, nullptr) ||
        !ReadFile(g_b2Pipe, ack, sizeof(ack), &rd, nullptr) ||
        ack[0] != 0x58474E48) {
        Log("ngx-b2: handshake FAILED (ack=%08X)", ack[0]);
        B2KillHelper(); g_b2UseHelper = false;
        return false;
    }
    Log("ngx-b2: helper connected pid=%lu", GetProcessId(g_b2Helper));
    return true;
}

struct B2SetupMsg {
    unsigned long long hColor, hOut, hFIn, hFOut;
    unsigned int w, h, fmt;
    unsigned int pad = 0;
    unsigned long long startVal = 0; // epoch-sync for helper fence loop
};

static bool B2SendSetup(UINT w, UINT h, DXGI_FORMAT fmt)
{
    if (!B2StartHelper()) return false;

    auto dupInto = [](HANDLE nt, unsigned long long* outVal) -> bool {
        if (!nt) return false;
        HANDLE val = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), nt, g_b2Helper, &val, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return false;
        *outVal = (unsigned long long)(uintptr_t)val;
        return true;
    };
    static unsigned long long s_fInVal = 0, s_fOutVal = 0;
    B2SetupMsg m = {};
    if (!dupInto(g_b2HColor, &m.hColor) || !dupInto(g_b2HOut, &m.hOut)) {
        Log("ngx-b2: DuplicateHandle(color/out) FAILED err=%lu", GetLastError());
        return false;
    }
    // Fences persist across resizes: duplicate ONCE, reuse values after.
    if (s_fInVal && s_fOutVal) {
        m.hFIn = s_fInVal; m.hFOut = s_fOutVal;
    } else if (!dupInto(g_b2HFIn, &m.hFIn) || !dupInto(g_b2HFOut, &m.hFOut)) {
        Log("ngx-b2: DuplicateHandle(fences) FAILED err=%lu", GetLastError());
        return false;
    } else { s_fInVal = m.hFIn; s_fOutVal = m.hFOut; }
    m.w = w; m.h = h; m.fmt = (unsigned)fmt;
    m.startVal = g_b2Val + 1; // next frame index helper should expect
    const char tagS = 0x53; // 'S'
    DWORD wa = 0, wb = 0, wr = 0, rd = 0;
    if (!WriteFile(g_b2Pipe, &tagS, 1, &wa, nullptr) ||
        !WriteFile(g_b2Pipe, &m, sizeof(m), &wb, nullptr)) return false;
    unsigned int resp = 0;
    if (!ReadFile(g_b2Pipe, &resp, sizeof(resp), &rd, nullptr) || resp != 0x59414B4F) {
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
    if (g_b2HFIn)   { CloseHandle(g_b2HFIn);   g_b2HFIn = nullptr; }
    if (g_b2HFOut)  { CloseHandle(g_b2HFOut);  g_b2HFOut = nullptr; }
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
            Log("ngx-b2: helper setup FAILED - disabling helper this session");
            g_b2UseHelper = false;
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
    ip.perfQuality = g_cfg.perfQuality;
    ip.mvJittered = g_cfg.mvJittered != 0;
    ip.autoExposure = g_cfg.autoExposure != 0;
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
    if (!EnsureNgxBridgeB2(w, h, fmt)) return;
    if (!g_b2UseHelper) B2EnsureDummyInputs(w, h);

    ++g_b2Val;
    UINT64 v = g_b2Val;

    static ID3D12CommandAllocator*    s_alA = nullptr; static ID3D12GraphicsCommandList* s_clA = nullptr;
    static ID3D12CommandAllocator*    s_alB = nullptr; static ID3D12GraphicsCommandList* s_clB = nullptr;
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
            FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alB))) ||
            FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alB, nullptr, IID_PPV_ARGS(&s_clB)))) {
            Log("ngx-b2: alloc/list creation FAILED");
            s_gq = nullptr;
            return;
        }
        s_clA->Close(); s_clB->Close();
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
        static unsigned s_wDiag = 0;
        const char tagF = 0x46; // 'F'
        DWORD wa = 0, wb = 0;
        bool wrOK = WriteFile(g_b2Pipe, &tagF, 1, &wa, nullptr) &&
                    WriteFile(g_b2Pipe, &v, sizeof(v), &wb, nullptr);
        if (++s_wDiag <= 3)
            Log("ngx-b2: frame msg write v=%llu ok=%d err=%lu",
                (unsigned long long)v, (int)wrOK, wrOK ? 0UL : GetLastError());
        unsigned long long ack = 0;
        DWORD rd = 0, have = 0;
        for (int spin = 0; spin < 250 && have < sizeof(ack); ++spin) {
            if (!PeekNamedPipe(g_b2Pipe, nullptr, 0, nullptr, &rd, nullptr)) break;
            if (rd > have) {
                DWORD want = sizeof(ack) - have;
                if (!ReadFile(g_b2Pipe, ((BYTE*)&ack) + have, want, &rd, nullptr)) break;
                have += rd;
                if (have >= sizeof(ack)) break;
            }
            Sleep(1);
        }
        if (have == sizeof(ack) && ack == v)
            helperAcked = true;
    }

    // STAGE 3 (game): only blit when helper CONFIRMED fOut signal enqueue.
    bool recorded = false;
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
        // Helper didn't ack (or local failed): skip blit, bb untouched.
        static unsigned s_skips = 0;
        if ((++s_skips % 120) == 1)
            Log("ngx-b2: frame %llu skipped (no ack/eval) x%u",
                (unsigned long long)v, s_skips);
        bb->Release();
        return;
    }
    // GPU-side ordering vs helper's writes is safe now: the ack proves the
    // Signal(fOut,v) was ENQUEUED on the helper queue before we submit this.
    s_gq->Wait(g_b2FenceOutG, v);
    s_clB->Reset(s_alB, nullptr);
    D3D12_RESOURCE_BARRIER b3 = {};
    b3.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b3.Transition.pResource = bb;
    b3.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b3.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b3.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_clB->ResourceBarrier(1, &b3);
    D3D12_TEXTURE_COPY_LOCATION d3 = { bb,       D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION s3 = { g_b2OutG, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    s_clB->CopyTextureRegion(&d3, 0, 0, 0, &s3, nullptr);
    b3.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b3.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    s_clB->ResourceBarrier(1, &b3);
    s_clB->Close();
    ID3D12CommandList* l3[] = { s_clB };
    s_gq->ExecuteCommandLists(1, l3);

    static unsigned s_brFrames = 0;
    if (++s_brFrames <= 5 || (s_brFrames % 600) == 0)
        Log("ngx-b2: frame %u eval=%s", s_brFrames, recorded ? "ok" : "SKIP");
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
    auto it = g_resourceStates.find(bb);
    if (it == g_resourceStates.end()) {
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
            if (depthStale) { g_depthResource = nullptr; g_depthValid = false; }
            if (mvStale) { g_mvResource = nullptr; g_mvValid = false; }
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

    D3D12_RESOURCE_STATES bbState = it->second;
    auto dit = g_resourceStates.find(g_depthResource);
    auto mit = g_resourceStates.find(g_mvResource);
    D3D12_RESOURCE_STATES depthState = dit != g_resourceStates.end() ? dit->second : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mvState = mit != g_resourceStates.end() ? mit->second : D3D12_RESOURCE_STATE_COMMON;

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
                auto ri = g_rtvMap.find(g_mvLastRtvKey);
                if (ri != g_rtvMap.end() && ri->second) {
                    StoreTracked(&g_mvResource, ri->second);
                    g_mvValid = true;
                    g_mvStamp = g_frameCounter;
                    if (!g_mvFirstValidFrame) g_mvFirstValidFrame = g_frameCounter;
                    g_resourceStates[g_mvResource] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    Log("hooks: MV re-adopted from registry %p", (void*)ri->second);
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
                StoreTracked(&g_depthResource, nullptr); g_depthValid = false; g_depthStamp = 0;
                StoreTracked(&g_mvResource, nullptr); g_mvValid = false; g_mvStamp = 0;
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
            if (sc != g_swapchain) {
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
            if (g_dlaaMode && !g_passiveMode) {
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

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    // Unconditional first-call proof: if THIS never logs, nothing on earth
    // is calling dxgi's public Present in this process.
    {
        static volatile LONG s_first = 0;
        if (InterlockedCompareExchange(&s_first, 1, 0) == 0)
            Log("Hook_Present ENTER sc=%p sync=%u flags=%u", (void*)sc, syncInterval, flags);
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
            if (sc == g_egshDummySC) { /* EGSH self-test present: never adopt */ }
            else if (sc != g_swapchain) {
                g_swapchain = sc;
                Log("hooks: present on real swapchain %p (format %d)", (void*)sc, (int)g_bbFormat);
            }
            // SELF-CONTAINED NGX PIPELINE ENTRY: the ECL hook is build-disabled,
            // so Present IS the per-frame driver.
            if (g_dlaaMode && !g_passiveMode && g_swapchain) {
                __try {
                    InjectAtPresentImpl(nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    static int s_presFault = 0;
                    if (++s_presFault <= 5)
                        Log("ngx-pipe: present-path guarded fault #%d", s_presFault);
                }
            }
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
            if (sc == (IDXGISwapChain1*)g_egshDummySC) { /* EGSH self-test present */ }
            else if (sc != g_swapchain) {
                g_swapchain = sc;
                Log("hooks: present on real swapchain %p (format %d)", (void*)sc, (int)g_bbFormat);
            }
            // SELF-CONTAINED NGX PIPELINE ENTRY (Present1 variant)
            if (g_dlaaMode && !g_passiveMode && g_swapchain) {
                __try {
                    InjectAtPresentImpl(nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    static int s_pres1Fault = 0;
                    if (++s_pres1Fault <= 5)
                        Log("ngx-pipe: present1-path guarded fault #%d", s_pres1Fault);
                }
            }
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

                // GetDevice from the queue → real ID3D12Device
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
                        // NOTE: no EnsureUpscalerInit here - this runs on the
                        // engine ECL thread; NGX init races the Present thread
                        // (double-init corrupted NVIDIA global state).
                        // ISOLATION (reviewer #16 pattern): SCALENG_NO_JITTER=1
                        // disables the CB patch entirely - single-variable test
                        // for whether jitter writing triggers nvwgf2umx AVs.
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

static void CopyTexBody(ID3D12GraphicsCommandList* list,
                        const D3D12_TEXTURE_COPY_LOCATION* dst, UINT dstX, UINT dstY,
                        UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION* src,
                        const D3D12_BOX* srcBox)
{
    // SAFETY: skip ALL analysis when bridge not ready (prevents GetDesc on
    // engine resources during unstable startup / resource churn)
    if (!g_bridgeReady || !g_dlaaMode) return;
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
        if (newNode)
            g_lastNewChainFrame = g_frameCounter; // new node entered the chain
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
                        if (!Local::AltIsPairHalf(g_sceneColorAlt))
                            g_sceneColorAlt = nullptr;
                    }
                    if (!srcIsTracked && !g_sceneColorAlt) {
                        StoreTracked(&g_sceneColorAlt, src->pResource);
                        g_resourceStates[g_sceneColorAlt] = D3D12_RESOURCE_STATE_COMMON;
                        Log("hooks: terminal pair node adopted as ALT %p (f10)", (void*)src->pResource);
                    } else if (!dstIsTracked && !g_sceneColorAlt) {
                        StoreTracked(&g_sceneColorAlt, dst->pResource);
                        g_resourceStates[g_sceneColorAlt] = D3D12_RESOURCE_STATE_COMMON;
                        Log("hooks: terminal pair node adopted as ALT %p (f10 dst)", (void*)dst->pResource);
                    } else if (!altIsPairHalf && g_sceneColorAlt &&
                               g_sceneColorAlt != src->pResource && g_sceneColorAlt != dst->pResource) {
                        ID3D12Resource* oldAlt = g_sceneColorAlt;
                        (void)oldAlt;
                        ID3D12Resource* cand = srcIsTracked ? dst->pResource : src->pResource;
                        StoreTracked(&g_sceneColorAlt, cand);
                        g_resourceStates[g_sceneColorAlt] = D3D12_RESOURCE_STATE_COMMON;
                        Log("hooks: terminal pair REPLACED non-pair ALT -> %p (f10)", (void*)cand);
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
            // A display-sized UNORM src here is the scene color - adopt it.
            if (!g_sceneColorValid && !isMvDst) {
                D3D12_RESOURCE_DESC sd = src->pResource->GetDesc();
                if (sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    sd.MipLevels == 1 &&
                    sd.Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
                    StoreTracked(&g_sceneColor, src->pResource);
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
            bool quietNow = ((int)(g_frameCounter - g_quietUntilFrame) < 0);
            if (!quietNow && !isSceneSrc && !isMvDst && dst->pResource != g_dlssOut) {
                if (!g_depthValid) g_depthFirstValidFrame = g_frameCounter;
                StoreTracked(&g_depthResource, dst->pResource);
                g_depthValid = true;
                g_depthStamp = g_frameCounter;
                {
                    D3D12_RESOURCE_DESC dd = dst->pResource->GetDesc();
                    g_depthRealFmt = dd.Format;
                    g_depthMsaa = dd.SampleDesc.Count != 1;
                }
                BookGuard _bgCopy;
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

void Hook_RSSetViewports(ID3D12GraphicsCommandList* list, UINT numViewports,
                         const D3D12_VIEWPORT* pViewports)
{
    // PASSTHROUGH unless bridge ready + DLAA on
    if (!g_bridgeReady || !g_dlaaMode) {
        Real_RSSetViewports(list, numViewports, pViewports);
        return;
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
    // SAFETY: skip tracking when bridge not ready (prevents map writes during churn)
    if (!g_bridgeReady || !g_dlaaMode) {
        Real_ResourceBarrier(list, numBarriers, pBarriers);
        return;
    }
    if (pBarriers && numBarriers > 0) {
        for (UINT i = 0; i < numBarriers; ++i) {
            if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                ID3D12Resource* res = pBarriers[i].Transition.pResource;
                if (res) {
                    { BookGuard _bgRb; g_resourceStates[res] = pBarriers[i].Transition.StateAfter; }
                    // LIVENESS: the engine transitions depth/MV every frame it
                    // uses them. Refresh stamps so the staleness gate (which
                    // protects against freed resources) only trips on real
                    // renderer transitions, not on normal steady-state play.
                    if (res == g_depthResource)
                        g_depthStamp = g_frameCounter;
                    if (res == g_mvResource || res == g_mvResourceAlt)
                        g_mvStamp = g_frameCounter;
                }
            }
        }
    }
    Real_ResourceBarrier(list, numBarriers, pBarriers);
}

void Hook_SetDescriptorHeaps(ID3D12GraphicsCommandList* list, UINT numHeaps,
                             ID3D12DescriptorHeap* const* heaps)
{
    // PASSTHROUGH: heap save/restore causes solid-color artifacts when the
    // saved state from one command list is restored onto another.
    Real_SetDescriptorHeaps(list, numHeaps, heaps);
}

void Hook_OMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT numRenderTargets,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargets,
                             BOOL RTsSingleHandleToDescriptorRange,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    // SAFETY: skip tracking when bridge not ready (prevents map writes during churn)
    if (!g_bridgeReady || !g_dlaaMode) {
        Real_OMSetRenderTargets(list, numRenderTargets, pRenderTargets,
                                RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
        return;
    }
    if (numRenderTargets >= 1 && pRenderTargets) {
        g_boundRtv = pRenderTargets[0];
        g_boundRtvValid = true;
        g_boundRtvResource = nullptr;
        BookGuard _bgOmrt;
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
                    StoreTracked(&g_sceneColor, g_boundRtvResource);
                    g_sceneColorRtv = pRenderTargets[0];
                    g_sceneColorValid = true;
                    AdoptDisplaySize((unsigned int)rd.Width, (unsigned int)rd.Height);
                    g_resourceStates[g_sceneColor] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    Log("hooks: scene color adopted from RTV bind %p (%ux%u)", (void*)g_sceneColor,
                        (unsigned int)rd.Width, (unsigned int)rd.Height);
                }
            }
            // MV TRACK-BY-BIND: the engine binds MV as an RTV every frame it
            // renders it. Adopting here always holds the CURRENT texture -
            // immune to the rotation that caused repeated stale-MV faults.
            if (g_boundRtvResource && g_loadPhase == 0 &&
                g_boundRtvResource != g_mvResource && g_boundRtvResource != g_mvResourceAlt) {
                BookGuard _bgOmrt2;
            auto ri = g_rtvMap.find(pRenderTargets[0].ptr);
                if (ri != g_rtvMap.end() && ri->second == g_boundRtvResource) {
                    D3D12_RESOURCE_DESC mrd = g_boundRtvResource->GetDesc();
                    if (mrd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                        mrd.MipLevels == 1 && mrd.SampleDesc.Count == 1 &&
                        (unsigned)mrd.Width == g_displayW && mrd.Height == g_displayH &&
                        mrd.Format == DXGI_FORMAT_R16G16_FLOAT) {
                        bool first = !g_mvValid;
                        StoreTracked(&g_mvResource, g_boundRtvResource);
                        g_mvValid = true;
                        g_mvStamp = g_frameCounter;
                        if (first) g_mvFirstValidFrame = g_frameCounter;
                        g_resourceStates[g_mvResource] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    }
                }
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
                g_resourceStates[g_sceneColor] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                Log("hooks: scene color refreshed on bind %p", (void*)g_sceneColor);
                D3D12_RESOURCE_DESC rd = g_sceneColor->GetDesc();
                if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
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
    // Once the GAME's device is captured, later creators are INTERNAL
    // libraries (the NGX core calls D3D12CreateDevice during Init). Those
    // must be passthrough too - capturing NGX's internal device as
    // g_device poisoned every game-device assumption and cascaded into
    // DEVICE_REMOVED after each successful pInit.
    if (g_device) {
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
        g_device = newDev;
        if (reinstallHooks) {
            // DISABLED FOR BISECTION: re-enable one at a time
            if (false) {
            // PURE VTABLE SWAP: never patch driver code bytes. Just redirect
            // the vtable pointer to our hook and save the original for
            // forwarding. The original function code is untouched.
            void** vtbl = *(void***)g_device;

            // Make vtable page writable (it may be read-only)
            MEMORY_BASIC_INFORMATION mbi = {};
            VirtualQuery(vtbl, &mbi, sizeof(mbi));
            DWORD oldProt = 0;
            if (VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_READWRITE, &oldProt)) {
                // Save originals and swap
                Real_CreateRenderTargetView = (PFN_CreateRenderTargetView)vtbl[20];
                Real_CreateShaderResourceView = (PFN_CreateShaderResourceView)vtbl[18];
                vtbl[20] = (void*)&Hook_CreateRenderTargetView;
                vtbl[18] = (void*)&Hook_CreateShaderResourceView;
                VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProt, &oldProt);
                Log("hooks: device vtable SWAPPED (20, 18) - no code patched");
            } else {
                Log("hooks: VirtualProtect on vtable failed - device hooks not installed");
            }
            {
                void* targets[2] = { (void*)Hook_CreateRenderTargetView, (void*)Hook_CreateShaderResourceView };
                CfgMarkValid(targets, 2);
            }
            } // end disabled device vtable swap
        }

        ID3D12CommandQueue* queue = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue) {
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

void InstallCommandListHooks(ID3D12GraphicsCommandList* list)
{
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
    Log("hooks: config applied (dlaa=%d)", g_dlaaMode ? 1 : 0);
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
    return g_lastNewChainFrame ? (g_frameCounter - g_lastNewChainFrame) : 0;
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
