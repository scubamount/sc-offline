#pragma once
// voxel_bridge: the voxel-game bridge, an optional built-in (CMake SCO_BRIDGE_VOXEL, off by default
// and never in the release zip). Shared between voxel_plugin.cpp (the built-in) and voxel_ui.cpp
// (its tab). docs/bridges.md describes what it does and the wire the voxel game's mod speaks.
#include "sc_voxel_bridge.h" // the channel layout, MIT, from sco-core's SDK
#include <cstddef>
#include <cstdint>

// What the tab shows. Written by the tick, read by the tab's draw: both on the game thread.
struct VxStatus {
    bool     linkOpen = false;
    bool     linked = false;      // the voxel game beat within the last 8 s
    bool     inWorld = false;     // and has a world loaded
    bool     anchored = false;    // a building area exists
    bool     paused = false;      // you're outside it (another zone, or far away)
    uint32_t peerAgeMs = 0;
    uint32_t epoch = 0;
    size_t   crates = 0;          // solid blocks shown as crates (spawned or waiting)
    size_t   pending = 0;         // of those, still to spawn
    uint32_t dropped = 0;         // solid blocks over the crate cap, not shown
    double   feet[3] = {};        // your feet in voxel coordinates
    char     zone[96] = "";       // the area's zone
    char     block[96] = "";      // the crate class (data\voxel_bridge.txt)
    double   blockSize = 0;
    char     peerStatus[64] = "";
    char     peerLog[VX_MSG_LOG_MAX + 1] = "";
};

const VxStatus& VxGetStatus();

// The commands' work, for the tab's buttons (game thread). Each writes a reply line.
void VxToggle(char* reply, uint32_t size);
void VxAnchor(char* reply, uint32_t size);
