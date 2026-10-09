#pragma once
// sc-offline's built-in plugins: its features as plugins compiled into the DLL (sco-core's
// docs/framework.md, Phase 4). Each is a sco::plugins::Builtin, the three plugin functions listed
// in a table instead of exported from the DLL. StartHostKit hands kBuiltins to sco::app::Start,
// which loads them before any plugin folder, with plugins on or off.
#include "sco/plugins.h"

extern const sco::plugins::Builtin kTeleportBuiltin;   // teleport_plugin.cpp: teleport.save, teleport.go

// Every built-in, in load order.
inline const sco::plugins::Builtin kBuiltins[] = { kTeleportBuiltin };
