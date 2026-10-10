// npc: the NPCs tab as a built-in plugin, a pure consumer of sco-core's game.actors (sc_actors.h).
//
// npc.spawn spawns through game.actors' spawn_npc, in your zone a few metres in front of you (the
// pose comes from teleport.spatial, sc_spatial.h); the game pack owns the NPCs and despawns them
// when this plugin unloads or crashes. npc.clear despawns the ids this plugin spawned. Nothing here
// touches the game: the includes are the SDK headers (plus the host's built-in table type).
// The NPCs tab's draw code and npcs.txt list stay in npc_ui.cpp and npc.cpp; the tab calls these
// commands through sco_api's invoke.
#include "sco_api.h"
#include "sc_actors.h"
#include "sc_spatial.h"
#include "builtins.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>

// npc_ui.cpp: registers the NPCs tab (sco.ui) and keeps api/self for the tab's invoke calls.
void RegisterNpcsTab(const sco_api* api, sco_plugin* self);

extern const sco::plugins::Builtin kNpcBuiltin;

namespace {

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "npc", "1.0.0", "sc-offline",
};

constexpr int64_t kMaxCount = 10;   // the tab's limit
constexpr double  kAhead = 3.0;     // metres in front of you
constexpr double  kSpacing = 1.2;   // metres between NPCs spawned together
constexpr double  kLift = 0.2;      // metres above your feet

const sco_api*       g_api = nullptr;
sco_plugin*          g_self = nullptr;
const sc_actors_v1*  g_actors = nullptr;
const sc_spatial_v1* g_spatial = nullptr;

uint64_t g_owned[SC_ACTORS_MAX_NPCS];   // the NPCs this plugin spawned and hasn't despawned
uint32_t g_ownedCount = 0;

// The tables are host-owned and stay valid while this plugin is loaded; they are looked up again
// when missing so a command still works if the game pack published after this plugin loaded.
bool OpenServices() {
    if (!g_api || g_api->size <= offsetof(sco_api, query_service)) return false;
    const void* t = nullptr;
    if (!g_actors && g_api->query_service(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, &t) == SCO_OK)
        g_actors = static_cast<const sc_actors_v1*>(t);
    t = nullptr;
    if (!g_spatial && g_api->query_service(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION, &t) == SCO_OK)
        g_spatial = static_cast<const sc_spatial_v1*>(t);
    return g_actors && g_spatial;
}

void LastError(char* out, uint32_t cap) {
    out[0] = 0;
    if (!g_actors || g_actors->size <= offsetof(sc_actors_v1, last_error)) return;
    uint32_t n = cap;
    if (g_actors->last_error(g_self, out, &n) != SCO_OK) out[0] = 0;
}

sco_result Spawn(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const char* npc = args[0].v.s;
    const int64_t count = args[1].v.i;
    if (!npc || !*npc) {
        snprintf(reply, size, "Name an NPC archetype from npcs.txt");
        return SCO_BAD_ARG;
    }
    if (count < 1 || count > kMaxCount) {
        snprintf(reply, size, "Count must be 1 to %d", static_cast<int>(kMaxCount));
        return SCO_BAD_ARG;
    }
    if (!OpenServices() || g_actors->size <= offsetof(sc_actors_v1, spawn_npc)) {
        snprintf(reply, size, "game.actors or teleport.spatial isn't published by the game pack");
        return SCO_UNAVAILABLE;
    }
    double you[3], q[4];
    uint64_t zone = 0;
    if (!g_spatial->player_pose(you, q, &zone) || !zone) {
        snprintf(reply, size, "You haven't spawned yet");
        return SCO_FAILED;
    }
    // Your axes in the zone's frame from your rotation quaternion (x, y, z, w): +X right, +Y
    // forward, +Z up, as build mode's PlaceNearPlayer reads it.
    const double right[3] = { 1 - 2 * (q[1] * q[1] + q[2] * q[2]), 2 * (q[0] * q[1] + q[3] * q[2]), 2 * (q[0] * q[2] - q[3] * q[1]) };
    const double fwd[3]   = { 2 * (q[0] * q[1] - q[3] * q[2]), 1 - 2 * (q[0] * q[0] + q[2] * q[2]), 2 * (q[1] * q[2] + q[3] * q[0]) };
    const double up[3]    = { 2 * (q[0] * q[2] + q[3] * q[1]), 2 * (q[1] * q[2] - q[3] * q[0]), 1 - 2 * (q[0] * q[0] + q[1] * q[1]) };

    int spawned = 0;
    sco_result failed = SCO_OK;
    char why[160] = "";
    for (int i = 0; i < count; ++i) {
        if (g_ownedCount >= SC_ACTORS_MAX_NPCS) { failed = SCO_TOO_MANY; snprintf(why, sizeof(why), "%u NPCs already spawned", g_ownedCount); break; }
        const double side = (i - (count - 1) * 0.5) * kSpacing;
        double pos[3];
        for (int k = 0; k < 3; ++k) pos[k] = you[k] + fwd[k] * kAhead + right[k] * side + up[k] * kLift;
        uint64_t id = 0;
        failed = g_actors->spawn_npc(g_self, npc, zone, pos, &id);
        if (failed != SCO_OK) { LastError(why, sizeof(why)); break; }
        g_owned[g_ownedCount++] = id;
        ++spawned;
    }
    if (failed == SCO_OK) {
        snprintf(reply, size, "Spawning %d x %s in front of you", spawned, npc);
        return SCO_OK;
    }
    snprintf(reply, size, "Spawned %d of %d x %s: %s", spawned, static_cast<int>(count), npc, why[0] ? why : "refused");
    return spawned ? SCO_OK : failed;
}

sco_result Clear(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    if (!OpenServices() || g_actors->size <= offsetof(sc_actors_v1, despawn)) {
        snprintf(reply, size, "game.actors isn't published by the game pack");
        return SCO_UNAVAILABLE;
    }
    uint32_t removed = 0, kept = 0;
    char why[160] = "";
    for (uint32_t i = 0; i < g_ownedCount; ++i) {
        const sco_result r = g_actors->despawn(g_self, g_owned[i]);
        if (r == SCO_OK) { ++removed; continue; }
        if (r == SCO_NOT_FOUND) continue;   // gone already (the host took it, or the game did)
        if (!why[0]) LastError(why, sizeof(why));
        g_owned[kept++] = g_owned[i];       // refused: still ours, try again next time
    }
    g_ownedCount = kept;
    if (kept) {
        snprintf(reply, size, "Removing %u NPCs; %u refused: %s", removed, kept, why[0] ? why : "refused");
        return removed ? SCO_OK : SCO_FAILED;
    }
    snprintf(reply, size, "Removing the NPCs you spawned (%u)", removed);
    return SCO_OK;
}

const sco_plugin_info* NpcQuery() { return &kInfo; }

sco_result NpcLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    g_ownedCount = 0;
    OpenServices();   // a miss is retried when a command runs
    const sco_arg_def spawn[2] = {
        { "npc", SCO_ARG_STRING, 0, "NPC archetype, as in npcs.txt" },
        { "count", SCO_ARG_INT, 0, "How many (1 to 10)" },
    };
    sco_command c = {};
    c.size = sizeof(c);
    c.name = "npc.spawn";
    c.title = "Spawn NPCs";
    c.help = "Spawns NPCs of an archetype in front of you through game.actors, as the NPCs tab does";
    c.capability = "game.actors.spawn_npc";
    c.args = spawn;
    c.nargs = 2;
    c.arg_def_size = sizeof(sco_arg_def);
    c.fn = Spawn;
    sco_result r = api->register_command(self, &c);
    if (r == SCO_OK) {
        sco_command d = {};
        d.size = sizeof(d);
        d.name = "npc.clear";
        d.title = "Clear NPCs";
        d.help = "Despawns every NPC this plugin spawned (the game pack also removes them when it unloads)";
        d.capability = "game.actors.despawn";
        d.fn = Clear;
        r = api->register_command(self, &d);
    }
    if (r != SCO_OK) return r;   // the host releases what was registered
    // Its page of the menu (and keys), through sco.ui; the menu shell draws it.
    RegisterNpcsTab(api, self);
    return SCO_OK;
}

// The host despawns what this plugin spawned; just forget the ids and the tables.
void NpcUnload() {
    g_ownedCount = 0;
    g_actors = nullptr;
    g_spatial = nullptr;
    g_api = nullptr;
    g_self = nullptr;
}

}  // namespace

const sco::plugins::Builtin kNpcBuiltin = { "npc", NpcQuery, NpcLoad, NpcUnload };
