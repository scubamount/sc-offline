// sc-offline.exe - starts Star Citizen with the offline mod and takes the mod back out when the
// game closes.
//
//   sc-offline.exe [play|install|uninstall|status|help] [--game <folder>] [--dry-run] [--skip-eac-check]
//
// play (the default, also what a double-click does):
//   1. find the game's Bin64 folder: --game <path>, else `game =` in sc-offline.ini, else the
//      folder remembered from the last run, what the RSI Launcher recorded, the usual folders on
//      every fixed drive, a bounded drive search, and finally a folder picker (see DetectBin64);
//   2. self-checks: which dinput8.dll this is, whether the game updated, leftovers, Easy Anti-Cheat;
//   3. set the SC_OFFLINE_* variables the mod reads, pointing at data\ next to this exe;
//   4. start a helper (this exe again, `--helper`, no window) that copies dinput8.dll into Bin64
//      and data\OfflineDB\default_1.xml into <channel>\user\client\0, backing up anything it
//      replaces. The helper runs as administrator only when the game folder refuses a plain
//      write; the game never does;
//      Before that, on play, the helper blocks StarCitizen.exe in Windows Firewall, adds the EAC
//      hosts line and renames EasyAntiCheat_EOS.exe (each switchable in sc-offline.ini), recording
//      every change in %ProgramData%\sc-offline\pc-changes.txt; the helper then also runs elevated;
//   5. start StarCitizen.exe. The helper waits until this launcher has exited AND no
//      StarCitizen.exe is left, then takes the mod out, restores the backups and undoes the recorded
//      PC changes. Because it is a
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
#include <shobjidl.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <commctrl.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <share.h>
#include <ctime>
#include <algorithm>
#include <string>
#include <vector>
#include "../src/version.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
// Visual styles for the window (issue #20); MSVC embeds this, MinGW builds get classic controls.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

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
// Set by the launcher window (issue #20) for the command it runs: output goes to a pipe the window
// shows, and [y/N] questions are answered with its Yes/No buttons through stdin.
static bool GuiChild() { return GetEnvironmentVariableW(L"SC_OFFLINE_GUI", nullptr, 0) > 0; }

static void PauseIfOwnConsole() {
    if (GuiChild()) return;
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) > 1) return;
    std::printf("\nPress Enter to close this window.");
    std::getchar();
}

// Exit codes: 0 ok, 1 error, 2 Easy Anti-Cheat active, 3 the game is running.
// kExitRestart: only for the window; an update was applied and the window should restart itself.
enum { kExitOk = 0, kExitError = 1, kExitEac = 2, kExitRunning = 3, kExitRestart = 10 };

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
    // What the helper changes on the PC while you play, and undoes when the game closes.
    bool firewall = true, eacHosts = true, eacRename = true;
    // After the game closes: offer to delete the logs the game wrote during this session.
    bool cleanLogs = true;
    // Check GitHub for a newer sc-offline release on play/status (issue #14).
    bool checkUpdates = true;
    // After a crash, offer a redacted log bundle and a prefilled bug form (issue #18).
    bool crashReports = true;
};

static bool ParseOnOff(const wstring& v, bool& out) {
    if (!_wcsicmp(v.c_str(), L"1") || !_wcsicmp(v.c_str(), L"on") || !_wcsicmp(v.c_str(), L"yes") || !_wcsicmp(v.c_str(), L"true")) { out = true; return true; }
    if (!_wcsicmp(v.c_str(), L"0") || !_wcsicmp(v.c_str(), L"off") || !_wcsicmp(v.c_str(), L"no") || !_wcsicmp(v.c_str(), L"false")) { out = false; return true; }
    return false;
}

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
        else if (!_wcsicmp(k.c_str(), L"block_network") || !_wcsicmp(k.c_str(), L"eac_hosts") || !_wcsicmp(k.c_str(), L"eac_rename")) {
            bool& b = !_wcsicmp(k.c_str(), L"block_network") ? c.firewall : !_wcsicmp(k.c_str(), L"eac_hosts") ? c.eacHosts : c.eacRename;
            if (!ParseOnOff(v, b)) Out("[!] sc-offline.ini line %d: %ls must be on or off\n", lineNo, k.c_str());
        }
        else if (!_wcsicmp(k.c_str(), L"crash_reports")) {
            if (!ParseOnOff(v, c.crashReports)) Out("[!] sc-offline.ini line %d: crash_reports must be on or off\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"check_updates")) {
            if (!ParseOnOff(v, c.checkUpdates)) Out("[!] sc-offline.ini line %d: check_updates must be on or off\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"clean_logs")) {
            if (!_wcsicmp(v.c_str(), L"ask")) c.cleanLogs = true;
            else if (!ParseOnOff(v, c.cleanLogs)) Out("[!] sc-offline.ini line %d: clean_logs must be ask or off\n", lineNo);
        }
        else Out("[!] sc-offline.ini line %d: unknown key '%ls'\n", lineNo, k.c_str());
    }
    return true;
}

// Accepts the Bin64 folder itself, the channel folder (LIVE), the StarCitizen folder, the
// Roberts Space Industries folder, or a library folder holding "Star Citizen\StarCitizen". Returns Bin64, or empty when no StarCitizen.exe is there.
static wstring ToBin64(wstring p, const wstring& channel) {
    p = StripSlashes(Trim(p));
    if (p.empty()) return L"";
    const wstring tries[] = {
        p,
        p + L"\\Bin64",
        p + L"\\" + channel + L"\\Bin64",
        p + L"\\StarCitizen\\" + channel + L"\\Bin64",
        p + L"\\Star Citizen\\" + channel + L"\\Bin64",
        p + L"\\Star Citizen\\StarCitizen\\" + channel + L"\\Bin64",
        p + L"\\Roberts Space Industries\\StarCitizen\\" + channel + L"\\Bin64",
    };
    for (const wstring& t : tries)
        if (IsFile(t + L"\\" + kGameExe)) return t;
    return L"";
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

// --- Self-update (issue #14) ---------------------------------------------------------------
// Ask GitHub for the latest full release; if it is newer, offer to download the release zip,
// check it against the SHA-256 digest GitHub publishes for the asset, and swap the files in.
// The swap is journaled in data\update\applied.txt before each step, so a crash rolls back on
// the next run. Files the player owns are never replaced: sc-offline.ini is kept (new keys are
// appended to it, commented out), and anything in data\ that the release doesn't ship (wallet,
// saved places, bookmarks, logs) is left alone because only shipped paths are written.

static const wchar_t* kReleasesApi = L"https://api.github.com/repos/scubamount/sc-offline/releases/latest";

static bool HttpGet(const wstring& url, std::string& body, DWORD timeoutMs, size_t cap) {
    body.clear();
    URL_COMPONENTS u{}; u.dwStructSize = sizeof(u);
    wchar_t host[256] = {}, path[2048] = {};
    u.lpszHostName = host; u.dwHostNameLength = ARRAYSIZE(host);
    u.lpszUrlPath = path; u.dwUrlPathLength = ARRAYSIZE(path);
    wchar_t extra[2048] = {}; u.lpszExtraInfo = extra; u.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS) return false;
    const wstring agent = L"sc-offline/" + wstring(SCO_VERSION, SCO_VERSION + strlen(SCO_VERSION));
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", (wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            ok = true;
            char buf[65536];
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(r, buf, sizeof(buf), &got)) { ok = false; break; }
                if (!got) break;
                body.append(buf, got);
                if (body.size() > cap) { ok = false; break; }
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// The value of `key` directly inside the JSON object that starts at j[from] ('{'): a string is
// unescaped, any other value (true, 12, null) is returned as its raw token. Nested objects and
// arrays are skipped, so an asset's uploader can't shadow the asset's own fields.
static bool JsonKey(const std::string& j, size_t from, const char* key, std::string& out, size_t* valueAt = nullptr) {
    int depth = 0;
    for (size_t i = from; i < j.size(); ++i) {
        const char ch = j[i];
        if (ch == '"') {
            size_t e = i + 1; std::string str;
            for (; e < j.size() && j[e] != '"'; ++e) {
                if (j[e] == '\\' && e + 1 < j.size()) { ++e; str += j[e] == 'n' ? '\n' : j[e]; } else str += j[e];
            }
            size_t k = e + 1; while (k < j.size() && isspace((unsigned char)j[k])) ++k;
            if (depth == 1 && k < j.size() && j[k] == ':' && str == key) {
                ++k; while (k < j.size() && isspace((unsigned char)j[k])) ++k;
                if (valueAt) *valueAt = k;
                if (k < j.size() && j[k] == '"') {
                    out.clear();
                    for (size_t v = k + 1; v < j.size() && j[v] != '"'; ++v) {
                        if (j[v] == '\\' && v + 1 < j.size()) ++v;
                        out += j[v];
                    }
                } else {
                    size_t v = k; while (v < j.size() && j[v] != ',' && j[v] != '}' && !isspace((unsigned char)j[v])) ++v;
                    out = j.substr(k, v - k);
                }
                return true;
            }
            i = e;
        } else if (ch == '{' || ch == '[') ++depth;
        else if (ch == '}' || ch == ']') { if (--depth <= 0) return false; }
    }
    return false;
}

// "v0.4.2" / "0.4.2" -> {0,4,2,0}. False on anything that isn't 1-4 dot-separated numbers
// (a "-rc" suffix included), so an unreadable tag is never offered.
static bool ParseVersion(std::string v, int out[4]) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    int n = 0; out[0] = out[1] = out[2] = out[3] = 0;
    size_t i = 0;
    while (i < v.size()) {
        if (n == 4 || !isdigit((unsigned char)v[i])) return false;
        long x = 0;
        while (i < v.size() && isdigit((unsigned char)v[i])) { x = x * 10 + (v[i] - '0'); if (x > 100000) return false; ++i; }
        out[n++] = (int)x;
        if (i < v.size()) { if (v[i] != '.' || i + 1 == v.size()) return false; ++i; }
    }
    return n > 0;
}

static bool IsNewerVersion(const std::string& latest, const std::string& current) {
    int a[4], b[4];
    if (!ParseVersion(latest, a) || !ParseVersion(current, b)) return false;
    for (int i = 0; i < 4; ++i) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

struct UpdateInfo { std::string tag, zipName, zipUrl, sha256; };

// What GitHub says the latest release is. Empty tag when there's nothing newer to offer.
static UpdateInfo CheckLatest(DWORD timeoutMs, std::string* why) {
    UpdateInfo u;
    std::string body;
    if (!HttpGet(kReleasesApi, body, timeoutMs, 1u << 20)) { if (why) *why = "couldn't reach GitHub"; return u; }
    const size_t top = body.find('{');
    std::string tag, pre, draft;
    if (top == std::string::npos || !JsonKey(body, top, "tag_name", tag)) { if (why) *why = "unexpected reply from GitHub"; return u; }
    JsonKey(body, top, "prerelease", pre); JsonKey(body, top, "draft", draft);
    if (pre == "true" || draft == "true") { if (why) *why = "latest release is a pre-release"; return u; }
    if (!IsNewerVersion(tag, SCO_VERSION)) { if (why) *why = "up to date (latest " + tag + ")"; return u; }
    const std::string want = "sc-offline-" + tag + ".zip";
    size_t at = 0;
    if (!JsonKey(body, top, "assets", pre, &at) || at >= body.size() || body[at] != '[') { if (why) *why = "release has no assets"; return u; }
    // Walk the assets array object by object.
    for (size_t i = at + 1; i < body.size();) {
        while (i < body.size() && (isspace((unsigned char)body[i]) || body[i] == ',')) ++i;
        if (i >= body.size() || body[i] != '{') break;
        std::string name, url, digest;
        JsonKey(body, i, "name", name);
        if (name == want && JsonKey(body, i, "browser_download_url", url) && JsonKey(body, i, "digest", digest) &&
            !digest.compare(0, 7, "sha256:") && digest.size() == 7 + 64) {
            u.tag = tag; u.zipName = name; u.zipUrl = url; u.sha256 = digest.substr(7);
            for (char& c : u.sha256) c = (char)tolower((unsigned char)c);
            return u;
        }
        // skip this object
        int depth = 0; bool str = false;
        for (; i < body.size(); ++i) {
            const char c = body[i];
            if (str) { if (c == '\\') ++i; else if (c == '"') str = false; continue; }
            if (c == '"') str = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) { ++i; break; } }
        }
    }
    if (why) *why = tag + " has no " + want + " with a SHA-256 digest";
    return u;
}

static std::vector<wstring> FilesUnder(const wstring& root, const wstring& rel = L"") {
    std::vector<wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + (rel.empty() ? L"" : L"\\" + rel) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const wstring r = rel.empty() ? wstring(fd.cFileName) : rel + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { auto sub = FilesUnder(root, r); out.insert(out.end(), sub.begin(), sub.end()); }
        else out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static void RemoveTree(const wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const wstring p = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static bool RunWait(const wstring& exe, wstring cmd, const wstring& cwd, DWORD& code) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

static wstring Wide(const std::string& s) {
    wstring w(s.size(), L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size()));
    return w;
}

// Paths (relative to the launcher folder) an update must never replace.
static bool PlayerOwned(const wstring& rel) {
    static const wchar_t* kKeep[] = { L"sc-offline.ini", L"data\\wallet.txt", L"data\\spawn.txt", L"data\\bookmarks.txt",
        L"data\\locations_found.txt", L"data\\game-path.txt", L"data\\game-build.txt", L"data\\mod.log",
        L"data\\launcher.log", L"data\\registry-dump.txt" };
    for (const wchar_t* k : kKeep) if (!_wcsicmp(rel.c_str(), k)) return true;
    return !_wcsnicmp(rel.c_str(), L"data\\update\\", 12);
}

static bool CanWriteTo(const wstring& dir);

static wstring UpdateDir(const wstring& here) { return here + L"\\data\\update"; }
static wstring Journal(const wstring& here) { return UpdateDir(here) + L"\\applied.txt"; }

static bool JournalLine(const wstring& here, const std::string& line) {
    HANDLE f = CreateFileW(Journal(here).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const std::string l = line + "\n";
    DWORD put = 0;
    const bool ok = WriteFile(f, l.data(), (DWORD)l.size(), &put, nullptr) && put == l.size();
    FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}

// Undo an unfinished swap from its journal: "R <rel>" = the old file was moved to <rel>.update-old,
// "N <rel>" = a new file was created where none was. Processed newest first.
static void RollBackUpdate(const wstring& here) {
    std::string j;
    if (!ReadAll(Journal(here), j)) return;
    std::vector<std::string> lines;
    for (size_t p = 0; p < j.size();) { size_t n = j.find('\n', p); if (n == std::string::npos) n = j.size(); lines.push_back(j.substr(p, n - p)); p = n + 1; }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->size() < 3) continue;
        const wstring path = here + L"\\" + Wide(it->substr(2));
        if ((*it)[0] == 'N') DeleteFileW(path.c_str());
        else if ((*it)[0] == 'R' && IsFile(path + L".update-old")) {
            if (IsFile(path) && !DeleteFileW(path.c_str())) MoveFileExW(path.c_str(), (path + L".update-failed").c_str(), MOVEFILE_REPLACE_EXISTING);
            MoveFileExW((path + L".update-old").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
}

// Run at every start: finish (clean up) or roll back whatever a previous update left.
static void SettlePreviousUpdate(const wstring& here) {
    std::string j;
    if (!ReadAll(Journal(here), j)) return;
    const bool done = j.size() >= 5 && j.compare(j.size() - 5, 5, "done\n") == 0;
    if (!done) {
        Out("[!] a previous update didn't finish; putting the old files back\n");
        RollBackUpdate(here);
    }
    bool leftover = false;
    for (size_t p = 0; p < j.size();) {
        size_t n = j.find('\n', p); if (n == std::string::npos) n = j.size();
        const std::string line = j.substr(p, n - p); p = n + 1;
        if (line.size() > 2 && line[0] == 'R') {
            const wstring old = here + L"\\" + Wide(line.substr(2)) + L".update-old";
            if (IsFile(old) && !DeleteFileW(old.c_str())) leftover = true;   // the old exe can still be locked for a moment
        }
    }
    if (!leftover) RemoveTree(UpdateDir(here));
}

// Keys the new sc-offline.ini has that the player's doesn't: appended commented out, with defaults.
static void MergeIni(const wstring& here, const wstring& newIni, const std::string& tag) {
    std::string mine, theirs;
    if (!ReadAll(here + L"\\sc-offline.ini", mine) || !ReadAll(newIni, theirs)) return;
    auto keys = [](const std::string& t) {
        std::vector<std::pair<std::string, std::string>> out;
        for (size_t p = 0; p < t.size();) {
            size_t n = t.find('\n', p); if (n == std::string::npos) n = t.size();
            std::string l = t.substr(p, n - p); p = n + 1;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            size_t b = l.find_first_not_of(" \t#"); if (b == std::string::npos) continue;
            const size_t eq = l.find('=', b); if (eq == std::string::npos) continue;
            std::string k = l.substr(b, eq - b); while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
            if (k.empty() || k.find(' ') != std::string::npos) continue;
            out.push_back({ k, l.substr(b) });
        }
        return out;
    };
    const auto mineKeys = keys(mine);
    std::string add;
    for (const auto& kv : keys(theirs)) {
        bool have = false;
        for (const auto& m : mineKeys) if (!_stricmp(m.first.c_str(), kv.first.c_str())) { have = true; break; }
        if (!have) { add += "# " + kv.second + "\r\n"; Out("[i] sc-offline.ini: new setting %s (added commented out; the default applies)\n", kv.first.c_str()); }
    }
    if (add.empty()) return;
    if (!mine.empty() && mine.back() != '\n') mine += "\r\n";
    WriteAll(here + L"\\sc-offline.ini", mine + "\r\n# New in " + tag + ":\r\n" + add);
}

// Download, verify, swap. Returns true when the new version is in place.
static bool ApplyUpdate(const wstring& here, const UpdateInfo& u);

// Issue #16: a mod folder only administrators can write (e.g. under Program Files). Run
// `sc-offline.exe --elevated-update` once elevated; it checks, verifies and swaps but never starts
// the game or relaunches, so the new launcher (and the game) still start with normal rights.
static bool ApplyUpdateMaybeElevated(const wstring& here, const UpdateInfo& u) {
    if (CanWriteTo(here) && CanWriteTo(here + L"\\data")) return ApplyUpdate(here, u);
    Out("[i] this folder needs administrator rights to update; Windows will ask once.\n"
        "    (Only the update runs as administrator; the game never does.)\n");
    wchar_t self[MAX_PATH * 2]; GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    const wstring args = L"--elevated-update " + Wide(u.tag);
    SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof(sei); sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; sei.lpFile = self; sei.lpParameters = args.c_str(); sei.lpDirectory = here.c_str(); sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) { Out("[!] administrator rights refused; nothing changed\n"); return false; }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code); CloseHandle(sei.hProcess);
    if (code) { Out("[!] the update failed (exit %lu); see data\\launcher.log. Your current version is unchanged.\n", code); return false; }
    return true;
}

static bool ApplyUpdate(const wstring& here, const UpdateInfo& u) {
    const wstring dir = UpdateDir(here);
    RemoveTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    Out("[i] downloading %s ...\n", u.zipName.c_str());
    std::string zip;
    if (!HttpGet(Wide(u.zipUrl), zip, 60000, 256u << 20)) { Out("[!] download failed; nothing changed\n"); RemoveTree(dir); return false; }
    const std::string got = Sha256(zip);
    if (got != u.sha256) {
        Out("[!] the download doesn't match GitHub's SHA-256 (got %s, expected %s); nothing changed\n", got.c_str(), u.sha256.c_str());
        RemoveTree(dir); return false;
    }
    Out("[+] SHA-256 matches GitHub's digest\n");
    const wstring zipPath = dir + L"\\" + Wide(u.zipName), ex = dir + L"\\new";
    CreateDirectoryW(ex.c_str(), nullptr);
    if (!WriteAll(zipPath, zip)) { Out("[!] couldn't save the download; nothing changed\n"); RemoveTree(dir); return false; }
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const wstring tar = wstring(sys) + L"\\tar.exe";
    DWORD code = 1;
    if (!IsFile(tar) || !RunWait(tar, L"\"" + tar + L"\" -xf \"" + zipPath + L"\" -C \"" + ex + L"\"", ex, code) || code) {
        Out("[!] couldn't unpack the zip (Windows' tar.exe %s); nothing changed.\n"
            "    Update by hand: %s\n", IsFile(tar) ? "failed" : "is missing", "https://github.com/scubamount/sc-offline/releases/latest");
        RemoveTree(dir); return false;
    }
    const wstring pkg = ex + L"\\" + Wide(u.zipName.substr(0, u.zipName.size() - 4));
    if (!IsFile(pkg + L"\\sc-offline.exe") || !IsFile(pkg + L"\\dinput8.dll")) {
        Out("[!] the zip doesn't hold sc-offline.exe and dinput8.dll where expected; nothing changed\n");
        RemoveTree(dir); return false;
    }
    // Swap, journaling each step before it happens.
    const std::vector<wstring> files = FilesUnder(pkg);
    int replaced = 0, added = 0;
    bool ok = true;
    for (const wstring& rel : files) {
        if (PlayerOwned(rel)) continue;
        const wstring dst = here + L"\\" + rel, src = pkg + L"\\" + rel;
        const size_t cut = rel.find_last_of(L'\\');
        if (cut != wstring::npos) SHCreateDirectoryExW(nullptr, (here + L"\\" + rel.substr(0, cut)).c_str(), nullptr);
        if (IsFile(dst)) {
            if (!JournalLine(here, "R " + Narrow(rel)) || !MoveFileExW(dst.c_str(), (dst + L".update-old").c_str(), MOVEFILE_REPLACE_EXISTING)) {
                Out("[!] couldn't move %ls aside (error %lu)\n", rel.c_str(), GetLastError()); ok = false; break;
            }
            ++replaced;
        } else {
            if (!JournalLine(here, "N " + Narrow(rel))) { ok = false; break; }
            ++added;
        }
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) { Out("[!] couldn't write %ls (error %lu)\n", rel.c_str(), GetLastError()); ok = false; break; }
    }
    if (!ok) {
        Out("[!] update failed; putting the old files back\n");
        RollBackUpdate(here);
        RemoveTree(dir);
        return false;
    }
    if (IsFile(pkg + L"\\sc-offline.ini")) MergeIni(here, pkg + L"\\sc-offline.ini", u.tag);
    JournalLine(here, "done");
    Out("[+] updated to %s: %d files replaced, %d added; your saves and sc-offline.ini were kept\n", u.tag.c_str(), replaced, added);
    return true;
}

// Starts the new launcher with the same arguments and waits for it, so a double-clicked window
// stays open; this process then exits with its code. (While this old exe waits, its .update-old
// copy is locked, so the cleanup of data\update finishes on the run after.)
static void Relaunch(const wstring& here) {
    const wstring exe = here + L"\\sc-offline.exe";
    wstring cmd = GetCommandLineW();
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, here.c_str(), &si, &pi)) {
        Out("[!] couldn't start the new launcher (error %lu); run sc-offline.exe again\n", GetLastError());
        PauseIfOwnConsole();
        ExitProcess(kExitError);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);   // keep this console open for the new launcher
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    ExitProcess(code);
}


// --- Crash reports (issue #18) -------------------------------------------------------------
// After a session in which the game crashed (new files under <channel>\Crashes), offer to bundle
// the logs into data\crash-reports\*.zip with the RSI handle, account numbers and Windows user
// name redacted, then open a prefilled bug form. Nothing is uploaded: the player attaches the zip.
// Minidumps (.dmp) are raw memory and can't be redacted, so they are left out.

// "Handle[Name]" -> "Name". Every bracketed value after these keys is redacted as well.
static const char* kRedactKeys[] = { "Handle", "Nickname", "GEID", "PlayerGEID", "AccountId", "Account", "accountId", "playerGEID" };

static std::vector<std::string> HandlesIn(const std::string& text) {
    std::vector<std::string> found;
    for (const char* key : { "Handle[", "Nickname[", "nickname=\"", "handle=\"" }) {
        const char close = key[strlen(key) - 1] == '[' ? ']' : '"';
        for (size_t p = text.find(key); p != std::string::npos; p = text.find(key, p + 1)) {
            const size_t a = p + strlen(key), b = text.find(close, a);
            if (b == std::string::npos || b == a || b - a > 64) continue;
            const std::string h = text.substr(a, b - a);
            bool plain = true;
            for (char c : h) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) plain = false;
            if (plain && h.size() >= 3 && std::find(found.begin(), found.end(), h) == found.end()) found.push_back(h);
        }
    }
    return found;
}

static void ReplaceAllNoCase(std::string& t, const std::string& what, const std::string& with) {
    if (what.empty()) return;
    std::string low = t, w = what;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    for (char& c : w) c = (char)tolower((unsigned char)c);
    std::string out; size_t from = 0;
    for (size_t p = low.find(w); p != std::string::npos; p = low.find(w, p + w.size())) {
        out.append(t, from, p - from); out += with; from = p + w.size();
    }
    out.append(t, from, std::string::npos);
    t.swap(out);
}

// Redacts one file's text. handles: every RSI handle seen in any bundled file; user: Windows user name.
static std::string Redact(std::string t, const std::vector<std::string>& handles, const std::string& user) {
    for (const char* key : kRedactKeys) {
        const std::string k = std::string(key) + "[";
        for (size_t p = t.find(k); p != std::string::npos; p = t.find(k, p + 1)) {
            const size_t a = p + k.size(), b = t.find(']', a);
            if (b == std::string::npos || b - a > 80) continue;
            const bool numeric = !strcmp(key, "Handle") || !strcmp(key, "Nickname") ? false : true;
            t.replace(a, b - a, numeric ? "<id>" : "<handle>");
        }
    }
    // "geid 200012345", "accountId=9876543", "playerGEID: 1": a key, up to 2 separators, then digits.
    std::string low = t;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    for (const char* key : { "geid", "accountid", "account_id", "playerid" }) {
        const size_t kl = strlen(key);
        for (size_t p = low.find(key); p != std::string::npos; p = low.find(key, p + kl)) {
            if (p && isalnum((unsigned char)low[p - 1]) && low[p - 1] != 'r') continue;   // "playergeid" ok, "xgeid" no
            size_t a = p + kl, seps = 0;
            while (a < low.size() && seps < 3 && (low[a] == ' ' || low[a] == '=' || low[a] == ':' || low[a] == '"' || low[a] == '[')) { ++a; ++seps; }
            size_t b = a; while (b < low.size() && isdigit((unsigned char)low[b])) ++b;
            if (b - a < 3) continue;
            t.replace(a, b - a, "<id>"); low.replace(a, b - a, "<id>");
        }
    }
    for (const std::string& h : handles) ReplaceAllNoCase(t, h, "<handle>");
    if (user.size() >= 2) {
        for (const char* sep : { "\\Users\\", "/Users/", "\\\\Users\\\\", "/home/" })
            ReplaceAllNoCase(t, std::string(sep) + user, std::string(sep) + "<user>");
    }
    return t;
}

static std::string UrlEncode(const std::string& s) {
    std::string o; char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { std::snprintf(b, sizeof(b), "%%%02X", c); o += b; }
    }
    return o;
}

// --- Session logs (issue #12) ---------------------------------------------------------------
// After a modded session, the game's own logs in the channel folder record it. Offer to delete
// the ones written during this session, list them first, and only delete on an explicit "y".
// Older files (online play before this session) are never touched.

static ULONGLONG FileTimeU64(const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

static void SessionFilesIn(const wstring& dir, ULONGLONG since, bool recurse, std::vector<wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (recurse) SessionFilesIn(p, since, true, out); continue; }
        if (FileTimeU64(fd.ftLastWriteTime) >= since) out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::vector<wstring> SessionLogs(const wstring& channelDir, ULONGLONG since) {
    std::vector<wstring> files;
    const wstring gameLog = channelDir + L"\\Game.log";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(gameLog.c_str(), GetFileExInfoStandard, &a) && FileTimeU64(a.ftLastWriteTime) >= since)
        files.push_back(gameLog);
    SessionFilesIn(channelDir + L"\\logbackups", since, false, files);
    SessionFilesIn(channelDir + L"\\Crashes", since, true, files);
    return files;
}

static bool AskYes(const char* question) {
    std::printf("%s [y/N] ", question);
    std::fflush(stdout);
    char line[32] = {};
    if (!std::fgets(line, sizeof(line), stdin)) return false;
    return line[0] == 'y' || line[0] == 'Y';
}

static void OfferToCleanLogs(const wstring& channelDir, ULONGLONG since) {
    const std::vector<wstring> files = SessionLogs(channelDir, since);
    if (files.empty()) { Out("Logs:     the game wrote no logs this session\n"); return; }
    Out("\nThe game wrote these logs during this modded session:\n");
    for (const wstring& f : files) Out("    %ls\n", f.c_str());
    Out("Deleting them removes the record of this session from your game folder. Keep Game.log if you\n"
        "want to report a bug. Older logs are not touched. (clean_logs = off in sc-offline.ini skips this.)\n");
    if (!AskYes("Delete these files?")) { Out("Logs:     kept\n"); return; }
    if (!AskYes("Are you sure? They can't be recovered.")) { Out("Logs:     kept\n"); return; }
    int gone = 0, failed = 0, denied = 0;
    for (const wstring& f : files) {
        if (DeleteFileW(f.c_str())) ++gone;
        else if (GetLastError() == ERROR_ACCESS_DENIED) ++denied;
        else { ++failed; Out("[!] couldn't delete %ls (error %lu)\n", f.c_str(), GetLastError()); }
    }
    if (denied) {
        // Issue #16: the game folder needs administrator rights. One UAC prompt; the elevated run
        // works out this session's files again itself (DeleteSessionLogs), it isn't handed a list.
        Out("[i] %d file%s need administrator rights to delete; Windows will ask once.\n", denied, denied == 1 ? "" : "s");
        wchar_t self[MAX_PATH * 2]; GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
        const wstring args = L"--delete-logs \"" + channelDir + L"\" " + std::to_wstring(since);
        SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof(sei); sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas"; sei.lpFile = self; sei.lpParameters = args.c_str(); sei.nShow = SW_HIDE;
        DWORD code = 1;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 60000); GetExitCodeProcess(sei.hProcess, &code); CloseHandle(sei.hProcess);
        }
        const size_t left = SessionLogs(channelDir, since).size();
        if (code == 0 && left == 0) gone += denied;
        else { failed += (int)left; Out("[!] %zu file%s still there (administrator rights refused?)\n", left, left == 1 ? "" : "s"); }
    }
    Out("Logs:     deleted %d file%s%s\n", gone, gone == 1 ? "" : "s", failed ? "; see above for the rest" : "");
}

// --delete-logs <channel folder> <since>: the elevated half of OfferToCleanLogs. Only acts on a
// real channel folder (Bin64\StarCitizen.exe inside) and only on this session's files there.
static int DeleteSessionLogs(const wstring& channelDir, ULONGLONG since) {
    if (!IsFile(channelDir + L"\\Bin64\\" + kGameExe) || since == 0) return 4;
    int failed = 0;
    for (const wstring& f : SessionLogs(channelDir, since)) if (!DeleteFileW(f.c_str())) ++failed;
    return failed ? 3 : 0;
}


// --- Finding the game -------------------------------------------------------------------
// Tried in order, first hit wins: --game, `game =` in sc-offline.ini, the folder remembered
// from the last run (data\game-path.txt), what the RSI Launcher itself recorded (its uninstall
// entry and the paths in %APPDATA%\rsilauncher), the usual folders on every fixed drive, a
// bounded search of every fixed drive, and finally a folder picker. Every candidate is only
// accepted when <channel>\Bin64\StarCitizen.exe exists, so a stale or wrong path is skipped.

struct Found { wstring bin; const char* how; };

static bool SamePath(const wstring& a, const wstring& b) { return !_wcsicmp(a.c_str(), b.c_str()); }

static void AddFound(std::vector<Found>& out, const wstring& bin, const char* how) {
    if (bin.empty()) return;
    for (const Found& f : out) if (SamePath(f.bin, bin)) return;
    out.push_back({ bin, how });
}

// A path the RSI Launcher wrote may point anywhere inside the install (the library folder,
// StarCitizen, LIVE, Bin64 or a file). Walk up a few levels and take the first that resolves.
static wstring ResolveUpward(wstring p, const wstring& channel) {
    p = StripSlashes(p);
    for (int up = 0; up < 6 && p.size() > 3; ++up) {
        const wstring bin = ToBin64(p, channel);
        if (!bin.empty()) return bin;
        const size_t cut = p.find_last_of(L"\\/");
        if (cut == wstring::npos || cut < 2) break;
        p.resize(cut);
    }
    return L"";
}

static wstring RememberedPathFile(const wstring& here) { return here + L"\\data\\game-path.txt"; }

static void FromRemembered(const wstring& here, const wstring& channel, std::vector<Found>& out) {
    std::string bytes;
    if (!ReadAll(RememberedPathFile(here), bytes)) return;
    wstring w(bytes.size(), L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), w.data(), (int)w.size()));
    AddFound(out, ToBin64(Trim(w), channel), "remembered from your last run");
}

// HKLM/HKCU ...\Uninstall\*: the RSI Launcher's InstallLocation is usually
// <library>\RSI Launcher, and the game sits beside it in <library>\StarCitizen.
static void FromUninstallKeys(const wstring& channel, std::vector<Found>& out) {
    static const struct { HKEY root; const wchar_t* path; } kKeys[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    for (const auto& k : kKeys) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(k.root, k.path, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD n = ARRAYSIZE(sub);
            if (RegEnumKeyExW(h, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t name[256] = {}, loc[MAX_PATH * 2] = {};
            DWORD cb = sizeof(name);
            if (RegGetValueW(h, sub, L"DisplayName", RRF_RT_REG_SZ, nullptr, name, &cb) != ERROR_SUCCESS) continue;
            if (!StrStrIW(name, L"RSI Launcher") && !StrStrIW(name, L"Star Citizen")) continue;
            cb = sizeof(loc);
            if (RegGetValueW(h, sub, L"InstallLocation", RRF_RT_REG_SZ, nullptr, loc, &cb) != ERROR_SUCCESS) continue;
            const wstring l = StripSlashes(Trim(loc));
            if (l.empty()) continue;
            wstring bin = ToBin64(l, channel);
            if (bin.empty()) {
                const size_t cut = l.find_last_of(L"\\/");
                if (cut != wstring::npos) bin = ToBin64(l.substr(0, cut), channel);
            }
            AddFound(out, bin, "from the RSI Launcher's install entry");
        }
        RegCloseKey(h);
    }
}

// The RSI Launcher keeps its settings and logs in %APPDATA%\rsilauncher. Rather than depend on
// their exact format, pull every drive-letter path out of them and keep the ones that resolve.
static void PathsInText(const std::string& text, const wstring& channel, std::vector<Found>& out) {
    int tried = 0;
    for (size_t i = 0; i + 3 < text.size() && tried < 400; ++i) {
        const char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) || text[i + 1] != ':' ||
            (text[i + 2] != '\\' && text[i + 2] != '/') || (i && std::isalnum((unsigned char)text[i - 1])))
            continue;
        size_t e = i + 2;
        while (e < text.size() && text[e] != '"' && text[e] != '\'' && text[e] != '\n' && text[e] != '\r' &&
               text[e] != ',' && text[e] != ')' && text[e] != '>' && text[e] != '<' && text[e] != '|' && e - i < 400)
            ++e;
        std::string p = text.substr(i, e - i);
        std::string clean;   // JSON escapes "\\" and some logs use "/"
        for (size_t k = 0; k < p.size(); ++k) {
            if (p[k] == '/') { clean += '\\'; continue; }
            if (p[k] == '\\' && k + 1 < p.size() && p[k + 1] == '\\') ++k;
            clean += p[k];
        }
        while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
        i = e; ++tried;
        wstring w(clean.size(), L'\0');
        w.resize(MultiByteToWideChar(CP_UTF8, 0, clean.data(), (int)clean.size(), w.data(), (int)w.size()));
        AddFound(out, ResolveUpward(w, channel), "from the RSI Launcher's settings or logs");
    }
}

static void FromRsiLauncherFiles(const wstring& channel, std::vector<Found>& out) {
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, ARRAYSIZE(appdata))) return;
    const wstring base = wstring(appdata) + L"\\rsilauncher";
    const wchar_t* kGlobs[] = { L"\\*.json", L"\\logs\\*.log", L"\\*.log" };
    for (const wchar_t* g : kGlobs) {
        const wstring pattern = base + g;
        const wstring dir = pattern.substr(0, pattern.find_last_of(L'\\'));
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pattern.c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (fd.nFileSizeHigh || fd.nFileSizeLow > 8u * 1024 * 1024) continue;   // skip huge logs
            std::string text;
            if (ReadAll(dir + L"\\" + fd.cFileName, text)) PathsInText(text, channel, out);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
}

static std::vector<wstring> FixedDrives() {
    std::vector<wstring> roots;
    const DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        const wstring root = wstring(1, wchar_t(L'A' + d)) + L":\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) roots.push_back(root);   // never a disc or network share
    }
    return roots;
}

static void FromUsualFolders(const wstring& channel, std::vector<Found>& out) {
    static const wchar_t* kBases[] = {
        L"Program Files\\Roberts Space Industries",   // the RSI Launcher's default, also under Wine
        L"Roberts Space Industries",
        L"Games\\Roberts Space Industries",
        L"Game\\Roberts Space Industries",
        L"Program Files (x86)\\Roberts Space Industries",
        L"StarCitizen",
        L"Star Citizen",
        L"Games\\StarCitizen",
        L"Games\\Star Citizen",
        L"Game\\StarCitizen",
        L"Game\\Star Citizen",
    };
    for (const wstring& root : FixedDrives())
        for (const wchar_t* b : kBases) AddFound(out, ToBin64(root + b, channel), "in a usual install folder");
}

// Breadth-first over every fixed drive, a few folders deep, skipping system folders and
// links. Bounded by depth and by the number of folders looked at, so it finishes in seconds.
static void FromDriveSearch(const wstring& channel, std::vector<Found>& out) {
    static const wchar_t* kSkip[] = { L"Windows", L"$Recycle.Bin", L"System Volume Information", L"Recovery",
                                      L"PerfLogs", L"$WinREAgent", L"AppData", L"node_modules", L"WindowsApps",
                                      L"Microsoft", L"WinSxS", L".git" };
    const int kMaxDepth = 4;
    int budget = 40000;
    for (const wstring& root : FixedDrives()) {
        std::vector<std::pair<wstring, int>> queue{ { StripSlashes(root), 0 } };
        for (size_t qi = 0; qi < queue.size() && budget > 0; ++qi) {
            const wstring dir = queue[qi].first; const int depth = queue[qi].second;
            if (depth && IsFile(dir + L"\\" + channel + L"\\Bin64\\" + kGameExe)) {
                AddFound(out, dir + L"\\" + channel + L"\\Bin64", "by searching your drives");
                continue;
            }
            if (depth >= kMaxDepth) continue;
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories,
                                        nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (f == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    continue;
                if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
                bool skip = false;
                for (const wchar_t* k : kSkip) if (!_wcsicmp(fd.cFileName, k)) { skip = true; break; }
                if (skip) continue;
                --budget;
                queue.push_back({ dir + L"\\" + fd.cFileName, depth + 1 });
            } while (FindNextFileW(f, &fd) && budget > 0);
            FindClose(f);
        }
    }
}

// Last resort when double-clicked: a folder picker. Returns empty if cancelled or unavailable.
static wstring PickFolder() {
    wstring result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"sc-offline: pick your StarCitizen folder (the one with LIVE in it)");
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->Show(GuiChild() ? nullptr : GetConsoleWindow())) && SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); }
            item->Release();
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

static bool OwnsConsoleInput() { DWORD m; return GuiChild() || GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &m) != 0; }

static bool OwnsConsole() { DWORD ids[2]; return GetConsoleProcessList(ids, 2) <= 1; }

// Automatic detection, cheapest and most reliable first. The drive search only runs when
// nothing cheaper found the game.
static std::vector<Found> DetectBin64(const wstring& here, const wstring& channel) {
    std::vector<Found> found;
    FromRemembered(here, channel, found);
    FromUninstallKeys(channel, found);
    FromRsiLauncherFiles(channel, found);
    FromUsualFolders(channel, found);
    if (found.empty()) {
        Out("[i] searching your drives for StarCitizen\\%ls\\Bin64 (a few seconds)...\n", channel.c_str());
        FromDriveSearch(channel, found);
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


// The original author's prebuilt dinput8.dll (once at the repository root, removed in 0.3.0).
// Still recognized so status/uninstall can identify and remove an old install.
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
struct DllInfo { bool present = false; std::string sha, what, version; bool ours = false; };

static DllInfo Identify(const wstring& path) {
    DllInfo d;
    std::string bytes;
    if (!ReadAll(path, bytes) || bytes.empty()) return d;
    d.present = true;
    d.sha = Sha256(bytes);
    // Ours: any sc-offline build (0.3.0+ says "sc-offline v<ver>"; earlier ones said
    // "ChrisWareOffline ... / sc-offline <ver>"), or the original prebuilt DLL.
    d.ours = bytes.find("ChrisWareOffline") != std::string::npos || bytes.find("sc-offline v") != std::string::npos;
    size_t v = bytes.find("sc-offline v"), skip = 12;
    if (v == std::string::npos) { v = bytes.find("/ sc-offline "); skip = 13; }
    if (v != std::string::npos) {
        size_t e = v + skip;
        while (e < bytes.size() && bytes[e] > 0x20 && bytes[e] < 0x7F) ++e;
        d.version = bytes.substr(v + skip, e - v - skip);
        d.what = "sc-offline " + d.version;
    } else if (d.sha == kPrebuiltSha256) {
        d.what = "the original author's prebuilt DLL";
    } else {
        d.what = d.ours ? "unknown sc-offline build" : "not sc-offline";
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

    if (IsFile(g.dll) && !leftover && !Identify(g.dll).ours) {
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
        if (haveMarker || Identify(g.dll).ours) {
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

static bool IsTextReport(const wstring& f) {
    const size_t dot = f.find_last_of(L'.');
    if (dot == wstring::npos) return false;
    const wstring ext = f.substr(dot);
    for (const wchar_t* e : { L".log", L".txt", L".json", L".xml", L".cfg", L".ini" }) if (!_wcsicmp(ext.c_str(), e)) return true;
    return false;
}

static void OfferCrashReport(const wstring& here, const wstring& channelDir, ULONGLONG since, const std::string& gameBuild) {
    std::vector<wstring> crash;
    SessionFilesIn(channelDir + L"\\Crashes", since, true, crash);
    if (crash.empty()) return;
    int dumps = 0;
    for (const wstring& f : crash) if (!IsTextReport(f)) ++dumps;
    Out("\n[!] the game crashed this session (%zu file%s in %ls\\Crashes).\n", crash.size(), crash.size() == 1 ? "" : "s", channelDir.c_str());
    Out("    A crash report zips mod.log, launcher.log, Game.log and the crash text files into data\\crash-reports,\n"
        "    with your RSI handle, account numbers and Windows user name replaced. Nothing is sent: you attach\n"
        "    the zip to the bug form yourself.%s\n", dumps ? " Memory dumps (.dmp) are left out; they can't be redacted." : "");
    if (!AskYes("Make a crash report?")) { Out("Crash:    no report made\n"); return; }

    struct Item { wstring src; wstring name; };
    std::vector<Item> items = { { here + L"\\data\\mod.log", L"mod.log" }, { here + L"\\data\\launcher.log", L"launcher.log" },
                                { channelDir + L"\\Game.log", L"Game.log" } };
    const wstring crashRoot = channelDir + L"\\Crashes\\";
    for (const wstring& f : crash) {
        if (!IsTextReport(f)) continue;
        wstring rel = f.substr(crashRoot.size());
        for (wchar_t& c : rel) if (c == L'\\') c = L'_';
        items.push_back({ f, L"Crashes_" + rel });
    }
    std::vector<std::string> texts(items.size());
    std::vector<std::string> handles;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!ReadAll(items[i].src, texts[i])) continue;
        if (texts[i].size() > (16u << 20)) texts[i].erase(0, texts[i].size() - (16u << 20));   // keep the tail of huge logs
        for (const std::string& h : HandlesIn(texts[i])) if (std::find(handles.begin(), handles.end(), h) == handles.end()) handles.push_back(h);
    }
    const std::string user = Narrow(EnvOr(L"USERNAME", L""));

    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[32]; swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const wstring dir = here + L"\\data\\crash-reports", stage = dir + L"\\tmp-" + stamp;
    const wstring zip = dir + L"\\sc-offline-crash-" + stamp + L".zip";
    SHCreateDirectoryExW(nullptr, stage.c_str(), nullptr);
    int put = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (texts[i].empty()) continue;
        if (WriteAll(stage + L"\\" + items[i].name, Redact(texts[i], handles, user))) ++put;
    }
    WriteAll(stage + L"\\README.txt", std::string("sc-offline ") + SCO_VERSION + " crash report\r\nGame: " + gameBuild +
             "\r\nRedacted: RSI handle -> <handle>, account/GEID numbers -> <id>, Windows user name -> <user>.\r\n");
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const wstring tar = wstring(sys) + L"\\tar.exe";
    DWORD code = 1;
    const bool zipped = IsFile(tar) && RunWait(tar, L"\"" + tar + L"\" -a -cf \"" + zip + L"\" -C \"" + stage + L"\" .", stage, code) && code == 0;
    if (zipped) RemoveTree(stage);
    const wstring made = zipped ? zip : stage;
    Out("Crash:    report saved: %ls (%d file%s%s)\n", made.c_str(), put, put == 1 ? "" : "s",
        zipped ? "" : "; tar.exe failed, so it is a folder - zip it yourself");
    Out("          Look through it before you share it: the redaction covers handle, IDs and user name only.\n");
    if (!AskYes("Open the bug form in your browser (you attach the zip there)?")) return;
    const std::string url = "https://github.com/scubamount/sc-offline/issues/new?template=bug.yml&title=" +
        UrlEncode("Crash: ") + "&version=" + UrlEncode(SCO_VERSION) + "&game=" + UrlEncode(gameBuild) +
        "&os=" + UrlEncode(OnWine() ? "Linux (Wine/Proton)" : "Windows");
    ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const wstring sel = L"/select,\"" + made + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
}


// --- what the helper changes on the PC while you play ------------------------------------------
// Three switches in sc-offline.ini, all on by default, all undone when the game closes:
//   block_network  a Windows Firewall rule that blocks StarCitizen.exe (this install only), in and out;
//   eac_hosts      the hosts line that stops the RSI Launcher downloading Easy Anti-Cheat again;
//   eac_rename     EasyAntiCheat_EOS.exe renamed to .bak.
// Only what this helper changed is recorded, and only that is undone: a hosts line or a .bak you made
// yourself is left alone. The record lives in %ProgramData%\sc-offline\pc-changes.txt so that after a
// crash any later run (play, status, uninstall) finds it.

static const char*    kEacHost      = "modules-cdn.eac-prod.on.epicgames.com";
static const char*    kHostsTag     = "# added by sc-offline, removed when the game closes";
static const wchar_t* kFirewallRule = L"sc-offline: block StarCitizen.exe";
// Issue #17: while you play, also block the RSI Launcher and CIG's crash uploader, so neither
// reports a modded session. sc-offline.exe itself stays online (self-update, #14).
static const wchar_t* kLauncherRule = L"sc-offline: block RSI Launcher.exe";
static const wchar_t* kCrashRule    = L"sc-offline: block CrashHandler.exe";

struct PcWanted { bool firewall = false, eacHosts = false, eacRename = false; };

static wstring PcChangesPath() { return EnvOr(L"ProgramData", L"C:\\ProgramData") + L"\\sc-offline\\pc-changes.txt"; }
static wstring HostsPath()     { return EnvOr(L"SystemRoot", L"C:\\Windows") + L"\\System32\\drivers\\etc\\hosts"; }
static wstring EacExe()        { return EnvOr(L"ProgramFiles(x86)", L"C:\\Program Files (x86)") + L"\\EasyAntiCheat_EOS\\EasyAntiCheat_EOS.exe"; }

// Runs a Windows tool without a window. Returns its exit code, or -1 if it couldn't start.
static int RunTool(const wstring& exe, const wstring& args) {
    wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

static wstring System32(const wchar_t* tool) { return EnvOr(L"SystemRoot", L"C:\\Windows") + L"\\System32\\" + tool; }

static bool FirewallRuleExists(const wchar_t* rule) {
    return RunTool(System32(L"netsh.exe"), L"advfirewall firewall show rule name=\"" + wstring(rule) + L"\"") == 0;
}

static bool AddFirewallRule(const wchar_t* rule, const wstring& exe) {
    for (const wchar_t* dir : { L"out", L"in" }) {
        const wstring args = L"advfirewall firewall add rule name=\"" + wstring(rule) + L"\" dir=" + dir +
                             L" action=block enable=yes profile=any program=\"" + exe + L"\"";
        if (RunTool(System32(L"netsh.exe"), args) != 0) return false;
    }
    return true;
}

static bool DeleteFirewallRule(const wchar_t* rule) {
    return RunTool(System32(L"netsh.exe"), L"advfirewall firewall delete rule name=\"" + wstring(rule) + L"\"") == 0;
}

// RSI Launcher.exe: its uninstall entry first, then beside the game library, then the default folder.
static wstring RsiLauncherExe(const wstring& bin) {
    static const struct { HKEY root; const wchar_t* path; } kKeys[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    for (const auto& k : kKeys) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(k.root, k.path, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD n = ARRAYSIZE(sub);
            if (RegEnumKeyExW(h, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t name[256] = {}, loc[MAX_PATH * 2] = {};
            DWORD cb = sizeof(name);
            if (RegGetValueW(h, sub, L"DisplayName", RRF_RT_REG_SZ, nullptr, name, &cb) != ERROR_SUCCESS) continue;
            if (!StrStrIW(name, L"RSI Launcher")) continue;
            cb = sizeof(loc);
            if (RegGetValueW(h, sub, L"InstallLocation", RRF_RT_REG_SZ, nullptr, loc, &cb) != ERROR_SUCCESS) continue;
            const wstring exe = StripSlashes(Trim(loc)) + L"\\RSI Launcher.exe";
            if (IsFile(exe)) { RegCloseKey(h); return exe; }
        }
        RegCloseKey(h);
    }
    // <library>\StarCitizen\<channel>\Bin64 -> <library>\RSI Launcher
    const wstring beside = ParentDir(ParentDir(ParentDir(bin))) + L"\\RSI Launcher\\RSI Launcher.exe";
    if (IsFile(beside)) return beside;
    const wstring def = EnvOr(L"ProgramFiles", L"C:\\Program Files") + L"\\Roberts Space Industries\\RSI Launcher\\RSI Launcher.exe";
    return IsFile(def) ? def : L"";
}

// <channel>\Tools\Public\CrashHandler.exe, CIG's crash reporter.
static wstring CrashHandlerExe(const wstring& bin) {
    const wstring exe = ParentDir(bin) + L"\\Tools\\Public\\CrashHandler.exe";
    return IsFile(exe) ? exe : L"";
}

struct FwRule { const char* key; const wchar_t* name; };
static const FwRule kFwRules[] = { { "firewall", kFirewallRule }, { "fw_launcher", kLauncherRule }, { "fw_crash", kCrashRule } };

// Appends the EAC line, tagged so it can be found again. Keeps the file's line endings.
static bool AddHostsLine() {
    std::string hosts;
    if (!ReadAll(HostsPath(), hosts)) return false;
    const std::string eol = hosts.find("\r\n") != std::string::npos || hosts.empty() ? "\r\n" : "\n";
    if (!hosts.empty() && hosts.back() != '\n') hosts += eol;
    hosts += std::string("127.0.0.1 ") + kEacHost + " " + kHostsTag + eol;
    return WriteAll(HostsPath(), hosts);
}

// Removes only the lines this launcher added (the ones carrying kHostsTag).
static bool RemoveHostsLine() {
    std::string hosts, kept;
    if (!ReadAll(HostsPath(), hosts)) return false;
    bool removed = false;
    for (size_t pos = 0; pos < hosts.size();) {
        size_t nl = hosts.find('\n', pos);
        const size_t end = nl == std::string::npos ? hosts.size() : nl + 1;
        const std::string line = hosts.substr(pos, end - pos);
        if (line.find(kHostsTag) != std::string::npos) removed = true; else kept += line;
        pos = end;
    }
    return !removed || WriteAll(HostsPath(), kept);
}

static void FlushDns() { RunTool(System32(L"ipconfig.exe"), L"/flushdns"); }

// What a previous helper recorded and hasn't undone yet ("" = nothing).
static std::string PcChangesLeft() {
    std::string s;
    return ReadAll(PcChangesPath(), s) ? s : "";
}

static void DescribePcChanges(const std::string& rec) {
    for (const FwRule& r : kFwRules)
        if (Field(rec, r.key) == "added") Out("          - firewall rule \"%ls\"\n", r.name);
    if (Field(rec, "hosts") == "added")    Out("          - hosts line for %s\n", kEacHost);
    if (Field(rec, "eac") == "renamed")    Out("          - EasyAntiCheat_EOS.exe renamed to .bak\n");
}

// Undoes what the record says this launcher did, then deletes the record. 0 ok, 5 something stayed.
static int UndoPcChanges(bool dry) {
    const std::string rec = PcChangesLeft();
    if (rec.empty()) return 0;
    int rc = 0;
    for (const FwRule& r : kFwRules) {
        if (Field(rec, r.key) == "added" && Step(dry, "remove the firewall rule \"%ls\"", r.name) &&
            !DeleteFirewallRule(r.name) && FirewallRuleExists(r.name)) {
            Out("[!] couldn't remove the firewall rule; remove \"%ls\" in Windows Defender Firewall\n", r.name); rc = 5;
        }
    }
    if (Field(rec, "hosts") == "added" && Step(dry, "remove sc-offline's line from the hosts file")) {
        if (RemoveHostsLine()) FlushDns();
        else { Out("[!] couldn't edit %ls (error %lu); delete the %s line by hand\n", HostsPath().c_str(), GetLastError(), kEacHost); rc = 5; }
    }
    if (Field(rec, "eac") == "renamed" && Step(dry, "rename EasyAntiCheat_EOS.exe.bak back")) {
        const wstring exe = EacExe(), bak = exe + L".bak";
        if (IsFile(bak) && !IsFile(exe) && !MoveFileExW(bak.c_str(), exe.c_str(), 0)) {
            Out("[!] couldn't rename %ls back (error %lu)\n", bak.c_str(), GetLastError()); rc = 5;
        }
    }
    if (rc == 0 && !dry) DeleteFileW(PcChangesPath().c_str());
    return rc;
}

// Makes the wanted changes and records each one as soon as it is made. 0 ok, 5 failed (anything
// already changed stays recorded, and the caller undoes it).
static int ApplyPcChanges(const PcWanted& w, const wstring& gameExe, bool dry) {
    std::string rec = PcChangesLeft();   // a leftover record after a crash: keep it, add to it
    const wstring dir = ParentDir(PcChangesPath());
    if (!dry) CreateDirectoryW(dir.c_str(), nullptr);
    auto record = [&](const char* key, const char* value) {
        if (Field(rec, key) == value) return;
        rec += std::string(key) + "=" + value + "\n";
        if (!WriteAll(PcChangesPath(), rec)) Out("[!] couldn't write %ls (error %lu)\n", PcChangesPath().c_str(), GetLastError());
    };
    if (!dry && rec.find("time=") == std::string::npos) {
        SYSTEMTIME t; GetLocalTime(&t);
        char ts[32]; std::snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
        record("time", ts);
    }

    if (w.firewall) {
        const wstring bin = ParentDir(gameExe);
        const wstring exes[] = { gameExe, RsiLauncherExe(bin), CrashHandlerExe(bin) };
        for (size_t i = 0; i < ARRAYSIZE(kFwRules); ++i) {
            const FwRule& r = kFwRules[i];
            if (exes[i].empty()) { if (i == 1) Out("[i] RSI Launcher.exe not found; not blocking it\n"); continue; }
            if (Field(rec, r.key) == "added" && !dry && FirewallRuleExists(r.name)) Out("[i] firewall rule \"%ls\" already in place\n", r.name);
            else if (Step(dry, "add a firewall rule blocking %ls", exes[i].c_str())) {
                if (!AddFirewallRule(r.name, exes[i])) { Out("[!] couldn't add the firewall rule (netsh failed)\n"); DeleteFirewallRule(r.name); return 5; }
                record(r.key, "added");
            }
        }
    }
    if (w.eacHosts) {
        if (HostsBlocksEac()) Out("[i] hosts file already blocks %s\n", kEacHost);
        else if (Step(dry, "add `127.0.0.1 %s` to the hosts file", kEacHost)) {
            if (!AddHostsLine()) { Out("[!] couldn't edit %ls (error %lu); antivirus may protect it\n", HostsPath().c_str(), GetLastError()); return 5; }
            record("hosts", "added");
            FlushDns();
        }
    }
    if (w.eacRename) {
        const wstring exe = EacExe(), bak = exe + L".bak";
        if (!IsFile(exe)) Out("[i] Easy Anti-Cheat is already off or not installed\n");
        else if (Step(dry, "rename EasyAntiCheat_EOS.exe to .bak")) {
            if (!MoveFileExW(exe.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                Out("[!] couldn't rename %ls (error %lu)\n", exe.c_str(), GetLastError()); return 5;
            }
            record("eac", "renamed");
        }
    }
    return 0;
}

static bool NeedsPcChanges(const PcWanted& w) { return !OnWine() && (w.firewall || w.eacHosts || w.eacRename); }

static wstring PcFlags(const PcWanted& w) {
    if (OnWine()) return L"-";
    wstring f;
    if (w.firewall) f += L'f';
    if (w.eacHosts) f += L'h';
    if (w.eacRename) f += L'e';
    return f.empty() ? L"-" : f;
}

static PcWanted ParsePcFlags(const wstring& f) {
    PcWanted w;
    w.firewall = f.find(L'f') != wstring::npos;
    w.eacHosts = f.find(L'h') != wstring::npos;
    w.eacRename = f.find(L'e') != wstring::npos;
    return w;
}

// --helper <play|install|uninstall> <Bin64> <launcher pid> <event name> <pc flags>
// The part that changes the game folder and the PC; runs elevated when either needs it.
// play: make the PC changes (pc flags: f firewall, h hosts, e EAC rename, - none), put the mod in,
// signal the event, wait for the launcher and every StarCitizen.exe to exit, then take the mod
// out and undo the PC changes. install: put in. uninstall: take out and undo leftover PC changes.
// Exit codes: 0 ok, 2 copy failed, 3 removal failed, 4 bad args, 5 PC change failed.
static int Helper(const wstring& op, const wstring& bin, DWORD parentPid, const wstring& eventName, const wstring& flags) {
    const wstring here = ExeDir();
    OpenLog(here + L"\\data", true, Narrow(L"helper " + op).c_str());
    const GamePaths g(bin);
    if (!_wcsicmp(op.c_str(), L"uninstall")) { const int a = TakeMod(g, false), b = UndoPcChanges(false); return a ? a : b; }
    const bool play = !_wcsicmp(op.c_str(), L"play");
    if (!play && _wcsicmp(op.c_str(), L"install")) return 4;
    HANDLE parent = play ? OpenProcess(SYNCHRONIZE, FALSE, parentPid) : nullptr;
    if (play && !parent) return 4;
    if (play) {
        const int pc = ApplyPcChanges(ParsePcFlags(flags), bin + L"\\" + kGameExe, false);
        if (pc) { UndoPcChanges(false); CloseHandle(parent); return pc; }
    }
    const int rc = PutMod(here, g, play ? "play" : "install", false);
    if (rc) { if (parent) { UndoPcChanges(false); CloseHandle(parent); } return rc; }
    if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str())) { SetEvent(ready); CloseHandle(ready); }
    if (!play) return 0;

    WaitForSingleObject(parent, INFINITE);
    CloseHandle(parent);
    while (GameRunning()) Sleep(2000);
    const int taken = TakeMod(g, false);
    const bool hadPc = !PcChangesLeft().empty();
    const int undone = UndoPcChanges(false);
    if (hadPc && undone == 0) Out("[+] PC changes undone\n");
    return taken ? taken : undone;
}

// Starts the helper: hidden and unelevated when the game folder is writable, otherwise elevated
// (one UAC prompt). Returns its process handle, or null.
static HANDLE StartHelper(const wchar_t* op, const GamePaths& g, const wstring& eventName, const PcWanted& pc) {
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    const wstring args = L"--helper " + wstring(op) + L" \"" + g.bin + L"\" " + std::to_wstring(GetCurrentProcessId()) +
                         L" " + eventName + L" " + PcFlags(pc);
    const bool isPlay = !_wcsicmp(op, L"play");
    const bool pcChanges = (isPlay && NeedsPcChanges(pc)) ||
                           ((isPlay || !_wcsicmp(op, L"uninstall")) && !PcChangesLeft().empty());
    if (!pcChanges && CanWriteTo(g.bin) && CanWriteTo(ParentDir(g.bin))) {
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
    if (pcChanges)
        Out("[i] Windows will ask for administrator rights once: the helper blocks the game's network and\n"
            "    turns Easy Anti-Cheat off while you play, and undoes both when the game closes.\n"
            "    Only the helper runs as administrator, not the game.\n");
    else
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
    SetEnvironmentVariableW(name, v.empty() ? nullptr : v.c_str());   // empty = unset
}

struct CheckResult { bool eacActive = false, pcLeftover = false; std::string gameBuild; };

static CheckResult SelfChecks(const wstring& here, const Config& cfg, const GamePaths& g) {
    CheckResult r;
    // 1. The DLL this launcher would copy in.
    const DllInfo mod = Identify(here + L"\\dinput8.dll");
    if (!mod.present) Out("Mod DLL:  missing (dinput8.dll next to sc-offline.exe)\n");
    else {
        Out("Mod DLL:  %s\n          sha256 %s\n", mod.what.c_str(), mod.sha.c_str());
        if (mod.sha == kPrebuiltSha256 && _wcsicmp(cfg.bootMap.c_str(), L"PU"))
            Out("[!] the prebuilt DLL only knows boot_map = PU; set that in sc-offline.ini (now %ls)\n", cfg.bootMap.c_str());
        if (!mod.version.empty() && mod.version != SCO_VERSION)
            Out("[i] this launcher is %s; the DLL is %s\n", SCO_VERSION, mod.version.c_str());
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
    else if (haveMarker || inGame.ours)
        Out("Installed: yes, %s%s%s\n", inGame.what.c_str(), haveMarker ? ", since " : "",
            haveMarker ? Field(marker, "time").c_str() : "");
    else Out("Installed: no; Bin64 has another mod's dinput8.dll (it is set aside while you play)\n");
    if ((haveMarker || inGame.ours) && inGame.present && !GameRunning())
        Out("[!] the mod is still in the game folder (a crash, or `install`).\n"
            "    Run `sc-offline.exe uninstall` before going online.\n");

    // 4. What sc-offline changed on the PC, if a previous run didn't get to undo it (a crash).
    const std::string pcLeft = PcChangesLeft();
    if (!pcLeft.empty()) {
        r.pcLeftover = true;
        Out("[!] PC changes from a previous run (%s) are still in place:\n", Field(pcLeft, "time").c_str());
        DescribePcChanges(pcLeft);
        if (!GameRunning()) Out("    Run `sc-offline.exe uninstall` to undo them before going online.\n");
    }
    if (!OnWine()) Out("Network:  %s\n", cfg.firewall ? "blocked for StarCitizen.exe, RSI Launcher.exe and CrashHandler.exe while you play (block_network)"
                                                       : "NOT blocked (block_network = off)");

    // 5. Easy Anti-Cheat.
    const Eac eac = EacState();
    Out("EAC:      %s\n", eac == Eac::Active ? "ACTIVE" : eac == Eac::Disabled ? "disabled (EasyAntiCheat_EOS.exe.bak)" : "not installed");
    if (OnWine()) Out("Hosts:    checked by sc-offline.sh against /etc/hosts\n");
    else Out("Hosts:    %s\n", HostsBlocksEac() ? "EAC download server blocked" : "EAC download server NOT blocked");
    const bool autoEac = !OnWine() && cfg.eacRename;
    r.eacActive = eac == Eac::Active;
    if (eac == Eac::Active && autoEac) {
        Out("[i] Easy Anti-Cheat is on; `play` turns it off while you play and back on afterwards (eac_rename).\n");
    } else if (eac == Eac::Active) {
        Out("[!] Easy Anti-Cheat is active; the mod can't run with it. In PowerShell as administrator:\n"
            "      ren \"C:\\Program Files (x86)\\EasyAntiCheat_EOS\\EasyAntiCheat_EOS.exe\" EasyAntiCheat_EOS.exe.bak\n"
            "    and block modules-cdn.eac-prod.on.epicgames.com in your hosts file (README, Setup).\n");
    } else if (!OnWine() && !cfg.eacHosts && !HostsBlocksEac()) {
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


// --- Launcher window (issue #20) -----------------------------------------------------------
// Double-clicking sc-offline.exe (no arguments, its own console) opens this window instead of the
// console run. Every button runs `sc-offline.exe <command>` as a hidden child with SC_OFFLINE_GUI
// set, so the window and the CLI share one code path: the child's output is streamed into the box,
// and its [y/N] questions are answered by the Yes/No buttons through its stdin.

enum { kIdPlay = 101, kIdStatus, kIdUpdate, kIdInstall, kIdUninstall, kIdSettings, kIdLogs, kIdYes, kIdNo,
       kIdOutput, kIdLight, kIdLightText, kIdPrompt };
enum { kMsgOutput = WM_APP + 1, kMsgDone, kTimerLight = 1 };

struct Gui {
    HWND wnd = nullptr, out = nullptr, light = nullptr, lightText = nullptr, prompt = nullptr, yes = nullptr, no = nullptr;
    HWND buttons[5] = {};
    HANDLE child = nullptr, childIn = nullptr;
    std::wstring tail;           // the end of the output, to spot a [y/N] question
    std::wstring running;        // command being run, "" when idle
    int state = 0;               // 0 grey (unknown / game running), 1 green, 2 red
    bool hasLeftovers = false;
    HFONT font = nullptr, mono = nullptr;
    HBRUSH brushes[3] = {};
    wstring here;
};
static Gui g_gui;

// What the light shows. Uses only cheap facts (no drive search): the game folder from
// sc-offline.ini or the one remembered from the last run.
static void RefreshLight() {
    Gui& g = g_gui;
    Config cfg;
    { FILE* saved = g_log; g_log = nullptr; ReadConfig(g.here + L"\\sc-offline.ini", cfg); g_log = saved; }
    wstring bin = cfg.game.empty() ? L"" : ToBin64(cfg.game, cfg.channel);
    if (bin.empty()) { std::vector<Found> f; FromRemembered(g.here, cfg.channel, f); if (!f.empty()) bin = f.front().bin; }
    std::wstring text;
    std::vector<std::wstring> left;
    if (!bin.empty()) {
        const GamePaths p(bin);
        const DllInfo d = Identify(p.dll);
        if (IsFile(p.marker) || (d.present && d.ours)) left.push_back(L"the mod is still in Bin64");
    }
    const std::string pc = PcChangesLeft();
    if (!pc.empty()) {
        for (const FwRule& r : kFwRules) if (Field(pc, r.key) == "added") left.push_back(std::wstring(L"firewall rule \"") + r.name + L"\"");
        if (Field(pc, "hosts") == "added") left.push_back(L"the EAC line in the hosts file");
        if (Field(pc, "eac") == "renamed") left.push_back(L"EasyAntiCheat_EOS.exe renamed to .bak");
    }
    g.hasLeftovers = !left.empty();
    if (GameRunning()) {
        g.state = 0; text = L"Star Citizen is running. When it closes, sc-offline takes the mod out and undoes its PC changes.";
    } else if (!left.empty()) {
        g.state = 2; text = L"NOT safe to go online yet. Click Uninstall to undo: ";
        for (size_t i = 0; i < left.size(); ++i) text += (i ? L"; " : L"") + left[i];
    } else if (bin.empty()) {
        g.state = 0; text = L"Game folder not known yet. Click Status or Play to find it. No PC changes are left over.";
    } else {
        g.state = 1; text = L"Safe to go online: the mod is out of the game folder and no PC changes are left.";
    }
    SetWindowTextW(g.lightText, text.c_str());
    InvalidateRect(g.light, nullptr, TRUE);
    const bool idle = g.running.empty();
    const bool gameUp = g.state == 0 && GameRunning();
    for (int i = 0; i < 5; ++i) {
        bool on = idle && !gameUp;
        if (i == 4) on = on && g.hasLeftovers;   // Uninstall: only with something to undo
        if (i == 1) on = idle;                   // Status: always, it changes nothing
        EnableWindow(g.buttons[i], on);
    }
}

static void AppendOutput(const std::wstring& w) {
    Gui& g = g_gui;
    std::wstring t;
    for (wchar_t c : w) { if (c == L'\r') continue; if (c == L'\n') t += L"\r\n"; else t += c; }
    const int len = GetWindowTextLengthW(g.out);
    if (len > 400000) { SendMessageW(g.out, EM_SETSEL, 0, 100000); SendMessageW(g.out, EM_REPLACESEL, FALSE, (LPARAM)L""); }
    const int end = GetWindowTextLengthW(g.out);
    SendMessageW(g.out, EM_SETSEL, end, end);
    SendMessageW(g.out, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
    SendMessageW(g.out, EM_SCROLLCARET, 0, 0);
    g.tail += w;
    if (g.tail.size() > 600) g.tail.erase(0, g.tail.size() - 600);
    // A question is the last line, ending in "[y/N] " with no newline after it.
    const size_t q = g.tail.rfind(L"[y/N] ");
    const bool asking = q != std::wstring::npos && q + 6 == g.tail.size();
    if (asking) {
        const size_t nl = g.tail.find_last_of(L'\n', q);
        SetWindowTextW(g.prompt, g.tail.substr(nl == std::wstring::npos ? 0 : nl + 1, q - (nl == std::wstring::npos ? 0 : nl + 1)).c_str());
    }
    ShowWindow(g.prompt, asking ? SW_SHOW : SW_HIDE);
    ShowWindow(g.yes, asking ? SW_SHOW : SW_HIDE);
    ShowWindow(g.no, asking ? SW_SHOW : SW_HIDE);
    if (asking) SetFocus(g.no);
}

struct ReaderArgs { HANDLE pipe, proc; HWND wnd; };
static DWORD WINAPI ReaderThread(void* p) {
    ReaderArgs a = *(ReaderArgs*)p; delete (ReaderArgs*)p;
    char buf[4096]; DWORD got = 0; std::string pending;
    while (ReadFile(a.pipe, buf, sizeof(buf), &got, nullptr) && got) {
        pending.append(buf, got);
        // Hand over whole UTF-8 sequences only.
        size_t cut = pending.size();
        while (cut > 0 && cut > pending.size() - 4 && (pending[cut - 1] & 0xC0) == 0x80) --cut;
        if (cut > 0 && (unsigned char)pending[cut - 1] >= 0xC0) --cut;
        if (!cut) continue;
        const std::wstring w = Wide(pending.substr(0, cut));
        pending.erase(0, cut);
        PostMessageW(a.wnd, kMsgOutput, 0, (LPARAM)new std::wstring(w));
    }
    if (!pending.empty()) PostMessageW(a.wnd, kMsgOutput, 0, (LPARAM)new std::wstring(Wide(pending)));
    CloseHandle(a.pipe);
    WaitForSingleObject(a.proc, INFINITE);
    DWORD code = 1; GetExitCodeProcess(a.proc, &code);
    PostMessageW(a.wnd, kMsgDone, code, 0);
    return 0;
}

static void RunCommand(const wchar_t* cmd) {
    Gui& g = g_gui;
    if (!g.running.empty()) return;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE outR, outW, inR, inW;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return;
    if (!CreatePipe(&inR, &inW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); return; }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    wchar_t self[MAX_PATH * 2]; GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    wstring line = L"\"" + wstring(self) + L"\" " + cmd;
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = outW;
    PROCESS_INFORMATION pi{};
    SetEnvironmentVariableW(L"SC_OFFLINE_GUI", L"1");
    const BOOL ok = CreateProcessW(self, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g.here.c_str(), &si, &pi);
    SetEnvironmentVariableW(L"SC_OFFLINE_GUI", nullptr);
    CloseHandle(outW); CloseHandle(inR);
    if (!ok) { CloseHandle(outR); CloseHandle(inW); AppendOutput(L"[!] couldn't start sc-offline.exe " + wstring(cmd) + L"\n"); return; }
    CloseHandle(pi.hThread);
    g.child = pi.hProcess; g.childIn = inW; g.running = cmd; g.tail.clear();
    SetWindowTextW(g.out, L"");
    AppendOutput(L"> sc-offline.exe " + wstring(cmd) + L"\n");
    RefreshLight();
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
    CloseHandle(CreateThread(nullptr, 0, ReaderThread, new ReaderArgs{ outR, dup, g.wnd }, 0, nullptr));
}

static void Answer(bool yes) {
    Gui& g = g_gui;
    if (!g.childIn) return;
    const char* a = yes ? "y\n" : "n\n"; DWORD put = 0;
    WriteFile(g.childIn, a, 2, &put, nullptr);
    AppendOutput(yes ? L"y\n" : L"n\n");
}

static void Layout(HWND wnd) {
    Gui& g = g_gui;
    RECT r; GetClientRect(wnd, &r);
    const int W = r.right, H = r.bottom, m = 12, bh = 34;
    MoveWindow(g.light, m, m, 18, 18, TRUE);
    MoveWindow(g.lightText, m + 28, m - 2, W - 2 * m - 28, 40, TRUE);
    int x = m; const int y = m + 46;
    const int widths[] = { 90, 80, 80, 80, 90 };
    for (int i = 0; i < 5; ++i) { MoveWindow(g.buttons[i], x, y, widths[i], bh, TRUE); x += widths[i] + 6; }
    MoveWindow(GetDlgItem(wnd, kIdLogs), W - m - 100, y, 100, bh, TRUE);
    MoveWindow(GetDlgItem(wnd, kIdSettings), W - m - 100 - 6 - 110, y, 110, bh, TRUE);
    const int qy = H - m - bh;
    MoveWindow(g.prompt, m, qy + 7, W - 2 * m - 190, 24, TRUE);
    MoveWindow(g.yes, W - m - 180, qy, 85, bh, TRUE);
    MoveWindow(g.no, W - m - 90, qy, 90, bh, TRUE);
    MoveWindow(g.out, m, y + bh + 10, W - 2 * m, qy - (y + bh + 10) - 8, TRUE);
}

static LRESULT CALLBACK GuiProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    Gui& g = g_gui;
    switch (msg) {
    case WM_CREATE: {
        g.wnd = wnd;
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
            return h;
        };
        g.light = mk(L"STATIC", L"", SS_NOTIFY, kIdLight);
        g.lightText = mk(L"STATIC", L"", 0, kIdLightText);
        const wchar_t* names[] = { L"\u25B6  Play", L"Status", L"Update", L"Install", L"Uninstall" };
        for (int i = 0; i < 5; ++i) g.buttons[i] = mk(L"BUTTON", names[i], BS_PUSHBUTTON | WS_TABSTOP, kIdPlay + i);
        SendMessageW(g.buttons[0], BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        mk(L"BUTTON", L"Open settings", BS_PUSHBUTTON | WS_TABSTOP, kIdSettings);
        mk(L"BUTTON", L"Open logs", BS_PUSHBUTTON | WS_TABSTOP, kIdLogs);
        g.out = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, 10, 10, wnd, (HMENU)(INT_PTR)kIdOutput, nullptr, nullptr);
        SendMessageW(g.out, WM_SETFONT, (WPARAM)g.mono, TRUE);
        SendMessageW(g.out, EM_SETLIMITTEXT, 0, 0);
        g.prompt = mk(L"STATIC", L"", 0, kIdPrompt);
        g.yes = mk(L"BUTTON", L"Yes", BS_PUSHBUTTON | WS_TABSTOP, kIdYes);
        g.no = mk(L"BUTTON", L"No", BS_PUSHBUTTON | WS_TABSTOP, kIdNo);
        ShowWindow(g.prompt, SW_HIDE); ShowWindow(g.yes, SW_HIDE); ShowWindow(g.no, SW_HIDE);
        SetWindowTextW(g.out, (L"sc-offline " + Wide(SCO_VERSION) + L" - Star Citizen offline mod\r\n\r\n"
            L"Play       add the mod, start the game, take the mod out when it closes\r\n"
            L"Status     check the setup; changes nothing\r\n"
            L"Update     check GitHub for a newer sc-offline\r\n"
            L"Install    add the mod and leave it (Uninstall before going online)\r\n"
            L"Uninstall  take the mod out and undo the PC changes\r\n\r\n"
            L"The light above tells you whether it is safe to go back online.\r\n").c_str());
        SetTimer(wnd, kTimerLight, 3000, nullptr);
        RefreshLight();
        return 0;
    }
    case WM_SIZE: Layout(wnd); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = { 720, 420 }; return 0;
    case WM_TIMER: if (g.running.empty()) RefreshLight(); return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g.light) {
            static const COLORREF c[3] = { RGB(150, 150, 150), RGB(40, 170, 70), RGB(210, 50, 50) };
            if (!g.brushes[0]) for (int i = 0; i < 3; ++i) g.brushes[i] = CreateSolidBrush(c[i]);
            return (LRESULT)g.brushes[g.state];
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdPlay: RunCommand(L"play"); return 0;
        case kIdStatus: RunCommand(L"status"); return 0;
        case kIdUpdate: RunCommand(L"update"); return 0;
        case kIdInstall: RunCommand(L"install"); return 0;
        case kIdUninstall: RunCommand(L"uninstall"); return 0;
        case kIdYes: Answer(true); return 0;
        case kIdNo: Answer(false); return 0;
        case kIdSettings: ShellExecuteW(wnd, L"open", L"notepad.exe", (L"\"" + g.here + L"\\sc-offline.ini\"").c_str(), nullptr, SW_SHOWNORMAL); return 0;
        case kIdLogs: { const wstring d = g.here + L"\\data"; CreateDirectoryW(d.c_str(), nullptr);
                        ShellExecuteW(wnd, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0; }
        }
        break;
    case kMsgOutput: { std::wstring* w = (std::wstring*)lp; AppendOutput(*w); delete w; return 0; }
    case kMsgDone: {
        const DWORD code = (DWORD)wp;
        if (g.child) { CloseHandle(g.child); g.child = nullptr; }
        if (g.childIn) { CloseHandle(g.childIn); g.childIn = nullptr; }
        ShowWindow(g.prompt, SW_HIDE); ShowWindow(g.yes, SW_HIDE); ShowWindow(g.no, SW_HIDE);
        g.running.clear();
        if (code == kExitRestart) {
            wchar_t self[MAX_PATH * 2]; GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
            ShellExecuteW(nullptr, L"open", self, nullptr, g.here.c_str(), SW_SHOWNORMAL);
            DestroyWindow(wnd);
            return 0;
        }
        AppendOutput(code == 0 ? L"\n[done]\n" : L"\n[finished with exit code " + std::to_wstring(code) + L"; see above]\n");
        RefreshLight();
        return 0;
    }
    case WM_CLOSE:
        if (!g.running.empty() &&
            MessageBoxW(wnd, L"A command is still running. If you close this window, sc-offline still takes the mod out and undoes "
                             L"its PC changes when the game closes, but any question it asks is answered No.\n\nClose anyway?",
                        L"sc-offline", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return 0;
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

static int RunGui() {
    g_gui.here = ExeDir();
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_gui.font = CreateFontIndirectW(&ncm.lfMessageFont);
    g_gui.mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    WNDCLASSW wc{};
    wc.lpfnWndProc = GuiProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"sc-offline";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    const wstring title = L"sc-offline " + Wide(SCO_VERSION) + L" - Star Citizen offline mod";
    HWND wnd = CreateWindowExW(0, L"sc-offline", title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 820, 560,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) return kExitError;
    ShowWindow(wnd, SW_SHOWNORMAL);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(wnd, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return kExitOk;
}

static const char* kUsage =
    "usage: sc-offline.exe [command] [--game <folder>] [--dry-run] [--skip-eac-check]\n"
    "\n"
    "  play       (default) copy the mod in, start the game, take the mod out when it closes\n"
    "  install    copy the mod in and leave it there; run `uninstall` before going online\n"
    "  uninstall  take the mod out and restore anything it replaced\n"
    "  status     check the setup and report; changes nothing\n"
    "  update     check GitHub for a newer sc-offline and offer to install it (saves and ini kept)\n"
    "  help       show this\n"
    "\n"
    "  --game <folder>   your StarCitizen, LIVE or Bin64 folder (else sc-offline.ini, else searched)\n"
    "  --dry-run         print every step, change nothing, start nothing\n"
    "  --skip-eac-check  don't stop when Easy Anti-Cheat looks active\n"
    "  --console         double-clicked: run `play` in this console instead of opening the window\n"
    "\n"
    "exit codes: 0 ok, 1 error, 2 Easy Anti-Cheat active, 3 the game is running\n";

int wmain(int argc, wchar_t** argv) {
    if (argc == 7 && !_wcsicmp(argv[1], L"--helper"))
        return Helper(argv[2], argv[3], wcstoul(argv[4], nullptr, 10), argv[5], argv[6]);
    // Double-clicked with no arguments: the window (issue #20). From a terminal, or with any
    // argument, the CLI runs exactly as before. `sc-offline.exe --console` forces the console run.
    if (argc == 1 && !GuiChild() && OwnsConsole() && !OnWine()) { FreeConsole(); return RunGui(); }
    if (GuiChild()) { std::setvbuf(stdout, nullptr, _IONBF, 0); SetConsoleOutputCP(CP_UTF8); }
    if (argc == 4 && !_wcsicmp(argv[1], L"--delete-logs"))
        return DeleteSessionLogs(argv[2], _wcstoui64(argv[3], nullptr, 10));
    if (argc == 3 && !_wcsicmp(argv[1], L"--elevated-update")) {
        // The elevated half of ApplyUpdateMaybeElevated: re-asks GitHub itself (never trusts a URL from
        // the command line) and only applies the tag the unelevated launcher showed the player.
        const wstring here = ExeDir();
        OpenLog(here + L"\\data", true, "elevated update");
        const UpdateInfo u = CheckLatest(15000, nullptr);
        if (u.tag.empty() || Wide(u.tag) != argv[2]) { Out("[!] GitHub's latest release changed; run update again\n"); return kExitError; }
        return ApplyUpdate(here, u) ? kExitOk : kExitError;
    }

    wstring command = L"play", gameArg;
    bool dry = false, skipEac = false, sawCommand = false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!_wcsicmp(a, L"--game")) {
            if (i + 1 >= argc) return Fail("--game needs a folder: sc-offline.exe --game \"D:\\Games\\StarCitizen\"");
            gameArg = argv[++i];
        } else if (!_wcsicmp(a, L"--dry-run")) dry = true;
        else if (!_wcsicmp(a, L"--console")) {}
        else if (!_wcsicmp(a, L"--skip-eac-check")) skipEac = true;
        else if (!sawCommand && (!_wcsicmp(a, L"play") || !_wcsicmp(a, L"install") || !_wcsicmp(a, L"uninstall") ||
                                 !_wcsicmp(a, L"status") || !_wcsicmp(a, L"update") || !_wcsicmp(a, L"help"))) {
            command = a; sawCommand = true;
        } else if (!_wcsicmp(a, L"--help") || !_wcsicmp(a, L"-h") || !_wcsicmp(a, L"/?")) {
            command = L"help"; sawCommand = true;
        } else {
            std::printf("%s", kUsage);
            return Fail("unknown argument '%ls'", a);
        }
    }
    for (wchar_t& c : command) c = towlower(c);
    if (command == L"help") { std::printf("sc-offline launcher %s (%s)\n\n%s", SCO_VERSION, SCO_BASED_ON, kUsage); return kExitOk; }

    const wstring here = ExeDir();
    const wstring data = here + L"\\data";
    std::string header = "sc-offline.exe";
    for (int i = 1; i < argc; ++i) header += " " + Narrow(argv[i]);
    OpenLog(data, false, header.c_str());
    Out("sc-offline launcher %s%s\n\n", SCO_VERSION, dry ? " - dry run, nothing is changed" : "");

    if (!CanWriteTo(data))
        Out("[!] this folder (%ls) can't be written without administrator rights, so the mod can't save\n"
            "    your wallet or places. Move the sc-offline folder out of Program Files, e.g. to your Desktop.\n", here.c_str());
    if (!dry) SettlePreviousUpdate(here);
    Config cfg;
    if (!ReadConfig(here + L"\\sc-offline.ini", cfg)) Out("[i] no sc-offline.ini next to this exe; using defaults\n");

    // 0. Updates (issue #14). Never blocks play: no network, a timeout or any odd reply is just skipped.
    if (command == L"update" || (cfg.checkUpdates && !dry && (command == L"play" || command == L"status"))) {
        const bool explicitUpdate = command == L"update";
        std::string why;
        const UpdateInfo u = CheckLatest(explicitUpdate ? 15000 : 3000, &why);
        if (u.tag.empty()) {
            if (explicitUpdate) { Out("Update:   %s\n", why.c_str()); return why.rfind("up to date", 0) == 0 ? kExitOk : kExitError; }
        } else {
            Out("Update:   sc-offline %s is available (you have %s)\n", u.tag.c_str(), SCO_VERSION);
            if (command == L"status") Out("          run `sc-offline.exe update` to install it\n");
            else if (dry) Out("[dry-run] would download %s, check its SHA-256 and replace the program files\n", u.zipName.c_str());
            else if (GameRunning()) Out("[!] close the game before updating\n");
            else if (!OwnsConsoleInput()) Out("          (not asking: no console to answer in)\n");
            else if (AskYes("Update now? Your saves and sc-offline.ini are kept.")) {
                if (ApplyUpdateMaybeElevated(here, u)) {
                    if (GuiChild()) { Out("[i] restarting sc-offline with the new version\n"); return kExitRestart; }
                    Out("[i] starting the new version\n\n");
                    if (g_log) { std::fclose(g_log); g_log = nullptr; }
                    Relaunch(here);
                }
                if (explicitUpdate) return Fail("update failed; your current version is unchanged");
            } else Out("[i] not updating\n");
        }
        if (explicitUpdate) return kExitOk;
        Out("\n");
    }

    // 1. Find the game.
    wstring bin;
    if (!gameArg.empty()) {
        bin = ToBin64(gameArg, cfg.channel);
        if (bin.empty()) return Fail("no %ls under --game %ls (channel %ls)", kGameExe, gameArg.c_str(), cfg.channel.c_str());
    } else if (!cfg.game.empty()) {
        bin = ToBin64(cfg.game, cfg.channel);
        if (bin.empty()) return Fail("no %ls under game = %ls in sc-offline.ini (channel %ls)", kGameExe, cfg.game.c_str(), cfg.channel.c_str());
    } else {
        const std::vector<Found> found = DetectBin64(here, cfg.channel);
        if (!found.empty()) {
            bin = found.front().bin;
            Out("[i] found the game %s\n", found.front().how);
            for (size_t i = 1; i < found.size(); ++i)
                Out("[i] also found %ls (set game = in sc-offline.ini to use it)\n", found[i].bin.c_str());
        } else if (OwnsConsole()) {
            Out("[i] couldn't find Star Citizen (%ls) by itself. Pick your StarCitizen folder in the window that opens.\n",
                cfg.channel.c_str());
            const wstring picked = PickFolder();
            bin = picked.empty() ? L"" : ToBin64(picked, cfg.channel);
            if (bin.empty())
                return Fail("no %ls under %ls (channel %ls). Open sc-offline.ini and set\n"
                            "    game = <your StarCitizen folder>", kGameExe,
                            picked.empty() ? L"(nothing picked)" : picked.c_str(), cfg.channel.c_str());
        } else {
            return Fail("couldn't find Star Citizen (%ls). Open sc-offline.ini and set\n"
                        "    game = <your StarCitizen folder>\n"
                        "or pass --game <folder>", cfg.channel.c_str());
        }
        // Remember it, so the next run skips the search. game = and --game always win over this.
        if (!dry) WriteAll(RememberedPathFile(here), Narrow(bin) + "\n");
    }
    Out("Folder:   %ls\n", bin.c_str());
    const GamePaths g(bin);

    PcWanted pc;
    pc.firewall = cfg.firewall; pc.eacHosts = cfg.eacHosts; pc.eacRename = cfg.eacRename;

    // 2. Self-checks.
    const CheckResult checks = SelfChecks(here, cfg, g);
    Out("\n");
    if (command == L"status") return kExitOk;

    const bool running = GameRunning();
    if (command == L"uninstall") {
        if (running) return FailCode(kExitRunning, "%ls is running. Close it first.", kGameExe);
        if (dry) return (TakeMod(g, true) | UndoPcChanges(true)) ? kExitError : kExitOk;
    } else {
        if (!IsFile(here + L"\\dinput8.dll"))
            return Fail("dinput8.dll is missing next to sc-offline.exe.\n    Extract the whole zip into one folder and run this from there.");
        if (running) return FailCode(kExitRunning, "%ls is already running. Close it first.", kGameExe);
        if (checks.pcLeftover && !dry && command == L"play") {
            if (AskYes("Undo them now and stop, instead of playing?")) { Out("[i] undoing them (same as `sc-offline.exe uninstall`)\n"); command = L"uninstall"; }
            else Out("[i] playing; they are undone together with this session's changes when the game closes.\n");
        }
        const bool playHandlesEac = command == L"play" && cfg.eacRename && !OnWine();
        if (checks.eacActive && !skipEac && !playHandlesEac)
            return FailCode(kExitEac, "stopped: Easy Anti-Cheat is active (see above). --skip-eac-check overrides this.");
    }

    // 3. What the mod reads (the SC_OFFLINE_* variables, see docs/data-files.md).
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
        if (play && NeedsPcChanges(pc)) ApplyPcChanges(pc, bin + L"\\" + kGameExe, true);
        if (command == L"uninstall") UndoPcChanges(true);
        if (PutMod(here, g, play ? "play" : "install", true)) return kExitError;
        if (play) {
            Out("[dry-run] would start %ls\\%ls\n", bin.c_str(), kGameExe);
            Out("[dry-run] after the game closes: take the mod out, put back what was set aside, undo the PC changes\n");
            if (cfg.cleanLogs) Out("[dry-run] then list this session's Game.log, logbackups and crash files and ask (twice) before deleting them\n");
        }
        return kExitOk;
    }
    const wstring eventName = L"Local\\sc-offline-ready-" + std::to_wstring(GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    HANDLE helper = ready ? StartHelper(command.c_str(), g, eventName, pc) : nullptr;
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
    if (NeedsPcChanges(pc))
        Out("PC:       %s%s%s(undone when the game closes)\n", pc.firewall ? "game, RSI Launcher and CrashHandler network blocked, " : "",
            pc.eacHosts ? "EAC hosts line, " : "", pc.eacRename ? "EAC renamed " : "");
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    // 5. Play. The game runs with this window's rights, never the helper's.
    const wstring exe = bin + L"\\" + kGameExe;
    wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    int rc = kExitOk;
    FILETIME startFt; GetSystemTimeAsFileTime(&startFt);
    const ULONGLONG sessionStart = FileTimeU64(startFt) - 2ull * 10000000ull;   // 2 s of clock slack
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, bin.c_str(), &si, &pi)) {
        Out("[!] couldn't start %ls (error %lu)\n", exe.c_str(), GetLastError());
        rc = kExitError;
    } else {
        if (!checks.gameBuild.empty()) WriteAll(data + L"\\game-build.txt", checks.gameBuild + "\n");
        Out("\nPlaying. When the game closes, the mod is removed from Bin64.\n");
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        while (GameRunning()) Sleep(2000);   // the game can hand over to a second StarCitizen.exe
        if (cfg.crashReports && OwnsConsoleInput()) OfferCrashReport(here, ParentDir(bin), sessionStart, checks.gameBuild);
        if (cfg.cleanLogs && OwnsConsoleInput()) OfferToCleanLogs(ParentDir(bin), sessionStart);
        else if (cfg.cleanLogs) Out("Logs:     not asking (no console to answer in); run from a window to be asked\n");
    }
    CloseHandle(helper);
    Out("Mod:      the helper removes it from Bin64 and undoes the PC changes as this window closes.\n");
    if (rc) PauseIfOwnConsole();
    return rc;
}
