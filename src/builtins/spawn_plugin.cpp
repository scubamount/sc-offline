// spawn: sc-offline's second built-in plugin (sco-core's docs/framework.md, Phase 4, step 2).
//
// The sco_api surface over spawner.cpp: the spawn.ship command, the spawn.entities service
// (spawn_service.h) for other plugins, and the spawner's tick, run from a "tick" subscription so a
// fault in it disables this plugin instead of the game. The mechanics stay in spawner.cpp; the
// menu still calls them directly (its tabs move onto commands with the menu, Phase 4 step 4).
#include "builtins.h"
#include "spawn_service.h"
#include "../spawner.h"
#include "../version.h"
#include "sco/runtime.h"
#include <cstdio>
#include <cstring>

namespace {

const sco_api* g_api = nullptr;    // from SpawnLoad until SpawnUnload
sco_plugin*    g_self = nullptr;
bool           g_ticking = false;  // the tick subscription owns ProcessShipMenu

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

// ---- spawn.entities (spawn_service.h) ---------------------------------------------------------

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
int SvcClassExists(const char* cls) {
    return sco::OnGameThread() && SpawnerReady() && cls && *cls && ClassExists(cls) ? 1 : 0;
}
uint64_t SvcLocalPlayerId() { return sco::OnGameThread() && SpawnerReady() ? LocalPlayer() : 0; }
uint64_t SvcPlayerShipId() { return sco::OnGameThread() && SpawnerReady() ? PlayerShip() : 0; }
int SvcEntityAlive(uint64_t id) { return sco::OnGameThread() && SpawnerReady() && EntityAlive(id) ? 1 : 0; }

const sc_spawn_service_v1 kService = {
    sizeof(sc_spawn_service_v1), SvcSpawnNearPlayer, SvcClassExists, SvcLocalPlayerId, SvcPlayerShipId, SvcEntityAlive,
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
    g_ticking = true;
    return SCO_OK;
}

// A crash leaves g_ticking set: the host never calls a crashed plugin again, and dllmain doesn't
// take ProcessShipMenu back, so a spawner that faulted stays off instead of faulting the game.
void SpawnUnload() {
    g_ticking = false;
    g_api = nullptr;
    g_self = nullptr;
}

}  // namespace

bool SpawnBuiltinOwnsTick() { return g_ticking; }

const sco::plugins::Builtin kSpawnBuiltin = { "spawn", SpawnQuery, SpawnLoad, SpawnUnload };
