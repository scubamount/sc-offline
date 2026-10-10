/* spawn_probe: a test plugin for in-game checks of sc-offline's spawn built-in (not shipped).
 *   Ctrl+Alt+1  logs what the spawn.entities service answers, then invokes spawn.ship with an
 *               unknown class (expect "failed") and with DRAK_Cutlass_Black at 40 m.
 *   Ctrl+Alt+2  spawns a DRAK_Cutlass_Black 30 m from you through spawn.entities.
 *   Ctrl+Alt+3  logs what the teleport.spatial service answers about you (pose, zone name, a
 *               local -> world -> local round trip, zone_of_entity) and spawn.entities 1.1's
 *               entity_alive for you and for a made-up id.
 * (Not F-keys: sc-offline uses F6 for build mode and F7/F8 for teleport, and the game many more.)
 * Every result goes to mod.log as "[spawn_probe] ...". */
#include "sco_api.h"
#include "spawn_service.h"
#include <sc_spatial.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static const sco_api* g_api;
static sco_plugin*    g_self;
static int            g_down1, g_down2, g_down3;

static const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "spawn_probe", "1.0.0", "sc-offline tests",
};

static void say(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_api->log(g_self, SCO_LOG_INFO, buf);
}

static const char* result_name(sco_result r) {
    switch (r) {
        case SCO_OK: return "ok";
        case SCO_UNAVAILABLE: return "unavailable";
        case SCO_NOT_FOUND: return "not_found";
        case SCO_BAD_ARG: return "bad_arg";
        case SCO_CRASHED: return "crashed";
        case SCO_WRONG_THREAD: return "wrong_thread";
        case SCO_TOO_MANY: return "too_many";
        case SCO_FAILED: return "failed";
        default: return "?";
    }
}

static const void* service(const char* name, uint32_t version) {
    const void* table = NULL;
    sco_result r;
    if (g_api->size <= offsetof(sco_api, query_service) || !g_api->query_service) {
        say("this host has no services (api %u.%u)", g_api->major, g_api->minor);
        return NULL;
    }
    r = g_api->query_service(name, version, &table);
    say("query_service(%s %u.%u) -> %s", name, version >> 16, version & 0xFFFFu, result_name(r));
    return r == SCO_OK ? table : NULL;
}

static const sc_spawn_service_v1* spawn_service(void) {
    return (const sc_spawn_service_v1*)service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION);
}

static void on_done(sco_result r, const char* reply, void* ctx) {
    say("spawn.ship %s -> %s \"%s\"", (const char*)ctx, result_name(r), reply ? reply : "");
}

static void ship(const char* cls, double height) {
    sco_arg a[2];
    a[0].type = SCO_ARG_STRING; a[0]._pad = 0; a[0].v.s = cls;
    a[1].type = SCO_ARG_FLOAT;  a[1]._pad = 0; a[1].v.f = height;
    g_api->invoke(g_self, "spawn.ship", a, 2, on_done, (void*)cls);   /* game thread: runs now */
}

static void run_checks(void) {
    const sc_spawn_service_v1* s = spawn_service();
    if (s) {
        say("class_exists: DRAK_Cutlass_Black=%d NOT_A_CLASS_123=%d", s->class_exists("DRAK_Cutlass_Black"),
            s->class_exists("NOT_A_CLASS_123"));
        say("local_player_id=%llu player_ship_id=%llu", (unsigned long long)s->local_player_id(),
            (unsigned long long)s->player_ship_id());
    }
    ship("NOT_A_CLASS_123", 40.0);
    ship("DRAK_Cutlass_Black", 40.0);
}

static void run_service_spawn(void) {
    const sc_spawn_service_v1* s = spawn_service();
    double offset[3] = { 0.0, 0.0, 30.0 };
    uint64_t id = 0;
    const char* err;
    if (!s) return;
    err = s->spawn_near_player("DRAK_Cutlass_Black", offset, &id);
    say("spawn_near_player(DRAK_Cutlass_Black, 30 m) -> %s, id %llu", err ? err : "ok", (unsigned long long)id);
}

static double distance(const double a[3], const double b[3]) {
    const double d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

static void run_spatial(void) {
    const sc_spatial_v1* sp = (const sc_spatial_v1*)service(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION);
    const sc_spawn_service_v1* s = spawn_service();
    double pos[3] = { 0 }, rot[4] = { 0 }, world[3] = { 0 }, back[3] = { 0 }, viaZone[3] = { 0 };
    uint64_t zone = 0, entityZone = 0, me = 0;
    char name[96] = "";
    int ok, a, b, c;
    if (s) me = s->local_player_id();
    if (sp) {
        ok = sp->player_pose(pos, rot, &zone);
        say("player_pose -> %d: zone %llu pos (%.3f, %.3f, %.3f) m rot (%.4f, %.4f, %.4f, %.4f) |rot| %.6f", ok,
            (unsigned long long)zone, pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], rot[3],
            sqrt(rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3]));
        if (ok) {
            a = sp->zone_name(zone, name, sizeof(name));
            say("zone_name -> %d '%s'", a, name);
            a = sp->local_to_world(zone, pos, world);
            b = a && sp->world_to_local(zone, world, back);
            say("local_to_world -> %d world (%.3f, %.3f, %.3f) m; world_to_local -> %d; round-trip error %.3g m", a,
                world[0], world[1], world[2], b, b ? distance(pos, back) : -1.0);
            c = sp->zone_to_zone(zone, 0, pos, viaZone);
            say("zone_to_zone(zone, world) -> %d; differs from local_to_world by %.3g m", c, c && a ? distance(world, viaZone) : -1.0);
        }
        if (me) {
            ok = sp->zone_of_entity(me, &entityZone);
            say("zone_of_entity(player %llu) -> %d: zone %llu (%s player_pose's)", (unsigned long long)me, ok,
                (unsigned long long)entityZone, entityZone == zone ? "same as" : "differs from");
        }
    }
    if (s) {
        if (s->size > offsetof(sc_spawn_service_v1, entity_alive))
            say("entity_alive(player %llu)=%d entity_alive(0xDEAD)=%d", (unsigned long long)me, s->entity_alive(me),
                s->entity_alive(0xDEAD));
        else
            say("spawn.entities table is 1.0 (size %u): no entity_alive", s->size);
    }
}

static int held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void on_tick(const char* event, const void* data, void* ctx) {
    const int chord = held(VK_CONTROL) && held(VK_MENU);
    const int down1 = chord && held('1');
    const int down2 = chord && held('2');
    const int down3 = chord && held('3');
    (void)event; (void)data; (void)ctx;
    if (down1 && !g_down1) run_checks();
    if (down2 && !g_down2) run_service_spawn();
    if (down3 && !g_down3) run_spatial();
    g_down1 = down1;
    g_down2 = down2;
    g_down3 = down3;
}

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &kInfo; }

SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    sco_result r;
    g_api = api;
    g_self = self;
    r = api->subscribe(self, "tick", on_tick, NULL);
    if (r == SCO_OK)
        say("loaded: Ctrl+Alt+1 = service checks + spawn.ship, Ctrl+Alt+2 = spawn through spawn.entities, "
            "Ctrl+Alt+3 = teleport.spatial + entity_alive");
    return r;
}

SCO_EXPORT void sco_plugin_unload(void) {}
