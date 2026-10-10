#pragma once
// titanlink: the Titanfall 2 bridge, an optional built-in (CMake SCO_BRIDGE_TITANLINK, off by
// default and never in the release zip). Shared between titanlink_plugin.cpp (the built-in: link,
// pilot mode, commands, tick), titanlink_ui.cpp (its tab) and titanlink_overlay.cpp (the window
// that shows Titanfall's picture over the game). docs/bridges.md describes what it does.
#include "sco_ipc.h"
#include "titanlink_wire.h"
#include <windows.h>
#include <cstdint>

// What the tab shows. Written by the tick, read by the tab's draw: both on the game thread.
struct TlStatus {
    bool     linkOpen = false;   // the channel exists (pilot mode was asked for at least once)
    bool     linked = false;     // the Titanfall side beat within the last 4 s
    bool     inMatch = false;
    bool     wantPilot = false;  // asked for; starts once Titanfall is in a match
    bool     pilot = false;
    bool     inTitan = false;
    bool     titanParked = false;
    bool     launched = false;   // Titanfall 2 was started from here this session
    uint32_t peerAgeMs = 0;
    double   titanDistance = -1; // metres, -1 when unknown
    float    titanHealth = 0;
    int32_t  clip = -1;
    char     weapon[48] = "";
    char     peerLog[TL_MSG_LOG_MAX + 1] = "";   // the last line the Titanfall side sent
    char     frameStatus[96] = "";
    char     game[MAX_PATH] = "";            // the Titanfall 2 folder (data\titanlink.txt)
    char     map[64] = "";
    char     mode[32] = "";
};

const TlStatus& TlGetStatus();

// The commands' work, for the tab's buttons (game thread). Each writes a reply line.
void TlTogglePilot(char* reply, uint32_t size);
void TlCallTitan(char* reply, uint32_t size);
void TlEmbark(char* reply, uint32_t size);
void TlUnlink(char* reply, uint32_t size);

// The overlay (titanlink_overlay.cpp): a click-through window of its own over the game's client
// area, on its own thread, drawing the newest picture the Titanfall side shares in tl_frame. It
// reads the frame block through sco.ipc itself (any thread), so it never touches the game.
void TlOverlayStart(const sco_ipc_v1* ipc, sco_plugin* self, uint64_t channel);
void TlOverlayStop();               // joins the thread; call before closing the channel
void TlOverlayShow(bool on);        // shown only while on and the game is in front
