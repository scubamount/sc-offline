#include "travel.h"
#include "teleport.h"
#include "spawner.h"
#include "menu.h"
#include <share.h>
#include <cctype>
#include <algorithm>

// =============================================================================================
// Shared state. The menu thread reads snapshots; the game thread owns every change.
// =============================================================================================

constexpr int kMaxPlaces    = 3000;
constexpr int kMaxBookmarks = 400;

static SRWLOCK        g_travelLock = SRWLOCK_INIT;
static TravelPlace    g_places[kMaxPlaces];
static int            g_placeCount = 0;
static TravelBookmark g_marks[kMaxBookmarks];
static Spot           g_markSpots[kMaxBookmarks];   // game thread only
static int            g_markCount = 0;
static char           g_currentSystem[32] = "";
static volatile LONG  g_placesVersion = 0;      // bumped whenever g_places changes

enum ReqKind { Req_None, Req_Place, Req_Bookmark, Req_Save, Req_Delete, Req_Scan };
static struct { int kind; TravelPlace place; float altitude; int index; char name[64]; } g_req;

static void Queue(int kind, const TravelPlace* place = nullptr, float altitude = 0, int index = -1, const char* name = nullptr) {
    AcquireSRWLockExclusive(&g_travelLock);
    g_req = {};
    g_req.kind = kind;
    if (place) g_req.place = *place;
    g_req.altitude = altitude;
    g_req.index = index;
    if (name) strncpy_s(g_req.name, name, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_travelLock);
}

void Travel_RequestPlace(const TravelPlace& place, float altitude) { Queue(Req_Place, &place, altitude); }
void Travel_RequestBookmark(int index)                            { Queue(Req_Bookmark, nullptr, 0, index); }
void Travel_RequestSaveBookmark(const char* name)                 { Queue(Req_Save, nullptr, 0, -1, name); }
void Travel_RequestDeleteBookmark(int index)                      { Queue(Req_Delete, nullptr, 0, index); }
void Travel_RequestScan()                                         { Queue(Req_Scan); }

int Travel_PlacesVersion() { return g_placesVersion; }

int Travel_GetPlaces(TravelPlace* out, int max) {
    AcquireSRWLockShared(&g_travelLock);
    const int n = g_placeCount < max ? g_placeCount : max;
    memcpy(out, g_places, n * sizeof(TravelPlace));
    ReleaseSRWLockShared(&g_travelLock);
    return n;
}

int Travel_GetBookmarks(TravelBookmark* out, int max) {
    AcquireSRWLockShared(&g_travelLock);
    const int n = g_markCount < max ? g_markCount : max;
    memcpy(out, g_marks, n * sizeof(TravelBookmark));
    ReleaseSRWLockShared(&g_travelLock);
    return n;
}

void Travel_CurrentSystem(char* out, size_t n) {
    AcquireSRWLockShared(&g_travelLock);
    strncpy_s(out, n, g_currentSystem, _TRUNCATE);
    ReleaseSRWLockShared(&g_travelLock);
}

// =============================================================================================
// Files
// =============================================================================================

static void Trim(char* s) {
    char* start = s;
    while (*start == ' ' || *start == '\t') ++start;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) s[--len] = 0;
}

static void Capitalize(char* s) { if (s[0] >= 'a' && s[0] <= 'z') s[0] -= 'a' - 'A'; }

static void Underscores(char* s) { for (char* c = s; *c; ++c) if (*c == '_') *c = ' '; }

// Interiors and small zones: real zones, but not places you'd pick from a list.
static bool IsMinorZone(const char* n) {
    static const char* const kMinor[] = {
        "_int", "int_", "entrance_", "elev", "lobby", "restaurant", "sfce", "ObjectContainer", "_segment",
        "social_", "gascloud", "LocationHarvestable", "hangar", "rstop_hatch", "rs_habs", "rs_cargo", "rs_cz_",
        "rs_comm_", "rs_entry_", "deck", "dummy", "_occu", "ab_mine_", "rs_refin", "reststop_",
    };
    char low[96];
    strncpy_s(low, n, _TRUNCATE);
    for (char* c = low; *c; ++c) *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
    for (const char* m : kMinor) {
        char lm[32];
        strncpy_s(lm, m, _TRUNCATE);
        for (char* c = lm; *c; ++c) *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
        if (strstr(low, lm)) return true;
    }
    return false;
}

// What the game calls its places, per system:
//   OOC_Stanton_2b_Daymar        Stanton, moon 2b, "Daymar"
//   OOC_Stanton_4_Microtech      Stanton, planet 4
//   OOC_Stanton1_L3              Stanton, Lagrange point L3 of body 1   (shown as "Hurston L3")
//   OOC_Stanton2a_CommArray      Stanton, comm array near body 2a
//   OOC_JumpPoint_stanton_pyro   Stanton, "Jump point to Pyro"
//   jumppoint_pyro_nyx           Pyro, "Jump point to Nyx"
//   pyro2 / pyro5a               Pyro, planet 2 / moon 5a
//   P3_L2                        (system from its zone), Lagrange point L2 of body 3
// Anything else keeps its own name, with the system taken from the zone it sits in.
// Returns false when nothing here applies; p.system is "" when only the zone can tell.
static bool DescribeEntity(const char* entity, TravelPlace& p) {
    p.kind = Place_Other;
    p.body[0] = 0;
    p.system[0] = 0;
    strncpy_s(p.entity, entity, _TRUNCATE);

    const char* jp = nullptr;
    if (_strnicmp(entity, "OOC_JumpPoint_", 14) == 0) jp = entity + 14;
    else if (_strnicmp(entity, "jumppoint_", 10) == 0) jp = entity + 10;
    if (jp) {
        const size_t len = strcspn(jp, "_");
        if (!len || len >= sizeof(p.system) || !jp[len]) return false;
        memcpy(p.system, jp, len);
        p.system[len] = 0;
        Capitalize(p.system);
        char to[48];
        strncpy_s(to, jp + len + 1, _TRUNCATE);
        Capitalize(to);
        Underscores(to);
        snprintf(p.name, sizeof(p.name), "Jump point to %s", to);
        return true;
    }

    // pyro2, pyro5a: a system word, one digit, maybe a moon letter
    {
        size_t letters = 0;
        while (isalpha(static_cast<unsigned char>(entity[letters]))) ++letters;
        const char* d = entity + letters;
        static const char* const kSystems[] = { "stanton", "pyro", "nyx", "castra", "terra", "magnus", "odin", "sol" };
        bool known = false;
        for (const char* k : kSystems) known |= strlen(k) == letters && _strnicmp(entity, k, letters) == 0;
        if (known && letters < sizeof(p.system) && isdigit(static_cast<unsigned char>(d[0]))
            && (d[1] == 0 || (isalpha(static_cast<unsigned char>(d[1])) && d[2] == 0))) {
            memcpy(p.system, entity, letters);
            p.system[letters] = 0;
            Capitalize(p.system);
            strncpy_s(p.body, d, _TRUNCATE);
            p.kind = d[1] ? Place_Moon : Place_Planet;
            snprintf(p.name, sizeof(p.name), "%s %s", p.system, d);
            return true;
        }
    }

    // P3_L2: Lagrange point of body 3
    if ((entity[0] == 'P' || entity[0] == 'p') && isdigit(static_cast<unsigned char>(entity[1])) && entity[2] == '_'
        && (entity[3] == 'L' || entity[3] == 'l') && isdigit(static_cast<unsigned char>(entity[4])) && entity[5] == 0) {
        p.body[0] = entity[1];
        p.body[1] = 0;
        snprintf(p.name, sizeof(p.name), "L%c", entity[4]);
        return true;
    }

    if (_strnicmp(entity, "OOC_", 4) != 0) return false;
    const char* rest = entity + 4;
    const size_t tokLen = strcspn(rest, "_");
    if (!tokLen || !rest[tokLen]) return false;
    size_t letters = 0;
    while (letters < tokLen && isalpha(static_cast<unsigned char>(rest[letters]))) ++letters;
    if (!letters || letters >= sizeof(p.system)) return false;
    memcpy(p.system, rest, letters);
    p.system[letters] = 0;
    const char* after = rest + tokLen + 1;

    if (letters < tokLen) {                                   // OOC_Stanton1a_CommArray
        const size_t idx = tokLen - letters;
        if (idx < sizeof(p.body)) { memcpy(p.body, rest + letters, idx); p.body[idx] = 0; }
        strncpy_s(p.name, after, _TRUNCATE);
        Underscores(p.name);
        return true;
    }
    const size_t digits = strspn(after, "0123456789");          // OOC_Stanton_2b_Daymar
    const size_t t2 = strcspn(after, "_");
    if (digits && after[t2] == '_' && t2 < sizeof(p.body)) {
        if (t2 == digits) p.kind = Place_Planet;
        else if (t2 == digits + 1 && isalpha(static_cast<unsigned char>(after[digits]))) p.kind = Place_Moon;
        if (p.kind != Place_Other) {
            memcpy(p.body, after, t2);
            p.body[t2] = 0;
            strncpy_s(p.name, after + t2 + 1, _TRUNCATE);
            Underscores(p.name);
            return true;
        }
    }
    strncpy_s(p.name, after, _TRUNCATE);
    Underscores(p.name);
    return true;
}

// "L3" near body 1 -> "Hurston L3", once Hurston (planet 1) is in the list. Caller holds the lock.
static void NameByBody() {
    ++g_placesVersion;
    for (int i = 0; i < g_placeCount; ++i) {
        TravelPlace& p = g_places[i];
        if (p.kind != Place_Other || !p.body[0]) continue;
        for (int j = 0; j < g_placeCount; ++j) {
            const TravelPlace& b = g_places[j];
            if (b.kind == Place_Other || _stricmp(b.body, p.body) != 0 || _stricmp(b.system, p.system) != 0) continue;
            if (_strnicmp(p.name, b.name, strlen(b.name)) == 0) break;   // already renamed
            char renamed[64];
            snprintf(renamed, sizeof(renamed), "%s %s", b.name, p.name);
            strcpy_s(p.name, renamed);
            break;
        }
    }
}

static bool HavePlace(const char* entity) {
    for (int i = 0; i < g_placeCount; ++i)
        if (_stricmp(g_places[i].entity, entity) == 0) return true;
    return false;
}

static bool AddPlace(const TravelPlace& p) {     // caller holds the lock
    if (g_placeCount >= kMaxPlaces || HavePlace(p.entity)) return false;
    g_places[g_placeCount++] = p;
    ++g_placesVersion;
    return true;
}

// Line format: system | name shown | entity name | radius in metres (0 = unknown)
static void LoadPlaces(const char* file, bool curated) {
    char path[MAX_PATH];
    if (!DataFilePath(path, sizeof(path), file)) return;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) return;
    char line[512];
    int added = 0, stale = 0;
    AcquireSRWLockExclusive(&g_travelLock);
    while (fgets(line, sizeof(line), f)) {
        Trim(line);
        if (!line[0] || line[0] == '#') continue;
        char* field[4] = {};
        int n = 0;
        for (char* p = line; p && n < 4; ++n) {
            field[n] = p;
            p = strchr(p, '|');
            if (p) *p++ = 0;
        }
        if (n < 3) continue;
        for (int i = 0; i < n; ++i) Trim(field[i]);
        TravelPlace p = {};
        const bool parsed = DescribeEntity(field[2], p);
        if (!parsed) {
            strncpy_s(p.name, field[2], _TRUNCATE);
            Underscores(p.name);
            p.kind = IsMinorZone(field[2]) ? Place_Minor : Place_Other;
        }
        // Curated lines (locations.txt) choose their own names; found ones are re-read from the entity
        // name, so older files with rough names get the current naming.
        if (field[0][0] && (curated || !p.system[0])) strncpy_s(p.system, field[0], _TRUNCATE);
        if (field[1][0] && curated) strncpy_s(p.name, field[1], _TRUNCATE);
        // System zone ids change from session to session, so a "SolarSystem_<id>" saved earlier is meaningless.
        if (_strnicmp(p.system, "SolarSystem", 11) == 0) { ++stale; continue; }
        p.radius = n > 3 ? atof(field[3]) : 0.0;
        if (p.entity[0] && AddPlace(p)) ++added;
    }
    NameByBody();
    ReleaseSRWLockExclusive(&g_travelLock);
    fclose(f);
    Log("[travel] %d places from %s%s", added, file, stale ? " (older entries with unnamed systems skipped; run the scan again)" : "");
}

static void AppendFoundPlace(const TravelPlace& p) {
    char path[MAX_PATH];
    if (!DataFilePath(path, sizeof(path), "locations_found.txt")) return;
    const bool fresh = GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
    FILE* f = _fsopen(path, "a", _SH_DENYNO);
    if (!f) return;
    if (fresh) fprintf(f, "# Places found by the in-game scan (Travel tab). Same format as locations.txt.\n"
                          "# Radius 0 = unknown; put the real radius in metres to arrive right above the surface.\n");
    fprintf(f, "%s | %s | %s | 0\n", p.system, p.name, p.entity);
    fclose(f);
}

static void SaveBookmarks() {
    char path[MAX_PATH];
    if (!DataFilePath(path, sizeof(path), "bookmarks.txt")) return;
    FILE* f = _fsopen(path, "w", _SH_DENYNO);
    if (!f) { Log("[travel] couldn't write %s", path); return; }
    fprintf(f, "# Saved spots from the Travel tab.\n");
    for (int i = 0; i < g_markCount; ++i) {
        fprintf(f, "bookmark %s|%s\n", g_marks[i].system, g_marks[i].name);
        for (int z = 0; z < g_markSpots[i].n; ++z) {
            const ZoneSpot& e = g_markSpots[i].z[z];
            fprintf(f, "spot %.6f %.6f %.6f %s\n", e.local[0], e.local[1], e.local[2], e.name);  // secret-scan: allow e.local is a struct member, not a hostname
        }
    }
    fclose(f);
}

static void LoadBookmarks() {
    char path[MAX_PATH];
    if (!DataFilePath(path, sizeof(path), "bookmarks.txt")) return;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) return;
    char line[256];
    AcquireSRWLockExclusive(&g_travelLock);
    int cur = -1;
    while (fgets(line, sizeof(line), f)) {
        Trim(line);
        if (strncmp(line, "bookmark ", 9) == 0) {
            if (g_markCount >= kMaxBookmarks) break;
            cur = g_markCount++;
            g_marks[cur] = {};
            g_markSpots[cur] = Spot{};
            char* bar = strchr(line + 9, '|');
            if (bar) { *bar = 0; strncpy_s(g_marks[cur].system, line + 9, _TRUNCATE); strncpy_s(g_marks[cur].name, bar + 1, _TRUNCATE); }
            else strncpy_s(g_marks[cur].name, line + 9, _TRUNCATE);
        } else if (cur >= 0 && strncmp(line, "spot ", 5) == 0 && g_markSpots[cur].n < kMaxZoneDepth) {
            ZoneSpot& e = g_markSpots[cur].z[g_markSpots[cur].n];
            if (sscanf_s(line, "spot %lf %lf %lf %95[^\r\n]", &e.local[0], &e.local[1], &e.local[2],  // secret-scan: allow e.local is a struct member, not a hostname
                         e.name, static_cast<unsigned>(sizeof(e.name))) == 4)
                ++g_markSpots[cur].n;
        }
    }
    // drop empty bookmarks
    int kept = 0;
    for (int i = 0; i < g_markCount; ++i)
        if (g_markSpots[i].n) { g_marks[kept] = g_marks[i]; g_markSpots[kept] = g_markSpots[i]; ++kept; }
    g_markCount = kept;
    ReleaseSRWLockExclusive(&g_travelLock);
    fclose(f);
    Log("[travel] %d saved spots", g_markCount);
}

// =============================================================================================
// Location scan
//
// Everything the level places at load (planets, moons, stations, Lagrange points, comm arrays, jump
// points...) gets an entity id just above 200,000,000,000, well below the ids of things spawned while
// you play. Walking that range with the game's own id lookup and keeping the entities that host a
// zone finds every place in every loaded system in a few seconds, whatever it's called.
// =============================================================================================

constexpr uint64_t kStaticIdBase = 200000000000ull;
constexpr uint64_t kIdsPerTick   = 40000;
constexpr uint64_t kGiveUpGap    = 3000000;     // stop this far past the last entity found
constexpr uint64_t kMaxSpan      = 80000000;

static struct {
    bool active; uint64_t next, lastHit; int entities, zones, added, ships, deep; DWORD start;
    int newFrom;   // first g_places index added by this scan
} g_scan;

// SolarSystem_<id> zone name -> the name its object containers use ("Stanton", "Pyro").
struct SystemName { char zone[48]; char name[32]; };
static SystemName g_systemNames[16];
static int        g_systemNameCount = 0;

static void LearnSystemName(const char* zone, const char* name) {
    for (int i = 0; i < g_systemNameCount; ++i) if (_stricmp(g_systemNames[i].zone, zone) == 0) return;
    if (g_systemNameCount < 16) {
        strncpy_s(g_systemNames[g_systemNameCount].zone, zone, _TRUNCATE);
        strncpy_s(g_systemNames[g_systemNameCount].name, name, _TRUNCATE);
        ++g_systemNameCount;
        Log("[travel] %s is %s", zone, name);
    }
}

static const char* KnownSystemName(const char* zone) {
    for (int i = 0; i < g_systemNameCount; ++i) if (_stricmp(g_systemNames[i].zone, zone) == 0) return g_systemNames[i].name;
    return nullptr;
}

bool Travel_Scanning(float& progress) {
    if (!g_scan.active) return false;
    const double span = static_cast<double>(g_scan.lastHit - kStaticIdBase + kGiveUpGap);
    const double done = static_cast<double>(g_scan.next - kStaticIdBase);
    progress = static_cast<float>(done / span > 1.0 ? 1.0 : done / span);
    return true;
}

static void StartScan(DWORD now) {
    if (g_scan.active) { SetMenuStatus("A scan is already running."); return; }
    g_scan = {};
    g_scan.active = true;
    g_scan.next = g_scan.lastHit = kStaticIdBase;
    g_scan.start = now;
    AcquireSRWLockShared(&g_travelLock);
    g_scan.newFrom = g_placeCount;
    ReleaseSRWLockShared(&g_travelLock);
    SetMenuStatus("Scanning for places...");
}

// What the scan learns about one entity. Kept apart from the bookkeeping so the fenced reads have
// nothing to unwind.
struct Probe { bool zoneHost, ship; int depth; char name[96]; char systemZone[48]; };

static bool ProbeEntity(uint64_t id, Probe& out) {
    out = {};
    __try {
        const uintptr_t e = VCall<uintptr_t>(*g_tp.entitySystem, 0x120, id);
        if (!e) return false;
        const char* name = VCall<const char*>(e, 0x78);
        strncpy_s(out.name, name ? name : "", _TRUNCATE);
        const uintptr_t zone = VCall<uintptr_t>(e, 0x6E0);
        if (!zone) return true;
        out.zoneHost = true;
        out.ship = EntityComponent(e, "IItemPortContainer") != 0;
        const uintptr_t sys = SystemZoneOf(zone);
        if (!sys || sys == zone) return true;
        const char* sysName = ZoneName(sys);
        strncpy_s(out.systemZone, sysName ? sysName : "", _TRUNCATE);
        for (uintptr_t z = zone; z && z != sys && out.depth < 8; z = ZoneParent(z)) ++out.depth;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.zoneHost = false;
    }
    return true;
}

static void FinishScan(DWORD now) {
    g_scan.active = false;
    AcquireSRWLockExclusive(&g_travelLock);
    ++g_placesVersion;
    for (int i = 0; i < g_placeCount; ++i)             // name the systems we learned
        if (_strnicmp(g_places[i].system, "SolarSystem", 11) == 0)
            if (const char* known = KnownSystemName(g_places[i].system)) strncpy_s(g_places[i].system, known, _TRUNCATE);
    NameByBody();
    const int from = g_scan.newFrom, to = g_placeCount;
    static TravelPlace added[kMaxPlaces];
    int n = 0;
    for (int i = from; i < to; ++i) added[n++] = g_places[i];
    ReleaseSRWLockExclusive(&g_travelLock);
    int unnamed = 0;
    for (int i = 0; i < n; ++i) {
        if (_strnicmp(added[i].system, "SolarSystem", 11) == 0) { ++unnamed; continue; }   // ids change per session
        AppendFoundPlace(added[i]);
    }
    if (unnamed) Log("[travel] %d places are in a system whose name wasn't found (shown, but not saved)", unnamed);
    Log("[travel] scan: ids %llu..%llu, %d entities, %d zone hosts (%d ships skipped, %d too deep), %d new places, %.1f s",
        static_cast<unsigned long long>(kStaticIdBase), static_cast<unsigned long long>(g_scan.lastHit), g_scan.entities,
        g_scan.zones, g_scan.ships, g_scan.deep, n, (now - g_scan.start) / 1000.0);
    SetMenuStatus("Scan finished: %d new place%s.", n, n == 1 ? "" : "s");
}

static void StepScan(DWORD now) {
    if (!g_scan.active) return;
    for (uint64_t k = 0; k < kIdsPerTick; ++k) {
        const uint64_t id = g_scan.next++;
        Probe pr;
        if (!ProbeEntity(id, pr)) continue;
        g_scan.lastHit = id;
        ++g_scan.entities;
        if (!pr.zoneHost || !pr.name[0] || !pr.systemZone[0]) continue;
        ++g_scan.zones;
        if (pr.ship || _strnicmp(pr.name, "StreamingSOC", 12) == 0) { ++g_scan.ships; continue; }
        if (pr.depth > 2) { ++g_scan.deep; continue; }
        TravelPlace p = {};
        if (!DescribeEntity(pr.name, p)) {
            strncpy_s(p.name, pr.name, _TRUNCATE);
            Underscores(p.name);
            p.kind = IsMinorZone(pr.name) ? Place_Minor : Place_Other;
        }
        // Only names that say which system they're in (OOC_<system>_..., jumppoint_<system>_...,
        // pyro2) teach us the system's name; P3_L2 and the like just take it from their zone.
        if (p.system[0] && (_strnicmp(pr.name, "OOC_", 4) == 0 || _strnicmp(pr.name, "jumppoint_", 10) == 0))
            LearnSystemName(pr.systemZone, p.system);
        if (!p.system[0]) {
            const char* known = KnownSystemName(pr.systemZone);
            strncpy_s(p.system, known ? known : pr.systemZone, _TRUNCATE);
        }
        AcquireSRWLockExclusive(&g_travelLock);
        AddPlace(p);
        ReleaseSRWLockExclusive(&g_travelLock);
    }
    if (g_scan.next - g_scan.lastHit > kGiveUpGap || g_scan.next - kStaticIdBase > kMaxSpan) FinishScan(now);
}

// =============================================================================================
// Going places
// =============================================================================================

static const char* GoToPlace(const TravelPlace& p, float altitude) {
    uintptr_t entity;
    uint64_t id;
    if (!FindEntityByNameEx(p.entity, entity, id))
        return "isn't loaded - it may be in another system, or the name in the list is wrong (try the scan)";
    uintptr_t zone = 0;
    __try { zone = VCall<uintptr_t>(entity, 0x6E0); } __except (EXCEPTION_EXECUTE_HANDLER) { zone = 0; }
    if (zone) {
        // Planets and moons are zones of their own: arrive straight above the north pole.
        double height = altitude;
        if (p.radius > 0) height += p.radius;
        else if (p.kind == Place_Planet) height = 9.0e6;      // radius unknown: well clear of even a gas giant
        else if (p.kind == Place_Moon) height = 1.5e6;
        else height = altitude < 2000 ? 2000 : altitude;
        const double local[3] = { 0, 0, height };
        return TeleportIntoZone(zone, local);
    }
    if (!id) return "found, but it has no id to teleport to";
    return TeleportToEntity(id, altitude < 50 ? altitude : 50);
}

static void HandleRequest(DWORD now) {
    AcquireSRWLockExclusive(&g_travelLock);
    const auto req = g_req;
    g_req.kind = Req_None;
    ReleaseSRWLockExclusive(&g_travelLock);

    switch (req.kind) {
    case Req_Place: {
        if (const char* err = GoToPlace(req.place, req.altitude)) SetMenuStatus("%s %s.", req.place.name, err);
        else if (req.place.radius > 0)
            SetMenuStatus("Teleported to %s, %.0f m above the surface.", req.place.name, req.altitude);
        else if (req.place.kind != Place_Other)
            SetMenuStatus("Teleported to %s. Its size isn't known, so you're in orbit - fly in from here.", req.place.name);
        else
            SetMenuStatus("Teleported to %s.", req.place.name);
        break;
    }
    case Req_Bookmark: {
        if (req.index < 0 || req.index >= g_markCount) break;
        const TravelBookmark mark = g_marks[req.index];
        if (const char* err = GoToSpot(g_markSpots[req.index], now, "bookmark")) SetMenuStatus("Can't go to '%s': %s.", mark.name, err);
        else SetMenuStatus("Teleported to '%s'.", mark.name);
        break;
    }
    case Req_Save: {
        Spot s;
        if (const char* err = CaptureCurrentSpot(s)) { SetMenuStatus("Couldn't save this spot: %s.", err); break; }
        if (g_markCount >= kMaxBookmarks) { SetMenuStatus("You have %d saved spots; delete one first.", kMaxBookmarks); break; }
        TravelBookmark mark = {};
        SpotSystemName(s, mark.system, sizeof(mark.system));
        if (!mark.system[0]) strcpy_s(mark.system, "Unknown system");
        if (req.name[0]) strncpy_s(mark.name, req.name, _TRUNCATE);
        else strncpy_s(mark.name, s.z[0].name, _TRUNCATE);
        AcquireSRWLockExclusive(&g_travelLock);
        g_marks[g_markCount] = mark;
        g_markSpots[g_markCount] = s;
        ++g_markCount;
        ReleaseSRWLockExclusive(&g_travelLock);
        SaveBookmarks();
        SetMenuStatus("Saved '%s' (%s).", mark.name, mark.system);
        break;
    }
    case Req_Delete: {
        if (req.index < 0 || req.index >= g_markCount) break;
        char name[64];
        strcpy_s(name, g_marks[req.index].name);
        AcquireSRWLockExclusive(&g_travelLock);
        for (int i = req.index; i + 1 < g_markCount; ++i) { g_marks[i] = g_marks[i + 1]; g_markSpots[i] = g_markSpots[i + 1]; }
        --g_markCount;
        ReleaseSRWLockExclusive(&g_travelLock);
        SaveBookmarks();
        SetMenuStatus("Deleted '%s'.", name);
        break;
    }
    case Req_Scan:
        StartScan(now);
        break;
    default:
        break;
    }
}

void ProcessTravel(DWORD now) {
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        LoadPlaces("locations.txt", true);
        LoadPlaces("locations_found.txt", false);
        LoadBookmarks();
    }
    static DWORD lastSystem = 0;
    if (now - lastSystem > 2000) {
        lastSystem = now;
        char sys[48];
        CurrentSystemName(sys, sizeof(sys));
        if (!sys[0]) {                                   // no OOC_ names around: use the system zone
            CurrentSystemZoneName(sys, sizeof(sys));
            if (const char* known = KnownSystemName(sys)) strncpy_s(sys, known, _TRUNCATE);
        }
        AcquireSRWLockExclusive(&g_travelLock);
        strncpy_s(g_currentSystem, sys, _TRUNCATE);
        ReleaseSRWLockExclusive(&g_travelLock);
    }
    HandleRequest(now);
    StepScan(now);
}
