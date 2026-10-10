#pragma once
#include "common.h"

extern const uint8_t* g_isOnlineFlag;

bool ApplyOfflinePatches();
void LogOfflinePatches();
// Sets capability `name` (sco/game/offline.h, or features' offline.or_loop_bound) from its rows
// with sco::caps::SetFromSignatures; true when it is ready.
bool OfflineCapReady(const char* name);
// LogPatch, or "[!] <name>: not patched, <cap> isn't ready (see the [core] lines ...)".
void LogOfflinePatch(const char* name, const PatchStatus& st, const char* cap);
