#include "hooks.h"
#include "teleport.h"
#include <initializer_list>
#include <nmmintrin.h>
#include <share.h>

static uint8_t* g_cave    = nullptr;
static uint8_t* g_caveEnd = nullptr;
bool            g_hooksInstalled = false;

static bool AllocCaveNear(const uint8_t* anchor) {
    if (g_cave) return true;
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t a = reinterpret_cast<uintptr_t>(anchor) & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x70000000; d += gran) {
        for (uintptr_t cand : { a - d, a + d }) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(cand), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) { g_cave = static_cast<uint8_t*>(p); g_caveEnd = g_cave + 0x1000; return true; }
        }
    }
    return false;
}

static bool InstallDetour(uint8_t* target, size_t stolen, void* detour, void** original, DWORD& err) {
    const size_t need = 14 + stolen + 14;
    if (!g_cave || g_cave + need > g_caveEnd || stolen < 5 || stolen > 16) return false;
    uint8_t* relay = g_cave;
    relay[0] = 0xFF; relay[1] = 0x25; memset(relay + 2, 0, 4); memcpy(relay + 6, &detour, 8);
    uint8_t* tramp = relay + 14;
    memcpy(tramp, target, stolen);
    uint8_t* back = target + stolen;
    tramp[stolen] = 0xFF; tramp[stolen + 1] = 0x25; memset(tramp + stolen + 2, 0, 4); memcpy(tramp + stolen + 6, &back, 8);
    g_cave = tramp + stolen + 14;
    g_cave += (16 - reinterpret_cast<uintptr_t>(g_cave) % 16) % 16;

    const int64_t rel = relay - (target + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;
    uint8_t patch[16];
    patch[0] = 0xE9;
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(patch + 1, &rel32, 4);
    memset(patch + 5, 0x90, stolen - 5);
    *original = tramp;
    return WriteCode(target, patch, stolen, err);
}

using ValidateFilterFn     = uint64_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using ValidateProjectionFn = uint64_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static ValidateFilterFn     g_origValidateFilter     = nullptr;
static ValidateProjectionFn g_origValidateProjection = nullptr;

static const char* ArenaString(uintptr_t tagged) {
    const uintptr_t s = tagged & ~uintptr_t(3);
    if (!s) return "";
    return *reinterpret_cast<const size_t*>(s + 24) > 15 ? *reinterpret_cast<const char* const*>(s)
                                                          : reinterpret_cast<const char*>(s);
}

static void DescribeFilter(uintptr_t filter, char* out, size_t n) {
    __try {
        const int type = *reinterpret_cast<const int*>(filter + 28);
        const uintptr_t payload = *reinterpret_cast<const uintptr_t*>(filter + 16);
        if (type == 3 && payload) {
            const int count = Rd<int>(payload + 24);
            const uintptr_t rep = Rd<uintptr_t>(payload + 32);
            char kinds[64] = "";
            size_t k = 0;
            for (int i = 0; i < count && i < 8 && rep; ++i)
                k += snprintf(kinds + k, sizeof(kinds) - k, "%s%d", i ? "," : "", Rd<int>(Rd<uintptr_t>(rep + 8 + 8 * i) + 28));
            snprintf(out, n, "type=3 property='%.80s' op=%d values=%d kinds=[%s]",
                     ArenaString(Rd<uintptr_t>(payload + 40)), Rd<int>(payload + 48), count, kinds);
        }
        else if (type == 5 && payload)
            snprintf(out, n, "type=5 property='%.80s' bit=%d", ArenaString(*reinterpret_cast<const uintptr_t*>(payload + 16)),
                     *reinterpret_cast<const int*>(payload + 24));
        else if (type == 2 && payload)
            snprintf(out, n, "type=2 value='%.80s' kind=%d", ArenaString(*reinterpret_cast<const uintptr_t*>(payload + 16)),
                     *reinterpret_cast<const int*>(payload + 36));
        else
            snprintf(out, n, "type=%d payload=0x%llx", type, static_cast<unsigned long long>(payload));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(out, n, "<unreadable filter 0x%llx>", static_cast<unsigned long long>(filter));
    }
}

static void DescribeDescriptor(uintptr_t d, char* out, size_t n) {
    size_t k = 0;
    out[0] = '\0';
    for (uintptr_t off = 0; off < 0xE0 && k + 40 < n; off += 8) {
        __try {
            const uintptr_t p = Rd<uintptr_t>(d + off);
            const char* s = nullptr;
            if (p > 0x10000 && p < 0x7FFFFFFFFFFF) {
                const char* c = reinterpret_cast<const char*>(p);
                int len = 0;
                while (len < 64 && c[len] >= 0x20 && c[len] < 0x7F) ++len;
                if (len >= 3 && c[len] == '\0') s = c;
            }
            if (s) k += snprintf(out + k, n - k, " +%llx='%.48s'", static_cast<unsigned long long>(off), s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static void DumpPropertyRegistry(uintptr_t self) {
    static volatile LONG done = 0;
    if (InterlockedExchange(&done, 1)) return;
    __try {
        for (int t = 0; t < 8; ++t)
            Log("[inv] registry: property type %d allowed-op mask 0x%08x", t, Rd<uint32_t>(self + 4 * (638 + 3 * t)));
        const uintptr_t head = Rd<uintptr_t>(self + 0x440);
        int count = 0;
        for (uintptr_t node = Rd<uintptr_t>(head); node && node != head && count < 2000; node = Rd<uintptr_t>(node), ++count) {
            const uintptr_t d = Rd<uintptr_t>(node + 24);
            char names[512];
            DescribeDescriptor(d, names, sizeof(names));
            Log("[inv] registry: key=0x%08x type=%d desc=0x%llx%s", Rd<uint32_t>(node + 16), d ? Rd<int>(d + 20) : -1,
                static_cast<unsigned long long>(d), names);
        }
        Log("[inv] registry: %d properties", count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[inv] registry: dump faulted");
    }
}

static uint32_t Crc32c(const char* s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; ++s) c = _mm_crc32_u8(c, static_cast<uint8_t>(*s));
    return ~c;
}

static bool PropertyRegistered(uintptr_t self, uint32_t key) {
    const uintptr_t head = Rd<uintptr_t>(self + 0x440);
    int guard = 0;
    for (uintptr_t node = Rd<uintptr_t>(head); node && node != head && guard < 4000; node = Rd<uintptr_t>(node), ++guard)
        if (Rd<uint32_t>(node + 16) == key) return true;
    return false;
}

static const char* UnregisteredFilterProperty(uintptr_t self, uintptr_t filter) {
    __try {
        const int type = Rd<int>(filter + 28);
        const uintptr_t payload = Rd<uintptr_t>(filter + 16);
        if (!payload || (type != 3 && type != 5)) return nullptr;
        const char* name = ArenaString(Rd<uintptr_t>(payload + (type == 3 ? 40 : 16)));
        if (!name[0] || PropertyRegistered(self, Crc32c(name))) return nullptr;
        return name;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static uint64_t __fastcall Hook_ValidateFilter(uintptr_t self, uintptr_t parent, uintptr_t state, uintptr_t filter,
                                               uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8) {
    const uint64_t r = g_origValidateFilter(self, parent, state, filter, a5, a6, a7, a8);
    if (r == 1 && parent != 0) {
        if (const char* prop = UnregisteredFilterProperty(self, filter)) {
            static volatile LONG reported = 0;
            if (InterlockedIncrement(&reported) <= 20)
                Log("[inv] dropping filter on property '%.80s' (not indexed offline)", prop);
            return 4;
        }
    }
    if (r == 1 || r == 3) {
        DumpPropertyRegistry(self);
        char desc[256];
        DescribeFilter(filter, desc, sizeof(desc));
        Log("[inv] filter REJECTED (result %llu, parent 0x%llx): %s", static_cast<unsigned long long>(r),
            static_cast<unsigned long long>(parent), desc);
    }
    return r;
}

static uint64_t __fastcall Hook_ValidateProjection(uintptr_t self, uintptr_t state, uintptr_t projection, uintptr_t a4) {
    const uint64_t r = g_origValidateProjection(self, state, projection, a4);
    if (r != 0) Log("[inv] projection REJECTED (result %llu, projection 0x%llx)", static_cast<unsigned long long>(r),
                    static_cast<unsigned long long>(projection));
    return r;
}

using InstanceGroupQueryFn = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t*);
static InstanceGroupQueryFn g_origInstanceGroupQuery = nullptr;
static uintptr_t*           g_servicesManager = nullptr;

static bool PrepareInstanceGroupQuery(uint8_t* target) {
    static const uint8_t loadMgr[] = { 0x48, 0x8B, 0x0D };
    static const uint8_t callHub[] = { 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18 };
    if (memcmp(target + 0x5B, loadMgr, sizeof(loadMgr)) != 0 || memcmp(target + 0x99, callHub, sizeof(callHub)) != 0)
        return false;
    g_servicesManager = reinterpret_cast<uintptr_t*>(target + 0x5B + 7 + Rel32(target + 0x5E));
    return true;
}

static void __fastcall Hook_InstanceGroupQuery(uintptr_t self, uintptr_t key, uintptr_t* out) {
    uintptr_t hub = 0;
    __try {
        const uintptr_t mgr = *g_servicesManager;
        if (mgr) hub = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t)>(Rd<uintptr_t>(Rd<uintptr_t>(mgr) + 0x18))(mgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hub = 0;
    }
    if (hub) { g_origInstanceGroupQuery(self, key, out); return; }
    out[1] = out[0];
    static volatile LONG reported = 0;
    if (InterlockedIncrement(&reported) <= 5)
        Log("[fix] hangar instance-group lookup skipped: server services hub unavailable offline (prevented crash)");
}

using EntitlementsResultFn = void(__fastcall*)(uintptr_t, uintptr_t);
using RetrieveVehicleFn    = void(__fastcall*)(uintptr_t, uintptr_t);
static EntitlementsResultFn g_origEntitlementsResult = nullptr;
static RetrieveVehicleFn    g_origRetrieveVehicle = nullptr;
static void __fastcall Hook_EntitlementsResult(uintptr_t self, uintptr_t result);
static void __fastcall Hook_RetrieveVehicle(uintptr_t asop, uintptr_t slot);
static bool PrepareEntitlementsResult(uint8_t* target);
static bool PrepareRetrieveVehicle(uint8_t* target);

struct HookSpec {
    const char*   name;
    const char*   pattern;
    size_t      stolen;
    void*       detour;
    void**      original;
    bool      (*prepare)(uint8_t* target);
};

static const HookSpec kHooks[] = {
    { "inventory filter validator",
      "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 49 89 50 28 49 8B F8 41 8B 41 1C 48 8B DA",
      5, reinterpret_cast<void*>(&Hook_ValidateFilter), reinterpret_cast<void**>(&g_origValidateFilter), nullptr },
    { "inventory projection validator",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 49 8B F0",
      5, reinterpret_cast<void*>(&Hook_ValidateProjection), reinterpret_cast<void**>(&g_origValidateProjection), nullptr },
    { "hangar elevator crash guard",
      "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 48 8D AC 24 70 FE FF FF 48 81 EC 90 02 00 00 49 8B 00 49 8B F8",
      5, reinterpret_cast<void*>(&Hook_InstanceGroupQuery), reinterpret_cast<void**>(&g_origInstanceGroupQuery),
      &PrepareInstanceGroupQuery },
    { "fleet manager ship list (offline)",
      "40 55 56 41 56 48 8D AC 24 00 FF FF FF 48 81 EC 00 02 00 00 4C 8B F1 48 8B F2 48 83 C1 08 E8",
      5, reinterpret_cast<void*>(&Hook_EntitlementsResult), reinterpret_cast<void**>(&g_origEntitlementsResult),
      &PrepareEntitlementsResult },
    { "fleet manager retrieve -> spaceport ATC",
      "48 89 54 24 10 55 53 41 55 41 57 48 8D AC 24 A8 FE FF FF 48 81 EC 68 02 00 00 4C 8B FA 4C 8B E9 48 8B 51 08 48 8D 8D 80 01 00 00 E8",
      5, reinterpret_cast<void*>(&Hook_RetrieveVehicle), reinterpret_cast<void**>(&g_origRetrieveVehicle),
      &PrepareRetrieveVehicle },
};
static PatchStatus g_hookStatus[sizeof(kHooks) / sizeof(kHooks[0])];

void InstallHooks(const Section& text) {
    if (!AllocCaveNear(text.base)) {
        for (PatchStatus& st : g_hookStatus) st.result = PatchResult::ProtectFailed;
        return;
    }
    for (size_t i = 0; i < sizeof(kHooks) / sizeof(kHooks[0]); ++i) {
        const HookSpec& h = kHooks[i];
        PatchStatus& st = g_hookStatus[i];
        st.expected = 1;
        uint8_t* target = FindUniquePattern(text, h.pattern, st.sites);
        if (!target) { st.result = st.sites ? PatchResult::WrongMatchCount : PatchResult::NotFound; continue; }
        if (h.prepare && !h.prepare(target)) { st.result = PatchResult::NotFound; continue; }
        if (!InstallDetour(target, h.stolen, h.detour, h.original, st.err)) { st.result = PatchResult::ProtectFailed; continue; }
        st.result = PatchResult::Applied;
        st.at = target;
        g_hooksInstalled = true;
    }
}

void LogHooks() {
    for (size_t i = 0; i < sizeof(kHooks) / sizeof(kHooks[0]); ++i)
        LogPatch(kHooks[i].name, g_hookStatus[i]);
}

bool HookFunction(uint8_t* target, size_t stolen, void* detour, void** original) {
    DWORD err = 0;
    if (!target || !InstallDetour(target, stolen, detour, original, err)) return false;
    g_hooksInstalled = true;
    return true;
}

uint8_t* NearData(size_t n) {
    if (!g_text.base || !AllocCaveNear(g_text.base) || g_cave + n > g_caveEnd) return nullptr;
    uint8_t* p = g_cave;
    g_cave += n;
    g_cave += (16 - reinterpret_cast<uintptr_t>(g_cave) % 16) % 16;
    return p;
}

constexpr size_t   kEntitlementSize = 0x150;
constexpr uint16_t kUrnType = 0x1E11;
constexpr uint64_t kShipUrnMarker = 0x53434F4600000000ull;
constexpr size_t   kSlotUrn = 0x1698;
constexpr size_t   kAsopAtcId = 0x9F8;

struct FleetShip { char name[64]; uint64_t guid[2]; };
// Same bound as the spawn menu (spawner.cpp kMaxMenuShips): ships.txt holds 1102 entries, and the
// old 1024 here dropped the last 78 from the fleet manager without saying so.
constexpr int kMaxFleetShips = 2048;

static FleetShip* g_ships = nullptr;
static int        g_shipCount = 0;
static uint8_t*   g_fakeEntitlements = nullptr;
static SRWLOCK    g_shipsLock = SRWLOCK_INIT;
static bool       g_shipsBuilt = false;

using StrCtorFn          = void*(__fastcall*)(void* out, const char* s);
using StrDtorFn          = void(__fastcall*)(void* s);
using GetATCCompFn       = uint64_t*(__fastcall*)(uint64_t* out, uint64_t entityId);
using RequestTakingOffFn = void(__fastcall*)(uintptr_t atc, uint64_t player, void* location, uint64_t vehicle,
                                             bool spawnVehicle, void* archetype, void* padFilter);
static StrCtorFn          g_strCtor = nullptr;

bool MakeCryString(void* out, const char* s) {
    if (!g_strCtor) return false;
    g_strCtor(out, s);
    return true;
}
static StrDtorFn          g_strDtor = nullptr;

void FreeCryString(void* s) {
    if (g_strDtor) g_strDtor(s);
}
static GetATCCompFn       g_getATCComp = nullptr;
static RequestTakingOffFn g_requestTakingOff = nullptr;

static bool PrepareEntitlementsResult(uint8_t* target) {
    const uint8_t* msg = FindCString(g_rdata, "QueryEntitlements failed with error: $$");
    return msg && BytesMatch(target + 0x55, "4C 8D 0D") && target + 0x55 + 7 + Rel32(target + 0x58) == msg;
}

static bool PrepareRetrieveVehicle(uint8_t* target) {
    if (!BytesMatch(target + 0xC7, "49 8B 95 F8 09 00 00") || !BytesMatch(target + 0xDD, "E8")) return false;
    const uint8_t* usage = FindCString(g_rdata,
        "Invalid arguments. Usage: g_ATC_requestTakeOff <atc_name (autocompletable)> [<ship_archetype>] [<pad_name_filter>]");
    const uint8_t* lea = usage ? FindRipLea(g_text, 0x48, 0x8D, 0x15, usage) : nullptr;
    if (!lea) return false;
    const uint8_t* cmd = lea - 0x1CD;
    if (!BytesMatch(cmd, "40 55 53 48 8B EC 48 83 EC 78") || !BytesMatch(cmd + 0x72, "E8")
        || !BytesMatch(cmd + 0xA9, "E8") || !BytesMatch(cmd + 0x190, "E8"))
        return false;
    g_getATCComp       = reinterpret_cast<GetATCCompFn>(target + 0xDD + 5 + Rel32(target + 0xDE));
    g_strCtor          = reinterpret_cast<StrCtorFn>(cmd + 0x72 + 5 + Rel32(cmd + 0x73));
    g_strDtor          = reinterpret_cast<StrDtorFn>(cmd + 0xA9 + 5 + Rel32(cmd + 0xAA));
    g_requestTakingOff = reinterpret_cast<RequestTakingOffFn>(cmd + 0x190 + 5 + Rel32(cmd + 0x191));
    return true;
}

static uint64_t LocalPlayerId() {
    const uintptr_t mgr = *g_tp.clientMgr;
    const uintptr_t sub = mgr ? Rd<uintptr_t>(mgr + 0xE0) : 0;
    const uintptr_t info = sub ? VCall<uintptr_t>(sub, 0x2E0) : 0;
    return info ? Rd<uint64_t>(info + 8) : 0;
}

static void FindClassGuids(uintptr_t registry, const uintptr_t* classes) {
    const uintptr_t head = Rd<uintptr_t>(registry + 0x48);
    uintptr_t stack[256];
    int sp = 0;
    const uintptr_t root = Rd<uintptr_t>(head + 0x08);
    if (root && !Rd<uint8_t>(root + 0x19)) stack[sp++] = root;
    while (sp) {
        const uintptr_t n = stack[--sp];
        const uintptr_t cls = Rd<uintptr_t>(n + 0x30);
        for (int i = 0; i < g_shipCount; ++i)
            if (classes[i] == cls) { g_ships[i].guid[0] = Rd<uint64_t>(n + 0x20); g_ships[i].guid[1] = Rd<uint64_t>(n + 0x28); }
        for (size_t off : { size_t(0x00), size_t(0x10) }) {
            const uintptr_t c = Rd<uintptr_t>(n + off);
            if (c && !Rd<uint8_t>(c + 0x19) && sp < 256) stack[sp++] = c;
        }
    }
}

static int BuildFleetShips() {
    char path[MAX_PATH];
    if (!ShipsFilePath(path, sizeof(path)) || !g_tp.entitySystem || !*g_tp.entitySystem) return 0;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) { Log("[fleet] can't open %s", path); return 0; }
    static FleetShip ships[kMaxFleetShips];
    static uintptr_t classes[kMaxFleetShips];
    g_ships = ships;
    int missing = 0;
    char line[128];
    const uintptr_t registry = VCall<uintptr_t>(*g_tp.entitySystem, 0xC0);
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char* name = line + strspn(line, " \t");
        if (!*name) continue;
        if (g_shipCount >= kMaxFleetShips) { Log("[fleet] more than %d ships in %s; the rest are left out", kMaxFleetShips, path); break; }
        const uintptr_t cls = VCall<uintptr_t>(registry, 0x20, static_cast<const char*>(name));
        if (!cls) { if (++missing <= 5) Log("[fleet] unknown ship class '%s' (skipped)", name); continue; }
        strncpy_s(ships[g_shipCount].name, name, _TRUNCATE);
        classes[g_shipCount++] = cls;
    }
    fclose(f);
    FindClassGuids(registry, classes);

    int kept = 0;
    for (int i = 0; i < g_shipCount; ++i) {
        uint64_t guid[2] = { ships[i].guid[0], ships[i].guid[1] };
        if ((guid[0] | guid[1]) && VCall<uintptr_t>(registry, 0x18, guid) == classes[i]) ships[kept++] = ships[i];
        else Log("[fleet] no class GUID for '%s' (skipped)", ships[i].name);
    }
    g_shipCount = kept;

    g_fakeEntitlements = static_cast<uint8_t*>(calloc(kept ? kept : 1, kEntitlementSize));
    for (int i = 0; g_fakeEntitlements && i < kept; ++i) {
        uint8_t* e = g_fakeEntitlements + i * kEntitlementSize;
        *reinterpret_cast<uint16_t*>(e + 0x00) = kUrnType;
        e[0x08] = 5;
        *reinterpret_cast<uint64_t*>(e + 0x10) = kShipUrnMarker | static_cast<uint32_t>(i);
        e[0x20] = 1;
        e[0x50] = 3;
        *reinterpret_cast<uint32_t*>(e + 0x54) = 1;
        memcpy(e + 0x58, ships[i].guid, 16);
    }
    if (!g_fakeEntitlements) g_shipCount = 0;
    Log("[fleet] %d ships available in the fleet manager (%d unknown names)", g_shipCount, missing);
    return g_shipCount;
}

static int EnsureFleetShips() {
    AcquireSRWLockExclusive(&g_shipsLock);
    if (!g_shipsBuilt) {
        g_shipsBuilt = true;
        __try { BuildFleetShips(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_shipCount = 0; Log("[fleet] fault while building the ship list"); }
    }
    const int n = g_shipCount;
    ReleaseSRWLockExclusive(&g_shipsLock);
    return n;
}

static void __fastcall Hook_EntitlementsResult(uintptr_t self, uintptr_t result) {
    bool failed = false;
    __try { failed = Rd<uint8_t>(result) != 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int n = failed ? EnsureFleetShips() : 0;
    if (!n) { g_origEntitlementsResult(self, result); return; }
    struct { uint8_t ok; uint8_t pad[7]; uint8_t* begin; uint8_t* end; uint8_t* cap; uint8_t spare[64]; } fake = {};
    fake.ok = 1;
    fake.begin = g_fakeEntitlements;
    fake.end = fake.cap = g_fakeEntitlements + n * kEntitlementSize;
    static volatile LONG reported = 0;
    if (InterlockedIncrement(&reported) <= 3) Log("[fleet] online ship list unavailable -> listing %d offline ships", n);
    g_origEntitlementsResult(self, reinterpret_cast<uintptr_t>(&fake));
}

static int SlotShipIndex(uintptr_t slot) {
    __try {
        const uintptr_t urn = slot + kSlotUrn;
        if (Rd<uint16_t>(urn) != kUrnType || Rd<uint8_t>(urn + 0x20) != 1) return -1;
        const uint64_t id = Rd<uint64_t>(urn + 0x10);
        const int i = static_cast<int>(static_cast<uint32_t>(id));
        return (id & 0xFFFFFFFF00000000ull) == kShipUrnMarker && i < g_shipCount ? i : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const char* RequestShipFromAtc(uint64_t atcEntity, uint64_t player, const char* shipClass) {
    if (!g_getATCComp || !g_requestTakingOff) return "the ATC functions weren't found";
    __try {
        uint64_t handle[2] = {};
        g_getATCComp(handle, atcEntity);
        const uintptr_t atc = handle[0] & kPtrMask;
        if (!atc) return "this terminal has no spaceport ATC";
        if (!player) return "player not spawned";
        alignas(16) uint8_t location[32] = {};
        void* archetype = nullptr;
        void* padFilter = nullptr;
        g_strCtor(location, "");
        g_strCtor(&archetype, shipClass);
        g_strCtor(&padFilter, "");
        g_requestTakingOff(atc, player, location, 0, true, &archetype, &padFilter);
        g_strDtor(&padFilter);
        g_strDtor(&archetype);
        g_strDtor(location);
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while asking the ATC";
    }
}

static void __fastcall Hook_RetrieveVehicle(uintptr_t asop, uintptr_t slot) {
    const int i = SlotShipIndex(slot);
    if (i < 0) { g_origRetrieveVehicle(asop, slot); return; }
    uint64_t atc = 0;
    __try { atc = Rd<uint64_t>(asop + kAsopAtcId); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (const char* err = RequestShipFromAtc(atc, LocalPlayerId(), g_ships[i].name))
        Log("[fleet] retrieve %s failed: %s", g_ships[i].name, err);
    else Log("[fleet] retrieve %s: asked the spaceport ATC for a hangar/pad", g_ships[i].name);
}
