// ammo: infinite ammo as a built-in plugin.
//
// ammo.infinite and ammo.ship_infinite switch the Player tab's Infinite ammo and the Vehicles
// tab's Infinite ship ammo, gated on the "ammo" capability (the magazine setter hooked). The
// built-in's tick subscription runs ProcessAmmo, which tracks you and your ships and tops up
// ship magazines. The detour and the mechanics stay in ammo.cpp.
//
// The menu's checkboxes keep their own state: a command doesn't tick or untick them.
#include "builtins.h"
#include "../ammo.h"
#include "../menu.h"
#include "../teleport.h"
#include <cstdio>

namespace {

bool g_ticking = false;   // the tick subscription owns ProcessAmmo

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "ammo", SCO_VERSION, "sc-offline",
};

constexpr const char* kCap = "ammo";

sco_result Infinite(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const bool on = args[0].v.i != 0;
    Menu_SetInfiniteAmmo(on);   // logs "[ammo] infinite ammo on/off"
    snprintf(reply, size, "Infinite ammo %s", on ? "on" : "off");
    return SCO_OK;
}

sco_result ShipInfinite(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const bool on = args[0].v.i != 0;
    Menu_SetInfiniteShipAmmo(on);   // logs "[ammo] infinite ship ammo on/off"
    snprintf(reply, size, "Infinite ship ammo %s", on ? "on" : "off");
    return SCO_OK;
}

void OnTick(const char*, const void*, void*) {
    if (g_tp.ok) ProcessAmmo();
}

const sco_plugin_info* AmmoQuery() { return &kInfo; }

sco_result AmmoLoad(const sco_api* api, sco_plugin* self) {
    const sco_arg_def on[1] = { BuiltinArg("on", SCO_ARG_BOOL, "true = on, false = off") };
    sco_result r = RegisterBuiltinCommand(api, self, kCap, "ammo.infinite", "Infinite ammo",
        "Your weapons' magazines never run down (the Player tab's checkbox)", Infinite, on, 1);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "ammo.ship_infinite", "Infinite ship ammo",
        "The magazines of the ship you're in and the Crew target ship stay full (the Vehicles tab's checkbox)",
        ShipInfinite, on, 1);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) return r;   // the host releases what was registered
    g_ticking = true;
    return SCO_OK;
}

// A crash leaves g_ticking set, so dllmain doesn't take ProcessAmmo back.
void AmmoUnload() { g_ticking = false; }

}  // namespace

bool AmmoBuiltinOwnsTick() { return g_ticking; }

const sco::plugins::Builtin kAmmoBuiltin = { "ammo", AmmoQuery, AmmoLoad, AmmoUnload };
