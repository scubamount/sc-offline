#include "missions.h"
#include "cvars.h"
#include "spawner.h"
#include "teleport.h"
#include "hooks.h"
#include "menu.h"
#include "sco/game/missions.h"
#include "world_caps.h"
#include <share.h>
#include <cstring>

using CreateMissionFn = void(__fastcall*)(const void* request);
static CreateMissionFn g_createMission = nullptr;

static uintptr_t* g_missionSettings = nullptr;

using ConsoleCmdFn = void(__fastcall*)(void* args);
static ConsoleCmdFn g_loadAllMissions = nullptr;
static bool         g_scriptsLoaded = false;
static int __fastcall NoArgCount(void*) { return 1; }
static const char* __fastcall NoArg(void*, int) { return "mission_load_all"; }
static void* const kNoArgsVtbl[] = { nullptr, reinterpret_cast<void*>(&NoArgCount), reinterpret_cast<void*>(&NoArg), nullptr };
static void* const kNoArgs[] = { const_cast<void**>(kNoArgsVtbl) };


using LoadXmlFileFn   = void*(__fastcall*)(uintptr_t system, void* out, const char* path, bool reuseStrings);
using LoadXmlBufferFn = void*(__fastcall*)(uintptr_t system, void* out, const char* buffer, size_t size, bool reuseStrings);
using FileChangeFn    = void(__fastcall*)(uintptr_t listener, const void* info);
using MissionMgrFn    = uintptr_t(__fastcall*)(uintptr_t subsumption);
constexpr size_t kLoadXmlFileSlot = 0x3C0, kLoadXmlBufferSlot = 0x3B8;
constexpr int    kMaxScripts = 1024;
struct Script { char game[200]; char disk[MAX_PATH]; char* data; size_t size; };
static Script*       g_scripts = nullptr;
static int           g_scriptCount = 0;
static bool          g_scriptsAdded = false;
static uintptr_t*    g_subsumption = nullptr;
static uintptr_t*    g_system = nullptr;
static FileChangeFn  g_fileChange = nullptr;
static LoadXmlFileFn g_loadXmlFileOrig = nullptr;


static bool ScriptsFolder(char* dir, size_t n) { return DataFilePath(dir, static_cast<DWORD>(n), "scripts"); }

static void CollectScripts(const char* dir, const char* game) {
    char pattern[MAX_PATH];
    sprintf_s(pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char disk[MAX_PATH], sub[200];
        sprintf_s(disk, "%s\\%s", dir, fd.cFileName);
        sprintf_s(sub, "%s%s%s", game, *game ? "/" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { CollectScripts(disk, sub); continue; }
        const char* dot = strrchr(fd.cFileName, '.');
        if (!dot || _stricmp(dot, ".xml") || g_scriptCount >= kMaxScripts) continue;
        Script& s = g_scripts[g_scriptCount++];
        strcpy_s(s.game, sub);
        strcpy_s(s.disk, disk);
        s.data = nullptr;
        s.size = 0;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static bool ReadScript(Script& s) {
    if (s.data) return true;
    FILE* f = _fsopen(s.disk, "rb", _SH_DENYNO);
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = size > 0 ? static_cast<char*>(malloc(static_cast<size_t>(size) + 1)) : nullptr;
    const bool ok = data && fread(data, 1, static_cast<size_t>(size), f) == static_cast<size_t>(size);
    fclose(f);
    if (!ok) { free(data); return false; }
    data[size] = 0;
    s.data = data;
    s.size = static_cast<size_t>(size);
    return true;
}

static void* __fastcall LoadXmlFileHook(uintptr_t system, void* out, const char* path, bool reuseStrings) {
    for (int i = 0; path && i < g_scriptCount; ++i) {
        Script& s = g_scripts[i];
        if (_stricmp(path, s.game)) continue;
        if (!ReadScript(s)) break;
        return VCall<void*>(system, kLoadXmlBufferSlot, out, static_cast<const char*>(s.data), s.size, reuseStrings);
    }
    return g_loadXmlFileOrig(system, out, path, reuseStrings);
}

static void ScriptId(const Script& s, char id[37]) {
    id[0] = 0;
    static const char* const kTags[] = { "<Subactivity ID=\"", "<Mission ID=\"" };
    for (const char* tag : kTags) {
        const char* p = s.data ? strstr(s.data, tag) : nullptr;
        if (!p || strnlen(p += strlen(tag), 36) < 36) continue;
        memcpy(id, p, 36);
        id[36] = 0;
        return;
    }
}

static bool ScriptReady(int i, const bool* added, char (*ids)[37]) {
    const Script& s = g_scripts[i];
    if (!s.data) return true;
    if (strstr(s.data, "<Mission ")) {
        const size_t stem = strlen(s.game) - 4;
        for (int j = 0; j < g_scriptCount; ++j)
            if (j != i && !added[j] && !_strnicmp(g_scripts[j].game, s.game, stem) && g_scripts[j].game[stem] == '/') return false;
    }
    static const char kCall[] = "FunctionArchetype=\"";
    for (const char* p = strstr(s.data, kCall); p; p = strstr(p, kCall)) {
        p += sizeof(kCall) - 1;
        for (int j = 0; j < g_scriptCount; ++j)
            if (j != i && !added[j] && ids[j][0] && !_strnicmp(p, ids[j], 36)) return false;
    }
    return true;
}

static void AddOurScripts() {
    char dir[MAX_PATH];
    if (!ScriptsFolder(dir, sizeof(dir))) return;
    if (!g_scripts) g_scripts = static_cast<Script*>(calloc(kMaxScripts, sizeof(Script)));
    if (!g_scripts) return;
    CollectScripts(dir, "");
    if (!g_scriptCount) { Log("[missions] no scripts of ours in %s", dir); return; }
    const uintptr_t subsumption = g_subsumption ? *g_subsumption : 0, system = g_system ? *g_system : 0;
    if (!subsumption || !system || !g_fileChange) {
        Log("[missions] %d scripts of ours, but the script library wasn't found (subsumption %d, system %d, file change %d)",
            g_scriptCount, subsumption != 0, system != 0, g_fileChange != nullptr);
        return;
    }
    const uintptr_t manager = VCall<uintptr_t>(subsumption, 0xA0);
    // sco-core's game pack finds it (sco/game/missions.h): the reads and the canary live there.
    const char* why = nullptr;
    const uintptr_t library = sco::game::missions::ScriptLibrary(manager, &why);
    if (!library) { Log("[missions] %s", why); return; }

    uint8_t* slot = reinterpret_cast<uint8_t*>(Rd<uintptr_t>(system) + kLoadXmlFileSlot);
    if (!g_loadXmlFileOrig) {
        g_loadXmlFileOrig = reinterpret_cast<LoadXmlFileFn>(Rd<uintptr_t>(reinterpret_cast<uintptr_t>(slot)));
        const uintptr_t hook = reinterpret_cast<uintptr_t>(&LoadXmlFileHook);
        DWORD err = 0;
        if (!WriteCode(slot, reinterpret_cast<const uint8_t*>(&hook), sizeof(hook), err)) {
            Log("[missions] couldn't hook LoadXmlFromFile (%lu)", err);
            g_loadXmlFileOrig = nullptr;
            return;
        }
    }
    static char ids[kMaxScripts][37];
    static bool done[kMaxScripts];
    for (int i = 0; i < g_scriptCount; ++i) {
        ReadScript(g_scripts[i]);
        ScriptId(g_scripts[i], ids[i]);
        done[i] = false;
    }
    int added = 0, placed = 0;
    while (placed < g_scriptCount) {
        int next = -1;
        for (int i = 0; i < g_scriptCount && next < 0; ++i)
            if (!done[i] && ScriptReady(i, done, ids)) next = i;
        if (next < 0) {
            for (int i = 0; i < g_scriptCount && next < 0; ++i)
                if (!done[i]) next = i;
            Log("[missions] %s calls a function that also needs it; added anyway", g_scripts[next].game);
        }
        done[next] = true;
        ++placed;
        struct { uint8_t path[8]; uint32_t type; uint8_t a, b; uint8_t pad[2]; } info = {};
        if (!MakeCryString(info.path, g_scripts[next].game)) { Log("[missions] no CryString constructor; scripts not added"); return; }
        info.type = 1;
        __try {
            g_fileChange(library + 8, &info);
            ++added;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[missions] fault while adding script %s", g_scripts[next].game);
        }
    }
    g_scriptsAdded = added == g_scriptCount;
    Log("[missions] added %d of our %d scripts to the game's script library (from %s)", added, g_scriptCount, dir);
}

bool HaveScript(const char* path) {
    if (!g_scriptsAdded || !path) return false;
    for (int i = 0; i < g_scriptCount; ++i)
        if (!_stricmp(g_scripts[i].game, path)) return true;
    return false;
}

static void LoadMissionScripts() {
    if (g_scriptsLoaded) return;
    g_scriptsLoaded = true;
    if (!g_loadAllMissions) { Log("[missions] mission_load_all not found; the game can't find any mission"); return; }
    __try { AddOurScripts(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[missions] fault while adding our scripts"); }
    __try {
        const DWORD t0 = GetTickCount();
        g_loadAllMissions(const_cast<void**>(kNoArgs));
        Log("[missions] loaded the game's mission scripts in %lu ms (Game.log: \"Mission loaded [...]\")", GetTickCount() - t0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[missions] fault while loading the mission scripts");
    }
}

// The addresses come from sco-core's missions.* rows (sco/game/world.h).
void ResolveMissionsApi(const Section&, const Section&) {
    if (WorldCapability("missions.load_all"))
        g_loadAllMissions = reinterpret_cast<ConsoleCmdFn>(sco::Sig("missions.load_all"));
    if (WorldCapability("missions.scripts")) {
        g_subsumption = reinterpret_cast<uintptr_t*>(sco::Sig("missions.subsumption"));
        g_fileChange  = reinterpret_cast<FileChangeFn>(sco::Sig("missions.file_change"));
        g_system      = reinterpret_cast<uintptr_t*>(sco::Sig("missions.xml_system"));
    }
    Log("[missions] script loading: load all %s, subsumption %s, file change %s, system %s", g_loadAllMissions ? "ok" : "MISSING",
        g_subsumption ? "ok" : "MISSING", g_fileChange ? "ok" : "MISSING", g_system ? "ok" : "MISSING");
    if (WorldCapability("missions.start")) {
        g_missionSettings = reinterpret_cast<uintptr_t*>(sco::Sig("missions.settings"));
        g_createMission   = reinterpret_cast<CreateMissionFn>(sco::Sig("missions.create"));
        return;
    }
    Log("[missions] mission create handler not found; missions disabled (see the [core] lines in mod.log)");
}

bool StartMissionNearPlayer(const char* missionId, const char* name, float minM, float maxM) {
    if (!g_createMission) { Log("[missions] mission create handler not found; '%s' not started", name); return false; }
    const uint64_t player = LocalPlayerEntityId();
    if (!player) { Log("[missions] you're not spawned yet (no player id)"); return false; }
    char json[512];
    snprintf(json, sizeof(json),
             "{\"MissionID\":\"%s\",\"PlayerID\":%lld,\"UsePrecisePosition\":false,"
             "\"SpawnDistanceMin\":%.1f,\"SpawnDistanceMax\":%.1f}",
             missionId, static_cast<long long>(player), static_cast<double>(minM), static_cast<double>(maxM));
    const char* request = json;
    LoadMissionScripts();
    __try {
        if (g_missionSettings && *g_missionSettings) *reinterpret_cast<int32_t*>(*g_missionSettings + 0xC) = 1;
        g_createMission(&request);
        Log("[missions] asked the game to start '%s' near you (Game.log shows [EVMissionManager] lines): %s", name, json);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[missions] fault while starting '%s'", name);
        return false;
    }
}

void ProcessMissions() {
    static int stage = 0;
    if (stage == 2 || !g_tp.ok) return;
    uintptr_t actor, entity;
    bool live = false;
    __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!live) return;
    if (stage == 0) {
        // The original sends the bare name; keep the ai_ form only for builds that register it.
        float v;
        Menu_RunConsole(GetCVarNow("ai_SubsumptionEnableDebugNodes", v) && !GetCVarNow("SubsumptionEnableDebugNodes", v)
                        ? "ai_SubsumptionEnableDebugNodes 1" : "SubsumptionEnableDebugNodes 1");
        stage = 1;
        return;
    }
    stage = 2;
    LoadMissionScripts();
}
