#include "npc.h"
#include "spawner.h"
#include "teleport.h"
#include "menu.h"
#include "build.h"
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

static int32_t g_removeSlot = 0;

void ResolveNpcApi(const Section& text) {
    uint8_t* sites[16] = {};
    const int n = FindPattern(text, "48 8B 0D ?? ?? ?? ?? 48 8B 13 48 8B 01 FF 90 ?? ?? ?? ?? 48 83 C3 08 48 3B DF 75 E4", sites, 16);
    int32_t slot = 0;
    bool agree = n > 0 && n <= 16;
    for (int i = 0; agree && i < n; ++i) {
        const int32_t s = Rel32(sites[i] + 15);
        if (reinterpret_cast<uintptr_t*>(sites[i] + 7 + Rel32(sites[i] + 3)) != g_tp.entitySystem) continue;
        if (slot && s != slot) agree = false;
        slot = s;
    }
    if (agree && slot > 0 && slot < 0x1000) g_removeSlot = slot;
    else Log("[npc] RemoveEntity not found; Clear NPCs disabled");
}

void Menu_RequestClearNpcs() { InterlockedExchange(&g_clearRequested, 1); }

void RemoveEntityById(uint64_t id) {
    if (!g_removeSlot || !id) return;
    __try { VCall<void>(*g_tp.entitySystem, g_removeSlot, id); } __except (EXCEPTION_EXECUTE_HANDLER) {}
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

static bool NpcsFilePath(char* path, DWORD n) {
    if (!ShipsFilePath(path, n)) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash || static_cast<DWORD>(slash + 1 - path) + 9 > n) return false;
    strcpy_s(slash + 1, n - static_cast<DWORD>(slash + 1 - path), "npcs.txt");
    return true;
}

static int BuildNpcList() {
    char path[MAX_PATH];
    if (!NpcsFilePath(path, sizeof(path))) return 0;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) { Log("[npc] can't open %s", path); return 0; }
    const uintptr_t registry = VCall<uintptr_t>(*g_tp.entitySystem, 0xC0);
    int n = 0, unknown = 0;
    char line[160];
    while (n < kMaxNpcs && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char* name = line + strspn(line, " \t");
        if (!*name) continue;
        if (!VCall<uintptr_t>(registry, 0x20, static_cast<const char*>(name))) { ++unknown; continue; }
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
}
