#include "spawner.h"
#include "patches.h"
#include "teleport.h"
#include "menu.h"
#include "npc.h"
#include "sco/caps.h"
#include "sco/game/actors.h"
#include "sco/game/features.h"
#include "sco/signatures.h"
#include "sco/status.h"
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <share.h>

using SpawnParamsCtorFn = void(__fastcall*)(void* params);
using SpawnSetClassFn   = void(__fastcall*)(void* params, uintptr_t entityClass);
using SpawnSetLocFn     = void(__fastcall*)(void* params, const void* quatTS, uint64_t zoneHostId);
using SpawnSetFlagsFn   = void(__fastcall*)(void* params, uint32_t flags);
using TeamCategoryFn    = uint8_t(__fastcall*)(uint8_t tag);
using FindSeatFn        = bool(__fastcall*)(uintptr_t controllableManager, uintptr_t itemUser, const void* filter);
using ForEachSeatFn     = void(__fastcall*)(uintptr_t itemContainer, const void* callback, uint32_t itemType);
using SeatPriorityFn    = uint32_t(__fastcall*)(uintptr_t seat);
using ActorOfUserFn     = uint64_t*(__fastcall*)(uintptr_t itemUser, uint64_t* actorHandle);
using HandleToIdFn      = uint64_t*(__fastcall*)(const void* handleField, uint64_t* entityId);
using ActorLinkFn       = uintptr_t(__fastcall*)(uintptr_t actor);
using ForceLinkFn       = void(__fastcall*)(uintptr_t actorLink, uint64_t seatEntityId);
using ForceDelinkFn     = void(__fastcall*)(uintptr_t actorLink);
using IsLinkedFn        = bool(__fastcall*)(uintptr_t actor);
using FindByNameFn      = uint64_t*(__fastcall*)(uintptr_t entitySystem, uint64_t* handle, const char* name);
using ToggleFlightReadyFn = void(__fastcall*)(uintptr_t entitySystem, uintptr_t dashboard, const void* callback);
using RequestFlyModeFn  = uint16_t(__fastcall*)(uintptr_t actionHandler, int mode);
using SetGodModeFn      = void(__fastcall*)(uintptr_t godModeState, uint8_t state);

struct SpawnApi {
    bool              ok = false;
    SpawnParamsCtorFn ctor = nullptr;
    SpawnSetClassFn   setClass = nullptr;
    SpawnSetLocFn     setLocation = nullptr;
    SpawnSetFlagsFn   setFlags = nullptr;
    TeamCategoryFn    teamCategory = nullptr;
    FindSeatFn        findSeat = nullptr;
    ForEachSeatFn     forEachSeat = nullptr;
    SeatPriorityFn    seatPriority = nullptr;
    ActorOfUserFn     actorOfUser = nullptr;
    HandleToIdFn      handleToId = nullptr;
    ActorLinkFn       actorLink = nullptr;
    ForceLinkFn       forceLink = nullptr;
    ForceDelinkFn     forceDelink = nullptr;
    IsLinkedFn        isLinked = nullptr;
    FindByNameFn      findEntityByName = nullptr;
    ToggleFlightReadyFn toggleFlightReady = nullptr;
    RequestFlyModeFn  requestFlyMode = nullptr;
    uintptr_t*        gameCVars = nullptr;
    uint32_t          flySpeedOffset = 0;
    SetGodModeFn      setGodMode = nullptr;
    uint32_t          godModeByte = 0;
    uintptr_t*        system = nullptr;
    int32_t           pathMgrSlot = 0, pathIdSlot = 0, attrSetSlot = 0;
    uint32_t          (__fastcall* attrTypeId)() = nullptr;
    void*             attrWriter = nullptr;
    uintptr_t*        game = nullptr;
    uintptr_t*        components = nullptr;
    uint8_t           teamTag = 0;   // the spawn helper's own tag (mov cl, imm8 at +0x4F)
};
static SpawnApi g_sp;

namespace actors = sco::game::actors;

// One capability of sco/game/actors.h or sco/game/features.h, set from its signature rows. When a
// row isn't OK, logs one line naming it and pointing at the [core] report, and returns false.
bool ActorsCapability(const char* name, const char* tag, const char* what) {
    const char* const* rows = nullptr;
    size_t count = 0, n = 0;
    const sco::game::actors::Capability* a = sco::game::actors::Capabilities(n);
    for (size_t i = 0; i < n && !rows; ++i)
        if (strcmp(a[i].name, name) == 0) { rows = a[i].rows; count = a[i].count; }
    const sco::game::features::Capability* f = sco::game::features::Capabilities(n);
    for (size_t i = 0; i < n && !rows; ++i)
        if (strcmp(f[i].name, name) == 0) { rows = f[i].rows; count = f[i].count; }
    if (!rows) { Log("[%s] %s disabled (no capability %s in sco-core)", tag, what, name); return false; }
    const sco::Result r = sco::caps::SetFromSignatures(name, rows, count);
    if (r != sco::Result::Ok) Log("[!] capability %s: %s", name, sco::ResultName(r));
    for (size_t i = 0; i < count; ++i) {
        const sco::SigResult* s = sco::SigLookup(rows[i]);
        if (s && s->state == sco::SigState::Ok) continue;
        Log("[%s] %s disabled (%s %s; see the [core] lines in mod.log)", tag, what, rows[i],
            s ? sco::SigStateName(s->state) : "unknown");
        return false;
    }
    return true;
}

template <typename T> static T Row(const char* id) { return reinterpret_cast<T>(sco::Sig(id)); }

// The addresses come from sco-core's actor rows (sco/game/actors.h); each part of the spawner
// switches on with its capability.
bool ResolveSpawnApi(const Section&, const Section&) {
    if (!ActorsCapability("spawn.helpers", "ship", "ship spawner")) return false;
    const uintptr_t genv = reinterpret_cast<uintptr_t>(g_isOnlineFlag) - 0x60E;
    if (!g_isOnlineFlag || reinterpret_cast<uintptr_t*>(genv + 0xA8) != g_tp.entitySystem) {
        Log("[ship] spawn params / seat helper not found; ship spawner disabled");
        return false;
    }
    g_sp.teamTag      = *sco::Sig("spawn.team_tag");
    g_sp.teamCategory = Row<TeamCategoryFn>("spawn.team_category");
    g_sp.setFlags     = Row<SpawnSetFlagsFn>("spawn.set_flags");
    g_sp.setClass     = Row<SpawnSetClassFn>("spawn.set_class");
    g_sp.setLocation  = Row<SpawnSetLocFn>("spawn.set_location");
    g_sp.ctor         = Row<SpawnParamsCtorFn>("spawn.params_ctor");
    g_sp.findSeat     = Row<FindSeatFn>("spawn.find_seat");
    g_sp.game         = reinterpret_cast<uintptr_t*>(genv + 0xA0);
    g_sp.components   = reinterpret_cast<uintptr_t*>(genv + 0xB0);
    g_sp.ok = true;

    if (ActorsCapability("spawn.seat_picker", "ship", "seat control (using the game's default seat choice)")) {
        g_sp.isLinked     = Row<IsLinkedFn>("spawn.is_linked");
        g_sp.forceDelink  = Row<ForceDelinkFn>("spawn.force_delink");
        g_sp.forEachSeat  = Row<ForEachSeatFn>("spawn.for_each_seat");
        g_sp.actorOfUser  = Row<ActorOfUserFn>("spawn.actor_of_user");
        g_sp.handleToId   = Row<HandleToIdFn>("spawn.handle_to_id");
        g_sp.actorLink    = Row<ActorLinkFn>("spawn.actor_link");
        g_sp.forceLink    = Row<ForceLinkFn>("spawn.force_link");
        g_sp.seatPriority = Row<SeatPriorityFn>("spawn.seat_priority");
    }

    if (ActorsCapability("spawn.find_by_name", "ship", "entity lookup by name (Daymar)"))
        g_sp.findEntityByName = Row<FindByNameFn>("spawn.find_entity_by_name");

    if (ActorsCapability("spawn.flight_ready", "ship", "Flight Ready event (will press R instead)"))
        g_sp.toggleFlightReady = Row<ToggleFlightReadyFn>("spawn.toggle_flight_ready");

    if (ActorsCapability("spawn.fly_mode", "noclip", "noclip"))
        g_sp.requestFlyMode = Row<RequestFlyModeFn>("spawn.request_fly_mode");
    if (ActorsCapability("spawn.fly_speed", "noclip", "fly speed setting (speed stays at the game's default)")) {
        g_sp.gameCVars      = Row<uintptr_t*>("spawn.game_cvars");
        g_sp.flySpeedOffset = static_cast<uint32_t>(Rel32(sco::Sig("spawn.fly_speed_scaler") + actors::kFlySpeedDisp));
    }

    if (ActorsCapability("spawn.god_mode", "god", "god mode")) {
        g_sp.setGodMode  = Row<SetGodModeFn>("spawn.set_god_mode");
        g_sp.godModeByte = static_cast<uint32_t>(Rel32(sco::Sig("spawn.set_god_mode") + actors::kGodModeByteDisp));
    }

    if (ActorsCapability("spawn.prefabs", "build", "prefab spawning (outposts/prefabs)")) {
        const uint8_t* s = sco::Sig("spawn.prefab_site");
        g_sp.system      = Row<uintptr_t*>("spawn.prefab_system");
        g_sp.pathMgrSlot = Rel32(s + actors::kPrefabPathMgrSlot);
        g_sp.pathIdSlot  = Rel32(s + actors::kPrefabPathIdSlot);
        g_sp.attrSetSlot = s[actors::kPrefabAttrSetSlot];
        g_sp.attrWriter  = sco::Sig("spawn.prefab_attr_writer");
        g_sp.attrTypeId  = Row<uint32_t(__fastcall*)()>("spawn.prefab_attr_type_id");
    }
    return true;
}

bool PrefabsReady() { return g_sp.ok && g_sp.attrTypeId; }

uint64_t LocalPlayerEntityId() {
    if (!g_sp.handleToId) return 0;
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return 0;
        uint64_t id = 0;
        g_sp.handleToId(reinterpret_cast<const void*>(actor + 8), &id);
        return id;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint64_t EntityIdOfHandle(const void* handle) {
    if (!g_sp.handleToId || !handle) return 0;
    __try {
        uint64_t id = 0;
        g_sp.handleToId(handle, &id);
        return id;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint64_t EntityIdOfComponent(uintptr_t component) {
    return component ? EntityIdOfHandle(reinterpret_cast<const void*>(component + 8)) : 0;
}

constexpr int        kMaxMenuShips = 2048;
static MenuShip      g_menuShips[kMaxMenuShips];
static volatile LONG g_menuShipCount = -1;
static volatile LONG g_menuWantShips = 0;
static SRWLOCK       g_menuLock = SRWLOCK_INIT;
// The status strip's text until a feature or a plugin sets one (sco::Status).
static const char    kMenuStatusDefault[] = "Pick a ship and press Spawn.";
static struct { bool pending; int index; MenuSpawnOptions opt; } g_spawnRequest;
static struct { bool pending; bool enemyWing; char cls[64]; float height; bool sit; bool flightReady; } g_classRequest;

static bool g_startDaymarPending = false;
static char g_startShip[64] = "DRAK_Cutlass_Black";
static bool g_pluginsOn = false;

void ReadStartOptions() {
    char v[64];
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_START", v, sizeof(v));
    g_startDaymarPending = n > 0 && n < sizeof(v) && _stricmp(v, "Daymar") == 0;
    const DWORD s = GetEnvironmentVariableA("SC_OFFLINE_START_SHIP", v, sizeof(v));
    if (s > 0 && s < sizeof(v)) strcpy_s(g_startShip, v);
    // plugins = on|off in sc-offline.ini; the launcher passes "on" or "off". Anything else is off.
    const DWORD p = GetEnvironmentVariableA("SC_OFFLINE_PLUGINS", v, sizeof(v));
    g_pluginsOn = p > 0 && p < sizeof(v) && _stricmp(v, "on") == 0;
    if (g_startDaymarPending && g_sp.ok && g_sp.findEntityByName)
        Log("[ship] start: over Daymar in %s (SC_OFFLINE_START=Daymar)", g_startShip);
    else if (g_startDaymarPending)
        Log("[!] start over Daymar requested but the spawner isn't available");
}

bool SpawnerReady() { return g_sp.ok; }
bool StartingOverDaymar() { return g_startDaymarPending; }
bool PluginsEnabled() { return g_pluginsOn; }

void SetMenuStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // One status line for features and plugins. sco::Status stores it and logs it once, as
    // "[status] ..." (this used to log "[ship] ...").
    sco::Status("%s", buf);
}

int Menu_ShipCount() {
    const LONG n = g_menuShipCount;
    if (n < 0) InterlockedExchange(&g_menuWantShips, 1);
    return n;
}

const MenuShip* Menu_Ships() { return g_menuShips; }

void Menu_RequestSpawn(int index, const MenuSpawnOptions& options) {
    AcquireSRWLockExclusive(&g_menuLock);
    g_spawnRequest.pending = true;
    g_spawnRequest.index = index;
    g_spawnRequest.opt = options;
    g_spawnRequest.opt.seatName[sizeof(g_spawnRequest.opt.seatName) - 1] = 0;
    ReleaseSRWLockExclusive(&g_menuLock);
}

static struct { bool modePending; bool on; bool speedPending; float speed; } g_noclipRequest;

void Menu_SetNoclip(bool on, float speed) {
    AcquireSRWLockExclusive(&g_menuLock);
    g_noclipRequest = { true, on, true, speed };
    ReleaseSRWLockExclusive(&g_menuLock);
}

void Menu_SetNoclipSpeed(float speed) {
    AcquireSRWLockExclusive(&g_menuLock);
    g_noclipRequest.speedPending = true;
    g_noclipRequest.speed = speed;
    ReleaseSRWLockExclusive(&g_menuLock);
}

static volatile LONG g_godModeOn = 1;

void Menu_SetGodMode(bool on) { InterlockedExchange(&g_godModeOn, on ? 1 : 0); }

void Menu_GetStatus(char* out, size_t n) {
    if (!sco::GetStatus(out, n)) strncpy_s(out, n, kMenuStatusDefault, _TRUNCATE);
}

static uintptr_t ClassRegistry() { return VCall<uintptr_t>(*g_tp.entitySystem, actors::kEsClassRegistry); }

bool EntityClassExists(const char* name) {
    const uintptr_t registry = ClassRegistry();
    return registry && VCall<uintptr_t>(registry, actors::kRegistryFindClass, name) != 0;
}

static int VehicleSize(uintptr_t entityClass) {
    const uintptr_t rec = VCall<uintptr_t>(*g_sp.game, actors::kGameVehicleRecord, entityClass);
    return rec ? static_cast<int>(Rd<uint32_t>(rec + actors::kVehicleRecordSize)) : 0;
}

// The Vanduul wing that the "Bengal + Vanduul wing" row spawns alongside its Bengal.
static const char* const kEnemySideClasses[] = {
    "VNCL_Blade_PU_AI_VAN", "VNCL_Scythe_PU_AI_VAN", "VNCL_Glaive_PU_AI_VAN",
};

static bool ClassInRegistry(const char* cls) {
    if (!g_sp.ok || !cls || !*cls) return false;
    bool found = false;
    __try {
        const uintptr_t registry = ClassRegistry();
        found = registry && VCall<uintptr_t>(registry, actors::kRegistryFindClass, cls) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = false;
    }
    return found;
}

// -1 until it has been read once on the game thread; the menu thread only
// ever reads the cached value, it never touches the registry itself.
static volatile LONG g_enemySide = -1;

static bool EnemySideInRegistry() {
    for (const char* c : kEnemySideClasses)
        if (ClassInRegistry(c)) return true;
    return false;
}

void RefreshEnemySide() {
    if (InterlockedCompareExchange(&g_enemySide, 0, 0) == 1) return;  // found once is enough
    // Not found (yet): the registry can fill after the first tick, so keep
    // retrying on a slow cadence instead of latching the first answer.
    static volatile LONG lastTry = 0;
    const DWORD now = GetTickCount();
    if (now - static_cast<DWORD>(InterlockedCompareExchange(&lastTry, 0, 0)) < 2000) return;
    InterlockedExchange(&lastTry, static_cast<LONG>(now));
    bool enemy = false;
    __try { enemy = EnemySideInRegistry(); } __except (EXCEPTION_EXECUTE_HANDLER) { enemy = false; }
    if (enemy) InterlockedExchange(&g_enemySide, 1);
}

bool Menu_EnemySideAvailable() { return InterlockedCompareExchange(&g_enemySide, 0, 0) == 1; }

void Menu_RequestSpawnClass(const char* cls, float heightAboveMe, bool sitInPilotSeat, bool flightReady, bool enemyWing) {
    AcquireSRWLockExclusive(&g_menuLock);
    g_classRequest = {};
    g_classRequest.pending = true;
    g_classRequest.enemyWing = enemyWing;
    strncpy_s(g_classRequest.cls, cls ? cls : "", _TRUNCATE);
    g_classRequest.height = heightAboveMe;
    g_classRequest.sit = sitInPilotSeat;
    g_classRequest.flightReady = flightReady;
    ReleaseSRWLockExclusive(&g_menuLock);
}

static const struct { const char* prefix; float meters; } kHullLengths[] = {
    { "RSI_Bengal", 980 }, { "AEGS_Javelin", 345 }, { "AEGS_Idris", 242 }, { "ORIG_890Jump", 210 },
    { "AEGS_Reclaimer", 158 }, { "RSI_Polaris", 155 }, { "DRAK_Ironclad", 128 }, { "ANVL_Carrack", 126 },
    { "AEGS_Hammerhead", 115 }, { "DRAK_Caterpillar", 111 }, { "MISC_Hull_C", 104 }, { "RSI_Perseus", 100 },
    { "MISC_Starfarer", 93 }, { "CRUS_Starlifter", 91 }, { "AEGS_Retaliator", 70 }, { "GAMA_Railen", 67 },
    { "ORIG_600i", 63 }, { "RSI_Constellation", 61 }, { "ORIG_400i", 60 }, { "MISC_Starlancer", 56 },
    { "DRAK_Corsair", 55 }, { "MISC_Hull_B", 50 }, { "RSI_Apollo", 50 }, { "ANVL_Asgard", 49 },
    { "VNCL_Glaive", 46 }, { "AEGS_Redeemer", 40 }, { "RSI_Zeus", 39 }, { "ANVL_Valkyrie", 38 },
    { "MISC_Freelancer", 38 }, { "DRAK_Cutlass", 38 }, { "AEGS_Vanguard", 38 }, { "CRUS_Star_Runner", 38 },
    { "VNCL_Scythe", 38 }, { "ARGO_MOLE", 37 }, { "ANVL_Paladin", 35 }, { "ESPR_Prowler", 32 },
    { "CRUS_Starfighter", 31 }, { "BANU_Defender", 30 }, { "CRUS_Spirit", 27 }, { "RSI_Mantis", 27 },
    { "ANVL_Gladiator", 26 }, { "DRAK_Vulture", 26 }, { "ARGO_SRV", 26 }, { "CNOU_Nomad", 26 },
    { "AEGS_Eclipse", 25 }, { "VNCL_Blade", 25 }, { "XNAA_SanTokYai", 25 }, { "ANVL_Terrapin", 24 },
    { "MISC_Prospector", 24 }, { "DRAK_Buccaneer", 24 }, { "XIAN_Scout", 24 }, { "ORIG_300i", 24 },
    { "ORIG_315p", 24 }, { "ORIG_325a", 24 }, { "ORIG_350r", 24 }, { "DRAK_Herald", 23 },
    { "AEGS_Sabre", 23 }, { "ANVL_Lightning", 23 }, { "CNOU_Mustang", 23 }, { "RSI_Scorpius", 23 },
    { "AEGS_Avenger", 22.5f }, { "ANVL_Hornet", 22.5f }, { "ANVL_Hurricane", 22 }, { "MISC_Hull_A", 22 },
    { "DRAK_Cutter", 21 }, { "AEGS_Gladius", 20 }, { "ANVL_Hawk", 20 }, { "ORIG_100i", 19 },
    { "ORIG_125a", 19 }, { "ORIG_135c", 19 }, { "RSI_Aurora", 18 }, { "ANVL_Arrow", 16.5f },
    { "MISC_Reliant", 15 }, { "ESPR_Talon", 15 }, { "MISC_Razor", 13 }, { "ANVL_C8", 12 },
    { "ORIG_85x", 12 }, { "ORIG_m50", 12 }, { "KRIG_P52_Merlin", 12 }, { "ARGO_MPUV", 9.5f },
    { "MISC_Fury", 6 }, { "ORIG_X1", 6 }, { "DRAK_Dragonfly", 6 },
};

static float HullLength(const char* name) {
    float meters = 0;
    size_t best = 0;
    for (const auto& h : kHullLengths) {
        const size_t len = strlen(h.prefix);
        if (len > best && _strnicmp(name, h.prefix, len) == 0) { best = len; meters = h.meters; }
    }
    return meters;
}

static int BuildMenuShips() {
    char path[MAX_PATH];
    if (!ShipsFilePath(path, sizeof(path))) return 0;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) return 0;
    const uintptr_t registry = ClassRegistry();
    int n = 0, unknown = 0;
    char line[128];
    while (n < kMaxMenuShips && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char* name = line + strspn(line, " \t");
        if (!*name) continue;
        const uintptr_t cls = VCall<uintptr_t>(registry, actors::kRegistryFindClass, static_cast<const char*>(name));
        if (!cls) { ++unknown; continue; }
        strncpy_s(g_menuShips[n].name, name, _TRUNCATE);
        g_menuShips[n++].size = VehicleSize(cls);
    }
    fclose(f);
    if (unknown) Log("[ship] %d names in the ships file aren't spawnable classes (skipped)", unknown);

    for (int i = 0; i < n; ++i) {
        if (g_menuShips[i].size) continue;
        size_t best = 0;
        for (int j = 0; j < n; ++j) {
            const size_t len = strlen(g_menuShips[j].name);
            if (j == i || !g_menuShips[j].size || len <= best || g_menuShips[i].name[len] != '_'
                || _strnicmp(g_menuShips[i].name, g_menuShips[j].name, len) != 0)
                continue;
            best = len;
            g_menuShips[i].size = g_menuShips[j].size;
        }
    }

    for (int i = 0; i < n; ++i) {
        MenuShip& s = g_menuShips[i];
        s.length = HullLength(s.name);
        if (s.length >= 300) s.size = 6;
        else if (s.size < 1 || s.size > 6) s.size = s.length > 0 ? (s.length <= 26 ? 2 : s.length <= 45 ? 3 : s.length <= 70 ? 4 : 5) : 2;
    }

    std::sort(g_menuShips, g_menuShips + n, [](const MenuShip& a, const MenuShip& b) {
        if (a.size != b.size) return a.size > b.size;
        if (a.length != b.length) return a.length > b.length;
        return _stricmp(a.name, b.name) < 0;
    });
    return n;
}

uintptr_t EntityComponent(uintptr_t entity, const char* type) {
    uint8_t tmp[16] = {};
    const uint16_t* id = VCall<const uint16_t*>(*g_sp.components, actors::kComponentsTypeId, tmp, type);
    if (!id) return 0;
    uint16_t typeId = *id;
    uint8_t out[16] = {};
    const uint64_t* h = VCall<const uint64_t*>(entity, actors::kEntityComponent, out, &typeId);
    return h ? (*h & kPtrMask) : 0;
}

static const char* SpawnShipInZone(const char* shipClass, uint64_t zoneId, const double pos[3], uint64_t& shipId,
                                   const double* rot = nullptr, const char* ocPath = nullptr) {
    __try {
        const uintptr_t es = *g_tp.entitySystem;
        const uintptr_t cls = VCall<uintptr_t>(ClassRegistry(), actors::kRegistryFindClass, shipClass);
        if (!cls) return "unknown entity class";

        alignas(16) uint8_t params[actors::kSpawnParamsSize] = {};
        g_sp.ctor(params);
        g_sp.setClass(params, cls);
        const struct { double rot[4]; double pos[3]; double scale; } where = {
            { rot ? rot[0] : 0, rot ? rot[1] : 0, rot ? rot[2] : 0, rot ? rot[3] : 1 }, { pos[0], pos[1], pos[2] }, 1.0 };
        g_sp.setLocation(params, &where, zoneId);
        g_sp.setFlags(params, actors::kSpawnFlags);

        uintptr_t batch = 0;
        VCall<void>(es, actors::kEsCreateBatch, &batch, "starcitzenofflinemods ship spawner",
                    static_cast<uint32_t>(g_sp.teamCategory(g_sp.teamTag)), static_cast<uint32_t>(0));
        if (!batch) return "couldn't create a spawn batch";
        uintptr_t attributes[2] = {};
        VCall<void>(es, actors::kEsSpawnAttributes, attributes);
        if (ocPath) {
            if (!attributes[0]) return "couldn't create spawn attributes";
            const uintptr_t paths = VCall<uintptr_t>(*g_sp.system, g_sp.pathMgrSlot);
            uint32_t pathId = paths ? VCall<uint32_t>(paths, g_sp.pathIdSlot, ocPath) : 0;
            const struct { const char* name; uint32_t* value; } field = { "ocFilename", &pathId };
            const struct { const void* field; void* writer; } setter = { &field, g_sp.attrWriter };
            VCall<void>(attributes[0], g_sp.attrSetSlot, "ocFilename", g_sp.attrTypeId(), uintptr_t(0), &setter);
        }
        uint64_t newId[2] = {};
        VCall<void>(batch, actors::kBatchSpawn, newId, params, attributes);
        uintptr_t owned = batch;
        VCall<void>(es, actors::kEsReleaseBatch, &owned);
        shipId = newId[0];
        return shipId ? nullptr : "the spawn batch gave no entity id";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while spawning";
    }
}

static const char* PlayerZonePos(const double offset[3], uint64_t& zoneId, double pos[3]) {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return "you're not spawned yet";
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return "you're not in a zone";
        zoneId = ZoneId(zone);
        if (!zoneId || ZoneFromId(zoneId) != zone) return "zone id lookup mismatch";
        Vec3Out(entity, 0x2B8, pos);
        for (int i = 0; i < 3; ++i) pos[i] += offset[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while reading your position";
    }
    return nullptr;
}

const char* SpawnEntityNearPlayer(const char* entityClass, const double offset[3], uint64_t& id) {
    if (!g_sp.ok) return "the spawner isn't available";
    uint64_t zoneId = 0;
    double pos[3] = {};
    if (const char* err = PlayerZonePos(offset, zoneId, pos)) return err;
    return SpawnShipInZone(entityClass, zoneId, pos, id);
}

const char* SpawnEntityInPlayerZone(const char* entityClass, const double pos[3], const double rot[4], uint64_t& id) {
    if (!g_sp.ok) return "the spawner isn't available";
    uint64_t zoneId = 0;
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return "you're not spawned yet";
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return "you're not in a zone";
        zoneId = ZoneId(zone);
        if (!zoneId || ZoneFromId(zoneId) != zone) return "zone id lookup mismatch";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while reading your zone";
    }
    return SpawnShipInZone(entityClass, zoneId, pos, id, rot);
}

const char* SpawnEntityInZone(const char* entityClass, uint64_t zoneId, const double pos[3], const double rot[4], uint64_t& id) {
    if (!g_sp.ok) return "the spawner isn't available";
    return SpawnShipInZone(entityClass, zoneId, pos, id, rot);
}

const char* SpawnPrefabInPlayerZone(const char* ocPath, const double pos[3], const double rot[4], uint64_t& id) {
    if (!PrefabsReady()) return "prefab spawning isn't available";
    uint64_t zoneId = 0;
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return "you're not spawned yet";
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return "you're not in a zone";
        zoneId = ZoneId(zone);
        if (!zoneId || ZoneFromId(zoneId) != zone) return "zone id lookup mismatch";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while reading your zone";
    }
    return SpawnShipInZone("StreamingObjectContainer", zoneId, pos, id, rot, ocPath);
}

static const char* SpawnShipAbovePlayer(const char* shipClass, double height, uint64_t& shipId) {
    const double up[3] = { 0, 0, height };
    return SpawnEntityNearPlayer(shipClass, up, shipId);
}

bool FindEntityByNameEx(const char* name, uintptr_t& entity, uint64_t& id) {
    entity = 0;
    id = 0;
    if (!g_sp.ok || !g_sp.findEntityByName || !name || !*name) return false;
    __try {
        uint64_t handle = 0;
        g_sp.findEntityByName(*g_tp.entitySystem, &handle, name);
        entity = handle & kPtrMask;
        if (entity && g_sp.handleToId) g_sp.handleToId(&handle, &id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        entity = 0;
    }
    return entity != 0;
}

constexpr const char* kDaymarEntity = "OOC_Stanton_2b_Daymar";
constexpr double      kDaymarRadius = 295000.0;
constexpr double      kArrivalAltitude = 3000.0;

static const char* SpawnShipAboveDaymar(const char* shipClass, uint64_t& shipId) {
    if (!g_sp.findEntityByName) return "can't look Daymar up in this game version";
    uint64_t zoneId = 0;
    __try {
        uint64_t handle = 0;
        g_sp.findEntityByName(*g_tp.entitySystem, &handle, kDaymarEntity);
        const uintptr_t daymar = handle & kPtrMask;
        if (!daymar) return "Daymar isn't loaded";
        const uintptr_t zone = VCall<uintptr_t>(daymar, actors::kEntityOocZone);
        if (!zone) return "Daymar has no zone";
        zoneId = ZoneId(zone);
        if (!zoneId || ZoneFromId(zoneId) != zone) return "Daymar zone lookup mismatch";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while looking Daymar up";
    }
    const double pos[3] = { 0, 0, kDaymarRadius + kArrivalAltitude };
    return SpawnShipInZone(shipClass, zoneId, pos, shipId);
}

// ---------------------------------------------------------------------------------------------
// Seats, crew and power
//
// Seats are enumerated with the game's own seat visitor (forEachSeat, resolved from the game's
// seat picker). For every seat we also work out WHO is in it, so the spawner can pick a specific
// seat, kick an NPC out of the seat you want, and put NPCs into seats.
// ---------------------------------------------------------------------------------------------

enum class SeatStep { NotReady, Sent, Evicting, NoSeat, Blocked, Fault };

constexpr uint64_t kUnknownOccupant = ~0ull;
constexpr uint32_t kPilotPriority   = 1000;
constexpr int      kMaxSeats        = 128;

struct SeatInfo {
    uintptr_t seat; uint32_t priority; bool occupied; uint64_t seatId; uint64_t occupant; int state; char name[64];
};
static SeatInfo g_seatList[kMaxSeats];
static int      g_seatListCount;

static uintptr_t EntityById(uint64_t id) { return id ? VCall<uintptr_t>(*g_tp.entitySystem, 0x120, id) : 0; }

static uintptr_t EntityByIdSafe(uint64_t id) {
    __try { return EntityById(id); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool EntityAlive(uint64_t id) { return g_tp.ok && EntityByIdSafe(id) != 0; }

static uintptr_t LocalPlayerEntity() {
    __try {
        uintptr_t actor = 0, entity = 0;
        return GetLocalPlayer(actor, entity) ? entity : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool SeatControl() { return g_sp.forEachSeat != nullptr; }
bool Menu_SeatControlAvailable() { return g_sp.ok && SeatControl(); }

static char __fastcall CollectSeat(uintptr_t seat) {
    if (!seat || g_seatListCount >= kMaxSeats) return 1;
    SeatInfo& s = g_seatList[g_seatListCount++];
    s = {};
    s.seat = seat;
    s.occupied = Rd<uint64_t>(seat + actors::kSeatOccupant) != 0;
    s.priority = g_sp.seatPriority(seat);
    const uintptr_t owner = Rd<uint64_t>(seat + 8) & kPtrMask;
    const char* name = owner ? VCall<const char*>(owner, actors::kEntityName) : nullptr;
    strncpy_s(s.name, name ? name : "?", _TRUNCATE);
    return 1;
}

// seat+0x158 is the field the game's own seat picker tests to skip taken seats. In the current
// build it holds the occupant's entity id directly (the first log showed 0x2e914d0fec =
// 200006242284 next to a seat with id 200006242283). The older guesses - an entity handle or a
// pointer to a component - are kept as fallbacks for other builds. Each attempt is fenced on its
// own so one faulting read can't hide the others.
static int  g_occupantLayout    = 0;   // 0 = not learned, 1 = handle, 2 = component pointer, 3 = raw entity id

static bool IsActorEntity(uint64_t id, uint64_t shipId, uint64_t seatId) {
    if (!id || id == shipId || id == seatId) return false;
    const uintptr_t e = EntityById(id);
    return e && (EntityComponent(e, "ISCItemUser") != 0 || EntityComponent(e, "Actor") != 0);
}

static uint64_t OccupantAsRawId(uint64_t raw, uint64_t shipId, uint64_t seatId) {
    __try { return IsActorEntity(raw, shipId, seatId) ? raw : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static uint64_t OccupantAsHandle(uintptr_t seat, uint64_t shipId, uint64_t seatId) {
    __try {
        uint64_t id = 0;
        g_sp.handleToId(reinterpret_cast<const void*>(seat + actors::kSeatOccupant), &id);
        return IsActorEntity(id, shipId, seatId) ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static uint64_t OccupantAsComponent(uint64_t raw, uint64_t shipId, uint64_t seatId) {
    __try {
        const uintptr_t p = raw & kPtrMask;
        if (!p || (p & 7)) return 0;
        uint64_t id = 0;
        g_sp.handleToId(reinterpret_cast<const void*>(p + 8), &id);
        return IsActorEntity(id, shipId, seatId) ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// NPCs this mod put in seats, so they can be identified even if decoding fails.
struct PlacedCrew { uint64_t seatId, npcId; };
static PlacedCrew g_placedCrew[128];
static int        g_placedCrewCount = 0;

static void RememberPlacedCrew(uint64_t seatId, uint64_t npcId) {
    for (int i = 0; i < g_placedCrewCount; ++i)
        if (g_placedCrew[i].seatId == seatId) { g_placedCrew[i].npcId = npcId; return; }
    if (g_placedCrewCount < 128) g_placedCrew[g_placedCrewCount++] = { seatId, npcId };
}

static void ForgetPlacedCrew(uint64_t npcId) {
    for (int i = 0; i < g_placedCrewCount; ++i)
        if (g_placedCrew[i].npcId == npcId) { g_placedCrew[i] = g_placedCrew[--g_placedCrewCount]; return; }
}

static uint64_t PlacedCrewIn(uint64_t seatId) {
    for (int i = 0; i < g_placedCrewCount; ++i)
        if (g_placedCrew[i].seatId == seatId) return g_placedCrew[i].npcId;
    return 0;
}

static uint64_t DecodeOccupant(const SeatInfo& s, uint64_t shipId) {
    uint64_t raw = 0;
    __try { raw = Rd<uint64_t>(s.seat + actors::kSeatOccupant); } __except (EXCEPTION_EXECUTE_HANDLER) { return kUnknownOccupant; }
    if (!raw) return 0;

    static const char* const kLayoutName[] = { "", "an entity handle", "a component pointer", "a raw entity id" };
    uint64_t id = 0;
    int layout = 0;
    if (!id && (!g_occupantLayout || g_occupantLayout == 3) && (id = OccupantAsRawId(raw, shipId, s.seatId)))   layout = 3;
    if (!id && (!g_occupantLayout || g_occupantLayout == 1) && (id = OccupantAsHandle(s.seat, shipId, s.seatId))) layout = 1;
    if (!id && (!g_occupantLayout || g_occupantLayout == 2) && (id = OccupantAsComponent(raw, shipId, s.seatId))) layout = 2;
    if (id) {
        if (!g_occupantLayout) { g_occupantLayout = layout; Log("[crew] seat occupant field read as %s", kLayoutName[layout]); }
        return id;
    }
    if (const uint64_t placed = PlacedCrewIn(s.seatId)) return placed;
    return kUnknownOccupant;
}

static int EnumerateSeats(uintptr_t ship, uint64_t shipId) {
    g_seatListCount = 0;
    const uintptr_t ports = EntityComponent(ship, "IItemPortContainer");
    if (!ports) return 0;
    const struct { void* invoke; uintptr_t manager; void* storage; } visitor = { reinterpret_cast<void*>(&CollectSeat), 1, nullptr };
    g_sp.forEachSeat(VCall<uintptr_t>(ports, actors::kPortsSeatContainer), &visitor, actors::kSeatItemType);
    const uint64_t me = LocalPlayerEntityId();
    for (int i = 0; i < g_seatListCount; ++i) {
        SeatInfo& s = g_seatList[i];
        g_sp.handleToId(reinterpret_cast<const void*>(s.seat + 8), &s.seatId);
        s.occupant = s.occupied ? DecodeOccupant(s, shipId) : 0;
        s.state = !s.occupant ? SeatState_Empty
                : s.occupant == kUnknownOccupant ? SeatState_Taken
                : (me && s.occupant == me) ? SeatState_You : SeatState_Npc;
    }
    return g_seatListCount;
}

// -1 = the ship entity isn't there (not streamed in yet, or gone); 0 = no seats / fault.
static int EnumerateShipSeats(uint64_t shipId) {
    if (!SeatControl() || !shipId) return 0;
    __try {
        const uintptr_t ship = EntityById(shipId);
        return ship ? EnumerateSeats(ship, shipId) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool reported = false;
        if (!reported) { reported = true; Log("[ship] seat enumeration faulted"); }
        g_seatListCount = 0;
        return 0;
    }
}

static const SeatInfo* SeatById(uint64_t seatId) {
    for (int i = 0; i < g_seatListCount; ++i)
        if (g_seatList[i].seatId == seatId) return &g_seatList[i];
    return nullptr;
}

static const SeatInfo* PilotSeat() {
    const SeatInfo* top = nullptr;
    for (int i = 0; i < g_seatListCount; ++i)
        if (!top || g_seatList[i].priority > top->priority) top = &g_seatList[i];
    return top;
}

static const char* SeatStateName(int state) {
    switch (state) {
    case SeatState_You: return "you";
    case SeatState_Npc: return "npc";
    case SeatState_Taken: return "taken";
    default: return "empty";
    }
}

static bool ContainsNoCase(const char* s, const char* needle, size_t n) {
    for (; *s; ++s) {
        size_t i = 0;
        while (i < n && s[i] && tolower(static_cast<unsigned char>(s[i])) == tolower(static_cast<unsigned char>(needle[i]))) ++i;
        if (i == n) return true;
    }
    return false;
}

// Every word in `words` (split on spaces / underscores) must appear somewhere in `name`.
static bool SeatNameMatches(const char* name, const char* words) {
    bool any = false;
    for (const char* p = words; *p; ) {
        p += strspn(p, " _");
        const size_t n = strcspn(p, " _");
        if (n) { any = true; if (!ContainsNoCase(name, p, n)) return false; }
        p += n;
    }
    return any;
}

static uintptr_t ActorOfEntity(uintptr_t entity) {
    const uintptr_t user = EntityComponent(entity, "ISCItemUser");
    if (!user) return 0;
    uint64_t handle = 0;
    g_sp.actorOfUser(user, &handle);
    return handle & kPtrMask;
}

// Works for any actor - you or an NPC.
static bool LinkEntityToSeat(uintptr_t entity, uint64_t seatId) {
    if (!entity || !seatId) return false;
    __try {
        const uintptr_t actor = ActorOfEntity(entity);
        if (!actor) return false;
        if (g_sp.isLinked(actor)) g_sp.forceDelink(g_sp.actorLink(actor));
        g_sp.forceLink(g_sp.actorLink(actor), seatId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool UnlinkEntity(uintptr_t entity) {
    if (!entity) return false;
    __try {
        const uintptr_t actor = ActorOfEntity(entity);
        if (!actor || !g_sp.isLinked(actor)) return false;
        g_sp.forceDelink(g_sp.actorLink(actor));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool UnseatEntityById(uint64_t id) { return UnlinkEntity(EntityByIdSafe(id)); }

// --- crew jobs: an NPC we spawned that still has to be put into its seat ---------------------

struct CrewJob { uint64_t npcId, shipId, seatId; DWORD since, nextTry; int links; };
constexpr int  kMaxCrewJobs = 32;
static CrewJob g_crewJobs[kMaxCrewJobs];
static int     g_crewJobCount = 0;

static void ForgetCrewJob(uint64_t npcId) {
    for (int i = 0; i < g_crewJobCount; ++i)
        if (g_crewJobs[i].npcId == npcId) { g_crewJobs[i] = g_crewJobs[--g_crewJobCount]; return; }
}

static bool CrewJobForSeat(uint64_t seatId) {
    for (int i = 0; i < g_crewJobCount; ++i)
        if (g_crewJobs[i].seatId == seatId) return true;
    return false;
}

// Unseats an NPC and deletes it. Never touches you.
static bool EvictSeat(const SeatInfo& s) {
    if (s.state != SeatState_Npc) return false;
    const uint64_t npc = s.occupant;
    UnlinkEntity(EntityByIdSafe(npc));
    const bool removed = CanRemoveEntities();
    if (removed) RemoveEntityById(npc);
    ForgetCrewJob(npc);
    ForgetPlacedCrew(npc);
    Log("[crew] %s NPC %llu from '%s'", removed ? "removed" : "unseated (RemoveEntity not found)",
        static_cast<unsigned long long>(npc), s.name);
    return true;
}

static const char* AddCrew(uint64_t shipId, uint64_t seatId, int npcIndex, DWORD now) {
    if (g_crewJobCount >= kMaxCrewJobs) return "too many crew spawns in progress";
    const char* npc = Menu_NpcName(npcIndex);
    if (!npc || !*npc) return "pick an NPC in the NPC list first";
    const double offset[3] = { 1.5, 1.5, 0.2 };
    uint64_t id = 0;
    if (const char* err = SpawnEntityNearPlayer(npc, offset, id)) return err;
    TrackSpawnedNpc(id);   // so "Clear NPCs" removes crew too
    g_crewJobs[g_crewJobCount++] = { id, shipId, seatId, now, now + 750, 0 };
    Log("[crew] spawned %s (%llu) for seat %llu", npc, static_cast<unsigned long long>(id), static_cast<unsigned long long>(seatId));
    return nullptr;
}

static bool CrewSeated(const CrewJob& j) {
    if (EnumerateShipSeats(j.shipId) <= 0) return false;
    const SeatInfo* s = SeatById(j.seatId);
    if (!s) return false;
    if (s->state == SeatState_Npc && s->occupant == j.npcId) return true;
    return s->state == SeatState_Taken && !g_occupantLayout;   // can't decode at all: trust that the link took
}

static void UpdateCrewJobs(DWORD now) {
    for (int i = 0; i < g_crewJobCount; ) {
        CrewJob& j = g_crewJobs[i];
        bool drop = false;
        if (static_cast<LONG>(now - j.nextTry) >= 0) {
            j.nextTry = now + 1500;
            if (j.links && CrewSeated(j)) {
                RememberPlacedCrew(j.seatId, j.npcId);
                Log("[crew] NPC %llu is seated", static_cast<unsigned long long>(j.npcId));
                drop = true;
            } else if (j.links >= 5 || now - j.since > 30000) {
                Log("[crew] couldn't seat NPC %llu (links tried: %d)", static_cast<unsigned long long>(j.npcId), j.links);
                drop = true;
            } else if (LinkEntityToSeat(EntityByIdSafe(j.npcId), j.seatId)) {
                ++j.links;
            }
        }
        if (drop) { g_crewJobs[i] = g_crewJobs[--g_crewJobCount]; continue; }
        ++i;
    }
}

// --- the target ship the Crew & seats panel works on ----------------------------------------

static struct { uint64_t shipId; char name[64]; } g_target;
static MenuSeat      g_menuSeats[kMaxSeats];
static int           g_menuSeatCount = -1;
static char          g_menuTargetName[64];
static volatile LONG g_seatPanelSeen = 0;

static void PublishSeats(int n) {
    AcquireSRWLockExclusive(&g_menuLock);
    if (!g_target.shipId) {
        g_menuSeatCount = -1;
    } else {
        g_menuSeatCount = n < 0 ? 0 : n;
        for (int i = 0; i < g_menuSeatCount; ++i) {
            strcpy_s(g_menuSeats[i].name, g_seatList[i].name);
            g_menuSeats[i].priority = g_seatList[i].priority;
            g_menuSeats[i].state = g_seatList[i].state;
            g_menuSeats[i].id = g_seatList[i].seatId;
        }
        snprintf(g_menuTargetName, sizeof(g_menuTargetName), "%s%s", g_target.name, n < 0 ? " (not loaded)" : "");
    }
    ReleaseSRWLockExclusive(&g_menuLock);
}

static void RefreshTargetSeats() {
    PublishSeats(g_target.shipId ? EnumerateShipSeats(g_target.shipId) : -1);
}

static void SetTarget(uint64_t shipId, const char* name) {
    g_target.shipId = shipId;
    strncpy_s(g_target.name, name ? name : "ship", _TRUNCATE);
    RefreshTargetSeats();
}

static const char* TargetShipImIn() {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return "you're not spawned yet";
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return "you're not in a zone";
        const uint64_t id = ZoneId(zone);
        const uintptr_t ship = EntityById(id);
        if (!ship || !EntityComponent(ship, "IItemPortContainer")) return "you're not inside a ship - stand or sit in it first";
        const char* name = ZoneName(zone);
        g_target.shipId = id;
        strncpy_s(g_target.name, name ? name : "ship", _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while reading your ship";
    }
    RefreshTargetSeats();
    return nullptr;
}

uint64_t TargetShipId() { return g_target.shipId; }

uint64_t PlayerShipId() {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return 0;
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return 0;
        const uint64_t id = ZoneId(zone);
        const uintptr_t ship = EntityById(id);
        return ship && EntityComponent(ship, "IItemPortContainer") ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int Menu_GetSeats(MenuSeat* out, int max, char* shipName, size_t shipNameLen) {
    InterlockedExchange(&g_seatPanelSeen, static_cast<LONG>(GetTickCount()));
    AcquireSRWLockShared(&g_menuLock);
    const int n = g_menuSeatCount;
    for (int i = 0; i < n && i < max; ++i) out[i] = g_menuSeats[i];
    if (shipName && shipNameLen) strncpy_s(shipName, shipNameLen, n >= 0 ? g_menuTargetName : "", _TRUNCATE);
    ReleaseSRWLockShared(&g_menuLock);
    return n;
}


enum SeatActionKind { SA_None, SA_Sit, SA_Kick, SA_AddCrew, SA_FillCrew, SA_ClearCrew, SA_FlightReady, SA_TargetMine,
                      SA_StandUp, SA_StandAll };
static struct { int kind; uint64_t seatId; int npc; bool replace; int dash; } g_seatAction;

static void QueueSeatAction(int kind, uint64_t seatId = 0, int npc = -1, bool replace = false, int dash = -1) {
    AcquireSRWLockExclusive(&g_menuLock);
    g_seatAction = { kind, seatId, npc, replace, dash };
    ReleaseSRWLockExclusive(&g_menuLock);
}

void Menu_TargetShipImIn()                                   { QueueSeatAction(SA_TargetMine); }
void Menu_RequestSit(unsigned long long seatId, bool replace) { QueueSeatAction(SA_Sit, seatId, -1, replace); }
void Menu_RequestKick(unsigned long long seatId)             { QueueSeatAction(SA_Kick, seatId); }
void Menu_RequestAddCrew(unsigned long long seatId, int npc) { QueueSeatAction(SA_AddCrew, seatId, npc); }
void Menu_RequestFillCrew(int npc)                           { QueueSeatAction(SA_FillCrew, 0, npc); }
void Menu_RequestClearCrew()                                 { QueueSeatAction(SA_ClearCrew); }
void Menu_RequestFlightReady()                               { QueueSeatAction(SA_FlightReady); }
void Menu_RequestStandUp(unsigned long long seatId)          { QueueSeatAction(SA_StandUp, seatId); }
void Menu_RequestStandAll()                                  { QueueSeatAction(SA_StandAll); }

// --- getting you into a seat ---------------------------------------------------------------

static struct {
    uint64_t id; DWORD since; DWORD lastSend; int sends; int evictions;
    bool flightReady, replaceNpc, startedAboard, lastWasEvict, namedFallback, seatsLogged;
    int mode; uint64_t wantSeat; uint64_t sentSeat; char name[64]; char seatName[48];
} g_seatJob;
static SeatInfo g_lastSeat;
static bool     g_haveLastSeat = false;

static bool PlayerAboard(uint64_t shipId, const char* shipClass) {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return false;
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return false;
        if (ZoneId(zone) == shipId) return true;
        const char* name = ZoneName(zone);
        return name && _strnicmp(name, shipClass, strlen(shipClass)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void StartSeatJob(uint64_t shipId, const char* shipName, int mode, const char* seatName, bool replaceNpc,
                         bool flightReady, uint64_t wantSeat, DWORD now) {
    g_seatJob = {};
    g_seatJob.id = shipId;
    g_seatJob.since = now;
    g_seatJob.mode = mode;
    g_seatJob.replaceNpc = replaceNpc;
    g_seatJob.flightReady = flightReady;
    g_seatJob.wantSeat = wantSeat;
    strncpy_s(g_seatJob.name, shipName, _TRUNCATE);
    strncpy_s(g_seatJob.seatName, seatName ? seatName : "", _TRUNCATE);
    g_seatJob.startedAboard = PlayerAboard(shipId, g_seatJob.name);
    g_haveLastSeat = false;
}

static const SeatInfo* ChooseSeat() {
    if (g_seatJob.wantSeat) return SeatById(g_seatJob.wantSeat);

    if (g_seatJob.mode == SeatMode_Named && g_seatJob.seatName[0]) {
        const SeatInfo *free = nullptr, *npc = nullptr;
        for (int i = 0; i < g_seatListCount; ++i) {
            const SeatInfo& s = g_seatList[i];
            if (!SeatNameMatches(s.name, g_seatJob.seatName)) continue;
            if (s.state == SeatState_You) return &s;
            if (s.state == SeatState_Empty && (!free || s.priority > free->priority)) free = &s;
            if (s.state == SeatState_Npc && (!npc || s.priority > npc->priority)) npc = &s;
        }
        if (free) return free;
        if (npc && g_seatJob.replaceNpc) return npc;
        if (!g_seatJob.namedFallback) {
            g_seatJob.namedFallback = true;
            Log("[ship] no %sseat matches '%s'; using the pilot seat rules instead", npc ? "free " : "", g_seatJob.seatName);
        }
    }

    const SeatInfo *top = PilotSeat(), *free = nullptr, *mine = nullptr;
    for (int i = 0; i < g_seatListCount; ++i) {
        const SeatInfo& s = g_seatList[i];
        if (s.state == SeatState_Empty && (!free || s.priority > free->priority)) free = &s;
        if (s.state == SeatState_You) mine = &s;
    }
    if (top && (top->state == SeatState_Empty || top->state == SeatState_You)) return top;
    if (top && top->state == SeatState_Npc && g_seatJob.replaceNpc) return top;
    return free ? free : mine;
}

static SeatStep SeatPlayer(uintptr_t playerEntity) {
    const int n = EnumerateShipSeats(g_seatJob.id);
    if (n < 0) return SeatStep::NotReady;
    if (n == 0) return SeatStep::NoSeat;
    if (!g_seatJob.seatsLogged) {
        // Like the original: one line per seat job naming every seat, so a tester's mod.log
        // shows the names the "Board in a seat by name" mode matches against.
        g_seatJob.seatsLogged = true;
        char line[1024]; int len = 0;
        for (int i = 0; i < n && len < static_cast<int>(sizeof(line)) - 80; ++i) {
            const SeatInfo& s = g_seatList[i];
            const int w = snprintf(line + len, sizeof(line) - len, "%s%s(%u%s)", i ? ", " : "", s.name, s.priority,
                                   s.state == SeatState_Empty ? "" : ", taken");
            if (w < 0) break;
            len += w;
        }
        line[sizeof(line) - 1] = 0;
        Log("[ship] %d seats: %s", n, line);
    }

    const SeatInfo* s = ChooseSeat();
    if (!s) { g_haveLastSeat = false; return g_seatJob.wantSeat ? SeatStep::Blocked : SeatStep::NoSeat; }
    g_lastSeat = *s;
    g_haveLastSeat = true;

    switch (s->state) {
    case SeatState_You:
        g_seatJob.sentSeat = s->seatId;
        return SeatStep::Sent;
    case SeatState_Taken:
        return SeatStep::Blocked;   // somebody we can't identify - don't double-seat
    case SeatState_Npc:
        if (!g_seatJob.replaceNpc) return SeatStep::Blocked;   // only reachable for a seat picked in the menu
        if (++g_seatJob.evictions > 3) {
            Log("[ship] the NPC in '%s' keeps coming back; taking another seat", s->name);
            g_seatJob.replaceNpc = false;
            if (g_seatJob.wantSeat) return SeatStep::Blocked;
            return SeatStep::NotReady;
        }
        EvictSeat(*s);
        return SeatStep::Evicting;
    default:
        break;
    }
    if (!LinkEntityToSeat(playerEntity, s->seatId)) return SeatStep::NotReady;
    g_seatJob.sentSeat = s->seatId;
    Log("[ship] linking you into '%s' (priority %u%s)", s->name, s->priority, s->priority >= kPilotPriority ? ", pilot" : "");
    return SeatStep::Sent;
}

static SeatStep GameDefaultSeat(uintptr_t entity, uint64_t shipId) {
    __try {
        const uintptr_t ship = EntityById(shipId);
        if (!ship) return SeatStep::NotReady;
        const uintptr_t seats = EntityComponent(ship, "ISCItemControllableManager");
        const uintptr_t user = EntityComponent(entity, "ISCItemUser");
        if (!seats || !user) return SeatStep::NotReady;
        return g_sp.findSeat(seats, user, nullptr) ? SeatStep::Sent : SeatStep::NoSeat;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SeatStep::Fault;
    }
}

static SeatStep SendSeatRequest() {
    uintptr_t actor = 0, entity = 0;
    bool live = false;
    __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) { return SeatStep::Fault; }
    if (!live) return SeatStep::NotReady;
    if (SeatControl()) {
        const SeatStep step = SeatPlayer(entity);
        if (step != SeatStep::NoSeat || g_seatJob.wantSeat) return step;
    }
    g_haveLastSeat = false;
    g_seatJob.sentSeat = 0;
    return GameDefaultSeat(entity, g_seatJob.id);
}

static bool SeatJobDone() {
    if (g_seatJob.sentSeat && g_occupantLayout) {
        if (EnumerateShipSeats(g_seatJob.id) <= 0) return false;
        const SeatInfo* s = SeatById(g_seatJob.sentSeat);
        return s && s->state == SeatState_You;
    }
    if (g_seatJob.startedAboard) return true;   // can't verify the exact seat; assume the link took
    return PlayerAboard(g_seatJob.id, g_seatJob.name);
}

static void PressFlightReadyKey() {
    if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wScan = in[1].ki.wScan = 0x13;
            in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
            in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &in[0], sizeof(INPUT));
            Sleep(120);
            SendInput(1, &in[1], sizeof(INPUT));
            return 0;
        }, nullptr, 0, nullptr))
        CloseHandle(t);
}

// --- everything mounted on a ship ------------------------------------------------------------
//
// A spawned ship's parts get consecutive entity ids right after the ship's own (in the logs, a
// Perseus at ...069 has seats at ...071 to ...200). Walking that range and keeping the entities
// whose chain of parents leads back to the ship lists every part: weapons, turrets, dashboards,
// power plants, coolers, shields and so on.

struct ShipItem { uint64_t id; uint64_t parent; int depth; uintptr_t entity; char name[96]; };
constexpr int kMaxShipItems = 1500;
static ShipItem g_shipItems[kMaxShipItems];
static int      g_shipItemCount = 0;
static uint64_t g_shipItemsOf = 0;
static DWORD    g_shipItemsAt = 0;

static uint64_t ItemParentId(uintptr_t entity) {
    __try {
        uint64_t port = 0;
        VCall<void>(entity, actors::kEntityParentPort, &port, 0ull);
        if (!(port & kPtrMask)) return 0;
        uint64_t id = 0;
        const uint64_t* owner = VCall<const uint64_t*>(port & kPtrMask, actors::kPortOwnerId, &id);
        return owner ? *owner : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void EntityNameOf(uintptr_t entity, char* out, size_t n) {
    strncpy_s(out, n, "?", _TRUNCATE);
    __try {
        if (const char* name = VCall<const char*>(entity, actors::kEntityName)) strncpy_s(out, n, name, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static int CollectShipItems(uint64_t shipId, DWORD now) {
    if (g_shipItemsOf == shipId && now - g_shipItemsAt < 3000) return g_shipItemCount;
    g_shipItemCount = 0;
    g_shipItemsOf = shipId;
    g_shipItemsAt = now;
    int misses = 0;
    for (uint64_t id = shipId + 1; id < shipId + 8000 && misses < 400 && g_shipItemCount < kMaxShipItems; ++id) {
        const uintptr_t e = EntityByIdSafe(id);
        if (!e) { ++misses; continue; }
        misses = 0;
        const uint64_t parent = ItemParentId(e);
        uint64_t up = parent;
        int depth = 1;
        while (up && up != shipId && depth < 12) {
            const uintptr_t next = EntityByIdSafe(up);
            if (!next) { up = 0; break; }
            up = ItemParentId(next);
            ++depth;
        }
        if (up != shipId) continue;     // not part of this ship (crew, other ships, loose items)
        ShipItem& it = g_shipItems[g_shipItemCount++];
        it.id = id;
        it.parent = parent;
        it.depth = depth;
        it.entity = e;
        EntityNameOf(e, it.name, sizeof(it.name));
    }
    return g_shipItemCount;
}

int ShipPartComponents(uint64_t shipId, const char* type, uintptr_t* components, char (*names)[96], int max) {
    if (!shipId) return 0;
    const int n = CollectShipItems(shipId, GetTickCount());   // cached for 3 s
    int found = 0;
    for (int i = 0; i < n && found < max; ++i) {
        uintptr_t c = 0;
        __try { c = EntityComponent(g_shipItems[i].entity, type); } __except (EXCEPTION_EXECUTE_HANDLER) { c = 0; }
        if (!c) continue;
        components[found] = c;
        strcpy_s(names[found], 96, g_shipItems[i].name);
        ++found;
    }
    return found;
}

// --- power ----------------------------------------------------------------------------------
//
// Powering on uses the game's own Flight Ready dashboard event (the same thing the R key ends up
// sending), fired straight at the pilot seat's dashboard. Big ships stream their interior in over
// several seconds, so the dashboard is polled for up to 20 s before giving up and pressing R.

static struct { uint64_t shipId, seatId; DWORD at, deadline; bool waitingLogged; char name[64]; } g_powerJob;

static uintptr_t DashboardOn(uintptr_t entity) {
    __try { return entity ? EntityComponent(entity, "SCItemSeatDashboard") : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The seat itself (Perseus-style "SeatDashboard"), then the ship, then any part of the ship that has
// a dashboard - preferring one mounted on the pilot seat (F8C, Moth and most smaller ships).
static uintptr_t FindDashboard(uint64_t shipId, uint64_t seatId) {
    if (const uintptr_t d = DashboardOn(EntityByIdSafe(seatId))) return d;
    if (const uintptr_t d = DashboardOn(EntityByIdSafe(shipId))) return d;
    const int n = CollectShipItems(shipId, GetTickCount());
    uintptr_t any = 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t d = DashboardOn(g_shipItems[i].entity);
        if (!d) continue;
        if (g_shipItems[i].parent == seatId) {
            Log("[ship] using dashboard '%s' on the pilot seat", g_shipItems[i].name);
            return d;
        }
        if (!any) any = d;
    }
    if (any) Log("[ship] using the first dashboard found on the ship");
    return any;
}

static bool SendDashEvent(ToggleFlightReadyFn send, uintptr_t dashboard) {
    if (!send || !dashboard) return false;
    __try {
        const struct { void* invoke; uintptr_t manager; void* storage; } noCallback = {};
        send(*g_tp.entitySystem, dashboard, &noCallback);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void StartPowerJob(uint64_t shipId, uint64_t seatId, const char* name, DWORD at, DWORD wait) {
    g_powerJob = {};
    g_powerJob.shipId = shipId;
    g_powerJob.seatId = seatId;
    g_powerJob.at = at;
    g_powerJob.deadline = at + wait;
    strncpy_s(g_powerJob.name, name, _TRUNCATE);
}

static void RunPowerJob(DWORD now) {
    if (!g_powerJob.shipId || static_cast<LONG>(now - g_powerJob.at) < 0) return;
    const uintptr_t dashboard = g_sp.toggleFlightReady ? FindDashboard(g_powerJob.shipId, g_powerJob.seatId) : 0;
    if (dashboard && SendDashEvent(g_sp.toggleFlightReady, dashboard)) {
        SetMenuStatus("%s powered on - sent the game's Flight Ready event to the pilot dashboard.", g_powerJob.name);
        g_powerJob.shipId = 0;
        return;
    }
    if (g_sp.toggleFlightReady && static_cast<LONG>(now - g_powerJob.deadline) < 0) {
        if (!g_powerJob.waitingLogged) { g_powerJob.waitingLogged = true; Log("[ship] waiting for the %s's pilot dashboard to load...", g_powerJob.name); }
        g_powerJob.at = now + 500;
        return;
    }
    if (g_sp.toggleFlightReady) Log("[ship] no seat dashboard on the %s; falling back to the R key", g_powerJob.name);
    if (GameHasFocus()) {
        PressFlightReadyKey();
        SetMenuStatus("Couldn't reach the %s's dashboard directly - pressed Flight Ready (R) for you.", g_powerJob.name);
    } else {
        SetMenuStatus("Couldn't reach the %s's dashboard. Press R (Flight Ready) to power up.", g_powerJob.name);
    }
    g_powerJob.shipId = 0;
}

static void FinishSeatJob(DWORD now) {
    const bool have = g_haveLastSeat;
    const bool pilot = have && g_lastSeat.priority >= kPilotPriority;
    if (pilot && g_seatJob.flightReady) {
        StartPowerJob(g_seatJob.id, g_lastSeat.seatId, g_seatJob.name, now + 1500, 20000);
        SetMenuStatus("You're in the %s's pilot seat - powering up...", g_seatJob.name);
    } else if (pilot) {
        SetMenuStatus("You're in the %s's pilot seat ('%s'). Press R (Flight Ready) to power up.", g_seatJob.name, g_lastSeat.name);
    } else if (have && g_seatJob.mode == SeatMode_Pilot && !g_seatJob.wantSeat) {
        SetMenuStatus("You're in the %s, but its pilot seat wasn't free%s - you got '%s'.", g_seatJob.name,
                      g_seatJob.replaceNpc ? "" : " (tick 'replace NPC' to take it)", g_lastSeat.name);
    } else if (have) {
        SetMenuStatus("You're in '%s' on the %s.", g_lastSeat.name, g_seatJob.name);
    } else {
        SetMenuStatus("You're in the %s. Have fun!", g_seatJob.name);
    }
    g_seatJob.id = 0;
    RefreshTargetSeats();
}

static void UpdateSeatJob(DWORD now) {
    static DWORD lastCheck = 0;
    if (!g_seatJob.id || now - lastCheck < 500) return;
    lastCheck = now;
    if (g_seatJob.sends && now - g_seatJob.lastSend >= 1500) {
        if (SeatJobDone()) { FinishSeatJob(now); return; }
    }
    if (now - g_seatJob.since > 180000) {
        SetMenuStatus("%s spawned, but I couldn't get you aboard within 3 minutes.", g_seatJob.name);
        g_seatJob.id = 0;
        return;
    }
    if (g_seatJob.lastSend && now - g_seatJob.lastSend < (g_seatJob.lastWasEvict ? 1200u : 4000u)) return;
    switch (SendSeatRequest()) {
    case SeatStep::NotReady: break;
    case SeatStep::NoSeat:   g_seatJob.lastSend = now; g_seatJob.lastWasEvict = false; break;
    case SeatStep::Blocked:
        if (!g_haveLastSeat)
            SetMenuStatus("That seat isn't on the %s any more - refresh the list.", g_seatJob.name);
        else if (g_lastSeat.state == SeatState_Npc && g_seatJob.evictions > 3)
            SetMenuStatus("The NPC in '%s' keeps coming back - try another seat.", g_lastSeat.name);
        else if (g_lastSeat.state == SeatState_Npc)
            SetMenuStatus("'%s' has an NPC in it - tick 'replace NPC' or kick it first.", g_lastSeat.name);
        else
            SetMenuStatus("'%s' is taken by someone I can't identify, so I left it.", g_lastSeat.name);
        g_seatJob.id = 0;
        RefreshTargetSeats();
        break;
    case SeatStep::Evicting:
        g_seatJob.lastSend = now;
        g_seatJob.lastWasEvict = true;
        SetMenuStatus("Removing the NPC from '%s' on the %s...", g_lastSeat.name, g_seatJob.name);
        break;
    case SeatStep::Fault:    SetMenuStatus("Seating failed (fault)."); g_seatJob.id = 0; break;
    case SeatStep::Sent:
        g_seatJob.lastSend = now;
        g_seatJob.lastWasEvict = false;
        if (++g_seatJob.sends > 8) { SetMenuStatus("Asked %s to seat you 8 times; it didn't take.", g_seatJob.name); g_seatJob.id = 0; }
        else Log("[ship] seat request %d sent to %s", g_seatJob.sends, g_seatJob.name);
        break;
    }
}

// --- menu actions on the target ship --------------------------------------------------------

static void ProcessSeatAction(DWORD now) {
    AcquireSRWLockExclusive(&g_menuLock);
    const auto act = g_seatAction;
    g_seatAction.kind = SA_None;
    ReleaseSRWLockExclusive(&g_menuLock);
    if (act.kind == SA_None) return;

    if (act.kind == SA_TargetMine) {
        if (const char* err = TargetShipImIn()) SetMenuStatus("Can't pick your ship: %s", err);
        else SetMenuStatus("Crew & seats now shows the %s.", g_target.name);
        return;
    }
    if (!SeatControl()) { SetMenuStatus("Seat control isn't available in this game version."); return; }
    if (!g_target.shipId) { SetMenuStatus("Spawn a ship first (or press 'Use the ship I'm in')."); return; }
    const int n = EnumerateShipSeats(g_target.shipId);
    if (n <= 0) { SetMenuStatus("The %s isn't loaded (or has no seats).", g_target.name); PublishSeats(n); return; }
    const SeatInfo* seat = act.seatId ? SeatById(act.seatId) : nullptr;

    switch (act.kind) {
    case SA_Sit:
        if (!seat) { SetMenuStatus("That seat is gone - refresh the list."); break; }
        StartSeatJob(g_target.shipId, g_target.name, SeatMode_Pilot, "", act.replace, false, seat->seatId, now);
        SetMenuStatus("Moving you to '%s'...", seat->name);
        break;
    case SA_Kick:
        if (!seat) { SetMenuStatus("That seat is gone - refresh the list."); break; }
        if (seat->state == SeatState_Npc) { const SeatInfo copy = *seat; EvictSeat(copy); SetMenuStatus("Removed the NPC from '%s'.", copy.name); }
        else if (seat->state == SeatState_Taken) SetMenuStatus("Can't tell who is in '%s', so I left them.", seat->name);
        else SetMenuStatus("There's no NPC in '%s'.", seat->name);
        break;
    case SA_StandUp:
        if (!seat) { SetMenuStatus("That seat is gone - refresh the list."); break; }
        if (seat->state == SeatState_You)
            SetMenuStatus(UnlinkEntity(LocalPlayerEntity()) ? "You got out of '%s'." : "Couldn't get you out of '%s'.", seat->name);
        else if (seat->state == SeatState_Npc) {
            const SeatInfo copy = *seat;
            const bool ok = UnlinkEntity(EntityByIdSafe(copy.occupant));
            if (ok) ForgetPlacedCrew(copy.occupant);
            SetMenuStatus(ok ? "The NPC in '%s' stood up." : "Couldn't get the NPC out of '%s'.", copy.name);
        }
        else if (seat->state == SeatState_Taken) SetMenuStatus("Can't tell who is in '%s', so I left them.", seat->name);
        else SetMenuStatus("'%s' is already empty.", seat->name);
        break;
    case SA_StandAll: {
        uint64_t npcs[kMaxSeats];
        int count = 0, stood = 0;
        for (int i = 0; i < g_seatListCount; ++i)
            if (g_seatList[i].state == SeatState_Npc) npcs[count++] = g_seatList[i].occupant;
        for (int i = 0; i < count; ++i)
            if (UnlinkEntity(EntityByIdSafe(npcs[i]))) { ForgetPlacedCrew(npcs[i]); ++stood; }
        SetMenuStatus("%d of %d NPCs on the %s stood up.", stood, count, g_target.name);
        break;
    }
    case SA_AddCrew: {
        if (!seat) { SetMenuStatus("That seat is gone - refresh the list."); break; }
        if (seat->state != SeatState_Empty) { SetMenuStatus("'%s' isn't empty - kick its occupant first.", seat->name); break; }
        const SeatInfo copy = *seat;
        if (const char* err = AddCrew(g_target.shipId, copy.seatId, act.npc, now)) SetMenuStatus("Adding crew failed: %s", err);
        else SetMenuStatus("Seating %s in '%s'...", Menu_NpcName(act.npc), copy.name);
        break;
    }
    case SA_FillCrew: {
        uint64_t empty[kMaxSeats];
        int count = 0, added = 0;
        for (int i = 0; i < g_seatListCount; ++i)
            if (g_seatList[i].state == SeatState_Empty && !CrewJobForSeat(g_seatList[i].seatId)) empty[count++] = g_seatList[i].seatId;
        const char* err = nullptr;
        for (int i = 0; i < count && !err; ++i)
            if (!(err = AddCrew(g_target.shipId, empty[i], act.npc, now))) ++added;
        if (err && !added) SetMenuStatus("Filling seats failed: %s", err);
        else SetMenuStatus("Seating %d x %s on the %s...", added, Menu_NpcName(act.npc), g_target.name);
        break;
    }
    case SA_ClearCrew: {
        SeatInfo npcs[kMaxSeats];
        int count = 0;
        for (int i = 0; i < g_seatListCount; ++i)
            if (g_seatList[i].state == SeatState_Npc) npcs[count++] = g_seatList[i];
        for (int i = 0; i < count; ++i) EvictSeat(npcs[i]);
        SetMenuStatus("Removed %d NPC crew from the %s.", count, g_target.name);
        break;
    }
    case SA_FlightReady: {
        const SeatInfo* pilot = PilotSeat();
        if (!pilot || !g_sp.toggleFlightReady) { SetMenuStatus("Flight Ready event not available - press R in the pilot seat."); break; }
        StartPowerJob(g_target.shipId, pilot->seatId, g_target.name, now, 3000);
        break;
    }
    default: break;
    }
    RefreshTargetSeats();
}

static void StartDaymarArrival(const char* shipClass, DWORD now) {
    uint64_t id = 0;
    if (const char* err = SpawnShipAboveDaymar(shipClass, id)) {
        SetMenuStatus("Going to Daymar failed: %s", err);
        return;
    }
    SetTarget(id, shipClass);
    StartSeatJob(id, shipClass, SeatMode_Pilot, "", true, true, 0, now);
    SetMenuStatus("Spawning %s %.0f km over Daymar - you'll be put in its pilot seat, then fly down and land.",
                  shipClass, kArrivalAltitude / 1000);
}

static void ProcessNoclip() {
    AcquireSRWLockExclusive(&g_menuLock);
    const auto req = g_noclipRequest;
    g_noclipRequest.modePending = g_noclipRequest.speedPending = false;
    ReleaseSRWLockExclusive(&g_menuLock);
    if (req.speedPending && g_sp.gameCVars && g_sp.flySpeedOffset) {
        __try {
            if (const uintptr_t cvars = *g_sp.gameCVars) *reinterpret_cast<float*>(cvars + g_sp.flySpeedOffset) = req.speed;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!req.modePending) return;
    if (!g_sp.requestFlyMode || !g_sp.actorLink) { SetMenuStatus("Noclip isn't available (fly mode not found)."); return; }
    const char* err = nullptr;
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) err = "you're not spawned yet";
        else if (const uintptr_t comp = EntityComponent(entity, "Actor")) g_sp.requestFlyMode(g_sp.actorLink(comp), req.on ? 2 : 0);
        else err = "no Actor component";
    } __except (EXCEPTION_EXECUTE_HANDLER) { err = "fault"; }
    if (err) SetMenuStatus("Noclip %s failed: %s", req.on ? "on" : "off", err);
    else SetMenuStatus("Noclip %s (speed %.0f).", req.on ? "on" : "off", req.speed);
}

static void ProcessGodMode(DWORD now) {
    static DWORD lastCheck = 0;
    static bool  wasOn = false;
    if (!g_sp.setGodMode || now - lastCheck < 500) return;
    lastCheck = now;
    const bool on = g_godModeOn != 0;
    if (!on && !wasOn) return;
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return;
        const uintptr_t comp = EntityComponent(entity, "Actor");
        const uintptr_t data = comp ? Rd<uintptr_t>(comp + actors::kGodModeData) : 0;
        if (!data) return;
        const uintptr_t state = data + actors::kGodModeState;
        const uint8_t want = on ? 2 : 0;
        if (Rd<uint8_t>(state + g_sp.godModeByte) != want) {
            g_sp.setGodMode(state, want);
            Log("[god] god mode %s", on ? "on (no damage)" : "off");
        }
        wasOn = on;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ProcessShipMenu(DWORD now) {
    if (!g_sp.ok) return;
    ProcessNoclip();
    ProcessGodMode(now);
    RefreshEnemySide();
    if (g_menuShipCount < 0 && g_menuWantShips) {
        uintptr_t actor, entity;
        bool live = false;
        __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (live) {
            int n = 0;
            __try { n = BuildMenuShips(); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
            InterlockedExchange(&g_menuShipCount, n);
            Log("[ship] %d ships in the spawn menu", n);
        }
    }

    AcquireSRWLockExclusive(&g_menuLock);
    const auto req = g_spawnRequest;
    g_spawnRequest.pending = false;
    const auto classReq = g_classRequest;
    g_classRequest.pending = false;
    ReleaseSRWLockExclusive(&g_menuLock);
    if (classReq.pending && classReq.cls[0]) {
        uint64_t id = 0;
        if (const char* err = SpawnShipAbovePlayer(classReq.cls, classReq.height, id))
            SetMenuStatus("Spawning %s failed: %s", classReq.cls, err);
        else if (classReq.enemyWing) {
            SetTarget(id, classReq.cls);   // the Bengal itself, like the plain Bengal row; the wing ships are not targeted
            // The wing goes 300 m up — the height the menu's own hint promises for
            // the Vanduul hulls — rather than on top of the 980 m Bengal.
            int wing = 0;
            for (const char* c : kEnemySideClasses) {
                if (!ClassInRegistry(c)) continue;
                uint64_t wingId = 0;
                if (const char* err = SpawnShipAbovePlayer(c, 300.0, wingId)) { Log("[ship] enemy wing: %s failed: %s", c, err); continue; }
                ++wing;
            }
            SetMenuStatus("%s spawned %.0f m above you; %d of 3 enemy wing ships came in at 300 m.",
                          classReq.cls, classReq.height, wing);
        } else if (!classReq.sit) {
            SetTarget(id, classReq.cls);
            SetMenuStatus("Spawning %s %.0f m above you (big ships take up to a minute).", classReq.cls, classReq.height);
        } else {
            SetTarget(id, classReq.cls);
            StartSeatJob(id, classReq.cls, SeatMode_Pilot, nullptr, true, classReq.flightReady, 0, now);
            SetMenuStatus("Spawning %s - you'll be put in the pilot seat as soon as it's there (big ships take up to a minute).", classReq.cls);
        }
    } else if (req.pending && req.index >= 0 && req.index < g_menuShipCount) {
        const char* name = g_menuShips[req.index].name;
        const MenuSpawnOptions& o = req.opt;
        uint64_t id = 0;
        if (const char* err = SpawnShipAbovePlayer(name, o.height, id)) {
            SetMenuStatus("Spawning %s failed: %s", name, err);
        } else {
            SetTarget(id, name);
            switch (o.seatMode) {
            case SeatMode_None:
                SetMenuStatus("Spawning %s %.0f m above you (big ships take up to a minute).", name, o.height);
                break;
            case SeatMode_PickLater:
                SetMenuStatus("Spawning %s - pick a seat under Crew & seats once it has loaded.", name);
                break;
            default: {
                const bool named = o.seatMode == SeatMode_Named && o.seatName[0];
                StartSeatJob(id, name, named ? SeatMode_Named : SeatMode_Pilot, o.seatName, o.replaceNpc, o.flightReady, 0, now);
                if (named) SetMenuStatus("Spawning %s - you'll be put in a '%s' seat as soon as it's there.", name, o.seatName);
                else SetMenuStatus("Spawning %s - you'll be put in the pilot seat as soon as it's there (big ships take up to a minute).", name);
                break;
            }
            }
        }
    }

    static DWORD lastTestFile = 0;
    if (now - lastTestFile >= 2000) {
        lastTestFile = now;
        char path[MAX_PATH];
        char line[128] = "";
        if (ModLogSibling(path, MAX_PATH, "spawn_test.txt")) {
            FILE* f = nullptr;
            if (fopen_s(&f, path, "r") == 0 && f) {
                if (!fgets(line, sizeof(line), f)) line[0] = 0;
                fclose(f);
                DeleteFileA(path);
            }
        }
        if (char* at = strchr(line, '@')) {
            *at = 0;
            const double height = atof(at + 1);
            uint64_t id = 0;
            if (const char* err = SpawnShipAbovePlayer(line, height, id)) Log("[ship] test spawn of %s failed: %s", line, err);
            else {
                SetTarget(id, line);
                StartSeatJob(id, line, SeatMode_Pilot, "", true, true, 0, now);
                Log("[ship] test spawn: %s %.0f m above you", line, height);
            }
        }
    }

    if (g_startDaymarPending) {
        static DWORD liveSince = 0;
        uintptr_t actor, entity;
        bool live = false;
        __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!live) liveSince = 0;
        else if (!liveSince) liveSince = now;
        else if (now - liveSince > 10000) {
            g_startDaymarPending = false;
            StartDaymarArrival(g_startShip, now);
        }
    }

    UpdateSeatJob(now);
    RunPowerJob(now);
}

// The Crew & seats panel's work: its queued seat actions (and the Vehicles tab's Power on), the
// crew jobs, and the seat list. Run by the crew built-in's tick, after the spawner's.
void ProcessCrew(DWORD now) {
    if (!g_sp.ok) return;
    ProcessSeatAction(now);
    UpdateCrewJobs(now);

    // Keep the Crew & seats list live while the menu is showing it.
    static DWORD lastSeatRefresh = 0;
    if (g_target.shipId && now - static_cast<DWORD>(g_seatPanelSeen) < 3000 && now - lastSeatRefresh >= 1000) {
        lastSeatRefresh = now;
        RefreshTargetSeats();
    }
}
