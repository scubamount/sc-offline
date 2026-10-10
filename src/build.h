#pragma once
#include "common.h"
#include <sc_entities.h>
#include <sc_world.h>

bool ResolveBuildApi(const Section& text, const Section& rdata);
void ProcessBuild();
// The build built-in hands build mode the game.entities table and its plugin handle (null when
// the service is missing or the built-in unloads): props are then spawned, moved and removed
// through it. Game thread.
void BuildUseEntities(const sc_entities_v1* ent, sco_plugin* self);
// The build built-in hands build mode the game.world table (null when the service is missing or the
// built-in unloads): the ground ray with nothing skipped (GroundRay, PlaceNearPlayer) and the camera
// build mode aims from go through it; without it they read the game through sco-core's build.* rows.
// self is the build plugin's handle, for last_error. Game thread.
void BuildUseWorld(const sc_world_v1* world, sco_plugin* self);
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
