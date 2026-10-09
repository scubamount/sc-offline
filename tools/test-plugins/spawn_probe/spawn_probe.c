/* spawn_probe: a test plugin for in-game checks of sc-offline's spawn built-in (not shipped).
 *   F6       logs what the spawn.entities service answers, then invokes spawn.ship with an
 *            unknown class (expect "failed") and with DRAK_Cutlass_Black at 40 m.
 *   Ctrl+F6  spawns a DRAK_Cutlass_Black 30 m from you through spawn.entities.
 * Every result goes to mod.log as "[spawn_probe] ...". */
#include "sco_api.h"
#include "spawn_service.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static const sco_api* g_api;
static sco_plugin*    g_self;
static int            g_f6Down;

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

static const sc_spawn_service_v1* spawn_service(void) {
    const void* table = NULL;
    sco_result r;
    if (g_api->size <= offsetof(sco_api, query_service) || !g_api->query_service) {
        say("this host has no services (api %u.%u)", g_api->major, g_api->minor);
        return NULL;
    }
    r = g_api->query_service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION, &table);
    say("query_service(%s 1.0) -> %s", SC_SPAWN_SERVICE_NAME, result_name(r));
    return r == SCO_OK ? (const sc_spawn_service_v1*)table : NULL;
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

static void on_tick(const char* event, const void* data, void* ctx) {
    const int down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    (void)event; (void)data; (void)ctx;
    if (down && !g_f6Down) {
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) run_service_spawn();
        else run_checks();
    }
    g_f6Down = down;
}

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &kInfo; }

SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    sco_result r;
    g_api = api;
    g_self = self;
    r = api->subscribe(self, "tick", on_tick, NULL);
    if (r == SCO_OK) say("loaded: F6 = service checks + spawn.ship, Ctrl+F6 = spawn through spawn.entities");
    return r;
}

SCO_EXPORT void sco_plugin_unload(void) {}
