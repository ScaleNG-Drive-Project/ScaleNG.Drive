// dmpscan - userdump autopsy via mapped-view MiniDumpReadDumpStream.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <minidumpapiset.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "dbghelp.lib")

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: dmpscan <minidump>\n"); return 1; }
    HANDLE f = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { printf("open failed\n"); return 1; }
    HANDLE map = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map) return 2;
    PVOID base = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!base) return 2;

    typedef BOOL(WINAPI* PFN_Read)(PVOID, ULONG, PMINIDUMP_DIRECTORY*, PVOID*, ULONG*);
    HMODULE dh = LoadLibraryA("dbghelp.dll");
    PFN_Read Read = dh ? (PFN_Read)GetProcAddress(dh, "MiniDumpReadDumpStream") : nullptr;
    if (!Read) { printf("no dbghelp api\n"); return 3; }

    // Exception stream (6)
    PMINIDUMP_DIRECTORY dir = nullptr; PVOID stream = nullptr; ULONG size = 0;
    if (!Read(base, 6, &dir, &stream, &size) || !stream) { printf("no exception stream\n"); return 4; }
    auto ex = (MINIDUMP_EXCEPTION_STREAM*)stream;
    ULONG64 rip = ex->ExceptionRecord.ExceptionAddress;
    ULONG tid = ex->ThreadId;
    printf("thread %lu  exception 0x%016llX  code 0x%08X\n",
        tid, (unsigned long long)rip, ex->ExceptionRecord.ExceptionCode);

    // Module list (4)
    PMINIDUMP_MODULE_LIST mods = nullptr;
    if (!Read(base, 4, &dir, (PVOID*)&mods, &size) || !mods) return 5;
    struct Mod { std::string name, shortname; ULONG64 base_, size_; };
    std::vector<Mod> mv;
    for (ULONG32 i = 0; i < mods->NumberOfModules; ++i) {
        auto& m = mods->Modules[i];
        Mod e; e.base_ = m.BaseOfImage; e.size_ = m.SizeOfImage;
        char buf[MAX_PATH * 2] = {};
        if (m.ModuleNameRva) {
            MINIDUMP_STRING* ms = (MINIDUMP_STRING*)((BYTE*)base + m.ModuleNameRva);
            WideCharToMultiByte(CP_ACP, 0, ms->Buffer, -1, buf, sizeof(buf), nullptr, nullptr);
        }
        e.name = buf;
        size_t s = e.name.find_last_of("\\/");
        e.shortname = (s == std::string::npos) ? e.name : e.name.substr(s + 1);
        mv.push_back(e);
    }
    auto locate = [&](ULONG64 a) -> const Mod* {
        for (auto& m : mv)
            if (a >= m.base_ && a < m.base_ + m.size_) return &m;
        return nullptr;
    };
    const Mod* fm = locate(rip);
    if (fm) printf("FAULT: %s+0x%llX\n", fm->shortname.c_str(),
                   (unsigned long long)(rip - fm->base_));

    // Thread list (3)
    PMINIDUMP_THREAD_LIST tl = nullptr;
    if (!Read(base, 3, &dir, (PVOID*)&tl, &size) || !tl) return 6;
    MINIDUMP_THREAD* th = nullptr;
    for (ULONG32 i = 0; i < tl->NumberOfThreads; ++i)
        if (tl->Threads[i].ThreadId == tid) { th = &tl->Threads[i]; break; }
    if (!th || !th->Stack.Memory.DataSize) { printf("no stack\n"); return 7; }

    std::vector<BYTE> stk(th->Stack.Memory.DataSize);
    memcpy(stk.data(), (BYTE*)base + th->Stack.Memory.Rva, stk.size());
    printf("--- stack-scan backtrace (code refs) ---\n");
    int shown = 0;
    for (size_t off = 0; off + 8 <= stk.size() && shown < 40; off += 8) {
        ULONG64 v = *(ULONG64*)(stk.data() + off);
        const Mod* m = locate(v);
        if (m && m->shortname != "ntdll.dll") {
            printf("  rsp+%05llX %s+0x%llX\n",
                (unsigned long long)off, m->shortname.c_str(),
                (unsigned long long)(v - m->base_));
            ++shown;
        }
    }
    return 0;
}
