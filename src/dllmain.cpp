#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include "common.h"
#include "patches.h"
#include "hooks.h"
#include "teleport.h"
#include "spawner.h"
#include "loadout.h"
#include "npc.h"
#include "build.h"
#include "cvars.h"
#include "missions.h"
#include "contracts.h"
#include "quantum.h"
#include "ammo.h"
#include "services.h"
#include "menu.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

#pragma comment(linker, "/export:DirectInput8Create=C:\\Windows\\System32\\dinput8.DirectInput8Create,@1")

static const wchar_t* kTargetModule = L"StarCitizen.exe";

static bool SelfModuleContains(const wchar_t* needle) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    bool hit = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (StrStrIW(me.szModule, needle)) { hit = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return hit;
}

static bool ProcessRunningContains(const wchar_t* needle) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool hit = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (StrStrIW(pe.szExeFile, needle)) { hit = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return hit;
}

static bool ServiceRunning(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    bool running = false;
    if (SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS st{};
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED)
            running = true;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return running;
}

static const wchar_t* AntiCheatProcess() {
    const wchar_t* procs[] = { L"EACLauncher", L"EasyAntiCheat", L"start_protected_game" };
    for (const wchar_t* p : procs)
        if (ProcessRunningContains(p)) return p;
    return nullptr;
}

static bool AntiCheatPresent() {
    if (SelfModuleContains(L"EasyAntiCheat")) { Log("[AC] EasyAntiCheat module is loaded in-process."); return true; }

    if (const wchar_t* p = AntiCheatProcess()) { Log("[AC] anti-cheat process running: %ls", p); return true; }

    const wchar_t* svcs[] = { L"EasyAntiCheat", L"EasyAntiCheat_EOS", L"EasyAntiCheatEOS" };
    for (const wchar_t* s : svcs)
        if (ServiceRunning(s)) { Log("[AC] anti-cheat service running: %ls", s); return true; }

    return false;
}

static bool g_offline = false;

static void StartOffline() {
    g_offline = ApplyOfflinePatches();
    if (!g_offline) return;
    InstallHooks(g_text);
    ResolveQuantumApi(g_text, g_rdata);
    if (ResolveTeleportApi(g_text, g_rdata)) {
        ResolveSpawnApi(g_text, g_rdata);
        ResolveLoadoutApi(g_text, g_rdata);
        ResolveNpcApi(g_text);
        ResolveBuildApi(g_text, g_rdata);
        ResolveCVarsApi(g_text, g_rdata);
        ResolveMissionsApi(g_text, g_rdata);
        ResolveContractsApi(g_text, g_rdata);
        ResolveAmmoApi(g_text);
        ResolveHangarsApi(g_text, g_rdata);
    }
}

static void LogStartup() {
    LogOfflinePatches();
    LogHooks();
    LogQuantum();
    if (g_tp.ok) Log("[+] teleport: ready (F7 = save spot, F8 = go there)");
    else         Log("[!] teleport: unavailable (see above)");
    if (SpawnerReady()) Log("[+] ship spawner: ready (M = menu)");
    else                Log("[!] ship spawner: unavailable (see above)");
    if (g_offline)
        Log("[i] check Game.log: \"Process sc-client started\" line should show bOnline[0].");
    else
        Log("[!] Game is running ONLINE.");
}

static void OnMainThreadTick() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 100) return;
    last = now;

    ProcessShipMenu(now);
    ProcessLoadout();
    ProcessNpcs();
    ProcessBuild();
    ProcessCVars();
    ProcessQuantum();
    ProcessMissions();
    ProcessContracts();
    ProcessAmmo();
    TeleportTick(now);
}

static HHOOK g_msgHook = nullptr;

static LRESULT CALLBACK GetMsgProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) OnMainThreadTick();
    return CallNextHookEx(g_msgHook, code, wp, lp);
}

static BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t title[128];
    if (GetWindowTextW(hwnd, title, 128) && wcsstr(title, L"Star Citizen")) { *reinterpret_cast<HWND*>(out) = hwnd; return FALSE; }
    return TRUE;
}

static void RunMainThreadService() {
    if (!g_tp.ok) return;
    LoadSavedSpot(StartingOverDaymar());
    HWND hwnd = nullptr;
    while (!hwnd) { EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd)); if (!hwnd) Sleep(1000); }
    g_msgHook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, GetWindowThreadProcessId(hwnd, nullptr));
    if (!g_msgHook) { Log("[tp] could not hook the game's message loop (%lu); hotkeys disabled", GetLastError()); return; }
    if (SpawnerReady()) Menu_Start(hwnd);
    for (;;) { PostMessageW(hwnd, WM_NULL, 0, 0); Sleep(200); }
}

static DWORD WINAPI ModThread(LPVOID param) {
    HMODULE self = static_cast<HMODULE>(param);
    OpenConsole();
    Log("ChrisWareOffline");
    Log("Join our Discord server https://discord.gg/bUAuKMJUJs");

    HMODULE game = GetModuleHandleW(kTargetModule);
    if (!game) game = GetModuleHandleW(nullptr);
    Log("[+] game module base: 0x%p", reinterpret_cast<void*>(game));

    if (AntiCheatPresent()) {
        Log("[!] ANTI-CHEAT DETECTED. This mod is OFFLINE-ONLY and will not run here.");
        if (g_hooksInstalled) {
            Log("[!] Hooks are already installed, so staying loaded (idle). Close the game.");
            for (;;) Sleep(1000);
        }
        Log("[!] Launch Star Citizen offline with no EAC, then load this. Unloading in 5s.");
        Sleep(5000);
        FreeLibraryAndExitThread(self, 0);
    }
    Log("[+] No anti-cheat present. Safe to proceed (offline instance confirmed).");

    LogStartup();
    ReadStartOptions();
    RunMainThreadService();

    for (;;) Sleep(1000);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitLog();
        if (!AntiCheatProcess()) StartOffline();
        CreateThread(nullptr, 0, ModThread, hModule, 0, nullptr);
    }
    return TRUE;
}
