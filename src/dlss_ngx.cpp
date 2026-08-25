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
    // Self-host: mirror the snippet next to the exe once.
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wchar_t dst[MAX_PATH] = {};
        lstrcpyW(dst, exePath);
        lstrcatW(dst, L"nvngx_dlss.dll");
        if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES && dllPath && *dllPath) {
            if (CopyFileW(dllPath, dst, FALSE))
                Log("DLSS: mirrored snippet to exe dir: %ls", dst);
        }
    }
    // PART 1: preload nvapi64.dll BEFORE the NGX core. Driver 596.49's
    // nvapi64.dll is a stripped "direct mode" shim (exports only
    // nvapi_QueryInterface + nvapi_Direct_GetMethod); without this preload
    // the core's NVSDK_NGX_D3D12_Init fails with 0xBAD00001.
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    Log("DLSS: preloaded nvapi64.dll = %p", (void*)nvapi);

    // Enable the NGX core's own log (C:\ProgramData\NVIDIA\NGX\models\nvngx.log)
    // BEFORE loading it - it names the exact parameter on EvaluateFeature
    // rejections, which ScaleNG.log cannot see.
    SetEnvironmentVariableA("__NGX_LOG_LEVEL", "3");
    SetEnvironmentVariableA("__NGX_DISABLE_UPDATER", "1");

    // PART 2: locate and load the driver's NGX core (nvngx.dll in the driver
    // store). It exports the classic API: Init / AllocateParameters /
    // CreateFeature / EvaluateFeature / Shutdown.
    wchar_t corePath[MAX_PATH] = {};
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nvlti.inf_*", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        FindClose(hFind);
        CopyW(corePath, MAX_PATH, L"C:\\Windows\\System32\\DriverStore\\FileRepository\\");
        AppendW(corePath, MAX_PATH, fd.cFileName);
        AppendW(corePath, MAX_PATH, L"\\nvngx.dll");
        m_ngxDll = LoadLibraryW(corePath);
        if (m_ngxDll) {
            CopyW(m_dlssPath, MAX_PATH, corePath);
            Log("DLSS: loaded driver core nvngx.dll (%ls)", corePath);
        }
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

    pInit = (PFN_NVSDK_NGX_D3D12_Init)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_Init");
    pAllocateParameters = (PFN_NVSDK_NGX_D3D12_AllocateParameters)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_AllocateParameters");
    pCreateFeature = (PFN_NVSDK_NGX_D3D12_CreateFeature)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_CreateFeature");
    pEvaluateFeature = (PFN_NVSDK_NGX_D3D12_EvaluateFeature)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_EvaluateFeature");
    pShutdown = (PFN_NVSDK_NGX_D3D12_Shutdown)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_Shutdown");
    pGetParameters = (PFN_NVSDK_NGX_D3D12_GetParameters)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_GetParameters");

    if (!pInit || !pAllocateParameters || !pCreateFeature || !pEvaluateFeature || !pShutdown) {
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
    m_perfQuality = params.perfQuality;
    m_mvJittered = params.mvJittered;
    m_autoExposure = params.autoExposure;
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
    // Use the SAME data path as the harness (test_mini) which provably works.
    // The module directory was causing NGX to search for snippets/models in
    // the wrong location, leading to incomplete initialization.
    CopyW(m_ngxDataPath, MAX_PATH, L"C:\\ProgramData\\NVIDIA\\NGX\\models");

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
        return false;

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
            return false;
        }
    }


    // SEH around pInit: NGX's D3D12_Init faults at driver level when called
    // on a secondary device while the game's primary device is active.
    // Catching here lets the game survive - DLAA just stays disabled.
    unsigned sehCode = 0;
    int irc = SafeNgxInit(pInit, m_appId, m_ngxDataPath, m_device,
                          NVSDK_NGX_Version_API, &sehCode);
    if (irc == -2) {
        Log("DLSS: NVSDK_NGX_D3D12_Init FAULTED (SEH 0x%08X) - DLAA unavailable", sehCode);
        s_lastFailTick = GetTickCount();
        return false;
    }
    if (irc < 0 || !NVSDK_NGX_SUCCEEDED((NVSDK_NGX_Result)irc)) {
        Log("DLSS: NVSDK_NGX_D3D12_Init failed, result=%d", irc);
        s_lastFailTick = GetTickCount();
        return false;
    }

    // DIAGNOSTIC: can the target device do the fundamentals NGX needs?
    {
        IDXGIDevice* dxgidev = nullptr;
        HRESULT qhr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgidev);
        Log("DLSS diag: QI(IDXGIDevice) hr=0x%08X", (unsigned)qhr);
        if (dxgidev) { dxgidev->Release(); dxgidev = nullptr; }
        // VTABLE REPAIR PROBE: the "device" is believed to be the REAL device
        // with a game-patched vtable[0] (QI) that blocks IDXGIDevice. Restore
        // the genuine QI from a clean device we create ourselves, test, and
        // keep the repair permanent if it unlocks DXGI interop.
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
                        DWORD oldProt = 0;
                        if (VirtualProtect(&wrapVt[0], sizeof(void*), PAGE_READWRITE, &oldProt)) {
                            wrapVt[0] = genuineQI;
                            VirtualProtect(&wrapVt[0], sizeof(void*), oldProt, &oldProt);
                            Log("DLSS diag: vtable[0] RESTORED to genuine QI");
                        }
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

    int flags = 0;
    if (m_mvJittered) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (m_autoExposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    m_paramStore->SetI(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    m_paramStore->SetI(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);

    unsigned cfSeh = 0;
    int cr = SafeNgxCreateFeature(pCreateFeature, cmdList, NVSDK_NGX_Feature_SuperSampling,
                                  m_parameters, (void**)&m_feature, &cfSeh);
    if (cr == -2) {
        Log("DLSS: CreateFeature FAULTED (SEH 0x%08X)", cfSeh);
        s_lastFailTick = GetTickCount();
        return false;
    }
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)cr;
    if (!NVSDK_NGX_SUCCEEDED(r) || !m_feature) {
        Log("DLSS: CreateFeature failed, result=%d", r);
        s_lastFailTick = GetTickCount();
        return false;
    }

    m_featureCreated = true;
    Log("DLSS: feature created (render %ux%u -> display %ux%u)",
        m_renderWidth, m_renderHeight, m_displayWidth, m_displayHeight);
    return true;
}

bool NvDlssUpscaler::Evaluate(const UpscalerEvaluateParams& params)
{
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
                Log("DLSS: DEVICE MISMATCH! cmdList dev=%p but NGX dev=%p — skipping eval",
                    (void*)cmdDev, (void*)m_device);
                cmdDev->Release();
                return false;
            }
            cmdDev->Release();
        }
    }

    if (!m_featureCreated) {
        if (!CreateFeature(params.commandList))
            return false;
    }

    m_paramStore->SetR12(NVSDK_NGX_Parameter_Color, params.color);
    m_paramStore->SetR12(NVSDK_NGX_Parameter_Output, params.output);
    m_paramStore->SetR12(NVSDK_NGX_Parameter_Depth, params.depth);
    m_paramStore->SetR12(NVSDK_NGX_Parameter_MotionVectors, params.motionVectors);
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
        Log("DLSS: EvaluateFeature FAULTED (SEH 0x%08X)", evSeh);
        return false;
    }
    NVSDK_NGX_Result r = (NVSDK_NGX_Result)evr;
    if (!NVSDK_NGX_SUCCEEDED(r)) {
        Log("DLSS: EvaluateFeature failed, result=%d", r);
        return false;
    }

    if (savedCount > 0)
        HooksRestoreDescriptorHeaps(params.commandList, savedCount, savedHeaps);

    m_firstEvaluate = false;
    return true;
}

void NvDlssUpscaler::DestroyFeature()
{
    // Release ONLY the feature handle. The old code called pShutdown() here,
    // which tears down the ENTIRE NGX core including the snippet's JIT-compiled
    // pass code - the next EvaluateFeature then jumped into freed JIT memory
    // (constant fault address, RIP=RAX) and crashed the game after size churn.
    if (m_featureCreated && m_ngxDll) {
        typedef int(__cdecl* PFN_Release)(void*);
        PFN_Release pRelease = (PFN_Release)GetProcAddress(m_ngxDll, "NVSDK_NGX_D3D12_ReleaseFeature");
        if (pRelease && m_feature) {
            NVSDK_NGX_Result rr = pRelease(m_feature);
            Log("DLSS: feature released (rr=%d)", rr);
        }
        m_featureCreated = false;
    }
    m_feature = nullptr;
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

void NvDlssUpscaler::Shutdown()
{
    DestroyFeature();
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