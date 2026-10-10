#pragma once
// The multiplayer built-in's messages on sco.net (sco-core's docs/net.md), version 1. Plain bytes,
// little-endian (every sc-offline peer is x64 Windows), written and read field by field: nothing
// here is a game object, a pointer or a zone id. A zone travels by its NAME ("OOC_Stanton_2b_Daymar")
// with a position local to it, as teleport's saved spots keep it, because zone ids are volatile
// streaming handles that mean something only inside one game process.
//
//   multiplayer.pose      unreliable, ~10 per second. Where the sender's player is:
//     u8  version (1)   u8 flags (bit 0: aboard a ship, the ship section follows)
//     u8  levels (1..kMaxLevels)   u8 shipFirst (0 without a ship, else 1..levels-1)
//     u32 seq (counts up per pose; older ones are dropped)
//     levels x { u8 nameLen (1..kMaxZoneName), name, f64 pos[3], f64 rot[4] (x y z w) }
//       the player's position and rotation in each zone of their chain, innermost first
//     ship: levels - shipFirst x { f64 pos[3], f64 rot[4] }
//       the ship's pose in the zones from shipFirst outwards (zone shipFirst - 1 is the ship's own)
//   multiplayer.spawn     reliable. What one of the sender's things is:
//     u8 version (1)   u8 kind (1 avatar, 2 ship)   u8 classLen (1..kMaxClass)   u8 0
//     u64 sessionId ((sender peer id << 32) | kind)   class bytes ([A-Za-z0-9_-])
//   multiplayer.despawn   reliable. That thing is gone:
//     u8 version (1)   u8 kind   u16 0   u32 0   u64 sessionId
//
// Decode* check everything (sizes, counts, characters, finite numbers, unit rotations) and return
// false for anything else: the sender is another player's game, so nothing is trusted.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mpwire {

constexpr uint8_t  kVersion = 1;
constexpr int      kMaxLevels = 6;
constexpr size_t   kMaxZoneName = 63;
constexpr size_t   kMaxClass = 63;
constexpr uint8_t  kAvatar = 1;
constexpr uint8_t  kShip = 2;
constexpr double   kMaxCoord = 1.0e13;   // metres; a system-level zone holds planets ~1e11-1e12 m out

constexpr size_t kPoseHeader = 8;
constexpr size_t kPoseRecord = 7 * sizeof(double);
constexpr size_t kMaxPose = kPoseHeader + kMaxLevels * (1 + kMaxZoneName + kPoseRecord) + (kMaxLevels - 1) * kPoseRecord;
constexpr size_t kSpawnHeader = 12;
constexpr size_t kMaxSpawn = kSpawnHeader + kMaxClass;
constexpr size_t kDespawn = 12;
static_assert(kMaxPose <= 1200, "a pose must fit one unreliable sco.net message");

struct Level {
    char   name[kMaxZoneName + 1];
    double pos[3];
    double rot[4];
};

struct Pose {
    uint32_t seq;
    uint8_t  levels;
    bool     ship;
    uint8_t  shipFirst;
    Level    lv[kMaxLevels];
    double   shipPos[kMaxLevels][3];   // valid for shipFirst <= i < levels
    double   shipRot[kMaxLevels][4];
};

struct Spawn {
    uint8_t  kind;
    uint64_t sessionId;
    char     cls[kMaxClass + 1];
};

inline uint64_t SessionId(uint64_t peer, uint8_t kind) { return (peer << 32) | kind; }

// ---- checks -------------------------------------------------------------------------------------

inline bool ZoneNameOk(const char* s, size_t n) {
    if (n < 1 || n > kMaxZoneName) return false;
    for (size_t i = 0; i < n; ++i)
        if (s[i] < 0x21 || s[i] > 0x7E) return false;
    return true;
}

inline bool ClassOk(const char* s, size_t n) {
    if (n < 1 || n > kMaxClass) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

// A position in range and a rotation near unit length (normalized in place).
inline bool PoseOk(double pos[3], double rot[4]) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i]) || std::fabs(pos[i]) > kMaxCoord) return false;
    double n = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(rot[i])) return false;
        n += rot[i] * rot[i];
    }
    n = std::sqrt(n);
    if (!(n > 0.5 && n < 1.5)) return false;
    for (int i = 0; i < 4; ++i) rot[i] /= n;
    return true;
}

// ---- writing ------------------------------------------------------------------------------------

struct Writer {
    uint8_t* p;
    size_t   cap, n = 0;
    bool     ok = true;
    Writer(uint8_t* buf, size_t size) : p(buf), cap(size) {}
    void Bytes(const void* d, size_t len) {
        if (!ok || len > cap - n) { ok = false; return; }
        memcpy(p + n, d, len);
        n += len;
    }
    void U8(uint8_t v) { Bytes(&v, 1); }
    void U16(uint16_t v) { Bytes(&v, 2); }
    void U32(uint32_t v) { Bytes(&v, 4); }
    void U64(uint64_t v) { Bytes(&v, 8); }
    void F64s(const double* v, int count) { Bytes(v, count * sizeof(double)); }
};

// The pose into buf (kMaxPose bytes); its length, or 0 when it doesn't encode.
inline size_t EncodePose(const Pose& p, uint8_t* buf, size_t cap) {
    if (p.levels < 1 || p.levels > kMaxLevels) return 0;
    if (p.ship && (p.shipFirst < 1 || p.shipFirst >= p.levels)) return 0;
    Writer w(buf, cap);
    w.U8(kVersion);
    w.U8(p.ship ? 1 : 0);
    w.U8(p.levels);
    w.U8(p.ship ? p.shipFirst : 0);
    w.U32(p.seq);
    for (int i = 0; i < p.levels; ++i) {
        const size_t len = strnlen(p.lv[i].name, kMaxZoneName + 1);
        if (!ZoneNameOk(p.lv[i].name, len)) return 0;
        w.U8(static_cast<uint8_t>(len));
        w.Bytes(p.lv[i].name, len);
        w.F64s(p.lv[i].pos, 3);
        w.F64s(p.lv[i].rot, 4);
    }
    if (p.ship)
        for (int i = p.shipFirst; i < p.levels; ++i) {
            w.F64s(p.shipPos[i], 3);
            w.F64s(p.shipRot[i], 4);
        }
    return w.ok ? w.n : 0;
}

inline size_t EncodeSpawn(const Spawn& s, uint8_t* buf, size_t cap) {
    const size_t len = strnlen(s.cls, kMaxClass + 1);
    if ((s.kind != kAvatar && s.kind != kShip) || !ClassOk(s.cls, len)) return 0;
    Writer w(buf, cap);
    w.U8(kVersion);
    w.U8(s.kind);
    w.U8(static_cast<uint8_t>(len));
    w.U8(0);
    w.U64(s.sessionId);
    w.Bytes(s.cls, len);
    return w.ok ? w.n : 0;
}

inline size_t EncodeDespawn(uint8_t kind, uint64_t sessionId, uint8_t* buf, size_t cap) {
    Writer w(buf, cap);
    w.U8(kVersion);
    w.U8(kind);
    w.U16(0);
    w.U32(0);
    w.U64(sessionId);
    return w.ok ? w.n : 0;
}

// ---- reading ------------------------------------------------------------------------------------

struct Reader {
    const uint8_t* p;
    size_t         len, n = 0;
    bool           ok = true;
    Reader(const void* buf, size_t size) : p(static_cast<const uint8_t*>(buf)), len(size) {}
    void Bytes(void* d, size_t count) {
        if (!ok || count > len - n) { ok = false; return; }
        memcpy(d, p + n, count);
        n += count;
    }
    uint8_t U8() { uint8_t v = 0; Bytes(&v, 1); return v; }
    uint16_t U16() { uint16_t v = 0; Bytes(&v, 2); return v; }
    uint32_t U32() { uint32_t v = 0; Bytes(&v, 4); return v; }
    uint64_t U64() { uint64_t v = 0; Bytes(&v, 8); return v; }
    void F64s(double* v, int count) { Bytes(v, count * sizeof(double)); }
};

inline bool DecodePose(const void* buf, size_t len, Pose& out) {
    if (!buf || len < kPoseHeader || len > kMaxPose) return false;
    Reader r(buf, len);
    out = Pose{};
    if (r.U8() != kVersion) return false;
    const uint8_t flags = r.U8();
    if (flags & ~1u) return false;
    out.ship = (flags & 1) != 0;
    out.levels = r.U8();
    out.shipFirst = r.U8();
    out.seq = r.U32();
    if (out.levels < 1 || out.levels > kMaxLevels) return false;
    if (out.ship ? (out.shipFirst < 1 || out.shipFirst >= out.levels) : out.shipFirst != 0) return false;
    for (int i = 0; i < out.levels; ++i) {
        Level& l = out.lv[i];
        const uint8_t n = r.U8();
        if (!r.ok || n < 1 || n > kMaxZoneName) return false;
        r.Bytes(l.name, n);
        l.name[n] = 0;
        r.F64s(l.pos, 3);
        r.F64s(l.rot, 4);
        if (!r.ok || !ZoneNameOk(l.name, n) || !PoseOk(l.pos, l.rot)) return false;
    }
    if (out.ship)
        for (int i = out.shipFirst; i < out.levels; ++i) {
            r.F64s(out.shipPos[i], 3);
            r.F64s(out.shipRot[i], 4);
            if (!r.ok || !PoseOk(out.shipPos[i], out.shipRot[i])) return false;
        }
    return r.ok && r.n == len;
}

// sender is the peer the message came from: a session id must be its own.
inline bool DecodeSpawn(const void* buf, size_t len, uint64_t sender, Spawn& out) {
    if (!buf || len < kSpawnHeader + 1 || len > kMaxSpawn) return false;
    Reader r(buf, len);
    out = Spawn{};
    if (r.U8() != kVersion) return false;
    out.kind = r.U8();
    const uint8_t n = r.U8();
    if (r.U8() != 0) return false;
    out.sessionId = r.U64();
    if ((out.kind != kAvatar && out.kind != kShip) || out.sessionId != SessionId(sender, out.kind)) return false;
    if (n < 1 || n > kMaxClass || len != kSpawnHeader + n) return false;
    r.Bytes(out.cls, n);
    out.cls[n] = 0;
    return r.ok && ClassOk(out.cls, n);
}

inline bool DecodeDespawn(const void* buf, size_t len, uint64_t sender, uint8_t& kind) {
    if (!buf || len != kDespawn) return false;
    Reader r(buf, len);
    if (r.U8() != kVersion) return false;
    kind = r.U8();
    if (r.U16() != 0 || r.U32() != 0) return false;
    const uint64_t id = r.U64();
    return r.ok && (kind == kAvatar || kind == kShip) && id == SessionId(sender, kind);
}

}  // namespace mpwire
