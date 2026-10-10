#pragma once
// The mover behind spawn.entities 1.2's set_entity_transform (sc_spawn.h), for sc-offline's own
// code. Everything here is game thread only.
#include <cstdint>

// spawn_plugin.cpp. The player's own vehicle, retrieved or delivered by ATC: any plugin may move a
// registered id with set_entity_transform. ASOP registers it once that lands; nothing does yet.
// Registering an id twice or unregistering one that isn't registered changes nothing.
void RegisterPlayerVehicle(uint64_t entityId);
void UnregisterPlayerVehicle(uint64_t entityId);

// teleport_plugin.cpp, through teleport.spatial's zone tree (the conversions sc_spatial.h
// publishes). The id of the zone an entity is in, or 0.
uint64_t TeleportZoneOfEntity(uint64_t entityId);
// A pose (pos in metres, rot x y z w) in zone `from` to zone `to`; either may be 0, the world.
// Positions go through the zones' common ancestor. False when the teleport built-in can't answer
// or a zone can't be placed.
bool TeleportPoseToZone(uint64_t from, uint64_t to, const double pos[3], const double rot[4], double outPos[3],
                        double outRot[4]);
