// spawn: sc-offline's second built-in plugin (sco-core's docs/framework.md, Phase 4, step 2).
//
// The sco_api surface over spawner.cpp: the spawn.ship command, the spawn.entities service
// (sc_spawn.h, shipped in sco-core's SDK) for other plugins, and the spawner's tick, run from a
// "tick" subscription so a fault in it disables this plugin instead of the game. The mechanics stay
// in spawner.cpp; the menu still calls them directly (its tabs move onto commands with the menu,
// Phase 4 step 4).
//
// The service's mover (1.2, set_entity_transform) moves only what the caller may move: the ids it
// spawned through spawn_as (recorded here per plugin handle and forgotten when the runtime releases
// that plugin), or a player vehicle sc-offline registered (mover.h). Frames convert through the
// teleport built-in's zone tree; the move itself is build.cpp's MoveEntityLocal.
#include "builtins.h"
#include "tabs.h"
#include "mover.h"
#include <sc_spawn.h>
#include "../build.h"
#include "../spawner.h"
#include "../version.h"
#include "sco/runtime.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

const sco_api* g_api = nullptr;    // from SpawnLoad until SpawnUnload
sco_plugin*    g_self = nullptr;
bool           g_ticking = false;  // the tick subscription owns ProcessShipMenu
bool           g_releaseHook = false;  // OnRelease is added (sco::AddReleaseHook)

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "spawn", SCO_VERSION, "sc-offline",
};

constexpr double kMaxHeight = 10000.0;   // metres above you
constexpr size_t kMaxClassLen = 63;      // spawner.cpp's request buffer is 64 bytes

// Game reads under SEH. No C++ object with a destructor lives in these frames (MSVC C2712).
bool ClassExists(const char* cls) {
    bool found = false;
    __try { found = EntityClassExists(cls); } __except (EXCEPTION_EXECUTE_HANDLER) { found = false; }
    return found;
}
uint64_t LocalPlayer() {
    uint64_t id = 0;
    __try { id = LocalPlayerEntityId(); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    return id;
}
uint64_t PlayerShip() {
    uint64_t id = 0;
    __try { id = PlayerShipId(); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    return id;
}

// ---- spawn.entities (sc_spawn.h) --------------------------------------------------------------

// Who may move what (game thread only, like every service call and the runtime's Release).
struct Owned { const void* owner; uint64_t id; };
std::vector<Owned>    g_owned;            // spawns made through spawn_as, by plugin handle
std::vector<uint64_t> g_playerVehicles;   // RegisterPlayerVehicle (ASOP)
constexpr size_t kMaxOwned = 4096;        // past it, ids that no longer resolve are dropped first

void ForgetOwner(const void* owner) {
    g_owned.erase(std::remove_if(g_owned.begin(), g_owned.end(), [owner](const Owned& o) { return o.owner == owner; }),
                  g_owned.end());
}

bool Remember(const void* owner, uint64_t id) {
    if (g_owned.size() >= kMaxOwned)
        g_owned.erase(std::remove_if(g_owned.begin(), g_owned.end(), [](const Owned& o) { return !EntityAlive(o.id); }),
                      g_owned.end());
    if (g_owned.size() >= kMaxOwned) return false;
    g_owned.push_back({ owner, id });
    return true;
}

bool MayMove(const void* owner, uint64_t id) {
    if (g_releaseHook)
        for (const Owned& o : g_owned)
            if (o.owner == owner && o.id == id) return true;
    return std::find(g_playerVehicles.begin(), g_playerVehicles.end(), id) != g_playerVehicles.end();
}

// A plugin unloaded or crashed (sco::AddReleaseHook): its spawns are nobody's now, so a later
// plugin given the same handle can't move them.
void OnRelease(const void* owner) { ForgetOwner(owner); }

const char* SvcSpawnNearPlayer(const char* cls, const double offset[3], uint64_t* outId) {
    if (outId) *outId = 0;
    if (!sco::OnGameThread()) return "game thread only";
    if (!SpawnerReady()) return "the spawner isn't available on this game build";
    if (!cls || !*cls || !offset || !outId) return "bad argument";
    uint64_t id = 0;
    const char* err = SpawnEntityNearPlayer(cls, offset, id);   // guards the game calls itself
    if (!err) *outId = id;
    return err;
}

const char* SvcSpawnAs(sco_plugin* self, const char* cls, const double offset[3], uint64_t* outId) {
    if (outId) *outId = 0;
    if (!sco::OnGameThread()) return "game thread only";
    if (!self) return "bad argument";
    const char* err = SvcSpawnNearPlayer(cls, offset, outId);
    if (!err && !Remember(self, *outId)) return "spawned, but too many entities are recorded to move it";
    return err;
}

// Why set_entity_transform answered 0, logged once per id and reason so plugin authors can tell
// the 0s apart without mod.log filling up when a plugin retries every tick.
enum class Refusal : uint8_t { BadArgument, NotYours, NotStreamedIn, NoZone, ZoneConversion, MoveFailed };
struct Logged { uint64_t id; Refusal why; };
std::vector<Logged> g_logged;
constexpr size_t kMaxLogged = 1024;   // forgotten all at once past it: at worst a reason is logged again

int Refuse(uint64_t id, Refusal why) {
    for (const Logged& l : g_logged)
        if (l.id == id && l.why == why) return 0;
    if (g_logged.size() >= kMaxLogged) g_logged.clear();
    g_logged.push_back({ id, why });
    static const char* const kWhy[] = {
        "bad argument (null self, id 0, or a position or rotation that isn't finite or a zero rotation)",
        "not yours (move only entities you spawned with spawn_as)",
        "not streamed in yet (a fresh spawn takes seconds; wait for entity_alive)",
        "zone conversion failed (the entity isn't in a zone yet)",
        "zone conversion failed (the target zone or the entity's zone can't be placed)",
        "the game refused the move (entity move slots don't match this build)",
    };
    char msg[192];
    snprintf(msg, sizeof(msg), "set_entity_transform(%llu) -> 0: %s", static_cast<unsigned long long>(id),
             kWhy[static_cast<int>(why)]);
    if (g_api) g_api->log(g_self, SCO_LOG_WARN, msg);
    return 0;
}

int SvcSetEntityTransform(sco_plugin* self, uint64_t id, uint64_t zoneId, const double pos[3], const double rot[4]) {
    if (!sco::OnGameThread() || !SpawnerReady()) return 0;   // the documented no-ops: nothing to explain per id
    if (!self || !id || !pos || !rot) return Refuse(id, Refusal::BadArgument);
    if (!MayMove(self, id)) return Refuse(id, Refusal::NotYours);
    double q[4], n = 0;
    for (int i = 0; i < 4; ++i) n += rot[i] * rot[i];
    n = std::sqrt(n);
    if (!std::isfinite(n) || n < 1e-9) return Refuse(id, Refusal::BadArgument);
    for (int i = 0; i < 4; ++i) q[i] = rot[i] / n;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i])) return Refuse(id, Refusal::BadArgument);
    if (!EntityAlive(id)) return Refuse(id, Refusal::NotStreamedIn);
    const uint64_t in = TeleportZoneOfEntity(id);   // the zone the entity is in: MoveEntityLocal's frame
    if (!in) return Refuse(id, Refusal::NoZone);
    double localPos[3], localRot[4];
    if (!TeleportPoseToZone(zoneId, in, pos, q, localPos, localRot)) return Refuse(id, Refusal::ZoneConversion);
    return MoveEntityLocal(id, localPos, localRot) ? 1 : Refuse(id, Refusal::MoveFailed);
}
int SvcClassExists(const char* cls) {
    return sco::OnGameThread() && SpawnerReady() && cls && *cls && ClassExists(cls) ? 1 : 0;
}
uint64_t SvcLocalPlayerId() { return sco::OnGameThread() && SpawnerReady() ? LocalPlayer() : 0; }
uint64_t SvcPlayerShipId() { return sco::OnGameThread() && SpawnerReady() ? PlayerShip() : 0; }
int SvcEntityAlive(uint64_t id) { return sco::OnGameThread() && SpawnerReady() && EntityAlive(id) ? 1 : 0; }

const sc_spawn_service_v1 kService = {
    sizeof(sc_spawn_service_v1), SvcSpawnNearPlayer, SvcClassExists, SvcLocalPlayerId, SvcPlayerShipId, SvcEntityAlive,
    SvcSetEntityTransform,       SvcSpawnAs,
};

// ---- spawn.ship -------------------------------------------------------------------------------

// Queues the spawn the menu's "spawn class" row makes (spawner.cpp runs it on the next tick and
// puts the result on the status strip): the ship appears <height> m above you and becomes the
// Crew & seats target.
sco_result Ship(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const char* cls = args[0].v.s;
    const double height = args[1].v.f;
    if (!cls || !*cls || strlen(cls) > kMaxClassLen) {
        snprintf(reply, size, "Name a ship class, at most %u characters", static_cast<unsigned>(kMaxClassLen));
        return SCO_BAD_ARG;
    }
    if (!(height >= 0.0 && height <= kMaxHeight)) {
        snprintf(reply, size, "Height must be 0 to %.0f m", kMaxHeight);
        return SCO_BAD_ARG;
    }
    if (!ClassExists(cls)) {
        snprintf(reply, size, "'%s' isn't a spawnable class on this game build", cls);
        return SCO_FAILED;
    }
    Menu_RequestSpawnClass(cls, static_cast<float>(height), false, false, false);
    snprintf(reply, size, "Spawning %s %.0f m above you", cls, height);
    return SCO_OK;
}

void OnTick(const char*, const void* data, void*) {
    if (data) ProcessShipMenu(*static_cast<const uint32_t*>(data));   // checks SpawnerReady itself
}

const sco_plugin_info* SpawnQuery() { return &kInfo; }

sco_result SpawnLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    sco_arg_def args[2] = {};
    args[0].name = "class";
    args[0].type = SCO_ARG_STRING;
    args[0].help = "Entity class, as in ships.txt (DRAK_Cutlass_Black)";
    args[1].name = "height";
    args[1].type = SCO_ARG_FLOAT;
    args[1].help = "Metres above you (0 to 10000)";
    sco_command c = {};
    c.size = sizeof(c);
    c.name = "spawn.ship";
    c.title = "Spawn ship";
    c.help = "Spawns a ship above you, as the Vehicles tab does; the status strip says when it's there";
    c.capability = "spawn.ship";
    c.args = args;
    c.nargs = 2;
    c.arg_def_size = sizeof(sco_arg_def);
    c.fn = Ship;
    sco_result r = api->register_command(self, &c);
    if (r == SCO_OK) r = api->provide_service(self, SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION, &kService);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) { g_api = nullptr; g_self = nullptr; return r; }   // the host releases what was registered
    // Without the hook a reloaded plugin could inherit an unloaded one's spawns, so the mover
    // refuses everything spawned through spawn_as then (spawn_as still spawns).
    g_releaseHook = sco::AddReleaseHook(OnRelease) == sco::Result::Ok;
    if (!g_releaseHook) api->log(self, SCO_LOG_WARN, "no release hook left: set_entity_transform moves no spawn_as entity");
    // Its page of the menu (and keys), through sco.ui; the menu shell draws it (tabs.h).
    RegisterBuiltinTab(api, self, "spawn.vehicles", "Vehicles", kTabVehicles, DrawVehiclesTab);
    g_ticking = true;
    return SCO_OK;
}

// A crash leaves g_ticking set: the host never calls a crashed plugin again, and dllmain doesn't
// take ProcessShipMenu back, so a spawner that faulted stays off instead of faulting the game.
void SpawnUnload() {
    if (g_releaseHook) sco::RemoveReleaseHook(OnRelease);
    g_releaseHook = false;
    g_owned.clear();
    g_logged.clear();
    g_ticking = false;
    g_api = nullptr;
    g_self = nullptr;
}

}  // namespace

bool SpawnBuiltinOwnsTick() { return g_ticking; }

void RegisterPlayerVehicle(uint64_t entityId) {
    if (entityId && std::find(g_playerVehicles.begin(), g_playerVehicles.end(), entityId) == g_playerVehicles.end())
        g_playerVehicles.push_back(entityId);
}

void UnregisterPlayerVehicle(uint64_t entityId) {
    g_playerVehicles.erase(std::remove(g_playerVehicles.begin(), g_playerVehicles.end(), entityId), g_playerVehicles.end());
}

const sco::plugins::Builtin kSpawnBuiltin = { "spawn", SpawnQuery, SpawnLoad, SpawnUnload };
