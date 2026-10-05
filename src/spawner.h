#pragma once
#include "common.h"

bool ResolveSpawnApi(const Section& text, const Section& rdata);
bool SpawnerReady();
void ReadStartOptions();
bool StartingOverDaymar();
void ProcessShipMenu(DWORD now);

// Squadron 42 tab: spawn an entity class the menu's ship list doesn't carry.
// enemyWing also spawns the Vanduul AI wing next to it (Bengal B).
bool Menu_EnemySideAvailable();
void RefreshEnemySide();   // game thread only: caches the enemy-side lookup
void Menu_RequestSpawnClass(const char* cls, float heightAboveMe, bool sitInPilotSeat, bool flightReady, bool enemyWing);
const char* SpawnEntityNearPlayer(const char* entityClass, const double offset[3], uint64_t& id);
uintptr_t EntityComponent(uintptr_t entity, const char* type);
const char* SpawnEntityInPlayerZone(const char* entityClass, const double pos[3], const double rot[4], uint64_t& id);
const char* SpawnEntityInZone(const char* entityClass, uint64_t zoneId, const double pos[3], const double rot[4], uint64_t& id);
const char* SpawnPrefabInPlayerZone(const char* ocPath, const double pos[3], const double rot[4], uint64_t& id);
bool PrefabsReady();
uint64_t LocalPlayerEntityId();
uint64_t EntityIdOfComponent(uintptr_t component);
uint64_t EntityIdOfHandle(const void* handle);
