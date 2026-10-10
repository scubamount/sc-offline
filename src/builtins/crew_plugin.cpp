// crew: the Crew & seats commands as a built-in plugin, and a plain consumer of sco-core's game pack.
//
// Seats, seating, ejecting and Flight Ready come from game.vehicles (sc_vehicles.h); the NPCs come
// from game.actors (sc_actors.h) and are spawned next to you through teleport.spatial's pose
// (sc_spatial.h). Nothing here touches the game or spawner.cpp: this file includes only the SDK's
// headers (plus the built-ins' own builtins.h / tabs.h shell and the version stamp).
//
// The target ship is the ship you're aboard, or the one crew.target pinned. crew.fill takes only seats
// that are empty and usable (SC_SEAT_USABLE: the game's own seat picker accepts them) and says how
// many it skipped; the game pack's mod.log lines name each seat and whether it is interactable.
// The Crew tab (crew_ui.cpp) still runs on spawner.cpp's seat code; CrewTabTick keeps that going.
#include "builtins.h"
#include "tabs.h"
#include "../version.h"
#include "sco_api.h"
#include "sc_actors.h"
#include "sc_spatial.h"
#include "sc_vehicles.h"
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

bool g_ticking = false;   // the tick subscription owns the Crew tab's tick, too

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "crew", SCO_VERSION, "sc-offline",
};

constexpr uint32_t kMaxSeats = 128;
constexpr int kMaxJobs = 64;                 // NPCs (and your own seat request) in flight or kept as crew
constexpr int kMaxLinks = 5;                 // seat() calls that took, with the seat still not yours
constexpr uint32_t kFirstTryMs = 750;        // after the spawn: the NPC streams in
constexpr uint32_t kRetryMs = 1500;          // seat() takes effect within a second or two
constexpr uint32_t kGiveUpMs = 30000;
constexpr size_t kMaxNpcClass = 127;

const sco_api* g_api = nullptr;
sco_plugin* g_self = nullptr;
const sc_vehicles_v1* g_veh = nullptr;
const sc_actors_v1* g_act = nullptr;
const sc_spatial_v1* g_sp = nullptr;
uint64_t g_pinned = 0;                       // crew.target's ship (a session handle: never stored past unload)
uint32_t g_now = 0;                          // the last tick's time, ms

// One actor on its way into a seat, or kept as crew once it's there.
struct Job {
    bool used = false;
    bool linking = false;                    // still being seated
    bool player = false;                     // you, not an NPC: never despawned
    uint64_t actor = 0;
    uint64_t ship = 0;
    uint32_t seat = 0;
    uint32_t since = 0;
    uint32_t nextTry = 0;
    int links = 0;
    char seatName[SC_VEHICLE_SEAT_NAME_MAX] = "";
    char why[96] = "";                       // the last refusal from seat()
};
Job g_jobs[kMaxJobs];
sc_vehicle_seat g_seats[kMaxSeats];

void LogF(sco_log_level level, const char* fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_api->log(g_self, level, line);
}

template <class T>
bool Open(const char* name, uint32_t version, const T*& table) {
    if (table) return true;
    const void* p = nullptr;
    if (g_api->size > offsetof(sco_api, query_service) && g_api->query_service(name, version, &p) == SCO_OK && p)
        table = static_cast<const T*>(p);
    return table != nullptr;
}

// game.vehicles, which every command needs, or a reply saying it isn't there.
bool Services(char* reply, uint32_t size) {
    if (!Open(SC_VEHICLES_SERVICE_NAME, SC_VEHICLES_SERVICE_VERSION, g_veh)) {
        snprintf(reply, size, "game.vehicles isn't published (the game pack is off or too old)");
        return false;
    }
    return true;
}

void VehicleWhy(char* out, size_t n) {
    out[0] = 0;
    uint32_t size = static_cast<uint32_t>(n);
    if (g_veh->last_error(g_self, out, &size) != SCO_OK) out[0] = 0;
}

void ActorsWhy(char* out, size_t n) {
    out[0] = 0;
    uint32_t size = static_cast<uint32_t>(n);
    if (!g_act || g_act->last_error(g_self, out, &size) != SCO_OK) out[0] = 0;
}

sco_result NoTarget(char* reply, uint32_t size) {
    snprintf(reply, size, "No target ship: stand or sit aboard one (crew.target keeps it after you leave)");
    return SCO_FAILED;
}

// crew.target's ship while it is still streamed in, else the ship you're aboard.
sco_result TargetShip(uint64_t& ship) {
    if (g_pinned) {
        uint32_t n = 0, more = 0;
        if (g_veh->seats(g_pinned, nullptr, 0, &n, &more) == SCO_OK) { ship = g_pinned; return SCO_OK; }
        g_pinned = 0;
    }
    return g_veh->player_ship(&ship);
}

// The target ship's seats into g_seats; the count, or a negative sco_result's reply in why.
int ListSeats(uint64_t& ship, char* reply, uint32_t size) {
    if (TargetShip(ship) != SCO_OK) { NoTarget(reply, size); return -1; }
    uint32_t n = 0, more = 0;
    const sco_result r = g_veh->seats(ship, g_seats, kMaxSeats, &n, &more);
    if (r != SCO_OK) {
        char why[160];
        VehicleWhy(why, sizeof(why));
        snprintf(reply, size, "Can't read the ship's seats: %s", why[0] ? why : "unknown");
        return -1;
    }
    return static_cast<int>(n);
}

bool PlayerActor(uint64_t& me) {
    me = 0;
    return Open(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, g_act) && g_act->local_player(&me, nullptr) == SCO_OK && me;
}

Job* JobOfActor(uint64_t actor) {
    for (Job& j : g_jobs) if (j.used && j.actor == actor) return &j;
    return nullptr;
}

Job* JobForSeat(uint64_t ship, uint32_t seat) {
    for (Job& j : g_jobs) if (j.used && j.linking && j.ship == ship && j.seat == seat) return &j;
    return nullptr;
}

Job* FreeJob() {
    for (Job& j : g_jobs) if (!j.used) return &j;
    return nullptr;
}

void StartJob(Job& j, uint64_t actor, bool player, uint64_t ship, const sc_vehicle_seat& s, uint32_t firstTry) {
    j = Job();
    j.used = true;
    j.linking = true;
    j.player = player;
    j.actor = actor;
    j.ship = ship;
    j.seat = s.index;
    j.since = g_now;
    j.nextTry = firstTry;
    strncpy_s(j.seatName, s.name, _TRUNCATE);
}

// Words matched against the seat's name, as the Vehicles tab's "seat by name" does (any case).
bool SeatMatches(const char* seat, const char* words) {
    char s[SC_VEHICLE_SEAT_NAME_MAX], w[64];
    strncpy_s(s, seat, _TRUNCATE);
    strncpy_s(w, words, _TRUNCATE);
    _strlwr_s(s);
    _strlwr_s(w);
    char* next = nullptr;
    for (char* word = strtok_s(w, " ", &next); word; word = strtok_s(nullptr, " ", &next))
        if (!strstr(s, word)) return false;
    return true;
}

// ---- commands ---------------------------------------------------------------------------------

sco_result Target(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    if (!Services(reply, size)) return SCO_FAILED;
    uint64_t ship = 0;
    if (g_veh->player_ship(&ship) != SCO_OK) {
        snprintf(reply, size, "You're not aboard a ship");
        return SCO_FAILED;
    }
    g_pinned = ship;
    snprintf(reply, size, "Targeting the ship you're in");
    return SCO_OK;
}

sco_result Sit(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const char* words = args[0].v.s;
    if (!words || !*words || strlen(words) > 47) {
        snprintf(reply, size, "Name the seat: words from its name, at most 47 characters (\"pilot\", \"turret left\")");
        return SCO_BAD_ARG;
    }
    if (!Services(reply, size)) return SCO_FAILED;
    uint64_t me = 0;
    if (!PlayerActor(me)) {
        snprintf(reply, size, "Can't find your actor (game.actors isn't ready, or you haven't spawned)");
        return SCO_FAILED;
    }
    uint64_t ship = 0;
    const int n = ListSeats(ship, reply, size);
    if (n < 0) return SCO_FAILED;
    const sc_vehicle_seat* unusable = nullptr;
    for (int i = 0; i < n; ++i) {
        const sc_vehicle_seat& s = g_seats[i];
        if (!SeatMatches(s.name, words)) continue;
        if (s.occupant_id == me) {
            snprintf(reply, size, "You're already in %s", s.name);
            return SCO_OK;
        }
        if (!(s.flags & SC_SEAT_USABLE)) { if (!unusable) unusable = &s; continue; }
        if (s.flags & SC_SEAT_OCCUPIED) {
            Job* crew = s.occupant_id ? JobOfActor(s.occupant_id) : nullptr;
            if (!crew || crew->player) {
                snprintf(reply, size, "Someone this plugin didn't add is in %s; stand them up first", s.name);
                return SCO_FAILED;
            }
            g_act->despawn(g_self, crew->actor);   // "removing an NPC in it": the seat frees once it's gone
            *crew = Job();
        }
        if (Job* mine = JobOfActor(me)) *mine = Job();
        Job* j = FreeJob();
        if (!j) {
            snprintf(reply, size, "The crew list is full (%d): run crew.clear", kMaxJobs);
            return SCO_FAILED;
        }
        StartJob(*j, me, true, ship, s, 0);
        snprintf(reply, size, "Sitting you in %s", s.name);
        return SCO_OK;
    }
    if (unusable) snprintf(reply, size, "%s isn't interactable: the game's seat picker skips it", unusable->name);
    else snprintf(reply, size, "The target ship has no seat matching '%s'", words);
    return SCO_FAILED;
}

sco_result StandAll(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    if (!Services(reply, size)) return SCO_FAILED;
    uint64_t me = 0;
    PlayerActor(me);
    uint64_t ship = 0;
    const int n = ListSeats(ship, reply, size);
    if (n < 0) return SCO_FAILED;
    for (Job& j : g_jobs) if (j.used && j.ship == ship) j.linking = false;   // no more seat() for them
    int up = 0, others = 0;
    for (int i = 0; i < n; ++i) {
        const uint64_t who = g_seats[i].occupant_id;
        if (!who) { if (g_seats[i].flags & SC_SEAT_OCCUPIED) ++others; continue; }
        if (who == me || JobOfActor(who)) {
            if (g_veh->eject(g_self, who) == SCO_OK) ++up; else ++others;
        } else {
            ++others;
        }
    }
    snprintf(reply, size, others ? "%d stood up; %d others aren't this plugin's to move" : "%d stood up", up, others);
    return SCO_OK;
}

sco_result Fill(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const char* npc = args[0].v.s;
    if (!npc || !*npc || strlen(npc) > kMaxNpcClass) {
        snprintf(reply, size, "Name an NPC archetype from npcs.txt");
        return SCO_BAD_ARG;
    }
    if (!Services(reply, size)) return SCO_FAILED;
    if (!Open(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, g_act) || !g_api->has("game.actors.spawn_npc")) {
        snprintf(reply, size, "game.actors can't spawn NPCs on this game build");
        return SCO_FAILED;
    }
    if (!Open(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION, g_sp)) {
        snprintf(reply, size, "teleport.spatial isn't published");
        return SCO_FAILED;
    }
    uint64_t ship = 0;
    const int n = ListSeats(ship, reply, size);
    if (n < 0) return SCO_FAILED;
    double pos[3], rot[4];
    uint64_t zone = 0;
    if (!g_sp->player_pose(pos, rot, &zone) || !zone) {
        snprintf(reply, size, "Can't read your position (you haven't spawned yet)");
        return SCO_FAILED;
    }
    pos[0] += 1.5;   // beside you; seat() moves the NPC into the ship

    // Only seats that are empty AND usable; the rest is counted, not tried (the game's own seat
    // picker never uses a seat whose owner has no IInteractableComponent).
    int seated = 0, skipped = 0, full = 0;
    char why[160] = "";
    for (int i = 0; i < n; ++i) {
        const sc_vehicle_seat& s = g_seats[i];
        if ((s.flags & SC_SEAT_OCCUPIED) || JobForSeat(ship, s.index)) continue;
        if (!(s.flags & SC_SEAT_USABLE)) { ++skipped; continue; }
        Job* j = FreeJob();
        if (!j) { ++full; continue; }
        uint64_t id = 0;
        const sco_result r = g_act->spawn_npc(g_self, npc, zone, pos, &id);
        if (r != SCO_OK || !id) {
            ActorsWhy(why, sizeof(why));
            break;
        }
        StartJob(*j, id, false, ship, s, g_now + kFirstTryMs);
        ++seated;
        LogF(SCO_LOG_INFO, "spawned %s (%llu) for seat '%s'", npc, static_cast<unsigned long long>(id), s.name);
    }
    if (!seated && why[0]) {
        snprintf(reply, size, "Couldn't spawn %s: %s", npc, why);
        return SCO_FAILED;
    }
    LogF(SCO_LOG_INFO, "Seating %d x %s (%d seats skipped: not interactable%s)", seated, npc, skipped,
         full ? ", crew list full" : "");
    snprintf(reply, size, "Seating %d x %s (%d seats skipped: not interactable)%s", seated, npc, skipped,
             why[0] ? "; stopped early, the game refused a spawn" : "");
    return seated || !n ? SCO_OK : SCO_FAILED;
}

sco_result Clear(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    if (!Services(reply, size)) return SCO_FAILED;
    if (!Open(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, g_act)) {
        snprintf(reply, size, "game.actors isn't published");
        return SCO_FAILED;
    }
    uint64_t ship = 0;
    if (TargetShip(ship) != SCO_OK) return NoTarget(reply, size);
    int removed = 0;
    for (Job& j : g_jobs) {
        if (!j.used || j.player || j.ship != ship) continue;
        if (g_act->despawn(g_self, j.actor) == SCO_OK) ++removed;
        j = Job();
    }
    snprintf(reply, size, "Removing %d NPCs this plugin added to the target ship", removed);
    return SCO_OK;
}

sco_result PowerOn(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    if (!Services(reply, size)) return SCO_FAILED;
    uint64_t ship = 0;
    if (TargetShip(ship) != SCO_OK) return NoTarget(reply, size);
    if (g_veh->power_on(g_self, ship) != SCO_OK) {
        char why[160];
        VehicleWhy(why, sizeof(why));
        snprintf(reply, size, "Flight Ready didn't go out: %s", why[0] ? why : "unknown");
        return SCO_FAILED;
    }
    snprintf(reply, size, "Sending Flight Ready to the target ship");
    return SCO_OK;
}

// ---- the seat jobs, once per tick -------------------------------------------------------------

void RunJobs() {
    if (!g_veh) return;
    for (Job& j : g_jobs) {
        if (!j.used || !j.linking || static_cast<int32_t>(g_now - j.nextTry) < 0) continue;
        uint64_t in = 0;
        const sco_result occ = g_veh->seat_occupant(j.ship, j.seat, &in);
        if (occ == SCO_OK && in == j.actor) {
            j.linking = false;
            if (j.player) LogF(SCO_LOG_INFO, "you are seated in '%s'", j.seatName);
            else LogF(SCO_LOG_INFO, "NPC %llu is seated in '%s'", static_cast<unsigned long long>(j.actor), j.seatName);
            continue;
        }
        if (occ == SCO_NOT_FOUND || j.links >= kMaxLinks || static_cast<uint32_t>(g_now - j.since) > kGiveUpMs) {
            LogF(SCO_LOG_WARN, "couldn't seat %s in '%s' (links tried: %d, occupant %s%llu%s%s)",
                 j.player ? "you" : "an NPC", j.seatName, j.links, occ == SCO_OK ? "" : "unreadable, ",
                 static_cast<unsigned long long>(in), j.why[0] ? ", last refusal: " : "", j.why);
            if (!j.player && g_act) g_act->despawn(g_self, j.actor);
            j = Job();
            continue;
        }
        if (g_veh->seat(g_self, j.actor, j.ship, j.seat) == SCO_OK) {
            ++j.links;
            j.why[0] = 0;
        } else {
            VehicleWhy(j.why, sizeof(j.why));
        }
        j.nextTry = g_now + kRetryMs;
    }
}

void OnTick(const char*, const void* data, void*) {
    if (data) g_now = *static_cast<const uint32_t*>(data);
    CrewTabTick(g_now);   // the Crew tab's own seat actions (spawner.cpp)
    RunJobs();
}

const sco_plugin_info* CrewQuery() { return &kInfo; }

sco_result CrewLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    const sco_arg_def seat[1] = { BuiltinArg("seat", SCO_ARG_STRING, "Words from the seat's name (pilot, copilot, turret left)") };
    const sco_arg_def npc[1] = { BuiltinArg("npc", SCO_ARG_STRING, "NPC archetype, as in npcs.txt") };
    // Each command is gated on the game pack capability it needs, not on a sc-offline row.
    sco_result r = RegisterBuiltinCommand(api, self, "game.vehicles.seats", "crew.target", "Target the ship I'm in",
        "Makes the ship you're in the target of the crew commands", Target);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, "game.vehicles.seat", "crew.sit", "Sit in seat",
        "Puts you in the target ship's first usable seat whose name has these words, removing a crew NPC in it", Sit, seat, 1);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, "game.vehicles.seat", "crew.stand_all", "Everyone stand up",
        "You and the crew NPCs this plugin added on the target ship get out of the seats", StandAll);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, "game.vehicles.seat", "crew.fill", "Fill empty seats",
        "Puts an NPC of this archetype in every empty, usable seat of the target ship and says how many seats it skipped", Fill, npc, 1);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, "game.actors.despawn", "crew.clear", "Remove NPC crew",
        "Removes the NPCs this plugin added to the target ship", Clear);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, "game.vehicles.flight_ready", "crew.power_on", "Power on",
        "Sends the game's Flight Ready event to the target ship", PowerOn);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) return r;   // the host releases what was registered
    // Its page of the menu (and keys), through sco.ui; the menu shell draws it (tabs.h).
    RegisterBuiltinTab(api, self, "crew.crew", "Crew", kTabCrew, DrawCrewTab);
    g_ticking = true;
    return SCO_OK;
}

// A crash leaves g_ticking set, so dllmain doesn't take the Crew tab's tick back (as with spawn).
// The host despawns the NPCs this plugin spawned through game.actors, so the jobs just go.
void CrewUnload() {
    g_ticking = false;
    for (Job& j : g_jobs) j = Job();
    g_pinned = 0;
    g_veh = nullptr;
    g_act = nullptr;
    g_sp = nullptr;
}

}  // namespace

bool CrewBuiltinOwnsTick() { return g_ticking; }

const sco::plugins::Builtin kCrewBuiltin = { "crew", CrewQuery, CrewLoad, CrewUnload };
