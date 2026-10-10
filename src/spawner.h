#pragma once
#include "common.h"

bool ResolveSpawnApi(const Section& text, const Section& rdata);
// Sets capability `name` (sco/game/actors.h or sco/game/features.h) from its signature rows;
// false, after one "[tag] what disabled (row STATE; see the [core] lines)" line, unless all are OK.
bool ActorsCapability(const char* name, const char* tag, const char* what);
bool SpawnerReady();
void ReadStartOptions();
bool StartingOverDaymar();
bool PluginsEnabled();   // plugins = on (SC_OFFLINE_PLUGINS); read by ReadStartOptions
void ProcessShipMenu(DWORD now);
void ProcessCrew(DWORD now);   // seat actions, crew jobs, the seat list (the crew built-in's tick)

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
bool EntityAlive(uint64_t id);   // does the id resolve in the entity system now? (game thread; guarded)
bool FindEntityByNameEx(const char* name, uintptr_t& entity, uint64_t& id);   // entity pointer + id, or false
bool EntityClassExists(const char* name);   // is this a spawnable entity class? (call inside __try)
bool UnseatEntityById(uint64_t id);          // take an actor out of whatever seat it's in
