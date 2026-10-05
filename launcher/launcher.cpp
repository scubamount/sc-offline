// sc-offline.exe — starts Star Citizen with the offline mod and takes the mod back out when the
// game closes. It replaces launch_offline.bat and does the same steps, plus finding the game:
//
//   1. find the game's Bin64 folder: --game <path>, else `game =` in sc-offline.ini, else the
//      usual Roberts Space Industries folders on every fixed drive (Wine's C: and Z: included);
//   2. set the SC_OFFLINE_* variables the mod reads, pointing at data\ next to this exe;
//   3. copy dinput8.dll into Bin64 (asks for administrator rights only if the copy is refused)
//      and data\OfflineDB\default_1.xml into LIVE\user\client\0;
//   4. start StarCitizen.exe, wait until every StarCitizen.exe has exited, then delete
//      Bin64\dinput8.dll so the RSI Launcher never sees it.
//
// On Linux, sc-offline.sh runs this inside the game's Wine prefix (see that file).
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/version.h"

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

// Only waits for a key when this exe owns its console window (double-clicked), so the window
// doesn't vanish before the player reads the message. From a terminal it returns at once.
static void PauseIfOwnConsole() {
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) > 1) return;
    std::printf("\nPress Enter to close this window.");
    std::getchar();
}

static int Fail(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::printf("[!] "); std::vprintf(fmt, ap); std::printf("\n");
    va_end(ap);
    PauseIfOwnConsole();
    return 1;
}

// sc-offline.ini: `key = value` lines, '#' starts a comment. Unknown keys are reported, not ignored.
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
        if (const size_t h = line.find(L'#'); h != wstring::npos) line.erase(h);
        const size_t eq = line.find(L'=');
        if (Trim(line).empty()) continue;
        if (eq == wstring::npos) { std::printf("[!] sc-offline.ini line %d: expected key = value\n", lineNo); continue; }
        const wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if      (!_wcsicmp(k.c_str(), L"game"))       c.game = v;
        else if (!_wcsicmp(k.c_str(), L"channel"))    { if (!v.empty()) c.channel = v; }
        else if (!_wcsicmp(k.c_str(), L"boot_map"))   c.bootMap = v;
        else if (!_wcsicmp(k.c_str(), L"start_ship")) c.startShip = v;
        else if (!_wcsicmp(k.c_str(), L"start"))      c.start = v;
        else std::printf("[!] sc-offline.ini line %d: unknown key '%ls'\n", lineNo, k.c_str());
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

static bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool hit = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !hit; ok = Process32NextW(snap, &pe))
        hit = !_wcsicmp(pe.szExeFile, kGameExe);
    CloseHandle(snap);
    return hit;
}

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{}; DWORD n = 0;
    const bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

// Runs this exe again as administrator with the same arguments and returns its exit code.
static int RelaunchElevated() {
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = PathGetArgsW(GetCommandLineW());
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
        return Fail("Administrator rights were refused, so the mod can't be copied into the game folder.");
    std::printf("Continuing in the administrator window.\n");
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

static void SetVar(const wchar_t* name, const wstring& v) {
    SetEnvironmentVariableW(name, v.empty() ? nullptr : v.c_str());   // empty = unset, like the .bat's `set X=`
}

static bool OnWine() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

int wmain(int argc, wchar_t** argv) {
    std::printf("sc-offline launcher (ChrisWareOffline %s)\n\n", CWO_VERSION);
    const wstring here = ExeDir();
    const wstring modDll = here + L"\\dinput8.dll";
    const wstring data = here + L"\\data";

    Config cfg;
    const wstring ini = here + L"\\sc-offline.ini";
    if (!ReadConfig(ini, cfg)) std::printf("[i] no sc-offline.ini next to this exe; using defaults\n");

    wstring gameArg;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--game") && i + 1 < argc) gameArg = argv[++i];
        else return Fail("unknown argument '%ls'. Usage: sc-offline.exe [--game <Star Citizen folder>]", argv[i]);
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
        const std::vector<wstring> found = DetectBin64(cfg.channel);
        if (found.empty())
            return Fail("couldn't find Star Citizen (%ls). Open sc-offline.ini and set\n"
                        "    game = <your StarCitizen folder>", cfg.channel.c_str());
        bin = found.front();
        for (size_t i = 1; i < found.size(); ++i)
            std::printf("[i] also found %ls (set game = in sc-offline.ini to use it)\n", found[i].c_str());
    }
    std::printf("Game:  %ls\n", bin.c_str());
    if (OnWine()) std::printf("Wine:  yes (dinput8 must load native: sc-offline.sh sets WINEDLLOVERRIDES)\n");

    if (!IsFile(modDll)) return Fail("dinput8.dll is missing: %ls\n    Extract the whole zip into one folder and run this from there.", modDll.c_str());
    if (GameRunning()) return Fail("%ls is already running. Close it first.", kGameExe);

    // 2. What the mod reads (same names and values launch_offline.bat used).
    const wstring user = bin + L"\\..\\user\\client\\0";
    SetVar(L"SC_OFFLINE_BOOT_MAP", cfg.bootMap);
    SetVar(L"SC_OFFLINE_MOD_LOG", data + L"\\mod.log");
    SetVar(L"SC_OFFLINE_SPAWN_FILE", data + L"\\spawn.txt");
    SetVar(L"SC_OFFLINE_SHIPS_FILE", data + L"\\ships.txt");
    SetVar(L"SC_OFFLINE_START", cfg.start);
    SetVar(L"SC_OFFLINE_START_SHIP", cfg.startShip);
    SetVar(L"SC_USER", user);

    // 3. Put the mod in.
    const wstring target = bin + L"\\dinput8.dll";
    if (!CopyFileW(modDll.c_str(), target.c_str(), FALSE)) {
        const DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED && !IsElevated()) {
            std::printf("[i] the game folder needs administrator rights; asking Windows for them\n");
            return RelaunchElevated();
        }
        return Fail("couldn't copy dinput8.dll into %ls (error %lu)", bin.c_str(), e);
    }
    std::printf("Mod:   copied into Bin64\n");

    SHCreateDirectoryExW(nullptr, user.c_str(), nullptr);   // fine if it already exists
    const wstring loadout = data + L"\\OfflineDB\\default_1.xml";
    if (!CopyFileW(loadout.c_str(), (user + L"\\default_1.xml").c_str(), FALSE))
        std::printf("[!] couldn't copy data\\OfflineDB\\default_1.xml (error %lu) - you will start with no ships\n", GetLastError());

    // 4. Play, then take the mod back out.
    const wstring exe = bin + L"\\" + kGameExe;
    wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, bin.c_str(), &si, &pi)) {
        std::printf("[!] couldn't start %ls (error %lu)\n", exe.c_str(), GetLastError());
    } else {
        std::printf("\nPlaying. Leave this window open: when the game closes, the mod is removed.\n");
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        while (GameRunning()) Sleep(2000);   // the game can hand over to a second StarCitizen.exe
    }

    for (int tries = 0; tries < 30; ++tries) {
        if (DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            std::printf("Mod:   removed from Bin64. The RSI Launcher is safe to use.\n");
            return 0;
        }
        Sleep(1000);
    }
    return Fail("couldn't remove %ls (error %lu)\n    DELETE IT YOURSELF before launching through the RSI Launcher.", target.c_str(), GetLastError());
}
