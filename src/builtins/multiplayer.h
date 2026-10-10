#pragma once
// The multiplayer built-in (multiplayer_plugin.cpp) for its tab (multiplayer_ui.cpp). Game thread
// only, like the draw callback and every sco.net message callback.
#include <cstddef>
#include <cstdint>

// What the player sets in the tab, kept in data/storage/multiplayer.db. The passphrase is kept
// only when rememberPass is on (off by default); it is never logged.
struct MpSettings {
    char     name[65];          // shown to the other players: 1-64 bytes
    int      hostPort;          // 1024..65535
    char     joinAddress[64];   // numeric IPv4
    int      joinPort;
    char     avatarClass[64];   // how others see you, when their npcs.txt has it
    bool     showShips;         // spawn other players' ships
    bool     rememberPass;
    char     passphrase[129];
};

MpSettings& Mp_Settings();
void        Mp_SaveSettings();

// Host / Join with the settings; nullptr when the session is starting, else why not.
const char* Mp_Host();
const char* Mp_Join();
void        Mp_Leave();

bool        Mp_Enabled();        // multiplayer = on (sc-offline.ini) and the sco.net service is there
bool        Mp_InSession();      // hosting, joining or joined
bool        Mp_GhostsReady();    // spawn.entities 1.2 and teleport.spatial: ghosts can be shown
const char* Mp_StateText();      // "Not in a session", "Hosting on UDP port 64091", ...
const char* Mp_LastReason();     // why the last session ended, or ""
const char* Mp_AllowText();      // the extra allowed networks (multiplayer_allow), or ""

struct MpPeerView {
    uint64_t    peer;
    const char* name;
    bool        self;
    const char* avatar;          // the ghost's state, for the list
    const char* ship;            // "" with no ship
    uint64_t    sessionId;       // the session-scoped id of this player's avatar
};
int  Mp_PeerCount();
bool Mp_Peer(int index, MpPeerView& out);
// Teleports you next to that player's ghost; nullptr on success, else why not.
const char* Mp_GoTo(uint64_t peer);
