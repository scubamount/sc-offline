// teleport: sc-offline's first built-in plugin (sco-core's docs/framework.md, Phase 4).
//
// The sco_api surface over teleport.cpp: two commands, gated on the "teleport" capability
// (SetFeatureCaps in dllmain.cpp), that any plugin reaches through invoke; the built-in binds F7
// and F8 to them through sco.ui, and the product dispatches those keys (hotkeys.cpp). The
// mechanics stay in teleport.cpp. Commands run on the game thread, where F7 and F8 always ran.
//
// It also publishes the teleport.spatial service (sc_spatial.h): a tick subscription feeds a
// sco::engine::ZoneTree with your zone chain, read with teleport.cpp's zone readers, and the
// service's conversions read through that tree. The tree holds ids and transforms only, never a
// game pointer, and is rebuilt every tick; a zone queried that isn't in it yet is read on the spot.
//
// A built-in is a table entry (builtins.h), not a DLL export: these functions are file-local, so
// they can't clash with sco_api.h's sco_plugin_* declarations and the DLL still exports only
// DirectInput8Create.
#include "builtins.h"
#include "builtin_store.h"
#include <sc_spatial.h>
#include "tabs.h"
#include "../build.h"
#include "../hotkeys.h"
#include "../teleport.h"
#include "../version.h"
#include "sco/engine/zone.h"
#include "sco/runtime.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const sco_api* g_api = nullptr;    // from TeleportLoad until TeleportUnload
sco_plugin*    g_self = nullptr;

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "teleport", SCO_VERSION, "sc-offline",
};

// A spot that can't be saved or reached answers SCO_UNAVAILABLE; the reply says why. From a hotkey,
// mod.log names the key ("F7"), as before.
sco_result Save(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    const char* key = Hotkeys_Current();
    return SaveSpotHere(key ? key : "teleport.save", reply, size) ? SCO_OK : SCO_UNAVAILABLE;
}

sco_result Go(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    const char* key = Hotkeys_Current();
    return GoToSavedSpot(key ? key : "teleport.go", reply, size) ? SCO_OK : SCO_UNAVAILABLE;
}

sco_result Register(const char* name, const char* title, const char* help, sco_command_fn fn) {
    sco_command c = {};
    c.size = sizeof(c);
    c.name = name;
    c.title = title;
    c.help = help;
    c.capability = "teleport";
    c.arg_def_size = sizeof(sco_arg_def);
    c.fn = fn;
    return g_api->register_command(g_self, &c);
}

// ---- teleport.spatial (sc_spatial.h) ----------------------------------------------------------

using sco::engine::Quatd;
using sco::engine::Transform;
using sco::engine::Vector3d;

sco::engine::ZoneTree g_zones;    // cleared and refilled every tick; ids and transforms only
constexpr int kChainMax = 16;     // zones per chain; the game's run about 6 deep

// Game reads under SEH, into plain structs. No C++ object with a destructor lives in these frames
// (MSVC C2712); the tree is fed outside them.
int ChainOfZoneId(uint64_t zoneId, ZoneFrame* out) {
    __try {
        const uintptr_t zone = ZoneFromId(zoneId);
        return zone ? ReadZoneChain(zone, out, kChainMax) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int ChainOfPlayer(ZoneFrame* out) {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return 0;
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        return zone ? ReadZoneChain(zone, out, kChainMax) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct Pose { double pos[3]; double rot[4]; uint64_t zone; };

bool ReadPlayerPose(Pose& p) {
    __try {
        uintptr_t actor, entity;
        if (!GetLocalPlayer(actor, entity)) return false;
        const uintptr_t zone = VCall<uintptr_t>(entity, 0x6B8);
        if (!zone) return false;
        p.zone = ZoneId(zone);
        if (!p.zone || ZoneFromId(p.zone) != zone) return false;
        Vec3Out(entity, 0x2B8, p.pos);
        return EntityRotation(entity, p.rot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t ZoneIdOfEntity(uint64_t entityId) {
    __try {
        const uintptr_t es = *g_tp.entitySystem;
        const uintptr_t ent = es && entityId ? VCall<uintptr_t>(es, 0x120, entityId) : 0;
        const uintptr_t zone = ent ? VCall<uintptr_t>(ent, 0x6B8) : 0;
        if (!zone) return 0;
        const uint64_t id = ZoneId(zone);
        return id && ZoneFromId(id) == zone ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The rotation whose matrix has columns a[0], a[1], a[2] (the world directions of a zone's axes).
Quatd FromAxes(const double a[3][3]) {
    const double m00 = a[0][0], m11 = a[1][1], m22 = a[2][2];
    const double m01 = a[1][0], m10 = a[0][1], m02 = a[2][0], m20 = a[0][2], m12 = a[2][1], m21 = a[1][2];
    const double t = m00 + m11 + m22;
    Quatd q;
    if (t > 0) {
        const double s = std::sqrt(t + 1.0) * 2.0;
        q = { 0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s };
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q = { (m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s };
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q = { (m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s };
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q = { (m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s };
    }
    return q.Normalized();
}

bool RightHanded(const double a[3][3]) {
    const Vector3d x{ a[0][0], a[0][1], a[0][2] }, y{ a[1][0], a[1][1], a[1][2] }, z{ a[2][0], a[2][1], a[2][2] };
    return x.Cross(y).Dot(z) > 0;
}

Transform WorldFrame(const ZoneFrame& f) {
    return { { f.origin[0], f.origin[1], f.origin[2] }, FromAxes(f.axis), 1.0 };
}

// Puts a chain (innermost first) into the tree, each zone relative to the next one up; the
// outermost is a root zone with its world frame. A mirrored frame (no rotation can express it)
// cuts the chain below it: the zones inside it still go in, the outermost of them as a root.
void Feed(const ZoneFrame* f, int n) {
    for (int i = 0; i < n; ++i)
        if (!RightHanded(f[i].axis)) { n = i; break; }
    for (int i = 0; i < n; ++i) {
        const Transform world = WorldFrame(f[i]);
        if (i + 1 < n) {
            const Transform parent = WorldFrame(f[i + 1]);
            const Transform local = { parent.InverseTransformPoint(world.position),
                                      (parent.rotation.Conjugate() * world.rotation).Normalized(), 1.0 };
            g_zones.Set(f[i].id, f[i + 1].id, f[i].name, local);
        } else {
            g_zones.Set(f[i].id, 0, f[i].name, world);
        }
    }
}

void RefreshZones() {
    g_zones.Clear();
    if (!g_tp.ok) return;
    ZoneFrame chain[kChainMax];
    Feed(chain, ChainOfPlayer(chain));
}

bool Ready() { return g_tp.ok && sco::OnGameThread(); }

// The zone is in the tree, read this tick: the player's chain from the tick, or its own chain read
// now. 0, the world, always is.
bool EnsureZone(uint64_t zoneId) {
    if (!zoneId || g_zones.Has(zoneId)) return true;
    ZoneFrame chain[kChainMax];
    Feed(chain, ChainOfZoneId(zoneId, chain));
    return g_zones.Has(zoneId);
}

Vector3d V(const double p[3]) { return { p[0], p[1], p[2] }; }
void Out(const Vector3d& v, double p[3]) { p[0] = v.x; p[1] = v.y; p[2] = v.z; }

int SvcPlayerPose(double pos[3], double rot[4], uint64_t* zoneId) {
    if (!Ready() || !pos || !rot || !zoneId) return 0;
    Pose p = {};
    if (!ReadPlayerPose(p)) return 0;
    memcpy(pos, p.pos, sizeof(p.pos));
    memcpy(rot, p.rot, sizeof(p.rot));
    *zoneId = p.zone;
    return 1;
}

int SvcZoneOfEntity(uint64_t entityId, uint64_t* zoneId) {
    if (!Ready() || !zoneId) return 0;
    const uint64_t id = ZoneIdOfEntity(entityId);
    if (!id || !EnsureZone(id)) return 0;
    *zoneId = id;
    return 1;
}

int SvcLocalToWorld(uint64_t zoneId, const double local[3], double world[3]) {
    if (!Ready() || !local || !world || !EnsureZone(zoneId)) return 0;
    Vector3d w;
    if (!g_zones.LocalToWorld(zoneId, V(local), &w)) return 0;
    Out(w, world);
    return 1;
}

int SvcWorldToLocal(uint64_t zoneId, const double world[3], double local[3]) {
    if (!Ready() || !world || !local || !EnsureZone(zoneId)) return 0;
    Vector3d l;
    if (!g_zones.WorldToLocal(zoneId, V(world), &l)) return 0;
    Out(l, local);
    return 1;
}

int SvcZoneToZone(uint64_t from, uint64_t to, const double in[3], double out[3]) {
    if (!Ready() || !in || !out || !EnsureZone(from) || !EnsureZone(to)) return 0;
    Vector3d p;
    if (!g_zones.Transform(from, to, V(in), &p)) return 0;
    Out(p, out);
    return 1;
}

int SvcZoneName(uint64_t zoneId, char* out, uint32_t cap) {
    if (!Ready() || !out || !cap || !zoneId || !EnsureZone(zoneId)) return 0;
    sco::engine::Zone z;
    if (!g_zones.Find(zoneId, &z)) return 0;
    const size_t n = z.name.size() < cap - 1 ? z.name.size() : cap - 1;
    memcpy(out, z.name.data(), n);
    out[n] = 0;
    return 1;
}

const sc_spatial_v1 kSpatial = {
    sizeof(sc_spatial_v1), SvcPlayerPose, SvcZoneOfEntity, SvcLocalToWorld, SvcWorldToLocal, SvcZoneToZone, SvcZoneName,
};

void OnTick(const char*, const void*, void*) { RefreshZones(); }

// ---- load / unload ----------------------------------------------------------------------------

const sco_plugin_info* TeleportQuery() { return &kInfo; }

sco_result TeleportLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    g_teleportStore.Open(api, self);   // data/storage/teleport.db: the saved spot (teleport.cpp)
    sco_result r = Register("teleport.save", "Save spot", "Save where you're standing (F7)", Save);
    if (r == SCO_OK) r = Register("teleport.go", "Go to saved spot", "Teleport to the saved spot (F8)", Go);
    if (r == SCO_OK) r = api->provide_service(self, SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION, &kSpatial);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) { g_api = nullptr; g_self = nullptr; g_teleportStore.Close(); return r; }   // the host releases what was registered
    BindBuiltinHotkey(api, self, "f7", "teleport.save");
    BindBuiltinHotkey(api, self, "f8", "teleport.go");
    return SCO_OK;
}

void TeleportUnload() {
    g_zones.Clear();
    g_teleportStore.Close();
    g_api = nullptr;
    g_self = nullptr;
}

}  // namespace

const sco::plugins::Builtin kTeleportBuiltin = { "teleport", TeleportQuery, TeleportLoad, TeleportUnload };
