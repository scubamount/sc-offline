#pragma once
#include "common.h"

using HubFn = uintptr_t(__fastcall*)(uintptr_t service);

uintptr_t StandInHub();
bool SwapHubSlot(HubFn hub, HubFn* real);

// After sco::ResolveAll, from DllMain. The hangar request with the stand-in hub always; with
// asop = on, the hangar groups of sco/game/asop.h that are ready (hangar.instance, atc.tokens,
// asop.diagnostics).
void ResolveHangarsApi();
void LogHangars();      // one [+] / [!] line per hangar feature, for LogStartup
void SetHangarCaps();   // the same readiness as capabilities, for SetFeatureCaps
// Every window message on the game thread, not throttled: a loading thread may be waiting for it
// to run a hangar continuation (services.cpp, rc7).
void ProcessHangars(DWORD now);
