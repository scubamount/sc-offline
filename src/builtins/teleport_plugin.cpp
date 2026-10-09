// teleport: sc-offline's first built-in plugin (sco-core's docs/framework.md, Phase 4).
//
// The sco_api surface over teleport.cpp: two commands, gated on the "teleport" capability
// (SetFeatureCaps in dllmain.cpp), that any plugin and the F7/F8 hotkeys reach through invoke.
// The mechanics stay in teleport.cpp. Commands run on the game thread, where F7 and F8 always ran.
//
// A built-in is a table entry (builtins.h), not a DLL export: these functions are file-local, so
// they can't clash with sco_api.h's sco_plugin_* declarations and the DLL still exports only
// DirectInput8Create.
#include "builtins.h"
#include "../teleport.h"
#include "../version.h"
#include <cstdio>

namespace {

const sco_api* g_api = nullptr;    // from TeleportLoad until TeleportUnload
sco_plugin*    g_self = nullptr;
const char*    g_hotkey = nullptr;   // "F7" / "F8" while a hotkey's invoke runs, so mod.log reads as before

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "teleport", SCO_VERSION, "sc-offline",
};

// A spot that can't be saved or reached answers SCO_UNAVAILABLE; the reply says why.
sco_result Save(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    return SaveSpotHere(g_hotkey ? g_hotkey : "teleport.save", reply, size) ? SCO_OK : SCO_UNAVAILABLE;
}

sco_result Go(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    return GoToSavedSpot(g_hotkey ? g_hotkey : "teleport.go", reply, size) ? SCO_OK : SCO_UNAVAILABLE;
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

const sco_plugin_info* TeleportQuery() { return &kInfo; }

sco_result TeleportLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    sco_result r = Register("teleport.save", "Save spot", "Save where you're standing (F7), in spawn.txt", Save);
    if (r == SCO_OK) r = Register("teleport.go", "Go to saved spot", "Teleport to the saved spot (F8)", Go);
    if (r != SCO_OK) { g_api = nullptr; g_self = nullptr; }   // the host releases what was registered
    return r;
}

void TeleportUnload() {
    g_api = nullptr;
    g_self = nullptr;
}

// A hotkey invokes its command as the teleport plugin itself (its own handle, kept from load), so
// it goes through the host's command table, capability check and crash guard like any plugin's
// call. Invoked from the game thread, the command runs before invoke returns. Before the built-in
// has loaded (or after it unloaded) the hotkey calls teleport.cpp directly, as it used to.
void Hotkey(const char* command, const char* key, bool (*direct)(const char*, char*, size_t)) {
    if (!g_api) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("[tp] %s: the teleport built-in isn't loaded, so %s runs directly", key, command);
        }
        char reply[256];
        direct(key, reply, sizeof(reply));
        return;
    }
    g_hotkey = key;
    const sco_result r = g_api->invoke(g_self, command, nullptr, 0, nullptr, nullptr);
    g_hotkey = nullptr;
    // SCO_UNAVAILABLE is the command's own failure, already logged with its reason.
    if (r != SCO_OK && r != SCO_UNAVAILABLE) Log("[tp] %s: %s returned %d", key, command, static_cast<int>(r));
}

}  // namespace

void TeleportSaveHotkey() { Hotkey("teleport.save", "F7", SaveSpotHere); }
void TeleportGoHotkey()   { Hotkey("teleport.go", "F8", GoToSavedSpot); }

const sco::plugins::Builtin kTeleportBuiltin = { "teleport", TeleportQuery, TeleportLoad, TeleportUnload };
