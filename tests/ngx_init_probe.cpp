// Out-of-game NGX Init discriminator (see docs/STATUS.md).
// Distinguishes in-process interference (ASI hooks / BeamNG modules) from a
// system-wide NGX platform absence: creates its own D3D12 device, loads the
// shipped nvngx_dlss.dll, and calls Init_Ext exactly as dlss_ngx.cpp does.
// Prints exact result codes; touches nothing belonging to the game.
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <cstdio>

typedef int NVSDK_NGX_Version_Local;
typedef int NVSDK_NGX_Result_Local;
struct NVSDK_NGX_PathListInfo_L {
    const wchar_t* const* Path;
    unsigned int Length;
};
struct NVSDK_NGX_LoggingInfo_L {
    void (__cdecl* LoggingCallback)(const char*, int, int);
    int MinimumLoggingLevel;
    bool DisableOtherLoggingSinks;
};
struct NVSDK_NGX_FeatureCommonInfo_L {
    NVSDK_NGX_PathListInfo_L PathListInfo;
    void* InternalData;
    NVSDK_NGX_LoggingInfo_L LoggingInfo;
};
typedef NVSDK_NGX_Result_Local (__cdecl* PFN_Init_Ext_L)(
    unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version_Local,
    const NVSDK_NGX_FeatureCommonInfo_L*);

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* dllPath = L"dist\\nvngx_dlss.dll";
    unsigned long long appId = 241534720ULL;
    if (argc > 1) dllPath = argv[1];
    if (argc > 2) appId = _wcstoui64(argv[2], nullptr, 10);
    wprintf(L"probe: loading %ls\n", dllPath);
    HMODULE dll = LoadLibraryW(dllPath);
    if (!dll) { printf("probe: LoadLibrary FAILED err=%lu\n", GetLastError()); return 10; }
    PFN_Init_Ext_L pExt = (PFN_Init_Ext_L)GetProcAddress(dll, "NVSDK_NGX_D3D12_Init_Ext");
    printf("probe: Init_Ext=%p\n", (void*)pExt);
    if (!pExt) return 11;

    ID3D12Device* dev = nullptr;
    // Prefer an NVIDIA adapter explicitly: default-adapter creation lands on
    // integrated graphics in headless/console processes (verified: 0x1002).
    IDXGIAdapter* picked = nullptr;
    {
        IDXGIFactory* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&f)) && f) {
            IDXGIAdapter* first = nullptr;
            for (UINT i = 0; ; ++i) {
                IDXGIAdapter* ad = nullptr;
                if (f->EnumAdapters(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
                if (!ad) continue;
                DXGI_ADAPTER_DESC d = {};
                if (SUCCEEDED(ad->GetDesc(&d))) {
                    wprintf(L"probe: adapter%u vendor=0x%04X '%ls'\n", i, d.VendorId, d.Description);
                    if (!first) { first = ad; continue; }
                    if (d.VendorId == 0x10DE) { picked = ad; break; }
                }
                if (ad != first) ad->Release();
            }
            if (!picked) picked = first;
            else if (first) first->Release();
            f->Release();
        }
    }
    HRESULT chr = D3D12CreateDevice(picked, D3D_FEATURE_LEVEL_11_0,
                                    __uuidof(ID3D12Device), (void**)&dev);
    printf("probe: D3D12CreateDevice hr=0x%08X dev=%p\n", (unsigned)chr, (void*)dev);
    if (FAILED(chr) || !dev) return 12;
    {
        LUID luid = dev->GetAdapterLuid();
        IDXGIFactory* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&f)) && f) {
            for (UINT i = 0; ; ++i) {
                IDXGIAdapter* ad = nullptr;
                if (f->EnumAdapters(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
                if (!ad) continue;
                DXGI_ADAPTER_DESC d = {};
                if (SUCCEEDED(ad->GetDesc(&d)) &&
                    d.AdapterLuid.LowPart == luid.LowPart &&
                    d.AdapterLuid.HighPart == luid.HighPart)
                    wprintf(L"probe: device adapter vendor=0x%04X '%ls'\n", d.VendorId, d.Description);
                ad->Release();
            }
            f->Release();
        }
    }

    wchar_t dataPath[MAX_PATH] = {};
    DWORD tn = GetTempPathW(MAX_PATH, dataPath);
    if (tn == 0 || tn >= MAX_PATH) return 13;
    static wchar_t sub[MAX_PATH] = {};
    size_t n = 0;
    while (dataPath[n] && n + 1 < MAX_PATH) { sub[n] = dataPath[n]; ++n; }
    const wchar_t* tail = L"ngx_probe";
    size_t m = 0;
    while (tail[m] && n + 1 < MAX_PATH) { sub[n++] = tail[m++]; }
    sub[n] = L'\0';
    CreateDirectoryW(sub, nullptr);

    static const wchar_t* paths[1] = { nullptr };
    static wchar_t dllDir[MAX_PATH] = {};
    {
        wchar_t full[MAX_PATH] = {};
        GetFullPathNameW(dllPath, MAX_PATH, full, nullptr);
        size_t len = 0;
        while (full[len]) ++len;
        size_t cut = len;
        for (size_t i = 0; i < len; ++i)
            if (full[i] == L'\\') cut = i + 1;
        for (size_t i = 0; i < cut && i + 1 < MAX_PATH; ++i) dllDir[i] = full[i];
        paths[0] = dllDir;
    }
    NVSDK_NGX_FeatureCommonInfo_L fc = {};
    fc.PathListInfo.Path = paths;
    fc.PathListInfo.Length = 1;
    fc.LoggingInfo.LoggingCallback = nullptr;
    fc.LoggingInfo.MinimumLoggingLevel = 1;
    fc.LoggingInfo.DisableOtherLoggingSinks = false;

    int rc = pExt(appId, sub, dev, 0x15, &fc);
    printf("probe: Init_Ext(appId=%llu) result=%d (0x%08X)\n", appId, rc, (unsigned)rc);
    dev->Release();
    return 0;
}
