#include "npc.h"
#include "spawner.h"
#include "teleport.h"
#include "menu.h"
#include "build.h"
#include "sco/game/actors.h"
#include "sco/game/features.h"
#include "sco/signatures.h"
#include <share.h>

constexpr int        kMaxNpcs = 4096;
static char          g_npcs[kMaxNpcs][96];
static volatile LONG g_npcCount = -1;
static volatile LONG g_npcWanted = 0;
static SRWLOCK       g_npcLock = SRWLOCK_INIT;
static struct { bool pending; int index; int count; } g_npcRequest;
static volatile LONG g_clearRequested = 0;

constexpr int        kMaxSpawned = 1024;
static uint64_t      g_spawned[kMaxSpawned];
static int           g_spawnedCount = 0;

namespace actors = sco::game::actors;

static int32_t g_removeSlot = 0;
static bool    g_directRows = false;   // the npc.direct_remove capability

// sco-core's npc.clear rows (sco/game/features.h) give the RemoveEntity slot; npc.direct_remove
// (sco/game/actors.h) the fallback below.
void ResolveNpcApi(const Section&) {
    if (!ActorsCapability("npc.clear", "npc", "Clear NPCs")) return;
    g_removeSlot = Rel32(sco::Sig("npc.remove_entity_call") + sco::game::features::kRemoveSlotDisp);
    g_directRows = ActorsCapability("npc.direct_remove", "npc", "direct removal fallback");
}

void Menu_RequestClearNpcs() { InterlockedExchange(&g_clearRequested, 1); }

// The entity system's RemoveEntity takes an entity HANDLE (pointer + tag bits), not an id: its first
// step is a handle validity check, so the ids this mod used to pass were refused every time (that's
// why Clear NPCs, Kick and build mode's undo never removed anything). Ids are converted first now.
//
// RemoveEntity can also hand the removal to a manager that never finishes it offline, leaving the
// entity flagged "being removed" but still there. So each removal is checked 1.5 s later and, if the
// entity is still around, removed with the internal function RemoveEntity itself ends in.
struct PendingRemoval { uint64_t id; DWORD at; bool direct; };
constexpr int         kMaxPendingRemovals = 512;
static PendingRemoval g_pendingRemovals[kMaxPendingRemovals];
static int            g_pendingRemovalCount = 0;

using DirectRemoveFn = bool(__fastcall*)(uintptr_t entitySystem, uint64_t handle);
static DirectRemoveFn g_directRemove = nullptr;
static int            g_directState = 0;     // 0 not tried, 1 found, -1 not found

static void ResolveDirectRemove() {
    if (g_directState) return;
    g_directState = -1;
    if (!g_directRows) return;
    __try {
        // The entity system's RemoveEntity must be the function npc.remove_entity found.
        const uint8_t* fn = *reinterpret_cast<const uint8_t* const*>(*reinterpret_cast<const uintptr_t*>(*g_tp.entitySystem) + g_removeSlot);
        if (fn != sco::Sig("npc.remove_entity")) {
            Log("[npc] RemoveEntity's code changed; direct removal fallback disabled");
            return;
        }
        g_directRemove = reinterpret_cast<DirectRemoveFn>(sco::Sig("npc.direct_remove"));
        g_directState = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[npc] fault while locating the direct removal fallback");
    }
}

// Entity system method 0x128 fills in an entity's handle from its id (the infinite ammo code uses it).
// It works for spawned NPCs, which the teleport code's player-handle helper doesn't (v0.8.2's log
// showed every NPC handle coming back empty). That helper stays as a second try.
static uint64_t HandleOf(uint64_t id) {
    uint64_t handle = 0;
    __try {
        uint64_t out = 0;
        if (const uint64_t* h = VCall<const uint64_t*>(*g_tp.entitySystem, actors::kEsHandleById, &out, id)) handle = *h;
        if (!(handle & kPtrMask)) reinterpret_cast<void(__fastcall*)(uint64_t*, uint64_t)>(g_tp.handleFromId)(&handle, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return (handle & kPtrMask) ? handle : 0;
}

static bool CallRemove(uint64_t id) {
    const uint64_t handle = HandleOf(id);
    if (!handle) return false;
    __try { return VCall<bool>(*g_tp.entitySystem, g_removeSlot, handle); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool CallDirectRemove(uint64_t id) {
    ResolveDirectRemove();
    const uint64_t handle = g_directRemove ? HandleOf(id) : 0;
    if (!handle) return false;
    __try { return g_directRemove(*g_tp.entitySystem, handle); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool EntityStillExists(uint64_t id) {
    __try { return VCall<uintptr_t>(*g_tp.entitySystem, 0x120, id) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void RemoveEntityById(uint64_t id) {
    if (!g_removeSlot || !id) return;
    CallRemove(id);
    if (g_pendingRemovalCount < kMaxPendingRemovals) g_pendingRemovals[g_pendingRemovalCount++] = { id, GetTickCount(), false };
}

// Last resort for entities whose removal is handed to an owner that never carries it out offline
// (spawned NPCs): out of any seat, then ~17,000 km away, far outside streaming range.
static void Banish(uint64_t id) {
    UnseatEntityById(id);
    const double away[3] = { 1.0e7, 1.0e7, 1.0e7 };
    if (!MoveEntityLocal(id, away)) Log("[npc] couldn't remove or move entity %llu", static_cast<unsigned long long>(id));
}

static void VerifyRemovals() {
    const DWORD now = GetTickCount();
    for (int i = 0; i < g_pendingRemovalCount;) {
        PendingRemoval& r = g_pendingRemovals[i];
        if (now - r.at < 1500) { ++i; continue; }
        bool done = true;
        if (EntityStillExists(r.id)) {
            if (!r.direct) {
                const bool ok = CallDirectRemove(r.id);
                r.direct = true;
                r.at = now;
                if (ok) done = false;           // check once more after it's had a frame
                else Banish(r.id);
            } else {
                Banish(r.id);
            }
        }
        if (done) g_pendingRemovals[i] = g_pendingRemovals[--g_pendingRemovalCount];
        else ++i;
    }
}

bool CanRemoveEntities() { return g_removeSlot != 0; }
int32_t RemoveEntitySlot() { return g_removeSlot; }

void TrackSpawnedNpc(uint64_t id) {
    if (id && g_spawnedCount < kMaxSpawned) g_spawned[g_spawnedCount++] = id;
}

static void ClearNpcs() {
    if (!g_removeSlot) { Log("[npc] can't clear: RemoveEntity not found"); return; }
    for (int i = 0; i < g_spawnedCount; ++i) RemoveEntityById(g_spawned[i]);
    Log("[npc] cleared %d NPCs", g_spawnedCount);
    g_spawnedCount = 0;
}

int Menu_NpcCount() {
    const LONG n = g_npcCount;
    if (n < 0) InterlockedExchange(&g_npcWanted, 1);
    return n;
}

const char* Menu_NpcName(int index) { return index >= 0 && index < g_npcCount ? g_npcs[index] : ""; }

void Menu_RequestNpc(int index, int count) {
    AcquireSRWLockExclusive(&g_npcLock);
    g_npcRequest = { true, index, count };
    ReleaseSRWLockExclusive(&g_npcLock);
}

static int BuildNpcList() {
    char path[MAX_PATH];
    if (!DataFilePath(path, sizeof(path), "npcs.txt")) return 0;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) { Log("[npc] can't open %s", path); return 0; }
    const uintptr_t registry = VCall<uintptr_t>(*g_tp.entitySystem, actors::kEsClassRegistry);
    int n = 0, unknown = 0;
    char line[160];
    while (n < kMaxNpcs && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char* name = line + strspn(line, " \t");
        if (!*name) continue;
        if (!VCall<uintptr_t>(registry, actors::kRegistryFindClass, static_cast<const char*>(name))) { ++unknown; continue; }
        strncpy_s(g_npcs[n++], name, _TRUNCATE);
    }
    fclose(f);
    Log("[npc] %d NPCs in the menu (%d unknown names skipped)", n, unknown);
    return n;
}

static void SpawnNpcs(const char* npc, int count) {
    int spawned = 0;
    const char* err = nullptr;
    for (int i = 0; i < count && !err; ++i) {
        double pos[3], rot[4];
        bool placed = false;
        __try { placed = PlaceNearPlayer(3.0, (i - (count - 1) * 0.5) * 1.2, 0.2, pos, rot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { placed = false; }
        uint64_t id = 0;
        if (placed) err = SpawnEntityInPlayerZone(npc, pos, rot, id);
        else {
            const double offset[3] = { 2.0 + 1.5 * i, 2.0, 0.3 };
            err = SpawnEntityNearPlayer(npc, offset, id);
        }
        if (err) break;
        ++spawned;
        if (g_spawnedCount < kMaxSpawned) g_spawned[g_spawnedCount++] = id;
    }
    if (err) Log("[npc] spawning %s failed: %s", npc, err);
    if (spawned) Log("[npc] spawned %d x %s", spawned, npc);
}

void ProcessNpcs() {
    if (!g_tp.ok || !SpawnerReady()) return;
    if (g_npcCount < 0 && g_npcWanted) {
        uintptr_t actor, entity;
        bool live = false;
        __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (live) {
            int n = 0;
            __try { n = BuildNpcList(); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; Log("[npc] fault while reading npcs.txt"); }
            InterlockedExchange(&g_npcCount, n);
        }
    }
    AcquireSRWLockExclusive(&g_npcLock);
    const auto req = g_npcRequest;
    g_npcRequest.pending = false;
    ReleaseSRWLockExclusive(&g_npcLock);
    if (req.pending && req.index >= 0 && req.index < g_npcCount)
        SpawnNpcs(g_npcs[req.index], req.count < 1 ? 1 : req.count > 10 ? 10 : req.count);
    if (InterlockedExchange(&g_clearRequested, 0)) ClearNpcs();
    VerifyRemovals();
}
