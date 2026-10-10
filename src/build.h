#pragma once
#include "common.h"

bool ResolveBuildApi(const Section& text, const Section& rdata);
void ProcessBuild();
bool PlaceNearPlayer(double ahead, double side, double lift, double pos[3], double rot[4]);
bool GroundRay(uintptr_t zone, const double from[3], const double to[3], double hit[3]);
// Moves any entity within its zone: pos and, when rot isn't null, rot (x, y, z, w) in the frame of
// the zone it's in. False when the id doesn't resolve or this build's entity slots don't match.
// Game thread. Build mode's preview, npc.cpp and the spawn.entities mover use it.
bool MoveEntityLocal(uint64_t id, const double pos[3], const double rot[4] = nullptr);
// An entity's rotation in its own zone, (x, y, z, w). False when this build's entity slots don't
// match. Game thread, inside __try.
bool EntityRotation(uintptr_t entity, double rot[4]);

// Squadron 42 tab: drop one buildable without entering build mode. It is recorded
// in the base, so Undo / Clear base in the build section remove it too.
void Menu_RequestPlace(int index, bool inFront, float aheadMetres);
