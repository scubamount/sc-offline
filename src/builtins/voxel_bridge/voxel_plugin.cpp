// voxel_bridge: a voxel game's world built into Star Citizen, an optional built-in (CMake
// SCO_BRIDGE_VOXEL).
//
// voxel_bridge.toggle (Ctrl+F9) opens the channel Local\SCO_voxel_bridge.link through sco.ipc
// (layout sc_voxel_bridge.h, docs/bridges.md) and makes a building area where you stand: block
// (0, 64, 0) at your feet, x along your right, y up, -z along your facing, block_size metres a
// block. voxel_bridge.anchor moves the area to where you stand now. Ten times a second the
// built-in writes your position and facing in voxel coordinates, so the voxel game's player can
// follow you; the voxel game's mod sends back which blocks are solid, one 16^3 section at a time,
// and each solid block becomes a crate (data\voxel_bridge.txt's block class) at its place, spawned
// a few per tick and removed again when the block goes. It also sends the voxel game Star
// Citizen's ground under the area, one 8 x 8 column region per tick around you, found with build
// mode's ground ray.
//
// Game access is teleport.spatial (your pose, zone conversions), the spawner's
// SpawnEntityInZone, the NPC built-in's RemoveEntityById and build mode's GroundRay, all existing
// rows; nothing else of the game is touched. Every value the voxel game writes is checked before
// use (ValidPeer, ApplySolids, DrainPeer), and at most kMaxCrates crates exist at once.
#include "voxel.h"
#include "../bridge_common.h"
#include "../builtins.h"
#include "../tabs.h"
#include "../../build.h"
#include "../../common.h"
#include "../../npc.h"
#include "../../spawner.h"
#include "../../teleport.h"
#include "../../version.h"
#include <sc_spatial.h>
#include <sco_ipc.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

const sco_api*       g_api = nullptr;
sco_plugin*          g_self = nullptr;
const sco_ipc_v1*    g_ipc = nullptr;
const sc_spatial_v1* g_sp = nullptr;
uint64_t             g_ch = 0;   // the channel; 0 while closed

const sco_plugin_info kInfo = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, "voxel_bridge", SCO_VERSION, "sc-offline",
};

constexpr const char* kCap = "voxel_bridge";
constexpr uint32_t    kLinkTimeoutMs = 8000;
constexpr size_t      kMaxCrates = 4096;     // crates at once; more solid blocks are counted, not shown
constexpr int         kSpawnsPerTick = 8;
constexpr double      kFarBlocks = 2000.0;   // past this from the area's origin, stop following
constexpr double      kPi = 3.14159265358979323846;

#define LOGI(...) BridgeLog(g_api, g_self, SCO_LOG_INFO, __VA_ARGS__)
#define LOGW(...) BridgeLog(g_api, g_self, SCO_LOG_WARN, __VA_ARGS__)

VxStatus g_st;

struct Config {
    char   block[96] = "CargoBox_050x050x050_Metal";   // "none": no crates
    double size = 0.5;    // metres per block
    double pivot = 0.0;   // where in the block the crate's origin sits, 0 = its floor
    int    radius = 2;    // ground regions (8 x 8 columns) scanned around you
};
Config g_cfg;

struct Area {
    bool     ok = false;
    uint64_t zone = 0;
    double   o[3] = {}, r[3] = {}, f[3] = {}, u[3] = {}, rot[4] = { 0, 0, 0, 1 };
};
Area     g_area;
uint32_t g_epoch = 0;
bool     g_ringBad = false, g_peerBad = false, g_spawnErrLogged = false, g_capLogged = false;

std::unordered_map<uint64_t, uint64_t> g_crates;   // block key -> crate entity id (0 until spawned)
std::vector<uint64_t>                  g_pending;  // block keys to spawn
std::unordered_map<uint64_t, DWORD>    g_scanned;  // ground region key -> when scanned

// ---- config: data\voxel_bridge.txt ---------------------------------------------------------------

bool ValidClass(const char* s) {
    size_t n = 0;
    for (; s[n]; ++n)
        if (n >= 95 || !((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '_'))
            return false;
    return n > 0;
}

void LoadConfig() {
    g_cfg = Config();
    char path[MAX_PATH];
    FILE* f = nullptr;
    if (DataFilePath(path, sizeof(path), "voxel_bridge.txt") && fopen_s(&f, path, "r") == 0 && f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char* key = line;
            while (*key == ' ') ++key;
            for (char* e = key + strlen(key); e > key && e[-1] == ' ';) *--e = 0;
            char* value = eq + 1;
            value[strcspn(value, "\r\n")] = 0;
            while (*value == ' ') ++value;
            for (char* e = value + strlen(value); e > value && e[-1] == ' ';) *--e = 0;
            if (!*value) continue;
            if (!_stricmp(key, "block")) {
                if (ValidClass(value)) strncpy_s(g_cfg.block, value, _TRUNCATE);
                else LOGW("[voxel_bridge] %s: block = %s isn't an entity class name; keeping %s", path, value, g_cfg.block);
            } else if (!_stricmp(key, "size")) {
                const double v = strtod(value, nullptr);
                if (v >= 0.1 && v <= 4.0) g_cfg.size = v;
            } else if (!_stricmp(key, "pivot")) {
                const double v = strtod(value, nullptr);
                if (v >= -1.0 && v <= 1.0) g_cfg.pivot = v;
            } else if (!_stricmp(key, "radius")) {
                const long v = strtol(value, nullptr, 10);
                if (v >= 1 && v <= 4) g_cfg.radius = static_cast<int>(v);
            }
        }
        fclose(f);
        LOGI("[voxel_bridge] %s: blocks are %s, %.2f m", path, g_cfg.block, g_cfg.size);
    }
    strncpy_s(g_st.block, g_cfg.block, _TRUNCATE);
    g_st.blockSize = g_cfg.size;
}

// ---- the area: voxel coordinates <-> the area zone's local frame -------------------------------

void VoxelToLocal(double x, double y, double z, double out[3]) {
    const double s = g_cfg.size;
    for (int i = 0; i < 3; ++i) out[i] = g_area.o[i] + g_area.r[i] * (x * s) + g_area.u[i] * ((y - VX_BASE_Y) * s) - g_area.f[i] * (z * s);
}

void LocalToVoxel(const double local[3], double out[3]) {
    const double d[3] = { local[0] - g_area.o[0], local[1] - g_area.o[1], local[2] - g_area.o[2] };
    const double s = g_cfg.size;
    out[0] = BridgeDot(d, g_area.r) / s;
    out[1] = BridgeDot(d, g_area.u) / s + VX_BASE_Y;
    out[2] = -BridgeDot(d, g_area.f) / s;
}

uint64_t Key(int x, int y, int z) {
    return (static_cast<uint64_t>(x & 0x1FFFFF) << 42) | (static_cast<uint64_t>(y & 0x1FFFFF) << 21) | static_cast<uint64_t>(z & 0x1FFFFF);
}

void Unkey(uint64_t k, int& x, int& y, int& z) {
    auto ext = [](uint64_t v) { return static_cast<int>((v & 0x100000) ? (v | ~0x1FFFFFull) : v); };
    x = ext((k >> 42) & 0x1FFFFF);
    y = ext((k >> 21) & 0x1FFFFF);
    z = ext(k & 0x1FFFFF);
}

// Your pose: the zone you're in, and your feet and facing in the area's zone.
struct Pose {
    uint64_t zone = 0;
    double   pos[3] = {}, f[3] = {};
    bool     inArea = false;   // pos/f are in the area's zone
};

bool ReadPose(Pose& p) {
    double pos[3], q[4], r[3], f[3], u[3];
    if (!g_sp || !g_sp->player_pose(pos, q, &p.zone) || !p.zone) return false;
    BridgeAxes(q, r, f, u);
    if (!g_area.ok) return true;
    if (p.zone == g_area.zone) {
        memcpy(p.pos, pos, sizeof(pos));
        memcpy(p.f, f, sizeof(f));
        p.inArea = true;
        return true;
    }
    const double pf[3] = { pos[0] + f[0], pos[1] + f[1], pos[2] + f[2] };
    double a[3], b[3];
    if (g_sp->zone_to_zone(p.zone, g_area.zone, pos, a) && g_sp->zone_to_zone(p.zone, g_area.zone, pf, b)) {
        for (int i = 0; i < 3; ++i) { p.pos[i] = a[i]; p.f[i] = b[i] - a[i]; }
        p.inArea = BridgeNormalize(p.f);
    }
    return true;
}

// ---- crates --------------------------------------------------------------------------------------

void RemoveCrate(uint64_t id) {
    if (id && CanRemoveEntities()) RemoveEntityById(id);
}

void RemoveAllCrates() {
    for (const auto& c : g_crates) RemoveCrate(c.second);
    g_crates.clear();
    g_pending.clear();
    g_st.dropped = 0;
}

// One section from the voxel game: checked (epoch, coordinates) before anything is spawned.
void ApplySolids(const vx_msg_solids& m) {
    if (m.epoch != g_epoch || !g_area.ok) return;
    const int64_t bx = static_cast<int64_t>(m.sx) * 16, by = static_cast<int64_t>(m.sy) * 16, bz = static_cast<int64_t>(m.sz) * 16;
    if (bx < -VX_MAX_XZ || bx + 15 > VX_MAX_XZ || bz < -VX_MAX_XZ || bz + 15 > VX_MAX_XZ || by < VX_MIN_Y || by + 15 > VX_MAX_Y) return;
    for (int i = 0; i < 4096; ++i) {
        const bool solid = ((m.bits[i >> 6] >> (i & 63)) & 1) != 0;
        const uint64_t key = Key(static_cast<int>(bx) + (i & 15), static_cast<int>(by) + (i >> 8), static_cast<int>(bz) + ((i >> 4) & 15));
        auto it = g_crates.find(key);
        if (solid && it == g_crates.end()) {
            if (g_crates.size() >= kMaxCrates) {
                ++g_st.dropped;
                if (!g_capLogged) LOGW("[voxel_bridge] %zu crates already; further solid blocks aren't shown", kMaxCrates);
                g_capLogged = true;
                continue;
            }
            g_crates.emplace(key, 0);
            g_pending.push_back(key);
        } else if (!solid && it != g_crates.end()) {
            RemoveCrate(it->second);
            g_crates.erase(it);
        }
    }
}

void SpawnPending() {
    if (!_stricmp(g_cfg.block, "none")) { g_pending.clear(); return; }
    for (int n = 0; n < kSpawnsPerTick && !g_pending.empty();) {
        const uint64_t key = g_pending.back();
        g_pending.pop_back();
        auto it = g_crates.find(key);
        if (it == g_crates.end() || it->second) continue;   // gone again, or spawned
        int x, y, z;
        Unkey(key, x, y, z);
        double pos[3];
        VoxelToLocal(x + 0.5, y + g_cfg.pivot, z + 0.5, pos);
        uint64_t id = 0;
        if (const char* err = SpawnEntityInZone(g_cfg.block, g_area.zone, pos, g_area.rot, id)) {
            if (!g_spawnErrLogged) LOGW("[voxel_bridge] crate for block %d %d %d: %s", x, y, z, err);
            g_spawnErrLogged = true;
            g_crates.erase(it);
        } else {
            it->second = id;
        }
        ++n;
    }
}

// ---- Star Citizen's ground for the voxel game -----------------------------------------------------

// Build mode's ground ray from `from` to `to` (world frame) in your zone. Plain values only (C2712).
bool GuardedGroundRay(uint64_t zoneId, const double from[3], const double to[3], double hit[3]) {
    __try {
        const uintptr_t zone = ZoneFromId(zoneId);
        return zone && GroundRay(zone, from, to, hit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ScanRegion(int rx, int rz, double feetY, uint64_t playerZone) {
    struct Msg { vx_msg_ground hdr; vx_ground col[64]; } msg = {};
    msg.hdr.x0 = rx * 8;
    msg.hdr.z0 = rz * 8;
    msg.hdr.epoch = g_epoch;
    const double low = std::floor(feetY) - 20.0;
    for (int i = 0; i < 64; ++i) {
        const int bx = rx * 8 + (i & 7), bz = rz * 8 + (i >> 3);
        double a[3], b[3], aw[3], bw[3], hw[3], hl[3], v[3];
        VoxelToLocal(bx + 0.5, feetY + 2.5, bz + 0.5, a);
        VoxelToLocal(bx + 0.5, low, bz + 0.5, b);
        if (!g_sp->local_to_world(g_area.zone, a, aw) || !g_sp->local_to_world(g_area.zone, b, bw)) return;
        if (!GuardedGroundRay(playerZone, aw, bw, hw) || !g_sp->world_to_local(g_area.zone, hw, hl)) continue;
        LocalToVoxel(hl, v);
        vx_ground& g = msg.col[msg.hdr.count++];
        g.x = bx;
        g.z = bz;
        g.top = static_cast<float>(v[1]);
    }
    g_ipc->ring_push(g_self, g_ch, VX_OFF_TO_PEER, VX_MSG_GROUND, &msg,
                     static_cast<uint32_t>(sizeof(vx_msg_ground) + msg.hdr.count * sizeof(vx_ground)));
}

// The nearest region around you not scanned in the last 3 s.
void ScanNext(const double feet[3], uint64_t playerZone, DWORD now) {
    const int prx = static_cast<int>(std::floor(feet[0] / 8.0)), prz = static_cast<int>(std::floor(feet[2] / 8.0));
    const int band = static_cast<int>(std::floor(feet[1] / 16.0));
    int bestX = 0, bestZ = 0, bestD = -1;
    for (int dz = -g_cfg.radius; dz <= g_cfg.radius; ++dz)
        for (int dx = -g_cfg.radius; dx <= g_cfg.radius; ++dx) {
            auto it = g_scanned.find(Key(prx + dx, band, prz + dz));
            if (it != g_scanned.end() && now - it->second < 3000) continue;
            const int d = dx * dx + dz * dz;
            if (bestD < 0 || d < bestD) { bestD = d; bestX = prx + dx; bestZ = prz + dz; }
        }
    if (bestD < 0) return;
    if (g_scanned.size() > 4096) g_scanned.clear();
    g_scanned[Key(bestX, band, bestZ)] = now;
    ScanRegion(bestX, bestZ, feet[1], playerZone);
}

// ---- the link --------------------------------------------------------------------------------------

bool Push(uint32_t type, const void* data, uint32_t size) {
    return g_ch && g_ipc->ring_push(g_self, g_ch, VX_OFF_TO_PEER, type, data, size) == SCO_OK;
}

bool MakeArea(char* reply, uint32_t size) {
    double pos[3], q[4], r[3], f[3], u[3];
    uint64_t zone = 0;
    if (!g_sp || !g_sp->player_pose(pos, q, &zone) || !zone) {
        snprintf(reply, size, "Your position isn't known yet (spawn first)");
        return false;
    }
    BridgeAxes(q, r, f, u);
    if (!BridgeNormalize(r) || !BridgeNormalize(f) || !BridgeNormalize(u)) {
        snprintf(reply, size, "Your orientation can't be read right now");
        return false;
    }
    RemoveAllCrates();
    g_scanned.clear();
    g_area.ok = true;
    g_area.zone = zone;
    memcpy(g_area.o, pos, sizeof(pos));
    memcpy(g_area.r, r, sizeof(r));
    memcpy(g_area.f, f, sizeof(f));
    memcpy(g_area.u, u, sizeof(u));
    memcpy(g_area.rot, q, sizeof(q));
    if (++g_epoch == 0) g_epoch = 1;
    g_spawnErrLogged = g_capLogged = false;
    const vx_msg_area m = { g_epoch, 0 };
    Push(VX_MSG_AREA, &m, sizeof(m));
    if (!g_sp->zone_name(zone, g_st.zone, sizeof(g_st.zone))) g_st.zone[0] = 0;
    snprintf(reply, size, "Building area made here (%s)", g_st.zone[0] ? g_st.zone : "this zone");
    LOGI("[voxel_bridge] building area %u at your feet in %s", g_epoch, g_st.zone[0] ? g_st.zone : "your zone");
    return true;
}

// Everything the voxel game wrote in its state block is checked before use.
bool ValidPeer(vx_peer_state& s) {
    s.flags &= VX_PEER_FLAGS;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(s.feet[i]) || std::fabs(s.feet[i]) > 1.0e7) return false;
    BridgeSanitize(s.status, sizeof(s.status));
    return true;
}

// Takes what the voxel game queued: sections, clears and log lines; other types are skipped.
void DrainPeer() {
    static uint8_t buf[sizeof(vx_msg_solids) > VX_MSG_LOG_MAX ? sizeof(vx_msg_solids) : VX_MSG_LOG_MAX];
    for (int i = 0; i < 64; ++i) {
        uint32_t type = 0, size = sizeof(buf);
        sco_result r = g_ipc->ring_pop(g_self, g_ch, VX_OFF_FROM_PEER, &type, buf, &size);
        if (r == SCO_TOO_MANY && size <= VX_RING_FROM_PEER) {
            // A record bigger than any this layout has: take it out of the way and skip it.
            std::vector<uint8_t> skip(size);
            r = g_ipc->ring_pop(g_self, g_ch, VX_OFF_FROM_PEER, &type, skip.data(), &size);
            if (r == SCO_OK) continue;
        }
        if (r == SCO_NOT_FOUND) return;
        if (r != SCO_OK) {
            if (!g_ringBad) LOGW("[voxel_bridge] the voxel game's message ring failed validation (%d); its messages are ignored", static_cast<int>(r));
            g_ringBad = true;
            return;
        }
        if (type == VX_MSG_SOLIDS && size == sizeof(vx_msg_solids)) {
            vx_msg_solids m;
            memcpy(&m, buf, sizeof(m));
            ApplySolids(m);
        } else if (type == VX_MSG_CLEAR && size == sizeof(vx_msg_area)) {
            vx_msg_area m;
            memcpy(&m, buf, sizeof(m));
            if (m.epoch == g_epoch) RemoveAllCrates();
        } else if (type == VX_MSG_LOG && size >= 1 && size <= VX_MSG_LOG_MAX) {
            memcpy(g_st.peerLog, buf, size);
            g_st.peerLog[size] = 0;
            BridgeSanitize(g_st.peerLog, sizeof(g_st.peerLog));
            LOGI("[voxel_bridge] voxel game: %s", g_st.peerLog);
        }
    }
}

bool OpenLink(char* reply, uint32_t size) {
    if (g_ch) return true;
    if (!g_ipc || !g_sp) {
        snprintf(reply, size, "The voxel bridge needs sco.ipc and teleport.spatial, and this build has %s", !g_ipc ? "no sco.ipc" : "no teleport.spatial");
        return false;
    }
    LoadConfig();
    uint64_t ch = 0;
    sco_result r = g_ipc->create(g_self, VX_CHANNEL_NAME, VX_CHANNEL_BYTES, VX_LAYOUT_ID, VX_LAYOUT_VERSION, &ch);
    if (r == SCO_OK) r = g_ipc->ring_init(g_self, ch, VX_OFF_TO_PEER, VX_RING_TO_PEER, SCO_IPC_TO_PEER);
    if (r == SCO_OK) r = g_ipc->ring_init(g_self, ch, VX_OFF_FROM_PEER, VX_RING_FROM_PEER, SCO_IPC_FROM_PEER);
    if (r != SCO_OK) {
        if (ch) g_ipc->close(g_self, ch);
        snprintf(reply, size, "Couldn't open the voxel bridge's channel (sco.ipc %d); see mod.log", static_cast<int>(r));
        LOGW("[voxel_bridge] couldn't open Local\\SCO_voxel_bridge.link: sco.ipc answered %d", static_cast<int>(r));
        return false;
    }
    g_ch = ch;
    g_st.linkOpen = true;
    g_ringBad = g_peerBad = false;
    // A fresh epoch per link, so sections a voxel game kept from an earlier link never match.
    g_epoch = GetTickCount() | 1;
    LOGI("[voxel_bridge] link open: Local\\SCO_voxel_bridge.link, waiting for the voxel game's mod");
    return true;
}

void CloseLink(bool removeCrates) {
    if (!g_ch) return;
    if (removeCrates) RemoveAllCrates();
    else { g_crates.clear(); g_pending.clear(); }
    g_ipc->close(g_self, g_ch);
    g_ch = 0;
    g_area.ok = false;
    g_scanned.clear();
    const VxStatus keep = g_st;
    g_st = VxStatus();
    strncpy_s(g_st.block, keep.block, _TRUNCATE);
    g_st.blockSize = keep.blockSize;
    LOGI("[voxel_bridge] link closed");
}

void Publish(uint64_t nowMs, const Pose* p) {
    vx_sc_state s = {};
    s.time_ms = nowMs;
    s.epoch = g_epoch;
    s.block_size = static_cast<float>(g_cfg.size);
    if (GameHasFocus()) s.flags |= VX_SC_FOCUSED;
    if (p) {
        s.flags |= VX_SC_SPAWNED;
        if (g_area.ok) s.flags |= VX_SC_ANCHORED;
        if (p->inArea) {
            LocalToVoxel(p->pos, s.feet);
            const double d[3] = { BridgeDot(p->f, g_area.r), BridgeDot(p->f, g_area.u), -BridgeDot(p->f, g_area.f) };
            const double len = std::sqrt(BridgeDot(d, d));
            if (len > 1e-6) {
                const double y = d[1] / len;
                s.yaw = static_cast<float>(std::atan2(-d[0], d[2]) * 180.0 / kPi);
                s.pitch = static_cast<float>(-std::asin(y < -1.0 ? -1.0 : y > 1.0 ? 1.0 : y) * 180.0 / kPi);
            }
            memcpy(g_st.feet, s.feet, sizeof(g_st.feet));
        }
        if (!p->inArea || std::fabs(s.feet[0]) > kFarBlocks || std::fabs(s.feet[2]) > kFarBlocks ||
            std::fabs(s.feet[1] - VX_BASE_Y) > kFarBlocks)
            s.flags |= VX_SC_PAUSED;
    }
    BridgeViewSize(s.view_w, s.view_h);
    g_st.paused = (s.flags & VX_SC_PAUSED) != 0;
    g_ipc->block_write(g_self, g_ch, VX_OFF_SC_STATE, &s, sizeof(s));
}

void OnTick(const char*, const void*, void*) {
    if (!g_ch) return;
    const uint64_t nowMs = GetTickCount64();
    const DWORD now = static_cast<DWORD>(nowMs);
    uint32_t age = 0;
    const bool linked = g_ipc->peer_age_ms(g_self, g_ch, &age) == SCO_OK && age < kLinkTimeoutMs;
    if (linked != g_st.linked) LOGI(linked ? "[voxel_bridge] voxel game linked" : "[voxel_bridge] the voxel game stopped answering");
    g_st.linked = linked;
    g_st.peerAgeMs = linked ? age : 0;
    g_st.inWorld = false;
    if (linked) {
        vx_peer_state ps = {};
        if (g_ipc->block_read(g_self, g_ch, VX_OFF_PEER_STATE, &ps, sizeof(ps)) == SCO_OK) {
            if (ValidPeer(ps)) {
                g_st.inWorld = (ps.flags & VX_PEER_IN_WORLD) != 0;
                strncpy_s(g_st.peerStatus, ps.status, _TRUNCATE);
            } else if (!g_peerBad) {
                g_peerBad = true;
                LOGW("[voxel_bridge] the voxel game wrote an impossible state; ignored");
            }
        }
        DrainPeer();
    }
    Pose p;
    const bool spawned = ReadPose(p);
    if (spawned && !g_area.ok) {
        char reply[160];
        MakeArea(reply, sizeof(reply));   // the first area: where you are once the link is open
    }
    Publish(nowMs, spawned ? &p : nullptr);
    if (linked && spawned && g_area.ok && !g_st.paused) {
        SpawnPending();
        double feet[3];
        LocalToVoxel(p.pos, feet);
        ScanNext(feet, p.zone, now);
    }
    g_st.anchored = g_area.ok;
    g_st.epoch = g_epoch;
    g_st.crates = g_crates.size();
    g_st.pending = g_pending.size();
}

// ---- commands ----------------------------------------------------------------------------------

sco_result CmdToggle(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    VxToggle(reply, size);
    return SCO_OK;
}
sco_result CmdAnchor(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    VxAnchor(reply, size);
    return SCO_OK;
}
sco_result CmdStatus(const sco_arg*, uint32_t, void*, char* reply, uint32_t size) {
    snprintf(reply, size, "Voxel bridge: link %s, voxel game %s%s, area %s, %zu crates (%zu to spawn, %u not shown)",
             g_st.linkOpen ? "open" : "closed", g_st.linked ? "linked" : "not answering", g_st.inWorld ? " in its world" : "",
             g_st.anchored ? (g_st.paused ? "made, you're outside it" : "made") : "none", g_st.crates, g_st.pending, g_st.dropped);
    return SCO_OK;
}

const sco_plugin_info* VxQuery() { return &kInfo; }

sco_result VxLoad(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    if (api->size > offsetof(sco_api, query_service)) {
        const void* t = nullptr;
        if (api->query_service(SCO_IPC_NAME, SCO_IPC_VERSION_1_0, &t) == SCO_OK) g_ipc = static_cast<const sco_ipc_v1*>(t);
        t = nullptr;
        if (api->query_service(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION, &t) == SCO_OK) g_sp = static_cast<const sc_spatial_v1*>(t);
    }
    if (!g_ipc || !g_sp) LOGW("[voxel_bridge] %s isn't published; the bridge can't open", !g_ipc ? "sco.ipc" : "teleport.spatial");
    LoadConfig();
    sco_result r = RegisterBuiltinCommand(api, self, kCap, "voxel_bridge.toggle", "Voxel bridge",
        "Opens or closes the link to the voxel game (Ctrl+F9); closing removes the crates", CmdToggle);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "voxel_bridge.anchor", "Building area here",
        "Moves the building area to where you stand, facing where you face; the crates are rebuilt there", CmdAnchor);
    if (r == SCO_OK) r = RegisterBuiltinCommand(api, self, kCap, "voxel_bridge.status", "Voxel bridge status",
        "The link, the area and the crates in one line", CmdStatus);
    if (r == SCO_OK) r = api->subscribe(self, "tick", OnTick, nullptr);
    if (r != SCO_OK) return r;   // the host releases what was registered
    RegisterBuiltinTab(api, self, "voxel_bridge.voxel", "Voxel", kTabVoxel, DrawVoxelTab);
    BindBuiltinHotkey(api, self, "ctrl+f9", "voxel_bridge.toggle");
    return SCO_OK;
}

// Built-ins unload as the game quits: the crates go with the game, so they're only forgotten here.
void VxUnload() {
    CloseLink(false);
    g_ipc = nullptr;
    g_sp = nullptr;
    g_api = nullptr;
    g_self = nullptr;
}

}  // namespace

// ---- the tab's view of it (voxel.h) ---------------------------------------------------------------

const VxStatus& VxGetStatus() { return g_st; }

void VxToggle(char* reply, uint32_t size) {
    if (g_ch) {
        CloseLink(true);
        snprintf(reply, size, "Voxel bridge closed; crates removed");
        return;
    }
    if (OpenLink(reply, size)) snprintf(reply, size, "Voxel bridge open: start the voxel game with its sc-offline mod");
}

void VxAnchor(char* reply, uint32_t size) {
    if (!g_ch) { snprintf(reply, size, "Open the voxel bridge first (Ctrl+F9)"); return; }
    MakeArea(reply, size);
}

const sco::plugins::Builtin kVoxelBuiltin = { "voxel_bridge", VxQuery, VxLoad, VxUnload };
