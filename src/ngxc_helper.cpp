// ============================================================================
// ScaleNG NGX Helper - cross-process bridge worker.
//
// Owns a CLEAN NVIDIA D3D12 device (BeamNG's native wrapper cannot reach us
// here). Receives duplicated NT handles for shared color/out textures and two
// fences from the game-side ASI over a named pipe, then runs the frame loop:
//
//     wait fIn(v)  ->  [future: NGX evaluate]  ->  signal fOut(v)
//
// Protocol (little-endian, single pipe, sequential):
//   C->H : uint32 magic 'SNGX' , uint32 version
//   H->C : uint32 magic 'HNGX' , uint32 version
//   C->H : SetupMsg { u64 hColor, hOut, hFIn, hFOut; u32 w, h, fmt; }
//          (handle VALUES already duplicated into this process by sender)
//   H->C : uint32 'OKAY' | 'FAIL'
//   ... helper loops forever; pipe close = graceful exit.
//
// Failure law: after setup succeeds, fOut is signaled EVERY frame regardless
// of NGX outcome so the game-side stage-3 never starves.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
// ---- dlss_ngx.cpp linkage stubs (helper has no hook layer) -----------------
#include "upscaler.h"
#include "d3d12_hooks.h"
void HooksGetDescriptorHeaps(UINT* count, ID3D12DescriptorHeap** heaps)
{
    if (count) *count = 0;
}
void HooksRestoreDescriptorHeaps(ID3D12GraphicsCommandList*, UINT,
                                 ID3D12DescriptorHeap* const*) {}

// remaining dlss_ngx externals
wchar_t g_logPath[MAX_PATH] = {}; // log.h global (helper: unused, LogLine writes its own file)
PFN_ScaleNG_CreateDevice Real_D3D12CreateDevice_Tramp = nullptr;
void HooksDumpDRED(const char*) {}


static void LogLine(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, (DWORD)n, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
    // mirror to file for post-mortem (same dir as exe)
    static FILE* f = nullptr;
    if (!f) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) *(slash + 1) = 0;
        lstrcatA(path, "ngx_helper.log");
        f = fopen(path, "ab");
    }
    if (f) { fputs(buf, f); fputc('\n', f); fflush(f); }
}

struct SetupMsg {
    unsigned long long hColor;
    unsigned long long hOut;
    unsigned long long hFIn;
    unsigned long long hFOut;
    unsigned int w, h, fmt;
    unsigned int pad;
    unsigned long long startVal; // ASI's next expected fence value
};
static_assert(sizeof(SetupMsg) == 56, "SetupMsg size mismatch - must be 56 bytes");

static ID3D12Device*              g_dev    = nullptr;
static ID3D12CommandQueue*        g_q      = nullptr;
static ID3D12CommandAllocator*    g_alloc  = nullptr;
static ID3D12GraphicsCommandList* g_list   = nullptr;
static ID3D12Resource*            g_color  = nullptr;
static ID3D12Resource*            g_out    = nullptr;
static ID3D12Fence*               g_fIn    = nullptr;
static ID3D12Fence*               g_fOut   = nullptr;
static HANDLE                     g_fInEv  = nullptr;
static ID3D12Resource*            g_depthIn = nullptr; // zero-filled dummies
static ID3D12Resource*            g_mvIn    = nullptr;
static IUpscaler*                 s_up      = nullptr;
static UINT                       s_initW   = 0, s_initH = 0;
static unsigned long long         s_seedVal = 1;

static bool InitDevice()
{
    typedef HRESULT(WINAPI* PFN_DC)(IUnknown*, D3D_FEATURE_LEVEL, const IID&, void**);
    HMODULE dm = GetModuleHandleA("d3d12.dll");
    if (!dm) dm = LoadLibraryA("d3d12.dll");
    PFN_DC dc = dm ? (PFN_DC)GetProcAddress(dm, "D3D12CreateDevice") : nullptr;
    if (!dc) return false;

    IDXGIFactory1* fac = nullptr;
    {
        typedef HRESULT(WINAPI* PFN_CF)(const IID&, void**);
        HMODULE xm = GetModuleHandleA("dxgi.dll");
        if (!xm) xm = LoadLibraryA("dxgi.dll");
        PFN_CF cf = xm ? (PFN_CF)GetProcAddress(xm, "CreateDXGIFactory1") : nullptr;
        if (cf) cf(__uuidof(IDXGIFactory1), (void**)&fac);
    }
    IDXGIAdapter1* nvidia = nullptr;
    if (fac) {
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d = {};
            if (SUCCEEDED(ad->GetDesc1(&d)) && d.VendorId == 0x10DE) { nvidia = ad; break; }
            ad->Release();
        }
    }
    HRESULT hr = E_FAIL;
    if (nvidia) hr = dc(nvidia, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&g_dev);
    if (!g_dev && fac) hr = dc(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&g_dev);
    if (nvidia) nvidia->Release();
    if (fac) fac->Release();
    if (!g_dev) { LogLine("helper: device FAILED hr=0x%08X", (unsigned)hr); return false; }

    // wrapper sanity: a real device answers IDXGIDevice
    IDXGIDevice* probe = nullptr;
    hr = g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&probe);
    if (probe) probe->Release();
    // NOTE: native ID3D12Device does NOT implement IDXGIDevice (D3D11-only).
    // E_NOINTERFACE is EXPECTED - never treat it as wrapper evidence.
    LogLine("helper: device %p QI(IDXGIDevice)=0x%08X (normal for D3D12)",
        (void*)g_dev, (unsigned)hr);

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q)))) return false;
    if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)))) return false;
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr,
                                        IID_PPV_ARGS(&g_list)))) return false;
    g_list->Close();
    LogLine("helper: queue/alloc/list ready");
    return true;
}

static bool OpenByValue(unsigned long long hv, REFIID iid, void** out)
{
    // The handle `hv` is ALREADY a valid handle in this process (duplicated
    // from the game process during setup). No need to self-duplicate —
    // that was failing with ERROR_INVALID_HANDLE because the handle is
    // already valid and self-duplication of certain handle types can fail.
    HRESULT hr = g_dev->OpenSharedHandle((HANDLE)(uintptr_t)hv, iid, out);
    if (FAILED(hr))
        LogLine("helper: OpenSharedHandle FAILED hr=0x%08X val=%llu", (unsigned)hr, hv);
    return SUCCEEDED(hr) && *out;
}

static bool ApplySetup(const SetupMsg& m)
{
    LogLine("helper: wire vals color=%llu out=%llu fIn=%llu fOut=%llu w=%u",
        (unsigned long long)m.hColor, (unsigned long long)m.hOut,
        (unsigned long long)m.hFIn, (unsigned long long)m.hFOut, m.w);
    IUnknown* olds[4] = { g_color, g_out, g_fIn, g_fOut };
    for (IUnknown* p : olds)
        if (p) p->Release();
    g_color = nullptr; g_out = nullptr; g_fIn = nullptr; g_fOut = nullptr;

    if (!OpenByValue(m.hColor, IID_PPV_ARGS(&g_color))) { LogLine("helper: open color FAIL"); return false; }
    if (!OpenByValue(m.hOut,   IID_PPV_ARGS(&g_out)))   { LogLine("helper: open out FAIL");   return false; }
    if (!OpenByValue(m.hFIn,   IID_PPV_ARGS(&g_fIn)))   { LogLine("helper: open fIn FAIL");   return false; }
    if (!OpenByValue(m.hFOut,  IID_PPV_ARGS(&g_fOut)))  { LogLine("helper: open fOut FAIL");  return false; }
    if (!g_fInEv) g_fInEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_RESOURCE_DESC rd = g_color->GetDesc();
    {
        // zero-input dummies at color dims (DLSS requires valid bindings)
        if (g_depthIn) { g_depthIn->Release(); g_depthIn = nullptr; }
        if (g_mvIn)    { g_mvIn->Release();    g_mvIn = nullptr; }
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC dd = {};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = rd.Width; dd.Height = rd.Height;
        dd.DepthOrArraySize = 1; dd.MipLevels = 1; dd.SampleDesc.Count = 1;
        dd.Format = DXGI_FORMAT_R32_FLOAT;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_depthIn));
        dd.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_mvIn));
        // force in-loop NGX reinit at new dims
        s_up = nullptr; s_initW = 0; s_initH = 0;
        s_seedVal = m.startVal;
    }
    LogLine("helper: setup %ux%u fmt=%u", m.w, m.h, m.fmt);
    return true;
}

// ----------------------------------------------------------------------------
// --selftest: standalone NGX validation. No game, no pipe.
// Measures commit at each stage and runs 100 real EvaluateFeature calls on
// 512x512 own-device textures. Exit code 0 = all pass.
// ----------------------------------------------------------------------------
static SIZE_T CommitMB()
{
    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return (SIZE_T)(pmc.PagefileUsage / 1048576ULL);
}

static int RunSelfTest()
{
    LogLine("=== SELFTEST begin ===");
    SIZE_T c0 = CommitMB();
    if (!InitDevice()) return 2;
    SIZE_T c1 = CommitMB();
    LogLine("selftest: commit after device: %llu MB (+%llu)", (unsigned long long)c1, (unsigned long long)(c1 - c0));

    // 512x512 own textures
    const UINT W = 512, H = 512;
    ID3D12Resource *color=nullptr, *depth=nullptr, *mv=nullptr, *out=nullptr;
    {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color));
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&depth));
        rd.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mv));
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out));
        if (!color || !depth || !mv || !out) { LogLine("selftest: tex create FAILED"); return 3; }
    }

    if (!g_alloc && FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc))))
        return 3;
    if (!g_list && FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr,
                                                   IID_PPV_ARGS(&g_list)))) return 3;

    // NGX init via production code path
    IUpscaler* up = CreateUpscaler(UPSCALER_DLSS);
    if (!up) { LogLine("selftest: upscaler create FAILED"); return 4; }
    UpscalerInitParams ip = {};
    ip.device = g_dev;
    ip.renderWidth = W;  ip.renderHeight = H;
    ip.displayWidth = W; ip.displayHeight = H;
    ip.appId = 241534720;
    ip.perfQuality = 0;
    ip.mvJittered = true;
    ip.autoExposure = true;
    {
        static wchar_t dllPath[MAX_PATH];
        char selfPathA[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, selfPathA, MAX_PATH);
        char* s = strrchr(selfPathA, '\\');
        if (s) *(s + 1) = 0;
        int n = MultiByteToWideChar(CP_ACP, 0, selfPathA, -1, dllPath, MAX_PATH);
        if (n > 0) lstrcatW(dllPath, L"nvngx_dlss.dll");
        ip.dlssDllPath = dllPath;
        LogLine("selftest: dlssDllPath=%ls", dllPath);
    }
    if (!up->Init(ip) || !up->IsReady()) { LogLine("selftest: NGX Init FAILED"); return 5; }
    SIZE_T c2 = CommitMB();
    LogLine("selftest: commit after NGX init: %llu MB (+%llu)", (unsigned long long)c2, (unsigned long long)(c2 - c1));

    ID3D12Fence* fence = nullptr;
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE evt = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    int pass = 0;
    unsigned long long v = s_seedVal;
    for (int i = 0; i < 100; ++i) {
        UpscalerEvaluateParams ep = {};
        ep.commandList = g_list;
        ep.color = color; ep.depth = depth; ep.motionVectors = mv; ep.output = out;
        ep.jitterX = 0; ep.jitterY = 0;
        ep.mvScaleX = (float)W; ep.mvScaleY = (float)H;
        ep.sharpness = 0.0f;

        bool ok = false;
        if (SUCCEEDED(g_list->Reset(g_alloc, nullptr))) {
            D3D12_RESOURCE_BARRIER b[3] = {};
            for (int k = 0; k < 3; ++k) { b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
            const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b[0].Transition.pResource = color; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b[0].Transition.StateAfter = SRV;
            b[1].Transition.pResource = depth; b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b[1].Transition.StateAfter = SRV;
            b[2].Transition.pResource = mv;    b[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b[2].Transition.StateAfter = SRV;
            g_list->ResourceBarrier(3, b);
            ok = up->Evaluate(ep);
            HRESULT chr = g_list->Close();
            if (SUCCEEDED(chr)) {
                ID3D12CommandList* l[] = { g_list };
                g_q->ExecuteCommandLists(1, l);
                fence->SetEventOnCompletion(++v, evt);
                g_q->Signal(fence, v);
                if (WaitForSingleObject(evt, 10000) == WAIT_OBJECT_0 && ok) ++pass;
            } else ok = false;
        }
        if (!ok && i < 5) LogLine("selftest: iter %d FAILED", i);
        Sleep(8); // pacing
    }

    SIZE_T c3 = CommitMB();
    LogLine("selftest: RESULT %d/100 pass. commit after eval: %llu MB (+%llu)",
        pass, (unsigned long long)c3, (unsigned long long)(c3 - c2));
    LogLine("=== SELFTEST end ===");
    CloseHandle(evt); fence->Release();
    return pass == 100 ? 0 : 6;
}


static bool ReadExact(HANDLE h, PVOID buf, DWORD n)
{
    BYTE* p = (BYTE*)buf;
    DWORD have = 0;
    while (have < n) {
        DWORD got = 0;
        if (!ReadFile(h, p + have, n - have, &got, nullptr) || got == 0) return false;
        have += got;
    }
    return true;
}
static bool WriteExact(HANDLE h, const VOID* buf, DWORD n)
{
    DWORD w = 0;
    return WriteFile(h, buf, n, &w, nullptr) && w == n;
}
static bool ReadTag(HANDLE h, char* t)
{
    BYTE b = 0;
    if (!ReadExact(h, &b, 1)) return false;
    *t = (char)b;
    return true;
}
int main(int argc, char** argv)
{
    if (argc > 1 && !lstrcmpiA(argv[1], "--selftest")) {
        int rc = RunSelfTest();
        LogLine("selftest: exit=%d", rc);
        return rc;
    }
    LogLine("helper: start pid=%lu parentArg=%s", GetCurrentProcessId(),
            argc > 1 ? argv[1] : "-");

    if (!InitDevice()) { LogLine("helper: init device failed"); return 2; }

    HANDLE pipe = CreateFileA("\\\\.\\pipe\\ScaleNG_NGX", GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        LogLine("helper: pipe connect FAILED err=%lu", GetLastError());
        return 3;
    }
    // BYTE mode: stream framing handled by exact-size read loops.


    unsigned int hello[2] = {};
    if (!ReadExact(pipe, hello, sizeof(hello)) || hello[0] != 0x58474E53) {
        LogLine("helper: bad hello");
        return 4;
    }
    unsigned int ackw[2] = { 0x58474E48, 1 };
    WriteExact(pipe, ackw, sizeof(ackw));
    LogLine("helper: handshake done");

    SetupMsg sm = {};
    for (;;) {
        if (!ReadExact(pipe, &sm, sizeof(sm))) break;
        if (!ApplySetup(sm)) {
            unsigned int r = 0x4C494146;
            WriteExact(pipe, &r, sizeof(r));
            continue;
        }
        unsigned int r = 0x59414B4F;
        WriteExact(pipe, &r, sizeof(r));

        // FRAME LOOP: one submission per game signal; NGX lands next stage.
        unsigned long long v = s_seedVal;
        unsigned long long frames = 0;
        DWORD lastReport = GetTickCount();
        HANDLE parent = argc > 1 ? OpenProcess(0x00100000L /*PROCESS_SYNCHRONIZE*/, FALSE, atoi(argv[1])) : nullptr;
        for (;;) {
            if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
                LogLine("helper: parent exited - bye");
                break;
            }

            // FRAME MSG (protocol v2): blocking read - the ASI sends one
            // message per captured frame. SetupMsg may interleave; handle it.
            {
                DWORD availNow = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &availNow, nullptr)) break;
                if (availNow == 0) {
                    // nothing yet: brief sleep to yield CPU (game paces us)
                    Sleep(1);
                    static unsigned s_idle = 0;
                    if ((++s_idle % 600) == 0) LogLine("helper: idle (waiting frames)");
                    continue;
                }
                char tg = 0;
                unsigned long long fv = 0;
                LogLine("helper: waiting msg...");
                if (!ReadTag(pipe, &tg)) { LogLine("helper: tag read FAILED"); break; }
                if (tg == 0x53) { // 'S' setup mid-stream
                    SetupMsg ms{};
                    if (!ReadExact(pipe, &ms, sizeof(ms))) break;
                    LogLine("helper: mid-stream setup");
                    if (!ApplySetup(ms)) break;
                    continue;
                }
                if (tg != 0x46) { LogLine("helper: BAD tag %02X", (unsigned)tg); break; }
                if (!ReadExact(pipe, &fv, sizeof(fv))) { LogLine("helper: frame read FAILED"); break; }
                LogLine("helper: frame v=%llu", (unsigned long long)fv);
                if (fv == 0xFFFFFFFFFFFFFFFF) { // setup marker follows
                    SetupMsg m2{};
                        if (!ReadExact(pipe, &m2, sizeof(m2))) break;
                    if (!ApplySetup(m2)) break;
                    continue;
                }
                v = fv;
            }
            ++frames;
            // NGX evaluate on shared color -> shared out (production path).

            if (!s_up) {
                const UINT W = 1920, H = 1080; // provisional; re-init handles real dims
                D3D12_RESOURCE_DESC cd = g_color ? g_color->GetDesc() : D3D12_RESOURCE_DESC{};
                UINT w = (UINT)cd.Width, h = cd.Height;
                s_up = CreateUpscaler(UPSCALER_DLSS);
                if (s_up) {
                    UpscalerInitParams ip = {};
                    ip.device = g_dev;
                    ip.renderWidth = w;  ip.renderHeight = h;
                    ip.displayWidth = w; ip.displayHeight = h;
                    ip.appId = 241534720; ip.perfQuality = 0;
                    ip.mvJittered = true; ip.autoExposure = true;
                    static wchar_t dllPath[MAX_PATH];
                    char selfA[MAX_PATH] = {};
                    GetModuleFileNameA(nullptr, selfA, MAX_PATH);
                    char* s = strrchr(selfA, '\\');
                    if (s) *(s + 1) = 0;
                    MultiByteToWideChar(CP_ACP, 0, selfA, -1, dllPath, MAX_PATH);
                    lstrcatW(dllPath, L"nvngx_dlss.dll");
                    ip.dlssDllPath = dllPath;
                    if (!s_up->Init(ip) || !s_up->IsReady()) {
                        LogLine("helper: in-loop NGX init FAILED");
                        s_up = nullptr;
                    } else {
                        LogLine("helper: in-loop NGX init ok (%ux%u)", w, h);
                    }
                }
            }

            bool recorded = false;
            if (s_up && SUCCEEDED(g_list->Reset(g_alloc, nullptr))) {
                const D3D12_RESOURCE_STATES SRV =
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                D3D12_RESOURCE_BARRIER b[4] = {};
                for (int k = 0; k < 4; ++k) { b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
                // simultaneous-access resources: states tracked per-device; on
                // this device they live in COMMON between frames.
                b[0].Transition.pResource = g_color; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b[0].Transition.StateAfter = SRV;
                b[1].Transition.pResource = g_out;   b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                g_list->ResourceBarrier(2, b);

                UpscalerEvaluateParams ep = {};
                ep.commandList = g_list;
                ep.color = g_color;
                ep.depth = g_depthIn;
                ep.motionVectors = g_mvIn;
                ep.output = g_out;
                ep.jitterX = 0; ep.jitterY = 0;
                D3D12_RESOURCE_DESC cd = g_color->GetDesc();
                ep.mvScaleX = (float)cd.Width; ep.mvScaleY = (float)cd.Height;
                ep.sharpness = 0.0f;

                recorded = s_up->Evaluate(ep);
                HRESULT chr = g_list->Close();
                if (SUCCEEDED(chr)) {
                    ID3D12CommandList* l[] = { g_list };
                    g_q->ExecuteCommandLists(1, l);
                } else recorded = false;
            }
            if (!recorded && SUCCEEDED(g_list->Reset(g_alloc, nullptr))) {
                g_list->Close(); // keep allocator warm even on skip
                ID3D12CommandList* l[] = { g_list };
                g_q->ExecuteCommandLists(1, l);
            }
            g_q->Signal(g_fOut, v); // enqueue BEFORE ack - ASI GPU-waits on this

            static unsigned s_evalOk = 0, s_evalSkip = 0;
            if (recorded) ++s_evalOk; else ++s_evalSkip;

            // ACK: tell ASI the signal is enqueued (protocol v2)
            {
                DWORD wr2 = 0;
                WriteFile(pipe, &v, sizeof(v), &wr2, nullptr);
            }

            DWORD nowT = GetTickCount();
            if ((nowT - lastReport) > 5000) {
                PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
                LogLine("helper: %llu frames (v=%llu) ok=%u skip=%u commit=%.0fMB",
                    frames, v, s_evalOk, s_evalSkip,
                    pmc.PagefileUsage / 1048576.0);
                lastReport = nowT;
            }

            DWORD avail = 0;
            if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= 1) {
                // peek tag without consuming via manual look is complex on pipes;
                // instead: only treat as setup if a tag byte read confirms
                char t1 = 0;
                if (ReadTag(pipe, &t1)) {
                    if (t1 == 0x53) {
                        SetupMsg m2 = {};
                        if (!ReadExact(pipe, &m2, sizeof(m2))) break;
                        LogLine("helper: re-setup");
                        if (!ApplySetup(m2)) break;
                    } else if (t1 == 0x46) {
                        unsigned long long skipV = 0;
                        if (!ReadExact(pipe, &skipV, sizeof(skipV))) break;
                        continue; // frame msg consumed out-of-band; loop handles order
                    }
                } else break;
            }
        }
        LogLine("helper: frame loop ended - waiting for new setup or exit");
    }
    LogLine("helper: pipe closed - exit");
    return 0;
}
