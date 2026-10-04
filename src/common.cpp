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
    SetConsoleTitleW(L"ChrisWareOffline");
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

const uint8_t* FindCString(const Section& s, const char* str) {
    const size_t n = strlen(str) + 1;
    uint8_t* const end = s.base + s.size;
    for (uint8_t* p = s.base; p + n <= end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, str[0], static_cast<size_t>(end - p) - n + 1));
        if (!p) break;
        if ((p == s.base || p[-1] == 0) && memcmp(p, str, n) == 0) return p;
    }
    return nullptr;
}

int32_t Rel32(const uint8_t* p) { int32_t v; memcpy(&v, p, sizeof(v)); return v; }

bool BytesMatch(const uint8_t* p, const char* pattern) {
    for (const char* c = pattern; *c; ) {
        if (*c == ' ') { ++c; continue; }
        if (c[0] == '?') { ++p; c += (c[1] == '?') ? 2 : 1; continue; }
        char hex[3] = { c[0], c[1], 0 };
        if (*p++ != static_cast<uint8_t>(strtoul(hex, nullptr, 16))) return false;
        c += 2;
    }
    return true;
}

bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t n, DWORD& err) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) { err = GetLastError(); return false; }
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

uint8_t* FindRipLea(const Section& text, uint8_t reg0, uint8_t reg1, uint8_t reg2, const uint8_t* target) {
    uint8_t* const end = text.base + text.size - 7;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, reg0, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] == reg1 && p[2] == reg2 && p + 7 + Rel32(p + 3) == target) return p;
    }
    return nullptr;
}

int FindPattern(const Section& text, const char* pattern, uint8_t** out, int max) {
    uint8_t bytes[96]; bool wild[96]; size_t n = 0;
    for (const char* c = pattern; *c && n < sizeof(bytes); ) {
        if (*c == ' ') { ++c; continue; }
        if (c[0] == '?') { wild[n] = true; bytes[n++] = 0; c += (c[1] == '?') ? 2 : 1; continue; }
        char hex[3] = { c[0], c[1], 0 };
        bytes[n] = static_cast<uint8_t>(strtoul(hex, nullptr, 16)); wild[n++] = false; c += 2;
    }
    int matches = 0;
    uint8_t* const end = text.base + text.size - n;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, bytes[0], static_cast<size_t>(end - p)));
        if (!p) break;
        size_t i = 1;
        while (i < n && (wild[i] || p[i] == bytes[i])) ++i;
        if (i == n) { if (matches < max) out[matches] = p; ++matches; }
    }
    return matches;
}

uint8_t* FindUniquePattern(const Section& text, const char* pattern, int& matches) {
    uint8_t* hit = nullptr;
    matches = FindPattern(text, pattern, &hit, 1);
    return matches == 1 ? hit : nullptr;
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
