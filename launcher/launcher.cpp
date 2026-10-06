// sc-offline.exe - starts Star Citizen with the offline mod and takes the mod back out when the
// game closes.
//
//   sc-offline.exe [play|install|uninstall|status|help] [--game <folder>] [--dry-run] [--skip-eac-check]
//
// play (the default, also what a double-click does):
//   1. find the game's Bin64 folder: --game <path>, else `game =` in sc-offline.ini, else the
//      usual Roberts Space Industries folders on every fixed drive (Wine's C: and Z: included);
//   2. self-checks: which dinput8.dll this is, whether the game updated, leftovers, Easy Anti-Cheat;
//   3. set the SC_OFFLINE_* variables the mod reads, pointing at data\ next to this exe;
//   4. start a helper (this exe again, `--helper`, no window) that copies dinput8.dll into Bin64
//      and data\OfflineDB\default_1.xml into <channel>\user\client\0, backing up anything it
//      replaces. The helper runs as administrator only when the game folder refuses a plain
//      write; the game never does;
//   5. start StarCitizen.exe. The helper waits until this launcher has exited AND no
//      StarCitizen.exe is left, then takes the mod out and restores the backups. Because it is a
//      separate process, closing this window early still cleans up.
// install / uninstall do step 4's copy or its cleanup on their own; status does steps 1-2 only.
// Everything printed also goes to data\launcher.log.
//
// On Linux, sc-offline.sh runs this inside the game's Wine prefix (see that file).
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <share.h>
#include <ctime>
#include <string>
#include <vector>
#include "../src/version.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

using std::wstring;

static const wchar_t* kGameExe = L"StarCitizen.exe";

static bool IsFile(const wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static wstring Trim(const wstring& s) {
    const size_t b = s.find_first_not_of(L" \t\r\n\"");
    if (b == wstring::npos) return L"";
    const size_t e = s.find_last_not_of(L" \t\r\n\"");
    return s.substr(b, e - b + 1);
}

static wstring StripSlashes(wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

static wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return L".";
    wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

static FILE* g_log = nullptr;   // data\launcher.log; the helper appends to the same file

// printf that also writes the line to data\launcher.log.
static void Out(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    va_list b; va_copy(b, a);
    std::vprintf(fmt, a);
    if (g_log) { std::vfprintf(g_log, fmt, b); std::fflush(g_log); }
    va_end(b); va_end(a);
}

static void OpenLog(const wstring& dataDir, bool append, const char* header) {
    g_log = _wfsopen((dataDir + L"\\launcher.log").c_str(), append ? L"a" : L"w", _SH_DENYNO);
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(g_log, "--- %04u-%02u-%02u %02u:%02u:%02u %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond, header);
    std::fflush(g_log);
}

// Only waits for a key when this exe owns its console window (double-clicked), so the window
// doesn't vanish before the player reads the message. From a terminal it returns at once.
static void PauseIfOwnConsole() {
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) > 1) return;
    std::printf("\nPress Enter to close this window.");
    std::getchar();
}

// Exit codes: 0 ok, 1 error, 2 Easy Anti-Cheat active, 3 the game is running.
enum { kExitOk = 0, kExitError = 1, kExitEac = 2, kExitRunning = 3 };

static int FailCode(int code, const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    va_list b; va_copy(b, a);
    std::printf("[!] "); std::vprintf(fmt, a); std::printf("\n");
    if (g_log) { std::fprintf(g_log, "[!] "); std::vfprintf(g_log, fmt, b); std::fprintf(g_log, "\n"); std::fflush(g_log); }
    va_end(b); va_end(a);
    PauseIfOwnConsole();
    return code;
}
#define Fail(...) FailCode(kExitError, __VA_ARGS__)

// sc-offline.ini: `key = value` lines; a line starting with '#' is a comment. Unknown keys are reported, not ignored.
struct Config {
    wstring game, channel = L"LIVE", bootMap = L"PU_All", startShip = L"DRAK_Cutlass_Black", start;
};

static bool ReadConfig(const wstring& path, Config& c) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string bytes;
    char buf[4096]; DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
    CloseHandle(f);
    if (bytes.size() >= 3 && !bytes.compare(0, 3, "\xEF\xBB\xBF")) bytes.erase(0, 3);
    wstring text(bytes.size(), L'\0');
    text.resize(MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), text.data(), (int)text.size()));
    size_t pos = 0; int lineNo = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos); if (nl == wstring::npos) nl = text.size();
        wstring line = text.substr(pos, nl - pos); pos = nl + 1; ++lineNo;
        if (Trim(line).empty() || Trim(line)[0] == L'#') continue;   // whole-line comments only: paths may hold '#'
        const size_t eq = line.find(L'=');
        if (eq == wstring::npos) { Out("[!] sc-offline.ini line %d: expected key = value\n", lineNo); continue; }
        const wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if      (!_wcsicmp(k.c_str(), L"game"))       c.game = v;
        else if (!_wcsicmp(k.c_str(), L"channel"))    { if (!v.empty()) c.channel = v; }
        else if (!_wcsicmp(k.c_str(), L"boot_map"))   c.bootMap = v;
        else if (!_wcsicmp(k.c_str(), L"start_ship")) c.startShip = v;
        else if (!_wcsicmp(k.c_str(), L"start"))      c.start = v;
        else Out("[!] sc-offline.ini line %d: unknown key '%ls'\n", lineNo, k.c_str());
    }
    return true;
}

// Accepts the Bin64 folder itself, the channel folder (LIVE), the StarCitizen folder, or the
// Roberts Space Industries folder. Returns Bin64, or empty when no StarCitizen.exe is there.
static wstring ToBin64(wstring p, const wstring& channel) {
    p = StripSlashes(Trim(p));
    if (p.empty()) return L"";
    const wstring tries[] = {
        p,
        p + L"\\Bin64",
        p + L"\\" + channel + L"\\Bin64",
        p + L"\\StarCitizen\\" + channel + L"\\Bin64",
    };
    for (const wstring& t : tries)
        if (IsFile(t + L"\\" + kGameExe)) return t;
    return L"";
}

static std::vector<wstring> DetectBin64(const wstring& channel) {
    static const wchar_t* kBases[] = {
        L"Program Files\\Roberts Space Industries",   // the RSI Launcher's default, also under Wine
        L"Roberts Space Industries",
        L"Games\\Roberts Space Industries",
        L"Program Files (x86)\\Roberts Space Industries",
    };
    std::vector<wstring> found;
    const DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        const wstring root = wstring(1, wchar_t(L'A' + d)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;   // never spin up a disc or a network share
        for (const wchar_t* b : kBases) {
            const wstring bin = ToBin64(root + b, channel);
            if (!bin.empty()) found.push_back(bin);
        }
    }
    return found;
}

// A failed snapshot counts as "running": the caller must never delete the mod early.
static bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool hit = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !hit; ok = Process32NextW(snap, &pe))
        hit = !_wcsicmp(pe.szExeFile, kGameExe);
    CloseHandle(snap);
    return hit;
}

static bool CanWriteTo(const wstring& dir) {
    const wstring probe = dir + L"\\sc-offline.write-test";
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

// Drops the last path component: ...\LIVE\Bin64 -> ...\LIVE.
static wstring ParentDir(const wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == wstring::npos ? p : p.substr(0, s);
}

static bool ReadAll(const wstring& path, std::string& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[65536]; DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return true;
}

static bool WriteAll(const wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
    CloseHandle(f);
    return ok;
}

static std::string Narrow(const wstring& w) {
    if (w.empty()) return "";
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

static std::string Sha256(const std::string& bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, (PUCHAR)bytes.data(), static_cast<ULONG>(bytes.size()), 0) == 0 &&
        BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
        char two[3];
        for (unsigned char c : digest) { std::snprintf(two, sizeof(two), "%02x", c); hex += two; }
    }
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// The original author's prebuilt dinput8.dll, as committed at the repository root.
static const char* kPrebuiltSha256 = "57e0e5ed2151acc1020fd2ee300944842385f84bb66e216b5ebecb1ee57c10c9";

// "key=value" lines, as in sc-offline.installed.
static std::string Field(const std::string& text, const char* key) {
    const std::string k = std::string(key) + "=";
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
        if (!text.compare(pos, k.size(), k)) {
            std::string v = text.substr(pos + k.size(), nl - pos - k.size());
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        }
        pos = nl + 1;
    }
    return "";
}

// The sc-offline part of a version string: "0.9.0-rc1 / sc-offline 0.2.0-rc4" -> "sc-offline 0.2.0-rc4".
static std::string ScOfflineVersion(const char* full) {
    const char* p = std::strstr(full, "sc-offline ");
    return p ? std::string(p) : std::string(full);
}

struct DllInfo { bool present = false; std::string sha, what, version; bool chrisware = false; };

static DllInfo Identify(const wstring& path) {
    DllInfo d;
    std::string bytes;
    if (!ReadAll(path, bytes) || bytes.empty()) return d;
    d.present = true;
    d.sha = Sha256(bytes);
    d.chrisware = bytes.find("ChrisWareOffline") != std::string::npos;
    const size_t v = bytes.find("/ sc-offline ");
    if (v != std::string::npos) {
        size_t e = v + 2;
        while (e < bytes.size() && bytes[e] >= 0x20 && bytes[e] < 0x7F) ++e;
        d.version = bytes.substr(v + 2, e - v - 2);
        d.what = "source build, " + d.version;
    } else if (d.sha == kPrebuiltSha256) {
        d.what = "the original author's prebuilt DLL";
    } else {
        d.what = d.chrisware ? "unknown ChrisWareOffline build" : "not ChrisWareOffline";
    }
    return d;
}

// --- what lives in the game folder ------------------------------------------------------------

struct GamePaths {
    wstring bin, dll, dllBackup, marker, userDir, xml, xmlBackup;
    explicit GamePaths(const wstring& bin64) : bin(bin64) {
        dll = bin + L"\\dinput8.dll";
        dllBackup = dll + L".sc-offline-backup";
        marker = bin + L"\\sc-offline.installed";
        userDir = ParentDir(bin) + L"\\user\\client\\0";
        xml = userDir + L"\\default_1.xml";
        xmlBackup = xml + L".sc-offline-backup";
    }
};

// Prints the step; returns true when it should really happen (false on --dry-run).
static bool Step(bool dry, const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    Out("%s %s\n", dry ? "[dry-run] would" : "[+]", buf);
    return !dry;
}

// Copies the mod in, backing up what it replaces, and writes the marker. 0 ok, 2 failed.
// After a crash the marker is still there: the backups were made then, so they are kept as they are.
static int PutMod(const wstring& here, const GamePaths& g, const char* mode, bool dry) {
    const wstring dllSrc = here + L"\\dinput8.dll";
    const wstring xmlSrc = here + L"\\data\\OfflineDB\\default_1.xml";
    std::string prev;
    const bool leftover = ReadAll(g.marker, prev);
    std::string xmlState = leftover ? Field(prev, "xml") : "";

    if (IsFile(xmlSrc)) {
        if (xmlState.empty()) {
            if (IsFile(g.xmlBackup)) xmlState = "backup";
            else if (IsFile(g.xml)) {
                xmlState = "backup";
                if (Step(dry, "back up your default_1.xml to %ls", g.xmlBackup.c_str()) &&
                    !CopyFileW(g.xml.c_str(), g.xmlBackup.c_str(), TRUE)) {
                    Out("[!] couldn't back up %ls (error %lu)\n", g.xml.c_str(), GetLastError());
                    return 2;
                }
            } else xmlState = "none";
        }
        if (Step(dry, "copy data\\OfflineDB\\default_1.xml to %ls", g.xml.c_str())) {
            SHCreateDirectoryExW(nullptr, g.userDir.c_str(), nullptr);   // ERROR_ALREADY_EXISTS is fine
            if (!CopyFileW(xmlSrc.c_str(), g.xml.c_str(), FALSE)) {
                Out("[!] couldn't copy default_1.xml to %ls (error %lu)\n", g.xml.c_str(), GetLastError());
                return 2;
            }
        }
    } else {
        Out("[i] data\\OfflineDB\\default_1.xml is missing; the game's own player data is used\n");
        if (xmlState.empty()) xmlState = "skip";
    }

    if (IsFile(g.dll) && !leftover && !Identify(g.dll).chrisware) {
        if (IsFile(g.dllBackup))
            return Out("[!] %ls belongs to another mod and %ls already exists; move one of them away first\n",
                       g.dll.c_str(), g.dllBackup.c_str()), 2;
        if (Step(dry, "set aside the other mod's dinput8.dll as %ls", g.dllBackup.c_str()) &&
            !MoveFileExW(g.dll.c_str(), g.dllBackup.c_str(), 0)) {
            Out("[!] couldn't rename %ls (error %lu)\n", g.dll.c_str(), GetLastError());
            return 2;
        }
    }
    if (Step(dry, "copy dinput8.dll to %ls", g.dll.c_str()) && !CopyFileW(dllSrc.c_str(), g.dll.c_str(), FALSE)) {
        Out("[!] couldn't copy dinput8.dll to %ls (error %lu)\n", g.dll.c_str(), GetLastError());
        return 2;
    }

    SYSTEMTIME t; GetLocalTime(&t);
    char when[32];
    std::snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::string bytes;
    const DllInfo mod = Identify(dllSrc);
    const std::string marker = std::string("version=") + (mod.version.empty() ? mod.what : mod.version) +
                               "\nsha256=" + mod.sha + "\nmode=" + mode + "\ntime=" + when + "\nxml=" + xmlState + "\n";
    if (Step(dry, "write %ls", g.marker.c_str()) && !WriteAll(g.marker, marker))
        Out("[!] couldn't write %ls (error %lu); uninstall still works, but can't tell whether default_1.xml was yours\n",
            g.marker.c_str(), GetLastError());
    return 0;
}

// Takes the mod out and puts back what PutMod replaced. 0 ok, 3 dinput8.dll couldn't be removed.
static int TakeMod(const GamePaths& g, bool dry) {
    std::string marker;
    const bool haveMarker = ReadAll(g.marker, marker);
    const std::string xmlState = haveMarker ? Field(marker, "xml") : "";
    int rc = 0;

    if (IsFile(g.dll)) {
        if (haveMarker || Identify(g.dll).chrisware) {
            if (Step(dry, "delete %ls", g.dll.c_str())) {
                bool gone = false;
                for (int tries = 0; tries < 30 && !gone; ++tries) {
                    gone = DeleteFileW(g.dll.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
                    if (!gone) Sleep(1000);
                }
                if (!gone) { Out("[!] couldn't delete %ls (error %lu)\n", g.dll.c_str(), GetLastError()); rc = 3; }
            }
        } else {
            Out("[i] %ls isn't sc-offline's; left alone\n", g.dll.c_str());
        }
    }
    if (rc == 0 && IsFile(g.dllBackup) && (dry || !IsFile(g.dll)) &&
        Step(dry, "put the other mod's dinput8.dll back") && !MoveFileExW(g.dllBackup.c_str(), g.dll.c_str(), 0))
        Out("[!] couldn't restore %ls (error %lu)\n", g.dllBackup.c_str(), GetLastError());

    if (IsFile(g.xmlBackup) && (xmlState == "backup" || !haveMarker)) {
        if (Step(dry, "restore your default_1.xml from %ls", g.xmlBackup.c_str()) &&
            !MoveFileExW(g.xmlBackup.c_str(), g.xml.c_str(), MOVEFILE_REPLACE_EXISTING))
            Out("[!] couldn't restore %ls (error %lu)\n", g.xmlBackup.c_str(), GetLastError());
    } else if (xmlState == "none" && IsFile(g.xml)) {
        if (Step(dry, "delete %ls (there was none before sc-offline)", g.xml.c_str()) && !DeleteFileW(g.xml.c_str()))
            Out("[!] couldn't delete %ls (error %lu)\n", g.xml.c_str(), GetLastError());
    } else if (!haveMarker && IsFile(g.xml)) {
        Out("[i] left %ls in place: an older sc-offline may have put it there, but there's no record of it\n", g.xml.c_str());
    }

    if (haveMarker && rc == 0 && Step(dry, "delete %ls", g.marker.c_str()) && !DeleteFileW(g.marker.c_str()))
        Out("[!] couldn't delete %ls (error %lu)\n", g.marker.c_str(), GetLastError());
    return rc;
}

// --helper <play|install|uninstall> <Bin64> <launcher pid> <event name>
// The part that writes to the game folder; runs elevated when that folder needs it.
// play: put the mod in, signal the event, wait for the launcher and every StarCitizen.exe to
// exit, take it out. install: put in. uninstall: take out.
// Exit codes: 0 ok, 2 copy failed, 3 removal failed, 4 bad args.
static int Helper(const wstring& op, const wstring& bin, DWORD parentPid, const wstring& eventName) {
    const wstring here = ExeDir();
    OpenLog(here + L"\\data", true, Narrow(L"helper " + op).c_str());
    const GamePaths g(bin);
    if (!_wcsicmp(op.c_str(), L"uninstall")) return TakeMod(g, false);
    const bool play = !_wcsicmp(op.c_str(), L"play");
    if (!play && _wcsicmp(op.c_str(), L"install")) return 4;
    HANDLE parent = play ? OpenProcess(SYNCHRONIZE, FALSE, parentPid) : nullptr;
    if (play && !parent) return 4;
    const int rc = PutMod(here, g, play ? "play" : "install", false);
    if (rc) { if (parent) CloseHandle(parent); return rc; }
    if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str())) { SetEvent(ready); CloseHandle(ready); }
    if (!play) return 0;

    WaitForSingleObject(parent, INFINITE);
    CloseHandle(parent);
    while (GameRunning()) Sleep(2000);
    return TakeMod(g, false);
}

// Starts the helper: hidden and unelevated when the game folder is writable, otherwise elevated
// (one UAC prompt). Returns its process handle, or null.
static HANDLE StartHelper(const wchar_t* op, const GamePaths& g, const wstring& eventName) {
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    const wstring args = L"--helper " + wstring(op) + L" \"" + g.bin + L"\" " + std::to_wstring(GetCurrentProcessId()) +
                         L" " + eventName;
    if (CanWriteTo(g.bin) && CanWriteTo(ParentDir(g.bin))) {
        wstring cmd = L"\"" + wstring(self) + L"\" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(self, cmd.data(), nullptr, nullptr, FALSE,
                            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
            return nullptr;
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }
    Out("[i] the game folder needs administrator rights for the mod; Windows will ask once.\n"
        "    Only the copy/remove helper runs as administrator, not the game.\n");
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return nullptr;
    return sei.hProcess;
}

// Ctrl+C would kill only this window; the helper still cleans up, but say so instead of dying.
static BOOL WINAPI OnCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        std::printf("\n[i] Ctrl+C ignored: close the game instead. The mod is removed when it exits.\n");
        return TRUE;
    }
    return FALSE;   // window closed / logoff: let it go; the helper removes the mod
}

static void SetVar(const wchar_t* name, const wstring& v) {
    SetEnvironmentVariableW(name, v.empty() ? nullptr : v.c_str());   // empty = unset, like the .bat's `set X=`
}

static bool OnWine() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

static wstring EnvOr(const wchar_t* name, const wchar_t* fallback) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return n && n < ARRAYSIZE(buf) ? wstring(buf, n) : wstring(fallback);
}

// --- self-checks -------------------------------------------------------------------------------

// "Version" and "RequestedP4ChangeNum" from <channel>\build_manifest.id, else StarCitizen.exe's size and date.
static std::string JsonString(const std::string& text, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = text.find(k);
    if (p == std::string::npos) return "";
    p = text.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = text.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || text[p] != '"') return "";
    const size_t e = text.find('"', p + 1);
    return e == std::string::npos ? "" : text.substr(p + 1, e - p - 1);
}

static std::string GameBuild(const wstring& bin) {
    std::string manifest;
    if (ReadAll(ParentDir(bin) + L"\\build_manifest.id", manifest)) {
        const std::string ver = JsonString(manifest, "Version"), cl = JsonString(manifest, "RequestedP4ChangeNum");
        if (!ver.empty() || !cl.empty()) return ver + (cl.empty() ? "" : " (CL " + cl + ")");
    }
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW((bin + L"\\" + kGameExe).c_str(), GetFileExInfoStandard, &a)) return "";
    SYSTEMTIME t{}; FileTimeToSystemTime(&a.ftLastWriteTime, &t);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "StarCitizen.exe %llu bytes, %04u-%02u-%02u %02u:%02u",
                  (unsigned long long)a.nFileSizeHigh << 32 | a.nFileSizeLow, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return buf;
}

enum class Eac { Active, Disabled, NotInstalled };

static Eac EacState() {
    const wstring exe = EnvOr(L"ProgramFiles(x86)", L"C:\\Program Files (x86)") + L"\\EasyAntiCheat_EOS\\EasyAntiCheat_EOS.exe";
    if (IsFile(exe)) return Eac::Active;
    return IsFile(exe + L".bak") ? Eac::Disabled : Eac::NotInstalled;
}

static bool HostsBlocksEac() {
    std::string hosts;
    if (!ReadAll(EnvOr(L"SystemRoot", L"C:\\Windows") + L"\\System32\\drivers\\etc\\hosts", hosts)) return false;
    for (size_t pos = 0; pos < hosts.size();) {
        size_t nl = hosts.find('\n', pos); if (nl == std::string::npos) nl = hosts.size();
        std::string line = hosts.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        if (line.find("modules-cdn.eac-prod.on.epicgames.com") != std::string::npos) return true;
    }
    return false;
}

struct CheckResult { bool eacActive = false; std::string gameBuild; };

static CheckResult SelfChecks(const wstring& here, const Config& cfg, const GamePaths& g) {
    CheckResult r;
    // 1. The DLL this launcher would copy in.
    const DllInfo mod = Identify(here + L"\\dinput8.dll");
    if (!mod.present) Out("Mod DLL:  missing (dinput8.dll next to sc-offline.exe)\n");
    else {
        Out("Mod DLL:  %s\n          sha256 %s\n", mod.what.c_str(), mod.sha.c_str());
        if (mod.sha == kPrebuiltSha256 && _wcsicmp(cfg.bootMap.c_str(), L"PU"))
            Out("[!] the prebuilt DLL only knows boot_map = PU; set that in sc-offline.ini (now %ls)\n", cfg.bootMap.c_str());
        if (!mod.version.empty() && mod.version != ScOfflineVersion(CWO_VERSION))
            Out("[i] this launcher is %s; the DLL is %s\n", ScOfflineVersion(CWO_VERSION).c_str(), mod.version.c_str());
    }

    // 2. The game build, against the one recorded after the last play.
    r.gameBuild = GameBuild(g.bin);
    std::string last;
    ReadAll(here + L"\\data\\game-build.txt", last);
    while (!last.empty() && (last.back() == '\n' || last.back() == '\r')) last.pop_back();
    Out("Game:     %s\n", r.gameBuild.empty() ? "(version unknown)" : r.gameBuild.c_str());
    if (!last.empty() && !r.gameBuild.empty() && last != r.gameBuild)
        Out("[!] the game updated since you last played (was %s).\n"
            "    If the mod misbehaves, check for a newer sc-offline release.\n", last.c_str());

    // 3. What is in the game folder now.
    std::string marker;
    const bool haveMarker = ReadAll(g.marker, marker);
    const DllInfo inGame = Identify(g.dll);
    if (!inGame.present) Out("Installed: no (Bin64 has no dinput8.dll)\n");
    else if (haveMarker || inGame.chrisware)
        Out("Installed: yes, %s%s%s\n", inGame.what.c_str(), haveMarker ? ", since " : "",
            haveMarker ? Field(marker, "time").c_str() : "");
    else Out("Installed: no; Bin64 has another mod's dinput8.dll (it is set aside while you play)\n");
    if ((haveMarker || inGame.chrisware) && inGame.present && !GameRunning())
        Out("[!] the mod is still in the game folder (a crash, or `install`).\n"
            "    Run `sc-offline.exe uninstall` before going online.\n");

    // 4. Easy Anti-Cheat.
    const Eac eac = EacState();
    Out("EAC:      %s\n", eac == Eac::Active ? "ACTIVE" : eac == Eac::Disabled ? "disabled (EasyAntiCheat_EOS.exe.bak)" : "not installed");
    if (OnWine()) Out("Hosts:    checked by sc-offline.sh against /etc/hosts\n");
    else Out("Hosts:    %s\n", HostsBlocksEac() ? "EAC download server blocked" : "EAC download server NOT blocked");
    if (eac == Eac::Active) {
        r.eacActive = true;
        Out("[!] Easy Anti-Cheat is active; the mod can't run with it. In PowerShell as administrator:\n"
            "      ren \"C:\\Program Files (x86)\\EasyAntiCheat_EOS\\EasyAntiCheat_EOS.exe\" EasyAntiCheat_EOS.exe.bak\n"
            "    and block modules-cdn.eac-prod.on.epicgames.com in your hosts file (README, Setup).\n");
    } else if (!OnWine() && !HostsBlocksEac()) {
        Out("[i] add `127.0.0.1 modules-cdn.eac-prod.on.epicgames.com` to your hosts file so the RSI Launcher\n"
            "    doesn't download Easy Anti-Cheat again (README, Setup).\n");
    }
    if (OnWine()) {
        wchar_t ov[512];
        const DWORD n = GetEnvironmentVariableW(L"WINEDLLOVERRIDES", ov, ARRAYSIZE(ov));
        Out("Wine:     yes, WINEDLLOVERRIDES=%ls\n", n && n < ARRAYSIZE(ov) ? ov : L"(not set - dinput8 needs n,b; use sc-offline.sh)");
    }
    return r;
}

static const char* kUsage =
    "usage: sc-offline.exe [command] [--game <folder>] [--dry-run] [--skip-eac-check]\n"
    "\n"
    "  play       (default) copy the mod in, start the game, take the mod out when it closes\n"
    "  install    copy the mod in and leave it there; run `uninstall` before going online\n"
    "  uninstall  take the mod out and restore anything it replaced\n"
    "  status     check the setup and report; changes nothing\n"
    "  help       show this\n"
    "\n"
    "  --game <folder>   your StarCitizen, LIVE or Bin64 folder (else sc-offline.ini, else searched)\n"
    "  --dry-run         print every step, change nothing, start nothing\n"
    "  --skip-eac-check  don't stop when Easy Anti-Cheat looks active\n"
    "\n"
    "exit codes: 0 ok, 1 error, 2 Easy Anti-Cheat active, 3 the game is running\n";

int wmain(int argc, wchar_t** argv) {
    if (argc == 6 && !_wcsicmp(argv[1], L"--helper"))
        return Helper(argv[2], argv[3], wcstoul(argv[4], nullptr, 10), argv[5]);

    wstring command = L"play", gameArg;
    bool dry = false, skipEac = false, sawCommand = false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!_wcsicmp(a, L"--game")) {
            if (i + 1 >= argc) return Fail("--game needs a folder: sc-offline.exe --game \"D:\\Games\\StarCitizen\"");
            gameArg = argv[++i];
        } else if (!_wcsicmp(a, L"--dry-run")) dry = true;
        else if (!_wcsicmp(a, L"--skip-eac-check")) skipEac = true;
        else if (!sawCommand && (!_wcsicmp(a, L"play") || !_wcsicmp(a, L"install") || !_wcsicmp(a, L"uninstall") ||
                                 !_wcsicmp(a, L"status") || !_wcsicmp(a, L"help"))) {
            command = a; sawCommand = true;
        } else if (!_wcsicmp(a, L"--help") || !_wcsicmp(a, L"-h") || !_wcsicmp(a, L"/?")) {
            command = L"help"; sawCommand = true;
        } else {
            std::printf("%s", kUsage);
            return Fail("unknown argument '%ls'", a);
        }
    }
    for (wchar_t& c : command) c = towlower(c);
    if (command == L"help") { std::printf("sc-offline launcher (ChrisWareOffline %s)\n\n%s", CWO_VERSION, kUsage); return kExitOk; }

    const wstring here = ExeDir();
    const wstring data = here + L"\\data";
    std::string header = "sc-offline.exe";
    for (int i = 1; i < argc; ++i) header += " " + Narrow(argv[i]);
    OpenLog(data, false, header.c_str());
    Out("sc-offline launcher (ChrisWareOffline %s)%s\n\n", CWO_VERSION, dry ? " - dry run, nothing is changed" : "");

    Config cfg;
    if (!ReadConfig(here + L"\\sc-offline.ini", cfg)) Out("[i] no sc-offline.ini next to this exe; using defaults\n");

    // 1. Find the game.
    wstring bin;
    if (!gameArg.empty()) {
        bin = ToBin64(gameArg, cfg.channel);
        if (bin.empty()) return Fail("no %ls under --game %ls (channel %ls)", kGameExe, gameArg.c_str(), cfg.channel.c_str());
    } else if (!cfg.game.empty()) {
        bin = ToBin64(cfg.game, cfg.channel);
        if (bin.empty()) return Fail("no %ls under game = %ls in sc-offline.ini (channel %ls)", kGameExe, cfg.game.c_str(), cfg.channel.c_str());
    } else {
        const std::vector<wstring> found = DetectBin64(cfg.channel);
        if (found.empty())
            return Fail("couldn't find Star Citizen (%ls). Open sc-offline.ini and set\n"
                        "    game = <your StarCitizen folder>", cfg.channel.c_str());
        bin = found.front();
        for (size_t i = 1; i < found.size(); ++i)
            Out("[i] also found %ls (set game = in sc-offline.ini to use it)\n", found[i].c_str());
    }
    Out("Folder:   %ls\n", bin.c_str());
    const GamePaths g(bin);

    // 2. Self-checks.
    const CheckResult checks = SelfChecks(here, cfg, g);
    Out("\n");
    if (command == L"status") return kExitOk;

    const bool running = GameRunning();
    if (command == L"uninstall") {
        if (running) return FailCode(kExitRunning, "%ls is running. Close it first.", kGameExe);
        if (dry) return TakeMod(g, true) ? kExitError : kExitOk;
    } else {
        if (!IsFile(here + L"\\dinput8.dll"))
            return Fail("dinput8.dll is missing next to sc-offline.exe.\n    Extract the whole zip into one folder and run this from there.");
        if (running) return FailCode(kExitRunning, "%ls is already running. Close it first.", kGameExe);
        if (checks.eacActive && !skipEac)
            return FailCode(kExitEac, "stopped: Easy Anti-Cheat is active (see above). --skip-eac-check overrides this.");
    }

    // 3. What the mod reads (same names and values launch_offline.bat used).
    const bool play = command == L"play";
    if (play) {
        SetVar(L"SC_OFFLINE_BOOT_MAP", cfg.bootMap);
        SetVar(L"SC_OFFLINE_MOD_LOG", data + L"\\mod.log");
        SetVar(L"SC_OFFLINE_SPAWN_FILE", data + L"\\spawn.txt");
        SetVar(L"SC_OFFLINE_SHIPS_FILE", data + L"\\ships.txt");
        SetVar(L"SC_OFFLINE_START", cfg.start);
        SetVar(L"SC_OFFLINE_START_SHIP", cfg.startShip);
        SetVar(L"SC_USER", g.userDir);
        if (dry)
            Out("[dry-run] would set SC_OFFLINE_BOOT_MAP=%ls SC_OFFLINE_START_SHIP=%ls SC_OFFLINE_START=%ls\n"
                "          SC_OFFLINE_MOD_LOG, _SPAWN_FILE, _SHIPS_FILE under %ls; SC_USER=%ls\n",
                cfg.bootMap.c_str(), cfg.startShip.c_str(), cfg.start.c_str(), data.c_str(), g.userDir.c_str());
    }

    // 4. Change the game folder, through the helper (see the top of this file).
    if (dry) {
        if (PutMod(here, g, play ? "play" : "install", true)) return kExitError;
        if (play) {
            Out("[dry-run] would start %ls\\%ls\n", bin.c_str(), kGameExe);
            Out("[dry-run] after the game closes: take the mod out and put back what was set aside\n");
        }
        return kExitOk;
    }
    const wstring eventName = L"Local\\sc-offline-ready-" + std::to_wstring(GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    HANDLE helper = ready ? StartHelper(command.c_str(), g, eventName) : nullptr;
    if (!helper) return Fail("couldn't start the mod helper (error %lu); administrator rights refused?", GetLastError());

    if (!play) {   // install / uninstall: the helper does the whole job
        WaitForSingleObject(helper, INFINITE);
        DWORD code = 1; GetExitCodeProcess(helper, &code);
        CloseHandle(helper); CloseHandle(ready);
        if (code) return Fail("the helper failed (exit %lu); see data\\launcher.log", code);
        if (command == L"install")
            Out("Mod:      installed. Start the game your usual way; run `sc-offline.exe uninstall` before going online.\n");
        else Out("Mod:      removed.\n");
        return kExitOk;
    }

    HANDLE waitOn[2] = { ready, helper };
    // No timeout: a UAC prompt can sit there as long as the player likes. The helper either
    // signals (copied) or exits (copy failed; it undid nothing because nothing was left half-done).
    if (WaitForMultipleObjects(2, waitOn, FALSE, INFINITE) != WAIT_OBJECT_0) {
        DWORD code = 0; GetExitCodeProcess(helper, &code);
        return Fail("couldn't copy the mod into %ls (helper exit %lu); see data\\launcher.log", bin.c_str(), code);
    }
    CloseHandle(ready);
    Out("Mod:      copied into Bin64\n");
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    // 5. Play. The game runs with this window's rights, never the helper's.
    const wstring exe = bin + L"\\" + kGameExe;
    wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    int rc = kExitOk;
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, bin.c_str(), &si, &pi)) {
        Out("[!] couldn't start %ls (error %lu)\n", exe.c_str(), GetLastError());
        rc = kExitError;
    } else {
        if (!checks.gameBuild.empty()) WriteAll(data + L"\\game-build.txt", checks.gameBuild + "\n");
        Out("\nPlaying. When the game closes, the mod is removed from Bin64.\n");
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        while (GameRunning()) Sleep(2000);   // the game can hand over to a second StarCitizen.exe
    }
    CloseHandle(helper);
    Out("Mod:      the helper removes it from Bin64 as this window closes.\n");
    if (rc) PauseIfOwnConsole();
    return rc;
}
