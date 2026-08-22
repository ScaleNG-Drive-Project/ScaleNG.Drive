#pragma once
#include <windows.h>

extern wchar_t g_logPath[MAX_PATH];

// ---------------------------------------------------------------------------
// Kernel32-only logging. NO CRT: the very first CRT fprintf call in this
// loading context (ASI loaded by OptiScaler's winmm.dll at process start)
// fast-fails the process with 0xC0000409. Everything here is CreateFile /
// WriteFile / GetLocalTime / GetModuleFileNameW only.
// ---------------------------------------------------------------------------

inline void ComputeLogPath()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&ComputeLogPath, &self);
    if (self) GetModuleFileNameW(self, g_logPath, MAX_PATH);
    wchar_t* slash = nullptr;
    for (wchar_t* p = g_logPath; *p; ++p)
        if (*p == L'\\') slash = p;
    const wchar_t name[] = L"ScaleNG.log";
    size_t n = 0;
    while (name[n]) ++n;
    size_t base = slash ? (size_t)(slash - g_logPath + 1) : 0;
    size_t i = 0;
    for (; i < n && base + i + 1 < MAX_PATH; ++i)
        g_logPath[base + i] = name[i];
    g_logPath[base + i] = L'\0';
}

inline void RawLog(const char* msg)
{
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        size_t len = 0;
        while (msg[len]) ++len;
        DWORD written = 0;
        WriteFile(h, msg, (DWORD)len, &written, nullptr);
        CloseHandle(h);
    }
}

namespace LogImpl {
    inline char g_buf[4096];
    inline long g_lock = 0;

    inline void PutStr(char*& p, char* end, const char* s)
    {
        while (*s && p < end) *p++ = *s++;
    }

    inline void PutUInt(char*& p, char* end, unsigned long long v, int minDigits)
    {
        char tmp[24];
        int n = 0;
        do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n < minDigits) tmp[n++] = '0';
        while (n > 0 && p < end) *p++ = tmp[--n];
    }

    inline void PutInt(char*& p, char* end, long long v)
    {
        if (v < 0) { if (p < end) *p++ = '-'; v = -v; }
        PutUInt(p, end, (unsigned long long)v, 1);
    }

    inline void PutHex(char*& p, char* end, unsigned long long v, int minDigits)
    {
        const char* hex = "0123456789ABCDEF";
        char tmp[24];
        int n = 0;
        do { tmp[n++] = hex[v & 0xF]; v >>= 4; } while (v);
        while (n < minDigits) tmp[n++] = '0';
        while (n > 0 && p < end) *p++ = tmp[--n];
    }

    inline void PutFloat(char*& p, char* end, double v, int digits)
    {
        if (v < 0) { if (p < end) *p++ = '-'; v = -v; }
        double mul = 1.0;
        for (int i = 0; i < digits; ++i) mul *= 10.0;
        unsigned long long ip = (unsigned long long)v;
        unsigned long long fp = (unsigned long long)((v - (double)ip) * mul + 0.500001);
        if (fp >= (unsigned long long)mul) { ++ip; fp -= (unsigned long long)mul; }
        PutUInt(p, end, ip, 1);
        if (p < end) *p++ = '.';
        PutUInt(p, end, fp, digits);
    }

    inline void PutWide(char*& p, char* end, const wchar_t* s)
    {
        while (*s && p + 3 < end) {
            unsigned c = (unsigned)*s++;
            if (c < 0x80) *p++ = (char)c;
            else if (c < 0x800) { *p++ = (char)(0xC0 | (c >> 6)); *p++ = (char)(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { *p++ = (char)(0xE0 | (c >> 12)); *p++ = (char)(0x80 | ((c >> 6) & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F)); }
            else { *p++ = (char)0xEF; *p++ = (char)0xBF; *p++ = (char)0xBD; }
        }
    }
} // namespace LogImpl

inline void LogV(const char* fmt, va_list ap)
{
    using namespace LogImpl;
    while (InterlockedExchange(&g_lock, 1)) Sleep(0);
    char* p = g_buf;
    char* end = g_buf + sizeof(g_buf) - 1;
    SYSTEMTIME st;
    GetLocalTime(&st);
    PutStr(p, end, "[");
    PutUInt(p, end, st.wHour, 2); PutStr(p, end, ":");
    PutUInt(p, end, st.wMinute, 2); PutStr(p, end, ":");
    PutUInt(p, end, st.wSecond, 2); PutStr(p, end, ".");
    PutUInt(p, end, st.wMilliseconds, 3); PutStr(p, end, "] ");
    while (*fmt && p < end) {
        char c = *fmt++;
        if (c != '%') { *p++ = c; continue; }
        bool wide = false;
        bool ll = false;
        int hexDigits = 0;
        int floatDigits = 2;
        if (*fmt == 'l' && fmt[1] == 'l') { ll = true; fmt += 2; }
        if (!ll && *fmt == 'l') { wide = true; ++fmt; }
        if (*fmt == '0') {
            int d = 0;
            const char* q = fmt + 1;
            while (*q >= '0' && *q <= '9' && d < 2) { hexDigits = hexDigits * 10 + (*q - '0'); ++q; ++d; }
            if (d > 0) fmt = q;
        }
        if (*fmt == '.' && fmt[1] >= '0' && fmt[1] <= '9') {
            floatDigits = fmt[1] - '0';
            fmt += 2;
        }
        char spec = *fmt++;
        switch (spec) {
        case 's':
            if (wide) PutWide(p, end, va_arg(ap, const wchar_t*));
            else PutStr(p, end, va_arg(ap, const char*));
            break;
        case 'p': PutHex(p, end, (unsigned long long)va_arg(ap, void*), 0); break;
        case 'd':
            if (ll) PutInt(p, end, va_arg(ap, long long));
            else PutInt(p, end, va_arg(ap, int));
            break;
        case 'u':
            if (ll) PutUInt(p, end, va_arg(ap, unsigned long long), 1);
            else PutUInt(p, end, va_arg(ap, unsigned int), 1);
            break;
        case 'X':
            if (ll) PutHex(p, end, va_arg(ap, unsigned long long), hexDigits);
            else PutHex(p, end, va_arg(ap, unsigned int), hexDigits);
            break;
        case 'f': PutFloat(p, end, va_arg(ap, double), floatDigits); break;
        case '%': if (p < end) *p++ = '%'; break;
        default: break;
        }
    }
    if (p < end) *p++ = '\n';
    *p = '\0';
    RawLog(g_buf);
    InterlockedExchange(&g_lock, 0);
}

inline void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

inline void LogInit()
{
    ComputeLogPath();
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
            DWORD written = 0;
            WriteFile(h, "\xEF\xBB\xBF", 3, &written, nullptr);
        }
        CloseHandle(h);
    }
    Log("ScaleNG.asi log started");
}