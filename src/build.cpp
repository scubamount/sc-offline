#include "build.h"
#include "spawner.h"
#include "teleport.h"
#include "npc.h"
#include "menu.h"
#include <cmath>
#include <share.h>

using FreeCamOnFn  = void(__fastcall*)(void* args);
using FreeCamOffFn = void(__fastcall*)();
static FreeCamOnFn    g_freeCamOn = nullptr;
static FreeCamOffFn   g_freeCamOff = nullptr;
static const uint8_t* g_freeCamFlag = nullptr;

static const uint8_t* RegisteredHandler(const Section& text, const uint8_t* name) {
    uint8_t* const end = text.base + text.size - 7;
    for (uint8_t* p = text.base + 0xF; name && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x15 || p + 7 + Rel32(p + 3) != name || !BytesMatch(p - 0xF, "4C 8D 05")) continue;
        const uint8_t* h = p - 0xF + 7 + Rel32(p - 0xF + 3);
        return h >= text.base && h + 0x40 < text.base + text.size ? h : nullptr;
    }
    return nullptr;
}

static int __fastcall FreeCamArgCount(void*) { return 2; }
static const char* __fastcall FreeCamArg(void*, int index) { return index == 1 ? "2" : "FreeCamEnable"; }
static void* const kFreeCamArgsVtbl[] = { nullptr, reinterpret_cast<void*>(&FreeCamArgCount), reinterpret_cast<void*>(&FreeCamArg), nullptr };
static void* const kFreeCamArgs[] = { const_cast<void**>(kFreeCamArgsVtbl) };

static bool LoadsString(const uint8_t* at, const uint8_t* str) {
    return str && BytesMatch(at, "48 8D 0D") && at + 7 + Rel32(at + 3) == str;
}

using ReleaseGridFn = void(__fastcall*)(uintptr_t grid);
static uintptr_t*    g_physWorld = nullptr;
static ReleaseGridFn g_releaseGrid = nullptr;
static const char*   g_rayTag = nullptr;

static bool GroundRaySite(const Section& text, const uint8_t* L) {
    if (L - 0xBF < text.base || L + 0xF9 > text.base + text.size || !BytesMatch(L - 0xBF, "48 8B 3D") || !BytesMatch(L - 0x42, "48 8B 98 C0 01 00 00")
        || !BytesMatch(L + 0x07, "C7 85 9C 00 00 00 01 01 00 00") || !BytesMatch(L + 0x26, "48 C7 85 A0 00 00 00 0F 02 00 00")
        || !BytesMatch(L + 0x80, "FF 50 30 41 B8 1E 00 00 00") || !BytesMatch(L + 0x9A, "FF D3") || !BytesMatch(L + 0xF2, "33 D2 E8"))
        return false;
    const uint8_t* release = L + 0xF9 + Rel32(L + 0xF5);
    uintptr_t* world = reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(L - 0xB8 + Rel32(L - 0xBC)));
    if (release < text.base || release + 0x14 > text.base + text.size
        || !BytesMatch(release, "48 8B D1 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 FF A0 28 02 00 00")
        || reinterpret_cast<uintptr_t*>(const_cast<uint8_t*>(release + 10 + Rel32(release + 6))) != world)
        return false;
    g_physWorld   = world;
    g_releaseGrid = reinterpret_cast<ReleaseGridFn>(const_cast<uint8_t*>(release));
    return true;
}

static void ResolveGroundRay(const Section& text, const Section& rdata) {
    const uint8_t* tag = FindCString(rdata, "PlanetRayIntersection");
    g_rayTag = reinterpret_cast<const char*>(tag);
    uint8_t* const end = text.base + text.size - 7;
    for (uint8_t* p = text.base; tag && p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] == 0x8D && p[2] == 0x05 && p + 7 + Rel32(p + 3) == tag && GroundRaySite(text, p)) return;
    }
    Log("[build] ground ray not found; objects go where the camera points instead of on the ground");
}

bool ResolveBuildApi(const Section& text, const Section& rdata) {
    ResolveGroundRay(text, rdata);
    const uint8_t* on  = RegisteredHandler(text, FindCString(rdata, "FreeCamEnable"));
    const uint8_t* off = RegisteredHandler(text, FindCString(rdata, "FreeCamDisable"));
    if (on && LoadsString(on + 0x23, FindCString(rdata, "Enabling free cam")))
        g_freeCamOn = reinterpret_cast<FreeCamOnFn>(const_cast<uint8_t*>(on));
    if (off && LoadsString(off + 0x11, FindCString(rdata, "Disabling free cam"))
        && BytesMatch(off, "48 83 EC 28 80 3D ?? ?? ?? ?? 00")) {
        g_freeCamOff  = reinterpret_cast<FreeCamOffFn>(const_cast<uint8_t*>(off));
        g_freeCamFlag = off + 4 + 7 + Rel32(off + 6);
    }
    if (!g_freeCamOn || !g_freeCamOff) Log("[build] free camera not found; build mode disabled");
    return g_freeCamOn && g_freeCamOff;
}

constexpr int        kMaxBuild = 4096, kMaxCats = 24;
static char          g_build[kMaxBuild][192];
static int           g_buildCat[kMaxBuild];
static bool          g_buildNpc[kMaxBuild];
static char          g_cats[kMaxCats][24];
static int           g_catCount = 0;
static volatile LONG g_buildCount = -1;
static volatile LONG g_buildWanted = 0;

static struct {
    volatile LONG selected = 0;
    volatile LONG reachCm = 5000;
    volatile LONG toggle = 0, undo = 0, clear = 0;
} g_ui;

int Menu_BuildCount() {
    const LONG n = g_buildCount;
    if (n < 0) InterlockedExchange(&g_buildWanted, 1);
    return n;
}
const char* Menu_BuildName(int i) { return i >= 0 && i < g_buildCount ? g_build[i] : ""; }
const char* Menu_BuildCategory(int i) { return i >= 0 && i < g_buildCount ? g_cats[g_buildCat[i]] : ""; }
int Menu_BuildCategoryOf(int i) { return i >= 0 && i < g_buildCount ? g_buildCat[i] : -1; }
int Menu_BuildCategoryCount() { return g_buildCount > 0 ? g_catCount : 0; }
const char* Menu_BuildCategoryName(int c) { return c >= 0 && c < g_catCount ? g_cats[c] : ""; }
void Menu_SetBuild(int index, float reach) {
    InterlockedExchange(&g_ui.selected, index);
    InterlockedExchange(&g_ui.reachCm, static_cast<LONG>(reach * 100));
}
float Menu_BuildReach() { return g_ui.reachCm / 100.0f; }
void Menu_ToggleBuildMode() { InterlockedExchange(&g_ui.toggle, 1); }
void Menu_BuildUndo() { InterlockedExchange(&g_ui.undo, 1); }
void Menu_BuildClear() { InterlockedExchange(&g_ui.clear, 1); }

static struct {
    volatile LONG pending  = 0;
    volatile LONG index    = -1;
    volatile LONG inFront  = 1;
    volatile LONG aheadCm  = 800;
} g_placeReq;

void Menu_RequestPlace(int index, bool inFront, float aheadMetres) {
    if (index < 0 || index >= kMaxBuild) return;
    InterlockedExchange(&g_placeReq.index, index);
    InterlockedExchange(&g_placeReq.inFront, inFront ? 1 : 0);
    InterlockedExchange(&g_placeReq.aheadCm, static_cast<LONG>(aheadMetres * 100.0f));
    InterlockedExchange(&g_placeReq.pending, 1);
}

static bool IsNpcClass(const char* name) {
    static const char* const kPrefixes[] = { "NPC_", "AIShip_CrewProfiles", "PU_Pilots", "PU_Human", "Vanduul_Pilot" };
    for (const char* p : kPrefixes)
        if (_strnicmp(name, p, strlen(p)) == 0) return true;
    return false;
}

static int BuildList() {
    char path[MAX_PATH];
    if (!DataFilePath(path, sizeof(path), "buildables.txt")) return 0;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) { Log("[build] can't open %s", path); return 0; }
    const uintptr_t registry = VCall<uintptr_t>(*g_tp.entitySystem, 0xC0);
    int n = 0, cats = 0, cat = -1, unknown = 0;
    char line[256];
    while (n < kMaxBuild && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char* name = line + strspn(line, " \t");
        if (!*name) continue;
        if (*name == '[') {
            name[strcspn(name, "]")] = 0;
            if (cats < kMaxCats) { strncpy_s(g_cats[cats], name + 1, _TRUNCATE); cat = cats++; }
            continue;
        }
        if (cat < 0) continue;
        const size_t len = strlen(name);
        const bool prefab = len > 7 && _stricmp(name + len - 7, ".socpak") == 0;
        if (prefab ? !PrefabsReady() : !VCall<uintptr_t>(registry, 0x20, static_cast<const char*>(name))) { ++unknown; continue; }
        strncpy_s(g_build[n], name, _TRUNCATE);
        g_buildNpc[n] = _stricmp(g_cats[cat], "guards") == 0 || IsNpcClass(name);
        g_buildCat[n++] = cat;
    }
    fclose(f);
    g_catCount = cats;
    Log("[build] %d objects in the build menu (%d unknown names skipped)", n, unknown);
    return n;
}

static int g_slotsOk = -1;

static bool EntitySlotsOk(uintptr_t entity) {
    if (g_slotsOk < 0) {
        const uintptr_t vt = Rd<uintptr_t>(entity);
        g_slotsOk =
            BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(vt + 0x2B0)), "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57")
            && BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(vt + 0x2C0)), "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 70")
            && BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(vt + 0x2C8)), "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 48 8B F9 41 0F B6 F0");
        if (!g_slotsOk) Log("[build] entity move/rotate functions changed; the preview won't follow the camera");
    }
    return g_slotsOk > 0;
}

static uintptr_t EntityById(uint64_t id) { return id ? VCall<uintptr_t>(*g_tp.entitySystem, 0x120, id) : 0; }

static void QuatMul(const double a[4], const double b[4], double r[4]) {
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static int g_rayOk = -1;

static bool RaySlotsOk(uintptr_t entity) {
    if (g_rayOk < 0) {
        const uintptr_t vt = Rd<uintptr_t>(entity);
        g_rayOk = g_physWorld
            && BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(vt + 0x208)), "40 53 48 83 EC 20 48 8B 89 80 02 00 00 48 8B DA 48 8B 01 FF 50 30")
            && BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(vt + 0x430)), "48 89 5C 24 08 57 48 83 EC 20 41 0F B6 D8 48 8B FA E8 ?? ?? ?? ?? 48 85 C0 74 12 41 B1 01");
        if (!g_rayOk && g_physWorld) Log("[build] ground ray slots changed; objects go where the camera points instead of on the ground");
    }
    return g_rayOk > 0;
}

struct PhysSkipList {
    uint64_t slots[8];
    uint64_t heap;
    int32_t  count, capacity;
    int64_t  lock;
    int32_t  owner;
    uint8_t  depth, pad[3];
};
static_assert(sizeof(PhysSkipList) == 0x60, "PhysSkipList layout");
static PhysSkipList g_skip = { {}, 0, 0, 8, 0, -1, 0, {} };

static double CastInZone(uintptr_t zone, const double from[3], const double to[3]) {
    double o[3], e[3];
    if (!WorldToLocal(zone, from, o) || !WorldToLocal(zone, to, e)) return -1;
    const double d[3] = { e[0] - o[0], e[1] - o[1], e[2] - o[2] };
    const double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.01) return -1;
    uintptr_t ref = 0;
    const uintptr_t* grid = VCall<const uintptr_t*>(zone, 0x30, &ref);
    alignas(16) uint8_t rp[0xA8] = {};
    alignas(16) uint8_t hits[0x60 * 2] = {};
    memcpy(rp + 0x18, o, sizeof(o));
    for (int i = 0; i < 3; ++i) reinterpret_cast<float*>(rp + 0x30)[i] = static_cast<float>(d[i]);
    *reinterpret_cast<int32_t*>(rp + 0x3C)     = 0x101;
    *reinterpret_cast<uint64_t*>(rp + 0x40)    = 0x20F;
    *reinterpret_cast<void**>(rp + 0x48)       = hits;
    *reinterpret_cast<int32_t*>(rp + 0x50)     = 1;
    *reinterpret_cast<void**>(rp + 0x70)       = &g_skip;
    *reinterpret_cast<uintptr_t*>(rp + 0x80)   = grid ? *grid : 0;
    *reinterpret_cast<const char**>(rp + 0x88) = g_rayTag;
    const int n = grid && *grid ? VCall<int>(*g_physWorld, 0x1C0, static_cast<void*>(rp), 30) : 0;
    if (ref) g_releaseGrid(ref);
    const float dist = *reinterpret_cast<const float*>(hits + 0x10);
    if (n <= 0 || !*reinterpret_cast<const uint64_t*>(hits) || !(dist >= 0.0f) || dist > len) return -1;
    return dist / len;
}

static bool RayHit(uintptr_t zone, const double from[3], const double to[3], double hit[3]) {
    for (int depth = 0; zone && depth < 3; ++depth, zone = ZoneParent(zone)) {
        if (!ZoneParent(zone)) break;
        const double t = CastInZone(zone, from, to);
        if (t < 0) continue;
        for (int i = 0; i < 3; ++i) hit[i] = from[i] + (to[i] - from[i]) * t;
        return true;
    }
    return false;
}

bool GroundRay(uintptr_t zone, const double from[3], const double to[3], double hit[3]) {
    uintptr_t actor, entity;
    if (!zone || !GetLocalPlayer(actor, entity) || !RaySlotsOk(entity)) return false;
    g_skip.count = 0;
    return RayHit(zone, from, to, hit);
}

static double g_fromYou = 0;
static bool   g_grounded = false;

static double g_camPos[3], g_camFwd[3];   // camera, as of the last Target() call

static bool Target(double reach, double yaw, double lift, uint64_t previewId, double pos[3], double rot[4]) {
    uintptr_t actor, entity;
    if (!GetLocalPlayer(actor, entity)) return false;
    const uintptr_t cam = Rd<uintptr_t>(actor + 0x208);
    const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
    if (!cam || !zone) return false;
    const double* p = reinterpret_cast<const double*>(cam + 0x6D18);
    const float*  q = reinterpret_cast<const float*>(cam + 0x6D30);
    const double fwd[3] = { 2.0 * (q[0] * q[1] - q[3] * q[2]),
                            1.0 - 2.0 * (q[0] * q[0] + q[2] * q[2]),
                            2.0 * (q[1] * q[2] + q[3] * q[0]) };
    memcpy(g_camPos, p, sizeof(g_camPos));
    memcpy(g_camFwd, fwd, sizeof(g_camFwd));
    double you[3], stand[4] = { 0, 0, 0, 1 };
    Vec3Out(entity, 0x2B8, you);
    if (EntitySlotsOk(entity)) {
        double buf[4] = {};
        const double* r = VCall<const double*>(entity, 0x2C8, buf, static_cast<uint8_t>(0));
        if (r) memcpy(stand, r, sizeof(stand));
    }
    const double upLocal[3] = { 2.0 * (stand[0] * stand[2] + stand[3] * stand[1]),
                                2.0 * (stand[1] * stand[2] - stand[3] * stand[0]),
                                1.0 - 2.0 * (stand[0] * stand[0] + stand[1] * stand[1]) };
    double head[3], feetW[3], headW[3], up[3];
    for (int i = 0; i < 3; ++i) head[i] = you[i] + upLocal[i];
    LocalToWorld(zone, you, feetW);
    LocalToWorld(zone, head, headW);
    for (int i = 0; i < 3; ++i) up[i] = headW[i] - feetW[i];

    double world[3];
    for (int i = 0; i < 3; ++i) world[i] = p[i] + fwd[i] * reach;
    g_grounded = false;
    if (RaySlotsOk(entity)) {
        g_skip.count = 0;
        if (const uintptr_t preview = EntityById(previewId)) VCall<void>(preview, 0x430, &g_skip, true);
        double hit[3], from[3], down[3];
        const bool aimed = RayHit(zone, p, world, hit);
        for (int i = 0; i < 3; ++i) from[i] = aimed ? hit[i] - fwd[i] * 0.3 : world[i];
        for (int i = 0; i < 3; ++i) down[i] = from[i] - up[i] * 2000.0;
        if (RayHit(zone, from, down, world)) g_grounded = true;
        else if (aimed) { memcpy(world, hit, sizeof(world)); g_grounded = true; }
    }
    for (int i = 0; i < 3; ++i) world[i] += up[i] * lift;
    if (!WorldToLocal(zone, world, pos)) return false;
    g_fromYou = sqrt((pos[0] - you[0]) * (pos[0] - you[0]) + (pos[1] - you[1]) * (pos[1] - you[1]) + (pos[2] - you[2]) * (pos[2] - you[2]));
    const double half = yaw * 3.14159265358979323846 / 360.0;
    const double turn[4] = { 0, 0, sin(half), cos(half) };
    QuatMul(stand, turn, rot);
    return true;
}

bool PlaceNearPlayer(double ahead, double side, double lift, double pos[3], double rot[4]) {
    uintptr_t actor, entity;
    if (!GetLocalPlayer(actor, entity)) return false;
    const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
    if (!zone || !EntitySlotsOk(entity)) return false;
    double you[3], stand[4] = { 0, 0, 0, 1 }, buf[4] = {};
    Vec3Out(entity, 0x2B8, you);
    if (const double* r = VCall<const double*>(entity, 0x2C8, buf, static_cast<uint8_t>(0))) memcpy(stand, r, sizeof(stand));
    const double* q = stand;
    const double right[3] = { 1 - 2 * (q[1] * q[1] + q[2] * q[2]), 2 * (q[0] * q[1] + q[3] * q[2]), 2 * (q[0] * q[2] - q[3] * q[1]) };
    const double fwd[3]   = { 2 * (q[0] * q[1] - q[3] * q[2]), 1 - 2 * (q[0] * q[0] + q[2] * q[2]), 2 * (q[1] * q[2] + q[3] * q[0]) };
    const double up[3]    = { 2 * (q[0] * q[2] + q[3] * q[1]), 2 * (q[1] * q[2] - q[3] * q[0]), 1 - 2 * (q[0] * q[0] + q[1] * q[1]) };
    double chest[3], spot[3], chestW[3], spotW[3], upW[3], probe[3], probeW[3];
    for (int i = 0; i < 3; ++i) {
        chest[i] = you[i] + up[i] * 1.2;
        spot[i]  = chest[i] + fwd[i] * ahead + right[i] * side;
        probe[i] = you[i] + up[i];
    }
    LocalToWorld(zone, chest, chestW);
    LocalToWorld(zone, spot, spotW);
    LocalToWorld(zone, you, probeW);
    LocalToWorld(zone, probe, upW);
    for (int i = 0; i < 3; ++i) upW[i] -= probeW[i];

    double world[3];
    for (int i = 0; i < 3; ++i) world[i] = spotW[i] - upW[i] * 1.2;
    if (RaySlotsOk(entity)) {
        g_skip.count = 0;
        double hit[3], down[3], floor[3];
        const double d[3] = { spotW[0] - chestW[0], spotW[1] - chestW[1], spotW[2] - chestW[2] };
        const double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 0.6 && RayHit(zone, chestW, spotW, hit))
            for (int i = 0; i < 3; ++i) spotW[i] = hit[i] - d[i] / len * 0.5;
        for (int i = 0; i < 3; ++i) down[i] = spotW[i] - upW[i] * 50.0;
        if (RayHit(zone, spotW, down, floor)) memcpy(world, floor, sizeof(world));
    }
    for (int i = 0; i < 3; ++i) world[i] += upW[i] * lift;
    if (!WorldToLocal(zone, world, pos)) return false;
    const double turn[4] = { 0, 0, 1, 0 };
    QuatMul(stand, turn, rot);
    return true;
}

constexpr int   kMaxPlaced = 2048;
static uint64_t g_placed[kMaxPlaced];
static int      g_placedCount = 0;
static bool     g_active = false;
static uint64_t g_previewId = 0;
static int      g_previewIndex = -1;
static double   g_yaw = 0;

int  Menu_BuildPlacedCount() { return g_placedCount; }
bool Menu_BuildModeActive() { return g_active; }

static const char* SpawnBuildable(const char* name, const double pos[3], const double rot[4], uint64_t& id) {
    const size_t len = strlen(name);
    if (len > 7 && _stricmp(name + len - 7, ".socpak") == 0) return SpawnPrefabInPlayerZone(name, pos, rot, id);
    return SpawnEntityInPlayerZone(name, pos, rot, id);
}

static bool IsPrefab(const char* name) {
    const size_t len = strlen(name);
    return len > 7 && _stricmp(name + len - 7, ".socpak") == 0;
}

// A prefab (.socpak) lays its buildings out once, where it's first spawned, so a live prefab can't
// follow the camera. Prefabs are previewed with a small marker instead, and built where it stands.
static const char* PrefabMarker() {
    static const char* marker = nullptr;
    static bool tried = false;
    if (tried) return marker;
    tried = true;
    static const char* const kCandidates[] = {
        "PlayerDeco_Flair_Hanger_Flag_UEE_1", "PlayerDeco_Flair_Hanger_Flag_IAE_2955_BIS_1", "PlayerDeco_Flair_Heart_Table_1_a",
    };
    for (const char* c : kCandidates) {
        bool exists = false;
        __try { exists = EntityClassExists(c); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (exists) { marker = c; break; }
    }
    if (!marker) Log("[build] no prefab marker class found; prefabs preview as themselves (they won't follow the camera)");
    return marker;
}

// The real prefab, built at the marker once the camera has held still for a moment, so you can see
// the actual building: where it lines up and whether it clips. Moving the camera, rotating or
// changing reach takes it away again; clicking while it's up keeps it as the placed building.
static struct {
    uint64_t id; double pos[3], rot[4], camPos[3], camFwd[3]; double yaw, reach;
    DWORD stillSince; bool suppressed;
} g_ghost;

static void RemoveGhost() {
    if (g_ghost.id) RemoveEntityById(g_ghost.id);
    g_ghost.id = 0;
}

static void RemovePreview() {
    if (g_previewId) RemoveEntityById(g_previewId);
    g_previewId = 0;
    g_previewIndex = -1;
    RemoveGhost();
}

// True when the camera hasn't moved (or turned) since the last call that returned false.
static bool CameraStill() {
    double d = 0, dot = 0;
    for (int i = 0; i < 3; ++i) {
        d += (g_camPos[i] - g_ghost.camPos[i]) * (g_camPos[i] - g_ghost.camPos[i]);
        dot += g_camFwd[i] * g_ghost.camFwd[i];
    }
    if (d < 0.05 * 0.05 && dot > 0.9998) return true;
    memcpy(g_ghost.camPos, g_camPos, sizeof(g_camPos));
    memcpy(g_ghost.camFwd, g_camFwd, sizeof(g_camFwd));
    return false;
}

static void Enter() {
    if (!g_freeCamOn || g_buildCount <= 0) { Log("[build] build mode isn't available yet"); return; }
    __try { g_freeCamOn(const_cast<void**>(kFreeCamArgs)); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[build] fault while enabling the free camera"); return; }
    g_active = true;
    Log("[build] build mode on: left click = place, R = rotate, [ ] = shorter / longer reach, Backspace = undo, F6 = exit");
}

static void Exit() {
    RemovePreview();
    __try { g_freeCamOff(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[build] fault while disabling the free camera"); }
    g_active = false;
    Log("[build] build mode off (%d objects placed)", g_placedCount);
}

static void Undo() {
    if (!g_placedCount) return;
    RemoveEntityById(g_placed[--g_placedCount]);
}

static void Clear() {
    while (g_placedCount) RemoveEntityById(g_placed[--g_placedCount]);
    Log("[build] base cleared");
}

// Moves any entity within its zone (local coordinates). Used to send NPCs that can't be removed far away.
bool MoveEntityLocal(uint64_t id, const double pos[3]) {
    __try {
        const uintptr_t e = EntityById(id);
        if (!e || !EntitySlotsOk(e)) return false;
        VCall<void>(e, 0x2B0, pos, 0, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void MovePreview(const double pos[3], const double rot[4]) {
    __try {
        const uintptr_t e = EntityById(g_previewId);
        if (!e || !EntitySlotsOk(e)) return;
        VCall<void>(e, 0x2B0, pos, 0, false);
        VCall<void>(e, 0x2C0, rot, 0, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool GameWindowInFront() {
    wchar_t title[64] = L"";
    return GameHasFocus() && GetWindowTextW(GetForegroundWindow(), title, 64) && wcsstr(title, L"Star Citizen");
}

static bool Pressed(int vk, bool& was, bool allowed) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool hit = allowed && down && !was;
    was = down;
    return hit;
}

void ProcessBuild() {
    if (!g_tp.ok || !SpawnerReady()) return;
    if (!g_freeCamOn) return;
    if (g_buildCount < 0 && g_buildWanted) {
        uintptr_t actor, entity;
        bool live = false;
        __try { live = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (live) {
            int n = 0;
            __try { n = BuildList(); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; Log("[build] fault while reading buildables.txt"); }
            InterlockedExchange(&g_buildCount, n);
        }
    }

    static bool f6, lmb, r, closer, farther, back;
    const bool keys = GameWindowInFront();
    const bool toggleKey = Pressed(VK_F6, f6, keys);
    if (InterlockedExchange(&g_ui.toggle, 0) != 0 || toggleKey) { if (g_active) Exit(); else Enter(); }
    const bool undoKey = Pressed(VK_BACK, back, keys && g_active);
    if (InterlockedExchange(&g_ui.undo, 0) != 0 || undoKey) Undo();
    if (InterlockedExchange(&g_ui.clear, 0)) Clear();
    if (InterlockedExchange(&g_placeReq.pending, 0)) {
        const int   idx     = static_cast<int>(InterlockedCompareExchange(&g_placeReq.index, 0, 0));
        const bool  inFront = InterlockedCompareExchange(&g_placeReq.inFront, 0, 0) != 0;
        const double ahead  = InterlockedCompareExchange(&g_placeReq.aheadCm, 0, 0) / 100.0;
        const char* where   = inFront ? "in front of you" : "at your feet";
        if (idx < 0 || idx >= g_buildCount)
            Log("[build] can't spawn anything (%s)", g_buildCount < 0 ? "build list not loaded yet" : "bad selection");
        else if (g_placedCount >= kMaxPlaced)
            Log("[build] the base is full (%d objects)", kMaxPlaced);
        else {
            double pos[3], rot[4];
            bool ok = false;
            __try { ok = PlaceNearPlayer(inFront ? ahead : 0.0, 0.0, g_buildNpc[idx] ? 0.3 : 0.0, pos, rot); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[build] fault while looking for a spot %s", where); }
            if (!ok) Log("[build] no spot %s", where);
            else {
                uint64_t id = 0;
                if (const char* err = SpawnBuildable(g_build[idx], pos, rot, id))
                    Log("[build] spawning %s failed: %s", g_build[idx], err);
                else {
                    g_placed[g_placedCount++] = id;
                    Log("[build] spawned %s %s (%d in the base)", g_build[idx], where, g_placedCount);
                }
            }
        }
    }
    if (!g_active) return;
    if (g_freeCamFlag && !*g_freeCamFlag) {
        RemovePreview();
        g_active = false;
        Log("[build] the game turned the free camera off; build mode off (%d objects placed)", g_placedCount);
        return;
    }

    double reach = g_ui.reachCm / 100.0;
    if (Pressed(VK_OEM_4, closer, keys)) InterlockedExchange(&g_ui.reachCm, static_cast<LONG>((reach = max(5.0, reach - 5.0)) * 100));
    if (Pressed(VK_OEM_6, farther, keys)) InterlockedExchange(&g_ui.reachCm, static_cast<LONG>((reach = min(300.0, reach + 5.0)) * 100));
    if (Pressed('R', r, keys)) g_yaw = fmod(g_yaw + 45.0, 360.0);

    const int index = g_ui.selected;
    if (index < 0 || index >= g_buildCount) return;
    double pos[3], rot[4];
    bool aimed = false;
    const double lift = g_buildNpc[index] ? 0.3 : 0.0;
    __try { aimed = Target(reach, g_yaw, lift, g_previewId, pos, rot); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_rayOk > 0) { g_rayOk = 0; Log("[build] fault in the ground ray; objects go where the camera points instead"); }
    }
    if (!aimed) return;
    const char* where = g_grounded ? "on the ground" : "in the air (no ground under it)";

    if (index != g_previewIndex) {
        RemovePreview();
        uint64_t id = 0;
        const char* marker = IsPrefab(g_build[index]) ? PrefabMarker() : nullptr;
        if (const char* err = SpawnBuildable(marker ? marker : g_build[index], pos, rot, id)) { Log("[build] preview of %s failed: %s", g_build[index], err); g_previewIndex = index; return; }
        g_previewId = id;
        g_previewIndex = index;
        Log("[build] previewing %s %s %.1f m from you%s", g_build[index], where, g_fromYou, marker ? " (marker: the prefab is built where the flag stands)" : "");
    } else if (!g_ghost.id) {
        MovePreview(pos, rot);
    }

    const bool prefab = IsPrefab(g_build[index]) && PrefabMarker();
    if (prefab) {
        const DWORD now = GetTickCount();
        const bool still = CameraStill() && g_ghost.yaw == g_yaw && g_ghost.reach == reach;
        if (!still) {
            g_ghost.stillSince = now;
            g_ghost.yaw = g_yaw;
            g_ghost.reach = reach;
            g_ghost.suppressed = false;
            RemoveGhost();
        } else if (!g_ghost.id && !g_ghost.suppressed && now - g_ghost.stillSince > 500) {
            uint64_t id = 0;
            if (!SpawnBuildable(g_build[index], pos, rot, id)) {
                g_ghost.id = id;
                memcpy(g_ghost.pos, pos, sizeof(g_ghost.pos));
                memcpy(g_ghost.rot, rot, sizeof(g_ghost.rot));
            }
        }
    }

    if (Pressed(VK_LBUTTON, lmb, keys) && g_placedCount < kMaxPlaced) {
        uint64_t id = 0;
        if (prefab && g_ghost.id) {                      // keep the preview as the real thing
            g_placed[g_placedCount++] = g_ghost.id;
            g_ghost.id = 0;
            g_ghost.suppressed = true;                   // no second preview on top until the camera moves
            Log("[build] placed %s %s %.1f m from you (%d)", g_build[index], where, g_fromYou, g_placedCount);
        } else if (const char* err = SpawnBuildable(g_build[index], prefab && g_ghost.id ? g_ghost.pos : pos, rot, id)) {
            Log("[build] placing %s failed: %s", g_build[index], err);
        } else {
            g_placed[g_placedCount++] = id;
            Log("[build] placed %s %s %.1f m from you (%d)", g_build[index], where, g_fromYou, g_placedCount);
        }
    }
}
