#include "missions.h"
#include "spawner.h"
#include "teleport.h"
#include "hooks.h"
#include "menu.h"
#include <share.h>

using CreateMissionFn = void(__fastcall*)(const void* request);
static CreateMissionFn g_createMission = nullptr;

static uintptr_t* g_missionSettings = nullptr;

static void FindMissionLogging(const Section& text, const Section& rdata) {
    const uint8_t* fmt = FindCString(rdata, "[EVMissionManager] Spawn Mission Request - Parsed MissionID: %s (%s)");
    const uint8_t* lea = fmt ? FindRipLea(text, 0x48, 0x8D, 0x0D, fmt) : nullptr;
    if (lea && lea - 0x4B >= text.base && BytesMatch(lea - 0x4B, "48 8B 0D ?? ?? ?? ?? 83 79 0C 00"))
        g_missionSettings = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(lea - 0x44 + Rel32(lea - 0x48)));
}

using ConsoleCmdFn = void(__fastcall*)(void* args);
static ConsoleCmdFn g_loadAllMissions = nullptr;
static bool         g_scriptsLoaded = false;
static int __fastcall NoArgCount(void*) { return 1; }
static const char* __fastcall NoArg(void*, int) { return "mission_load_all"; }
static void* const kNoArgsVtbl[] = { nullptr, reinterpret_cast<void*>(&NoArgCount), reinterpret_cast<void*>(&NoArg), nullptr };
static void* const kNoArgs[] = { const_cast<void**>(kNoArgsVtbl) };

static void FindLoadAllMissions(const Section& text, const Section& rdata) {
    const uint8_t* name = FindCString(rdata, "mission_load_all");
    const uint8_t* lea = name ? FindRipLea(text, 0x48, 0x8D, 0x15, name) : nullptr;
    if (!lea || lea - 0xC < text.base || !BytesMatch(lea - 0xC, "4C 8D 05")) return;
    const uint8_t* h = lea - 0xC + 7 + Rel32(lea - 0x9);
    if (h >= text.base && h + 0x20 <= text.base + text.size && BytesMatch(h, "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 08 83 F8 02 7C"))
        g_loadAllMissions = reinterpret_cast<ConsoleCmdFn>(const_cast<uint8_t*>(h));
}

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

static void FindScriptLoading(const Section& text, const Section& rdata) {
    const uint8_t* h = reinterpret_cast<const uint8_t*>(g_loadAllMissions);
    if (h && BytesMatch(h + 0x2E, "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 A0 00 00 00"))
        g_subsumption = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(h + 0x35 + Rel32(h + 0x31)));
    const uint8_t* name = FindCString(rdata, "void __cdecl Subsumption::XmlFileLibrary::OnFileChange(const struct SFileChangeInfo &)");
    const uint8_t* lea = name ? FindRipLea(text, 0x48, 0x8D, 0x05, name) : nullptr;
    for (const uint8_t* f = lea; f && f > lea - 0x400; --f) {
        if (!BytesMatch(f, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24")) continue;
        if (BytesMatch(f + 0x203, "48 8B 0D ?? ?? ?? ??")) {
            g_fileChange = reinterpret_cast<FileChangeFn>(const_cast<uint8_t*>(f));
            g_system = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(f + 0x20A + Rel32(f + 0x206)));
        }
        break;
    }
}

static bool ScriptsFolder(char* dir, size_t n) {
    char path[MAX_PATH];
    if (!ShipsFilePath(path, sizeof(path))) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash) return false;
    *slash = 0;
    return sprintf_s(dir, n, "%s\\scripts", path) > 0;
}

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
    const uint8_t* loadAll = manager ? reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(Rd<uintptr_t>(manager) + 0x48)) : nullptr;
    if (!loadAll || !BytesMatch(loadAll + 0x32, "48 8B 4B")) { Log("[missions] the mission manager's script library wasn't found"); return; }
    const uintptr_t library = Rd<uintptr_t>(manager + loadAll[0x35]);
    if (!library) { Log("[missions] the script library isn't there yet"); return; }

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

void ResolveMissionsApi(const Section& text, const Section& rdata) {
    FindMissionLogging(text, rdata);
    FindLoadAllMissions(text, rdata);
    FindScriptLoading(text, rdata);
    Log("[missions] script loading: load all %s, subsumption %s, file change %s, system %s", g_loadAllMissions ? "ok" : "MISSING",
        g_subsumption ? "ok" : "MISSING", g_fileChange ? "ok" : "MISSING", g_system ? "ok" : "MISSING");
    const uint8_t* name = FindCString(rdata, "dgs.subsumption.mission.create");
    uint8_t* const end = text.base + text.size - 7;
    for (uint8_t* p = text.base + 0x13; name && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x15 || p + 7 + Rel32(p + 3) != name) continue;
        if (!BytesMatch(p - 0x13, "48 8B 59 18") || !BytesMatch(p - 0xF, "48 8D 05")) continue;
        const uint8_t* h = p - 0xF + 7 + Rel32(p - 0xC);
        if (h >= text.base && h + 0x20 <= text.base + text.size
            && BytesMatch(h, "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 B9 C0 00 00 00 E8")) {
            g_createMission = reinterpret_cast<CreateMissionFn>(const_cast<uint8_t*>(h));
            return;
        }
    }
    Log("[missions] mission create handler not found; missions disabled");
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
        Menu_RunConsole("ai_SubsumptionEnableDebugNodes 1");
        stage = 1;
        return;
    }
    stage = 2;
    LoadMissionScripts();
}
