#include "dlss_ngx.h"
#include <dxgi.h>
#include <d3d12.h>
#include "log.h"
#include "d3d12_hooks.h"

#include <map>

// Genuine D3D12CreateDevice trampoline (defined in d3d12_hooks.cpp)
extern PFN_ScaleNG_CreateDevice Real_D3D12CreateDevice_Tramp;
#include <string>

// ---------------------------------------------------------------------------
// Own classic-API parameter object implementing the OFFICIAL NVSDK_NGX_Parameter
// vtable layout (nvsdk_ngx_params.h declaration order = slot order).
//
// WHY: the driver core's AllocateParameters object uses a NON-standard layout
// (probed black-box: UI=3/11 works per header, but float lives at 5/13 and
// resources at 7/15 - NOT the header's 1/9 and 6/14). Writing floats/resources
// through header slots on THAT object silently no-ops, so the snippet read
// empty values (MVScaleX=0, ColorExtentWidth=0) and EvaluateFeature returned
// 0xBAD00005 forever. Passing OUR OWN header-layout object makes the core +
// snippet read every parameter correctly - proven in test_mini.cpp.
// ---------------------------------------------------------------------------
union NgxV { unsigned long long u; float f; double d; int i; unsigned int ui; void* p; };
struct NgxVal { int t; NgxV v; };

class NgxParamStore
{
public:
    std::map<std::string, NgxVal> m;

    virtual void SetULL(const char* k, unsigned long long v) { NgxVal n; n.t = 0; n.v.u = v; m[k] = n; }
    virtual void SetF(const char* k, float v) { NgxVal n; n.t = 1; n.v.f = v; m[k] = n; }
    virtual void SetD(const char* k, double v) { NgxVal n; n.t = 2; n.v.d = v; m[k] = n; }
    virtual void SetUI(const char* k, unsigned int v) { NgxVal n; n.t = 3; n.v.ui = v; m[k] = n; }
    virtual void SetI(const char* k, int v) { NgxVal n; n.t = 4; n.v.i = v; m[k] = n; }
    virtual void SetR11(const char* k, void* v) { NgxVal n; n.t = 5; n.v.p = v; m[k] = n; }
    virtual void SetR12(const char* k, void* v) { NgxVal n; n.t = 6; n.v.p = v; m[k] = n; }
    virtual void SetPtr(const char* k, void* v) { NgxVal n; n.t = 7; n.v.p = v; m[k] = n; }
    virtual int GetULL(const char* k, unsigned long long* o) const { auto it = m.find(k); if (it == m.end()) return 0; *o = it->second.v.u; return 1; }
    virtual int GetF(const char* k, float* o) const { auto it = m.find(k); if (it == m.end()) return 0; *o = it->second.v.f; return 1; }
    virtual int GetD(const char* k, double* o) const { auto it = m.find(k); if (it == m.end()) return 0; *o = it->second.v.d; return 1; }
    virtual int GetUI(const char* k, unsigned int* o) const { auto it = m.find(k); if (it == m.end()) return 0; *o = it->second.v.ui; return 1; }
    virtual int GetI(const char* k, int* o) const { auto it = m.find(k); if (it == m.end()) return 0; *o = it->second.v.i; return 1; }
    virtual int GetR11(const char* k, void** o) const { auto it = m.find(k); if (it == m.end()) return 0; *o = it->second.v.p; return 1; }
    virtual int GetR12(const char* k, void** o) const { auto it = m.find(k); if (it == m.end()) return 0; *o = it->second.v.p; return 1; }
    virtual int GetPtr(const char* k, void** o) const { auto it = m.find(k); if (it == m.end()) return 0; *o = it->second.v.p; return 1; }
    virtual void ResetAll() { m.clear(); }
    // Slots 17-26: the core/snippet may call beyond the documented interface
    // (destructor pair, future methods). Without padding, those vtable reads
    // hit garbage and CreateFeature dies with PlatformError. Harness-proven.
    virtual void X1() {}
    virtual void X2() {}
    virtual void X3() {}
    virtual void X4() {}
    virtual void X5() {}
    virtual void X6() {}
    virtual void X7() {}
    virtual void X8() {}
    virtual void X9() {}
    virtual void X10() {}
};

namespace {
void NgxModuleAnchor() {}

// NGX app log callback (0x14+): may fire on any thread, concurrently.
// Bounded copy into Log (thread-safe spinlock writer); volume-capped.
static void __cdecl NgxAppLogCallback(const char* message, int level, int component)
{
    if (!message) return;
    static volatile LONG s_ngxLogN = 0;
    LONG n = InterlockedIncrement(&s_ngxLogN);
    if (n > 40 && (n % 200) != 0) return;
    char buf[512];
    size_t i = 0;
    while (message[i] && i + 1 < sizeof(buf)) { buf[i] = message[i]; ++i; }
    buf[i] = '\0';
    Log("NGX: %s", buf);
    (void)level; (void)component;
}

// FeatureCommonInfo for Init_Ext (the path working 310.x integrations use):
// feature discovery paths + verbose app logging. Statics outlive all Init
// calls (NGX may retain the pointers for later feature discovery).
static wchar_t s_ngxFeatureDir[MAX_PATH] = {};
static const wchar_t* s_ngxFeaturePaths[1] = {};
static NVSDK_NGX_FeatureCommonInfo_Local s_ngxFcInfo = {};
static bool BuildNgxFeatureInfo(const wchar_t* dllPath)
{
    if (!dllPath || !*dllPath) return false;
    size_t len = 0;
    while (dllPath[len]) ++len;
    size_t cut = len;
    for (size_t i = 0; i < len; ++i)
        if (dllPath[i] == L'\\') cut = i + 1;
    size_t n = 0;
    while (n + 1 < MAX_PATH && n < cut) { s_ngxFeatureDir[n] = dllPath[n]; ++n; }
    s_ngxFeatureDir[n] = L'\0';
    if (!s_ngxFeatureDir[0]) return false;
    s_ngxFeaturePaths[0] = s_ngxFeatureDir;
    s_ngxFcInfo.PathListInfo.Path = s_ngxFeaturePaths;
    s_ngxFcInfo.PathListInfo.Length = 1;
    s_ngxFcInfo.InternalData = nullptr;
    s_ngxFcInfo.LoggingInfo.LoggingCallback = NgxAppLogCallback;
    s_ngxFcInfo.LoggingInfo.MinimumLoggingLevel = (int)NVSDK_NGX_LOGGING_LEVEL_VERBOSE_L;
    s_ngxFcInfo.LoggingInfo.DisableOtherLoggingSinks = false;
    return true;
}

// SEH wrapper for NGX Init_Ext - standalone (no C++ unwinding in caller).
static int SafeNgxInitExt(PFN_NVSDK_NGX_D3D12_Init_Ext f, unsigned long long appId,
                          const wchar_t* dataPath, ID3D12Device* dev,
                          NVSDK_NGX_Version version,
                          const NVSDK_NGX_FeatureCommonInfo_Local* fc,
                          unsigned* outCode)
{
    if (!f) return -1;
    __try {
        NVSDK_NGX_Result r = f(appId, dataPath, dev, version, fc);
        return (int)r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCode = (unsigned)GetExceptionCode();
        return -2;
    }
}

void CopyW(wchar_t* dst, size_t cap, const wchar_t* src)
{
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
    dst[i] = L'\0';
}

void AppendW(wchar_t* dst, size_t cap, const wchar_t* src)
{
    size_t base = 0;
    while (dst[base]) ++base;
    size_t i = 0;
    while (src[i] && base + i + 1 < cap) { dst[base + i] = src[i]; ++i; }
    if (base + i < cap) dst[base + i] = L'\0';
}

// The DLSS SR snippet (nvngx_dlss.dll) must sit next to the module that calls
// the NGX core (the core searches the calling module's directory first). This
// plugin is the calling module, so its own directory is the primary target.
}

NvDlssUpscaler::NvDlssUpscaler() = default;

NvDlssUpscaler::~NvDlssUpscaler()
{
    Shutdown();
}

bool NvDlssUpscaler::LoadNGX(const wchar_t* dllPath)
{
    // NGX searches for feature snippets beside the PROCESS EXE (plus driver
    // store). Games ship nvngx_dlss.dll there; our copy lives in plugins\,
    // which the core never scans -> CreateFeature returned NotInitialized.
    // Self-host: mirror the snippet next to the exe EVERY init.
    // A stale/partial/corrupt copy from an interrupted session previously
    // caused FAIL_UnableToInitializeFeature (0xBAD0000B) - only-if-missing
    // copying left the bad file in place forever.
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wchar_t dst[MAX_PATH] = {};
        lstrcpyW(dst, exePath);
        lstrcatW(dst, L"nvngx_dlss.dll");
        if (dllPath && *dllPath) {
            BOOL copied = CopyFileW(dllPath, dst, FALSE); // always overwrite
            DWORD srcAttr = GetFileAttributesW(dllPath);
            DWORD dstAttr = GetFileAttributesW(dst);
            Log("DLSS: snippet mirror src=%ls exists=%d -> dst=%ls exists=%d copyOK=%d",
                dllPath, (srcAttr != INVALID_FILE_ATTRIBUTES),
                dst, (dstAttr != INVALID_FILE_ATTRIBUTES), (int)copied);
        } else {
            Log("DLSS: no dlssDllPath provided - snippet mirror skipped (dst may be stale)");
        }
    }
    // PART 1: preload nvapi64.dll BEFORE the NGX core. Driver 596.49's
    // nvapi64.dll is a stripped "direct mode" shim (exports only
    // nvapi_QueryInterface + nvapi_Direct_GetMethod); without this preload
    // the core's NVSDK_NGX_D3D12_Init fails with 0xBAD00001.
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    Log("DLSS: preloaded nvapi64.dll = %p", (void*)nvapi);
    // NvAPI_Initialize probe (once): the NGX platform layer may depend on an
    // initialized NvAPI (classic Init failure analysis). Discovery uses
    // NVIDIA's public nvapi_QueryInterface mechanism (0x0150E828 =
    // NvAPI_Initialize). x64 ABI: no calling-convention risk. Read-only vs
    // the game (standard API init, SEH-wrapped, result only logged).
    {
        static volatile LONG s_nvapiOnce = 0;
        if (InterlockedCompareExchange(&s_nvapiOnce, 1, 0) == 0 && nvapi) {
            typedef void* (__cdecl* PFN_nvapi_QI)(unsigned int id);
            PFN_nvapi_QI qi = (PFN_nvapi_QI)GetProcAddress(nvapi, "nvapi_QueryInterface");
            Log("DLSS: nvapi_QueryInterface=%p", (void*)qi);
            if (qi) {
                typedef int (__cdecl* PFN_NvAPI_Initialize)(void);
                PFN_NvAPI_Initialize pInitNv = (PFN_NvAPI_Initialize)qi(0x0150E828);
                Log("DLSS: NvAPI_Initialize=%p", (void*)pInitNv);
                if (pInitNv) {
                    int st = -9999;
                    __try { st = pInitNv(); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { st = -1000 - (int)GetExceptionCode(); }
                    Log("DLSS: NvAPI_Initialize result=%d", st);
                }
            }
        }
    }

    // Enable the NGX core's own log (C:\ProgramData\NVIDIA\NGX\models\nvngx.log)
    // BEFORE loading it - it names the exact parameter on EvaluateFeature
    // rejections, which ScaleNG.log cannot see.
    SetEnvironmentVariableA("__NGX_LOG_LEVEL", "3");
    // SIMCLASS experiment (forum 384236) REVERTED 2026-10-07: identical
    // 0xBAD00002 on this box (run 20261007T163545Z). No behavior remnant.
    // NOTE (2026-10-04): __NGX_DISABLE_UPDATER intentionally NOT set — the
    // 310.x NGX Update Module resolves feature DLLs, and disabling it is a
    // suspect in Init PlatformError. Reversible one-liner.

    // PART 2: locate and load the driver's NGX core (nvngx.dll in the driver
    // store). It exports the classic API: Init / AllocateParameters /
    // CreateFeature / EvaluateFeature / Shutdown.
    // 2026-10-07 fix (reviewer hypothesis B CONFIRMED): the old code hardcoded
    // only nvlti.inf_*, but this machine's core lives under nvltsi.inf_* (other
    // drivers use nv_dispi.inf_* etc.). Enumerate every nv*.inf_* dir and load
    // the first nvngx.dll found there. Verified live: FileRepository census
    // found nvltsi.inf_amd64_...\nvngx.dll (489KB core vs 74MB snippet).
    {
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv*.inf_*", &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            Log("DLSS: driver-store INF census failed err=%lu", GetLastError());
        }
        while (hFind != INVALID_HANDLE_VALUE) {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                fd.cFileName[0] != L'.') {
                wchar_t cand[MAX_PATH] = {};
                CopyW(cand, MAX_PATH, L"C:\\Windows\\System32\\DriverStore\\FileRepository\\");
                AppendW(cand, MAX_PATH, fd.cFileName);
                AppendW(cand, MAX_PATH, L"\\nvngx.dll");
                DWORD attr = GetFileAttributesW(cand);
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    m_ngxDll = LoadLibraryW(cand);
                    if (m_ngxDll) {
                        CopyW(m_dlssPath, MAX_PATH, cand);
                        Log("DLSS: loaded driver core nvngx.dll (%ls)", cand);
                        break;
                    }
                }
            }
            if (!FindNextFileW(hFind, &fd)) break;
        }
        if (hFind != INVALID_HANDLE_VALUE) FindClose(hFind);
    }
    if (!m_ngxDll) {
        // Fallback: System32 (older driver layouts).
        wchar_t sysDir[MAX_PATH] = {};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        wchar_t nvngx[MAX_PATH] = {};
        CopyW(nvngx, MAX_PATH, sysDir);
        AppendW(nvngx, MAX_PATH, L"\\nvngx.dll");
        m_ngxDll = LoadLibraryW(nvngx);
        if (m_ngxDll) {
            CopyW(m_dlssPath, MAX_PATH, nvngx);
            Log("DLSS: loaded driver core nvngx.dll from System32");
        }
    }
    if (!m_ngxDll && dllPath && *dllPath) {
        // Last resort: user-provided classic-API nvngx module.
        m_ngxDll = LoadLibraryW(dllPath);
        if (m_ngxDll) {
            CopyW(m_dlssPath, MAX_PATH, dllPath);
            Log("DLSS: loaded user nvngx module %ls", dllPath);
        }
    }
    if (!m_ngxDll) {
        Log("DLSS: FAILED to load the driver NGX core (no NVIDIA driver store core found)");
        return false;
    }
    if (BuildNgxFeatureInfo(m_dlssPath))
        Log("DLSS: feature discovery path + NGX app logging armed");
    else
        Log("DLSS: feature discovery path unavailable - Init_Ext will be skipped");

    pInit = (PFN_NVSDK_NGX_D3D12_Init)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_Init");
    pInitExt = (PFN_NVSDK_NGX_D3D12_Init_Ext)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_Init_Ext");
    pAllocateParameters = (PFN_NVSDK_NGX_D3D12_AllocateParameters)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_AllocateParameters");
    pCreateFeature = (PFN_NVSDK_NGX_D3D12_CreateFeature)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_CreateFeature");
    pEvaluateFeature = (PFN_NVSDK_NGX_D3D12_EvaluateFeature)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_EvaluateFeature");
    pShutdown = (PFN_NVSDK_NGX_D3D12_Shutdown)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_Shutdown");
    pGetParameters = (PFN_NVSDK_NGX_D3D12_GetParameters)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_GetParameters");

    // AllocateParameters is OPTIONAL: the integration never calls it (feature
    // creation uses our own header-layout NgxParamStore because the core's
    // object has a non-standard vtable). Recent DLSS redistributables
    // (verified live: nvngx_dlss.dll exports Init/Create/Evaluate/Shutdown
    // but not AllocateParameters) no longer ship it. Requiring it here
    // fails load for an export nothing dereferences.
    if (!pAllocateParameters)
        Log("DLSS: AllocateParameters absent - continuing with NgxParamStore (never called)");

    if (!pInit || !pCreateFeature || !pEvaluateFeature || !pShutdown) {
        Log("DLSS: driver core missing exports: init=%p alloc=%p create=%p eval=%p shutdown=%p",
            (void*)pInit, (void*)pAllocateParameters, (void*)pCreateFeature,
            (void*)pEvaluateFeature, (void*)pShutdown);
        UnloadNGX();
        return false;
    }
    return true;
}

void NvDlssUpscaler::UnloadNGX()
{
    if (m_ngxDll) {
        FreeLibrary(m_ngxDll);
        m_ngxDll = nullptr;
    }
    pInit = nullptr;
    pInitExt = nullptr;
    pAllocateParameters = nullptr;
    pCreateFeature = nullptr;
    pEvaluateFeature = nullptr;
    pShutdown = nullptr;
    pGetParameters = nullptr;
    pSetUI = nullptr;
    pSetI = nullptr;
    pSetF = nullptr;
    pSetULL = nullptr;
    pSetResource = nullptr;
    pSetPtr = nullptr;
    pGetUI = nullptr;
    m_paramSlotsResolved = false;
}

bool NvDlssUpscaler::Init(const UpscalerInitParams& params)
{
    if (m_initialized)
        return true;
    if (!params.device)
        return false;

    m_device = params.device;
    m_renderWidth = params.renderWidth;
    m_renderHeight = params.renderHeight;
    m_displayWidth = params.displayWidth;
    m_displayHeight = params.displayHeight;
    m_appId = params.appId;
    m_ngxVersion = params.ngxApiVersion ? params.ngxApiVersion : (uint32_t)NVSDK_NGX_Version_API;
    m_perfQuality = params.perfQuality;
    m_mvJittered = params.mvJittered;
    m_autoExposure = params.autoExposure;
    m_depthInverted = params.depthInverted;
    m_firstEvaluate = true;

    if (!LoadNGX(params.dlssDllPath))
        return false;

    // Data path = the ASI module's directory (NGX writes its logs/caches there).
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&NgxModuleAnchor, &self);
    wchar_t moduleDir[MAX_PATH] = {};
    if (self) GetModuleFileNameW(self, moduleDir, MAX_PATH);
    size_t len = 0;
    while (moduleDir[len]) ++len;
    size_t cut = len;
    for (size_t i = 0; i < len; ++i)
        if (moduleDir[i] == L'\\') cut = i + 1;
    moduleDir[cut] = L'\0';
    // Data path: dedicated fresh subdir (stale-state interference variant,
    // 2026-10-07). Previously the shared models dir itself; that dir is
    // writable (verified) and results were identical, so this tests whether
    // NGX wants a dir it owns. One variable; reversible by reverting these
    // lines. Falls back to the shared dir if creation fails.
    CopyW(m_ngxDataPath, MAX_PATH, L"C:\\ProgramData\\NVIDIA\\NGX\\models\\ScaleNG_ngx");
    if (!CreateDirectoryW(m_ngxDataPath, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        Log("DLSS: data subdir not creatable - falling back to shared models dir");
        CopyW(m_ngxDataPath, MAX_PATH, L"C:\\ProgramData\\NVIDIA\\NGX\\models");
    } else {
        Log("DLSS: data path %ls", m_ngxDataPath);
    }

    m_initialized = true;
    return true;
}

void NvDlssUpscaler::ResolveParamSlots()
{
    pSetULL = nullptr;
    pSetF = nullptr;
    pSetUI = nullptr;
    pSetI = nullptr;
    pSetResource = nullptr;
    pSetPtr = nullptr;
    pGetUI = nullptr;
    m_paramSlotsResolved = false;

    if (!m_parameters)
        return;
    void* const* vt = *(void* const**)m_parameters;
    if (!vt)
        return;
    pSetULL = (PFN_NVSDK_NGX_Parameter_SetULL)vt[NGX_PARAM_SLOT_SET_ULL];
    pSetF = (PFN_NVSDK_NGX_Parameter_SetF)vt[NGX_PARAM_SLOT_SET_F];
    pSetUI = (PFN_NVSDK_NGX_Parameter_SetUI)vt[NGX_PARAM_SLOT_SET_UI];
    pSetI = (PFN_NVSDK_NGX_Parameter_SetI)vt[NGX_PARAM_SLOT_SET_I];
    pSetResource = (PFN_NVSDK_NGX_Parameter_SetD3d12Resource)vt[NGX_PARAM_SLOT_SET_RES];
    pSetPtr = (PFN_NVSDK_NGX_Parameter_SetVoidPointer)vt[NGX_PARAM_SLOT_SET_PTR];
    pGetUI = (PFN_NVSDK_NGX_Parameter_GetUI)vt[NGX_PARAM_SLOT_GET_UI];
    m_paramSlotsResolved = pSetULL && pSetF && pSetUI && pSetI && pSetResource && pSetPtr;
    if (!m_paramSlotsResolved) {
        Log("DLSS: params vtable slots: ull=%p f=%p ui=%p i=%p res=%p ptr=%p",
            (void*)pSetULL, (void*)pSetF, (void*)pSetUI, (void*)pSetI,
            (void*)pSetResource, (void*)pSetPtr);
    }
}

// SEH wrapper for NGX init - must be a standalone function because __try
// cannot coexist with C++ object unwinding in the caller.
static int SafeNgxInit(PFN_NVSDK_NGX_D3D12_Init f, unsigned appId,
                       const wchar_t* dataPath, ID3D12Device* dev,
                       NVSDK_NGX_Version version, unsigned* outCode)
{
    if (!f) return -1;
    __try {
        NVSDK_NGX_Result r = f(appId, dataPath, dev, version);
        return (int)r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCode = (unsigned)GetExceptionCode();
        return -2;
    }
}

// SEH wrapper for NGX CreateFeature - same rationale as SafeNgxInit.
static int SafeNgxCreateFeature(PFN_NVSDK_NGX_D3D12_CreateFeature f,
    ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Feature feature,
    NVSDK_NGX_Parameter* params, void** outFeature, unsigned* outCode)
{
    if (!f) return -1;
    __try {
        NVSDK_NGX_Result r = f(cmdList, feature, params, outFeature);
        return (int)r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCode = (unsigned)GetExceptionCode();
        return -2;
    }
}

bool NvDlssUpscaler::CreateFeature(ID3D12GraphicsCommandList* cmdList)
{
    m_lastCreateResult = -3000;
    if (!cmdList)
        return false;

    // NGX INIT SEQUENCING: removed for single-device architecture.
    // Old gate was for cross-device bridge protection (concurrent submission
    // crashed driver). Single-device NGX on game's own device is safe to run
    // immediately - same device, same queue context as the rest of the engine.
    // Throttle: a failing CreateFeature burns GPU time and may leak internal
    // state. Retry at most once per second.
    static DWORD s_lastFailTick = 0;
    if (s_lastFailTick && GetTickCount() - s_lastFailTick < 1000)
    {
        m_lastCreateResult = -3001;
        return false;
    }

    // Item #4 (correctness.md): a device-removed adapter is a hard precondition
    // failure - never let NGX discover it internally. Observed: clean-device
    // creation returned 0x887A0001 and the whole attempt was wasted.
    {
        HRESULT drr = m_device->GetDeviceRemovedReason();
        if (FAILED(drr)) {
            static int s_drrLogs = 0;
            if (++s_drrLogs <= 3) {
                Log("DLSS: skip CreateFeature - device removed reason 0x%08X", (unsigned)drr);
                HooksDumpDRED("createfeature");
            }
            s_lastFailTick = GetTickCount();
            m_lastCreateResult = -3002 - (int)drr;
            return false;
        }
    }


    // SEH around pInit: NGX's D3D12_Init faults at driver level when called
    // on a secondary device while the game's primary device is active.
    // Catching here lets the game survive - DLAA just stays disabled.
    unsigned sehCode = 0;
    Log("DLSS: Init attempting apiVersion=0x%X appId=%u", m_ngxVersion, m_appId);
    // Prefer Init_Ext with feature discovery + app logging (the path working
    // 310.x integrations use); classic Init is the fallback. Result codes are
    // logged verbatim either way for exact comparison.
    int irc = -1;
    if (pInitExt && s_ngxFeaturePaths[0]) {
        Log("DLSS: calling Init_Ext (discovery + app logging)");
        irc = SafeNgxInitExt(pInitExt, m_appId, m_ngxDataPath, m_device,
                             (NVSDK_NGX_Version)m_ngxVersion, &s_ngxFcInfo, &sehCode);
        if (irc == -2) {
            Log("DLSS: NVSDK_NGX_D3D12_Init_Ext FAULTED (SEH 0x%08X)", sehCode);
            s_lastFailTick = GetTickCount();
            m_lastCreateResult = -3003 - (int)sehCode;
            return false;
        }
        if (irc < 0 || !NVSDK_NGX_SUCCEEDED((NVSDK_NGX_Result)irc))
            Log("DLSS: Init_Ext failed, result=%d - trying classic Init", irc);
        else
            Log("DLSS: Init_Ext SUCCEEDED");
    }
    if (irc == -1 || irc < 0 || !NVSDK_NGX_SUCCEEDED((NVSDK_NGX_Result)irc)) {
        irc = SafeNgxInit(pInit, m_appId, m_ngxDataPath, m_device,
                          (NVSDK_NGX_Version)m_ngxVersion, &sehCode);
    }
    if (irc == -2) {
        Log("DLSS: NVSDK_NGX_D3D12_Init FAULTED (SEH 0x%08X) - DLAA unavailable", sehCode);
        s_lastFailTick = GetTickCount();
        m_lastCreateResult = -3003 - (int)sehCode;
        return false;
    }
    if (irc < 0 || !NVSDK_NGX_SUCCEEDED((NVSDK_NGX_Result)irc)) {
        Log("DLSS: NVSDK_NGX_D3D12_Init failed, result=%d", irc);
        s_lastFailTick = GetTickCount();
        m_lastCreateResult = irc;
        return false;
    }

    // DIAGNOSTIC: can the target device do the fundamentals NGX needs?
    {
        IDXGIDevice* dxgidev = nullptr;
        HRESULT qhr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgidev);
        Log("DLSS diag: QI(IDXGIDevice) hr=0x%08X", (unsigned)qhr);
        if (dxgidev) { dxgidev->Release(); dxgidev = nullptr; }
        // VTABLE REPAIR PROBE (FALSIFIED 2026-10-04 — DO NOT RE-ENABLE THE
        // WRITE): clean-device probe proved pristine D3D12 devices also fail
        // QI(IDXGIDevice) with E_NOINTERFACE, so the failure is NOT game
        // wrapper patching — D3D12 devices simply do not expose IDXGIDevice.
        // This block is now LOG-ONLY comparison; the VirtualProtect write is
        // removed. See ProbeCleanDeviceInit + STATUS fence/adapter audit.
        {
            static bool s_repairAttempted = false;
            if (!s_repairAttempted) {
                s_repairAttempted = true;
                D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
                ID3D12Device* clean = nullptr;
                typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(void*, unsigned, const IID&, void**);
                PFN_D3D12CreateDevice mkDev = (PFN_D3D12CreateDevice)(void*)Real_D3D12CreateDevice_Tramp;
                HRESULT chr = mkDev ? mkDev(nullptr, (unsigned)fl, __uuidof(ID3D12Device), (void**)&clean) : E_FAIL;
                Log("DLSS diag: clean device hr=0x%08X", (unsigned)chr);
                if (SUCCEEDED(chr) && clean) {
                    void** cleanVt = *(void***)clean;
                    void** wrapVt = *(void***)m_device;
                    void* genuineQI = cleanVt[0];
                    void* currentQI = wrapVt[0];
                    Log("DLSS diag: genuineQI=%p currentQI=%p patched=%d",
                        genuineQI, currentQI, (int)(genuineQI != currentQI));
                    if (genuineQI != currentQI) {
                        // Write REMOVED (see header comment): differing slot-0
                        // does not prove patching, and overwriting a live
                        // game-device vtable risks game-wide behavior change.
                        Log("DLSS diag: vtable[0] differs - left UNTOUCHED (repair disproven)");
                    }
                    // Test interop now
                    IDXGIDevice* dg = nullptr;
                    HRESULT dhr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dg);
                    Log("DLSS diag: post-repair QI(IDXGIDevice) hr=0x%08X", (unsigned)dhr);
                    if (dg) dg->Release();
                }
                if (clean) clean->Release();
            }
        }
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp2 = {}; hp2.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* tb = nullptr;
        HRESULT bhr = m_device->CreateCommittedResource(&hp2, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tb));
        Log("DLSS diag: test buffer hr=0x%08X", (unsigned)bhr);
        if (tb) tb->Release();
    }

    // Use OUR OWN header-layout parameter object (see NgxParamStore above).
    // The core's AllocateParameters object has a non-standard vtable and is
    // unusable for direct float/resource writes.
    if (!m_paramStore)
        m_paramStore = new NgxParamStore();
    m_parameters = (NVSDK_NGX_Parameter*)m_paramStore;

    m_paramStore->SetUI(NVSDK_NGX_Parameter_Width, m_renderWidth);
    m_paramStore->SetUI(NVSDK_NGX_Parameter_Height, m_renderHeight);
    m_paramStore->SetUI(NVSDK_NGX_Parameter_OutWidth, m_displayWidth);
    m_paramStore->SetUI(NVSDK_NGX_Parameter_OutHeight, m_displayHeight);
    m_paramStore->SetI(NVSDK_NGX_Parameter_PerfQualityValue, m_perfQuality);

    // Color-mode flag: HDR unless overridden for testing. The shadow path
    // feeds an LDR backbuffer while the HDR scene input is unwired, so the
    // correct mode for TODAY's input is selectable live (F7, via IUpscaler::
    // SetHDR + ResetFeature): IsHDR=1 assumes linear-HDR input; IsHDR=0
    // assumes LDR. Per NVIDIA, linear-in-LDR bands/shifts and LDR-in-HDR
    // misresponds (softness/brightness shift).
    int flags = m_forceHDR ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0;
    if (m_mvJittered) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (m_autoExposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (m_depthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    m_paramStore->SetI(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    m_paramStore->SetI(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);

    unsigned cfSeh = 0;
    int cr = SafeNgxCreateFeature(pCreateFeature, cmdList, NVSDK_NGX_Feature_SuperSampling,
                                  m_parameters, (void**)&m_feature, &cfSeh);
    if (cr == -2) {
        m_lastCreateResult = -3004 - (int)cfSeh;
        Log("DLSS: CreateFeature FAULTED (SEH 0x%08X)", cfSeh);
        s_lastFailTick = GetTickCount();
        return false;
    }
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)cr;
    m_lastCreateResult = cr;
    if (!NVSDK_NGX_SUCCEEDED(r) || !m_feature) {
        Log("DLSS: CreateFeature failed, result=%d", r);
        s_lastFailTick = GetTickCount();
        return false;
    }

    m_featureCreated = true;
    Log("DLSS: feature created (render %ux%u -> display %ux%u %s)",
        m_renderWidth, m_renderHeight, m_displayWidth, m_displayHeight,
        m_forceHDR ? "HDR" : "LDR");
    return true;
}

bool NvDlssUpscaler::Evaluate(const UpscalerEvaluateParams& params)
{
    m_lastEvaluateResult = -1000;
    if (!m_initialized || !m_enabled)
        return false;
    if (!params.commandList || !params.color || !params.depth || !params.motionVectors || !params.output)
        return false;

    // DEVICE MISMATCH GUARD: cmdList must belong to the same device as m_device.
    // Cross-device usage is illegal in D3D12 and causes undefined behavior.
    {
        ID3D12Device* cmdDev = nullptr;
        if (SUCCEEDED(params.commandList->GetDevice(__uuidof(ID3D12Device), (void**)&cmdDev)) && cmdDev) {
            if (cmdDev != m_device) {
                m_lastEvaluateResult = -1001;
                Log("DLSS: DEVICE MISMATCH! cmdList dev=%p but NGX dev=%p — skipping eval",
                    (void*)cmdDev, (void*)m_device);
                cmdDev->Release();
                return false;
            }
            cmdDev->Release();
        }
    }

    if (!m_featureCreated) {
        if (!CreateFeature(params.commandList)) {
            m_lastEvaluateResult = -1002;
            return false;
        }
    }

    m_paramStore->SetR12(NVSDK_NGX_Parameter_Color, params.color);
    m_paramStore->SetR12(NVSDK_NGX_Parameter_Output, params.output);
    m_paramStore->SetR12(NVSDK_NGX_Parameter_Depth, params.depth);
    m_paramStore->SetR12(NVSDK_NGX_Parameter_MotionVectors, params.motionVectors);

    // Per-evaluate dimensions (NVIDIA sample: NGX_D3D12_EVALUATE_DLSS_EXT)
    m_paramStore->SetUI(NVSDK_NGX_Parameter_Width, m_renderWidth);
    m_paramStore->SetUI(NVSDK_NGX_Parameter_Height, m_renderHeight);
    m_paramStore->SetUI(NVSDK_NGX_Parameter_OutWidth, m_displayWidth);
    m_paramStore->SetUI(NVSDK_NGX_Parameter_OutHeight, m_displayHeight);

    m_paramStore->SetF(NVSDK_NGX_Parameter_Jitter_Offset_X, params.jitterX);
    m_paramStore->SetF(NVSDK_NGX_Parameter_Jitter_Offset_Y, params.jitterY);
    m_paramStore->SetF(NVSDK_NGX_Parameter_Sharpness, params.sharpness);
    m_paramStore->SetI(NVSDK_NGX_Parameter_Reset, m_firstEvaluate ? 1 : 0);
    // Engine velocity is a UV-space [0,1] delta (prevUV - curUV); DLSS expects pixel
    // space, so scale by the motion vector buffer's dimensions (see DLSS Programming
    // Guide 3.6.1.1 / 3.6.3).
    m_paramStore->SetF(NVSDK_NGX_Parameter_MV_Scale_X, params.mvScaleX);
    m_paramStore->SetF(NVSDK_NGX_Parameter_MV_Scale_Y, params.mvScaleY);
    // Camera matrices (InvViewProjectionMatrix / ClipToPrevClipMatrix) are deliberately
    // NOT set: the official DLSS Programming Guide (31 Mar 2026, sections 5.3/5.4) does
    // not pass them at all, and they are optional via the classic NGX parameter map.

    // Save the engine's descriptor heaps; NGX binds its own internally.
    UINT savedCount = 0;
    ID3D12DescriptorHeap* savedHeaps[2] = { nullptr, nullptr };
    HooksGetDescriptorHeaps(&savedCount, savedHeaps);

    // CRITICAL: Bind an explicit CBV/SRV/UAV heap before EvaluateFeature.
    // Without a shader-visible heap bound on the command list, NGX's internal
    // SetComputeRootDescriptorTable calls have no backing storage -> driver
    // fault -> device removed (0x887A0001).
    if (!m_evalHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4096;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_evalHeap)))) {
            Log("DLSS: failed to create eval descriptor heap");
            return false;
        }
    }
    ID3D12DescriptorHeap* heapsToBind[] = { m_evalHeap };
    params.commandList->SetDescriptorHeaps(1, heapsToBind);

    unsigned evSeh = 0;
    int evr;
    __try {
        NVSDK_NGX_Result rv = pEvaluateFeature(params.commandList, m_feature, m_parameters, nullptr);
        evr = (int)rv;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        evSeh = (unsigned)GetExceptionCode();
        evr = -2;
    }
    if (evr == -2) {
        m_lastEvaluateResult = -2000 - (int)evSeh;
        static volatile LONG s_evFaultLogs = 0;
        LONG efn = InterlockedIncrement(&s_evFaultLogs);
        if (efn <= 10 || (efn % 600) == 0)
            Log("DLSS: EvaluateFeature FAULTED (SEH 0x%08X #%ld)", evSeh, efn);
        return false;
    }
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)evr;
    m_lastEvaluateResult = evr;
    if (!NVSDK_NGX_SUCCEEDED(r)) {
        static volatile LONG s_evFailLogs = 0;
        LONG ern = InterlockedIncrement(&s_evFailLogs);
        if (ern <= 10 || (ern % 600) == 0)
            Log("DLSS: EvaluateFeature failed, result=%d (#%ld)", r, ern);
        return false;
    }

    if (savedCount > 0)
        HooksRestoreDescriptorHeaps(params.commandList, savedCount, savedHeaps);

    // Bounded per-eval echo of the ACTUAL NGX parameters just consumed above
    // (read from m_*/params, not NgxParamStore). Logging only.
    int resetSent = m_firstEvaluate ? 1 : 0;
    int evalFlags = m_forceHDR ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0;
    if (m_mvJittered) evalFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (m_autoExposure) evalFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    static volatile LONG s_evParamLogs = 0;
    LONG epn = InterlockedIncrement(&s_evParamLogs);
    if (epn <= 10 || (epn % 600) == 0)
        Log("DLSS: eval params jitter=%.2f/%.2f mvScale=%.0fx%.0f render=%ux%u display=%ux%u reset=%d sharp=%.2f flags=0x%X",
            params.jitterX, params.jitterY, params.mvScaleX, params.mvScaleY,
            m_renderWidth, m_renderHeight, m_displayWidth, m_displayHeight,
            resetSent, params.sharpness, (unsigned)evalFlags);

    m_firstEvaluate = false;
    return true;
}

void NvDlssUpscaler::DestroyFeature()
{
    // Release ONLY the feature handle. The old code called pShutdown() here,
    // which tears down the ENTIRE NGX core including the snippet's JIT-compiled
    // pass code - the next EvaluateFeature then jumped into freed JIT memory
    // (constant fault address, RIP=RAX) and crashed the game after size churn.
    // Fault-safe: the breaker calls this after 30 consecutive eval faults, i.e.
    // exactly when driver state is suspect. A fault inside ReleaseFeature must
    // neither escape (it would bypass the halt accounting) nor leave a live
    // m_feature dangling into the next Evaluate (fault-storm amplifier: the
    // next eval would reuse the half-released handle). Null first, then call
    // under SEH; no C++ objects in this frame so __try is legal.
    if (m_featureCreated && m_ngxDll) {
        typedef int(__cdecl* PFN_Release)(void*);
        PFN_Release pRelease = (PFN_Release)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_ReleaseFeature");
        void* doomed = m_feature;
        m_feature = nullptr;
        m_featureCreated = false;
        if (pRelease && doomed) {
            unsigned relSeh = 0;
            int relr = -3;
            __try {
                NVSDK_NGX_Result rr = pRelease(doomed);
                relr = (int)rr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                relSeh = (unsigned)GetExceptionCode();
            }
            if (relSeh)
                Log("DLSS: feature release FAULTED (SEH 0x%08X) - handle dropped, feature recreates next eval", relSeh);
            else
                Log("DLSS: feature released (rr=%d)", relr);
        }
    } else {
        m_featureCreated = false;
        m_feature = nullptr;
    }
}

void NvDlssUpscaler::UpdateSizes(unsigned int rw, unsigned int rh,
                                 unsigned int dw, unsigned int dh)
{
    m_renderWidth = rw;
    m_renderHeight = rh;
    m_displayWidth = dw;
    m_displayHeight = dh;
    if (m_featureCreated) {
        DestroyFeature();
        m_firstEvaluate = true;
        Log("DLSS: feature reset for size change (render %ux%u -> display %ux%u)",
            rw, rh, dw, dh);
    }
}

// Discriminator probe (see upscaler.h): NGX Init on a self-created clean
// device. READ-ONLY vs the game: no vtable writes, no game-device calls.
// Interprets game-device 0xBAD00001: success here => wrapper/device-interop
// blocks NGX (repair/unwrap route); same failure => AppId or driver platform
// (AppId/driver route). Runs once per process; all game threads share it.
int NvDlssUpscaler::ProbeCleanDeviceInit()
{
    static volatile LONG s_probeOnce = 0;
    static int s_probeResult = -9998;
    if (InterlockedCompareExchange(&s_probeOnce, 1, 0) != 0)
        return s_probeResult;
    if (!pInit) {
        Log("DLSS probe: NGX core not loaded - cannot probe");
        s_probeResult = -9997;
        return s_probeResult;
    }
    ID3D12Device* clean = nullptr;
    typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(void*, unsigned, const IID&, void**);
    PFN_D3D12CreateDevice mkDev = (PFN_D3D12CreateDevice)(void*)Real_D3D12CreateDevice_Tramp;
    HRESULT chr = mkDev ? mkDev(nullptr, (unsigned)D3D_FEATURE_LEVEL_11_0,
                                __uuidof(ID3D12Device), (void**)&clean) : E_FAIL;
    Log("DLSS probe: clean device hr=0x%08X", (unsigned)chr);
    if (FAILED(chr) || !clean) {
        s_probeResult = -9996;
        return s_probeResult;
    }
    // Adapter identity via LUID matching (no IDXGIDevice needed — D3D12
    // devices do not expose it, wrapped or not). Reports both the clean
    // device and the game device, so a default-adapter mismatch (e.g. clean
    // landing on integrated graphics) cannot be mistaken for a wrapper.
    {
        auto logAdapter = [](const char* tag, ID3D12Device* dev) {
            if (!dev) { Log("DLSS probe: %s device=null", tag); return; }
            LUID luid = dev->GetAdapterLuid();
            IDXGIFactory* factory = nullptr;
            HRESULT fhr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
            if (FAILED(fhr) || !factory) {
                Log("DLSS probe: %s factory hr=0x%08X", tag, (unsigned)fhr);
                return;
            }
            bool matched = false;
            for (UINT i = 0; ; ++i) {
                IDXGIAdapter* ad = nullptr;
                if (factory->EnumAdapters(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
                if (!ad) continue;
                DXGI_ADAPTER_DESC adesc = {};
                if (SUCCEEDED(ad->GetDesc(&adesc)) &&
                    adesc.AdapterLuid.LowPart == luid.LowPart &&
                    adesc.AdapterLuid.HighPart == luid.HighPart) {
                    Log("DLSS probe: %s vendor=0x%04X '%ls' (LUID-matched)",
                        tag, adesc.VendorId, adesc.Description);
                    matched = true;
                }
                ad->Release();
                if (matched) break;
            }
            if (!matched)
                Log("DLSS probe: %s LUID unmatched by any DXGI adapter", tag);
            factory->Release();
        };
        logAdapter("clean-device", clean);
        logAdapter("game-device", m_device);
    }
    // Ask the DRIVER what it thinks: snippet/API versions + SuperSampling
    // requirements on the NVIDIA adapter. Read-only; interprets Init codes.
    {
        PFN_NVSDK_NGX_GetAPIVersion_Local pGetAPI =
            (PFN_NVSDK_NGX_GetAPIVersion_Local)GetProcAddress(m_ngxDll, "NVSDK_NGX_GetAPIVersion");
        PFN_NVSDK_NGX_GetSnippetVersion_Local pGetSnip =
            (PFN_NVSDK_NGX_GetSnippetVersion_Local)GetProcAddress(m_ngxDll, "NVSDK_NGX_GetSnippetVersion");
        PFN_NVSDK_NGX_D3D12_GetFeatureRequirements_Local pGetReq =
            (PFN_NVSDK_NGX_D3D12_GetFeatureRequirements_Local)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_GetFeatureRequirements");
        Log("DLSS probe: versionFns api=%p snip=%p req=%p", (void*)pGetAPI, (void*)pGetSnip, (void*)pGetReq);
        if (pGetAPI) Log("DLSS probe: GetAPIVersion=0x%X", pGetAPI());
        if (pGetSnip) Log("DLSS probe: GetSnippetVersion=0x%X", pGetSnip());
        if (pGetReq) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)) && factory) {
                IDXGIAdapter* nvAd = nullptr;
                for (UINT i = 0; ; ++i) {
                    IDXGIAdapter* ad = nullptr;
                    if (factory->EnumAdapters(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
                    if (!ad) continue;
                    DXGI_ADAPTER_DESC adesc = {};
                    if (SUCCEEDED(ad->GetDesc(&adesc)) && adesc.VendorId == 0x10DE) { nvAd = ad; break; }
                    ad->Release();
                }
                if (nvAd) {
                    NVSDK_NGX_FeatureDiscoveryInfo_Local di = {};
                    di.SDKVersion = (int)m_ngxVersion;
                    di.FeatureID = 1; // SuperSampling
                    di.Identifier.IdentifierType = 0;
                    di.Identifier.v.ApplicationId = m_appId;
                    di.ApplicationDataPath = m_ngxDataPath;
                    di.FeatureInfo = s_ngxFeaturePaths[0] ? &s_ngxFcInfo : nullptr;
                    NVSDK_NGX_FeatureRequirement_Local req = {};
                    req.FeatureSupported = -1;
                    int rr = -9999;
                    __try { rr = pGetReq(nvAd, &di, &req); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { rr = -1000 - (int)GetExceptionCode(); }
                    Log("DLSS probe: GetFeatureRequirements rr=%d supported=%d minHW=0x%X minOS='%s'",
                        rr, req.FeatureSupported, req.MinHWArchitecture, req.MinOSVersion);
                    nvAd->Release();
                } else {
                    Log("DLSS probe: no 0x10DE adapter enumerated");
                }
                factory->Release();
            }
        }
    }
    unsigned sehCode = 0;
    int rc = SafeNgxInit(pInit, m_appId, m_ngxDataPath, clean,
                         (NVSDK_NGX_Version)m_ngxVersion, &sehCode);
    Log("DLSS probe: clean-device Init result=%d (0x%08X)%s", rc, (unsigned)rc,
        rc == -2 ? " FAULTED" : "");
    clean->Release();
    s_probeResult = rc;
    return s_probeResult;
}

void NvDlssUpscaler::Shutdown()
{
    DestroyFeature();
    if (m_evalHeap) { m_evalHeap->Release(); m_evalHeap = nullptr; }
    UnloadNGX();
    m_initialized = false;
}

IUpscaler* CreateUpscaler(UpscalerType type)
{
    if (type == UPSCALER_DLSS)
        return new NvDlssUpscaler();
    return nullptr;
}

void DestroyUpscaler(IUpscaler* upscaler)
{
    delete upscaler;
}
