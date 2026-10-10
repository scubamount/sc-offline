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
#include "travel.h"
#include "version.h"
#include "services.h"
#include "outfits.h"
#include "menu.h"
#include "hotkeys.h"
#include "builtins/builtins.h"
#include "sco/app.h"
#include "sco/caps.h"
#include "sco/log.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sco/game/signatures.h"
#include "sco/game/system.h"
#include "sco/runtime.h"
#include "sco_lua.h"
#include <filesystem>
#include <iterator>

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
static bool g_outfitsOk = false;
static bool g_buildOk = false;   // the free camera build mode uses
static bool g_signaturesResolved = false;
static const char* g_quitHookStatus = nullptr;   // "[app] game quit hook: ..." in LogStartup

static void InstallQuitHook();

static void StartOffline() {
    g_offline = ApplyOfflinePatches();
    if (!g_offline) return;
    InstallHooks(g_text);
    ResolveQuantumApi(g_text, g_rdata);
    // sco-core's signature rows, resolved where teleport used to scan (after the patches and
    // hooks, so the bytes scanned are the same as before). The report goes out with LogStartup.
    if (!sco::game::RegisterGameSignatures()) Log("[!] sco-core's game signature tables did not all register");
    sco::ResolveAll(sco::ModuleImage());
    g_signaturesResolved = true;
    EnableQuantumDrive();   // the pak.* rows: the new quantum drive's game data, served as it loads
    InstallQuitHook();
    if (ResolveTeleportApi()) {
        ResolveSpawnApi(g_text, g_rdata);
        g_outfitsOk = ResolveLoadoutApi(g_text, g_rdata);  // outfits ride the gear menu's loader
        ResolveNpcApi(g_text);
        g_buildOk = ResolveBuildApi(g_text, g_rdata);
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
    if (g_signaturesResolved) sco::LogSignatureReport(false);
    if (g_quitHookStatus) Log("[app] game quit hook: %s", g_quitHookStatus);
    if (g_tp.ok) Log("[+] teleport: ready (F7 = save spot, F8 = go there)");
    else         Log("[!] teleport: unavailable (see above)");
    if (SpawnerReady()) Log("[+] ship spawner: ready (M = menu)");
    else                Log("[!] ship spawner: unavailable (see above)");
    if (g_outfitsOk) Log("[+] outfits: ready (Squadron 42 tab, data/outfits.txt)");
    else             Log("[!] outfits: unavailable (they use the gear menu's loader; see [gear] above)");
    if (g_offline)
        Log("[i] check Game.log: \"Process sc-client started\" line should show bOnline[0].");
    else
        Log("[!] Game is running ONLINE.");
}

// --- sco-core's host kit (sco/app.h), on the game thread -----------------------------------------

static void SetCap(const char* name, bool ready) {
    const sco::Result r = sco::caps::Set(name, ready, ready ? nullptr : "unavailable on this game build (see mod.log)");
    if (r != sco::Result::Ok) Log("[!] capability %s: %s", name, sco::ResultName(r));
}

// One capability per feature, from the same readiness LogStartup prints.
static void SetFeatureCaps() {
    SetCap("offline", g_offline);
    SetCap("teleport", g_tp.ok);
    SetCap("spawn.ship", SpawnerReady());
    SetCap("crew", Menu_SeatControlAvailable());
    SetCap("npc", SpawnerReady());
    SetCap("outfits", g_outfitsOk);
    SetCap("loadout", g_outfitsOk);   // the gear menu's loader, which outfits ride
    SetCap("ammo", AmmoReady());
    SetCap("quantum.drive", QuantumDriveReady());
    SetCap("quantum.boost", QuantumBoostReady());
    SetCap("quantum", g_tp.ok);   // the Travel tab's requests all go through teleport
    SetCap("build", g_buildOk && SpawnerReady());
    SetCap("contracts", ContractsReady());
#ifdef SCO_BRIDGE_TITANLINK
    SetCap("titanlink", g_tp.ok);   // your pose and zone conversions come from teleport.spatial
#endif
#ifdef SCO_BRIDGE_VOXEL
    SetCap("voxel_bridge", g_tp.ok && SpawnerReady());   // teleport.spatial, plus the spawner for crates
#endif
}

static const sco::plugins::ScriptRuntime kLua{ sco_lua_load, sco_lua_unload };

static bool g_hostKitStarted = false;   // sco::app::Start ran (on the game thread)

static void StartHostKit() {
    sco::app::Platform pf;
    pf.hostVersion = SCO_TITLE;
    char dir[MAX_PATH];
    if (DataFilePath(dir, sizeof(dir), "plugins")) {   // data\plugins, beside ships.txt
        std::error_code ec;
        pf.pluginRoot = std::filesystem::absolute(dir, ec);
        if (ec) pf.pluginRoot = dir;
        // sco.storage: every plugin's database, the built-ins' included, in data\storage\<id>.db.
        pf.dataRoot = pf.pluginRoot.parent_path();
    } else {
        Log("[app] SC_OFFLINE_SHIPS_FILE is unset, so there is no data folder to load plugins from or keep storage in");
    }
    pf.pluginsEnabled = PluginsEnabled();
    pf.scripts = &kLua;
    // sc-offline's features as built-in plugins (src/builtins/builtins.h), loaded before any plugin
    // folder and with plugins on or off. Their capabilities still come from SetFeatureCaps.
    pf.builtins = kBuiltins;
    pf.nBuiltins = std::size(kBuiltins);
    // image stays nullptr: the features resolve their addresses in DllMain (StartOffline), before
    // this thread exists, so the signature rows were resolved there and reported by LogStartup.
    pf.setCapabilities = SetFeatureCaps;
    // The keys sc-offline handles itself (hotkeys.h); no plugin can bind them. The built-ins bind
    // F6, F7 and F8 to their commands through sco.ui like any plugin.
    pf.reservedChords = kReservedChords;
    pf.nReservedChords = kReservedChordCount;
    g_hostKitStarted = sco::app::Start(pf);
}

// game.exit and unload, once, on the game thread. Two triggers, whichever comes first: the game's
// own CSystem::Quit when it is about to end the process (QuitDetour), and WM_QUIT reaching the
// game's message loop (GetMsgProc). `trigger` names the one that fired.
static void StopHostKit(const char* trigger) {
    static bool stopped = false;
    if (stopped) return;
    stopped = true;
    Log("[app] game closing (%s): game.exit, unloading plugins", trigger);
    sco::app::Stop();
}

// --- the game's own Quit (sco-core's system.quit row) --------------------------------------------
//
// void CSystem::Quit(SDisconnectionInfo&& info, const int exitCode): rcx = CSystem*, rdx = info,
// r8d = exitCode. No stack or xmm arguments. Every call quits; it ends in one of two ways:
//   - fast shutdown, when the ExitOnQuit cvar (an ICVar* in CSystem, GetIVal != 0) or test mode
//     (a bool in CSystem) is set: "System Fast Shutdown (%s enabled)", then
//     TerminateProcess(GetCurrentProcess(), exitCode) inside the call. No WM_QUIT ever comes.
//     This is the menu Quit in LIVE.
//   - otherwise PostQuitMessage(exitCode) and a normal return; WM_QUIT then reaches GetMsgProc.
// The detour stops the host kit before the original only on the first path, testing the same two
// fields the original tests (their offsets read from its own instructions at +0x31a / +0x330),
// and leaves the second to the WM_QUIT trigger.
using QuitFn = void(__fastcall*)(void* system, void* info, int exitCode);
static QuitFn  g_quitOriginal = nullptr;
static int32_t g_quitCVarOffset = 0;      // ICVar* ExitOnQuit, in CSystem
static int32_t g_quitTestModeOffset = 0;  // bool test mode, in CSystem

// The original's test, step for step: cvar && cvar->GetIVal() (vtable +0x10), else test mode.
static bool QuitEndsProcess(void* system) {
    auto* sys = static_cast<uint8_t*>(system);
    void* cvar = *reinterpret_cast<void**>(sys + g_quitCVarOffset);
    if (cvar) {
        using GetIValFn = int(__fastcall*)(void* cvar);
        if ((*reinterpret_cast<GetIValFn* const*>(cvar))[2](cvar) != 0) return true;
    }
    return sys[g_quitTestModeOffset] != 0;
}

static void __fastcall QuitDetour(void* system, void* info, int exitCode) {
    if (sco::OnGameThread() && g_hostKitStarted && QuitEndsProcess(system)) StopHostKit("CSystem::Quit");
    g_quitOriginal(system, info, exitCode);
}

// After ResolveAll. Hooks CSystem::Quit when system.quit is OK and its fast-shutdown test is where
// this build has it; otherwise WM_QUIT stays the only trigger.
static void InstallQuitHook() {
    sco::game::QuitHook q;
    if (!sco::game::QuitFunction(q)) {
        g_quitHookStatus = "not installed (system.quit isn't OK, see [core] signatures; WM_QUIT only)";
        return;
    }
    // +0x31a  mov rcx,[r14+cvar]; test rcx,rcx; je; mov rax,[rcx]; call [rax+10h]; test eax,eax; jne
    // +0x330  cmp byte ptr [r14+testMode],0; je (PostQuitMessage)
    if (!BytesMatch(q.fn + 0x31a, "49 8B 8E ?? ?? ?? ?? 48 85 C9 74 0A 48 8B 01 FF 50 10 85 C0 75 0E "
                                  "41 80 BE ?? ?? ?? ?? 00 0F 84")) {
        g_quitHookStatus = "not installed (CSystem::Quit's fast-shutdown test moved; WM_QUIT only)";
        return;
    }
    g_quitCVarOffset     = *reinterpret_cast<const int32_t*>(q.fn + 0x31d);
    g_quitTestModeOffset = *reinterpret_cast<const int32_t*>(q.fn + 0x333);
    if (!HookFunction(q.fn, q.stolenBytes, reinterpret_cast<void*>(&QuitDetour), reinterpret_cast<void**>(&g_quitOriginal))) {
        g_quitHookStatus = "not installed (hook failed; WM_QUIT only)";
        return;
    }
    g_quitHookStatus = "installed";
}

static void RunFeatureTicks(DWORD now) {
    // Each built-in runs its feature's work from its tick subscription; these calls are the
    // fallback for a built-in that didn't load.
    if (!SpawnBuiltinOwnsTick()) ProcessShipMenu(now);
    if (!CrewBuiltinOwnsTick()) ProcessCrew(now);
    if (!LoadoutBuiltinOwnsTick()) { ProcessLoadout(); ProcessOutfits(); }
    if (!NpcBuiltinOwnsTick()) ProcessNpcs();
    if (!BuildBuiltinOwnsTick()) ProcessBuild();
    ProcessCVars();
    if (!QuantumBuiltinOwnsTick()) { ProcessQuantum(); ProcessTravel(now); }
    ProcessMissions();
    if (!ContractsBuiltinOwnsTick()) ProcessContracts();
    if (!AmmoBuiltinOwnsTick()) ProcessAmmo();
    TeleportTick(now);
}

static void OnMainThreadTick() {
    static bool hostKitStarted = false;
    if (!hostKitStarted) { hostKitStarted = true; StartHostKit(); }
    // Every pass, not throttled: the menu's frame (its tabs are plugin draws, game thread only)
    // and the keys bound through sco.ui.
    Menu_GameThreadFrame();
    if (g_hostKitStarted) Hotkeys_Poll();

    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 100) return;
    last = now;

    // Every feature below reads teleport's entity system, and none of them ran before this hook
    // started without teleport; the host kit's Tick runs either way.
    if (g_tp.ok) RunFeatureTicks(now);
    sco::app::Tick(now);   // plugin tasks, then "tick"
}

static HHOOK g_msgHook = nullptr;

static LRESULT CALLBACK GetMsgProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        OnMainThreadTick();
        // GetMessage hands the game's main loop WM_QUIT when the game closes: the last point we
        // reliably see on the game thread. Peeks that leave it queued don't count; a PeekMessage
        // that removes it may add PM_NOYIELD, so test the PM_REMOVE bit.
        if ((wp & PM_REMOVE) && reinterpret_cast<const MSG*>(lp)->message == WM_QUIT) StopHostKit("WM_QUIT");
    }
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

// The message hook is the bootstrap's, not teleport's: it starts the host kit (and with it the
// built-in plugins and plugins) on its first tick whether or not a feature resolved. It stays off
// only when the game is running online.
static void RunMainThreadService() {
    if (!g_offline) return;
    if (g_tp.ok) LoadSavedSpot(StartingOverDaymar());
    HWND hwnd = nullptr;
    while (!hwnd) { EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd)); if (!hwnd) Sleep(1000); }
    g_msgHook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, GetWindowThreadProcessId(hwnd, nullptr));
    if (!g_msgHook) { Log("[app] could not hook the game's message loop (%lu); hotkeys, menu and plugins disabled", GetLastError()); return; }
    if (SpawnerReady()) Menu_Start(hwnd);
    for (;;) { PostMessageW(hwnd, WM_NULL, 0, 0); Sleep(200); }
}

static DWORD WINAPI ModThread(LPVOID param) {
    HMODULE self = static_cast<HMODULE>(param);
    OpenConsole();
    Log(SCO_TITLE " (" SCO_BASED_ON ")");
    Log("Bug reports: https://github.com/scubamount/sc-offline/issues");

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

// sco-core's lines ([core], [plugin], [app], [status]) go to mod.log and the console like ours.
static void ForwardCoreLog(const char* line) { Log("%s", line); }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitLog();
        sco::SetLogSink(ForwardCoreLog);
        if (!AntiCheatProcess()) StartOffline();
        CreateThread(nullptr, 0, ModThread, hModule, 0, nullptr);
    }
    return TRUE;
}
