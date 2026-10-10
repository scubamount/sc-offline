#pragma once
// Ship terminals (ASOP) offline: the terminal opens its list, Deliver stores a ship at the station,
// Retrieve brings it up on the personal hangar's ship lift, and Store from the hangar terminal takes
// it back down. Sections 5-10 of the research doc "Offline ship terminals (ASOP), ship lifts,
// personal hangars and ATC" (build 4.10.196.36804). Every address is one of sco-core's rows
// (sco/game/asop.h, docs/game/asop.md); each capability group there switches its own part on.
// services.cpp has the hangar half (instances, elevators, ATC tokens).
#include "common.h"

// sc-offline.ini `asop = on|off` (SC_OFFLINE_ASOP from the launcher; unset = on). Off: nothing of
// this module or of services.cpp's hangar module is installed.
bool AsopEnabled();
// sc-offline.ini `asop_fleet_list = game|ships` (SC_OFFLINE_ASOP_FLEET_LIST; unset = game). True with
// ASOP on and `game`: hooks.cpp leaves the game's own ship list alone instead of listing ships.txt.
bool Fleet_UseGameList();

// The rows of one sco::game::asop capability ("asop.terminal") are all OK. Otherwise false, with
// "needs <row> (<STATE>)" in why.
bool AsopCapabilityRows(const char* capability, char* why, size_t n);

// From DllMain after sco::ResolveAll (the rows match the game's original bytes, so they are
// resolved before any hook or the OnRequestOpen patch). Installs what each ready group needs.
void ResolveFleetApi();
void LogFleet();       // one [+] / [!] line per feature, for LogStartup
void SetFleetCaps();   // the same readiness as capabilities, for SetFeatureCaps
void ProcessFleet(DWORD now);   // game thread, every 100 ms

// For services.cpp's ATC token hooks; any thread.
// The player's entity id when vehicle is a ship this module retrieved, else 0 (rc20).
uint64_t Fleet_RetrievedShipOwner(uint64_t vehicle);
// A pad token now holds vehicle: a retrieved ship moves to that ATC and pad (rc22).
void Fleet_OnPadVehicle(uint64_t atcEntity, uint64_t padEntity, uint64_t vehicle);

// hooks.cpp: while this module's Retrieve runs, the fleet manager's retrieve leaves the ATC alone
// and runs the game's own code (asking the ATC directly for a ship that isn't stored locks every
// terminal at the station).
bool Fleet_RetrieveActive();
