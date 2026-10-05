#include "spawner.h"
#include "patches.h"
#include "teleport.h"
#include "menu.h"
#include <algorithm>
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
};
static SpawnApi g_sp;

bool ResolveSpawnApi(const Section& text, const Section& rdata) {
    const uint8_t* msg = FindCString(rdata, "Landing Area could not be found.");
    const uint8_t* lea = msg ? FindRipLea(text, 0x4C, 0x8D, 0x0D, msg) : nullptr;
    if (!lea) { Log("[ship] spawn helpers not found; ship spawner disabled"); return false; }
    const uint8_t* f = lea - 0x59;
    if (!BytesMatch(f, "48 89 5C 24 10 4C 89 4C 24 20 56 57 41 54 41 56 41 57")
        || !BytesMatch(f + 0x4F, "B1 40 E8") || !BytesMatch(f + 0x40C, "BA 00 10 00 00")
        || !BytesMatch(f + 0x46D, "E8") || !BytesMatch(f + 0x47D, "E8") || !BytesMatch(f + 0x55C, "E8")) {
        Log("[ship] spawn helper layout changed; ship spawner disabled");
        return false;
    }
    int n = 0;
    uint8_t* ctor = FindUniquePattern(text,
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 F6 48 B8 00 00 00 00 00 00 F0 3F 48 89 71 08 48 8B F9 "
        "48 89 71 10 48 89 71 18 48 89 71 30 48 89 71 38 48 89 71 40 48 89 31", n);
    uint8_t* seat = FindUniquePattern(text,
        "40 55 41 54 41 56 48 8D AC 24 30 FC FF FF 48 81 EC D0 04 00 00 4D 8B E0 4C 8B F2 48 85 C9 0F 84", n);
    const uintptr_t genv = reinterpret_cast<uintptr_t>(g_isOnlineFlag) - 0x60E;
    if (!ctor || !seat || !g_isOnlineFlag || reinterpret_cast<uintptr_t*>(genv + 0xA8) != g_tp.entitySystem) {
        Log("[ship] spawn params / seat helper not found; ship spawner disabled");
        return false;
    }
    g_sp.teamCategory = reinterpret_cast<TeamCategoryFn>(f + 0x51 + 5 + Rel32(f + 0x52));
    g_sp.setFlags     = reinterpret_cast<SpawnSetFlagsFn>(f + 0x46D + 5 + Rel32(f + 0x46E));
    g_sp.setClass     = reinterpret_cast<SpawnSetClassFn>(f + 0x47D + 5 + Rel32(f + 0x47E));
    g_sp.setLocation  = reinterpret_cast<SpawnSetLocFn>(f + 0x55C + 5 + Rel32(f + 0x55D));
    g_sp.ctor         = reinterpret_cast<SpawnParamsCtorFn>(ctor);
    g_sp.findSeat     = reinterpret_cast<FindSeatFn>(seat);
    g_sp.game         = reinterpret_cast<uintptr_t*>(genv + 0xA0);
    g_sp.components   = reinterpret_cast<uintptr_t*>(genv + 0xB0);
    g_sp.ok = true;

    const uint8_t* s = seat;
    const uint8_t* cb = BytesMatch(s + 0x21C, "48 8D 05") ? s + 0x223 + Rel32(s + 0x21F) : nullptr;
    if (cb && BytesMatch(s + 0xB2, "E8") && BytesMatch(s + 0x19C, "E8") && BytesMatch(s + 0x1D6, "FF 90 78 07 00 00")
        && BytesMatch(s + 0x242, "E8") && BytesMatch(s + 0x3CE, "E8") && BytesMatch(s + 0x3F1, "E8")
        && BytesMatch(s + 0x3FE, "E8") && BytesMatch(s + 0x40B, "E8")
        && BytesMatch(cb + 0x37, "48 83 BF 58 01 00 00 00") && BytesMatch(cb + 0xD6, "E8")) {
        auto target = [](const uint8_t* call) { return const_cast<uint8_t*>(call + 5 + Rel32(call + 1)); };
        g_sp.isLinked     = reinterpret_cast<IsLinkedFn>(target(s + 0xB2));
        g_sp.forceDelink  = reinterpret_cast<ForceDelinkFn>(target(s + 0x19C));
        g_sp.forEachSeat  = reinterpret_cast<ForEachSeatFn>(target(s + 0x242));
        g_sp.actorOfUser  = reinterpret_cast<ActorOfUserFn>(target(s + 0x3CE));
        g_sp.handleToId   = reinterpret_cast<HandleToIdFn>(target(s + 0x3F1));
        g_sp.actorLink    = reinterpret_cast<ActorLinkFn>(target(s + 0x3FE));
        g_sp.forceLink    = reinterpret_cast<ForceLinkFn>(target(s + 0x40B));
        g_sp.seatPriority = reinterpret_cast<SeatPriorityFn>(target(cb + 0xD6));
    } else {
        Log("[ship] seat picker layout changed; using the game's default seat choice");
    }

    const uint8_t* fmsg = FindCString(rdata, "FindEntityByName_SlowDebugCodeOnly: %s");
    for (uint8_t* p = g_text.base; fmsg && p + 7 < g_text.base + g_text.size; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(g_text.base + g_text.size - 7 - p)));
        if (!p) break;
        if (p[1] == 0x8D && p[2] == 0x0D && p + 7 + Rel32(p + 3) == fmsg
            && BytesMatch(p - 0x36, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 C7 02 00 00 00 00")) {
            g_sp.findEntityByName = reinterpret_cast<FindByNameFn>(p - 0x36);
            break;
        }
    }
    if (!g_sp.findEntityByName) Log("[ship] entity lookup by name not found; Daymar disabled");

    uint8_t* senders[4] = {};
    const int nSenders = FindPattern(text,
        "48 89 5C 24 08 57 48 83 EC 50 8B 05 ?? ?? ?? ?? 48 8B FA 4C 89 44 24 20 48 8B D9 85 C0 75 19 41 B8 49 00 00 00 48 8D 15",
        senders, 4);
    const uint8_t* header = FindCString(rdata, "C:\\workspace\\CryEngine\\Code\\CryEngine\\CryCommon\\Events/ISC/Dashboards.h");
    for (int i = 0; header && i < nSenders && i < 4; ++i)
        if (senders[i] + 0x2C + Rel32(senders[i] + 0x28) == header)
            g_sp.toggleFlightReady = reinterpret_cast<ToggleFlightReadyFn>(senders[i]);
    if (!g_sp.toggleFlightReady) Log("[ship] Flight Ready event not found; will press R instead");

    const uint8_t* flyLabel = FindCString(rdata,
        "unsigned short __cdecl CSCActorActionHandler::Request<struct SCActorActionHandlerActions::SFlyMode,"
        "const enum ESCActorFlyMode&>(const enum ESCActorFlyMode &)");
    uint8_t* wrappers[16] = {};
    const int nWrappers = FindPattern(text, "89 54 24 10 48 83 EC 28 48 8D 54 24 38 E8", wrappers, 16);
    for (int i = 0; flyLabel && i < nWrappers && i < 16; ++i) {
        const uint8_t* req = wrappers[i] + 0x12 + Rel32(wrappers[i] + 0xE);
        if (req >= text.base && req + 0xB5 <= text.base + text.size && BytesMatch(req + 0xAE, "48 8D 05")
            && req + 0xB5 + Rel32(req + 0xB1) == flyLabel)
            g_sp.requestFlyMode = reinterpret_cast<RequestFlyModeFn>(wrappers[i]);
    }
    if (uint8_t* p = FindUniquePattern(text, "48 89 83 38 01 00 00 B9 98 00 00 00 48 89 05", n))
        g_sp.gameCVars = reinterpret_cast<uintptr_t*>(p + 0x13 + Rel32(p + 0xF));
    const uint8_t* speedName = FindCString(rdata, "g_FlyModeSpeedScaler");
    for (uint8_t* p = g_text.base + 0x14; speedName && p + 7 < g_text.base + g_text.size; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(g_text.base + g_text.size - 7 - p)));
        if (!p) break;
        if (p[1] == 0x8D && p[2] == 0x15 && p + 7 + Rel32(p + 3) == speedName && BytesMatch(p - 0x14, "4C 8D 87")) {
            const int32_t off = Rel32(p - 0x11);
            if (off > 0 && off < 0x4000) g_sp.flySpeedOffset = static_cast<uint32_t>(off);
            break;
        }
    }
    if (!g_sp.requestFlyMode) Log("[noclip] fly mode request not found; noclip disabled");
    if (!g_sp.gameCVars || !g_sp.flySpeedOffset) Log("[noclip] fly speed setting not found; speed stays at the game's default");

    const uint8_t* godMsg = FindCString(rdata, "Actor [$$] has changed GodMode State from [$$] to [$$]");
    if (uint8_t* p = FindUniquePattern(text, "49 8B 8E 08 02 00 00 41 0F B6 D4 48 81 C1 F0 27 00 00 E8", n)) {
        const uint8_t* set = p + 0x17 + Rel32(p + 0x13);
        if (godMsg && BytesMatch(p - 0x64, "4C 8D 0D") && p - 0x64 + 7 + Rel32(p - 0x61) == godMsg
            && set >= text.base && set + 0x10 <= text.base + text.size
            && BytesMatch(set, "48 89 5C 24 08 57 48 83 EC 20 88 91")) {
            g_sp.setGodMode = reinterpret_cast<SetGodModeFn>(const_cast<uint8_t*>(set));
            g_sp.godModeByte = static_cast<uint32_t>(Rel32(set + 0xC));
        }
    }
    if (!g_sp.setGodMode) Log("[god] god mode setter not found; god mode disabled");

    const uint8_t* fmt = FindCString(rdata, "ObjectContainers\\%s");
    const uint8_t* ocName = FindCString(rdata, "ocFilename");
    if (const uint8_t* s = fmt ? FindRipLea(text, 0x48, 0x8D, 0x15, fmt) : nullptr) {
        if (ocName && BytesMatch(s + 0x37, "48 8B 0D") && BytesMatch(s + 0x41, "FF 90") && BytesMatch(s + 0x58, "4C 8B 81")
            && BytesMatch(s + 0x6E, "48 8B 78") && BytesMatch(s + 0x72, "48 8D 05") && s + 0x79 + Rel32(s + 0x75) == ocName
            && BytesMatch(s + 0x94, "48 8D 05") && BytesMatch(s + 0xA0, "E8")) {
            g_sp.system      = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(s + 0x3E + Rel32(s + 0x3A)));
            g_sp.pathMgrSlot = Rel32(s + 0x43);
            g_sp.pathIdSlot  = Rel32(s + 0x5B);
            g_sp.attrSetSlot = s[0x71];
            g_sp.attrWriter  = const_cast<uint8_t*>(s + 0x9B + Rel32(s + 0x97));
            g_sp.attrTypeId  = reinterpret_cast<uint32_t(__fastcall*)()>(const_cast<uint8_t*>(s + 0xA5 + Rel32(s + 0xA1)));
        }
    }
    if (!g_sp.attrTypeId) Log("[build] prefab spawning not found; outposts/prefabs disabled");
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
static char          g_menuStatus[256] = "Pick a ship and press Spawn.";
static struct { bool pending; int index; float height; bool sit; bool flightReady; bool daymar; } g_spawnRequest;

static bool g_startDaymarPending = false;
static char g_startShip[64] = "DRAK_Cutlass_Black";

void ReadStartOptions() {
    char v[64];
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_START", v, sizeof(v));
    g_startDaymarPending = n > 0 && n < sizeof(v) && _stricmp(v, "Daymar") == 0;
    const DWORD s = GetEnvironmentVariableA("SC_OFFLINE_START_SHIP", v, sizeof(v));
    if (s > 0 && s < sizeof(v)) strcpy_s(g_startShip, v);
    if (g_startDaymarPending && g_sp.ok && g_sp.findEntityByName)
        Log("[ship] start: over Daymar in %s (SC_OFFLINE_START=Daymar)", g_startShip);
    else if (g_startDaymarPending)
        Log("[!] start over Daymar requested but the spawner isn't available");
}

bool SpawnerReady() { return g_sp.ok; }
bool StartingOverDaymar() { return g_startDaymarPending; }

static void SetMenuStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_menuLock);
    strcpy_s(g_menuStatus, buf);
    ReleaseSRWLockExclusive(&g_menuLock);
    Log("[ship] %s", buf);
}

int Menu_ShipCount() {
    const LONG n = g_menuShipCount;
    if (n < 0) InterlockedExchange(&g_menuWantShips, 1);
    return n;
}

const MenuShip* Menu_Ships() { return g_menuShips; }

void Menu_RequestSpawn(int index, float heightAboveMe, bool sitInPilotSeat, bool flightReady) {
    AcquireSRWLockExclusive(&g_menuLock);
    g_spawnRequest = { true, index, heightAboveMe, sitInPilotSeat, flightReady, false };
    ReleaseSRWLockExclusive(&g_menuLock);
}

void Menu_RequestDaymar(int index) {
    AcquireSRWLockExclusive(&g_menuLock);
    g_spawnRequest = { true, index, 0.0f, true, true, true };
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
    AcquireSRWLockShared(&g_menuLock);
    strncpy_s(out, n, g_menuStatus, _TRUNCATE);
    ReleaseSRWLockShared(&g_menuLock);
}

static uintptr_t ClassRegistry() { return VCall<uintptr_t>(*g_tp.entitySystem, 0xC0); }

static int VehicleSize(uintptr_t entityClass) {
    const uintptr_t rec = VCall<uintptr_t>(*g_sp.game, 0x298, entityClass);
    return rec ? static_cast<int>(Rd<uint32_t>(rec + 0x10)) : 0;
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
        const uintptr_t cls = VCall<uintptr_t>(registry, 0x20, static_cast<const char*>(name));
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
    const uint16_t* id = VCall<const uint16_t*>(*g_sp.components, 0x10, tmp, type);
    if (!id) return 0;
    uint16_t typeId = *id;
    uint8_t out[16] = {};
    const uint64_t* h = VCall<const uint64_t*>(entity, 0x390, out, &typeId);
    return h ? (*h & kPtrMask) : 0;
}

static const char* SpawnShipInZone(const char* shipClass, uint64_t zoneId, const double pos[3], uint64_t& shipId,
                                   const double* rot = nullptr, const char* ocPath = nullptr) {
    __try {
        const uintptr_t es = *g_tp.entitySystem;
        const uintptr_t cls = VCall<uintptr_t>(ClassRegistry(), 0x20, shipClass);
        if (!cls) return "unknown entity class";

        alignas(16) uint8_t params[0x800] = {};
        g_sp.ctor(params);
        g_sp.setClass(params, cls);
        const struct { double rot[4]; double pos[3]; double scale; } where = {
            { rot ? rot[0] : 0, rot ? rot[1] : 0, rot ? rot[2] : 0, rot ? rot[3] : 1 }, { pos[0], pos[1], pos[2] }, 1.0 };
        g_sp.setLocation(params, &where, zoneId);
        g_sp.setFlags(params, 0x1000);

        uintptr_t batch = 0;
        VCall<void>(es, 0xC8, &batch, "starcitzenofflinemods ship spawner",
                    static_cast<uint32_t>(g_sp.teamCategory(64)), static_cast<uint32_t>(0));
        if (!batch) return "couldn't create a spawn batch";
        uintptr_t attributes[2] = {};
        VCall<void>(es, 0x118, attributes);
        if (ocPath) {
            if (!attributes[0]) return "couldn't create spawn attributes";
            const uintptr_t paths = VCall<uintptr_t>(*g_sp.system, g_sp.pathMgrSlot);
            uint32_t pathId = paths ? VCall<uint32_t>(paths, g_sp.pathIdSlot, ocPath) : 0;
            const struct { const char* name; uint32_t* value; } field = { "ocFilename", &pathId };
            const struct { const void* field; void* writer; } setter = { &field, g_sp.attrWriter };
            VCall<void>(attributes[0], g_sp.attrSetSlot, "ocFilename", g_sp.attrTypeId(), uintptr_t(0), &setter);
        }
        uint64_t newId[2] = {};
        VCall<void>(batch, 0x10, newId, params, attributes);
        uintptr_t owned = batch;
        VCall<void>(es, 0xD8, &owned);
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
        const uintptr_t zone = VCall<uintptr_t>(daymar, 0x6E0);
        if (!zone) return "Daymar has no zone";
        zoneId = ZoneId(zone);
        if (!zoneId || ZoneFromId(zoneId) != zone) return "Daymar zone lookup mismatch";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while looking Daymar up";
    }
    const double pos[3] = { 0, 0, kDaymarRadius + kArrivalAltitude };
    return SpawnShipInZone(shipClass, zoneId, pos, shipId);
}

enum class SeatStep { NotReady, Sent, NoSeat, Fault };

struct SeatInfo { uintptr_t seat; uint32_t priority; bool occupied; char name[64]; };
static SeatInfo g_seatList[128];
static int      g_seatListCount;

static char __fastcall CollectSeat(uintptr_t seat) {
    if (!seat || g_seatListCount >= 128) return 1;
    SeatInfo& s = g_seatList[g_seatListCount++];
    s.seat = seat;
    s.occupied = Rd<uint64_t>(seat + 0x158) != 0;
    s.priority = g_sp.seatPriority(seat);
    const uintptr_t owner = Rd<uint64_t>(seat + 8) & kPtrMask;
    const char* name = owner ? VCall<const char*>(owner, 0x78) : nullptr;
    strncpy_s(s.name, name ? name : "?", _TRUNCATE);
    return 1;
}

static const SeatInfo* g_lastSeat = nullptr;

static SeatStep LinkIntoBestSeat(uintptr_t ship, uintptr_t user, bool logSeats) {
    g_seatListCount = 0;
    const uintptr_t ports = EntityComponent(ship, "IItemPortContainer");
    if (!ports) return SeatStep::NoSeat;
    const struct { void* invoke; uintptr_t manager; void* storage; } visitor = { reinterpret_cast<void*>(&CollectSeat), 1, nullptr };
    g_sp.forEachSeat(VCall<uintptr_t>(ports, 0x778), &visitor, 193);
    if (!g_seatListCount) return SeatStep::NoSeat;

    const SeatInfo* best = nullptr;
    for (int i = 0; i < g_seatListCount; ++i) {
        const SeatInfo& s = g_seatList[i];
        if (!s.occupied && (!best || s.priority > best->priority)) best = &s;
    }
    if (logSeats) {
        char line[1024] = "";
        for (int i = 0; i < g_seatListCount && strlen(line) < 900; ++i) {
            char one[96];
            snprintf(one, sizeof(one), "%s%s(%u%s)", i ? ", " : "", g_seatList[i].name, g_seatList[i].priority,
                     g_seatList[i].occupied ? ", taken" : "");
            strcat_s(line, one);
        }
        Log("[ship] %d seats: %s", g_seatListCount, line);
    }
    if (!best) return SeatStep::NoSeat;

    uint64_t actorHandle = 0, seatId = 0;
    g_sp.actorOfUser(user, &actorHandle);
    const uintptr_t actor = actorHandle & kPtrMask;
    g_sp.handleToId(reinterpret_cast<const void*>(best->seat + 8), &seatId);
    if (!actor || !seatId) return SeatStep::NoSeat;
    if (g_sp.isLinked(actor)) g_sp.forceDelink(g_sp.actorLink(actor));
    g_sp.forceLink(g_sp.actorLink(actor), seatId);
    g_lastSeat = best;
    Log("[ship] linking you into '%s' (priority %u%s)", best->name, best->priority, best->priority >= 1000 ? ", pilot" : "");
    return SeatStep::Sent;
}

static SeatStep TryOwnSeatPicker(uintptr_t ship, uintptr_t user, bool logSeats) {
    __try {
        return LinkIntoBestSeat(ship, user, logSeats);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool reported = false;
        if (!reported) { reported = true; Log("[ship] own seat picker faulted; using the game's seat choice"); }
        return SeatStep::NoSeat;
    }
}

static SeatStep SeatEntityInShip(uintptr_t entity, uint64_t shipId, bool logSeats) {
    const uintptr_t ship = VCall<uintptr_t>(*g_tp.entitySystem, 0x120, shipId);
    if (!ship) return SeatStep::NotReady;
    const uintptr_t seats = EntityComponent(ship, "ISCItemControllableManager");
    const uintptr_t user = EntityComponent(entity, "ISCItemUser");
    if (!seats || !user) return SeatStep::NotReady;
    if (g_sp.forEachSeat) {
        const SeatStep step = TryOwnSeatPicker(ship, user, logSeats);
        if (step != SeatStep::NoSeat) return step;
    }
    g_lastSeat = nullptr;
    return g_sp.findSeat(seats, user, nullptr) ? SeatStep::Sent : SeatStep::NoSeat;
}

static SeatStep SendSeatRequest(uint64_t shipId, bool logSeats) {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return SeatStep::NotReady;
        return SeatEntityInShip(entity, shipId, logSeats);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SeatStep::Fault;
    }
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

static void LogPlayerZone(uint64_t shipId) {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return;
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        const char* name = zone ? ZoneName(zone) : nullptr;
        char buf[96] = "?";
        if (name) strncpy_s(buf, name, _TRUNCATE);
        Log("[ship] after seat request: you're in zone '%s' (%llu), ship is %llu", buf,
            static_cast<unsigned long long>(zone ? ZoneId(zone) : 0), static_cast<unsigned long long>(shipId));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static struct {
    uint64_t id; DWORD since; DWORD lastSend; int sends; bool zoneLogged; bool flightReady; char name[64];
} g_seatJob;

static struct { uintptr_t seat; DWORD at; char name[64]; } g_powerJob;

static uintptr_t SeatDashboard(uintptr_t seat) {
    const uintptr_t seatEntity = Rd<uint64_t>(seat + 8) & kPtrMask;
    return seatEntity ? EntityComponent(seatEntity, "SCItemSeatDashboard") : 0;
}

static void RunPowerJob(DWORD now) {
    if (!g_powerJob.seat || static_cast<LONG>(now - g_powerJob.at) < 0) return;
    const uintptr_t seat = g_powerJob.seat;
    g_powerJob.seat = 0;
    uintptr_t dashboard = 0;
    __try {
        if (g_sp.toggleFlightReady && (dashboard = SeatDashboard(seat)) != 0) {
            const struct { void* invoke; uintptr_t manager; void* storage; } noCallback = {};
            g_sp.toggleFlightReady(*g_tp.entitySystem, dashboard, &noCallback);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dashboard = 0;
    }
    if (dashboard) {
        SetMenuStatus("You're in the %s's pilot seat - powered on (Flight Ready). Have fun!", g_powerJob.name);
    } else if (GameHasFocus()) {
        PressFlightReadyKey();
        SetMenuStatus("You're in the %s's pilot seat - pressed Flight Ready (R) for you.", g_powerJob.name);
    } else {
        SetMenuStatus("You're in the %s's pilot seat. Press R (Flight Ready) to power up.", g_powerJob.name);
    }
}

static void FinishSeatJob(DWORD now) {
    const SeatInfo* seat = g_lastSeat;
    const bool pilot = seat && seat->priority >= 1000;
    if (pilot && g_seatJob.flightReady) {
        g_powerJob.seat = seat->seat;
        g_powerJob.at = now + 2500;
        strncpy_s(g_powerJob.name, g_seatJob.name, _TRUNCATE);
        SetMenuStatus("You're in the %s's pilot seat - powering up...", g_seatJob.name);
    } else if (pilot) {
        SetMenuStatus("You're in the %s's pilot seat. Press R (Flight Ready) to power up.", g_seatJob.name);
    } else if (seat) {
        SetMenuStatus("You're in the %s, but its pilot seat wasn't free - you got '%s'.", g_seatJob.name, seat->name);
    } else {
        SetMenuStatus("You're in the %s. Have fun!", g_seatJob.name);
    }
    g_seatJob.id = 0;
}

static void UpdateSeatJob(DWORD now) {
    static DWORD lastCheck = 0;
    if (!g_seatJob.id || now - lastCheck < 500) return;
    lastCheck = now;
    if (g_seatJob.sends && now - g_seatJob.lastSend >= 1500) {
        if (PlayerAboard(g_seatJob.id, g_seatJob.name)) {
            FinishSeatJob(now);
            return;
        }
        if (!g_seatJob.zoneLogged) { g_seatJob.zoneLogged = true; LogPlayerZone(g_seatJob.id); }
    }
    if (now - g_seatJob.since > 180000) {
        SetMenuStatus("%s spawned, but I couldn't get you aboard within 3 minutes.", g_seatJob.name);
        g_seatJob.id = 0;
        return;
    }
    if (g_seatJob.lastSend && now - g_seatJob.lastSend < 4000) return;
    switch (SendSeatRequest(g_seatJob.id, g_seatJob.sends == 0)) {
    case SeatStep::NotReady: break;
    case SeatStep::NoSeat:   g_seatJob.lastSend = now; break;
    case SeatStep::Fault:    SetMenuStatus("Seating failed (fault)."); g_seatJob.id = 0; break;
    case SeatStep::Sent:
        g_seatJob.lastSend = now;
        g_seatJob.zoneLogged = false;
        if (++g_seatJob.sends > 8) { SetMenuStatus("Asked %s to seat you 8 times; it didn't take.", g_seatJob.name); g_seatJob.id = 0; }
        else Log("[ship] seat request %d sent to %s", g_seatJob.sends, g_seatJob.name);
        break;
    }
}

static void StartDaymarArrival(const char* shipClass, DWORD now) {
    uint64_t id = 0;
    if (const char* err = SpawnShipAboveDaymar(shipClass, id)) {
        SetMenuStatus("Going to Daymar failed: %s", err);
        return;
    }
    g_seatJob = { id, now };
    g_seatJob.flightReady = true;
    strncpy_s(g_seatJob.name, shipClass, _TRUNCATE);
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
        const uintptr_t data = comp ? Rd<uintptr_t>(comp + 0x208) : 0;
        if (!data) return;
        const uintptr_t state = data + 0x27F0;
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
    ReleaseSRWLockExclusive(&g_menuLock);
    if (req.pending && req.daymar && req.index >= 0 && req.index < g_menuShipCount) {
        StartDaymarArrival(g_menuShips[req.index].name, now);
    } else if (req.pending && req.index >= 0 && req.index < g_menuShipCount) {
        const char* name = g_menuShips[req.index].name;
        uint64_t id = 0;
        if (const char* err = SpawnShipAbovePlayer(name, req.height, id)) SetMenuStatus("Spawning %s failed: %s", name, err);
        else if (!req.sit) SetMenuStatus("Spawning %s %.0f m above you (big ships take up to a minute).", name, req.height);
        else {
            g_seatJob = { id, now };
            g_seatJob.flightReady = req.flightReady;
            strncpy_s(g_seatJob.name, name, _TRUNCATE);
            SetMenuStatus("Spawning %s - you'll be put in the pilot seat as soon as it's there (big ships take up to a minute).", name);
        }
    }

    static DWORD lastTestFile = 0;
    if (now - lastTestFile >= 2000) {
        lastTestFile = now;
        char path[MAX_PATH];
        const DWORD pn = GetEnvironmentVariableA("SC_OFFLINE_MOD_LOG", path, MAX_PATH);
        char* slash = pn && pn < MAX_PATH ? strrchr(path, '\\') : nullptr;
        char line[128] = "";
        if (slash) {
            strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), "spawn_test.txt");
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
                g_seatJob = { id, now };
                g_seatJob.flightReady = true;
                strncpy_s(g_seatJob.name, line, _TRUNCATE);
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
