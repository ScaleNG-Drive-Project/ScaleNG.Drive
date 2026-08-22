#include <windows.h>
#include "log.h"
#include "d3d12_hooks.h"

wchar_t g_logPath[MAX_PATH] = {};

static ScaleNgConfig g_config;

// ---------------------------------------------------------------------------
// Manual string/number parsing - NO CRT (wcstof/wcstol/_wcsicmp/wcscpy_s
// all die in this process context; see log.h).
// ---------------------------------------------------------------------------

static float ParseFloatW(const wchar_t* s)
{
    if (!s) return 0.0f;
    float sign = 1.0f;
    if (*s == L'-') { sign = -1.0f; ++s; }
    else if (*s == L'+') ++s;
    float v = 0.0f;
    while (*s >= L'0' && *s <= L'9') { v = v * 10.0f + (float)(*s - L'0'); ++s; }
    if (*s == L'.') {
        ++s;
        float frac = 0.1f;
        while (*s >= L'0' && *s <= L'9') { v += (float)(*s - L'0') * frac; frac *= 0.1f; ++s; }
    }
    return sign * v;
}

static int ParseIntW(const wchar_t* s)
{
    if (!s) return 0;
    bool neg = false;
    if (*s == L'-') { neg = true; ++s; }
    else if (*s == L'+') ++s;
    int v = 0;
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (*s - L'0'); ++s; }
    return neg ? -v : v;
}

static bool StrcaseEqW(const wchar_t* a, const wchar_t* b)
{
    if (!a || !b) return a == b;
    for (;;) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return false;
        if (!ca) return true;
        ++a; ++b;
    }
}

static void CopyStrW(wchar_t* dst, size_t cap, const wchar_t* src)
{
    dst[0] = L'\0';
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
    dst[i] = L'\0';
}

static void AppendStrW(wchar_t* dst, size_t cap, const wchar_t* src)
{
    size_t base = 0;
    while (dst[base]) ++base;
    size_t i = 0;
    while (src[i] && base + i + 1 < cap) { dst[base + i] = src[i]; ++i; }
    if (base + i < cap) dst[base + i] = L'\0';
}

static void LoadConfig()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&LoadConfig, &self);
    wchar_t moduleDir[MAX_PATH] = {};
    if (self) GetModuleFileNameW(self, moduleDir, MAX_PATH);
    wchar_t* slash = nullptr;
    for (wchar_t* p = moduleDir; *p; ++p)
        if (*p == L'\\') slash = p;
    if (slash) *(slash + 1) = L'\0';
    wchar_t iniPath[MAX_PATH] = {};
    CopyStrW(iniPath, MAX_PATH, moduleDir);
    AppendStrW(iniPath, MAX_PATH, L"ScaleNG.ini");

    auto getFloat = [&](const wchar_t* key, float def) {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"ScaleNG", key, L"", buf, 64, iniPath);
        return buf[0] ? ParseFloatW(buf) : def;
    };
    auto getInt = [&](const wchar_t* key, int def) {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"ScaleNG", key, L"", buf, 64, iniPath);
        return buf[0] ? ParseIntW(buf) : def;
    };
    auto getBool = [&](const wchar_t* key, bool def) {
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"ScaleNG", key, L"", buf, 64, iniPath);
        return buf[0] ? (ParseIntW(buf) != 0) : def;
    };

    float scale = getFloat(L"scale", 0.0f);
    float renderScale = getFloat(L"renderScale", scale > 0.0f ? scale : 0.67f);
    if (renderScale < 0.5f) renderScale = 0.5f;
    if (renderScale > 0.99f) renderScale = 0.99f;
    g_config.renderScale = renderScale;
    g_config.dlaa = getBool(L"dlaa", false);
    g_config.passive = getBool(L"passive", false);
    g_config.sharpness = getFloat(L"sharpness", 0.0f);
    g_config.perfQuality = getInt(L"perfQuality", 1);
    g_config.mvJittered = getBool(L"mvJittered", true);
    g_config.autoExposure = getBool(L"autoExposure", true);
    g_config.appId = (unsigned int)getInt(L"appId", 1);
    g_config.enabled = getBool(L"enabled", true);

    wchar_t upscaler[32] = {};
    GetPrivateProfileStringW(L"ScaleNG", L"upscaler", L"dlss", upscaler, 32, iniPath);
    if (!StrcaseEqW(upscaler, L"dlss")) {
        Log("config: upscaler=%ls not supported (only dlss) - ScaleNG inactive", upscaler);
        g_config.enabled = false;
    }

    wchar_t jitterPattern[32] = {};
    GetPrivateProfileStringW(L"ScaleNG", L"jitterPattern", L"halton", jitterPattern, 32, iniPath);
    if (!StrcaseEqW(jitterPattern, L"halton"))
        Log("config: jitterPattern=%ls ignored (only halton supported)", jitterPattern);

    CopyStrW(g_config.dlssDllPath, MAX_PATH, moduleDir);
    AppendStrW(g_config.dlssDllPath, MAX_PATH, L"nvngx_dlss.dll");
}

static int SehFilter(unsigned int code, _EXCEPTION_POINTERS* ep)
{
    char buf[256];
    const char* hex = "0123456789ABCDEF";
    char* p = buf;
    const char head[] = "FATAL: SEH exception 0x";
    for (size_t i = 0; i < sizeof(head) - 1; ++i) *p++ = head[i];
    for (int i = 7; i >= 0; --i) *p++ = hex[(code >> (i * 4)) & 0xF];
    const char mid[] = " at 0x";
    for (size_t i = 0; i < sizeof(mid) - 1; ++i) *p++ = mid[i];
    ULONG_PTR addr = ep ? (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress : 0;
    for (int i = 15; i >= 0; --i) *p++ = hex[(addr >> (i * 4)) & 0xF];
    *p++ = '\n';
    *p = '\0';
    RawLog(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

extern "C" __declspec(dllexport) void InitializeASI()
{
    LogInit();
    RawLog("init: entered InitializeASI\n");
    __try {
        // Enable NGX core logging so the driver writes the real reject reason
        // for EvaluateFeature into C:\ProgramData\NVIDIA\NGX\models\nvngx.log.
        SetEnvironmentVariableW(L"__NGX_LOG_LEVEL", L"3");
        SetEnvironmentVariableW(L"__NGX_DISABLE_UPDATER", L"1");
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        Log("process: %ls", exePath);
        Log("========================================");
        Log("ScaleNG.asi loaded");
        Log("init: loading config");
        LoadConfig();
        if (!g_config.enabled) {
            Log("ScaleNG.asi disabled via config - no hooks installed");
            return;
        }
        Log("config: renderScale=%.2f sharpness=%.2f perfQuality=%d mvJittered=%d autoExposure=%d appId=%u dlaa=%d",
            g_config.renderScale, g_config.sharpness, g_config.perfQuality,
            g_config.mvJittered ? 1 : 0, g_config.autoExposure ? 1 : 0, g_config.appId,
            g_config.dlaa ? 1 : 0);
        Log("init: setting config");
        HooksSetConfig(g_config);
        Log("init: installing D3D12CreateDevice detour");
        HooksInstallCreateDeviceDetour();
        Log("ScaleNG.asi initialization complete");
    } __except (SehFilter(GetExceptionCode(), GetExceptionInformation())) {
    }
}