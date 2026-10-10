// bridge-peer: a stand-in for the other program of an sc-offline bridge (docs/bridges.md), for
// testing the titanlink and voxel_bridge built-ins without Titanfall 2 or the voxel game.
//
//   bridge-peer titanlink [seconds]
//   bridge-peer voxel [seconds]
//
// Built only with SCO_BRIDGE_TITANLINK or SCO_BRIDGE_VOXEL (CI artifact "bridges-test"); never in a
// release. It opens the channel the built-in created, by name, with nothing from sco-core but the
// MIT wire header sc_ipc.h (plus the bridge's layout header), the way the real other side does. It
// beats, prints what sc-offline writes once a second, and answers like a minimal peer:
//   titanlink: says it's in a match; a Titan call parks a Titan where asked, E puts the pilot in it
//              and out again; no picture (tl_frame stays empty).
//   voxel:     says a world is loaded; for each new building area it sends one section with a
//              3 x 3 wall four blocks in front of the area's origin, and counts the ground regions
//              sc-offline sends.
// Ctrl+C ends it (or the seconds run out); the channel then sees its heartbeat stop.
#include <windows.h>
#include <sc_ipc.h>
#include "titanlink/titanlink_wire.h"
#include "voxel_bridge/voxel_wire.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

volatile LONG g_stop = 0;

BOOL WINAPI OnCtrl(DWORD) {
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

void Printable(char* s, size_t n) {
    s[n - 1] = 0;
    for (char* p = s; *p; ++p)
        if (*p < 0x20 || *p > 0x7e) *p = '?';
}

struct Link {
    HANDLE           map = nullptr;
    void*            base = nullptr;
    sc_ipc_chan      ch = {};
    sc_ipc_ring_view in = {}, out = {};   // in: sc-offline pushes; out: we push
    bool             up = false;

    void Drop() {
        if (base) UnmapViewOfFile(base);
        if (map) CloseHandle(map);
        base = nullptr;
        map = nullptr;
        up = false;
    }

    // Opens and validates the channel; false (quietly) until sc-offline has created it.
    bool Attach(const wchar_t* name, uint32_t layout, uint32_t version, uint64_t inOff, uint64_t outOff) {
        Drop();
        map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!map) return false;
        base = MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!base || !VirtualQuery(base, &mbi, sizeof(mbi))) { Drop(); return false; }
        int rc = sc_ipc_attach(base, mbi.RegionSize, layout, version, &ch);
        if (rc == SC_IPC_OK) rc = sc_ipc_ring_attach(&ch, inOff, &in);
        if (rc == SC_IPC_OK) rc = sc_ipc_ring_attach(&ch, outOff, &out);
        if (rc != SC_IPC_OK) {
            if (rc != SC_IPC_NOT_READY && rc != SC_IPC_BUSY) printf("attach: sc_ipc result %d\n", rc);
            Drop();
            return false;
        }
        if (in.direction != SC_IPC_TO_PEER || out.direction != SC_IPC_FROM_PEER) {
            printf("attach: the rings face the wrong way\n");
            Drop();
            return false;
        }
        up = true;
        printf("attached: %ls, epoch %llu, %llu bytes\n", name, static_cast<unsigned long long>(ch.epoch), static_cast<unsigned long long>(ch.bytes));
        return true;
    }

    void Log(uint32_t type, const char* text) { sc_ipc_ring_push(&out, type, text, static_cast<uint32_t>(strlen(text))); }
};

void RunTitanLink(Link& l, ULONGLONG until) {
    tl_tf_state tf = {};
    tf.pid = GetCurrentProcessId();
    tf.clip = 30;
    tf.titan_health = 1.0f;
    strcpy_s(tf.weapon, "bridge-peer rifle");
    tl_frame fr = {};
    strcpy_s(fr.status, "bridge-peer: no picture");
    ULONGLONG printAt = 0;
    while (!g_stop && (!until || GetTickCount64() < until)) {
        const ULONGLONG now = GetTickCount64();
        if (!l.up || sc_ipc_check(&l.ch) != SC_IPC_OK) {
            if (l.up) printf("channel re-created or closed; attaching again\n");
            if (!l.Attach(L"Local\\SCO_titanlink.link", TL_LAYOUT_ID, TL_LAYOUT_VERSION, TL_OFF_TO_PEER, TL_OFF_FROM_PEER)) { Sleep(500); continue; }
            l.Log(TL_MSG_LOG, "bridge-peer attached (no Titanfall 2 here)");
        }
        sc_ipc_peer_beat(&l.ch, GetCurrentProcessId(), now);
        tl_sc_state sc = {};
        const int scRc = sc_ipc_block_read(&l.ch, TL_OFF_SC_STATE, &sc, sizeof(sc));
        uint8_t msg[256];
        uint32_t type = 0, n = sizeof(msg);
        int rc;
        while ((rc = sc_ipc_ring_pop(&l.in, &type, msg, &n)) == SC_IPC_OK) {
            if (type == TL_MSG_START_MATCH && n == sizeof(tl_msg_start_match)) {
                tl_msg_start_match m;
                memcpy(&m, msg, sizeof(m));
                Printable(m.map, sizeof(m.map));
                Printable(m.mode, sizeof(m.mode));
                printf("start match: %s on %s\n", m.mode, m.map);
                tf.flags |= TL_TF_IN_MATCH;
            } else if (type == TL_MSG_CALL_TITAN && n == sizeof(tl_msg_call_titan)) {
                tl_msg_call_titan m;
                memcpy(&m, msg, sizeof(m));
                printf("call Titan: %.0f %.0f %.0f, yaw %.0f\n", m.drop[0], m.drop[1], m.drop[2], m.yaw);
                memcpy(tf.titan_origin, m.drop, sizeof(m.drop));
                tf.titan_yaw = m.yaw;
                tf.flags |= TL_TF_PARKED;
            } else if (type == TL_MSG_EMBARK) {
                printf("embark\n");
                tf.flags = (tf.flags & ~TL_TF_PARKED) | TL_TF_IN_TITAN;
            } else if (type == TL_MSG_DISEMBARK) {
                printf("disembark\n");
                tf.flags = (tf.flags & ~TL_TF_IN_TITAN) | TL_TF_PARKED;
            } else if (type == TL_MSG_PILOT_OFF) {
                printf("pilot mode off\n");
            } else {
                printf("message type %u, %u bytes (skipped)\n", type, n);
            }
            n = sizeof(msg);
        }
        if (rc != SC_IPC_EMPTY && rc != SC_IPC_OK) printf("pop: sc_ipc result %d\n", rc);
        if (scRc == SC_IPC_OK && (sc.flags & TL_SC_ANCHORED)) {
            memcpy(tf.origin, sc.feet, sizeof(tf.origin));
            tf.eye_z = sc.eye[2];
            if (tf.flags & TL_TF_IN_TITAN) memcpy(tf.titan_origin, sc.feet, sizeof(tf.titan_origin));
        }
        sc_ipc_block_write(&l.ch, TL_OFF_TF_STATE, &tf, sizeof(tf));
        sc_ipc_block_write(&l.ch, TL_OFF_FRAME, &fr, sizeof(fr));
        if (now - printAt >= 1000) {
            printAt = now;
            uint32_t age = 0;
            sc_ipc_owner_age_ms(&l.ch, now, &age);
            if (scRc == SC_IPC_OK)
                printf("sc-offline (%u ms): flags %x buttons %x feet %.0f %.0f %.0f ang %.0f %.0f vel %.0f %.0f %.0f fov %.2f view %ux%u\n",
                       age, sc.flags, sc.buttons, sc.feet[0], sc.feet[1], sc.feet[2], sc.ang[0], sc.ang[1], sc.vel[0], sc.vel[1], sc.vel[2],
                       sc.fov_scale, sc.view_w, sc.view_h);
            else
                printf("sc-offline (%u ms): no state yet (%d)\n", age, scRc);
        }
        Sleep(50);
    }
}

void RunVoxel(Link& l, ULONGLONG until) {
    vx_peer_state ps = {};
    ps.flags = VX_PEER_IN_WORLD;
    strcpy_s(ps.status, "bridge-peer (no voxel game)");
    uint32_t sentEpoch = 0, groundMsgs = 0, groundCols = 0;
    ULONGLONG printAt = 0;
    while (!g_stop && (!until || GetTickCount64() < until)) {
        const ULONGLONG now = GetTickCount64();
        if (!l.up || sc_ipc_check(&l.ch) != SC_IPC_OK) {
            if (l.up) printf("channel re-created or closed; attaching again\n");
            if (!l.Attach(L"Local\\SCO_voxel_bridge.link", VX_LAYOUT_ID, VX_LAYOUT_VERSION, VX_OFF_TO_PEER, VX_OFF_FROM_PEER)) { Sleep(500); continue; }
            l.Log(VX_MSG_LOG, "bridge-peer attached (no voxel game here)");
            sentEpoch = 0;
        }
        sc_ipc_peer_beat(&l.ch, GetCurrentProcessId(), now);
        vx_sc_state sc = {};
        const int scRc = sc_ipc_block_read(&l.ch, VX_OFF_SC_STATE, &sc, sizeof(sc));
        static uint8_t msg[sizeof(vx_msg_ground) + 64 * sizeof(vx_ground)];
        uint32_t type = 0, n = sizeof(msg);
        int rc;
        while ((rc = sc_ipc_ring_pop(&l.in, &type, msg, &n)) == SC_IPC_OK) {
            if (type == VX_MSG_GROUND && n >= sizeof(vx_msg_ground)) {
                vx_msg_ground g;
                memcpy(&g, msg, sizeof(g));
                ++groundMsgs;
                groundCols += g.count <= 64 ? g.count : 0;
            } else if (type == VX_MSG_AREA && n == sizeof(vx_msg_area)) {
                vx_msg_area a;
                memcpy(&a, msg, sizeof(a));
                printf("new building area, epoch %u\n", a.epoch);
            }
            n = sizeof(msg);
        }
        if (rc != SC_IPC_EMPTY && rc != SC_IPC_OK) printf("pop: sc_ipc result %d\n", rc);
        if (scRc == SC_IPC_OK && (sc.flags & VX_SC_ANCHORED) && sc.epoch != sentEpoch) {
            // A 3 x 3 wall at z = -4 (four blocks in front of the origin), x 0..2, y 64..66:
            // section (0, 4, -1), local z 12.
            vx_msg_solids s = {};
            s.sx = 0;
            s.sy = 4;
            s.sz = -1;
            s.epoch = sc.epoch;
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x) {
                    const int i = x + 12 * 16 + y * 256;
                    s.bits[i >> 6] |= 1ull << (i & 63);
                }
            if (sc_ipc_ring_push(&l.out, VX_MSG_SOLIDS, &s, sizeof(s)) == SC_IPC_OK) {
                sentEpoch = sc.epoch;
                ps.epoch = sc.epoch;
                printf("sent a 3 x 3 wall for area %u\n", sc.epoch);
            }
        }
        if (scRc == SC_IPC_OK) memcpy(ps.feet, sc.feet, sizeof(ps.feet));
        sc_ipc_block_write(&l.ch, VX_OFF_PEER_STATE, &ps, sizeof(ps));
        if (now - printAt >= 1000) {
            printAt = now;
            uint32_t age = 0;
            sc_ipc_owner_age_ms(&l.ch, now, &age);
            if (scRc == SC_IPC_OK)
                printf("sc-offline (%u ms): flags %x area %u feet %.1f %.1f %.1f yaw %.0f pitch %.0f block %.2f m; ground: %u regions, %u columns\n",
                       age, sc.flags, sc.epoch, sc.feet[0], sc.feet[1], sc.feet[2], sc.yaw, sc.pitch, sc.block_size, groundMsgs, groundCols);
            else
                printf("sc-offline (%u ms): no state yet (%d)\n", age, scRc);
        }
        Sleep(50);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (strcmp(argv[1], "titanlink") != 0 && strcmp(argv[1], "voxel") != 0)) {
        printf("usage: bridge-peer titanlink|voxel [seconds]\n");
        return 2;
    }
    const long seconds = argc > 2 ? strtol(argv[2], nullptr, 10) : 0;
    const ULONGLONG until = seconds > 0 ? GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000 : 0;
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    printf("bridge-peer %s: waiting for sc-offline's channel (open the bridge in game)\n", argv[1]);
    Link l;
    if (!strcmp(argv[1], "titanlink")) RunTitanLink(l, until);
    else RunVoxel(l, until);
    l.Drop();
    return 0;
}
