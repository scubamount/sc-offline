#pragma once
// sc-offline's built-in plugins: its features as plugins compiled into the DLL (sco-core's
// docs/framework.md, Phase 4). Each is a sco::plugins::Builtin, the three plugin functions listed
// in a table instead of exported from the DLL. StartHostKit hands kBuiltins to sco::app::Start,
// which loads them before any plugin folder, with plugins on or off.
#include "sco/plugins.h"
#include "../version.h"   // SCO_VERSION: every built-in reports it in its sco_plugin_info
#include <cstdint>
#include <cstring>

extern const sco::plugins::Builtin kTeleportBuiltin;   // teleport_plugin.cpp: teleport.save, teleport.go
extern const sco::plugins::Builtin kSpawnBuiltin;      // spawn_plugin.cpp: spawn.ship, spawn.entities, the spawner tick
extern const sco::plugins::Builtin kCrewBuiltin;       // crew_plugin.cpp: crew.*, seat actions and crew jobs
extern const sco::plugins::Builtin kNpcBuiltin;        // npc_plugin.cpp: npc.spawn, npc.clear (a game.actors consumer; no tick)
extern const sco::plugins::Builtin kLoadoutBuiltin;    // loadout_plugin.cpp: loadout.equip, loadout.wear, gear + outfit ticks
extern const sco::plugins::Builtin kAmmoBuiltin;       // ammo_plugin.cpp: ammo.infinite, ammo.ship_infinite, the ammo tick
extern const sco::plugins::Builtin kQuantumBuiltin;    // quantum_plugin.cpp: quantum.*, the quantum boost and travel ticks
extern const sco::plugins::Builtin kBuildBuiltin;      // build_plugin.cpp: build.*, the build-mode tick (F6)
extern const sco::plugins::Builtin kContractsBuiltin;  // contracts_plugin.cpp: contracts.status, the contracts tick
extern const sco::plugins::Builtin kMiningBuiltin;      // mining_plugin.cpp: mining.status, natural mining on the game.mining row (mining = on)
extern const sco::plugins::Builtin kMultiplayerBuiltin;  // multiplayer_plugin.cpp: multiplayer.*, co-presence over sco.net
// The optional bridges (docs/bridges.md), compiled in only with their CMake option; never in a release.
extern const sco::plugins::Builtin kTitanLinkBuiltin;  // titanlink/titanlink_plugin.cpp: titanlink.*, the Titanfall 2 link (SCO_BRIDGE_TITANLINK)
extern const sco::plugins::Builtin kVoxelBuiltin;      // voxel_bridge/voxel_plugin.cpp: voxel_bridge.*, the voxel-game link (SCO_BRIDGE_VOXEL)

// True while the spawn built-in is loaded (or crashed): its tick subscription runs ProcessShipMenu,
// so dllmain doesn't. False when it never loaded, and dllmain runs the spawner tick itself.
bool SpawnBuiltinOwnsTick();
// The same for the built-ins after it: true while that built-in's tick subscription runs its
// feature's per-tick work, so RunFeatureTicks (dllmain.cpp) runs it only as the fallback.
bool CrewBuiltinOwnsTick();
bool LoadoutBuiltinOwnsTick();
bool AmmoBuiltinOwnsTick();
bool QuantumBuiltinOwnsTick();
bool BuildBuiltinOwnsTick();
bool ContractsBuiltinOwnsTick();
// game.mining from sco-core's rows: ready when the cell function is found (SetFeatureCaps).
void SetMiningCaps();

// The index of the entry named `name` (any case) in a list of count names, or -1.
inline int FindBuiltinName(int count, const char* (*nameAt)(int), const char* name) {
    for (int i = 0; i < count; ++i)
        if (_stricmp(nameAt(i), name) == 0) return i;
    return -1;
}

// One argument of a built-in's command.
inline sco_arg_def BuiltinArg(const char* name, sco_arg_type type, const char* help) {
    sco_arg_def a = {};
    a.name = name;
    a.type = type;
    a.help = help;
    return a;
}

// Registers one of a built-in's commands, gated on its capability (the built-in's id).
inline sco_result RegisterBuiltinCommand(const sco_api* api, sco_plugin* self, const char* capability, const char* name,
                                         const char* title, const char* help, sco_command_fn fn,
                                         const sco_arg_def* args = nullptr, uint32_t nargs = 0) {
    sco_command c = {};
    c.size = sizeof(c);
    c.name = name;
    c.title = title;
    c.help = help;
    c.capability = capability;
    c.args = args;
    c.nargs = nargs;
    c.arg_def_size = sizeof(sco_arg_def);
    c.fn = fn;
    return api->register_command(self, &c);
}

// Every built-in, in load order.
inline const sco::plugins::Builtin kBuiltins[] = { kTeleportBuiltin, kSpawnBuiltin, kCrewBuiltin, kLoadoutBuiltin, kNpcBuiltin, kAmmoBuiltin, kQuantumBuiltin, kBuildBuiltin, kContractsBuiltin, kMiningBuiltin,
    kMultiplayerBuiltin
#ifdef SCO_BRIDGE_TITANLINK
    , kTitanLinkBuiltin
#endif
#ifdef SCO_BRIDGE_VOXEL
    , kVoxelBuiltin
#endif
};
