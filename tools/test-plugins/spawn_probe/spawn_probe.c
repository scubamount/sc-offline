/* spawn_probe: a test plugin for in-game checks of sc-offline's spawn built-in (not shipped).
 *   Ctrl+Alt+1  logs what the spawn.entities service answers, then invokes spawn.ship with an
 *               unknown class (expect "failed") and with DRAK_Cutlass_Black at 40 m.
 *   Ctrl+Alt+2  spawn.entities 1.2's mover, one step per press, then from the top again:
 *               1. spawns a DRAK_Cutlass_Black 30 m above you with spawn_near_player (nobody's)
 *                  and a small ship 20 m beside you with spawn_as (this plugin's);
 *               2. set_entity_transform in the world frame: the small ship to 60 m above you
 *                  (expect 1), then the Cutlass, you and your ship, none spawned through
 *                  spawn_as by this plugin (expect 0 for each);
 *               3. set_entity_transform in the small ship's own zone frame: to 25 m beside you,
 *                  turned as you stand (expect 1).
 *               Steps 2 and 3 need the small ship streamed in (entity_alive): until it is, the
 *               press logs "waiting for <id> to stream in" and the step runs by itself once it
 *               has, or gives up after 60 s.
 *   Ctrl+Alt+3  logs what the teleport.spatial service answers about you (pose, zone name, a
 *               local -> world -> local round trip, zone_of_entity) and spawn.entities 1.1's
 *               entity_alive for you and for a made-up id.
 *   Ctrl+Alt+=  game.entities 1.0 (sc_entities.h), one press: spawns a small prop 5 m ahead of you,
 *               waits for it to stream in (up to 60 s), then get_transform, set_transform 1 m up,
 *               get_transform again, class_of, alive, a set_transform on your own character (expect
 *               a refusal, and "[game] warning: ..." in mod.log) and despawn (twice: ok, then
 *               not_found). Ctrl+Alt+1-9 and 0 are all taken by the probes, hence the =.
 * (Not F-keys: sc-offline uses F6 for build mode and F7/F8 for teleport, and the game many more.)
 * Every result goes to mod.log as "[spawn_probe] ...". */
#include "sco_api.h"
#include <sc_entities.h>
#include <sc_spawn.h>
#include <sc_spatial.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static const sco_api* g_api;
static sco_plugin*    g_self;
static int            g_down1, g_down2, g_down3, g_downEnt;
static int            g_entStep;        /* Ctrl+Alt+=: 0 idle, 1 waiting for g_prop to stream in */
static uint64_t       g_prop;           /* spawned with game.entities.spawn */
static ULONGLONG      g_propUntil;      /* GetTickCount64 deadline while waiting for g_prop */
static int            g_moverStep;      /* Ctrl+Alt+2: 0 spawn, 1 world frame, 2 zone frame */
static ULONGLONG      g_waitUntil;      /* GetTickCount64 deadline while a step waits for g_mine; 0 = none */
static uint64_t       g_mine, g_theirs; /* spawned with spawn_as / spawn_near_player */

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

/* service() without its log line, for the per-tick wait. */
static const void* quiet_service(const char* name, uint32_t version) {
    const void* table = NULL;
    if (g_api->size <= offsetof(sco_api, query_service) || !g_api->query_service) return NULL;
    return g_api->query_service(name, version, &table) == SCO_OK ? table : NULL;
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

static const char* small_ship(const sc_spawn_service_v1* s) {
    static const char* const kSmall[] = { "ORIG_100i", "ANVL_Arrow", "AEGS_Gladius", "DRAK_Cutlass_Black" };
    size_t i;
    for (i = 0; i < sizeof(kSmall) / sizeof(kSmall[0]) - 1; ++i)
        if (s->class_exists(kSmall[i])) return kSmall[i];
    return kSmall[i];
}

static void mover_spawn(const sc_spawn_service_v1* s) {
    double above[3] = { 0.0, 0.0, 30.0 }, beside[3] = { 20.0, 0.0, 5.0 };
    const char* cls = small_ship(s);
    const char* err;
    g_theirs = g_mine = 0;
    err = s->spawn_near_player("DRAK_Cutlass_Black", above, &g_theirs);
    say("spawn_near_player(DRAK_Cutlass_Black, 30 m) -> %s, id %llu", err ? err : "ok", (unsigned long long)g_theirs);
    err = s->spawn_as(g_self, cls, beside, &g_mine);
    say("spawn_as(%s, 20 m beside) -> %s, id %llu", cls, err ? err : "ok", (unsigned long long)g_mine);
}

static void mover_world(const sc_spawn_service_v1* s, const sc_spatial_v1* sp) {
    const double ident[4] = { 0.0, 0.0, 0.0, 1.0 };
    double pos[3], rot[4], target[3], world[3] = { 0 };
    uint64_t zone = 0;
    const uint64_t me = s->local_player_id(), ship = s->player_ship_id();
    int ok = sp->player_pose(pos, rot, &zone);
    if (ok) {
        target[0] = pos[0]; target[1] = pos[1]; target[2] = pos[2] + 60.0;
        ok = sp->local_to_world(zone, target, world);
    }
    if (!ok) { say("mover: player_pose / local_to_world failed; are you spawned?"); return; }
    say("set_entity_transform(mine %llu, world frame, 60 m above you) -> %d (expect 1)", (unsigned long long)g_mine,
        s->set_entity_transform(g_self, g_mine, 0, world, ident));
    say("set_entity_transform(theirs %llu, spawn_near_player) -> %d (expect 0)", (unsigned long long)g_theirs,
        s->set_entity_transform(g_self, g_theirs, 0, world, ident));
    say("set_entity_transform(you %llu) -> %d (expect 0)", (unsigned long long)me,
        s->set_entity_transform(g_self, me, 0, world, ident));
    if (ship)
        say("set_entity_transform(your ship %llu) -> %d (expect 0)", (unsigned long long)ship,
            s->set_entity_transform(g_self, ship, 0, world, ident));
}

static void mover_zone(const sc_spawn_service_v1* s, const sc_spatial_v1* sp) {
    const double ident[4] = { 0.0, 0.0, 0.0, 1.0 };
    double pos[3], rot[4], target[3], local[3] = { 0 };
    uint64_t zone = 0, shipZone = 0;
    if (!sp->player_pose(pos, rot, &zone)) { say("mover: player_pose failed; are you spawned?"); return; }
    if (!sp->zone_of_entity(g_mine, &shipZone)) { say("mover: zone_of_entity(mine %llu) failed", (unsigned long long)g_mine); return; }
    target[0] = pos[0] + 25.0; target[1] = pos[1]; target[2] = pos[2] + 2.0;
    if (!sp->zone_to_zone(zone, shipZone, target, local)) {
        say("mover: zone_to_zone(%llu -> %llu) failed", (unsigned long long)zone, (unsigned long long)shipZone);
        return;
    }
    say("set_entity_transform(mine %llu, its zone %llu (%s yours), 25 m beside you) -> %d (expect 1)",
        (unsigned long long)g_mine, (unsigned long long)shipZone, shipZone == zone ? "same as" : "differs from",
        s->set_entity_transform(g_self, g_mine, shipZone, local, shipZone == zone ? rot : ident));
}

static void run_mover(void) {
    const sc_spawn_service_v1* s = spawn_service();
    const sc_spatial_v1* sp = (const sc_spatial_v1*)service(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION);
    if (!s || !sp) return;
    if (s->size <= offsetof(sc_spawn_service_v1, spawn_as)) { say("spawn.entities table is older than 1.2 (size %u)", s->size); return; }
    if (g_moverStep > 0 && !g_mine) { say("mover: no spawn_as ship to move; spawning first"); g_moverStep = 0; }
    g_waitUntil = 0;
    if (g_moverStep > 0 && !s->entity_alive(g_mine)) {
        say("waiting for %llu to stream in (step %d runs when it has; up to 60 s)", (unsigned long long)g_mine, g_moverStep + 1);
        g_waitUntil = GetTickCount64() + 60000;
        return;
    }
    say("mover step %d of 3", g_moverStep + 1);
    if (g_moverStep == 0) mover_spawn(s);
    else if (g_moverStep == 1) mover_world(s, sp);
    else mover_zone(s, sp);
    g_moverStep = (g_moverStep + 1) % 3;
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

/* ---- game.entities 1.0 -------------------------------------------------------------------- */

static const sc_entities_v1* entities(int quiet) {
    const sc_entities_v1* e = (const sc_entities_v1*)(quiet ? quiet_service(SC_ENTITIES_NAME, SC_ENTITIES_VERSION_1_0)
                                                            : service(SC_ENTITIES_NAME, SC_ENTITIES_VERSION_1_0));
    if (e && e->size <= offsetof(sc_entities_v1, last_error)) {
        say("game.entities table is too short (size %u)", e->size);
        return NULL;
    }
    return e;
}

/* "what -> result", plus the service's reason when it refused. */
static void say_result(const sc_entities_v1* e, const char* what, sco_result r, const char* expect) {
    char why[192] = "";
    uint32_t n = sizeof(why);
    if (r != SCO_OK) e->last_error(g_self, why, &n);
    say("%s -> %s (expect %s)%s%s", what, result_name(r), expect, why[0] ? ": " : "", why);
}

static void entities_start(void) {
    static const char* const kProps[] = { "PlayerDeco_Flair_Heart_Table_1_a", "PlayerDeco_Flair_Hanger_Flag_UEE_1" };
    const sc_entities_v1* e = entities(0);
    const sc_spatial_v1* sp = (const sc_spatial_v1*)service(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION);
    double pos[3], rot[4], at[3], fwd[3];
    uint64_t zone = 0;
    sco_result r = SCO_NOT_FOUND;
    size_t i;
    char what[160];
    if (!e || !sp) return;
    if (g_entStep) { say("game.entities check already running (waiting for %llu)", (unsigned long long)g_prop); return; }
    if (!sp->player_pose(pos, rot, &zone)) { say("game.entities: player_pose failed; are you spawned?"); return; }
    /* Forward is +y in a zone's frame, rotated by your rotation (x, y, z, w); up is +z. */
    fwd[0] = 2.0 * (rot[0] * rot[1] - rot[3] * rot[2]);
    fwd[1] = 1.0 - 2.0 * (rot[0] * rot[0] + rot[2] * rot[2]);
    fwd[2] = 2.0 * (rot[1] * rot[2] + rot[3] * rot[0]);
    at[0] = pos[0] + fwd[0] * 5.0; at[1] = pos[1] + fwd[1] * 5.0; at[2] = pos[2] + fwd[2] * 5.0;
    for (i = 0; i < sizeof(kProps) / sizeof(kProps[0]); ++i) {
        g_prop = 0;
        r = e->spawn(g_self, kProps[i], zone, at, rot, &g_prop);
        snprintf(what, sizeof(what), "spawn(%s, 5 m ahead, zone %llu) id %llu", kProps[i], (unsigned long long)zone,
                 (unsigned long long)g_prop);
        say_result(e, what, r, "ok");
        if (r != SCO_NOT_FOUND) break;   /* not found: this class isn't on this build; try the next */
    }
    if (r != SCO_OK) { g_prop = 0; return; }
    g_entStep = 1;
    g_propUntil = GetTickCount64() + 60000;
    say("waiting for %llu to stream in (up to 60 s)", (unsigned long long)g_prop);
}

static void entities_finish(const sc_entities_v1* e, int timedOut) {
    char what[160], cls[96] = "";
    double pos[3] = { 0 }, rot[4] = { 0 }, up[3], after[3] = { 0 };
    uint64_t zone = 0, me = 0;
    uint32_t n = sizeof(cls);
    sco_result r;
    const sc_spawn_service_v1* s = (const sc_spawn_service_v1*)quiet_service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION);
    g_entStep = 0;
    if (timedOut) {
        say("%llu didn't stream in within 60 s; skipping to despawn", (unsigned long long)g_prop);
    } else {
        r = e->get_transform(g_prop, pos, rot, &zone);
        snprintf(what, sizeof(what), "get_transform(%llu) zone %llu pos (%.3f, %.3f, %.3f)", (unsigned long long)g_prop,
                 (unsigned long long)zone, pos[0], pos[1], pos[2]);
        say_result(e, what, r, "ok");
        if (r == SCO_OK) {
            up[0] = pos[0]; up[1] = pos[1]; up[2] = pos[2] + 1.0;
            snprintf(what, sizeof(what), "set_transform(%llu, 1 m up)", (unsigned long long)g_prop);
            say_result(e, what, e->set_transform(g_self, g_prop, zone, up, rot), "ok");
            r = e->get_transform(g_prop, after, NULL, NULL);
            snprintf(what, sizeof(what), "get_transform again: z %.3f, moved %.3f m up", after[2], after[2] - pos[2]);
            say_result(e, what, r, "ok, 1.000 once the game has applied it");
        }
        r = e->class_of(g_prop, cls, &n);
        snprintf(what, sizeof(what), "class_of(%llu) '%s'", (unsigned long long)g_prop, cls);
        say_result(e, what, r, "ok");
        say("alive(%llu)=%d alive(0xDEAD)=%d", (unsigned long long)g_prop, e->alive(g_prop), e->alive(0xDEAD));
        if (s) me = s->local_player_id();
        if (me && zone) {
            snprintf(what, sizeof(what), "set_transform(you %llu), not spawned here", (unsigned long long)me);
            say_result(e, what, e->set_transform(g_self, me, zone, pos, rot), "failed + [game] warning in mod.log");
        }
    }
    snprintf(what, sizeof(what), "despawn(%llu)", (unsigned long long)g_prop);
    say_result(e, what, e->despawn(g_self, g_prop), "ok");
    snprintf(what, sizeof(what), "despawn(%llu) again", (unsigned long long)g_prop);
    say_result(e, what, e->despawn(g_self, g_prop), "not_found");
    g_prop = 0;
}

static void entities_tick(void) {
    const sc_entities_v1* e;
    if (!g_entStep) return;
    e = entities(1);
    if (!e) { g_entStep = 0; return; }
    if (e->alive(g_prop)) entities_finish(e, 0);
    else if (GetTickCount64() > g_propUntil) entities_finish(e, 1);
}

static int held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void on_tick(const char* event, const void* data, void* ctx) {
    const int chord = held(VK_CONTROL) && held(VK_MENU);
    const int down1 = chord && held('1');
    const int down2 = chord && held('2');
    const int down3 = chord && held('3');
    const int downEnt = chord && held(VK_OEM_PLUS);
    (void)event; (void)data; (void)ctx;
    if (down1 && !g_down1) run_checks();
    if (down2 && !g_down2) run_mover();
    else if (g_waitUntil) {   /* a step waiting for the small ship to stream in */
        const sc_spawn_service_v1* s = (const sc_spawn_service_v1*)quiet_service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION);
        if (s && s->entity_alive(g_mine)) run_mover();
        else if (GetTickCount64() > g_waitUntil) {
            say("gave up waiting for %llu after 60 s; press Ctrl+Alt+2 to wait again", (unsigned long long)g_mine);
            g_waitUntil = 0;
        }
    }
    if (down3 && !g_down3) run_spatial();
    if (downEnt && !g_downEnt) entities_start();
    entities_tick();
    g_downEnt = downEnt;
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
        say("loaded: Ctrl+Alt+1 = service checks + spawn.ship, Ctrl+Alt+2 = spawn.entities mover (3 steps), "
            "Ctrl+Alt+3 = teleport.spatial + entity_alive, Ctrl+Alt+= = game.entities");
    return r;
}

SCO_EXPORT void sco_plugin_unload(void) {}
