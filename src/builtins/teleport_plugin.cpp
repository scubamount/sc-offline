// teleport: sc-offline's first built-in plugin (sco-core's docs/framework.md, Phase 4).
//
// The sco_api surface over teleport.cpp: two commands, gated on the "teleport" capability
// (SetFeatureCaps in dllmain.cpp), that any plugin reaches through invoke; the built-in binds F7
// and F8 to them through sco.ui, and the product dispatches those keys (hotkeys.cpp). The
// mechanics stay in teleport.cpp. Commands run on the game thread, where F7 and F8 always ran.
//
// teleport.spatial (sc_spatial.h) is published by sco-core's game pack since game pack 0.1.0
// (Platform::gameServices in dllmain.cpp), with the same table this file used to provide.
//
// A built-in is a table entry (builtins.h), not a DLL export: these functions are file-local, so
// they can't clash with sco_api.h's sco_plugin_* declarations and the DLL still exports only
// DirectInput8Create.
#include "builtins.h"
#include "builtin_store.h"
#include "tabs.h"
#include "../build.h"
#include "../hotkeys.h"
#include "../teleport.h"
#include "../version.h"
#include "sco/runtime.h"
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

// ---- load / unload ----------------------------------------------------------------------------

const sco_plugin_info* TeleportQuery() { return &kInfo; }

sco_result TeleportLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    g_teleportStore.Open(api, self);   // data/storage/teleport.db: the saved spot (teleport.cpp)
    sco_result r = Register("teleport.save", "Save spot", "Save where you're standing (F7)", Save);
    if (r == SCO_OK) r = Register("teleport.go", "Go to saved spot", "Teleport to the saved spot (F8)", Go);
    if (r != SCO_OK) { g_api = nullptr; g_self = nullptr; g_teleportStore.Close(); return r; }   // the host releases what was registered
    BindBuiltinHotkey(api, self, "f7", "teleport.save");
    BindBuiltinHotkey(api, self, "f8", "teleport.go");
    return SCO_OK;
}

void TeleportUnload() {
    g_teleportStore.Close();
    g_api = nullptr;
    g_self = nullptr;
}

}  // namespace

const sco::plugins::Builtin kTeleportBuiltin = { "teleport", TeleportQuery, TeleportLoad, TeleportUnload };
