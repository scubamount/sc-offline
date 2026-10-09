#pragma once
#include "common.h"

bool ResolveSpawnApi(const Section& text, const Section& rdata);
bool SpawnerReady();
void ReadStartOptions();
bool StartingOverDaymar();
bool PluginsEnabled();   // plugins = on (SC_OFFLINE_PLUGINS); read by ReadStartOptions
void ProcessShipMenu(DWORD now);

// Squadron 42 tab: spawn an entity class the menu's ship list doesn't carry.
// enemyWing also spawns the Vanduul AI wing next to it (the "Bengal + Vanduul wing" row).
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
uint64_t PlayerShipId();   // the ship you are aboard, or 0
uint64_t TargetShipId();   // the Crew & seats target ship, or 0
bool EntityAlive(uint64_t id);   // does the id resolve in the entity system now? (game thread; guarded)
bool FindEntityByNameEx(const char* name, uintptr_t& entity, uint64_t& id);   // entity pointer + id, or false
bool EntityClassExists(const char* name);   // is this a spawnable entity class? (call inside __try)
// Components of one type on a ship's parts (game thread). Returns how many were found.
int ShipPartComponents(uint64_t shipId, const char* type, uintptr_t* components, char (*names)[96], int max);
bool UnseatEntityById(uint64_t id);          // take an actor out of whatever seat it's in
