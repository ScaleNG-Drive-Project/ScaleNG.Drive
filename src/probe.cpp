#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <minidumpapiset.h>
PMINIDUMP_DIRECTORY g_dir = 0;
typedef BOOL(WINAPI* PFN_Read)(HANDLE, PMINIDUMP_DIRECTORY, PVOID, ULONG, PMINIDUMP_STREAM*);
int main(){ return 0; }