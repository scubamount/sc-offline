// multiplayer: private co-presence between players who all run sc-offline (sco-core's
// docs/design/multiplayer.md, section 4.1).
//
// Each player runs their own offline game. One of them hosts a session from the Multiplayer tab and
// the others join it by address; sco-core's sco.net carries this built-in's three channels
// (multiplayer_wire.h has the formats):
//   multiplayer.pose     unreliable, ~10 per second: where your player (and the ship you're aboard)
//                        is, as zone NAMES with positions local to each, never zone ids
//   multiplayer.spawn    reliable: what you are (the NPC class others may show you as, your ship class)
//   multiplayer.despawn  reliable: that thing is gone
// Other players appear as ghosts: entities this built-in spawns in YOUR game through spawn.entities
// 1.2 (spawn_as), waits for (entity_alive), and moves every tick (set_entity_transform), between
// the last two poses. A peer's zone chain is matched by name against your own zone chain; when none
// of its zones is one you're in, the ghost is parked out of sight and the tab says so. Ghosts are
// visual only: no shared physics, damage, missions or inventory.
//
// Nothing networks until the player presses Host or Join (sco::net only binds a socket then), and
// sco.net keeps every session to the LAN plus the explicit multiplayer_allow ranges. The game's own
// netcode, servers and connection handling are never touched. With `multiplayer = off` in
// sc-offline.ini the built-in loads and does nothing. No other feature depends on it.
#include "builtins.h"
#include "builtin_store.h"
#include "multiplayer.h"
#include "multiplayer_wire.h"
#include "mover.h"
#include "tabs.h"
#include <sc_spatial.h>
#include <sc_spawn.h>
#include <sco_net.h>
#include "sco/net/session.h"
#include "../menu.h"
#include "../npc.h"
#include "../teleport.h"
#include "../version.h"
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using mpwire::kAvatar;
using mpwire::kShip;

const sco_api*              g_api = nullptr;    // from MpLoad until MpUnload
sco_plugin*                 g_self = nullptr;
const sco_net_v1*           g_net = nullptr;    // null: multiplayer = off, or no sco.net
const sc_spawn_service_v1*  g_spawn = nullptr;  // null without spawn.entities 1.2: no ghosts
const sc_spatial_v1*        g_sp = nullptr;     // null without teleport.spatial: no poses, no ghosts
BuiltinStore                g_store{ "multiplayer" };

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "multiplayer", SCO_VERSION, "sc-offline",
};

constexpr const char* kPoseChannel = "multiplayer.pose";
constexpr const char* kSpawnChannel = "multiplayer.spawn";
constexpr const char* kDespawnChannel = "multiplayer.despawn";
// An unarmed civilian: a ghost that never draws a weapon. Any human class in npcs.txt works.
constexpr const char* kDefaultAvatar = "PU_Human-Microtech-DeskWorker-Male-DistroHub_Civilian_01";
constexpr uint64_t kPoseEveryMs = 100;      // ~10 Hz
constexpr uint64_t kInterpDelayMs = 100;    // ghosts show where the peer was this long ago
constexpr uint64_t kPoseTimeoutMs = 10000;  // no pose this long: the ghosts go
constexpr uint64_t kStreamTimeoutMs = 60000;
constexpr uint64_t kRetryMs = 30000;        // after a failed spawn, or a ghost that streamed out
constexpr double   kShowWithinM = 10000.0;  // farther ghosts would only stream out again: not shown
constexpr int      kChainMax = 16;
constexpr size_t   kMaxRemotePeers = SCO_NET_MAX_PEERS - 1;

MpSettings g_set;
std::vector<sco::net::Cidr> g_allow;
std::string g_allowText;
char g_stateText[96] = "Not in a session";
char g_reason[160] = "";

// ---- settings -----------------------------------------------------------------------------------

void CopyText(char* dst, size_t cap, const std::string& s) {
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    memcpy(dst, s.data(), n);
    dst[n] = 0;
}

int ReadPort(const char* key, int fallback) {
    int64_t v = 0;
    return g_store && g_store.GetInt(key, v) == SCO_OK && v >= 1024 && v <= 65535 ? static_cast<int>(v) : fallback;
}

void LoadSettings() {
    g_set = MpSettings{};
    strcpy_s(g_set.name, "Pilot");
    g_set.hostPort = sco::net::kDefaultPort;
    g_set.joinPort = sco::net::kDefaultPort;
    strcpy_s(g_set.avatarClass, kDefaultAvatar);
    g_set.showShips = true;
    if (!g_store) return;
    std::string s;
    if (g_store.S().Get("name", s) == SCO_OK && !s.empty()) CopyText(g_set.name, sizeof(g_set.name), s);
    if (g_store.S().Get("join_address", s) == SCO_OK) CopyText(g_set.joinAddress, sizeof(g_set.joinAddress), s);
    if (g_store.S().Get("avatar_class", s) == SCO_OK && mpwire::ClassOk(s.c_str(), s.size()))
        CopyText(g_set.avatarClass, sizeof(g_set.avatarClass), s);
    g_set.hostPort = ReadPort("host_port", g_set.hostPort);
    g_set.joinPort = ReadPort("join_port", g_set.joinPort);
    int64_t v = 0;
    if (g_store.GetInt("show_ships", v) == SCO_OK) g_set.showShips = v != 0;
    if (g_store.GetInt("remember_passphrase", v) == SCO_OK) g_set.rememberPass = v != 0;
    if (g_set.rememberPass && g_store.S().Get("passphrase", s) == SCO_OK)
        CopyText(g_set.passphrase, sizeof(g_set.passphrase), s);
}

// ---- the session's other players ----------------------------------------------------------------

enum class GhostState : uint8_t { None, Waiting, Streaming, Alive, Parked, Failed };

struct Ghost {
    GhostState st;
    uint64_t   id;        // the local entity, 0 with none
    uint64_t   since;     // when st was entered (Streaming, Failed)
    char       cls[64];
    char       note[128];
};

struct Sample {
    bool          valid;
    uint64_t      at;     // when it arrived (local ms)
    mpwire::Pose  p;
};

struct Peer {
    uint64_t peer;
    char     name[SCO_NET_MAX_NAME + 1];
    char     avatarCls[64];   // as announced; "" before an announcement
    char     shipCls[64];     // as announced; "" with no ship
    bool     avatarGone;      // they announced their avatar is gone
    Sample   s0, s1;          // the last two poses, s1 the newest
    Ghost    avatar, ship;
};

std::vector<Peer> g_peers;
uint32_t g_seq = 0;
uint64_t g_lastPoseSent = 0;
uint64_t g_shipId = 0;              // the ship you're aboard, as last seen
char     g_shipCls[64] = "";        // its class, "" when unknown
char     g_announcedShip[64] = "";  // the ship class peers were last told about
char     g_announcedAvatar[64] = "";

uint64_t NowMs() { return GetTickCount64(); }

Peer* FindPeer(uint64_t id) {
    for (Peer& p : g_peers)
        if (p.peer == id) return &p;
    return nullptr;
}

Peer* FindOrAddPeer(uint64_t id) {
    if (Peer* p = FindPeer(id)) return p;
    if (!id || id == g_net->self_peer() || g_peers.size() >= kMaxRemotePeers) return nullptr;
    g_peers.emplace_back();
    Peer& p = g_peers.back();
    p.peer = id;
    if (g_net->get_peer_name(id, p.name, sizeof(p.name)) != SCO_OK) snprintf(p.name, sizeof(p.name), "player %llu", static_cast<unsigned long long>(id));
    return &p;
}

void Note(Ghost& g, const char* fmt, const char* arg = "") { snprintf(g.note, sizeof(g.note), fmt, arg); }

void RemoveGhost(Peer& p, Ghost& g, bool ship, const char* why) {
    if (g.id) {
        RemoveEntityById(g.id);
        Log("[multiplayer] removed %s's %s ghost %llu (%s)", p.name, ship ? "ship" : "avatar",
            static_cast<unsigned long long>(g.id), why);
        if (!ship && g_net->is_active()) sco::net::SetPeerEntity(p.peer, 0);
    }
    g.id = 0;
    g.st = GhostState::None;
    g.cls[0] = 0;
}

void RemovePeerGhosts(Peer& p, const char* why) {
    RemoveGhost(p, p.avatar, false, why);
    RemoveGhost(p, p.ship, true, why);
}

void RemoveAll(const char* why) {
    for (Peer& p : g_peers) RemovePeerGhosts(p, why);
    g_peers.clear();
}

// ---- zones --------------------------------------------------------------------------------------

// Game reads under SEH into plain structs (MSVC C2712: nothing with a destructor in this frame).
int ChainOfZone(uint64_t zoneId, ZoneFrame* out, int max) {
    __try {
        const uintptr_t zone = zoneId ? ZoneFromId(zoneId) : 0;
        return zone ? ReadZoneChain(zone, out, max) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct LocalChain {
    int       n;
    ZoneFrame z[kChainMax];
    uint64_t  zone;     // the zone you're in, and where you are in it
    double    pos[3];
};

bool ReadLocalChain(LocalChain& c) {
    c.n = 0;
    double rot[4];
    if (!g_sp || !g_sp->player_pose(c.pos, rot, &c.zone)) return false;
    c.n = ChainOfZone(c.zone, c.z, kChainMax);
    return c.n > 0;
}

// How far a pose in zone "zone" is from you, in metres; 0 when it can't be told.
double DistanceFromYou(const LocalChain& c, uint64_t zone, const double pos[3]) {
    double mine[3];
    if (!c.n || !g_sp->zone_to_zone(c.zone, zone, c.pos, mine)) return 0;
    const double d[3] = { pos[0] - mine[0], pos[1] - mine[1], pos[2] - mine[2] };
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// The innermost of a pose's levels (from `first`) that names a zone of your own chain: its index,
// with that zone's local id in *zone; -1 when none does.
int Resolve(const mpwire::Pose& p, int first, const LocalChain& c, uint64_t* zone) {
    for (int i = first; i < p.levels; ++i)
        for (int k = 0; k < c.n; ++k)
            if (strcmp(p.lv[i].name, c.z[k].name) == 0) { *zone = c.z[k].id; return i; }
    return -1;
}

int LevelNamed(const mpwire::Pose& p, int first, const char* name) {
    for (int i = first; i < p.levels; ++i)
        if (strcmp(p.lv[i].name, name) == 0) return i;
    return -1;
}

// Where a peer's avatar (or ship) is in your game, between its last two poses: true with the zone
// and the pose in it.
bool GhostPose(const Peer& p, bool ship, const LocalChain& c, uint64_t now, uint64_t* zone, double pos[3], double rot[4]) {
    const mpwire::Pose& b = p.s1.p;
    const int first = ship ? b.shipFirst : 0;
    const int lb = Resolve(b, first, c, zone);
    if (lb < 0) return false;
    const double* bp = ship ? b.shipPos[lb] : b.lv[lb].pos;
    const double* br = ship ? b.shipRot[lb] : b.lv[lb].rot;
    memcpy(pos, bp, 3 * sizeof(double));
    memcpy(rot, br, 4 * sizeof(double));
    if (!p.s0.valid || p.s1.at <= p.s0.at) return true;
    const mpwire::Pose& a = p.s0.p;
    if (ship && !a.ship) return true;
    const int la = LevelNamed(a, ship ? a.shipFirst : 0, b.lv[lb].name);
    if (la < 0) return true;
    const double* ap = ship ? a.shipPos[la] : a.lv[la].pos;
    const double* ar = ship ? a.shipRot[la] : a.lv[la].rot;
    const uint64_t at = now > kInterpDelayMs ? now - kInterpDelayMs : 0;
    double t = at <= p.s0.at ? 0.0 : static_cast<double>(at - p.s0.at) / static_cast<double>(p.s1.at - p.s0.at);
    if (t > 1.0) t = 1.0;
    for (int i = 0; i < 3; ++i) pos[i] = ap[i] + (bp[i] - ap[i]) * t;
    double dot = 0;
    for (int i = 0; i < 4; ++i) dot += ar[i] * br[i];
    const double s = dot < 0 ? -1.0 : 1.0;   // the short way round
    double n = 0;
    for (int i = 0; i < 4; ++i) { rot[i] = ar[i] * s * (1.0 - t) + br[i] * t; n += rot[i] * rot[i]; }
    n = std::sqrt(n);
    if (n < 1e-9) { memcpy(rot, br, 4 * sizeof(double)); return true; }
    for (int i = 0; i < 4; ++i) rot[i] /= n;
    return true;
}

// The outermost zone name of the peer's last pose ("Stanton"-level), for the tab.
const char* WhereText(const Peer& p) { return p.s1.valid ? p.s1.p.lv[p.s1.p.levels - 1].name : "?"; }

// ---- ghosts -------------------------------------------------------------------------------------

bool InNpcList(const char* cls) {
    const int n = Menu_NpcCount();
    for (int i = 0; i < n; ++i)
        if (_stricmp(Menu_NpcName(i), cls) == 0) return true;
    return false;
}

// The class you see this player as: theirs when your npcs.txt lists it, else your own setting.
const char* AvatarClassFor(const Peer& p) {
    return p.avatarCls[0] && InNpcList(p.avatarCls) ? p.avatarCls : g_set.avatarClass;
}

void Park(Ghost& g) {
    uint64_t zone = 0;
    static const double kAway[3] = { 1.0e7, 1.0e7, 1.0e7 };   // ~17,000 km off, as npc.cpp's Banish
    static const double kIdentity[4] = { 0, 0, 0, 1 };
    if (g_sp->zone_of_entity(g.id, &zone)) g_spawn->set_entity_transform(g_self, g.id, zone, kAway, kIdentity);
}

void UpdateGhost(Peer& p, bool ship, const LocalChain& c, uint64_t now) {
    Ghost& g = ship ? p.ship : p.avatar;
    const bool aboard = p.s1.valid && p.s1.p.ship;
    bool wanted;
    if (ship) wanted = aboard && g_set.showShips && p.shipCls[0];
    else wanted = p.s1.valid && !p.avatarGone && !(aboard && p.ship.st == GhostState::Alive);
    if (!wanted) {
        if (g.id) RemoveGhost(p, g, ship, ship ? "no ship" : "aboard their ship");
        g.st = GhostState::None;
        Note(g, ship ? "" : (aboard ? "aboard their ship" : (p.avatarGone ? "gone" : "waiting for a position")));
        return;
    }
    const char* cls = ship ? p.shipCls : AvatarClassFor(p);
    if (g.id && strcmp(g.cls, cls) != 0) RemoveGhost(p, g, ship, "class changed");
    uint64_t zone = 0;
    double pos[3], rot[4];
    const bool placed = GhostPose(p, ship, c, now, &zone, pos, rot);
    const double distance = placed ? DistanceFromYou(c, zone, pos) : 0;
    char farNote[96];
    snprintf(farNote, sizeof(farNote), "not shown: %.0f km away", distance / 1000.0);
    switch (g.st) {
        case GhostState::Failed:
            if (now - g.since < kRetryMs) return;
            [[fallthrough]];
        case GhostState::None:
        case GhostState::Waiting: {
            if (!placed) {
                g.st = GhostState::Waiting;
                Note(g, "not shown: they're in a zone you haven't loaded (%s)", WhereText(p));
                return;
            }
            if (distance > kShowWithinM) {
                g.st = GhostState::Waiting;
                Note(g, "%s", farNote);
                return;
            }
            if (!g_spawn->class_exists(cls)) {
                g.st = GhostState::Failed;
                g.since = now;
                Note(g, "'%s' isn't a spawnable class on this game build", cls);
                return;
            }
            const double offset[3] = { 0, 0, ship ? 100.0 : 2.0 };
            uint64_t id = 0;
            const char* err = g_spawn->spawn_as(g_self, cls, offset, &id);
            if (err) {
                g.st = GhostState::Failed;
                g.since = now;
                Note(g, "couldn't spawn: %s", err);
                return;
            }
            g.id = id;
            strcpy_s(g.cls, cls);
            g.st = GhostState::Streaming;
            g.since = now;
            Note(g, "streaming in");
            Log("[multiplayer] spawned %s's %s ghost %llu (%s)", p.name, ship ? "ship" : "avatar",
                static_cast<unsigned long long>(id), cls);
            return;
        }
        case GhostState::Streaming:
            if (!g_spawn->entity_alive(g.id)) {
                if (now - g.since > kStreamTimeoutMs) {
                    RemoveGhost(p, g, ship, "didn't stream in");
                    g.st = GhostState::Failed;
                    g.since = now;
                    Note(g, "didn't stream in within 60 s; trying again later");
                }
                return;
            }
            g.st = GhostState::Alive;
            if (!ship) sco::net::SetPeerEntity(p.peer, mpwire::SessionId(p.peer, kAvatar));
            [[fallthrough]];
        case GhostState::Alive:
        case GhostState::Parked:
            if (!g_spawn->entity_alive(g.id)) {   // streamed out (a parked ghost may), or the game removed it
                g.id = 0;
                g.st = GhostState::Failed;          // waits kRetryMs before spawning again
                g.since = now;
                if (!ship) sco::net::SetPeerEntity(p.peer, 0);
                Note(g, "streamed out; shown again within 30 s when they're near");
                return;
            }
            if (distance > kShowWithinM) {
                RemoveGhost(p, g, ship, "far away");
                g.st = GhostState::Waiting;
                Note(g, "%s", farNote);
                return;
            }
            if (placed) {
                if (g_spawn->set_entity_transform(g_self, g.id, zone, pos, rot)) {
                    g.st = GhostState::Alive;
                    Note(g, "shown");
                } else {
                    Note(g, "the game refused the move (mod.log says why)");
                }
            } else {
                if (g.st != GhostState::Parked) Park(g);
                g.st = GhostState::Parked;
                Note(g, "parked: they're in a zone you haven't loaded (%s)", WhereText(p));
            }
            return;
    }
}

// ---- sending ------------------------------------------------------------------------------------

void Send(const char* channel, const void* buf, size_t len, const char* what) {
    const sco_result r = g_net->send_channel(g_self, channel, buf, static_cast<uint32_t>(len));
    if (r != SCO_OK && r != SCO_UNAVAILABLE) Log("[multiplayer] %s not sent (result %d)", what, static_cast<int>(r));
}

bool SendSpawn(uint8_t kind, const char* cls) {
    mpwire::Spawn s{};
    s.kind = kind;
    s.sessionId = mpwire::SessionId(g_net->self_peer(), kind);
    strcpy_s(s.cls, cls);
    uint8_t buf[mpwire::kMaxSpawn];
    const size_t n = mpwire::EncodeSpawn(s, buf, sizeof(buf));
    return n && g_net->send_channel(g_self, kSpawnChannel, buf, static_cast<uint32_t>(n)) == SCO_OK;
}

bool SendDespawn(uint8_t kind) {
    uint8_t buf[mpwire::kDespawn];
    const size_t n = mpwire::EncodeDespawn(kind, mpwire::SessionId(g_net->self_peer(), kind), buf, sizeof(buf));
    return n && g_net->send_channel(g_self, kDespawnChannel, buf, static_cast<uint32_t>(n)) == SCO_OK;
}

// Tells the peers what you are, when it changed (or force: a peer joined). A refused send is tried
// again next tick.
void Announce(bool force) {
    if (force || strcmp(g_announcedAvatar, g_set.avatarClass) != 0) {
        if (SendSpawn(kAvatar, g_set.avatarClass)) strcpy_s(g_announcedAvatar, g_set.avatarClass);
        else g_announcedAvatar[0] = 0;
    }
    if (!force && strcmp(g_announcedShip, g_shipCls) == 0) return;
    if (g_shipCls[0]) {
        if (SendSpawn(kShip, g_shipCls)) strcpy_s(g_announcedShip, g_shipCls);
    } else if (g_announcedShip[0] || force) {
        if (SendDespawn(kShip)) g_announcedShip[0] = 0;
    }
}

// The ship's class from its zone's name: the name itself, or with trailing "_<digits>" cut off,
// whichever is a spawnable class. "" when neither is.
void ShipClassOf(const char* zoneName, char* out, size_t cap) {
    char buf[96];
    strcpy_s(buf, zoneName);
    for (int tries = 0; tries < 4; ++tries) {
        if (mpwire::ClassOk(buf, strlen(buf)) && g_spawn->class_exists(buf)) { strcpy_s(out, cap, buf); return; }
        char* cut = strrchr(buf, '_');
        if (!cut || !cut[1]) break;
        for (const char* d = cut + 1; *d; ++d)
            if (*d < '0' || *d > '9') { out[0] = 0; return; }
        *cut = 0;
    }
    out[0] = 0;
}

// Your pose in every zone of your chain (innermost first), and your ship's when you're aboard one.
bool BuildPose(mpwire::Pose& p) {
    double pos[3], rot[4];
    uint64_t zone = 0;
    if (!g_sp->player_pose(pos, rot, &zone)) return false;
    ZoneFrame chain[kChainMax];
    const int n = ChainOfZone(zone, chain, kChainMax);
    const uint64_t ship = g_spawn ? g_spawn->player_ship_id() : 0;
    if (ship != g_shipId) {
        g_shipId = ship;
        g_shipCls[0] = 0;
        for (int k = 0; ship && g_spawn && k < n; ++k)
            if (chain[k].id == ship) ShipClassOf(chain[k].name, g_shipCls, sizeof(g_shipCls));
        if (ship) Log("[multiplayer] aboard ship %llu: %s", static_cast<unsigned long long>(ship), g_shipCls[0] ? g_shipCls : "class not found, not shown to others");
    }
    p = mpwire::Pose{};
    p.seq = ++g_seq;
    int shipLevel = -1;
    bool shipOk = ship && g_shipCls[0];
    static const double kOrigin[3] = { 0, 0, 0 };
    static const double kIdentity[4] = { 0, 0, 0, 1 };
    for (int k = 0; k < n && p.levels < mpwire::kMaxLevels; ++k) {
        const size_t len = strlen(chain[k].name);
        if (!mpwire::ZoneNameOk(chain[k].name, len)) continue;
        mpwire::Level& l = p.lv[p.levels];
        if (!TeleportPoseToZone(zone, chain[k].id, pos, rot, l.pos, l.rot)) continue;
        memcpy(l.name, chain[k].name, len + 1);
        if (shipOk && chain[k].id == ship) shipLevel = p.levels;
        else if (shipOk && shipLevel >= 0 &&
                 !TeleportPoseToZone(ship, chain[k].id, kOrigin, kIdentity, p.shipPos[p.levels], p.shipRot[p.levels]))
            shipOk = false;
        ++p.levels;
    }
    if (!p.levels) return false;
    p.ship = shipOk && shipLevel >= 0 && shipLevel + 1 < p.levels;
    p.shipFirst = p.ship ? static_cast<uint8_t>(shipLevel + 1) : 0;
    return true;
}

void SendPose(uint64_t now) {
    if (now - g_lastPoseSent < kPoseEveryMs) return;
    g_lastPoseSent = now;
    mpwire::Pose p;
    if (!BuildPose(p)) return;
    uint8_t buf[mpwire::kMaxPose];
    const size_t n = mpwire::EncodePose(p, buf, sizeof(buf));
    if (n) Send(kPoseChannel, buf, n, "pose");
}

// ---- receiving (game thread, from the host tick) ------------------------------------------------

void OnPose(uint64_t from, const void* buf, uint32_t len, void*) {
    mpwire::Pose p;
    if (!mpwire::DecodePose(buf, len, p)) return;   // malformed: dropped
    Peer* peer = FindOrAddPeer(from);
    if (!peer) return;
    if (peer->s1.valid && static_cast<int32_t>(p.seq - peer->s1.p.seq) <= 0) return;   // late or repeated
    peer->s0 = peer->s1;
    peer->s1.valid = true;
    peer->s1.at = NowMs();
    peer->s1.p = p;
}

void OnSpawn(uint64_t from, const void* buf, uint32_t len, void*) {
    mpwire::Spawn s;
    if (!mpwire::DecodeSpawn(buf, len, from, s)) return;
    Peer* peer = FindOrAddPeer(from);
    if (!peer) return;
    if (s.kind == kAvatar) {
        strcpy_s(peer->avatarCls, s.cls);
        peer->avatarGone = false;
    } else {
        strcpy_s(peer->shipCls, s.cls);
    }
}

void OnDespawn(uint64_t from, const void* buf, uint32_t len, void*) {
    uint8_t kind = 0;
    if (!mpwire::DecodeDespawn(buf, len, from, kind)) return;
    Peer* peer = FindPeer(from);
    if (!peer) return;
    if (kind == kAvatar) {
        peer->avatarGone = true;
        RemoveGhost(*peer, peer->avatar, false, "they left the world");
    } else {
        peer->shipCls[0] = 0;
        RemoveGhost(*peer, peer->ship, true, "they left their ship");
    }
}

void OnNetState(const char*, const void* data, void*) {
    const auto* e = static_cast<const sco_net_state_event*>(data);
    if (!e || e->size < sizeof(sco_net_state_event)) return;
    if (e->active) {
        g_reason[0] = 0;
        g_announcedAvatar[0] = 0;
        g_announcedShip[0] = 0;
        sco::net::SetPeerEntity(g_net->self_peer(), mpwire::SessionId(g_net->self_peer(), kAvatar));
        Announce(true);
        Log("[multiplayer] session up: you are player %llu", static_cast<unsigned long long>(g_net->self_peer()));
    } else {
        strncpy_s(g_reason, e->reason, _TRUNCATE);
        RemoveAll("session ended");
        Log("[multiplayer] session ended: %s", g_reason[0] ? g_reason : "left");
    }
}

void OnNetPeer(const char*, const void* data, void*) {
    const auto* e = static_cast<const sco_net_peer_event*>(data);
    if (!e || e->size < sizeof(sco_net_peer_event)) return;
    const uint64_t id = e->peer.peer_id;
    if (e->what == SCO_NET_PEER_JOINED) {
        if (Peer* p = FindOrAddPeer(id)) Log("[multiplayer] %s joined", p->name);
        Announce(true);   // so the newcomer learns what you are
    } else if (e->what == SCO_NET_PEER_LEFT) {
        for (size_t i = 0; i < g_peers.size(); ++i)
            if (g_peers[i].peer == id) {
                Log("[multiplayer] %s left", g_peers[i].name);
                RemovePeerGhosts(g_peers[i], "left the session");
                g_peers.erase(g_peers.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
    }
}

// ---- the tick -----------------------------------------------------------------------------------

void UpdateStateText() {
    switch (sco::net::GetState()) {
        case sco::net::State::Idle: strcpy_s(g_stateText, "Not in a session"); break;
        case sco::net::State::Joining: strcpy_s(g_stateText, "Joining..."); break;
        case sco::net::State::Joined:
            snprintf(g_stateText, sizeof(g_stateText), "Joined as player %llu", static_cast<unsigned long long>(g_net->self_peer()));
            break;
        case sco::net::State::Hosting:
            snprintf(g_stateText, sizeof(g_stateText), "Hosting on UDP port %u", static_cast<unsigned>(sco::net::BoundPort()));
            break;
    }
}

void OnTick(const char*, const void*, void*) {
    UpdateStateText();
    if (!g_net->is_active()) {
        if (!g_peers.empty()) RemoveAll("no session");
        return;
    }
    const uint64_t now = NowMs();
    if (!g_sp || !g_spawn) return;   // the session runs (peer list) but there are no poses or ghosts
    SendPose(now);
    Announce(false);
    if (!g_spawn->local_player_id()) return;   // not in the world yet: nothing to place ghosts by
    LocalChain c;
    ReadLocalChain(c);
    for (Peer& p : g_peers) {
        if (p.s1.valid && now - p.s1.at > kPoseTimeoutMs) {
            RemovePeerGhosts(p, "no position for 10 s");
            p.s0.valid = p.s1.valid = false;
            Note(p.avatar, "no position for 10 s");
            continue;
        }
        UpdateGhost(p, true, c, now);    // the ship first: the avatar hides while its ship is shown
        UpdateGhost(p, false, c, now);
    }
}

// ---- commands -----------------------------------------------------------------------------------

sco_result CmdStatus(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    snprintf(reply, size, "%s; %u other player(s)%s%s", g_stateText, static_cast<unsigned>(g_peers.size()),
             g_reason[0] ? "; last ended: " : "", g_reason);
    return SCO_OK;
}

sco_result CmdLeave(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    if (!g_net->is_active() && sco::net::GetState() == sco::net::State::Idle) {
        snprintf(reply, size, "Not in a session");
        return SCO_UNAVAILABLE;
    }
    Mp_Leave();
    snprintf(reply, size, "Left the session");
    return SCO_OK;
}

sco_result CmdGoTo(const sco_arg* args, uint32_t, void*, char* reply, uint32_t size) {
    const char* who = args[0].v.s;
    for (const Peer& p : g_peers)
        if (who && _stricmp(p.name, who) == 0) {
            const char* err = Mp_GoTo(p.peer);
            snprintf(reply, size, err ? "%s" : "Teleported next to %s", err ? err : p.name);
            return err ? SCO_FAILED : SCO_OK;
        }
    snprintf(reply, size, "No player named '%s' in the session", who ? who : "");
    return SCO_FAILED;
}

// ---- load / unload ------------------------------------------------------------------------------

bool IniOn() {
    char v[16];
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_MULTIPLAYER", v, sizeof(v));
    return !(n > 0 && n < sizeof(v) && _stricmp(v, "off") == 0);   // unset (an older launcher): on
}

// multiplayer_allow from sc-offline.ini (the launcher passes it checked): extra CIDRs a session may
// reach beside the LAN, for the VPN the player chose. Anything that doesn't parse is ignored.
void ReadAllowList() {
    g_allow.clear();
    g_allowText.clear();
    char v[512];
    const DWORD n = GetEnvironmentVariableA("SC_OFFLINE_MULTIPLAYER_ALLOW", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return;
    for (char* save = nullptr, *tok = strtok_s(v, ", ", &save); tok; tok = strtok_s(nullptr, ", ", &save)) {
        sco::net::Cidr c;
        if (!sco::net::ParseCidr(tok, &c) || c.family != sco::net::Endpoint::kIpv4 || c.bits < 8 || g_allow.size() >= 8) {
            Log("[multiplayer] multiplayer_allow: '%s' ignored (an IPv4 range like 100.64.0.0/10, /8 or narrower)", tok);
            continue;
        }
        g_allow.push_back(c);
        if (!g_allowText.empty()) g_allowText += ", ";
        g_allowText += tok;
    }
}

template <typename T>
const T* Service(const char* name, uint32_t version) {
    const void* t = nullptr;
    if (g_api->size <= offsetof(sco_api, query_service) || g_api->query_service(name, version, &t) != SCO_OK) return nullptr;
    return static_cast<const T*>(t);
}

const sco_plugin_info* MpQuery() { return &kInfo; }

sco_result MpLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    if (!IniOn()) {
        Log("[multiplayer] off (multiplayer = off in sc-offline.ini): no tab, nothing networks");
        return SCO_OK;
    }
    g_net = Service<sco_net_v1>(SCO_NET_NAME, SCO_NET_VERSION_1_0);
    if (!g_net) {
        Log("[multiplayer] sco.net isn't available; multiplayer is off");
        return SCO_OK;
    }
    g_spawn = Service<sc_spawn_service_v1>(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION);
    if (g_spawn && g_spawn->size <= offsetof(sc_spawn_service_v1, spawn_as)) g_spawn = nullptr;
    g_sp = Service<sc_spatial_v1>(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION);
    if (!g_spawn || !g_sp)
        Log("[multiplayer] %s isn't available: sessions work, but nobody's position is sent or shown",
            !g_spawn ? "spawn.entities 1.2" : "teleport.spatial");
    g_store.Open(api, self);   // data/storage/multiplayer.db: the tab's settings
    LoadSettings();
    ReadAllowList();
    sco_result r = g_net->register_channel(self, kPoseChannel, 0, static_cast<uint32_t>(mpwire::kMaxPose), OnPose, nullptr);
    if (r == SCO_OK) r = g_net->register_channel(self, kSpawnChannel, SCO_NET_RELIABLE, static_cast<uint32_t>(mpwire::kMaxSpawn), OnSpawn, nullptr);
    if (r == SCO_OK) r = g_net->register_channel(self, kDespawnChannel, SCO_NET_RELIABLE, static_cast<uint32_t>(mpwire::kDespawn), OnDespawn, nullptr);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r == SCO_OK) r = api->subscribe(self, SCO_NET_EVENT_STATE, OnNetState, nullptr);
    if (r == SCO_OK) r = api->subscribe(self, SCO_NET_EVENT_PEER, OnNetPeer, nullptr);
    const sco_arg_def who = BuiltinArg("player", SCO_ARG_STRING, "A player's name, as the Multiplayer tab lists it");
    if (r == SCO_OK)
        r = RegisterBuiltinCommand(api, self, "sco.net", "multiplayer.status", "Multiplayer status",
                                   "The session's state and how many other players are in it", CmdStatus);
    if (r == SCO_OK)
        r = RegisterBuiltinCommand(api, self, "sco.net", "multiplayer.leave", "Leave the session",
                                   "Leaves (or stops hosting) the multiplayer session", CmdLeave);
    if (r == SCO_OK)
        r = RegisterBuiltinCommand(api, self, "sco.net", "multiplayer.goto", "Go to player",
                                   "Teleports you next to another player's ghost", CmdGoTo, &who, 1);
    if (r != SCO_OK) {   // the host releases what was registered
        g_store.Close();
        g_net = nullptr;
        g_spawn = nullptr;
        g_sp = nullptr;
        g_api = nullptr;
        g_self = nullptr;
        return r;
    }
    RegisterBuiltinTab(api, self, "multiplayer.session", "Multiplayer", kTabMultiplayer, DrawMultiplayerTab);
    return SCO_OK;
}

void MpUnload() {
    if (g_net) {
        if (sco::net::GetState() != sco::net::State::Idle) sco::net::Leave("the multiplayer built-in unloaded");
        RemoveAll("unloading");
    }
    g_store.Close();
    g_net = nullptr;
    g_spawn = nullptr;
    g_sp = nullptr;
    g_api = nullptr;
    g_self = nullptr;
}

}  // namespace

// ---- multiplayer.h, for the tab -----------------------------------------------------------------

MpSettings& Mp_Settings() { return g_set; }

void Mp_SaveSettings() {
    if (!g_store) return;
    sco::sdk::Storage& s = g_store.S();
    sco::sdk::StorageTransaction tx(s);
    sco_result r = tx.Result();
    if (r == SCO_OK) r = s.Put("name", std::string_view(g_set.name));
    if (r == SCO_OK) r = s.Put("join_address", std::string_view(g_set.joinAddress));
    if (r == SCO_OK && mpwire::ClassOk(g_set.avatarClass, strlen(g_set.avatarClass)))
        r = s.Put("avatar_class", std::string_view(g_set.avatarClass));
    if (r == SCO_OK) r = g_store.PutInt("host_port", g_set.hostPort);
    if (r == SCO_OK) r = g_store.PutInt("join_port", g_set.joinPort);
    if (r == SCO_OK) r = g_store.PutInt("show_ships", g_set.showShips ? 1 : 0);
    if (r == SCO_OK) r = g_store.PutInt("remember_passphrase", g_set.rememberPass ? 1 : 0);
    if (r == SCO_OK) {
        if (g_set.rememberPass) r = s.Put("passphrase", std::string_view(g_set.passphrase));
        else if (s.Delete("passphrase") == SCO_NOT_FOUND) r = SCO_OK;
    }
    if (r == SCO_OK) r = tx.Commit();
    if (r != SCO_OK) g_store.Failed("saving the settings", r, "they last until the game closes");
}

bool Mp_Enabled() { return g_net != nullptr; }
bool Mp_InSession() { return g_net && sco::net::GetState() != sco::net::State::Idle; }
bool Mp_GhostsReady() { return g_net && g_spawn && g_sp; }
const char* Mp_StateText() { return g_stateText; }
const char* Mp_LastReason() { return g_reason; }
const char* Mp_AllowText() { return g_allowText.c_str(); }

// The settings both Host and Join need; nullptr when they're fine.
static const char* CheckCommon() {
    if (!g_net) return "multiplayer is off";
    if (sco::net::GetState() != sco::net::State::Idle) return "already in a session; leave it first";
    const size_t name = strlen(g_set.name);
    if (name < 1 || name > SCO_NET_MAX_NAME) return "pick a name (1 to 64 characters)";
    for (const char* c = g_set.name; *c; ++c)
        if (static_cast<unsigned char>(*c) < 0x20) return "the name can't hold control characters";
    if (strlen(g_set.passphrase) < 8) return "pick a passphrase of at least 8 characters";
    return nullptr;
}

static sco::net::Scope MakeScope() {
    sco::net::Scope s;
    s.allow = g_allow;   // never `any`: sc-offline doesn't offer it
    return s;
}

static const char* ResultText(sco::Result r) {
    switch (r) {
        case sco::Result::Ok: return nullptr;
        case sco::Result::Failed: return "couldn't open the UDP port (in use? mod.log says why)";
        case sco::Result::BadArg: return "refused: check the address (LAN or multiplayer_allow only), name and passphrase";
        default: return "sco.net isn't running";
    }
}

const char* Mp_Host() {
    if (const char* err = CheckCommon()) return err;
    if (g_set.hostPort < 1024 || g_set.hostPort > 65535) return "the port must be 1024 to 65535";
    sco::net::HostOptions h;
    h.port = static_cast<uint16_t>(g_set.hostPort);
    h.passphrase = g_set.passphrase;
    h.playerName = g_set.name;
    h.scope = MakeScope();
    Mp_SaveSettings();
    const sco::Result r = sco::net::Host(h);
    Log("[multiplayer] host on UDP port %d: %s", g_set.hostPort, sco::ResultName(r));
    return ResultText(r);
}

const char* Mp_Join() {
    if (const char* err = CheckCommon()) return err;
    if (g_set.joinPort < 1024 || g_set.joinPort > 65535) return "the port must be 1024 to 65535";
    sco::net::Endpoint ep;
    if (!sco::net::ParseAddress(g_set.joinAddress, static_cast<uint16_t>(g_set.joinPort), &ep))
        return "enter the host's IPv4 address, like 192.168.1.20";
    if (!sco::net::InScope(MakeScope(), ep)) return "that address isn't on your LAN or in multiplayer_allow";
    sco::net::JoinOptions j;
    j.address = g_set.joinAddress;
    j.port = static_cast<uint16_t>(g_set.joinPort);
    j.passphrase = g_set.passphrase;
    j.playerName = g_set.name;
    j.scope = MakeScope();
    Mp_SaveSettings();
    const sco::Result r = sco::net::Join(j);
    Log("[multiplayer] join %s:%d: %s", g_set.joinAddress, g_set.joinPort, sco::ResultName(r));
    return ResultText(r);
}

void Mp_Leave() {
    if (!g_net) return;
    if (g_net->is_active()) {   // tell the others first, so their ghosts of you go at once
        SendDespawn(kAvatar);
        if (g_announcedShip[0]) SendDespawn(kShip);
    }
    sco::net::Leave("left from the Multiplayer tab");
    RemoveAll("left the session");
}

int Mp_PeerCount() { return g_net && g_net->is_active() ? static_cast<int>(g_peers.size()) + 1 : 0; }

static const char* GhostText(const Ghost& g) { return g.note[0] ? g.note : "waiting for a position"; }

bool Mp_Peer(int index, MpPeerView& out) {
    if (index < 0 || index >= Mp_PeerCount()) return false;
    out = MpPeerView{};
    if (index == 0) {
        static char self[SCO_NET_MAX_NAME + 8];
        const uint64_t id = g_net->self_peer();
        if (g_net->get_peer_name(id, self, sizeof(self)) != SCO_OK) strcpy_s(self, g_set.name);
        out.peer = id;
        out.name = self;
        out.self = true;
        out.avatar = "you";
        out.ship = g_shipCls;
        out.sessionId = mpwire::SessionId(id, kAvatar);
        return true;
    }
    const Peer& p = g_peers[static_cast<size_t>(index - 1)];
    out.peer = p.peer;
    out.name = p.name;
    out.avatar = GhostText(p.avatar);
    out.ship = p.shipCls[0] ? (p.ship.note[0] ? p.ship.note : p.shipCls) : "";
    out.sessionId = mpwire::SessionId(p.peer, kAvatar);
    return true;
}

const char* Mp_GoTo(uint64_t peer) {
    const Peer* p = FindPeer(peer);
    if (!p) return "no such player";
    const Ghost& g = p->ship.st == GhostState::Alive ? p->ship : p->avatar;
    if (!g.id || g.st != GhostState::Alive) return "their ghost isn't shown here (see the list)";
    return TeleportToEntity(g.id, g.id == p->ship.id ? 20.0 : 1.0);
}

const sco::plugins::Builtin kMultiplayerBuiltin = { "multiplayer", MpQuery, MpLoad, MpUnload };
