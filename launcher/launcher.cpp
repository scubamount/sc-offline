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
#include <windowsx.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <winsafer.h>
#include <commctrl.h>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <share.h>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include "../src/version.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "../src/third_party/imgui/imgui.h"
#include "../src/third_party/imgui/imgui_impl_win32.h"
#include "../src/third_party/imgui/imgui_impl_dx11.h"
#include "../src/third_party/imgui/imgui_internal.h"
// The plugins page edits a plugin's [settings] in its sco.storage database (sco-core's vendored SQLite).
#include "../external/sco-core/third_party/sqlite/sqlite3.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
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
    // Which releases the update check offers: stable (full releases) or prerelease (issue #31, S5).
    bool prereleases = false;
    // After a crash, offer a redacted log bundle and a prefilled bug form (issue #18).
    bool crashReports = true;
    // discord_presence = off from 0.7.4 and older: the Discord plugin (data\plugins\discord) is
    // turned off once, then the line is dropped. The status itself is that plugin's now.
    bool discordOff = false;
    // Load plugins from data\plugins (sco-core's plugin loader). On by default.
    bool plugins = true;
    // The Multiplayer tab (co-presence over sco-core's sco.net): on by default, but nothing networks
    // until the player presses Host or Join. multiplayerAllow: extra IPv4 ranges (a VPN), checked.
    bool multiplayer = true;
    wstring multiplayerAllow;
    // Ship terminals (ASOP), personal hangars, the hangar lift and ATC hails in the mod. On by default.
    bool asop = true;
    // Which ship list the terminals show with asop on: ships (ships.txt) or game (the game's own,
    // empty offline). ships when the ini doesn't say, so an older ini gets a usable terminal too.
    wstring asopFleetList = L"ships";
    // Natural mining (the mining built-in, on sco-core's game.mining row): off until it has been tested
    // in game. mining_debug adds its counters to mod.log.
    bool mining = false;
    bool miningDebug = false;
};

static bool ParseOnOff(const wstring& v, bool& out) {
    if (!_wcsicmp(v.c_str(), L"1") || !_wcsicmp(v.c_str(), L"on") || !_wcsicmp(v.c_str(), L"yes") || !_wcsicmp(v.c_str(), L"true")) { out = true; return true; }
    if (!_wcsicmp(v.c_str(), L"0") || !_wcsicmp(v.c_str(), L"off") || !_wcsicmp(v.c_str(), L"no") || !_wcsicmp(v.c_str(), L"false")) { out = false; return true; }
    return false;
}

// multiplayer_allow: up to 8 IPv4 ranges "a.b.c.d/n" (n 8 to 32, no host bits set), separated by
// commas. Fills the ranges and the same list rewritten from the numbers; false for anything else.
struct Ipv4Range { uint32_t first, last; };
static bool ParseAllowList(const wstring& v, std::vector<Ipv4Range>& out, wstring& text) {
    out.clear();
    text.clear();
    for (size_t pos = 0; pos < v.size();) {
        size_t comma = v.find(L',', pos);
        if (comma == wstring::npos) comma = v.size();
        const wstring item = Trim(v.substr(pos, comma - pos));
        pos = comma + 1;
        unsigned a = 0, b = 0, c = 0, d = 0, n = 0;
        wchar_t extra = 0;
        if (item.empty() || !iswdigit(item[0]) ||
            swscanf_s(item.c_str(), L"%u.%u.%u.%u/%u%c", &a, &b, &c, &d, &n, &extra, 1u) != 5)
            return false;
        if (a > 255 || b > 255 || c > 255 || d > 255 || n < 8 || n > 32 || out.size() >= 8) return false;
        const uint32_t ip = (a << 24) | (b << 16) | (c << 8) | d;
        const uint32_t mask = n == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> n);
        if (ip & ~mask) return false;
        out.push_back({ ip, ip | ~mask });
        wchar_t one[24];
        swprintf_s(one, L"%u.%u.%u.%u/%u", a, b, c, d, n);
        if (!text.empty()) text += L',';
        text += one;
    }
    return true;
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
        else if (!_wcsicmp(k.c_str(), L"discord_presence")) {
            bool on = true;
            if (ParseOnOff(v, on)) c.discordOff = !on;
        }
        else if (!_wcsicmp(k.c_str(), L"plugins")) {
            if (!ParseOnOff(v, c.plugins)) Out("[!] sc-offline.ini line %d: plugins must be on or off\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"multiplayer")) {
            if (!ParseOnOff(v, c.multiplayer)) Out("[!] sc-offline.ini line %d: multiplayer must be on or off\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"multiplayer_allow")) {
            std::vector<Ipv4Range> ranges;
            if (!ParseAllowList(v, ranges, c.multiplayerAllow)) {
                c.multiplayerAllow.clear();
                Out("[!] sc-offline.ini line %d: multiplayer_allow must be up to 8 IPv4 ranges like 100.64.0.0/10 (/8 or narrower); ignored\n", lineNo);
            }
        }
        else if (!_wcsicmp(k.c_str(), L"asop")) {
            if (!ParseOnOff(v, c.asop)) Out("[!] sc-offline.ini line %d: asop must be on or off\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"asop_fleet_list")) {
            if (!_wcsicmp(v.c_str(), L"game") || !_wcsicmp(v.c_str(), L"ships")) c.asopFleetList = v;
            else Out("[!] sc-offline.ini line %d: asop_fleet_list must be game or ships\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"mining")) {
            if (!ParseOnOff(v, c.mining)) Out("[!] sc-offline.ini line %d: mining must be on or off\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"mining_debug")) {
            if (!ParseOnOff(v, c.miningDebug)) Out("[!] sc-offline.ini line %d: mining_debug must be on or off\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"check_updates")) {
            if (!ParseOnOff(v, c.checkUpdates)) Out("[!] sc-offline.ini line %d: check_updates must be on or off\n", lineNo);
        }
        else if (!_wcsicmp(k.c_str(), L"update_channel")) {
            if (!_wcsicmp(v.c_str(), L"stable")) c.prereleases = false;
            else if (!_wcsicmp(v.c_str(), L"prerelease")) c.prereleases = true;
            else Out("[!] sc-offline.ini line %d: update_channel must be stable or prerelease\n", lineNo);
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

static wstring Wide(const std::string& s) {
    wstring w(s.size(), L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size()));
    return w;
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

// --- Self-update (issues #14, #31) ----------------------------------------------------------
// Ask GitHub for the newest release on the player's update_channel; if it is newer, offer to
// download the release zip and check it against the SHA-256 digest GitHub publishes for the asset.
// The zip's manifest.json then pins every file (path, size, SHA-256), its version must equal the
// tag and be newer than this launcher, and only listed files are copied. Download and checks run
// with normal rights and stage the files in data\update\staged; the part that may run as
// administrator only re-checks the staged files and swaps them in. Each file is written as
// .update-new, flushed, moved into place with write-through and re-hashed; the swap is journaled
// in data\update\applied.txt before each step, so a crash rolls back on the next run, and the new
// launcher must pass --self-test or the old files go back. Files the player owns are never
// replaced: sc-offline.ini is kept (new keys are appended to it, commented out), and anything in
// data\ that the release doesn't ship (wallet, saved places, bookmarks, logs) is left alone.

static const wchar_t* kReleasesApi = L"https://api.github.com/repos/scubamount/sc-offline/releases/latest";
static const wchar_t* kReleasesList = L"https://api.github.com/repos/scubamount/sc-offline/releases?per_page=20";

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
    // Each timeout covers one step (resolve, connect, send, one read), so a long download that keeps
    // moving never times out; only a stall does.
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

// "v0.4.2" / "0.4.2" -> {0,4,2,0}. False on anything that isn't 1-4 dot-separated numbers;
// ParseTagVersion handles a "-rc1" suffix.
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

struct UpdateInfo { std::string tag, zipName, zipUrl, sha256; unsigned long long size = 0; };

// Skips the JSON object or array starting at j[i]; returns the index just past it.
static size_t SkipJsonValue(const std::string& j, size_t i) {
    int depth = 0; bool str = false;
    for (; i < j.size(); ++i) {
        const char c = j[i];
        if (str) { if (c == '\\') ++i; else if (c == '"') str = false; continue; }
        if (c == '"') str = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { if (--depth == 0) return i + 1; }
    }
    return j.size();
}

// The release object at body[top]: its tag and the sc-offline-<tag>.zip asset with a SHA-256 digest.
static bool ReleaseAsset(const std::string& body, size_t top, UpdateInfo& u, std::string& why) {
    std::string tag, raw; size_t at = 0;
    if (!JsonKey(body, top, "tag_name", tag)) { why = "unexpected reply from GitHub"; return false; }
    const std::string want = "sc-offline-" + tag + ".zip";
    if (!JsonKey(body, top, "assets", raw, &at) || at >= body.size() || body[at] != '[') { why = tag + " has no assets"; return false; }
    for (size_t i = at + 1; i < body.size();) {
        while (i < body.size() && (isspace((unsigned char)body[i]) || body[i] == ',')) ++i;
        if (i >= body.size() || body[i] != '{') break;
        std::string name, url, digest;
        JsonKey(body, i, "name", name);
        if (name == want && JsonKey(body, i, "browser_download_url", url) && JsonKey(body, i, "digest", digest) &&
            !digest.compare(0, 7, "sha256:") && digest.size() == 7 + 64) {
            u.tag = tag; u.zipName = name; u.zipUrl = url; u.sha256 = digest.substr(7);
            std::string size;
            if (JsonKey(body, i, "size", size) && !size.empty() && size.size() < 16 && size.find_first_not_of("0123456789") == std::string::npos)
                u.size = std::stoull(size);
            for (char& c : u.sha256) c = (char)tolower((unsigned char)c);
            return true;
        }
        i = SkipJsonValue(body, i);
    }
    why = tag + " has no " + want + " with a SHA-256 digest";
    return false;
}

static bool VersionGreater(const std::string& a, const std::string& b);

// S5: what GitHub offers on the player's channel. stable = the latest full release; prerelease =
// the newest release of any kind (drafts never). Empty tag when there's nothing newer to offer.
static UpdateInfo CheckLatest(DWORD timeoutMs, bool prerelease, std::string* why) {
    UpdateInfo u;
    std::string body, w;
    const wstring url = prerelease ? wstring(kReleasesList) : wstring(kReleasesApi);
    if (!HttpGet(url, body, timeoutMs, 4u << 20)) { if (why) *why = "couldn't reach GitHub"; return u; }
    // One release object (stable) or an array of them (prerelease); take the newest acceptable one.
    std::vector<size_t> tops;
    size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { if (why) *why = "unexpected reply from GitHub"; return u; }
    if (body[first] == '{') tops.push_back(first);
    else if (body[first] == '[')
        for (size_t i = first + 1; i < body.size();) {
            while (i < body.size() && (isspace((unsigned char)body[i]) || body[i] == ',')) ++i;
            if (i >= body.size() || body[i] != '{') break;
            tops.push_back(i);
            i = SkipJsonValue(body, i);
        }
    std::string best, newestSeen;
    for (size_t top : tops) {
        std::string tag, pre, draft;
        if (!JsonKey(body, top, "tag_name", tag)) continue;
        JsonKey(body, top, "prerelease", pre); JsonKey(body, top, "draft", draft);
        if (draft == "true" || (!prerelease && pre == "true")) { if (tops.size() == 1) w = "latest release is a pre-release"; continue; }
        if (newestSeen.empty() || VersionGreater(tag, newestSeen)) newestSeen = tag;
        if (!VersionGreater(tag, SCO_VERSION)) continue;
        if (!best.empty() && !VersionGreater(tag, best)) continue;
        UpdateInfo cand; std::string candWhy;
        if (ReleaseAsset(body, top, cand, candWhy)) { u = cand; best = tag; }
        else w = candWhy;
    }
    if (u.tag.empty() && why) *why = !w.empty() ? w : newestSeen.empty() ? std::string("no releases found") : "up to date (latest " + newestSeen + ")";
    return u;
}

// --- Release manifest (issue #31, S1-S3) ---------------------------------------------------
// CI writes manifest.json into the release folder (tools/release-manifest.py): format 1, version,
// tag, commit and one {"path","sha256","size"} object per shipped file. The zip itself is still
// checked against GitHub's digest; the manifest then pins every file inside it. Only listed files
// are ever copied, each one re-hashed first.

struct ManifestFile { std::string path; std::string sha256; unsigned long long size = 0; };
struct Manifest { std::string version, tag, commit; std::vector<ManifestFile> files; };

// "v0.7.0-rc1" -> numbers {0,7,0,0} + pre "rc1". False unless 1-4 dot-separated numbers, then
// optionally "-" and [0-9A-Za-z.]+.
static bool ParseTagVersion(std::string v, int num[4], std::string& pre) {
    pre.clear();
    const size_t dash = v.find('-');
    if (dash != std::string::npos) {
        pre = v.substr(dash + 1);
        v.resize(dash);
        if (pre.empty()) return false;
        for (char c : pre) if (!isalnum((unsigned char)c) && c != '.') return false;
    }
    return ParseVersion(v, num);
}

// a > b. A pre-release sorts below the same numbers without one (0.7.0-rc1 < 0.7.0).
static bool VersionGreater(const std::string& a, const std::string& b) {
    int x[4], y[4]; std::string px, py;
    if (!ParseTagVersion(a, x, px) || !ParseTagVersion(b, y, py)) return false;
    for (int i = 0; i < 4; ++i) if (x[i] != y[i]) return x[i] > y[i];
    if (px.empty() != py.empty()) return px.empty();
    return px > py;
}

// S3: a manifest path is relative, '/'-separated, with no '..', '.', empty part, ':' (alternate
// data streams, drive letters), '\\' or control character. Returns the Windows relative path, or
// empty when refused.
static wstring SafeRelPath(const std::string& p) {
    if (p.empty() || p.size() > 400 || p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos)
        return L"";
    for (unsigned char c : p) if (c < 0x20 || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|') return L"";
    size_t from = 0;
    while (true) {
        const size_t slash = p.find('/', from);
        const std::string part = p.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return L"";
        if (slash == std::string::npos) break;
        from = slash + 1;
    }
    wstring w = Wide(p);
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    return w;
}

static bool IsHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// Parses manifest.json. Unknown keys are ignored. False with a reason when anything required is
// missing or malformed.
static bool ParseManifest(const std::string& j, Manifest& m, std::string& why) {
    m = Manifest{};
    const size_t top = j.find('{');
    std::string format;
    if (top == std::string::npos || !JsonKey(j, top, "format", format) || format != "1") { why = "manifest format isn't 1"; return false; }
    if (!JsonKey(j, top, "version", m.version) || !JsonKey(j, top, "tag", m.tag)) { why = "manifest has no version or tag"; return false; }
    JsonKey(j, top, "commit", m.commit);
    std::string raw; size_t at = 0;
    if (!JsonKey(j, top, "files", raw, &at) || at >= j.size() || j[at] != '[') { why = "manifest has no files list"; return false; }
    for (size_t i = at + 1; i < j.size();) {
        while (i < j.size() && (isspace((unsigned char)j[i]) || j[i] == ',')) ++i;
        if (i >= j.size() || j[i] == ']') break;
        if (j[i] != '{') { why = "manifest files list is malformed"; return false; }
        ManifestFile f; std::string size;
        if (!JsonKey(j, i, "path", f.path) || !JsonKey(j, i, "sha256", f.sha256) || !JsonKey(j, i, "size", size)) {
            why = "a manifest entry lacks path, sha256 or size"; return false;
        }
        for (char& c : f.sha256) c = (char)tolower((unsigned char)c);
        if (!IsHex64(f.sha256)) { why = "bad sha256 for " + f.path; return false; }
        if (size.empty() || size.size() > 15 || size.find_first_not_of("0123456789") != std::string::npos) { why = "bad size for " + f.path; return false; }
        f.size = std::stoull(size);
        if (SafeRelPath(f.path).empty()) { why = "refused path in manifest: " + f.path; return false; }
        for (const ManifestFile& o : m.files) if (!_stricmp(o.path.c_str(), f.path.c_str())) { why = "path listed twice: " + f.path; return false; }
        m.files.push_back(f);
        i = SkipJsonValue(j, i);
    }
    if (m.files.empty()) { why = "manifest lists no files"; return false; }
    return true;
}

// S2: the manifest must belong to the release that was asked for, and be newer than this launcher.
static bool ManifestMatchesRelease(const Manifest& m, const std::string& tag, std::string& why) {
    if (m.tag != tag) { why = "manifest tag " + m.tag + " isn't the release tag " + tag; return false; }
    if ("v" + m.version != tag) { why = "manifest version " + m.version + " doesn't match tag " + tag; return false; }
    if (!VersionGreater(m.version, SCO_VERSION)) { why = "manifest version " + m.version + " isn't newer than " + SCO_VERSION; return false; }
    return true;
}

// SHA-256 of a file, streamed (lowercase hex, empty when unreadable).
static std::string FileSha256(const wstring& path, unsigned long long* size = nullptr) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    std::string hex; unsigned long long total = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            std::vector<unsigned char> buf(1 << 20);
            DWORD got = 0; bool ok = true;
            while ((ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) != 0) && got) {
                if (BCryptHashData(h, buf.data(), got, 0) != 0) { ok = false; break; }
                total += got;
            }
            unsigned char digest[32];
            if (ok && BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
                char two[3];
                for (unsigned char c : digest) { std::snprintf(two, sizeof(two), "%02x", c); hex += two; }
            }
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(f);
    if (size) *size = total;
    return hex;
}

// Every manifest file exists under root with the listed size and sha256. Files under root that
// the manifest doesn't list are ignored here and never copied.
static bool VerifyAgainstManifest(const wstring& root, const Manifest& m, std::string& why) {
    for (const ManifestFile& f : m.files) {
        const wstring p = root + L"\\" + SafeRelPath(f.path);
        const DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) { why = f.path + " is missing"; return false; }
        unsigned long long size = 0;
        const std::string got = FileSha256(p, &size);
        if (got != f.sha256 || size != f.size) { why = f.path + " doesn't match the manifest"; return false; }
    }
    return true;
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

// D2: antivirus (Defender scanning a new exe or DLL) and indexers hold files open for a moment.
// Retry sharing / access-denied / lock errors with growing waits instead of giving up at once.
static bool TransientLock(DWORD e) { return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION; }
static const DWORD kLockWaits[] = { 100, 250, 500, 1000, 2000, 4000 };

static bool MoveRetrying(const wstring& from, const wstring& to, DWORD flags) {
    for (int i = 0;; ++i) {
        if (MoveFileExW(from.c_str(), to.c_str(), flags)) return true;
        const DWORD e = GetLastError();
        if (!TransientLock(e) || i == ARRAYSIZE(kLockWaits)) { SetLastError(e); return false; }
        Out("[i] %ls is in use (error %lu, antivirus?); trying again in %lu ms\n", from.c_str(), e, kLockWaits[i]);
        Sleep(kLockWaits[i]);
    }
}

// D1: copies src to dst and flushes dst to disk before returning.
static bool CopyFlushed(const wstring& src, const wstring& dst) {
    std::string bytes;
    if (!ReadAll(src, bytes)) return false;
    HANDLE f = INVALID_HANDLE_VALUE;
    for (int i = 0;; ++i) {
        f = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) break;
        const DWORD e = GetLastError();
        if (!TransientLock(e) || i == ARRAYSIZE(kLockWaits)) { SetLastError(e); return false; }
        Sleep(kLockWaits[i]);
    }
    DWORD put = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size();
    ok = FlushFileBuffers(f) && ok;
    const DWORD e = GetLastError();
    CloseHandle(f);
    SetLastError(e);
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
        DeleteFileW((path + L".update-new").c_str());
        if ((*it)[0] == 'N') DeleteFileW(path.c_str());
        else if ((*it)[0] == 'R' && IsFile(path + L".update-old")) {
            // A running new sc-offline.exe can't be deleted, but it can be renamed out of the way.
            if (IsFile(path) && !DeleteFileW(path.c_str())) MoveRetrying(path, path + L".update-failed", MOVEFILE_REPLACE_EXISTING);
            MoveRetrying(path + L".update-old", path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
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

// S4: the update runs in two parts. StageUpdate (normal rights) downloads, checks the zip against
// GitHub's digest, unpacks it, checks the manifest and copies the listed files into a staging
// folder. SwapStaged (the only part that may run as administrator) reads nothing from the network:
// it re-checks the staged files against the staged manifest and swaps them in.

// Where an update is staged: data\update when this folder is writable, else (a Program Files
// install) %LOCALAPPDATA%\sc-offline\update, which the elevated part re-checks file by file.
static wstring StageRoot(const wstring& here) {
    if (CanWriteTo(here + L"\\data")) return UpdateDir(here);
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, ARRAYSIZE(buf));
    if (!n || n >= ARRAYSIZE(buf)) return UpdateDir(here);
    const wstring root = wstring(buf, n) + L"\\sc-offline";
    CreateDirectoryW(root.c_str(), nullptr);
    return root + L"\\update";
}

// Reads and checks a staged manifest: parses, belongs to `tag`, newer than this launcher (S2).
static bool LoadManifest(const wstring& dir, const std::string& tag, Manifest& man, std::string& why) {
    std::string text;
    if (!ReadAll(dir + L"\\manifest.json", text)) { why = "no manifest.json"; return false; }
    return ParseManifest(text, man, why) && ManifestMatchesRelease(man, tag, why);
}

// Unpacks a downloaded, digest-checked zip and stages exactly the files its manifest lists.
// Refuses a zip holding files the manifest doesn't list (S3).
static bool StageZip(const wstring& root, const wstring& zipPath, const std::string& tag) {
    const wstring ex = root + L"\\new", staged = root + L"\\staged";
    RemoveTree(ex); RemoveTree(staged);
    CreateDirectoryW(ex.c_str(), nullptr);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const wstring tar = wstring(sys) + L"\\tar.exe";
    DWORD code = 1;
    if (!IsFile(tar) || !RunWait(tar, L"\"" + tar + L"\" -xf \"" + zipPath + L"\" -C \"" + ex + L"\"", ex, code) || code) {
        Out("[!] couldn't unpack the zip (Windows' tar.exe %s); nothing changed.\n"
            "    Update by hand: %s\n", IsFile(tar) ? "failed" : "is missing", "https://github.com/scubamount/sc-offline/releases/latest");
        return false;
    }
    const wstring pkg = ex + L"\\sc-offline-" + Wide(tag);
    Manifest man; std::string why;
    if (!LoadManifest(pkg, tag, man, why) || !VerifyAgainstManifest(pkg, man, why)) {
        Out("[!] the release's manifest check failed (%s); nothing changed\n", why.c_str());
        return false;
    }
    for (const wstring& rel : FilesUnder(ex)) {
        const wstring prefix = L"sc-offline-" + Wide(tag) + L"\\";
        const bool inPkg = !_wcsnicmp(rel.c_str(), prefix.c_str(), prefix.size());
        const wstring sub = inPkg ? rel.substr(prefix.size()) : rel;
        bool listed = inPkg && !_wcsicmp(sub.c_str(), L"manifest.json");
        for (const ManifestFile& f : man.files) if (inPkg && !_wcsicmp(SafeRelPath(f.path).c_str(), sub.c_str())) { listed = true; break; }
        if (!listed) { Out("[!] the zip holds %ls, which its manifest doesn't list; nothing changed\n", rel.c_str()); return false; }
    }
    if (!IsFile(pkg + L"\\sc-offline.exe") || !IsFile(pkg + L"\\dinput8.dll")) {
        Out("[!] the release doesn't hold sc-offline.exe and dinput8.dll; nothing changed\n");
        return false;
    }
    CreateDirectoryW(staged.c_str(), nullptr);
    for (const ManifestFile& f : man.files) {
        const wstring rel = SafeRelPath(f.path);
        const size_t cut = rel.find_last_of(L'\\');
        if (cut != wstring::npos) SHCreateDirectoryExW(nullptr, (staged + L"\\" + rel.substr(0, cut)).c_str(), nullptr);
        if (!CopyFileW((pkg + L"\\" + rel).c_str(), (staged + L"\\" + rel).c_str(), FALSE)) {
            Out("[!] couldn't stage %ls (error %lu); nothing changed\n", rel.c_str(), GetLastError()); return false;
        }
    }
    if (!CopyFileW((pkg + L"\\manifest.json").c_str(), (staged + L"\\manifest.json").c_str(), FALSE) ||
        !VerifyAgainstManifest(staged, man, why)) {
        Out("[!] staging failed (%s); nothing changed\n", why.empty() ? "manifest.json" : why.c_str()); return false;
    }
    RemoveTree(ex);
    Out("[+] all %zu files match the release manifest (%s); staged\n", man.files.size(), man.tag.c_str());
    return true;
}

// Downloads the release zip, checks it against GitHub's digest and stages it. Network: normal rights only.
static bool StageUpdate(const wstring& here, const UpdateInfo& u) {
    const wstring root = StageRoot(here);
    RemoveTree(root + L"\\staged"); RemoveTree(root + L"\\new");
    CreateDirectoryW(root.c_str(), nullptr);
    // D3: room for the zip, the unpacked copy and the staged copy, plus the files being replaced.
    ULARGE_INTEGER freeBytes{};
    const unsigned long long need = (u.size ? u.size : (64ull << 20)) * 6 + (32ull << 20);
    if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < need) {
        Out("[!] not enough free disk space for the update (%llu MB free, %llu MB needed); nothing changed\n",
            freeBytes.QuadPart >> 20, need >> 20);
        return false;
    }
    Out("[i] downloading %s ...\n", u.zipName.c_str());
    std::string zip;
    // D3: the timeout is per read (a stalled download), not for the whole file; one retry.
    bool got = HttpGet(Wide(u.zipUrl), zip, 30000, 256u << 20);
    if (!got) { Out("[i] the download stalled or failed; trying once more\n"); got = HttpGet(Wide(u.zipUrl), zip, 30000, 256u << 20); }
    if (!got) { Out("[!] download failed; nothing changed\n"); return false; }
    const std::string digest = Sha256(zip);
    if (digest != u.sha256) {
        Out("[!] the download doesn't match GitHub's SHA-256 (got %s, expected %s); nothing changed\n", digest.c_str(), u.sha256.c_str());
        return false;
    }
    Out("[+] SHA-256 matches GitHub's digest\n");
    const wstring zipPath = root + L"\\" + Wide(u.zipName);
    if (!WriteAll(zipPath, zip)) { Out("[!] couldn't save the download; nothing changed\n"); return false; }
    const bool ok = StageZip(root, zipPath, u.tag);
    DeleteFileW(zipPath.c_str());
    if (!ok) { RemoveTree(root + L"\\staged"); RemoveTree(root + L"\\new"); }
    return ok;
}

// Swaps the staged files in. Re-checks the staged manifest (tag, version) and every file first.
static bool GameRunning();
static bool SelfTestPasses(const wstring& here, const std::string& tag);

static bool SwapStaged(const wstring& here, const wstring& staged, const std::string& tag) {
    // D4: never swap files under a running game (it may hold dinput8.dll, and the player would get
    // a half-updated mod on the next start).
    if (GameRunning()) { Out("[!] %ls is running; close it before updating. Nothing changed\n", kGameExe); return false; }
    Manifest man; std::string why;
    if (!LoadManifest(staged, tag, man, why) || !VerifyAgainstManifest(staged, man, why)) {
        Out("[!] the staged update doesn't match its manifest (%s); nothing changed\n", why.c_str());
        return false;
    }
    CreateDirectoryW((here + L"\\data").c_str(), nullptr);
    CreateDirectoryW(UpdateDir(here).c_str(), nullptr);
    DeleteFileW(Journal(here).c_str());
    // Swap, journaling each step before it happens. Only files the manifest lists are copied.
    int replaced = 0, added = 0, done = 0;
    bool ok = true;
#ifdef SCO_UPDATE_TEST
    // Test hook (D6): SCO_TEST_FAIL_AFTER=N fails the swap after N files, as a crash or a full disk would.
    wchar_t failEnv[16] = {};
    const int failAfter = GetEnvironmentVariableW(L"SCO_TEST_FAIL_AFTER", failEnv, ARRAYSIZE(failEnv)) ? _wtoi(failEnv) : -1;
#endif
    for (const ManifestFile& mf : man.files) {
        const wstring rel = SafeRelPath(mf.path);
        if (PlayerOwned(rel)) continue;
#ifdef SCO_UPDATE_TEST
        if (failAfter >= 0 && done >= failAfter) { Out("[!] SCO_TEST_FAIL_AFTER=%d: failing the swap here\n", failAfter); ok = false; break; }
#endif
        const wstring dst = here + L"\\" + rel, src = staged + L"\\" + rel, fresh = dst + L".update-new";
        const size_t cut = rel.find_last_of(L'\\');
        if (cut != wstring::npos) SHCreateDirectoryExW(nullptr, (here + L"\\" + rel.substr(0, cut)).c_str(), nullptr);
        // D1: write the new file beside its target, flushed to disk, and check it before anything moves.
        if (!CopyFlushed(src, fresh) || FileSha256(fresh) != mf.sha256) {
            Out("[!] couldn't write %ls (error %lu)\n", rel.c_str(), GetLastError()); DeleteFileW(fresh.c_str()); ok = false; break;
        }
        const bool had = IsFile(dst);
        if (!JournalLine(here, std::string(had ? "R " : "N ") + Narrow(rel))) { DeleteFileW(fresh.c_str()); ok = false; break; }
        if (had && !MoveRetrying(dst, dst + L".update-old", MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            Out("[!] couldn't move %ls aside (error %lu)\n", rel.c_str(), GetLastError()); DeleteFileW(fresh.c_str()); ok = false; break;
        }
        if (!MoveRetrying(fresh, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) || FileSha256(dst) != mf.sha256) {
            Out("[!] couldn't put %ls in place (error %lu)\n", rel.c_str(), GetLastError()); DeleteFileW(fresh.c_str()); ok = false; break;
        }
        ++(had ? replaced : added); ++done;
    }
    if (!ok) {
        Out("[!] update failed; putting the old files back\n");
        RollBackUpdate(here);
        DeleteFileW(Journal(here).c_str());
        return false;
    }
    // D5: the new launcher must start. If its --self-test fails, the old files go back; the
    // .update-old copies stay until the next good start (SettlePreviousUpdate) either way.
    if (!SelfTestPasses(here, tag)) {
        Out("[!] the new sc-offline.exe failed its self-test; putting the old files back\n");
        RollBackUpdate(here);
        DeleteFileW(Journal(here).c_str());
        return false;
    }
    if (IsFile(staged + L"\\sc-offline.ini")) MergeIni(here, staged + L"\\sc-offline.ini", tag);
    JournalLine(here, "done");
    Out("[+] updated to %s: %d files replaced, %d added; your saves and sc-offline.ini were kept\n", tag.c_str(), replaced, added);
    return true;
}

// The swap, as administrator when this folder needs it (issue #16): `sc-offline.exe
// --elevated-update <tag> <staged folder>` only re-checks and swaps; it never touches the network,
// starts the game or relaunches, so the new launcher (and the game) still start with normal rights.
static bool SwapMaybeElevated(const wstring& here, const wstring& staged, const std::string& tag) {
    if (CanWriteTo(here) && CanWriteTo(here + L"\\data")) return SwapStaged(here, staged, tag);
    Out("[i] this folder needs administrator rights to update; Windows will ask once.\n"
        "    (Only the file swap runs as administrator; the game never does.)\n");
    wchar_t self[MAX_PATH * 2]; GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    const wstring args = L"--elevated-update " + Wide(tag) + L" \"" + staged + L"\"";
    SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof(sei); sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; sei.lpFile = self; sei.lpParameters = args.c_str(); sei.lpDirectory = here.c_str(); sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) { Out("[!] administrator rights refused; nothing changed\n"); return false; }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code); CloseHandle(sei.hProcess);
    if (code) { Out("[!] the update failed (exit %lu); see data\\launcher.log. Your current version is unchanged.\n", code); return false; }
    return true;
}

// The elevated half: only accepts a staging folder named ...\update\staged.
static int ElevatedUpdate(const wstring& here, const wstring& tag, const wstring& staged) {
    OpenLog(here + L"\\data", true, "elevated update");
    const wstring tail = L"\\update\\staged";
    if (staged.size() <= tail.size() || _wcsicmp(staged.c_str() + staged.size() - tail.size(), tail.c_str())) {
        Out("[!] refusing staging folder %ls\n", staged.c_str()); return kExitError;
    }
    return SwapStaged(here, StripSlashes(staged), Narrow(tag)) ? kExitOk : kExitError;
}

// Swaps a staged update in (as administrator only when needed); the staging folder goes either way.
static bool InstallStaged(const wstring& here, const std::string& tag) {
    const wstring staged = StageRoot(here) + L"\\staged";
    const bool ok = SwapMaybeElevated(here, staged, tag);
    RemoveTree(staged);
    return ok;
}

static bool ApplyUpdate(const wstring& here, const UpdateInfo& u) {
    return StageUpdate(here, u) && InstallStaged(here, u.tag);
}

// D5: runs `<here>\sc-offline.exe --self-test <tag>` and waits up to 30 s. When this process is
// elevated, the new exe still runs with normal rights (a Safer "normal user" token).
static bool SelfTestPasses(const wstring& here, const std::string& tag) {
    const wstring exe = here + L"\\sc-offline.exe";
    wstring cmd = L"\"" + exe + L"\" --self-test " + Wide(tag);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    bool started = false;
    SAFER_LEVEL_HANDLE level = nullptr; HANDLE token = nullptr;
    if (SaferCreateLevel(SAFER_SCOPEID_USER, SAFER_LEVELID_NORMALUSER, SAFER_LEVEL_OPEN, &level, nullptr)) {
        if (SaferComputeTokenFromLevel(level, nullptr, &token, 0, nullptr))
            started = CreateProcessAsUserW(token, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, here.c_str(), &si, &pi) != 0;
        if (token) CloseHandle(token);
        SaferCloseLevel(level);
    }
    if (!started) started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, here.c_str(), &si, &pi) != 0;
    if (!started) { Out("[!] couldn't start the new sc-offline.exe (error %lu)\n", GetLastError()); return false; }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); Out("[!] the new sc-offline.exe hung in its self-test\n"); }
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code) { Out("[!] the new sc-offline.exe --self-test exited %lu\n", code); return false; }
    Out("[+] the new sc-offline.exe passed its self-test\n");
    return true;
}

// --self-test <tag>: what an update runs on the new launcher before keeping it. Checks this exe is
// the version the manifest promised, that SHA-256 works and that dinput8.dll is beside it. Writes
// nothing, reads no network.
static int SelfTest(const std::string& tag) {
    const wstring here = ExeDir();
    if (tag != std::string("v") + SCO_VERSION) { std::printf("self-test: this is %s, expected %s\n", SCO_VERSION, tag.c_str()); return 2; }
    if (Sha256("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") { std::printf("self-test: SHA-256 broken\n"); return 3; }
    if (!IsFile(here + L"\\dinput8.dll")) { std::printf("self-test: no dinput8.dll beside sc-offline.exe\n"); return 4; }
    std::printf("self-test: sc-offline %s ok\n", SCO_VERSION);
    return 0;
}

#ifdef SCO_UPDATE_TEST
// D6 test hook, test builds only: --apply-zip <zip> <tag> <sha256> runs the real stage, verify,
// swap and self-test path against a local zip instead of a download. Exit 0 applied, 1 refused or
// rolled back. SCO_TEST_FAIL_AFTER=N fails the swap after N files.
static int ApplyZipForTest(const wstring& zipPath, const wstring& tagW, const wstring& shaW) {
    const wstring here = ExeDir();
    CreateDirectoryW((here + L"\\data").c_str(), nullptr);
    OpenLog(here + L"\\data", true, "test --apply-zip");
    SettlePreviousUpdate(here);
    const std::string tag = Narrow(tagW);
    std::string sha = Narrow(shaW), zip;
    for (char& c : sha) c = (char)tolower((unsigned char)c);
    if (!ReadAll(zipPath, zip)) { Out("[!] can't read %ls\n", zipPath.c_str()); return kExitError; }
    if (Sha256(zip) != sha) { Out("[!] the zip doesn't match the given SHA-256; nothing changed\n"); return kExitError; }
    const wstring root = StageRoot(here);
    RemoveTree(root + L"\\staged"); RemoveTree(root + L"\\new");
    CreateDirectoryW(root.c_str(), nullptr);
    const wstring copy = root + L"\\sc-offline-" + tagW + L".zip";
    if (!WriteAll(copy, zip)) { Out("[!] couldn't copy the zip\n"); return kExitError; }
    const bool staged = StageZip(root, copy, tag);
    DeleteFileW(copy.c_str());
    if (!staged) { RemoveTree(root + L"\\staged"); RemoveTree(root + L"\\new"); return kExitError; }
    return InstallStaged(here, tag) ? kExitOk : kExitError;
}
#endif

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


static bool SetPluginDisabled(const wstring& here, const wstring& folder, bool disable);

// Removes every uncommented `key = ...` line from sc-offline.ini. True when the file is written.
static bool DropIniKey(const wstring& iniPath, const char* key) {
    std::string text, out;
    if (!ReadAll(iniPath, text)) return false;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos); nl = nl == std::string::npos ? text.size() : nl + 1;
        const std::string line = text.substr(pos, nl - pos);
        pos = nl;
        const size_t b = line.find_first_not_of(" \t");
        if (b != std::string::npos && !_strnicmp(line.c_str() + b, key, strlen(key))) {
            size_t after = b + strlen(key);
            while (after < line.size() && (line[after] == ' ' || line[after] == '\t')) ++after;
            if (after < line.size() && line[after] == '=') continue;
        }
        out += line;
    }
    return WriteAll(iniPath, out);
}

// Sets `key = value` in sc-offline.ini: replaces the first uncommented line for the key, or appends one.
static bool SetIniValue(const wstring& iniPath, const char* key, const char* value) {
    std::string text;
    ReadAll(iniPath, text);
    const std::string line = std::string(key) + " = " + value;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
        const size_t b = text.find_first_not_of(" \t", pos);
        if (b < nl && !_strnicmp(text.c_str() + b, key, strlen(key))) {
            size_t after = b + strlen(key);
            while (after < nl && (text[after] == ' ' || text[after] == '\t')) ++after;
            if (after < nl && text[after] == '=') {
                size_t end = nl; if (end > pos && text[end - 1] == '\r') --end;
                return WriteAll(iniPath, text.substr(0, pos) + line + text.substr(end));
            }
        }
        pos = nl + 1;
    }
    if (!text.empty() && text.back() != '\n') text += "\r\n";
    return WriteAll(iniPath, text + "\r\n" + line + "\r\n");
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
// multiplayer = on: StarCitizen.exe's block rule leaves the LAN (and multiplayer_allow) out, and
// this rule lets other PCs there reach it over UDP, for a session the player hosts.
static const wchar_t* kMpRule       = L"sc-offline: allow StarCitizen.exe multiplayer (LAN)";

struct PcWanted {
    bool firewall = false, eacHosts = false, eacRename = false;
    bool lan = false;      // multiplayer: the LAN (and allow) stay reachable for StarCitizen.exe
    wstring allow;         // multiplayer_allow, as ParseAllowList rewrote it
};

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

// remote: the addresses to block ("" for every address).
static bool AddFirewallRule(const wchar_t* rule, const wstring& exe, const wstring& remote = L"") {
    for (const wchar_t* dir : { L"out", L"in" }) {
        const wstring args = L"advfirewall firewall add rule name=\"" + wstring(rule) + L"\" dir=" + dir +
                             L" action=block enable=yes profile=any program=\"" + exe + L"\"" +
                             (remote.empty() ? L"" : L" remoteip=" + remote);
        if (RunTool(System32(L"netsh.exe"), args) != 0) return false;
    }
    return true;
}

static wstring Ipv4Text(uint32_t ip) {
    wchar_t b[16];
    swprintf_s(b, L"%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
    return b;
}

// Every address except loopback, the private and link-local ranges and multiplayer_allow's: what
// StarCitizen.exe's block rule blocks when multiplayer is on. IPv6 is blocked entirely (sco.net is
// IPv4 only).
static wstring BlockedOutsideLan(const std::vector<Ipv4Range>& allow) {
    std::vector<Ipv4Range> keep = { { 0x0A000000u, 0x0AFFFFFFu }, { 0x7F000000u, 0x7FFFFFFFu }, { 0xA9FE0000u, 0xA9FEFFFFu },
                                    { 0xAC100000u, 0xAC1FFFFFu }, { 0xC0A80000u, 0xC0A8FFFFu } };
    keep.insert(keep.end(), allow.begin(), allow.end());
    std::sort(keep.begin(), keep.end(), [](const Ipv4Range& x, const Ipv4Range& y) { return x.first < y.first; });
    wstring out;
    uint64_t next = 0;
    for (const Ipv4Range& r : keep) {
        if (r.first > next) out += (out.empty() ? L"" : L",") + Ipv4Text(static_cast<uint32_t>(next)) + L"-" + Ipv4Text(r.first - 1);
        if (static_cast<uint64_t>(r.last) + 1 > next) next = static_cast<uint64_t>(r.last) + 1;
    }
    if (next <= 0xFFFFFFFFull) out += (out.empty() ? L"" : L",") + Ipv4Text(static_cast<uint32_t>(next)) + L"-255.255.255.255";
    return out + L",::-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff";
}

// Inbound UDP to StarCitizen.exe from the local subnet (and multiplayer_allow), so other players can
// join a session this PC hosts. sco.net itself refuses anything outside that and anyone without the
// passphrase, and listens only while the player hosts.
static bool AddMultiplayerRule(const wstring& exe, const wstring& allow) {
    const wstring args = L"advfirewall firewall add rule name=\"" + wstring(kMpRule) +
                         L"\" dir=in action=allow enable=yes profile=any protocol=UDP program=\"" + exe +
                         L"\" remoteip=LocalSubnet" + (allow.empty() ? L"" : L"," + allow);
    return RunTool(System32(L"netsh.exe"), args) == 0;
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
static const FwRule kFwRules[] = { { "firewall", kFirewallRule }, { "fw_launcher", kLauncherRule }, { "fw_crash", kCrashRule }, { "fw_mp", kMpRule } };

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
        std::vector<Ipv4Range> allow;
        wstring allowText;
        if (w.lan && !ParseAllowList(w.allow, allow, allowText)) { Out("[!] multiplayer_allow ignored (not a list of IPv4 ranges)\n"); allow.clear(); allowText.clear(); }
        for (size_t i = 0; i < ARRAYSIZE(exes); ++i) {   // kFwRules' first three: the block rules
            const FwRule& r = kFwRules[i];
            if (exes[i].empty()) { if (i == 1) Out("[i] RSI Launcher.exe not found; not blocking it\n"); continue; }
            if (Field(rec, r.key) == "added" && !dry && FirewallRuleExists(r.name)) Out("[i] firewall rule \"%ls\" already in place\n", r.name);
            else if (Step(dry, "add a firewall rule blocking %ls", exes[i].c_str())) {
                const wstring remote = i == 0 && w.lan ? BlockedOutsideLan(allow) : L"";
                bool added = AddFirewallRule(r.name, exes[i], remote);
                if (!added && !remote.empty()) {   // fail closed: block every address, as without multiplayer
                    DeleteFirewallRule(r.name);
                    Out("[!] couldn't leave your LAN open in the firewall rule; blocking every address (multiplayer can't reach other PCs)\n");
                    added = AddFirewallRule(r.name, exes[i]);
                }
                if (!added) { Out("[!] couldn't add the firewall rule (netsh failed)\n"); DeleteFirewallRule(r.name); return 5; }
                record(r.key, "added");
            }
        }
        if (w.lan) {
            if (Field(rec, "fw_mp") == "added" && !dry && FirewallRuleExists(kMpRule)) Out("[i] firewall rule \"%ls\" already in place\n", kMpRule);
            else if (Step(dry, "add a firewall rule letting your LAN%ls reach %ls over UDP (multiplayer)", allowText.empty() ? L"" : L" and multiplayer_allow", gameExe.c_str())) {
                if (AddMultiplayerRule(gameExe, allowText)) record("fw_mp", "added");
                else { DeleteFirewallRule(kMpRule); Out("[!] couldn't add the multiplayer firewall rule; a session you host may not be reachable\n"); }
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
    if (w.lan) f += L'l';
    return f.empty() ? L"-" : f;
}

static PcWanted ParsePcFlags(const wstring& f) {
    PcWanted w;
    w.firewall = f.find(L'f') != wstring::npos;
    w.eacHosts = f.find(L'h') != wstring::npos;
    w.eacRename = f.find(L'e') != wstring::npos;
    w.lan = f.find(L'l') != wstring::npos;
    return w;
}

// --helper <play|install|uninstall> <Bin64> <launcher pid> <event name> <pc flags> [<multiplayer_allow>]
// The part that changes the game folder and the PC; runs elevated when either needs it.
// play: make the PC changes (pc flags: f firewall, h hosts, e EAC rename, l the LAN stays open for
// multiplayer, - none), put the mod in,
// signal the event, wait for the launcher and every StarCitizen.exe to exit, then take the mod
// out and undo the PC changes. install: put in. uninstall: take out and undo leftover PC changes.
// Exit codes: 0 ok, 2 copy failed, 3 removal failed, 4 bad args, 5 PC change failed.
static int Helper(const wstring& op, const wstring& bin, DWORD parentPid, const wstring& eventName, const wstring& flags,
                  const wstring& allow) {
    const wstring here = ExeDir();
    OpenLog(here + L"\\data", true, Narrow(L"helper " + op).c_str());
    const GamePaths g(bin);
    if (!_wcsicmp(op.c_str(), L"uninstall")) { const int a = TakeMod(g, false), b = UndoPcChanges(false); return a ? a : b; }
    const bool play = !_wcsicmp(op.c_str(), L"play");
    if (!play && _wcsicmp(op.c_str(), L"install")) return 4;
    HANDLE parent = play ? OpenProcess(SYNCHRONIZE, FALSE, parentPid) : nullptr;
    if (play && !parent) return 4;
    if (play) {
        PcWanted wanted = ParsePcFlags(flags);
        wanted.allow = allow;
        const int pc = ApplyPcChanges(wanted, bin + L"\\" + kGameExe, false);
        if (pc) { UndoPcChanges(false); CloseHandle(parent); return pc; }
    }
    const int rc = PutMod(here, g, play ? "play" : "install", false);
    if (rc) { if (parent) { UndoPcChanges(false); CloseHandle(parent); } return rc; }
    if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str())) { SetEvent(ready); CloseHandle(ready); }
    if (!play) return 0;

    // Clean up when the launcher says the game closed (before it asks anything), or when the
    // launcher exits, whichever comes first.
    HANDLE closed = OpenEventW(SYNCHRONIZE, FALSE, (eventName + L"-closed").c_str());
    HANDLE waitFor[2] = { parent, closed };
    WaitForMultipleObjects(closed ? 2 : 1, waitFor, FALSE, INFINITE);
    if (closed) CloseHandle(closed);
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
                         L" " + eventName + L" " + PcFlags(pc) + (pc.lan && !pc.allow.empty() ? L" " + pc.allow : L"");
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
    if (!OnWine()) Out("Network:  %s%s\n", cfg.firewall ? "blocked for StarCitizen.exe, RSI Launcher.exe and CrashHandler.exe while you play (block_network)"
                                                         : "NOT blocked (block_network = off)",
                       cfg.firewall && cfg.multiplayer ? "; your LAN stays reachable for the Multiplayer tab (multiplayer)" : "");

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
// console run. It is drawn with ImGui on Direct3D 11 (the vendored src/third_party/imgui, the same
// copy the mod's menu uses). Every command button runs `sc-offline.exe <command>` as a hidden child
// with SC_OFFLINE_GUI set, so the window and the CLI share one code path: the child's output is
// streamed into the Output page, and its [y/N] questions are answered by the Yes/No buttons through
// its stdin. The Plugins page manages data\plugins\<id>\ itself (see the section below).

enum { kMsgOutput = WM_APP + 1, kMsgDone };

enum Action { kActNone, kActPlay, kActStatus, kActUpdate, kActInstall, kActUninstall, kActSettings, kActLogs,
              kActYes, kActNo, kActCopy, kActMinimize, kActClose, kActPluginsDir, kActPluginsOn, kActDiscord };

enum Icon { kIcoPlay, kIcoInstall, kIcoStatus, kIcoUpdate, kIcoHome, kIcoOutput, kIcoSettings, kIcoFolder,
            kIcoPad, kIcoShield, kIcoBolt, kIcoCheck, kIcoMark, kIcoServer, kIcoUsers, kIcoMods, kIcoSearch };

// The home page's list: each row runs `sc-offline.exe <cmd>` as a hidden child (see RunCommand).
struct GuiCommand { const char* label; const wchar_t* cmd; Action act; Icon icon; };
static const GuiCommand kGuiCommands[] = {
    { "Play", L"play", kActPlay, kIcoPlay },
    { "Status", L"status", kActStatus, kIcoStatus },
    { "Update", L"update", kActUpdate, kIcoUpdate },
    { "Install", L"install", kActInstall, kIcoInstall },
    { "Uninstall", L"uninstall", kActUninstall, kIcoShield },
};
static const int kGuiCommandCount = int(ARRAYSIZE(kGuiCommands));

static const ImU32 kUiBg          = IM_COL32(21, 21, 23, 255);
static const ImU32 kUiSide        = IM_COL32(17, 17, 19, 255);
static const ImU32 kUiCard        = IM_COL32(27, 27, 30, 255);
static const ImU32 kUiRow         = IM_COL32(36, 36, 40, 255);
static const ImU32 kUiRowHover    = IM_COL32(46, 46, 51, 255);
static const ImU32 kUiLine        = IM_COL32(40, 40, 44, 255);
static const ImU32 kUiText        = IM_COL32(236, 236, 240, 255);
static const ImU32 kUiTextBody    = IM_COL32(190, 192, 198, 255);
static const ImU32 kUiTextDim     = IM_COL32(118, 118, 126, 255);
static const ImU32 kUiTextFaint   = IM_COL32(74, 74, 82, 255);
static const ImU32 kUiAccent      = IM_COL32(236, 236, 240, 255);
static const ImU32 kUiAction      = IM_COL32(226, 226, 230, 255);
static const ImU32 kUiActionHover = IM_COL32(255, 255, 255, 255);
static const ImU32 kUiActionText  = IM_COL32(14, 14, 16, 255);
static const ImU32 kUiBannerFrom  = IM_COL32(62, 62, 68, 255);
static const ImU32 kUiBannerTo    = IM_COL32(31, 32, 37, 255);
static const ImU32 kUiGreen       = IM_COL32(46, 196, 118, 255);
static const ImU32 kUiRed         = IM_COL32(70, 70, 76, 255);
static const ImU32 kUiWarn        = IM_COL32(255, 132, 104, 255);

static const float kUiW = 720, kUiH = 328, kUiSideW = 72, kUiTop = 46, kUiPad = 18, kUiGap = 14;
static const double kIntroSpin = 0.9, kIntroGrow = 0.42, kIntroFade = 0.3;

// One `[settings]` line of a plugin.ini (sco-core sdk/docs/plugin-ini.md).
struct PluginSetting {
    std::string name, type, label, help, def;   // type: bool, int, float, string or enum
    bool hasMin = false, hasMax = false;
    long long minI = 0, maxI = 0;
    double minF = 0, maxF = 0;
    std::vector<std::string> choices;
    std::string value;      // what the game reads at its next start: the kept value, else the default
    bool kept = false;      // a value is kept in the plugin's database
    char edit[256] = "";    // the text in the editor
};

// One data/plugins/<folder>/ with a plugin.ini.
struct PluginEntry {
    wstring folder;
    std::string id, name, version, author, about, kind;   // id is the folder name: the host requires id == folder
    bool disabled = false;       // the folder holds an entry named "disabled"
    bool settingsBad = false;    // a [settings] line this reader doesn't understand (the host may refuse the plugin)
    std::vector<PluginSetting> settings;
};

struct Gui {
    HWND wnd = nullptr;
    HANDLE child = nullptr, childIn = nullptr;
    std::wstring tail;
    std::wstring running;
    bool hasLeftovers = false, gameUp = false;
    std::string lightTitle, lightDetail;
    std::string log;
    std::vector<size_t> lines{ 0 };
    bool newOutput = false;
    bool jumpToEnd = false;
    std::string question;
    Action action = kActNone;
    int sel = 0;
    int page = 0;                // 0 home, 1 output, 2 plugins
    double contentSince = 0, introStart = 0, pageSince = 0;
    bool introDone = false, animating = false, focused = true, inFrame = false, sizing = false, occluded = false, dwmBorder = false;
    RECT splash{}, full{};
    LONG fullW = 0, fullH = 0;
    float dpi = 1.0f, controlsX = 0;
    ULONGLONG lastRefresh = 0;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    UINT resizeW = 0, resizeH = 0;
    ImFont* font = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
    wstring here;
    bool discord = false, pluginsOn = true;   // the Discord plugin is on; plugins in sc-offline.ini
    std::vector<PluginEntry> plugins;
    ULONGLONG pluginsAt = 0;
    char plugSearch[64] = "";
    float plugScroll = 0;
    std::string plugOpen;        // folder of the plugin whose settings are open; "" shows the list
    std::string note;            // a short message on the plugins page
    double noteAt = 0;
};
static Gui g_gui;

static double Seconds() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

// What the status banner shows. Uses only cheap facts (no drive search): the game folder from
// sc-offline.ini or the one remembered from the last run.
static void RefreshLight() {
    Gui& g = g_gui;
    g.lastRefresh = GetTickCount64();
    Config cfg;
    { FILE* saved = g_log; g_log = nullptr; ReadConfig(g.here + L"\\sc-offline.ini", cfg); g_log = saved; }
    g.discord = IsFile(g.here + L"\\data\\plugins\\discord\\plugin.ini") &&
                GetFileAttributesW((g.here + L"\\data\\plugins\\discord\\disabled").c_str()) == INVALID_FILE_ATTRIBUTES;
    g.pluginsOn = cfg.plugins;
    wstring bin = cfg.game.empty() ? L"" : ToBin64(cfg.game, cfg.channel);
    if (bin.empty()) { std::vector<Found> f; FromRemembered(g.here, cfg.channel, f); if (!f.empty()) bin = f.front().bin; }
    std::vector<std::string> left;
    if (!bin.empty()) {
        const GamePaths p(bin);
        const DllInfo d = Identify(p.dll);
        if (IsFile(p.marker) || (d.present && d.ours)) left.push_back("the mod is still in Bin64");
    }
    const std::string pc = PcChangesLeft();
    if (!pc.empty()) {
        for (const FwRule& r : kFwRules) if (Field(pc, r.key) == "added") left.push_back("firewall rule \"" + Narrow(r.name) + "\"");
        if (Field(pc, "hosts") == "added") left.push_back("the EAC line in the hosts file");
        if (Field(pc, "eac") == "renamed") left.push_back("EasyAntiCheat_EOS.exe renamed to .bak");
    }
    g.hasLeftovers = !left.empty();
    g.gameUp = GameRunning();
    if (g.gameUp) {
        g.lightTitle = "Not safe to go online";
        g.lightDetail = "Star Citizen is running. When it closes, sc-offline takes the mod out and undoes its PC changes.";
    } else if (!left.empty()) {
        g.lightTitle = "Not safe to go online yet";
        g.lightDetail = "Click Uninstall to undo: ";
        for (size_t i = 0; i < left.size(); ++i) g.lightDetail += (i ? "; " : "") + left[i];
    } else if (bin.empty()) {
        g.lightTitle = "Game folder not known yet";
        g.lightDetail = "Click Status or Play to find it. No PC changes are left over.";
    } else {
        g.lightTitle = "Safe to go online";
        g.lightDetail = "The mod is out of the game folder and no PC changes are left.";
    }
}

static void IndexLines() {
    Gui& g = g_gui;
    g.lines.assign(1, 0);
    for (size_t i = 0; i < g.log.size(); ++i) if (g.log[i] == '\n') g.lines.push_back(i + 1);
}

static void ClearOutput() {
    Gui& g = g_gui;
    g.log.clear(); g.lines.assign(1, 0); g.tail.clear(); g.question.clear();
}

static void AppendOutput(const std::wstring& w) {
    Gui& g = g_gui;
    std::string t = Narrow(w);
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    if (g.log.size() + t.size() > 150000) {
        const size_t cut = g.log.find('\n', 50000);
        g.log.erase(0, cut == std::string::npos ? g.log.size() : cut + 1);
        IndexLines();
    }
    const size_t from = g.log.size();
    g.log += t;
    for (size_t i = from; i < g.log.size(); ++i) if (g.log[i] == '\n') g.lines.push_back(i + 1);
    g.newOutput = true;
    g.tail += w;
    if (g.tail.size() > 600) g.tail.erase(0, g.tail.size() - 600);
    const size_t q = g.tail.rfind(L"[y/N] ");
    if (q != std::wstring::npos && q + 6 == g.tail.size()) {
        const size_t nl = g.tail.find_last_of(L'\n', q);
        const size_t start = nl == std::wstring::npos ? 0 : nl + 1;
        g.question = Narrow(g.tail.substr(start, q - start));
        while (!g.question.empty() && g.question.back() == ' ') g.question.pop_back();
    } else {
        g.question.clear();
    }
}

struct ReaderArgs { HANDLE pipe, proc; HWND wnd; };
static DWORD WINAPI ReaderThread(void* p) {
    ReaderArgs a = *(ReaderArgs*)p; delete (ReaderArgs*)p;
    char buf[4096]; DWORD got = 0; std::string pending;
    while (ReadFile(a.pipe, buf, sizeof(buf), &got, nullptr) && got) {
        pending.append(buf, got);
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
    g.child = pi.hProcess; g.childIn = inW; g.running = cmd;
    ClearOutput();
    AppendOutput(L"> sc-offline.exe " + wstring(cmd) + L"\n");
    if (!wcscmp(cmd, L"status") || !wcscmp(cmd, L"update")) { g.page = 1; g.pageSince = Seconds(); }
    g.jumpToEnd = true;
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

static float Px(float v) { return v * g_gui.dpi; }
static float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float EaseOut(float t) { t = Clamp01(t); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

static ImU32 Mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

static ImU32 Col(ImU32 c, float alpha = 1.0f) { return ImGui::GetColorU32(c, alpha); }

static float Anim(ImGuiID id, float target, float speed = 16.0f) {
    float* v = ImGui::GetStateStorage()->GetFloatRef(id, target);
    *v += (target - *v) * (1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime));
    if (std::fabs(target - *v) < 0.002f) *v = target; else g_gui.animating = true;
    return *v;
}

static float Since(double since, double secs) {
    const float t = Clamp01(float((Seconds() - since) / secs));
    if (t < 1.0f) g_gui.animating = true;
    return EaseOut(t);
}

struct UseFont {
    explicit UseFont(ImFont* f, float size = 0.0f) { ImGui::PushFont(f, size); }
    ~UseFont() { ImGui::PopFont(); }
};

static void Label(ImVec2 at, ImU32 col, const char* s, const char* end = nullptr) {
    ImGui::GetWindowDrawList()->AddText(at, Col(col), s, end);
}

static void UiSpinner(ImVec2 c, float r, float thick, ImU32 col) {
    const float t = float(ImGui::GetTime());
    const float start = t * 6.0f, len = 3.14159265f * (0.3f + 1.2f * (0.5f + 0.5f * std::sin(t * 3.2f)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathArcTo(c, r, start, start + len, 40);
    dl->PathStroke(Col(col), ImDrawFlags_None, thick);
    if (g_gui.focused) g_gui.animating = true;
}

static ImU32 Fade(ImU32 c, float a) {
    const ImU32 alpha = ImU32(float((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

static std::string Fit(std::string s, float width, bool fromLeft) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    while (s.size() > 1 && ImGui::CalcTextSize((fromLeft ? "..." + s : s + "...").c_str()).x > width) {
        if (fromLeft) { s.erase(0, 1); while (!s.empty() && (s[0] & 0xC0) == 0x80) s.erase(0, 1); }
        else { char c; do { c = s.back(); s.pop_back(); } while (!s.empty() && (c & 0xC0) == 0x80); }
    }
    return fromLeft ? "..." + s : s + "...";
}

static void DrawIcon(Icon icon, ImVec2 c, float s, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = Px(1.6f);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    auto line = [&](std::initializer_list<ImVec2> pts, bool closed = false) {
        ImVec2 v[8]; int n = 0;
        for (const ImVec2& p : pts) if (n < 8) v[n++] = p;
        dl->AddPolyline(v, n, col, closed ? ImDrawFlags_Closed : ImDrawFlags_None, t);
    };
    switch (icon) {
    case kIcoPlay: dl->AddTriangleFilled(P(-0.55f, -0.8f), P(0.8f, 0.0f), P(-0.55f, 0.8f), col); break;
    case kIcoInstall:
        line({ P(0, -0.9f), P(0, 0.3f) });
        line({ P(-0.45f, -0.15f), P(0, 0.3f), P(0.45f, -0.15f) });
        line({ P(-0.9f, 0.35f), P(-0.9f, 0.9f), P(0.9f, 0.9f), P(0.9f, 0.35f) });
        break;
    case kIcoStatus:
        line({ P(-0.95f, 0.1f), P(-0.45f, 0.1f), P(-0.2f, -0.6f), P(0.15f, 0.7f), P(0.4f, 0.1f), P(0.95f, 0.1f) });
        break;
    case kIcoUpdate: {
        const float r = 0.75f * s, a1 = 1.6f * IM_PI;
        dl->PathArcTo(c, r, 0.1f * IM_PI, a1, 24);
        dl->PathStroke(col, ImDrawFlags_None, t);
        const ImVec2 e(c.x + r * std::cos(a1), c.y + r * std::sin(a1)), d(-std::sin(a1), std::cos(a1)), n(-d.y, d.x);
        line({ e - d * (0.45f * s) + n * (0.4f * s), e, e - d * (0.45f * s) - n * (0.4f * s) });
        break;
    }
    case kIcoHome:
        line({ P(-0.9f, -0.05f), P(0, -0.85f), P(0.9f, -0.05f) });
        line({ P(-0.62f, -0.3f), P(-0.62f, 0.85f), P(0.62f, 0.85f), P(0.62f, -0.3f) });
        break;
    case kIcoOutput:
        dl->AddRect(P(-0.95f, -0.8f), P(0.95f, 0.8f), col, 0.25f * s, 0, t);
        line({ P(-0.55f, -0.3f), P(-0.2f, 0.0f), P(-0.55f, 0.3f) });
        line({ P(0.0f, 0.35f), P(0.5f, 0.35f) });
        break;
    case kIcoSettings:
        for (int i = -1; i <= 1; ++i) {
            const float y = 0.62f * float(i), k = i < 0 ? 0.35f : i == 0 ? -0.35f : 0.15f;
            line({ P(-0.9f, y), P(0.9f, y) });
            dl->AddCircleFilled(P(k, y), 0.24f * s, col, 12);
        }
        break;
    case kIcoFolder:
        line({ P(-0.95f, -0.75f), P(-0.3f, -0.75f), P(-0.1f, -0.5f), P(0.95f, -0.5f), P(0.95f, 0.8f), P(-0.95f, 0.8f) }, true);
        break;
    case kIcoPad:
        dl->AddRect(P(-0.95f, -0.55f), P(0.95f, 0.6f), col, 0.5f * s, 0, t);
        line({ P(-0.6f, 0.02f), P(-0.15f, 0.02f) });
        line({ P(-0.375f, -0.21f), P(-0.375f, 0.25f) });
        dl->AddCircleFilled(P(0.32f, -0.08f), 0.12f * s, col, 10);
        dl->AddCircleFilled(P(0.58f, 0.15f), 0.12f * s, col, 10);
        break;
    case kIcoShield:
        line({ P(0, -0.95f), P(0.8f, -0.6f), P(0.7f, 0.3f), P(0, 0.95f), P(-0.7f, 0.3f), P(-0.8f, -0.6f) }, true);
        break;
    case kIcoBolt:
        dl->AddTriangleFilled(P(0.2f, -0.95f), P(0.1f, 0.15f), P(-0.65f, 0.15f), col);
        dl->AddTriangleFilled(P(-0.1f, -0.15f), P(0.65f, -0.15f), P(-0.2f, 0.95f), col);
        break;
    case kIcoCheck: line({ P(-0.8f, 0.05f), P(-0.25f, 0.6f), P(0.85f, -0.55f) }); break;
    case kIcoMark:
        dl->AddCircle(c, 0.92f * s, col, 40, Px(2.2f));
        dl->AddTriangleFilled(P(-0.28f, -0.45f), P(0.5f, 0.0f), P(-0.28f, 0.45f), col);
        break;
    case kIcoServer:
        for (int i = 0; i < 2; ++i) {
            const float top = i ? 0.1f : -0.85f, bottom = i ? 0.85f : -0.1f;
            dl->AddRect(P(-0.9f, top), P(0.9f, bottom), col, 0.2f * s, 0, t);
            dl->AddCircleFilled(P(0.5f, (top + bottom) * 0.5f), 0.12f * s, col, 10);
        }
        break;
    case kIcoUsers:
        dl->AddCircle(P(-0.3f, -0.4f), 0.3f * s, col, 20, t);
        dl->PathArcTo(P(-0.3f, 0.75f), 0.6f * s, IM_PI * 1.05f, IM_PI * 1.95f, 20);
        dl->PathStroke(col, ImDrawFlags_None, t);
        dl->AddCircle(P(0.48f, -0.25f), 0.24f * s, col, 20, t);
        dl->PathArcTo(P(0.48f, 0.75f), 0.46f * s, IM_PI * 1.1f, IM_PI * 1.9f, 20);
        dl->PathStroke(col, ImDrawFlags_None, t);
        break;
    case kIcoMods:
        for (int i = 0; i < 4; ++i) {
            const float x = (i & 1) ? 0.1f : -0.9f, y = (i & 2) ? 0.1f : -0.9f;
            if (i == 1) dl->AddRectFilled(P(x, y), P(x + 0.8f, y + 0.8f), col, 0.22f * s);
            else dl->AddRect(P(x, y), P(x + 0.8f, y + 0.8f), col, 0.22f * s, 0, t);
        }
        break;
    case kIcoSearch:
        dl->AddCircle(P(-0.15f, -0.15f), 0.62f * s, col, 24, t);
        line({ P(0.32f, 0.32f), P(0.88f, 0.88f) });
        break;
    }
}

static float UiCard(ImVec2 lo, ImVec2 hi, Icon icon, const char* title) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, Col(kUiCard), Px(10));
    const float cy = lo.y + Px(22);
    DrawIcon(icon, ImVec2(lo.x + Px(24), cy), Px(7), Col(kUiAccent));
    {
        UseFont f(g_gui.bold);
        Label(ImVec2(lo.x + Px(42), cy - ImGui::GetFontSize() * 0.5f), kUiText, title);
    }
    dl->AddLine(ImVec2(lo.x + Px(14), lo.y + Px(44)), ImVec2(hi.x - Px(14), lo.y + Px(44)), Col(kUiLine), Px(1));
    return lo.y + Px(44);
}

enum Look { kAction, kPlain, kQuiet };

static bool UiButton(const char* label, ImVec2 pos, ImVec2 size, Look look, bool enabled = true, int icon = -1) {
    ImGui::SetCursorScreenPos(pos);
    const ImGuiID id = ImGui::GetID(label);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hot = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
    ImGui::EndDisabled();
    const float h = Anim(id, hot ? 1.0f : 0.0f), d = Anim(id + 1, down ? 1.0f : 0.0f, 24.0f);
    const float a = 0.4f + 0.6f * Anim(id + 2, enabled ? 1.0f : 0.0f, 10.0f);
    ImU32 bg = 0, fg = kUiText;
    switch (look) {
    case kAction: bg = Mix(kUiAction, kUiActionHover, h); fg = kUiActionText; break;
    case kPlain:  bg = Mix(kUiRow, kUiRowHover, h); break;
    default:      fg = Mix(kUiTextDim, kUiText, h); break;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float in = d * Px(1);
    if (look != kQuiet) dl->AddRectFilled(pos + ImVec2(in, in), pos + size - ImVec2(in, in), Col(Fade(bg, a)), Px(8));
    const char* end = label;
    while (*end && !(end[0] == '#' && end[1] == '#')) ++end;
    UseFont f(g_gui.bold);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float iw = icon >= 0 ? Px(20) : 0.0f;
    const ImVec2 at(pos.x + (size.x - ts.x - iw) * 0.5f, pos.y + (size.y - ts.y) * 0.5f);
    if (icon >= 0) DrawIcon(Icon(icon), ImVec2(at.x + Px(6), pos.y + size.y * 0.5f), Px(6), Col(Fade(fg, a)));
    dl->AddText(ImVec2(at.x + iw, at.y), Col(Fade(fg, a)), label, end);
    return clicked && enabled;
}

static bool UiRow(const GuiCommand& c, ImVec2 pos, ImVec2 size, bool picked, bool enabled) {
    ImGui::SetCursorScreenPos(pos);
    const ImGuiID id = ImGui::GetID(c.label);
    const bool clicked = ImGui::InvisibleButton(c.label, size);
    const float h = Anim(id, ImGui::IsItemHovered() ? 1.0f : 0.0f), on = Anim(id + 1, picked ? 1.0f : 0.0f);
    const float a = enabled ? 1.0f : 0.45f;
    DrawIcon(c.icon, ImVec2(pos.x + Px(22), pos.y + size.y * 0.5f), Px(7), Col(Fade(Mix(Mix(kUiTextFaint, kUiTextDim, h), kUiAccent, on), a)));
    Label(ImVec2(pos.x + Px(44), pos.y + (size.y - ImGui::GetFontSize()) * 0.5f), Fade(Mix(Mix(kUiTextDim, kUiTextBody, h), kUiText, on), a), c.label);
    return clicked;
}

static ImU32 LineColour(const char* b, const char* e) {
    auto starts = [&](const char* p) { const size_t n = strlen(p); return size_t(e - b) >= n && !strncmp(b, p, n); };
    if (starts("[!]") || starts("[finished")) return kUiWarn;
    if (starts("[+]") || starts("[done]")) return kUiGreen;
    if (starts("[i]") || starts("> ")) return kUiAccent;
    return kUiTextBody;
}

static float DrawQuestion(ImVec2 at, float width) {
    Gui& g = g_gui;
    {
        UseFont f(g.bold);
        Label(at, kUiText, Fit(g.question, width, false).c_str());
        at.y += ImGui::GetFontSize() + Px(10);
    }
    const float bw = (width - Px(10)) * 0.5f, bh = Px(42);
    if (UiButton("Yes", at, ImVec2(bw, bh), kAction, true, kIcoCheck)) g.action = kActYes;
    if (UiButton("No", ImVec2(at.x + bw + Px(10), at.y), ImVec2(bw, bh), kPlain)) g.action = kActNo;
    return at.y + bh;
}

static bool CommandEnabled(Action a) {
    const Gui& g = g_gui;
    const bool idle = g.running.empty();
    if (a == kActStatus) return idle;                        // changes nothing
    if (a == kActUninstall) return idle && !g.gameUp && g.hasLeftovers;   // only with something to undo
    return idle && !g.gameUp;
}

static void ShowPage(int page) {
    Gui& g = g_gui;
    if (g.page == page) return;
    g.page = page; g.pageSince = Seconds();
    g.jumpToEnd = page == 1;
    if (page == 2) { g.pluginsAt = 0; g.plugOpen.clear(); g.plugScroll = 0; }
}

// A 34 x 18 switch with its top-left at lo; on is 0..1.
static void DrawSwitch(ImVec2 lo, float on) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = Px(34), h = Px(18);
    dl->AddRectFilled(lo, lo + ImVec2(w, h), Col(Mix(IM_COL32(52, 52, 58, 255), kUiAccent, on)), h * 0.5f);
    dl->AddCircleFilled(ImVec2(lo.x + h * 0.5f + (w - h) * on, lo.y + h * 0.5f), h * 0.5f - Px(3),
                        Col(Mix(kUiTextBody, IM_COL32(14, 14, 16, 255), on)), 20);
}

static void DrawSidebar(float H) {
    Gui& g = g_gui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = Px(kUiSideW);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, H), Col(kUiSide));
    dl->AddLine(ImVec2(w, 0), ImVec2(w, H), Col(kUiLine), Px(1));
    auto nav = [&](const char* id, Icon icon, float cy, bool active) {
        const float s = Px(40);
        const ImVec2 p(w * 0.5f - s * 0.5f, cy - s * 0.5f);
        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
        const ImGuiID key = ImGui::GetID(id);
        const float h = Anim(key, ImGui::IsItemHovered() ? 1.0f : 0.0f), on = Anim(key + 1, active ? 1.0f : 0.0f);
        dl->AddRectFilled(p, p + ImVec2(s, s), Col(kUiRow, (std::max)(on, h * 0.6f)), Px(10));
        dl->AddRectFilled(ImVec2(0, cy - Px(10)), ImVec2(Px(3), cy + Px(10)), Col(kUiAccent, on), Px(2));
        DrawIcon(icon, ImVec2(w * 0.5f, cy), Px(8), Col(Mix(Mix(kUiTextDim, kUiTextBody, h), kUiAccent, on)));
        return clicked;
    };
    if (nav("##home", kIcoHome, H * 0.5f - Px(52), g.page == 0)) ShowPage(0);
    if (nav("##plugins", kIcoMods, H * 0.5f, g.page == 2)) ShowPage(2);
    if (nav("##output", kIcoOutput, H * 0.5f + Px(52), g.page == 1)) ShowPage(1);
}

static void Tip(const char* text);

static void DrawControls(float W) {
    Gui& g = g_gui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = Px(28), y = Px(9);
    const float sx = W - Px(10) - 4 * s - Px(8), tw = Px(100), tx = sx - Px(8) - tw;
    g.controlsX = tx - Px(4);
    {   // Discord status toggle: the Discord plugin's `disabled` marker; the next Play uses it.
        const ImVec2 lo(tx, y);
        ImGui::SetCursorScreenPos(lo);
        if (ImGui::InvisibleButton("##discord", ImVec2(tw, s))) g.action = kActDiscord;
        const ImGuiID id = ImGui::GetID("##discord");
        const float h = Anim(id, ImGui::IsItemHovered() ? 1.0f : 0.0f), on = Anim(id + 1, g.discord ? 1.0f : 0.0f);
        Tip("Show \"Playing sc-offline\" on your Discord profile from the next Play");
        Label(ImVec2(lo.x, y + (s - ImGui::GetFontSize()) * 0.5f), Mix(kUiTextDim, kUiText, (std::max)(h, on)), "Discord");
        DrawSwitch(ImVec2(lo.x + tw - Px(34), y + (s - Px(18)) * 0.5f), on);
    }
    for (int i = 0; i < 2; ++i) {
        const char* id = i ? "##logs" : "##settings";
        const ImVec2 p(sx + i * s, y);
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton(id, ImVec2(s, s))) g.action = i ? kActLogs : kActSettings;
        const float h = Anim(ImGui::GetID(id), ImGui::IsItemHovered() ? 1.0f : 0.0f);
        Tip(i ? "Logs folder" : "Settings");
        dl->AddRectFilled(p, p + ImVec2(s, s), Col(kUiRow, h), Px(6));
        DrawIcon(i ? kIcoFolder : kIcoSettings, p + ImVec2(s, s) * 0.5f, Px(6), Col(Mix(kUiTextDim, kUiText, h)));
    }
    for (int i = 0; i < 2; ++i) {
        const char* id = i ? "##close" : "##minimize";
        const ImVec2 p(W - Px(10) - 2 * s + i * s, y);
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton(id, ImVec2(s, s))) g.action = i ? kActClose : kActMinimize;
        const float h = Anim(ImGui::GetID(id), ImGui::IsItemHovered() ? 1.0f : 0.0f);
        dl->AddRectFilled(p, p + ImVec2(s, s), Col(i ? kUiRed : kUiRow, h), Px(6));
        const ImVec2 c = p + ImVec2(s, s) * 0.5f;
        const ImU32 ic = Col(Mix(kUiTextBody, kUiText, h));
        const float r = Px(5);
        if (i) {
            dl->AddLine(c - ImVec2(r, r), c + ImVec2(r, r), ic, Px(1.4f));
            dl->AddLine(c + ImVec2(-r, r), c + ImVec2(r, -r), ic, Px(1.4f));
        } else {
            dl->AddLine(c - ImVec2(r, 0), c + ImVec2(r, 0), ic, Px(1.4f));
        }
    }
}

static void DrawHome(float x0, float y0, float x1) {
    Gui& g = g_gui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float gap = Px(kUiGap), lw = std::floor((x1 - x0 - gap) * 0.52f), rx = x0 + lw + gap;
    const float bannerH = Px(64), sBot = y0 + Px(44) + Px(14) + bannerH + Px(14);
    const float aTop = sBot + gap, aBot = aTop + Px(44) + Px(14) + Px(42) + Px(14);

    const float y = UiCard(ImVec2(x0, y0), ImVec2(x0 + lw, aBot), kIcoPad, "Launcher") + Px(10);
    const float rh = Px(36), step = Px(40), rowX = x0 + Px(12), rowW = lw - Px(24);
    const float hy = Anim(ImGui::GetID("##pick"), y + float(g.sel) * step, 20.0f);
    dl->AddRectFilled(ImVec2(rowX, hy), ImVec2(rowX + rowW, hy + rh), Col(kUiRow), Px(8));
    for (int i = 0; i < kGuiCommandCount; ++i) {
        const GuiCommand& c = kGuiCommands[i];
        const bool on = CommandEnabled(c.act);
        if (UiRow(c, ImVec2(rowX, y + float(i) * step), ImVec2(rowW, rh), g.sel == i, on)) g.sel = i;
        if (on && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) g.action = c.act;
    }

    const float by = UiCard(ImVec2(rx, y0), ImVec2(x1, sBot), kIcoShield, "Status") + Px(14);
    {
        const ImVec2 lo(rx + Px(14), by), hi(x1 - Px(14), by + bannerH);
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(lo, hi, Col(IM_COL32_WHITE), Px(8));
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, lo, ImVec2(hi.x, lo.y), kUiBannerFrom, kUiBannerTo);
        const float fs = ImGui::GetFontSize();
        { UseFont f(g.bold); Label(ImVec2(lo.x + Px(14), lo.y + Px(13)), kUiText, "Star Citizen"); }
        Label(ImVec2(lo.x + Px(14), lo.y + Px(16) + fs), kUiTextBody, g.lightTitle.c_str());
        ImGui::SetCursorScreenPos(lo);
        ImGui::InvisibleButton("##banner", hi - lo);
        Tip(g.lightDetail.c_str());
    }

    const float bw = x1 - rx - Px(28);
    std::string title = "Actions";
    if (!g.question.empty()) { UseFont f(g.bold); title = Fit(g.question, bw - Px(28), false); }
    const float ay = UiCard(ImVec2(rx, aTop), ImVec2(x1, aBot), kIcoBolt, title.c_str()) + Px(14);
    if (!g.question.empty()) {
        const float hw = (bw - Px(10)) * 0.5f;
        if (UiButton("Yes", ImVec2(rx + Px(14), ay), ImVec2(hw, Px(42)), kAction, true, kIcoCheck)) g.action = kActYes;
        if (UiButton("No", ImVec2(rx + Px(14) + hw + Px(10), ay), ImVec2(hw, Px(42)), kPlain)) g.action = kActNo;
    } else if (!g.running.empty()) {
        const std::string label = "Running " + Narrow(g.running) + "##act";
        UiButton(label.c_str(), ImVec2(rx + Px(14), ay), ImVec2(bw, Px(42)), kAction, false);
        UiSpinner(ImVec2(rx + Px(34), ay + Px(21)), Px(7), Px(2.0f), kUiActionText);
    } else {
        const GuiCommand& c = kGuiCommands[g.sel];
        const std::string label = std::string(c.label) + "##act";
        if (UiButton(label.c_str(), ImVec2(rx + Px(14), ay), ImVec2(bw, Px(42)), kAction, CommandEnabled(c.act), c.icon)) g.action = c.act;
    }
}

static void Tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(10), Px(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Px(6));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kUiCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kUiLine);
    ImGui::SetTooltip("%s", text);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

static float PluginsSlider(const char* id, float left, float top, float bottom, float trackX, float contentH, float step) {
    Gui& g = g_gui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float viewH = bottom - top;
    const float maxScroll = (std::max)(0.0f, contentH - viewH);
    if (ImGui::IsMouseHoveringRect(ImVec2(left, top), ImVec2(trackX + Px(8), bottom)) && ImGui::GetIO().MouseWheel != 0.0f)
        g.plugScroll -= ImGui::GetIO().MouseWheel * step;
    if (maxScroll > 0) {
        const float thumbH = (std::max)(Px(28), viewH * viewH / contentH);
        ImGui::SetCursorScreenPos(ImVec2(trackX - Px(6), top));
        ImGui::InvisibleButton(id, ImVec2(Px(16), viewH));
        const bool hot = ImGui::IsItemHovered(), drag = ImGui::IsItemActive();
        if (drag) g.plugScroll = (ImGui::GetIO().MousePos.y - top - thumbH * 0.5f) / (viewH - thumbH) * maxScroll;
        g.plugScroll = (std::min)((std::max)(g.plugScroll, 0.0f), maxScroll);
        const float h = Anim(ImGui::GetID(id), hot || drag ? 1.0f : 0.0f);
        const float ty = top + (viewH - thumbH) * (g.plugScroll / maxScroll);
        dl->AddRectFilled(ImVec2(trackX, top), ImVec2(trackX + Px(4), bottom), Col(kUiRow), Px(2));
        dl->AddRectFilled(ImVec2(trackX, ty), ImVec2(trackX + Px(4), ty + thumbH), Col(Mix(kUiTextFaint, kUiAccent, h)), Px(2));
    } else {
        g.plugScroll = 0;
    }
    return Anim(ImGui::GetID("##plugscroll"), g.plugScroll, 22.0f);
}

// --- Plugins page: data\plugins\<id>\plugin.ini ----------------------------------------------------
// The launcher reads what the host (sco-core) reads: each data\plugins\<folder>\plugin.ini, whose id,
// name, version, author and [settings] section are described in sco-core's sdk/docs/plugin-ini.md.
// A plugin is off when its folder holds an entry named `disabled`; the host checks for it at start.
// A plugin's [settings] values live in its own database, data\storage\<id>.db, table sco_kv, key
// `sco.settings.<name>`, value `<type>:<value>` (sco-core docs/plugins.md, "Values").

static std::string TrimA(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// `;` or `#` starts a comment at the start of a line or after a space or tab, outside double quotes.
static std::string StripIniComment(const std::string& s) {
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') quoted = !quoted;
        else if (!quoted && (c == ';' || c == '#') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) return s.substr(0, i);
    }
    return s;
}

// The next bare word or "quoted text" of a [settings] line. A bare word may hold a (...) group, as in
// `enum(a,b,c)`.
static bool NextToken(const std::string& s, size_t& i, std::string& tok, bool& quoted) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size()) return false;
    quoted = s[i] == '"';
    if (quoted) {
        const size_t e = s.find('"', i + 1);
        if (e == std::string::npos) return false;
        tok = s.substr(i + 1, e - i - 1);
        i = e + 1;
        return true;
    }
    const size_t b = i;
    bool paren = false;
    while (i < s.size() && (paren || (s[i] != ' ' && s[i] != '\t'))) {
        if (s[i] == '(') paren = true;
        else if (s[i] == ')') paren = false;
        ++i;
    }
    tok = s.substr(b, i - b);
    return true;
}

// [+-]digits[.digits][(e|E)[+-]digits]: the float grammar the host accepts.
static bool PlainDecimal(const std::string& s) {
    size_t i = 0;
    auto digits = [&]() { const size_t from = i; while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; return i > from; };
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    if (!digits()) return false;
    if (i < s.size() && s[i] == '.') { ++i; if (!digits()) return false; }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (!digits()) return false;
    }
    return i == s.size();
}

static std::string FloatText(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.17g", v);   // what the host stores
    return b;
}

// The value of a setting that has no `default`: 0 (or the nearest bound), false, "", the first choice.
static std::string ZeroText(const PluginSetting& s) {
    if (s.type == "bool") return "false";
    if (s.type == "int") {
        long long v = 0;
        if (s.hasMin && v < s.minI) v = s.minI;
        if (s.hasMax && v > s.maxI) v = s.maxI;
        return std::to_string(v);
    }
    if (s.type == "float") {
        double v = 0;
        if (s.hasMin && v < s.minF) v = s.minF;
        if (s.hasMax && v > s.maxF) v = s.maxF;
        return FloatText(v);
    }
    if (s.type == "enum" && !s.choices.empty()) return s.choices[0];
    return "";
}

// Checks text against the declaration (type, range, choices) with the host's rules, and gives the text
// the host would store. On failure `why` says what is wrong.
static bool CanonSetting(const PluginSetting& s, const std::string& text, std::string& out, std::string& why) {
    if (s.type == "bool") {
        if (text != "true" && text != "false") { why = "true or false"; return false; }
        out = text;
        return true;
    }
    if (s.type == "int") {
        size_t i = 0;
        bool neg = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) { neg = text[i] == '-'; ++i; }
        if (i == text.size() || text.size() - i > 18) { why = "a whole number"; return false; }
        long long v = 0;
        for (; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') { why = "a whole number"; return false; }
            v = v * 10 + (text[i] - '0');
        }
        if (neg) v = -v;
        if (s.hasMin && v < s.minI) { why = "at least " + std::to_string(s.minI); return false; }
        if (s.hasMax && v > s.maxI) { why = "at most " + std::to_string(s.maxI); return false; }
        out = std::to_string(v);
        return true;
    }
    if (s.type == "float") {
        if (text.size() > 64 || !PlainDecimal(text)) { why = "a number such as 1.5"; return false; }
        const double v = std::strtod(text.c_str(), nullptr);
        if (!std::isfinite(v)) { why = "not a finite number"; return false; }
        if (s.hasMin && v < s.minF) { why = "at least " + FloatText(s.minF); return false; }
        if (s.hasMax && v > s.maxF) { why = "at most " + FloatText(s.maxF); return false; }
        out = FloatText(v);
        return true;
    }
    if (s.type == "string") {
        if (text.size() > 255) { why = "at most 255 characters"; return false; }
        for (const char c : text) if (c < 0x20 || c > 0x7e) { why = "printable ASCII only"; return false; }
        out = text;
        return true;
    }
    for (const std::string& c : s.choices) if (c == text) { out = text; return true; }
    why = "pick one of the choices";
    return false;
}

static std::string SettingHint(const PluginSetting& s) {
    std::string h = s.type;
    if (s.type == "enum") {
        h = "one of ";
        for (size_t i = 0; i < s.choices.size(); ++i) h += (i ? ", " : "") + s.choices[i];
    } else if (s.hasMin || s.hasMax) {
        const bool f = s.type == "float";
        if (s.hasMin) h += ", min " + (f ? FloatText(s.minF) : std::to_string(s.minI));
        if (s.hasMax) h += ", max " + (f ? FloatText(s.maxF) : std::to_string(s.maxI));
    }
    return h;
}

// `name = type [default V] [min N] [max N] [label "text"] [help "text"]`. False when the line has
// something this reader doesn't understand (the host would refuse the whole plugin for it).
static bool ParseSettingLine(const std::string& name, const std::string& rhs, PluginEntry& p) {
    PluginSetting s;
    s.name = name;
    size_t i = 0;
    std::string tok;
    bool quoted = false;
    if (!NextToken(rhs, i, tok, quoted) || quoted) return false;
    const size_t par = tok.find('(');
    s.type = tok.substr(0, par);
    if (par != std::string::npos) {
        if (tok.back() != ')') return false;
        const std::string list = tok.substr(par + 1, tok.size() - par - 2);
        size_t a = 0;
        while (a <= list.size()) {
            size_t b = list.find(',', a);
            if (b == std::string::npos) b = list.size();
            const std::string c = TrimA(list.substr(a, b - a));
            if (!c.empty()) s.choices.push_back(c);
            a = b + 1;
        }
    }
    if (s.type != "bool" && s.type != "int" && s.type != "float" && s.type != "string" && s.type != "enum") return false;
    if (s.type == "enum" ? s.choices.empty() : par != std::string::npos) return false;
    std::string key, val;
    while (NextToken(rhs, i, key, quoted)) {
        if (quoted || !NextToken(rhs, i, val, quoted)) return false;
        if (key == "default") s.def = val;
        else if (key == "label") s.label = val;
        else if (key == "help") s.help = val;
        else if ((key == "min" || key == "max") && (s.type == "int" || s.type == "float")) {
            const bool lo = key == "min";
            if (s.type == "int") { (lo ? s.minI : s.maxI) = std::strtoll(val.c_str(), nullptr, 10); }
            else { (lo ? s.minF : s.maxF) = std::strtod(val.c_str(), nullptr); }
            (lo ? s.hasMin : s.hasMax) = true;
        } else return false;
    }
    std::string canon, why;
    s.value = !s.def.empty() && CanonSetting(s, s.def, canon, why) ? canon : ZeroText(s);
    if (s.label.empty()) s.label = s.name;
    std::snprintf(s.edit, sizeof(s.edit), "%s", s.value.c_str());
    p.settings.push_back(std::move(s));
    return true;
}

static void ParsePluginIni(std::string text, PluginEntry& p) {
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    p.id = Narrow(p.folder);
    p.name = p.id;
    bool inSettings = false, otherSection = false;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        const std::string line = TrimA(StripIniComment(text.substr(a, b - a)));
        a = b + 1;
        if (line.empty()) continue;
        if (line[0] == '[') { inSettings = line == "[settings]"; otherSection = !inSettings; continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimA(line.substr(0, eq)), value = TrimA(line.substr(eq + 1));
        if (inSettings) {
            if (p.settings.size() >= 32 || !ParseSettingLine(key, value, p)) p.settingsBad = true;
        } else if (!otherSection) {
            if (key == "name" && !value.empty()) p.name = value;
            else if (key == "version") p.version = value;
            else if (key == "author") p.author = value;
            else if (key == "description") p.about = value;   // not a host key; shown when a plugin adds one
            else if (key == "kind") p.kind = value;
        }
    }
}

static sqlite3* OpenPluginDb(const wstring& db, int flags) {
    sqlite3* h = nullptr;
    if (sqlite3_open_v2(Narrow(db).c_str(), &h, flags, nullptr) != SQLITE_OK) { if (h) sqlite3_close(h); return nullptr; }
    sqlite3_busy_timeout(h, 2000);
    return h;
}

static wstring PluginDbPath(const wstring& here, const wstring& folder) { return here + L"\\data\\storage\\" + folder + L".db"; }

// Reads each setting's kept value, if the plugin's database has one that still fits its declaration.
static void LoadKept(const wstring& here, PluginEntry& p) {
    if (p.settings.empty()) return;
    const wstring path = PluginDbPath(here, p.folder);
    if (!IsFile(path)) return;
    sqlite3* db = OpenPluginDb(path, SQLITE_OPEN_READONLY);
    if (!db) db = OpenPluginDb(path, SQLITE_OPEN_READWRITE);
    if (!db) return;
    for (PluginSetting& s : p.settings) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT value FROM sco_kv WHERE key = ?1", -1, &st, nullptr) != SQLITE_OK) break;
        const std::string key = "sco.settings." + s.name;
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char* v = static_cast<const char*>(sqlite3_column_blob(st, 0));
            const std::string text(v ? v : "", v ? size_t(sqlite3_column_bytes(st, 0)) : 0);
            const size_t colon = text.find(':');
            std::string canon, why;
            if (colon != std::string::npos && text.compare(0, colon, s.type) == 0 && CanonSetting(s, text.substr(colon + 1), canon, why)) {
                s.value = canon;
                s.kept = true;
                std::snprintf(s.edit, sizeof(s.edit), "%s", s.value.c_str());
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

static std::vector<PluginEntry> ReadPlugins(const wstring& here) {
    const wstring dir = here + L"\\data\\plugins";
    std::vector<PluginEntry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        PluginEntry p;
        p.folder = fd.cFileName;
        std::string text;
        if (!ReadAll(dir + L"\\" + p.folder + L"\\plugin.ini", text)) continue;   // the host skips a folder without one
        ParsePluginIni(text, p);
        p.disabled = GetFileAttributesW((dir + L"\\" + p.folder + L"\\disabled").c_str()) != INVALID_FILE_ATTRIBUTES;
        LoadKept(here, p);
        out.push_back(std::move(p));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const PluginEntry& x, const PluginEntry& y) { return _stricmp(x.name.c_str(), y.name.c_str()) < 0; });
    return out;
}

// Creates or removes the `disabled` marker; the host looks for any entry of that name at start.
static bool SetPluginDisabled(const wstring& here, const wstring& folder, bool disable) {
    const wstring marker = here + L"\\data\\plugins\\" + folder + L"\\disabled";
    const DWORD attr = GetFileAttributesW(marker.c_str());
    const bool there = attr != INVALID_FILE_ATTRIBUTES;
    if (disable == there) return true;
    if (disable) {
        HANDLE f = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        CloseHandle(f);
        return true;
    }
    return ((attr & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(marker.c_str()) : DeleteFileW(marker.c_str())) != 0;
}

// Writes the edited values (or, with reset, deletes the kept ones) into data\storage\<id>.db, the file
// the host reads them from at start. Only called while the game is closed: the host keeps its values
// in memory while it runs and would neither see the change nor announce settings.changed.
static bool SavePluginSettings(const wstring& here, const PluginEntry& p, bool reset, std::string& err) {
    std::vector<std::pair<std::string, std::string>> put;   // key, text
    for (const PluginSetting& s : p.settings) {
        const std::string key = "sco.settings." + s.name;
        if (reset) { if (s.kept) put.emplace_back(key, std::string()); continue; }
        std::string canon, why;
        if (!CanonSetting(s, s.edit, canon, why)) { err = s.label + ": " + why; return false; }
        if (canon != s.value) put.emplace_back(key, s.type + ":" + canon);
    }
    if (put.empty()) return true;
    CreateDirectoryW((here + L"\\data").c_str(), nullptr);
    CreateDirectoryW((here + L"\\data\\storage").c_str(), nullptr);
    sqlite3* db = OpenPluginDb(PluginDbPath(here, p.folder), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (!db) { err = "couldn't open data\\storage\\" + p.id + ".db"; return false; }
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK &&
              sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS sco_kv(key TEXT PRIMARY KEY NOT NULL, value BLOB NOT NULL) WITHOUT ROWID",
                           nullptr, nullptr, nullptr) == SQLITE_OK;
    for (size_t i = 0; ok && i < put.size(); ++i) {
        sqlite3_stmt* st = nullptr;
        const char* sql = reset ? "DELETE FROM sco_kv WHERE key = ?1" : "INSERT OR REPLACE INTO sco_kv(key, value) VALUES(?1, ?2)";
        ok = sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK;
        if (!ok) break;
        sqlite3_bind_text(st, 1, put[i].first.c_str(), -1, SQLITE_TRANSIENT);
        if (!reset) sqlite3_bind_blob(st, 2, put[i].second.data(), int(put[i].second.size()), SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (ok) ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ok) {
        err = std::string("couldn't write the values: ") + sqlite3_errmsg(db);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    sqlite3_close(db);
    return ok;
}

static void PluginNote(const std::string& s) { g_gui.note = s; g_gui.noteAt = Seconds(); }
static bool NoteLive() { return !g_gui.note.empty() && Seconds() - g_gui.noteAt < 10.0; }

static void DrawPluginList(float x0, float y0, float x1, float y1) {
    Gui& g = g_gui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    UiCard(ImVec2(x0, y0), ImVec2(x1, y1), kIcoMods, "Plugins");
    {
        float tw;
        { UseFont f(g.bold); tw = ImGui::CalcTextSize("Plugins").x; }
        int on = 0;
        for (const PluginEntry& p : g.plugins) on += !p.disabled;
        const std::string count = std::to_string(on) + " / " + std::to_string(g.plugins.size());
        Label(ImVec2(x0 + Px(42) + tw + Px(10), y0 + Px(22) - ImGui::GetFontSize() * 0.5f), kUiTextFaint, count.c_str());
    }

    const float fb = Px(28), sy = y0 + Px(8);
    float headerRight = x1 - Px(12);
    {
        const ImVec2 at(headerRight - fb, sy);
        ImGui::SetCursorScreenPos(at);
        if (ImGui::InvisibleButton("##plugdir", ImVec2(fb, fb))) g.action = kActPluginsDir;
        const float h = Anim(ImGui::GetID("##plugdir"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
        Tip("Open the plugins folder");
        dl->AddRectFilled(at, at + ImVec2(fb, fb), Col(kUiRow, h), Px(6));
        DrawIcon(kIcoFolder, at + ImVec2(fb, fb) * 0.5f, Px(6), Col(Mix(kUiTextDim, kUiText, h)));
        headerRight = at.x - Px(8);
    }
    {
        // Hot reload runs in the game (the sco.reload command); the launcher has no way to call it.
        const float rw = Px(76);
        if (UiButton("Reload##plug", ImVec2(headerRight - rw, sy), ImVec2(rw, fb), kPlain))
            PluginNote("Hot reload runs in the game: use the sco.reload command there. The launcher can't call it.");
        Tip("Plugins reload inside the running game (sco.reload)");
        headerRight -= rw + Px(8);
    }
    const float sw = Px(150);
    const ImVec2 searchAt(headerRight - sw, sy);
    ImGui::SetCursorScreenPos(searchAt);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(30), (fb - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(8));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kUiRow);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kUiRowHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kUiRowHover);
    ImGui::SetNextItemWidth(sw);
    if (ImGui::InputTextWithHint("##plugsearch", "Search", g.plugSearch, sizeof(g.plugSearch))) g.plugScroll = 0;
    const bool typing = ImGui::IsItemActive();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    DrawIcon(kIcoSearch, ImVec2(searchAt.x + Px(15), sy + fb * 0.5f), Px(5.5f), Col(typing ? kUiAccent : kUiTextDim));

    std::string needle = g.plugSearch;
    for (char& c : needle) c = char(tolower(static_cast<unsigned char>(c)));
    auto has = [&](std::string hay) {
        if (needle.empty()) return true;
        for (char& c : hay) c = char(tolower(static_cast<unsigned char>(c)));
        return hay.find(needle) != std::string::npos;
    };
    float top = y0 + Px(44) + Px(10), bottom = y1 - Px(12);
    const float left = x0 + Px(12), right = x1 - Px(30);

    if (NoteLive()) {
        const float nh = Px(36);
        bottom -= nh + Px(6);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(left + Px(2), bottom + Px(10)), Col(kUiTextDim), g.note.c_str(), nullptr, right - left);
    }
    if (!g.pluginsOn) {   // the host loads no plugin while plugins = off
        const float bh = Px(40);
        dl->AddRectFilled(ImVec2(left, top), ImVec2(right, top + bh), Col(kUiRow), Px(8));
        Label(ImVec2(left + Px(14), top + (bh - ImGui::GetFontSize()) * 0.5f), kUiWarn,
              Fit("Plugins are off (plugins = off in sc-offline.ini)", right - left - Px(130), false).c_str());
        if (UiButton("Turn on##plugson", ImVec2(right - Px(104), top + Px(5)), ImVec2(Px(96), bh - Px(10)), kAction)) g.action = kActPluginsOn;
        top += bh + Px(8);
    }

    std::vector<int> shown;
    for (int i = 0; i < int(g.plugins.size()); ++i) if (has(g.plugins[i].name + " " + g.plugins[i].id + " " + g.plugins[i].author)) shown.push_back(i);
    if (shown.empty()) {
        const float cy = (top + bottom) * 0.5f;
        const char* text = g.plugins.empty() ? "No plugins yet" : "No matches";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        DrawIcon(g.plugins.empty() ? kIcoMods : kIcoSearch, ImVec2((x0 + x1) * 0.5f, cy - Px(30)), Px(11), Col(kUiTextFaint));
        Label(ImVec2((x0 + x1) * 0.5f - ts.x * 0.5f, cy - ts.y * 0.5f), kUiTextDim, text);
        if (g.plugins.empty()) {
            const float bw = Px(150), bh = Px(34);
            if (UiButton("Open folder##plugempty", ImVec2((x0 + x1) * 0.5f - bw * 0.5f, cy + Px(16)), ImVec2(bw, bh), kPlain, true, kIcoFolder))
                g.action = kActPluginsDir;
        }
        return;
    }

    const float rowH = Px(44), step = Px(50);
    const float scroll = PluginsSlider("##pluginslider", left, top, bottom, x1 - Px(20), float(shown.size()) * step - (step - rowH), step);
    ImGui::PushClipRect(ImVec2(left, top), ImVec2(right, bottom), true);
    for (size_t k = 0; k < shown.size(); ++k) {
        const float ry = top + float(k) * step - scroll;
        if (ry + rowH < top || ry > bottom) continue;
        PluginEntry& p = g.plugins[shown[k]];
        const ImVec2 pos(left, ry), size(right - left, rowH);
        ImGui::PushID(p.id.c_str());
        ImGui::SetCursorScreenPos(pos);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##plug", size)) {
            if (SetPluginDisabled(g.here, p.folder, !p.disabled)) {
                p.disabled = !p.disabled;
                PluginNote(std::string(p.disabled ? "Off" : "On") + ": " + p.name + ". The game picks this up at its next start.");
            } else {
                PluginNote("Couldn't change data\\plugins\\" + p.id + "\\disabled (is the folder read-only?)");
            }
        }
        const ImGuiID id = ImGui::GetID("##plug");
        const float h = Anim(id, ImGui::IsItemHovered() ? 1.0f : 0.0f), on = Anim(id + 1, p.disabled ? 0.0f : 1.0f);
        std::string tip = p.about;
        if (!p.kind.empty()) tip += (tip.empty() ? "" : "\n") + p.kind + " plugin";
        if (!tip.empty()) Tip(tip.c_str());
        dl->AddRectFilled(pos, pos + size, Col(Mix(kUiRow, kUiRowHover, h)), Px(8));
        const float ty = ry + (rowH - ImGui::GetFontSize()) * 0.5f;
        const float tgW = Px(34), tgH = Px(18), tgX = pos.x + size.x - Px(14) - tgW;
        const bool gear = !p.settings.empty() || p.settingsBad;
        const float gearX = tgX - Px(10) - Px(28), rx = (gear ? gearX : tgX) - Px(12);
        float nameW;
        {
            UseFont f(g.bold);
            const std::string name = Fit(p.name, size.x * 0.4f, false);
            nameW = ImGui::CalcTextSize(name.c_str()).x;
            Label(ImVec2(pos.x + Px(14), ty), Mix(kUiTextBody, kUiText, (std::max)(h, on)), name.c_str());
        }
        const std::string meta = (p.version.empty() ? "" : "v" + p.version) + (p.author.empty() ? "" : (p.version.empty() ? "" : "  ") + p.author);
        const float mx = pos.x + Px(14) + nameW + Px(10);
        if (!meta.empty()) Label(ImVec2(mx, ty), kUiTextDim, Fit(meta, (std::max)(Px(10), rx - mx), false).c_str());
        DrawSwitch(ImVec2(tgX, ry + (rowH - tgH) * 0.5f), on);
        if (gear) {
            ImGui::SetCursorScreenPos(ImVec2(gearX, ry + (rowH - Px(28)) * 0.5f));
            const bool open = ImGui::InvisibleButton("##gear", ImVec2(Px(28), Px(28)));
            const float gh = Anim(ImGui::GetID("##gear"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
            Tip("Settings");
            DrawIcon(kIcoSettings, ImVec2(gearX + Px(14), ry + rowH * 0.5f), Px(6), Col(Mix(kUiTextDim, kUiText, gh)));
            if (open) { g.plugOpen = p.id; g.plugScroll = 0; }
        }
        ImGui::PopID();
    }
    ImGui::PopClipRect();
}

static void DrawPluginSettings(float x0, float y0, float x1, float y1, PluginEntry& p) {
    Gui& g = g_gui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string title = Fit(p.name + " settings", (x1 - x0) - Px(120), false);
    UiCard(ImVec2(x0, y0), ImVec2(x1, y1), kIcoSettings, title.c_str());
    if (UiButton("Back##plugback", ImVec2(x1 - Px(12) - Px(64), y0 + Px(9)), ImVec2(Px(64), Px(26)), kQuiet)) {
        g.plugOpen.clear(); g.plugScroll = 0; g.pluginsAt = 0;
        return;
    }
    const float left = x0 + Px(12), right = x1 - Px(30), top = y0 + Px(44) + Px(10);
    if (p.settingsBad || p.settings.empty()) {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(left + Px(2), top + Px(4)), Col(kUiWarn),
                    "plugin.ini has a [settings] line this launcher can't read; the game may refuse the plugin. "
                    "Run sco-plugin-check on the folder.", nullptr, right - left);
        return;
    }
    // Edits are written while the game is closed: it reads the values once, at start.
    const bool canEdit = g.running.empty() && !g.gameUp;
    const float bw = Px(96), bh = Px(32), footY = y1 - Px(12) - bh, bottom = footY - Px(8);
    const float rowH = Px(48), step = Px(54);
    const float scroll = PluginsSlider("##setslider", left, top, bottom, x1 - Px(20), float(p.settings.size()) * step - (step - rowH), step);
    bool dirty = false, anyKept = false;
    ImGui::PushClipRect(ImVec2(left, top), ImVec2(right, bottom), true);
    for (size_t k = 0; k < p.settings.size(); ++k) {
        PluginSetting& s = p.settings[k];
        dirty = dirty || std::strcmp(s.edit, s.value.c_str()) != 0;
        anyKept = anyKept || s.kept;
        const float ry = top + float(k) * step - scroll;
        if (ry + rowH < top || ry > bottom) continue;
        ImGui::PushID(int(k));
        dl->AddRectFilled(ImVec2(left, ry), ImVec2(right, ry + rowH), Col(kUiRow), Px(8));
        std::string canon, why;
        const bool ok = CanonSetting(s, s.edit, canon, why);
        const float wW = Px(170), wx = right - Px(14) - wW, textW = wx - left - Px(24);
        {
            UseFont f(g.bold);
            Label(ImVec2(left + Px(14), ry + Px(8)), ok ? kUiText : kUiWarn, Fit(s.label, textW, false).c_str());
        }
        const std::string sub = ok ? (s.help.empty() ? SettingHint(s) : s.help) : why;
        Label(ImVec2(left + Px(14), ry + rowH - Px(8) - ImGui::GetFontSize()), ok ? kUiTextDim : kUiWarn, Fit(sub, textW, false).c_str());
        if (s.type == "bool") {
            const bool on = !std::strcmp(s.edit, "true");
            const ImVec2 at(right - Px(14) - Px(34), ry + (rowH - Px(18)) * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(at.x, ry));
            ImGui::BeginDisabled(!canEdit);
            if (ImGui::InvisibleButton("##b", ImVec2(Px(34), rowH))) std::snprintf(s.edit, sizeof(s.edit), "%s", on ? "false" : "true");
            ImGui::EndDisabled();
            DrawSwitch(at, Anim(ImGui::GetID("##b"), on ? 1.0f : 0.0f));
        } else {
            ImGui::SetCursorScreenPos(ImVec2(wx, ry + (rowH - (ImGui::GetFontSize() + Px(12))) * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(10), Px(6)));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(8));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kUiBg);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kUiRowHover);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kUiRowHover);
            ImGui::SetNextItemWidth(wW);
            if (s.type == "enum") {
                ImGui::BeginDisabled(!canEdit);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(6), Px(6)));
                if (ImGui::BeginCombo("##e", s.edit)) {
                    for (const std::string& c : s.choices)
                        if (ImGui::Selectable(c.c_str(), c == s.edit)) std::snprintf(s.edit, sizeof(s.edit), "%s", c.c_str());
                    ImGui::EndCombo();
                }
                ImGui::PopStyleVar();
                ImGui::EndDisabled();
            } else {
                ImGuiInputTextFlags flags = canEdit ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_ReadOnly;
                if (s.type != "string") flags |= ImGuiInputTextFlags_CharsNoBlank;
                ImGui::InputText("##v", s.edit, sizeof(s.edit), flags);
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopID();
    }
    ImGui::PopClipRect();

    std::string err;
    if (UiButton("Save##plugsave", ImVec2(left, footY), ImVec2(bw, bh), kAction, canEdit && dirty, kIcoCheck)) {
        if (SavePluginSettings(g.here, p, false, err)) PluginNote("Saved. Applies at the next game start.");
        else PluginNote(err);
        g.plugins = ReadPlugins(g.here);   // p is gone after this; the edits come back from what was saved
        return;
    }
    if (UiButton("Reset##plugreset", ImVec2(left + bw + Px(8), footY), ImVec2(bw, bh), kPlain, canEdit && anyKept)) {
        if (SavePluginSettings(g.here, p, true, err)) PluginNote("Back to the defaults. Applies at the next game start.");
        else PluginNote(err);
        g.plugins = ReadPlugins(g.here);
        return;
    }
    const float mx = left + 2 * bw + Px(20);
    const std::string msg = !canEdit ? "Close the game to edit." : NoteLive() ? g.note : "Applies at the next game start.";
    Label(ImVec2(mx, footY + (bh - ImGui::GetFontSize()) * 0.5f), kUiTextDim, Fit(msg, right - mx, false).c_str());
}

static void DrawPlugins(float x0, float y0, float x1, float y1) {
    Gui& g = g_gui;
    if (g.plugOpen.empty() && (!g.pluginsAt || GetTickCount64() - g.pluginsAt > 3000)) {   // not while editing: that would drop the edits
        g.plugins = ReadPlugins(g.here);
        g.pluginsAt = GetTickCount64();
    }
    if (!g.plugOpen.empty()) {
        for (PluginEntry& p : g.plugins) if (p.id == g.plugOpen) { DrawPluginSettings(x0, y0, x1, y1, p); return; }
        g.plugOpen.clear();   // the plugin is gone
    }
    DrawPluginList(x0, y0, x1, y1);
}

static void DrawOutputPage(float x0, float y0, float x1, float y1) {
    Gui& g = g_gui;
    float y = UiCard(ImVec2(x0, y0), ImVec2(x1, y1), kIcoOutput, "Output") + Px(12);
    const float cw = Px(56);
    if (UiButton("Copy", ImVec2(x1 - Px(12) - cw, y0 + Px(9)), ImVec2(cw, Px(26)), kQuiet, !g.log.empty())) g.action = kActCopy;
    if (!g.running.empty()) UiSpinner(ImVec2(x1 - Px(12) - cw - Px(10), y0 + Px(22)), Px(6), Px(1.8f), kUiAccent);
    if (!g.question.empty()) y = DrawQuestion(ImVec2(x0 + Px(14), y), x1 - x0 - Px(28)) + Px(12);
    ImGui::SetCursorScreenPos(ImVec2(x0 + Px(14), y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(12), Px(10)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kUiSide));
    if (ImGui::BeginChild("##log", ImVec2(x1 - x0 - Px(28), (std::max)(y1 - Px(14) - y, Px(40))), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        UseFont f(g.mono, 13.0f);
        const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        ImGui::PushTextWrapPos(0.0f);
        for (size_t i = 0; i < g.lines.size(); ++i) {
            const char* b = g.log.data() + g.lines[i];
            const char* e = g.log.data() + (i + 1 < g.lines.size() ? g.lines[i + 1] - 1 : g.log.size());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(LineColour(b, e)));
            ImGui::TextUnformatted(b, e);
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        if ((g.newOutput && atEnd) || g.jumpToEnd) { ImGui::SetScrollHereY(1.0f); g.jumpToEnd = false; }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

static void DrawUi() {
    Gui& g = g_gui;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##sc-offline", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);

    if (!g.introDone) g.animating = true;
    const float t = float(Seconds() - g.introStart);
    const float card = Clamp01(t / 0.15f) * (1.0f - Clamp01(float((t - kIntroSpin) / (kIntroGrow * 0.5))));
    if (card > 0.0f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, card);
        UiSpinner(ImVec2(W * 0.5f, H * 0.5f), Px(13), Px(2.4f), kUiAccent);
        ImGui::PopStyleVar();
        g.animating = true;
    }

    const float show = g.introDone ? Since(g.contentSince, kIntroFade) : 0.0f;
    if (show > 0.0f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, show);
        DrawSidebar(H);
        DrawControls(W);
        const float pf = Since(g.pageSince, 0.22);
        const float slide = (1.0f - show) * Px(8) + (1.0f - pf) * Px(6);
        const float x0 = Px(kUiSideW) + Px(20), x1 = W - Px(kUiPad), y0 = Px(kUiTop) + slide, y1 = H - Px(kUiPad) + slide;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, show * pf);
        if (g.page == 0) DrawHome(x0, y0, x1);
        else if (g.page == 2) DrawPlugins(x0, y0, x1, y1);
        else DrawOutputPage(x0, y0, x1, y1);
        ImGui::PopStyleVar();
        ImGui::PopStyleVar();
    }
    g.newOutput = false;
    if (!g.question.empty() && g.action == kActNone) {
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) g.action = kActYes;
        else if (ImGui::IsKeyPressed(ImGuiKey_N, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g.action = kActNo;
    }

    if (!g.dwmBorder) ImGui::GetForegroundDrawList()->AddRect(ImVec2(0, 0), ImVec2(W, H), kUiLine, 0, 0, 1.0f);
    ImGui::End();
}

static void CreateTarget() {
    Gui& g = g_gui;
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g.dev->CreateRenderTargetView(back, nullptr, &g.rtv);
        back->Release();
    }
}

static void DropTarget() {
    Gui& g = g_gui;
    if (g.rtv) { g.rtv->Release(); g.rtv = nullptr; }
}

static bool CreateDevice(HWND wnd) {
    Gui& g = g_gui;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g.swap, &g.dev, &got, &g.ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g.swap, &g.dev, &got, &g.ctx);
    if (FAILED(hr)) return false;
    CreateTarget();
    return g.rtv != nullptr;
}

static void DropDevice() {
    Gui& g = g_gui;
    DropTarget();
    if (g.swap) { g.swap->Release(); g.swap = nullptr; }
    if (g.ctx) { g.ctx->Release(); g.ctx = nullptr; }
    if (g.dev) { g.dev->Release(); g.dev = nullptr; }
}

static void RenderFrame() {
    Gui& g = g_gui;
    if (g.inFrame || !g.swap) return;
    g.inFrame = true;
    if (!g.introDone) {
        const double t = Seconds() - g.introStart;
        if (t > kIntroSpin) {
            const float k = EaseOut(float((t - kIntroSpin) / kIntroGrow));
            auto mix = [k](LONG a, LONG b) { return int(a + (b - a) * k + 0.5f); };
            SetWindowPos(g.wnd, nullptr, mix(g.splash.left, g.full.left), mix(g.splash.top, g.full.top),
                         mix(g.splash.right - g.splash.left, g.full.right - g.full.left),
                         mix(g.splash.bottom - g.splash.top, g.full.bottom - g.full.top), SWP_NOZORDER | SWP_NOACTIVATE);
            if (k >= 1.0f) { g.introDone = true; g.contentSince = Seconds(); }
        }
    }
    if (g.resizeW && g.resizeH) {
        DropTarget();
        g.swap->ResizeBuffers(0, g.resizeW, g.resizeH, DXGI_FORMAT_UNKNOWN, 0);
        g.resizeW = g.resizeH = 0;
        CreateTarget();
    }
    g.animating = false;
    g.focused = GetForegroundWindow() == g.wnd;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();
    const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(kUiBg);
    const float clear[4] = { bg.x, bg.y, bg.z, 1.0f };
    g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
    g.ctx->ClearRenderTargetView(g.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g.occluded = g.swap->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    g.inFrame = false;
}

static void RunAction() {
    Gui& g = g_gui;
    const Action a = g.action;
    g.action = kActNone;
    for (const GuiCommand& c : kGuiCommands) if (c.act == a && c.cmd) { RunCommand(c.cmd); return; }
    switch (a) {
    case kActSettings: ShellExecuteW(g.wnd, L"open", L"notepad.exe", (L"\"" + g.here + L"\\sc-offline.ini\"").c_str(), nullptr, SW_SHOWNORMAL); break;
    case kActLogs: {
        const wstring d = g.here + L"\\data"; CreateDirectoryW(d.c_str(), nullptr);
        ShellExecuteW(g.wnd, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case kActPluginsDir: {
        const wstring d = g.here + L"\\data"; CreateDirectoryW(d.c_str(), nullptr);
        const wstring m = d + L"\\plugins"; CreateDirectoryW(m.c_str(), nullptr);
        ShellExecuteW(g.wnd, L"open", m.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        g.pluginsAt = 0;
        break;
    }
    case kActPluginsOn:
        if (SetIniValue(g.here + L"\\sc-offline.ini", "plugins", "on")) {
            RefreshLight();
            PluginNote("Plugins are on. They load the next time the game starts.");
        } else {
            PluginNote("Couldn't write sc-offline.ini; set plugins = on there by hand.");
        }
        break;
    case kActDiscord: {   // the Discord plugin's `disabled` marker; the next Play uses it
        const bool on = !g.discord;
        if (SetPluginDisabled(g.here, L"discord", !on)) {
            g.discord = on;
            AppendOutput(on ? L"\nDiscord status on: your profile shows \"Playing sc-offline\" from the next Play.\n"
                            : L"\nDiscord status off: from the next Play, nothing is shown on Discord.\n");
        } else {
            AppendOutput(L"\n[!] couldn't change data\\plugins\\discord\\disabled (is the folder read-only?)\n");
        }
        break;
    }
    case kActYes: Answer(true); break;
    case kActNo: Answer(false); break;
    case kActCopy: ImGui::SetClipboardText(g.log.c_str()); break;
    case kActMinimize: ShowWindow(g.wnd, SW_MINIMIZE); break;
    case kActClose: PostMessageW(g.wnd, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

static LRESULT CALLBACK GuiProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    Gui& g = g_gui;
    if (ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            if (IsZoomed(wnd)) {
                RECT& r = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0];
                const int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                const int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                r.left += fx; r.right -= fx; r.top += fy; r.bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(wnd, &p);
        if (g.introDone && p.x > LONG(Px(kUiSideW)) && p.y < LONG(Px(kUiTop)) && p.x < LONG(g.controlsX)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO:
        if (g.introDone) {
            MINMAXINFO* m = reinterpret_cast<MINMAXINFO*>(lp);
            m->ptMinTrackSize = m->ptMaxTrackSize = { g.fullW, g.fullH };
        }
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g.resizeW = LOWORD(lp); g.resizeH = HIWORD(lp); }
        return 0;
    case WM_ENTERSIZEMOVE: g.sizing = true; return 0;
    case WM_EXITSIZEMOVE: g.sizing = false; return 0;
    case WM_PAINT:
        ValidateRect(wnd, nullptr);
        if (g.sizing) RenderFrame();
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case kMsgOutput: { std::wstring* w = (std::wstring*)lp; AppendOutput(*w); delete w; return 0; }
    case kMsgDone: {
        const DWORD code = (DWORD)wp;
        if (g.child) { CloseHandle(g.child); g.child = nullptr; }
        if (g.childIn) { CloseHandle(g.childIn); g.childIn = nullptr; }
        g.question.clear();
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

static void LoadFonts() {
    Gui& g = g_gui;
    ImGuiIO& io = ImGui::GetIO();
    const wstring dir = EnvOr(L"WINDIR", L"C:\\Windows") + L"\\Fonts\\";
    auto load = [&](const wchar_t* file) -> ImFont* {
        const wstring p = dir + file;
        return IsFile(p) ? io.Fonts->AddFontFromFileTTF(Narrow(p).c_str(), 15.0f) : nullptr;
    };
    g.font = load(L"segoeui.ttf");
    g.bold = load(L"seguisb.ttf");
    if (!g.bold) g.bold = load(L"segoeuib.ttf");
    g.mono = load(L"consola.ttf");
    ImFont* fallback = (!g.font || !g.bold || !g.mono) ? io.Fonts->AddFontDefault() : nullptr;
    if (!g.font) g.font = fallback;
    if (!g.bold) g.bold = g.font;
    if (!g.mono) g.mono = fallback;
    io.FontDefault = g.font;
}

static void StyleUi() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0;
    s.WindowRounding = 0;
    s.ChildRounding = 8;
    s.ChildBorderSize = 0;
    s.ScrollbarSize = 6;
    s.ScrollbarRounding = 3;
    s.PopupRounding = 6;
    s.PopupBorderSize = 1;
    s.ItemSpacing = ImVec2(8, 3);
    s.DisabledAlpha = 1.0f;
    s.ScaleAllSizes(g_gui.dpi);
    s.FontSizeBase = 15.0f;
    s.FontScaleDpi = g_gui.dpi;
    ImVec4* c = s.Colors;
    auto v = [](ImU32 x) { return ImGui::ColorConvertU32ToFloat4(x); };
    c[ImGuiCol_Text] = v(kUiText);
    c[ImGuiCol_TextDisabled] = v(kUiTextDim);
    c[ImGuiCol_WindowBg] = v(kUiBg);
    c[ImGuiCol_ChildBg] = v(kUiSide);
    c[ImGuiCol_Border] = v(kUiLine);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v(kUiRow);
    c[ImGuiCol_ScrollbarGrabHovered] = v(kUiRowHover);
    c[ImGuiCol_ScrollbarGrabActive] = v(kUiAction);
    c[ImGuiCol_TextSelectedBg] = v(IM_COL32(110, 110, 120, 160));
    c[ImGuiCol_PopupBg] = v(kUiCard);
    c[ImGuiCol_Header] = v(kUiRow);
    c[ImGuiCol_HeaderHovered] = v(kUiRowHover);
    c[ImGuiCol_HeaderActive] = v(kUiRowHover);
}

static int RunGui() {
    Gui& g = g_gui;
    g.here = ExeDir();
    ImGui_ImplWin32_EnableDpiAwareness();
    RefreshLight();

    POINT cur{}; GetCursorPos(&cur);
    const HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    g.dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    const RECT& wa = mi.rcWork;
    const int cx = (wa.left + wa.right) / 2, cy = (wa.top + wa.bottom) / 2;
    const int fw = std::min<int>(int(Px(kUiW)), wa.right - wa.left - 40), fh = std::min<int>(int(Px(kUiH)), wa.bottom - wa.top - 40);
    const int sw = int(Px(280)), sh = int(Px(190));
    g.full = { cx - fw / 2, cy - fh / 2, cx - fw / 2 + fw, cy - fh / 2 + fh };
    g.splash = { cx - sw / 2, cy - sh / 2, cx - sw / 2 + sw, cy - sh / 2 + sh };
    g.fullW = fw; g.fullH = fh;

    WNDCLASSW wc{};
    wc.lpfnWndProc = GuiProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"sc-offline";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    const wstring title = L"sc-offline " + Wide(SCO_VERSION) + L" - Star Citizen offline mod";
    HWND wnd = CreateWindowExW(0, L"sc-offline", title.c_str(), WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               g.splash.left, g.splash.top, sw, sh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) return kExitError;
    g.wnd = wnd;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(wnd, 20, &dark, sizeof(dark));
    const int round = 2;
    DwmSetWindowAttribute(wnd, 33, &round, sizeof(round));
    const COLORREF edge = RGB(40, 40, 44);
    g.dwmBorder = SUCCEEDED(DwmSetWindowAttribute(wnd, 34, &edge, sizeof(edge)));
    const MARGINS shadow{ 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(wnd, &shadow);
    SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!CreateDevice(wnd)) {
        DropDevice();
        DestroyWindow(wnd);
        MessageBoxW(nullptr, L"sc-offline couldn't open its window (Direct3D 11 isn't available).\n\n"
                             L"Run it from a terminal instead, for example: sc-offline.exe play", L"sc-offline", MB_ICONERROR);
        return kExitError;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    LoadFonts();
    StyleUi();
    ImGui_ImplWin32_Init(wnd);
    ImGui_ImplDX11_Init(g.dev, g.ctx);

    g.introStart = Seconds();
    RenderFrame();
    ShowWindow(wnd, SW_SHOWNORMAL);
    g.introStart = Seconds();

    bool quit = false;
    while (!quit) {
        DWORD wait = 0;
        if (IsIconic(wnd) || g.occluded) wait = 250;
        else if (!g.animating) wait = 500;
        else if (!g.focused) wait = 66;
        if (wait) MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) quit = true;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (quit) break;
        if (GetTickCount64() - g.lastRefresh >= 1500) RefreshLight();
        if (!IsIconic(wnd)) RenderFrame();
        RunAction();
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DropDevice();
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
    "  --window          open the window (from a terminal, or under Wine/Proton)\n"
    "\n"
    "exit codes: 0 ok, 1 error, 2 Easy Anti-Cheat active, 3 the game is running\n";

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && !_wcsicmp(argv[1], L"--self-test")) return SelfTest(Narrow(argv[2]));
#ifdef SCO_UPDATE_TEST
    if (argc == 5 && !_wcsicmp(argv[1], L"--apply-zip")) return ApplyZipForTest(argv[2], argv[3], argv[4]);
#endif
    if ((argc == 7 || argc == 8) && !_wcsicmp(argv[1], L"--helper"))
        return Helper(argv[2], argv[3], wcstoul(argv[4], nullptr, 10), argv[5], argv[6], argc == 8 ? argv[7] : L"");
    // Double-clicked with no arguments: the window (issue #20). From a terminal, or with any
    // argument, the CLI runs exactly as before. `sc-offline.exe --console` forces the console run.
    if (argc == 1 && !GuiChild() && OwnsConsole() && !OnWine()) { FreeConsole(); return RunGui(); }
    // `--window` opens it from a terminal or under Wine/Proton too.
    if (argc == 2 && !_wcsicmp(argv[1], L"--window") && !GuiChild()) return RunGui();
    if (GuiChild()) { std::setvbuf(stdout, nullptr, _IONBF, 0); SetConsoleOutputCP(CP_UTF8); }
    if (argc == 4 && !_wcsicmp(argv[1], L"--delete-logs"))
        return DeleteSessionLogs(argv[2], _wcstoui64(argv[3], nullptr, 10));
    if (argc == 4 && !_wcsicmp(argv[1], L"--elevated-update"))
        return ElevatedUpdate(ExeDir(), argv[2], argv[3]);   // S4: no network as administrator

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
    CreateDirectoryW(data.c_str(), nullptr);   // a fresh unzip may not have it yet
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
    // 0.7.5: the Discord status is the discord plugin's. An old `discord_presence = off` turns that
    // plugin off once and the line goes, so the launcher's Discord switch owns it from then on.
    if (cfg.discordOff && !dry && SetPluginDisabled(here, L"discord", true) && DropIniKey(here + L"\\sc-offline.ini", "discord_presence"))
        Out("[i] discord_presence = off: the Discord plugin is turned off (data\\plugins\\discord\\disabled)\n");

    // 0. Updates (issue #14). Never blocks play: no network, a timeout or any odd reply is just skipped.
    if (command == L"update" || (cfg.checkUpdates && !dry && (command == L"play" || command == L"status"))) {
        const bool explicitUpdate = command == L"update";
        std::string why;
        const UpdateInfo u = CheckLatest(explicitUpdate ? 15000 : 3000, cfg.prereleases, &why);
        if (u.tag.empty()) {
            if (explicitUpdate) { Out("Update:   %s\n", why.c_str()); return why.rfind("up to date", 0) == 0 ? kExitOk : kExitError; }
        } else {
            Out("Update:   sc-offline %s is available (you have %s)\n", u.tag.c_str(), SCO_VERSION);
            if (command == L"status") Out("          run `sc-offline.exe update` to install it\n");
            else if (dry) Out("[dry-run] would download %s, check its SHA-256 and replace the program files\n", u.zipName.c_str());
            else if (GameRunning()) Out("[!] close the game before updating\n");
            else if (!OwnsConsoleInput()) Out("          (not asking: no console to answer in)\n");
            else if (AskYes("Update now? Your saves and sc-offline.ini are kept.")) {
                if (ApplyUpdate(here, u)) {
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
    pc.lan = cfg.firewall && cfg.multiplayer; pc.allow = cfg.multiplayerAllow;

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
        SetVar(L"SC_OFFLINE_PLUGINS", cfg.plugins ? L"on" : L"off");
        SetVar(L"SC_OFFLINE_MULTIPLAYER", cfg.multiplayer ? L"on" : L"off");
        SetVar(L"SC_OFFLINE_MULTIPLAYER_ALLOW", cfg.multiplayerAllow);
        SetVar(L"SC_OFFLINE_ASOP", cfg.asop ? L"on" : L"off");
        SetVar(L"SC_OFFLINE_ASOP_FLEET_LIST", cfg.asopFleetList);
        SetVar(L"SC_OFFLINE_MINING", cfg.mining ? L"on" : L"off");
        SetVar(L"SC_OFFLINE_MINING_DEBUG", cfg.miningDebug ? L"on" : L"off");
        SetVar(L"SC_USER", g.userDir);
        if (dry)
            Out("[dry-run] would set SC_OFFLINE_BOOT_MAP=%ls SC_OFFLINE_START_SHIP=%ls SC_OFFLINE_START=%ls SC_OFFLINE_PLUGINS=%ls\n"
                "          SC_OFFLINE_ASOP=%ls; SC_OFFLINE_MOD_LOG, _SPAWN_FILE, _SHIPS_FILE under %ls; SC_USER=%ls\n",
                cfg.bootMap.c_str(), cfg.startShip.c_str(), cfg.start.c_str(), cfg.plugins ? L"on" : L"off",
                cfg.asop ? L"on" : L"off", data.c_str(), g.userDir.c_str());
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
    HANDLE closed = play ? CreateEventW(nullptr, TRUE, FALSE, (eventName + L"-closed").c_str()) : nullptr;
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
        // Take the mod out and undo the PC changes now, before any question below: a question
        // left unanswered must not keep the game folder modded or the firewall rules in place.
        if (closed) SetEvent(closed);
        if (WaitForSingleObject(helper, 120000) == WAIT_OBJECT_0) {
            DWORD code = 1; GetExitCodeProcess(helper, &code);
            if (code == 0) Out("Mod:      removed from Bin64; PC changes undone. Safe to go online.\n");
            else Out("[!] the helper couldn't undo everything (exit %lu); click Uninstall. See data\\launcher.log\n", code);
        }
        if (cfg.crashReports && OwnsConsoleInput()) OfferCrashReport(here, ParentDir(bin), sessionStart, checks.gameBuild);
        if (cfg.cleanLogs && OwnsConsoleInput()) OfferToCleanLogs(ParentDir(bin), sessionStart);
        else if (cfg.cleanLogs) Out("Logs:     not asking (no console to answer in); run from a window to be asked\n");
    }
    if (WaitForSingleObject(helper, 0) != WAIT_OBJECT_0)
        Out("Mod:      the helper removes it from Bin64 and undoes the PC changes as this window closes.\n");
    CloseHandle(helper);
    if (closed) CloseHandle(closed);
    if (rc) PauseIfOwnConsole();
    return rc;
}
