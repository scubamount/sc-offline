#pragma once
#include "common.h"

bool ResolveSpawnApi(const Section& text, const Section& rdata);
bool SpawnerReady();
void ReadStartOptions();
bool StartingOverDaymar();
void ProcessShipMenu(DWORD now);
const char* SpawnEntityNearPlayer(const char* entityClass, const double offset[3], uint64_t& id);
uintptr_t EntityComponent(uintptr_t entity, const char* type);
const char* SpawnEntityInPlayerZone(const char* entityClass, const double pos[3], const double rot[4], uint64_t& id);
const char* SpawnEntityInZone(const char* entityClass, uint64_t zoneId, const double pos[3], const double rot[4], uint64_t& id);
const char* SpawnPrefabInPlayerZone(const char* ocPath, const double pos[3], const double rot[4], uint64_t& id);
bool PrefabsReady();
uint64_t LocalPlayerEntityId();
uint64_t EntityIdOfComponent(uintptr_t component);
uint64_t EntityIdOfHandle(const void* handle);
