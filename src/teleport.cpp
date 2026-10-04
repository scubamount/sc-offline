#include "teleport.h"
#include <cmath>
#include <share.h>

TeleportApi g_tp;

static bool VerifyEntityPositionSlots(const Section& text, const Section& rdata) {
    const uint8_t* fmt = FindCString(rdata, "Zone: %s, ZonePos:(%f.2,%f.2,%f.2), WorldPos((%f.2,%f.2,%f.2)");
    const uint8_t* lea = fmt ? FindRipLea(text, 0x4C, 0x8D, 0x05, fmt) : nullptr;
    if (!lea) return false;
    static const uint8_t callWorld[] = { 0xFF, 0x90, 0x18, 0x03, 0x00, 0x00 };
    static const uint8_t callLocal[] = { 0xFF, 0x90, 0xB8, 0x02, 0x00, 0x00 };
    static const uint8_t callZone[]  = { 0xFF, 0x90, 0xB8, 0x06, 0x00, 0x00 };
    static const uint8_t zoneName[]  = { 0x48, 0x8B, 0x91, 0x18, 0x02, 0x00, 0x00 };
    static const struct { ptrdiff_t off; const uint8_t* bytes; size_t n; } kChecks[] = {
        { -0xFA, callWorld, 6 }, { -0xD3, callWorld, 6 }, { -0xAC, callWorld, 6 },
        { -0x86, callLocal, 6 }, { -0x5F, callLocal, 6 }, { -0x38, callLocal, 6 },
        { -0x19, callZone, 6 },  { -0x10, zoneName, 7 },
    };
    for (const auto& c : kChecks)
        if (memcmp(lea + c.off, c.bytes, c.n) != 0) return false;
    return true;
}

static bool VerifyZoneSlots(const Section& text, const Section& rdata) {
    const uint8_t* fmt = FindCString(rdata,
        "Changing reference point to unstreamable parent zone: new zone: %s (%llu) old zone: %s (%llu)");
    const uint8_t* lea = fmt ? FindRipLea(text, 0x48, 0x8D, 0x15, fmt) : nullptr;
    return lea && BytesMatch(lea - 0x1D4, "FF 50 60") && BytesMatch(lea - 0x1B7, "FF 50 08")
        && BytesMatch(lea - 0x1AB, "FF 50 08") && BytesMatch(lea - 0x2F, "FF 50 58");
}

bool ResolveTeleportApi(const Section& text, const Section& rdata) {
    const uint8_t* name = FindCString(rdata, "CmdTeleportToCamera");
    uint8_t* lea = name ? FindRipLea(text, 0x48, 0x8D, 0x15, name) : nullptr;
    if (!lea) return false;
    uint8_t* f = lea - 0x158;
    static const struct { size_t off; const char* bytes; } kChecks[] = {
        { 0x000, "40 55" },
        { 0x024, "48 8B 05 ?? ?? ?? ??" },
        { 0x02B, "48 8B 88 E0 00 00 00" },
        { 0x035, "FF 90 E0 02 00 00" },
        { 0x09F, "E8 ?? ?? ?? ??" },
        { 0x136, "FF 90 08 0A 00 00" },
        { 0x13F, "48 8B 51 28" },
        { 0x151, "48 8B 83 08 02 00 00" },
        { 0x17A, "C5 FA 10 B0 3C 6D 00 00" },
        { 0x182, "C5 FA 10 B8 30 6D 00 00" },
        { 0x1AC, "C5 78 10 90 18 6D 00 00" },
        { 0x1BD, "C5 7B 10 98 28 6D 00 00" },
        { 0x1FF, "FF 90 D8 06 00 00" },
        { 0x2B0, "48 8B 0D ?? ?? ?? ??" },
        { 0x2BA, "FF 90 20 01 00 00" },
        { 0x2C8, "48 8B 91 E0 06 00 00" },
        { 0x2E5, "4C 8B 89 98 01 00 00" },
        { 0x368, "FF 90 D8 09 00 00" },
        { 0x376, "4C 8B 81 58 01 00 00" },
    };
    for (const auto& c : kChecks)
        if (!BytesMatch(f + c.off, c.bytes)) { Log("[tp] TeleportToCamera layout changed at +0x%zx; teleport disabled", c.off); return false; }
    if (!VerifyEntityPositionSlots(text, rdata)) { Log("[tp] entity position slots not confirmed; teleport disabled"); return false; }
    if (!VerifyZoneSlots(text, rdata)) { Log("[tp] zone parent/id slots not confirmed; teleport disabled"); return false; }
    g_tp.clientMgr    = reinterpret_cast<uintptr_t*>(f + 0x024 + 7 + Rel32(f + 0x027));
    g_tp.handleFromId = f + 0x09F + 5 + Rel32(f + 0x0A0);
    g_tp.entitySystem = reinterpret_cast<uintptr_t*>(f + 0x2B0 + 7 + Rel32(f + 0x2B3));
    g_tp.ok = true;
    return true;
}

constexpr double kMaxLocalCoord = 1.0e12;
constexpr int    kMaxZoneDepth = 12;

struct ZoneSpot { char name[96]; double local[3]; };
struct Spot { int n = 0; ZoneSpot z[kMaxZoneDepth] = {}; };

static bool PositionLooksValid(const double p[3]) {
    for (int i = 0; i < 3; ++i)
        if (!(p[i] > -kMaxLocalCoord && p[i] < kMaxLocalCoord)) return false;
    return true;
}

bool GetLocalPlayer(uintptr_t& actor, uintptr_t& entity) {
    const uintptr_t mgr = *g_tp.clientMgr;
    if (!mgr) return false;
    const uintptr_t sub = Rd<uintptr_t>(mgr + 0xE0);
    if (!sub) return false;
    const uintptr_t info = VCall<uintptr_t>(sub, 0x2E0);
    if (!info) return false;
    uint64_t handle = 0;
    reinterpret_cast<void(__fastcall*)(uint64_t*, uint64_t)>(g_tp.handleFromId)(&handle, Rd<uint64_t>(info + 8));
    actor = handle & kPtrMask;
    if (!actor) return false;
    const uintptr_t life = VCall<uintptr_t>(actor, 0xA08);
    if (!life || !VCall<uintptr_t>(life, 0x28)) return false;
    entity = Rd<uintptr_t>(actor + 8) & kPtrMask;
    return entity != 0;
}

uintptr_t ZoneParent(uintptr_t zone) { return VCall<uintptr_t>(zone, 0x08); }
const char* ZoneName(uintptr_t zone) { return VCall<const char*>(zone, 0x218); }

uint64_t ZoneId(uintptr_t zone) {
    uint8_t tmp[16] = {};
    const uint64_t* id = VCall<const uint64_t*>(zone, 0x58, tmp);
    return id ? *id : 0;
}

uintptr_t ZoneFromId(uint64_t zoneId) {
    const uintptr_t es = *g_tp.entitySystem;
    if (!es || !zoneId) return 0;
    const uintptr_t ent = VCall<uintptr_t>(es, 0x120, zoneId);
    return ent ? VCall<uintptr_t>(ent, 0x6E0) : 0;
}

void Vec3Out(uintptr_t obj, size_t off, double out[3]) {
    double buf[4] = {};
    const double* r = VCall<const double*>(obj, off, buf, uintptr_t(0));
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

void LocalToWorld(uintptr_t zone, const double local[3], double world[3]) {
    double buf[4] = {};
    const double* r = VCall<const double*>(zone, 0x198, buf, local);
    world[0] = r[0]; world[1] = r[1]; world[2] = r[2];
}

static double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool WorldToLocal(uintptr_t zone, const double world[3], double local[3]) {
    constexpr double kArm = 1.0e6;
    const double zero[3] = {};
    double origin[3], axis[3][3];
    LocalToWorld(zone, zero, origin);
    for (int i = 0; i < 3; ++i) {
        double probe[3] = {}, w[3];
        probe[i] = kArm;
        LocalToWorld(zone, probe, w);
        for (int k = 0; k < 3; ++k) axis[i][k] = (w[k] - origin[k]) / kArm;
    }
    for (int i = 0; i < 3; ++i) {
        if (fabs(Dot(axis[i], axis[i]) - 1.0) > 1e-6) return false;
        for (int j = i + 1; j < 3; ++j)
            if (fabs(Dot(axis[i], axis[j])) > 1e-6) return false;
    }
    const double d[3] = { world[0] - origin[0], world[1] - origin[1], world[2] - origin[2] };
    for (int i = 0; i < 3; ++i) local[i] = Dot(axis[i], d);
    return PositionLooksValid(local);
}

static const char* CaptureSpot(Spot& s, double world[3]) {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return "player not spawned";
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return "not in a zone";
        double local[3];
        Vec3Out(entity, 0x2B8, local);
        if (!PositionLooksValid(local)) return "position out of range";
        LocalToWorld(zone, local, world);
        s.n = 0;
        for (uintptr_t z = zone; z && s.n < kMaxZoneDepth; z = ZoneParent(z)) {
            const char* name = ZoneName(z);
            if (!name || !*name) continue;
            ZoneSpot& e = s.z[s.n];
            if (z == zone) memcpy(e.local, local, sizeof(local));
            else if (!WorldToLocal(z, world, e.local)) continue;
            strncpy_s(e.name, name, _TRUNCATE);
            ++s.n;
        }
        return s.n ? nullptr : "zone has no name";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while reading player";
    }
}

static int FindSavedZone(const Spot& s, uintptr_t entity, uintptr_t& zoneOut) {
    uintptr_t chain[kMaxZoneDepth];
    const char* names[kMaxZoneDepth];
    int n = 0;
    for (uintptr_t z = VCall<uintptr_t>(entity, 0x6B8); z && n < kMaxZoneDepth; z = ZoneParent(z)) {
        chain[n] = z;
        names[n++] = ZoneName(z);
    }
    for (int i = 0; i < s.n; ++i)
        for (int k = 0; k < n; ++k)
            if (names[k] && strcmp(names[k], s.z[i].name) == 0) { zoneOut = chain[k]; return i; }
    return -1;
}

static int SavedZoneLevel(const Spot& s) {
    __try {
        uintptr_t actor, entity, zone;
        return GetLocalPlayer(actor, entity) ? FindSavedZone(s, entity, zone) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static const char* TeleportToSpot(const Spot& s, int& level) {
    if (!s.n) return "no saved spot";
    __try {
        uintptr_t actor, entity, zone = 0;
        if (!GetLocalPlayer(actor, entity)) return "player not spawned";
        level = FindSavedZone(s, entity, zone);
        if (level < 0) return "you're not in any of the saved spot's zones (different planet/system?)";
        const uint64_t zoneId = ZoneId(zone);
        if (!zoneId || ZoneFromId(zoneId) != zone) return "zone id lookup mismatch";
        const double* local = s.z[level].local;
        double world[3];
        LocalToWorld(zone, local, world);
        alignas(16) uint8_t params[0x80] = {};
        *reinterpret_cast<uint64_t*>(params + 0x00) = zoneId;
        reinterpret_cast<double*>(params + 0x08)[3] = 1.0;
        memcpy(params + 0x28, local, 3 * sizeof(double));
        *reinterpret_cast<double*>(params + 0x40) = 1.0;
        memcpy(params + 0x48, world, sizeof(world));
        reinterpret_cast<float*>(params + 0x60)[3] = 1.0f;
        params[0x7D] = level > 0;
        const uintptr_t comp = VCall<uintptr_t>(actor, 0x9D8);
        if (!comp) return "no teleport component";
        VCall<void>(comp, 0x158, params);
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while teleporting";
    }
}

const char* TeleportToEntity(uint64_t entityId, double up) {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return "player not spawned";
        const uintptr_t es = *g_tp.entitySystem;
        const uintptr_t target = es && entityId ? VCall<uintptr_t>(es, 0x120, entityId) : 0;
        if (!target) return "no such entity here (not streamed in?)";
        const uintptr_t zone = VCall<uintptr_t>(target, 0x6B8);
        if (!zone) return "the entity isn't in a zone";
        const uint64_t zoneId = ZoneId(zone);
        if (!zoneId || ZoneFromId(zoneId) != zone) return "zone id lookup mismatch";
        double local[3];
        Vec3Out(target, 0x2B8, local);
        const double r = sqrt(Dot(local, local));
        if (r > 100000.0) for (int i = 0; i < 3; ++i) local[i] += local[i] / r * up;
        else local[2] += up;
        if (!PositionLooksValid(local)) return "position out of range";
        double world[3];
        LocalToWorld(zone, local, world);
        alignas(16) uint8_t params[0x80] = {};
        *reinterpret_cast<uint64_t*>(params + 0x00) = zoneId;
        reinterpret_cast<double*>(params + 0x08)[3] = 1.0;
        memcpy(params + 0x28, local, 3 * sizeof(double));
        *reinterpret_cast<double*>(params + 0x40) = 1.0;
        memcpy(params + 0x48, world, sizeof(world));
        reinterpret_cast<float*>(params + 0x60)[3] = 1.0f;
        params[0x7D] = 1;
        const uintptr_t comp = VCall<uintptr_t>(actor, 0x9D8);
        if (!comp) return "no teleport component";
        VCall<void>(comp, 0x158, params);
        Log("[tp] to entity %llu in '%s' (%.0f, %.0f, %.0f)", static_cast<unsigned long long>(entityId), ZoneName(zone),
            local[0], local[1], local[2]);
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while teleporting";
    }
}

static bool SpotFilePath(char* path, DWORD n) {
    const DWORD len = GetEnvironmentVariableA("SC_OFFLINE_SPAWN_FILE", path, n);
    return len > 0 && len < n;
}

static bool LoadSpot(Spot& s, bool& oldFormat) {
    s.n = 0;
    oldFormat = false;
    char path[MAX_PATH];
    if (!SpotFilePath(path, sizeof(path))) return false;
    FILE* f = _fsopen(path, "r", _SH_DENYNO);
    if (!f) return false;
    char line[256];
    while (s.n < kMaxZoneDepth && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "zone=", 5) == 0) { oldFormat = true; continue; }
        ZoneSpot& e = s.z[s.n];
        if (sscanf_s(line, "spot %lf %lf %lf %95[^\r\n]", &e.local[0], &e.local[1], &e.local[2],
                     e.name, static_cast<unsigned>(sizeof(e.name))) == 4 && PositionLooksValid(e.local))
            ++s.n;
    }
    fclose(f);
    return s.n > 0;
}

static bool SaveSpot(const Spot& s) {
    char path[MAX_PATH];
    if (!SpotFilePath(path, sizeof(path))) return false;
    FILE* f = _fsopen(path, "w", _SH_DENYNO);
    if (!f) return false;
    fprintf(f, "# starcitzenofflinemods spawn spot (F7). Position in each zone, innermost first.\n");
    for (int i = 0; i < s.n; ++i)
        fprintf(f, "spot %.6f %.6f %.6f %s\n", s.z[i].local[0], s.z[i].local[1], s.z[i].local[2], s.z[i].name);
    fclose(f);
    return true;
}

static void DescribeChain(const Spot& s, char* out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < s.n; ++i) {
        if (i) strncat_s(out, n, " > ", _TRUNCATE);
        strncat_s(out, n, s.z[i].name, _TRUNCATE);
    }
}

static Spot  g_spot;
static bool  g_autoTeleportPending = false;
static DWORD g_playerReadySince = 0;
static bool  g_keyWasDown[2] = {};

static bool KeyPressed(int vk, bool& wasDown) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

static int   g_refineLevel = 0;
static DWORD g_refineSince = 0;

static void StartRefine(int level, DWORD now, const char* why) {
    if (level == 0) { Log("[tp] %s -> teleported to saved spot in '%s'", why, g_spot.z[0].name); return; }
    Log("[tp] %s -> teleported via '%s' (waiting for '%s' to stream in)", why, g_spot.z[level].name, g_spot.z[0].name);
    g_refineLevel = level;
    g_refineSince = now;
}

void LoadSavedSpot(bool startingOverDaymar) {
    bool oldFormat = false;
    if (LoadSpot(g_spot, oldFormat)) {
        g_autoTeleportPending = true;
        char chain[512];
        DescribeChain(g_spot, chain, sizeof(chain));
        Log("[tp] saved spot loaded ('%s'; zones: %s); will teleport there after spawning. F7 = save new spot, F8 = go there now",
            g_spot.z[0].name, chain);
    } else if (oldFormat) {
        Log("[tp] saved spot is from an older version and can't be used after a relaunch: stand there and press F7 once more");
    } else {
        Log("[tp] no saved spot yet: stand where you want to spawn and press F7 (F8 = go there now)");
    }
    if (startingOverDaymar && g_autoTeleportPending) {
        g_autoTeleportPending = false;
        Log("[tp] starting over Daymar instead of the saved spot (F8 still goes there)");
    }
}

void TeleportTick(DWORD now) {
    const bool focus = GameHasFocus();
    if (KeyPressed(VK_F7, g_keyWasDown[0]) && focus) {
        Spot s;
        double world[3] = {};
        if (const char* err = CaptureSpot(s, world)) Log("[tp] F7 save failed: %s", err);
        else if (!SaveSpot(s)) Log("[tp] F7: could not write the spawn file (SC_OFFLINE_SPAWN_FILE)");
        else {
            g_spot = s;
            char chain[512];
            DescribeChain(s, chain, sizeof(chain));
            Log("[tp] F7 saved spot: '%s' pos (%.2f, %.2f, %.2f) m; zones: %s", s.z[0].name,
                s.z[0].local[0], s.z[0].local[1], s.z[0].local[2], chain);
        }
    }
    if (KeyPressed(VK_F8, g_keyWasDown[1]) && focus) {
        int level = -1;
        if (!g_spot.n) Log("[tp] F8: no saved spot yet (stand somewhere and press F7)");
        else if (const char* err = TeleportToSpot(g_spot, level)) Log("[tp] F8 teleport failed: %s", err);
        else StartRefine(level, now, "F8");
    }

    if (g_autoTeleportPending) {
        uintptr_t actor, entity;
        bool ready = false;
        __try { ready = GetLocalPlayer(actor, entity); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!ready) { g_playerReadySince = 0; return; }
        if (!g_playerReadySince) g_playerReadySince = now;
        if (now - g_playerReadySince < 8000) return;
        static DWORD lastTry = 0;
        if (now - lastTry < 2000) return;
        lastTry = now;
        int level = -1;
        const char* err = TeleportToSpot(g_spot, level);
        if (!err) { g_autoTeleportPending = false; StartRefine(level, now, "spawned"); }
        else if (now - g_playerReadySince > 120000) { g_autoTeleportPending = false; Log("[tp] auto-teleport gave up: %s", err); }
    }

    if (g_refineLevel > 0) {
        static DWORD lastCheck = 0;
        if (now - lastCheck < 1000) return;
        lastCheck = now;
        const int level = SavedZoneLevel(g_spot);
        if (level >= 0 && level < g_refineLevel) {
            int got = -1;
            if (TeleportToSpot(g_spot, got)) { g_refineLevel = 0; return; }
            g_refineLevel = got;
            if (got == 0) Log("[tp] '%s' streamed in -> exact spot", g_spot.z[0].name);
        } else if (now - g_refineSince > 60000) {
            Log("[tp] '%s' did not stream in within 60 s; stayed in '%s' (F8 = retry)", g_spot.z[0].name,
                g_spot.z[g_refineLevel].name);
            g_refineLevel = 0;
        }
    }
}
