#include "common.h"
#include <cstdarg>
#include <share.h>

static CRITICAL_SECTION g_logLock;
static HANDLE           g_console = INVALID_HANDLE_VALUE;
static FILE*            g_logFile = nullptr;
static bool             g_logFileTried = false;

void InitLog() { InitializeCriticalSection(&g_logLock); }

void OpenConsole() {
    EnterCriticalSection(&g_logLock);
    AllocConsole();
    SetConsoleTitleW(L"sc-offline");
    g_console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    LeaveCriticalSection(&g_logLock);
}


void Log(const char* fmt, ...) {
    char buf[1024];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, a);
    va_end(a);
    if (n < 0 || n > static_cast<int>(sizeof(buf)) - 3) n = static_cast<int>(sizeof(buf)) - 3;

    EnterCriticalSection(&g_logLock);
    if (!g_logFileTried) {
        g_logFileTried = true;
        char path[MAX_PATH];
        DWORD len = GetEnvironmentVariableA("SC_OFFLINE_MOD_LOG", path, sizeof(path));
        if (len > 0 && len < sizeof(path)) g_logFile = _fsopen(path, "w", _SH_DENYNO);
    }
    if (g_console != INVALID_HANDLE_VALUE) {
        buf[n] = '\r'; buf[n + 1] = '\n';
        DWORD written;
        WriteConsoleA(g_console, buf, static_cast<DWORD>(n + 2), &written, nullptr);
        buf[n] = '\0';
    }
    if (g_logFile) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(g_logFile, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_logFile);
    }
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(buf);
}

Section g_text, g_rdata;

Section FindSection(const char* name) {
    auto* image = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt    = reinterpret_cast<IMAGE_NT_HEADERS*>(image + reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (strncmp(reinterpret_cast<const char*>(sec->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0)
            return { image + sec->VirtualAddress, sec->Misc.VirtualSize };
    return {};
}

bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t n, DWORD& err) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) { err = GetLastError(); return false; }
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

void LogPatch(const char* name, const PatchStatus& st) {
    switch (st.result) {
    case PatchResult::Applied:         Log("[+] %s: patched %d site(s), first at 0x%p", name, st.sites, st.at); break;
    case PatchResult::NotFound:        Log("[!] %s: pattern not found (game updated?)", name); break;
    case PatchResult::WrongMatchCount: Log("[!] %s: pattern matched %d time(s), expected %d; refused to patch", name, st.sites, st.expected); break;
    case PatchResult::ProtectFailed:   Log("[!] %s: VirtualProtect failed (%lu)", name, st.err); break;
    case PatchResult::NotRun:          Log("[-] %s: not run", name); break;
    }
}

bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool ShipsFilePath(char* path, DWORD n) {
    const DWORD len = GetEnvironmentVariableA("SC_OFFLINE_SHIPS_FILE", path, n);
    return len > 0 && len < n;
}

bool SiblingPath(const char* envVar, const char* file, char* path, DWORD n) {
    const DWORD len = GetEnvironmentVariableA(envVar, path, n);
    if (len == 0 || len >= n) return false;
    char* back = strrchr(path, '\\');
    char* fwd  = strrchr(path, '/');
    char* slash = back > fwd ? back : fwd;   // the last separator of either kind: Windows accepts a mix
    if (!slash || static_cast<DWORD>(slash + 1 - path) + strlen(file) + 1 > n) return false;
    strcpy_s(slash + 1, n - static_cast<DWORD>(slash + 1 - path), file);
    return true;
}

bool DataFilePath(char* path, DWORD n, const char* file) { return SiblingPath("SC_OFFLINE_SHIPS_FILE", file, path, n); }
bool ModLogSibling(char* path, DWORD n, const char* file) { return SiblingPath("SC_OFFLINE_MOD_LOG", file, path, n); }
