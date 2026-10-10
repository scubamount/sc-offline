/* builtins_probe: a test plugin for in-game checks of sc-offline's feature built-ins (not shipped).
 *   Ctrl+Alt+4  lists every built-in command (crew, npc, loadout, quantum, build,
 *               contracts) with its title and whether its capability is there, then invokes
 *               contracts.status.
 *   Ctrl+Alt+5  invokes each command that takes a name with one no list has (expect "failed")
 *               and npc.spawn with a count of 11 (expect "bad_arg"). Nothing changes in game.
 *   Ctrl+Alt+6  creative.ammo and creative.ship_ammo (the creative plugin, once it is on): on the first press, off the next.
 *   Ctrl+Alt+7  build.toggle (as F6).
 *   Ctrl+Alt+8  first press: quantum.save_bookmark "builtins_probe" (saves where you are);
 *               next press: quantum.bookmark "builtins_probe" (takes you there). The Travel
 *               tab queues one request at a time, so they're separate presses.
 * (Not F-keys, and not Ctrl+Alt+1-3: spawn_probe uses those.)
 * Every result goes to mod.log as "[builtins_probe] <command> -> <result> \"<reply>\"". */
#include "sco_api.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const sco_api* g_api;
static sco_plugin*    g_self;
static int            g_down[5];
static int            g_ammoOn;
static int            g_saved;

static const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "builtins_probe", "1.0.0", "sc-offline tests",
};

static const char* const kOwners[] = { "crew.", "npc.", "loadout.", "quantum.", "build.", "contracts." };

static void say(const char* fmt, ...) {
    char buf[384];
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

static void on_done(sco_result r, const char* reply, void* ctx) {
    say("%s -> %s \"%s\"", (const char*)ctx, result_name(r), reply ? reply : "");
}

/* From the game thread (a tick): the command runs now and on_done logs it before this returns. */
static void call(const char* name, const sco_arg* args, uint32_t nargs) {
    const sco_result r = g_api->invoke(g_self, name, args, nargs, on_done, (void*)name);
    if (r == SCO_NOT_FOUND) say("%s -> not_found (no such command)", name);
}

static sco_arg str(const char* s) { sco_arg a; a.type = SCO_ARG_STRING; a._pad = 0; a.v.s = s; return a; }
static sco_arg num(double f) { sco_arg a; a.type = SCO_ARG_FLOAT; a._pad = 0; a.v.f = f; return a; }
static sco_arg integer(int64_t i) { sco_arg a; a.type = SCO_ARG_INT; a._pad = 0; a.v.i = i; return a; }
static sco_arg boolean(int on) { sco_arg a; a.type = SCO_ARG_BOOL; a._pad = 0; a.v.i = on ? 1 : 0; return a; }

static void list_builtin_commands(void) {
    const sco_command* cmds[256];
    uint32_t n = g_api->list_commands(cmds, 256), i, k, shown = 0;
    if (n > 256) n = 256;
    for (i = 0; i < n; ++i)
        for (k = 0; k < sizeof(kOwners) / sizeof(kOwners[0]); ++k)
            if (strncmp(cmds[i]->name, kOwners[k], strlen(kOwners[k])) == 0) {
                say("command %s \"%s\" (%u args) capability %s = %d", cmds[i]->name, cmds[i]->title ? cmds[i]->title : "",
                    cmds[i]->nargs, cmds[i]->capability ? cmds[i]->capability : "-",
                    cmds[i]->capability ? g_api->has(cmds[i]->capability) : 1);
                ++shown;
            }
    say("%u built-in feature commands", shown);
    call("contracts.status", NULL, 0);
}

static void failure_paths(void) {
    sco_arg a[2];
    a[0] = str("NOT_A_NAME_123"); a[1] = integer(1);
    call("npc.spawn", a, 2);
    a[0] = str("NPC_Test"); a[1] = integer(11);
    call("npc.spawn", a, 2);
    a[0] = str("NOT_A_NAME_123");
    call("crew.fill", a, 1);
    call("crew.sit", a, 1);
    call("loadout.equip", a, 1);
    call("loadout.wear", a, 1);
    call("quantum.bookmark", a, 1);
    a[1] = num(2000.0);
    call("quantum.travel", a, 2);
    a[1] = num(5.0);
    call("build.place", a, 2);
}

static void toggle_ammo(void) {
    sco_arg a = boolean(g_ammoOn = !g_ammoOn);
    call("creative.ammo", &a, 1);
    call("creative.ship_ammo", &a, 1);
}

static void travel(void) {
    sco_arg a = str("builtins_probe");
    call(g_saved ? "quantum.bookmark" : "quantum.save_bookmark", &a, 1);
    g_saved = 1;
}

static int held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void on_tick(const char* event, const void* data, void* ctx) {
    static const int keys[5] = { '4', '5', '6', '7', '8' };
    const int chord = held(VK_CONTROL) && held(VK_MENU);
    int i;
    (void)event; (void)data; (void)ctx;
    for (i = 0; i < 5; ++i) {
        const int down = chord && held(keys[i]);
        if (down && !g_down[i]) {
            switch (i) {
                case 0: list_builtin_commands(); break;
                case 1: failure_paths(); break;
                case 2: toggle_ammo(); break;
                case 3: call("build.toggle", NULL, 0); break;
                case 4: travel(); break;
            }
        }
        g_down[i] = down;
    }
}

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &kInfo; }

SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    sco_result r;
    g_api = api;
    g_self = self;
    r = api->subscribe(self, "tick", on_tick, NULL);
    if (r == SCO_OK)
        say("loaded: Ctrl+Alt+4 = list + contracts.status, 5 = failure paths, 6 = ammo on/off, 7 = build.toggle, "
            "8 = save bookmark, then go to it");
    return r;
}

SCO_EXPORT void sco_plugin_unload(void) {}
