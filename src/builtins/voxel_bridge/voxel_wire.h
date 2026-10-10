/* voxel_wire.h: the layout of the voxel-game channel, Local\SCO_voxel_bridge.link (docs/bridges.md).
 *
 * An sco.ipc channel (sco-core's include/sc_ipc.h): the 64-byte sc_ipc_hdr, then the blocks and
 * rings below at fixed offsets. sc-offline's voxel_bridge built-in creates it (the owner); the
 * voxel game's mod opens it by name with sc_ipc_attach and never creates it. Plain C, so either
 * side can include it; docs/bridges.md describes the same layout in prose.
 *
 * Voxel coordinates are the voxel game's blocks: x east, y up, z south (a right-handed frame with
 * y up). The building area maps them onto Star Citizen: block (0, 64, 0) sits at the anchor (your
 * feet when you made the area), x runs along your right, y along your up and -z along your
 * facing, each block block_size metres. Strings are NUL-terminated within their array; a reader
 * cuts them at the array's end whatever the writer left there. */
#ifndef SCO_VOXEL_WIRE_H
#define SCO_VOXEL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define VX_STATIC_ASSERT(e, m) static_assert(e, m)
#else
#define VX_STATIC_ASSERT(e, m) _Static_assert(e, m)
#endif

#define VX_CHANNEL_NAME    "link"        /* Local\SCO_voxel_bridge.link */
#define VX_LAYOUT_ID       0x31425856u   /* "VXB1" */
#define VX_LAYOUT_VERSION  1u
#define VX_CHANNEL_BYTES   0x800000u     /* 8 MiB */

#define VX_OFF_SC_STATE    0x0100u       /* block, sc-offline writes: vx_sc_state */
#define VX_OFF_PEER_STATE  0x0200u       /* block, the peer writes: vx_peer_state */
#define VX_OFF_TO_PEER     0x1000u       /* ring, SC_IPC_TO_PEER: VX_MSG_AREA, VX_MSG_GROUND */
#define VX_RING_TO_PEER    0x100000u     /* 1 MiB */
#define VX_OFF_FROM_PEER   0x101100u     /* ring, SC_IPC_FROM_PEER: VX_MSG_SOLIDS, VX_MSG_CLEAR, VX_MSG_LOG */
#define VX_RING_FROM_PEER  0x400000u     /* 4 MiB */

#define VX_BASE_Y          64.0          /* the voxel y at the anchor */
#define VX_MAX_XZ          30000         /* |x|, |z| a peer may name, in blocks */
#define VX_MIN_Y           (-64)
#define VX_MAX_Y           320

/* vx_sc_state.flags */
#define VX_SC_SPAWNED  0x1u  /* you're in the universe and your pose is known */
#define VX_SC_FOCUSED  0x2u  /* Star Citizen is the foreground window */
#define VX_SC_ANCHORED 0x4u  /* a building area exists and feet/yaw/pitch are in it */
#define VX_SC_PAUSED   0x8u  /* you're in another zone or far outside the area: don't follow */

/* sc-offline -> peer, ~10 times a second while the channel is open. */
typedef struct vx_sc_state {
    uint64_t time_ms;    /* GetTickCount64() when written */
    uint32_t flags;      /* VX_SC_* */
    uint32_t epoch;      /* the building area; bumped by every new area (VX_MSG_AREA) */
    double   feet[3];    /* your feet in voxel coordinates */
    float    yaw;        /* degrees, the voxel game's convention: 0 faces +z, 90 faces -x */
    float    pitch;      /* degrees, positive looking down */
    float    block_size; /* metres per block */
    uint32_t view_w;     /* Star Citizen's client area, pixels */
    uint32_t view_h;
    uint32_t reserved;   /* 0 */
} vx_sc_state;

/* vx_peer_state.flags */
#define VX_PEER_IN_WORLD 0x1u  /* a world is loaded */
#define VX_PEER_FLAGS    0x1u  /* every flag a reader accepts */

/* peer -> sc-offline. */
typedef struct vx_peer_state {
    uint32_t flags;      /* VX_PEER_* */
    uint32_t epoch;      /* the area the peer has applied (its solids carry it too) */
    double   feet[3];    /* the voxel player's feet (informational) */
    char     status[64];
} vx_peer_state;

/* Ring messages, sc-offline -> peer (VX_OFF_TO_PEER). */
#define VX_MSG_AREA    1u  /* vx_msg_area: a new building area; send every solid section again */
#define VX_MSG_GROUND  2u  /* vx_msg_ground + count vx_ground: Star Citizen's ground under an 8 x 8 column region */

/* Ring messages, peer -> sc-offline (VX_OFF_FROM_PEER). */
#define VX_MSG_SOLIDS  1u  /* vx_msg_solids: which blocks of one 16^3 section are solid */
#define VX_MSG_CLEAR   2u  /* vx_msg_area: forget every solid block of that epoch */
#define VX_MSG_LOG     3u  /* 1-200 bytes of text for mod.log and the tab (no NUL needed) */
#define VX_MSG_LOG_MAX 200u

typedef struct vx_msg_area {
    uint32_t epoch;
    uint32_t reserved;   /* 0 */
} vx_msg_area;

/* An 8 x 8 region of columns, x0..x0+7 by z0..z0+7, then `count` columns (at most 64) where a ray
 * found Star Citizen's ground: top is the ground's height in voxel y. Columns without ground are
 * left out. */
typedef struct vx_msg_ground {
    int32_t  x0, z0;
    uint32_t epoch;
    uint32_t count;
} vx_msg_ground;

typedef struct vx_ground {
    int32_t x, z;
    float   top;
    uint32_t reserved;   /* 0 */
} vx_ground;

/* Section (sx, sy, sz) covers blocks sx*16 .. sx*16+15 (and so on). Bit i of bits (bits[i / 64]
 * >> (i % 64)) is the block x = i & 15, z = (i >> 4) & 15, y = i >> 8 within it. A section a peer
 * sends replaces what it sent before for that section. */
typedef struct vx_msg_solids {
    int32_t  sx, sy, sz;
    uint32_t epoch;      /* the area this belongs to; sections of another epoch are ignored */
    uint64_t bits[64];
} vx_msg_solids;

VX_STATIC_ASSERT(sizeof(vx_sc_state) == 64, "vx_sc_state size");
VX_STATIC_ASSERT(offsetof(vx_sc_state, feet) == 16, "vx_sc_state.feet");
VX_STATIC_ASSERT(offsetof(vx_sc_state, yaw) == 40, "vx_sc_state.yaw");
VX_STATIC_ASSERT(sizeof(vx_peer_state) == 96, "vx_peer_state size");
VX_STATIC_ASSERT(offsetof(vx_peer_state, feet) == 8, "vx_peer_state.feet");
VX_STATIC_ASSERT(offsetof(vx_peer_state, status) == 32, "vx_peer_state.status");
VX_STATIC_ASSERT(sizeof(vx_msg_area) == 8, "vx_msg_area size");
VX_STATIC_ASSERT(sizeof(vx_msg_ground) == 16, "vx_msg_ground size");
VX_STATIC_ASSERT(sizeof(vx_ground) == 16, "vx_ground size");
VX_STATIC_ASSERT(sizeof(vx_msg_solids) == 528, "vx_msg_solids size");
VX_STATIC_ASSERT(offsetof(vx_msg_solids, bits) == 16, "vx_msg_solids.bits");
VX_STATIC_ASSERT(VX_OFF_SC_STATE + 16 + sizeof(vx_sc_state) <= VX_OFF_PEER_STATE, "sc state block overlaps");
VX_STATIC_ASSERT(VX_OFF_PEER_STATE + 16 + sizeof(vx_peer_state) <= VX_OFF_TO_PEER, "peer state block overlaps");
VX_STATIC_ASSERT(VX_OFF_TO_PEER + 192 + VX_RING_TO_PEER <= VX_OFF_FROM_PEER, "rings overlap");
VX_STATIC_ASSERT(VX_OFF_FROM_PEER % 64 == 0 && VX_OFF_TO_PEER % 64 == 0, "rings are 64-byte aligned");
VX_STATIC_ASSERT(VX_OFF_FROM_PEER + 192 + VX_RING_FROM_PEER <= VX_CHANNEL_BYTES, "channel too small");

#endif
