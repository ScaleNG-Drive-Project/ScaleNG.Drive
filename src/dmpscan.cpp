// dmpscan - userdump autopsy: faulting frame + StackWalk64 unwind + module map.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <minidumpapiset.h>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "dbghelp.lib")

struct Mod { std::string name, shortname; ULONG64 base_, size_; };
static std::vector<Mod> g_mods;

struct Run { ULONG64 start, size, rva; };
static std::vector<Run> g_runs;
static PVOID g_base = nullptr;

static const Mod* Locate(ULONG64 a)
{
    for (auto& m : g_mods)
        if (a >= m.base_ && a < m.base_ + m.size_) return &m;
    return nullptr;
}

static LONG g_reads = 0;
static BOOL CALLBACK ReadMem(HANDLE, ULONG64 addr, PVOID buf, ULONG sz, PULONG readn)
{
    InterlockedIncrement(&g_reads);
    InterlockedIncrement(&g_reads);
    for (auto& r : g_runs) {
        if (addr >= r.start && addr + sz <= r.start + r.size) {
            memcpy(buf, (BYTE*)g_base + r.rva + (addr - r.start), sz);
            if (readn) *readn = sz;
            return TRUE;
        }
    }
    for (auto& r : g_runs) {
        if (addr >= r.start && addr < r.start + r.size) {
            ULONG64 avail = r.start + r.size - addr;
            ULONG cpy = (ULONG)(avail < sz ? avail : sz);
            memcpy(buf, (BYTE*)g_base + r.rva + (addr - r.start), cpy);
            if (readn) *readn = cpy;
            return TRUE;
        }
    }
    if (readn) *readn = 0;
    return FALSE;
}

static ULONG64 CALLBACK ModBase(HANDLE, ULONG64 addr)
{
    const Mod* m = Locate(addr);
    return m ? (ULONG64)m->base_ : 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: dmpscan <minidump>\n"); return 1; }
    HANDLE f = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { printf("open failed\n"); return 1; }
    HANDLE map = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map) return 2;
    g_base = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!g_base) return 2;

    typedef BOOL(WINAPI* PFN_Read)(PVOID, ULONG, PMINIDUMP_DIRECTORY*, PVOID*, ULONG*);
    HMODULE dh = LoadLibraryA("dbghelp.dll");
    PFN_Read Read = dh ? (PFN_Read)GetProcAddress(dh, "MiniDumpReadDumpStream") : nullptr;
    if (!Read) { printf("no dbghelp api\n"); return 3; }

    PMINIDUMP_DIRECTORY dir = nullptr; PVOID stream = nullptr; ULONG size = 0;

    // Exception (6)
    if (!Read(g_base, 6, &dir, &stream, &size) || !stream) { printf("no exception stream\n"); return 4; }
    auto ex = (MINIDUMP_EXCEPTION_STREAM*)stream;
    ULONG64 rip = ex->ExceptionRecord.ExceptionAddress;
    ULONG tid = ex->ThreadId;
    printf("thread %lu  exception 0x%016llX  code 0x%08X\n",
        tid, (unsigned long long)rip, ex->ExceptionRecord.ExceptionCode);

    // Modules (4)
    PMINIDUMP_MODULE_LIST mods = nullptr;
    if (!Read(g_base, 4, &dir, (PVOID*)&mods, &size) || !mods) return 5;
    for (ULONG32 i = 0; i < mods->NumberOfModules; ++i) {
        auto& m = mods->Modules[i];
        Mod e; e.base_ = m.BaseOfImage; e.size_ = m.SizeOfImage;
        char buf[MAX_PATH * 2] = {};
        if (m.ModuleNameRva) {
            MINIDUMP_STRING* ms = (MINIDUMP_STRING*)((BYTE*)g_base + m.ModuleNameRva);
            WideCharToMultiByte(CP_ACP, 0, ms->Buffer, -1, buf, sizeof(buf), nullptr, nullptr);
        }
        e.name = buf;
        size_t s = e.name.find_last_of("\\/");
        e.shortname = (s == std::string::npos) ? e.name : e.name.substr(s + 1);
        g_mods.push_back(e);
    }

    // Threads (3)
    PMINIDUMP_THREAD_LIST tl = nullptr;
    if (!Read(g_base, 3, &dir, (PVOID*)&tl, &size) || !tl) return 6;
    MINIDUMP_THREAD* th = nullptr;
    for (ULONG32 i = 0; i < tl->NumberOfThreads; ++i)
        if (tl->Threads[i].ThreadId == tid) { th = &tl->Threads[i]; break; }
    if (!th) { printf("thread not found\n"); return 7; }

    // Memory64List (9) or legacy MemoryList (5)
    PMINIDUMP_MEMORY64_LIST ml = nullptr;
    if (Read(g_base, 9, &dir, (PVOID*)&ml, &size) && ml) {
        ULONG64 rva = ml->BaseRva;
        for (ULONG64 i = 0; i < ml->NumberOfMemoryRanges; ++i) {
            g_runs.push_back({ ml->MemoryRanges[i].StartOfMemoryRange,
                               ml->MemoryRanges[i].DataSize, rva });
            rva += ml->MemoryRanges[i].DataSize;
        }
    }
    if (g_runs.empty()) {
        PMINIDUMP_MEMORY_LIST m5 = nullptr;
        if (Read(g_base, 5, &dir, (PVOID*)&m5, &size) && m5) {
            for (ULONG32 i = 0; i < m5->NumberOfMemoryRanges; ++i)
                g_runs.push_back({ m5->MemoryRanges[i].StartOfMemoryRange,
                                   m5->MemoryRanges[i].Memory.DataSize,
                                   m5->MemoryRanges[i].Memory.Rva });
        }
    }

    // Faulting frame
    const Mod* fm = Locate(rip);
    if (fm) printf("FAULT: %s+0x%llX\n", fm->shortname.c_str(),
                   (unsigned long long)(rip - fm->base_));

    // Unwind
    printf("runs=%llu\n", (unsigned long long)g_runs.size());
    CONTEXT ctx{};
    memcpy(&ctx, (BYTE*)g_base + ex->ThreadContext.Rva,
           ex->ThreadContext.DataSize < sizeof(CONTEXT) ? ex->ThreadContext.DataSize : sizeof(CONTEXT));
    typedef BOOL(WINAPI* PFN_SW)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
        PREAD_PROCESS_MEMORY_ROUTINE64, PGET_MODULE_BASE_ROUTINE64,
        PTRANSLATE_ADDRESS_ROUTINE64, PVOID);
    PFN_SW SW = dh ? (PFN_SW)GetProcAddress(dh, "StackWalk64") : nullptr;
    if (!SW) { printf("no StackWalk64\n"); return 8; }
    // Register modules so the x64 unwinder can find .pdata
    typedef BOOL(WINAPI* PFN_SymInit)(HANDLE, PCSTR, BOOL);
    typedef ULONG64(WINAPI* PFN_SymLoad)(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD, ULONG);
    PFN_SymInit SymInit = dh ? (PFN_SymInit)GetProcAddress(dh, "SymInitialize") : nullptr;
    PFN_SymLoad SymLoad = dh ? (PFN_SymLoad)GetProcAddress(dh, "SymLoadModuleEx") : nullptr;
    const HANDLE PS = (HANDLE)(uintptr_t)0x534E4758; // 'SNGX' pseudo process
    if (SymInit) SymInit((HANDLE)PS, nullptr, FALSE);
    int loaded = 0;
    if (SymLoad)
        for (auto& m : g_mods)
            if (SymLoad((HANDLE)PS, nullptr, m.name.c_str(), m.shortname.c_str(),
                    m.base_, (DWORD)m.size_, 0)) ++loaded;
    printf("modules registered: %d/%d\n", loaded, (int)g_mods.size());
    typedef ULONG64(WINAPI* PFN_GetBase)(HANDLE, ULONG64);
    PFN_GetBase GetB = dh ? (PFN_GetBase)GetProcAddress(dh, "SymGetModuleBase64") : nullptr;
    if (GetB) printf("SymGetModuleBase64(rip)=0x%016llX\n", (unsigned long long)GetB((HANDLE)PS, rip));
    STACKFRAME64 sf{};
    sf.AddrPC.Mode = AddrModeFlat;    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrFrame.Mode = AddrModeFlat; sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Mode = AddrModeFlat; sf.AddrStack.Offset = ctx.Rsp;
    bool covered=false; for(auto&r:g_runs){ if(ctx.Rsp>=r.start && ctx.Rsp<r.start+r.size) covered=true; }
    printf("ctx.rsp=0x%016llX covered=%d ripmod=%s\n",(unsigned long long)ctx.Rsp,(int)covered, Locate(ctx.Rip)?Locate(ctx.Rip)->shortname.c_str():"?");
    printf("--- unwind ---\n");
    int n = 0;
            while (n < 32) {
                BOOL oksw = SW(IMAGE_FILE_MACHINE_AMD64, (HANDLE)PS, (HANDLE)PS, &sf, nullptr,
                        ReadMem, ModBase, nullptr, nullptr);
                if (!oksw) {
                    printf("  [SW failed n=%d reads=%ld GLE=%lu pc=%016llX]\n",
                        n, g_reads, (unsigned long)GetLastError(),
                        (unsigned long long)sf.AddrPC.Offset);
                    break;
                }
        if (sf.AddrPC.Offset == 0) break;
        const Mod* m = Locate(sf.AddrPC.Offset);
        printf("  #%02d %s\n", n, m ? (m->shortname + "+0x" +
            std::to_string(sf.AddrPC.Offset - m->base_)).c_str() : "???");
        ++n;
    }

    // Raw stack scan as backup
    if (!th->Stack.Memory.DataSize) return 0;
    std::vector<BYTE> stk(th->Stack.Memory.DataSize);
    memcpy(stk.data(), (BYTE*)g_base + th->Stack.Memory.Rva, stk.size());
    printf("--- stack-scan ---\n");
    int shown = 0;
    // Pass A: game-module frames only (pre-crash chain lives past handler noise)
    for (size_t off = 0; off + 8 <= stk.size() && shown < 30; off += 8) {
        ULONG64 v = *(ULONG64*)(stk.data() + off);
        const Mod* m = Locate(v);
        if (m && m->shortname == "BeamNG.drive.x64.exe" &&
            (v - m->base_) > 0x100000 && (v - m->base_) < 0x2000000) {
            printf("  BEAM rsp+%05llX +0x%llX\n",
                (unsigned long long)off, (unsigned long long)(v - m->base_));
            ++shown;
        }
    }
    shown = 0;
    // Pass B: everything else
    for (size_t off = 0; off + 8 <= stk.size() && shown < 25; off += 8) {
        ULONG64 v = *(ULONG64*)(stk.data() + off);
        const Mod* m = Locate(v);
        if (m && m->shortname != "ntdll.dll" && m->shortname != "BeamNG.drive.x64.exe") {
            printf("  rsp+%05llX %s+0x%llX\n",
                (unsigned long long)off, m->shortname.c_str(),
                (unsigned long long)(v - m->base_));
            ++shown;
        }
    }
    return 0;
}
