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
};

static ID3D12Device*              g_dev    = nullptr;
static ID3D12CommandQueue*        g_q      = nullptr;
static ID3D12CommandAllocator*    g_alloc  = nullptr;
static ID3D12GraphicsCommandList* g_list   = nullptr;
static ID3D12Resource*            g_color  = nullptr;
static ID3D12Resource*            g_out    = nullptr;
static ID3D12Fence*               g_fIn    = nullptr;
static ID3D12Fence*               g_fOut   = nullptr;
static HANDLE                     g_fInEv  = nullptr;

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
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)hv,
                         GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return false;
    HRESULT hr = g_dev->OpenSharedHandle(dup, iid, out);
    CloseHandle(dup);
    return SUCCEEDED(hr) && *out;
}

static bool ApplySetup(const SetupMsg& m)
{
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
    LogLine("helper: setup %ux%u fmt=%u (color desc %ux%u)",
        m.w, m.h, m.fmt, (unsigned)rd.Width, rd.Height);
    return true;
}

int main(int argc, char** argv)
{
    LogLine("helper: start pid=%lu parentArg=%s", GetCurrentProcessId(),
            argc > 1 ? argv[1] : "-");

    if (!InitDevice()) { LogLine("helper: init device failed"); return 2; }

    HANDLE pipe = CreateFileA("\\\\.\\pipe\\ScaleNG_NGX", GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        LogLine("helper: pipe connect FAILED err=%lu", GetLastError());
        return 3;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    unsigned int hello[2] = {};
    DWORD br = 0;
    if (!ReadFile(pipe, hello, sizeof(hello), &br, nullptr) ||
        hello[0] != 0x58474E53) { // 'SNGX'
        LogLine("helper: bad hello (%08X)", hello[0]);
        return 4;
    }
    unsigned int ack[2] = { 0x58474E48, 1 }; // 'HNGX'
    WriteFile(pipe, ack, sizeof(ack), &br, nullptr);
    LogLine("helper: handshake done");

    SetupMsg sm = {};
    while (ReadFile(pipe, &sm, sizeof(sm), &br, nullptr) && br == sizeof(sm)) {
        if (!ApplySetup(sm)) {
            unsigned int r = 0x4C494146; // 'FAIL'
            WriteFile(pipe, &r, sizeof(r), &br, nullptr);
            continue;
        }
        unsigned int r = 0x59414B4F; // 'OKAY'
        WriteFile(pipe, &r, sizeof(r), &br, nullptr);

        // FRAME LOOP: one submission per game signal; NGX lands next stage.
        unsigned long long v = 0;
        unsigned long long frames = 0;
        DWORD lastReport = GetTickCount();
        HANDLE parent = argc > 1 ? OpenProcess(0x00100000L /*PROCESS_SYNCHRONIZE*/, FALSE, atoi(argv[1])) : nullptr;
        for (;;) {
            if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
                LogLine("helper: parent exited - bye");
                break;
            }

            UINT64 completed = g_fIn->GetCompletedValue();
            if (completed >= v) {
                if (v != 0 && completed > v + 64)
                    LogLine("helper: resync v %llu -> %llu", v, completed + 1);
                v = completed + 1;
            }
            if (FAILED(g_fIn->SetEventOnCompletion(v, g_fInEv))) break;
            if (WaitForSingleObject(g_fInEv, INFINITE) != WAIT_OBJECT_0) break;
            ++frames;

            // TODO(next stage): NGX evaluate color->out recorded here.
            if (SUCCEEDED(g_list->Reset(g_alloc, nullptr))) {
                g_list->Close();
                ID3D12CommandList* l[] = { g_list };
                g_q->ExecuteCommandLists(1, l);
            }
            g_q->Signal(g_fOut, v); // ALWAYS - stage3 must never starve

            DWORD nowT = GetTickCount();
            if ((nowT - lastReport) > 5000) {
                PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
                LogLine("helper: %llu frames (v=%llu) commit=%.0fMB ws=%.0fMB",
                    frames, v, pmc.PagefileUsage / 1048576.0,
                    pmc.WorkingSetSize / 1048576.0);
                lastReport = nowT;
            }

            DWORD avail = 0;
            if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= sizeof(SetupMsg)) {
                SetupMsg m2 = {};
                DWORD r2 = 0;
                if (ReadFile(pipe, &m2, sizeof(m2), &r2, nullptr) && r2 == sizeof(m2)) {
                    LogLine("helper: re-setup");
                    if (!ApplySetup(m2)) break;
                }
            }
        }
        LogLine("helper: frame loop ended - waiting for new setup or exit");
    }
    LogLine("helper: pipe closed - exit");
    return 0;
}
