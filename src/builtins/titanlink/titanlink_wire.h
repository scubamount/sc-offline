/* titanlink_wire.h: the layout of the TitanLink channel, Local\SCO_titanlink.link (docs/bridges.md).
 *
 * The channel is an sco.ipc channel (sco-core's include/sc_ipc.h): the 64-byte sc_ipc_hdr, then
 * the blocks and rings below at fixed offsets. sc-offline's titanlink built-in creates it (the
 * owner); the Titanfall 2 side (a Northstar plugin) opens it by name with sc_ipc_attach and never
 * creates it. Plain C, so either side can include it; docs/bridges.md describes the same layout in
 * prose for a peer that declares its own structs.
 *
 * Units: Titanfall's (inches; 39.37 per metre), Source angles (pitch, yaw, roll in degrees, pitch
 * positive looking down), z up. Strings are NUL-terminated within their array; a reader cuts them
 * at the array's end whatever the writer left there. */
#ifndef SCO_TITANLINK_WIRE_H
#define SCO_TITANLINK_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define TL_STATIC_ASSERT(e, m) static_assert(e, m)
#else
#define TL_STATIC_ASSERT(e, m) _Static_assert(e, m)
#endif

#define TL_CHANNEL_NAME     "link"        /* Local\SCO_titanlink.link */
#define TL_LAYOUT_ID        0x314B4C54u   /* "TLK1" */
#define TL_LAYOUT_VERSION   1u
#define TL_CHANNEL_BYTES    0x10000u      /* 64 KiB */

#define TL_OFF_SC_STATE     0x0100u       /* block, sc-offline writes: tl_sc_state */
#define TL_OFF_TF_STATE     0x0400u       /* block, the peer writes: tl_tf_state */
#define TL_OFF_FRAME        0x0600u       /* block, the peer writes: tl_frame */
#define TL_OFF_TO_PEER      0x1000u       /* ring, SC_IPC_TO_PEER: TL_MSG_START_MATCH .. */
#define TL_RING_TO_PEER     0x4000u       /* its capacity */
#define TL_OFF_FROM_PEER    0x5100u       /* ring, SC_IPC_FROM_PEER: TL_MSG_LOG */
#define TL_RING_FROM_PEER   0x4000u

#define TL_UNITS_PER_METRE  39.37

/* tl_sc_state.flags */
#define TL_SC_PILOT    0x1u  /* pilot mode is on: draw the match from eye/ang */
#define TL_SC_FOCUSED  0x2u  /* Star Citizen is the foreground window */
#define TL_SC_ANCHORED 0x4u  /* feet/eye/vel are valid (you're in the anchor's zone or one it converts to) */

/* tl_sc_state.buttons: held while set */
#define TL_BTN_ATTACK  (1u << 0)  /* left mouse */
#define TL_BTN_ZOOM    (1u << 1)  /* right mouse */
#define TL_BTN_RELOAD  (1u << 2)  /* R */
#define TL_BTN_MELEE   (1u << 3)  /* F */
#define TL_BTN_OFFHAND (1u << 4)  /* G */
#define TL_BTN_ABILITY (1u << 5)  /* Q */
#define TL_BTN_TITAN_C (1u << 6)  /* C, in the Titan */
#define TL_BTN_TITAN_V (1u << 7)  /* the Titan key, in the Titan */
#define TL_BTN_CYCLE   (1u << 8)  /* Y: next weapon */
#define TL_BTN_X       (1u << 9)  /* X */

/* sc-offline -> peer, ~10 times a second while the channel is open. */
typedef struct tl_sc_state {
    uint64_t time_ms;   /* GetTickCount64() when written: interpolate with vel between writes */
    uint32_t flags;     /* TL_SC_* */
    uint32_t buttons;   /* TL_BTN_*; 0 unless TL_SC_PILOT and TL_SC_FOCUSED */
    float    eye[3];    /* your eye in the match */
    float    ang[3];    /* pitch, yaw, roll */
    float    feet[3];
    float    vel[3];    /* units per second */
    float    fov_scale; /* horizontal 4:3 field of view / 70 degrees, 0.6 to 2 */
    uint32_t view_w;    /* Star Citizen's client area, pixels: render at this size */
    uint32_t view_h;
    uint32_t reserved;  /* 0 */
} tl_sc_state;

/* tl_tf_state.flags */
#define TL_TF_IN_MATCH 0x1u  /* a match is loaded and the pilot exists */
#define TL_TF_IN_TITAN 0x2u  /* the pilot is in the Titan */
#define TL_TF_PARKED   0x4u  /* a Titan is down and empty */
#define TL_TF_BUSY     0x8u  /* embarking or disembarking right now */
#define TL_TF_FLAGS    0xFu  /* every flag a reader accepts; other bits are ignored */

/* peer -> sc-offline. */
typedef struct tl_tf_state {
    uint32_t flags;           /* TL_TF_* */
    uint32_t pid;             /* the Titanfall 2 process (sc-offline places its window) */
    float    origin[3];       /* the pilot's feet in the match */
    float    eye_z;
    float    titan_origin[3];
    float    titan_yaw;
    int32_t  clip;            /* rounds in the weapon, -1 if none */
    uint32_t shots;           /* shots fired since the match began (a counter) */
    float    damage;          /* damage of one shot */
    float    titan_health;    /* 0 to 1 */
    float    floor_pt[3];     /* where the anchor (your feet when pilot mode began) is in the match */
    float    zoom_frac;       /* 0 to 1 while aiming down sights */
    float    zoom_fov;
    char     weapon[48];
} tl_tf_state;

/* tl_frame.key_mode */
#define TL_KEY_NONE   0u
#define TL_KEY_DEPTH  1u
#define TL_KEY_BRIGHT 2u
#define TL_KEY_GUN    3u

/* peer -> sc-offline: the match's picture, as two D3D11 textures the peer shares (legacy shared
 * handles, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, premultiplied alpha: alpha 0 shows Star
 * Citizen). The peer writes a texture holding keyed-mutex key 0 and releases it with key 1, then
 * bumps tex_seq; sc-offline acquires key 1 and releases key 0. */
typedef struct tl_frame {
    uint64_t handle[2];  /* the shared handles (32-bit values); 0 = none */
    uint32_t tex_seq[2]; /* bumped each time that texture holds a new picture; the larger is newer */
    uint32_t width;
    uint32_t height;
    uint32_t key_mode;   /* TL_KEY_*: how the peer cut the picture out (informational) */
    uint32_t presents;   /* frames presented (informational) */
    char     status[96];
} tl_frame;

/* Ring messages, sc-offline -> peer (TL_OFF_TO_PEER). */
#define TL_MSG_START_MATCH 1u  /* tl_msg_start_match: load this local match (once per link) */
#define TL_MSG_CALL_TITAN  2u  /* tl_msg_call_titan */
#define TL_MSG_EMBARK      3u  /* no payload: get into the parked Titan */
#define TL_MSG_DISEMBARK   4u  /* no payload: get out */
#define TL_MSG_PILOT_OFF   5u  /* no payload: pilot mode ended; release held buttons */

/* Ring messages, peer -> sc-offline (TL_OFF_FROM_PEER). */
#define TL_MSG_LOG         1u  /* 1-200 bytes of text for mod.log and the tab (no NUL needed) */
#define TL_MSG_LOG_MAX     200u

/* map and mode are [a-z0-9_] names; the peer builds its own command from them and checks them
 * against the maps and modes it knows. */
typedef struct tl_msg_start_match {
    char map[64];
    char mode[32];
} tl_msg_start_match;

typedef struct tl_msg_call_titan {
    float drop[3]; /* where the Titan should land */
    float yaw;     /* the way it should face */
} tl_msg_call_titan;

TL_STATIC_ASSERT(sizeof(tl_sc_state) == 80, "tl_sc_state size");
TL_STATIC_ASSERT(offsetof(tl_sc_state, flags) == 8, "tl_sc_state.flags");
TL_STATIC_ASSERT(offsetof(tl_sc_state, eye) == 16, "tl_sc_state.eye");
TL_STATIC_ASSERT(offsetof(tl_sc_state, fov_scale) == 64, "tl_sc_state.fov_scale");
TL_STATIC_ASSERT(sizeof(tl_tf_state) == 124, "tl_tf_state size");
TL_STATIC_ASSERT(offsetof(tl_tf_state, origin) == 8, "tl_tf_state.origin");
TL_STATIC_ASSERT(offsetof(tl_tf_state, clip) == 40, "tl_tf_state.clip");
TL_STATIC_ASSERT(offsetof(tl_tf_state, floor_pt) == 56, "tl_tf_state.floor_pt");
TL_STATIC_ASSERT(offsetof(tl_tf_state, weapon) == 76, "tl_tf_state.weapon");
TL_STATIC_ASSERT(sizeof(tl_frame) == 136, "tl_frame size");
TL_STATIC_ASSERT(offsetof(tl_frame, tex_seq) == 16, "tl_frame.tex_seq");
TL_STATIC_ASSERT(offsetof(tl_frame, status) == 40, "tl_frame.status");
TL_STATIC_ASSERT(sizeof(tl_msg_start_match) == 96, "tl_msg_start_match size");
TL_STATIC_ASSERT(sizeof(tl_msg_call_titan) == 16, "tl_msg_call_titan size");
/* The regions fit the channel and don't overlap (a block is its 16-byte sc_ipc_block plus the
 * struct; a ring its 192-byte sc_ipc_ring plus the capacity). */
TL_STATIC_ASSERT(TL_OFF_SC_STATE + 16 + sizeof(tl_sc_state) <= TL_OFF_TF_STATE, "sc state block overlaps");
TL_STATIC_ASSERT(TL_OFF_TF_STATE + 16 + sizeof(tl_tf_state) <= TL_OFF_FRAME, "tf state block overlaps");
TL_STATIC_ASSERT(TL_OFF_FRAME + 16 + sizeof(tl_frame) <= TL_OFF_TO_PEER, "frame block overlaps");
TL_STATIC_ASSERT(TL_OFF_TO_PEER + 192 + TL_RING_TO_PEER <= TL_OFF_FROM_PEER, "rings overlap");
TL_STATIC_ASSERT(TL_OFF_FROM_PEER % 64 == 0 && TL_OFF_TO_PEER % 64 == 0, "rings are 64-byte aligned");
TL_STATIC_ASSERT(TL_OFF_FROM_PEER + 192 + TL_RING_FROM_PEER <= TL_CHANNEL_BYTES, "channel too small");

#endif
