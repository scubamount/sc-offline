# Bridges: Titanfall 2 and a voxel game

Two optional built-ins link Star Citizen to another game running on the same PC: **TitanLink** (Titanfall 2 through Northstar) and the **voxel bridge** (a Minecraft-style voxel game). They talk through sco-core's `sco.ipc` service: one named shared-memory channel per bridge, for the current Windows user only, laid out with sco-core's MIT wire header [`sc_ipc.h`](../external/sco-core/include/sc_ipc.h) ([sco-core docs/ipc.md](../external/sco-core/docs/ipc.md)). Nothing goes over the network.

**They are not in the release.** Both are off by default at build time, so the `dinput8.dll` in the release zip doesn't contain them (CI checks for this), and nothing in `data/` belongs to them. To try one:

```text
cmake -S . -B build -A x64 -DSCO_BRIDGE_TITANLINK=ON -DSCO_BRIDGE_VOXEL=ON
cmake --build build --config Release
```

CI builds them that way in its `bridges` job and uploads the result as the `bridges-test` artifact (`dinput8.dll` with both bridges, and `bridge-peer.exe`). Even in such a build, a bridge does nothing until you open it.

Design: [sco-core docs/design/multiplayer.md, section 4.4](../external/sco-core/docs/design/multiplayer.md#44-the-bridges-optional-built-ins-on-sc_ipch).

## TitanLink (Titanfall 2)

**In game.** Press **F9** (or **Start pilot mode** in the **Titanfall** tab). The built-in opens its channel. If Titanfall 2 isn't answering yet, it starts the EA app (if it isn't running) and then `NorthstarLauncher.exe`, sized to the game window. Both command lines go to `mod.log`, and nothing starts unless you pressed F9. Once Titanfall 2's TitanLink plugin attaches, the built-in asks it for a local match (`mode` on `map`). When the match is loaded, pilot mode starts:

- The match is anchored where you stand: your feet are the match's anchor point, your facing its +y. Ten times a second the built-in sends where you are and where you face, converted into the match's frame, and Titanfall draws its match from your eyes. You move with Star Citizen's own controls.
- An overlay window of its own, click-through and over the game, shows Titanfall's picture (the textures the Titanfall side shares), with a small crosshair. Titanfall 2's window is kept behind the game, at the game's size.
- **V** calls your Titan down 8 m in front of you, and **E** gets you in when you're within 9 m of it, or out again. The mouse buttons, R, F, G, Q, X and Y (and C and V in the Titan) go across as held buttons.
- **F9** again ends pilot mode. **Close link** in the tab (or `titanlink.unlink`) closes the channel, and Titanfall's side sees it drop at once.

Star Citizen still sees the keys you press in pilot mode. Commands: `titanlink.pilot`, `titanlink.titan`, `titanlink.embark`, `titanlink.unlink`, `titanlink.status`, gated on the `titanlink` capability (teleport available).

**Settings**, in `data\titanlink.txt` (optional, `key = value`):

| Key | Default | |
|---|---|---|
| `game` | `C:\Program Files (x86)\Steam\steamapps\common\Titanfall2` | The Titanfall 2 folder with `NorthstarLauncher.exe` |
| `map` | `mp_coliseum` | The local match's map (`[a-z0-9_]`) |
| `mode` | `tdm` | Its mode (`[a-z0-9_]`) |
| `extra` | | More arguments for `NorthstarLauncher.exe` |
| `fov` | `0` | Vertical field of view sent across; 0 uses Star Citizen's `cl_fov` |

## The voxel bridge

**In game.** Press **Ctrl+F9** (or **Open the bridge** in the **Voxel** tab). The built-in opens its channel and makes a **building area** where you stand: block (0, 64, 0) is at your feet, x runs to your right, y up and -z the way you face, each block `size` metres. **Move the area here** (`voxel_bridge.anchor`) moves it.

- Ten times a second the built-in sends your position and facing in block coordinates, so the voxel game's player can follow you.
- The voxel game sends back which blocks are solid. Each solid block becomes a crate (`block`, a cargo box by default) at its place in Star Citizen: up to 8 spawn per tick, at most 4,096 at once, and a block that stops being solid loses its crate.
- It also sends the voxel game Star Citizen's ground under the area: one 8 x 8 column region per tick around you, found with build mode's ground ray.
- **Ctrl+F9** again closes the channel and removes the crates.

Commands: `voxel_bridge.toggle`, `voxel_bridge.anchor`, `voxel_bridge.status`, gated on `voxel_bridge` (teleport and the spawner available).

**Settings**, in `data\voxel_bridge.txt` (optional): `block` (an entity class, default `CargoBox_050x050x050_Metal`, or `none` for no crates), `size` (metres per block, 0.1 to 4, default 0.5), `pivot` (where in the block the crate's origin sits, -1 to 1, default 0), `radius` (ground regions scanned around you, 1 to 4, default 2).

## Where the other sides live

Each bridge has a second half inside the other game. Neither is in this repository, and neither needs anything from it: both build against the published sco-core SDK alone (`sc_ipc.h` and the bridge's layout header, all MIT; see [The wire](#the-wire)).

- **The Titanfall side** is a Northstar plugin (a native DLL that hooks Titanfall 2's Direct3D to share its picture, plus a Squirrel script that runs the local match). The fork's version is `TitanLink/src/plugin.cpp` and `TitanLink/mod/` in the 2026-10-09 download. It lives in its own repository as a separate plugin built against the sco-core SDK (maintainer's decision of 2026-10-10): it is a mod for another game, with Titanfall-side match settings (`sv_cheats`, `ns_auth_allow_insecure` in Titanfall 2) that the design left for the maintainer to decide on, and the SDK headers it includes are MIT so that a non-GPL program can speak the wire. To port the fork's plugin: replace its `Local\SCTitanLink_v1` mapping (`LinkOpen`, `ReadSc`, the `TfBlock` / `FrameBlock` writes and the `cmd` text) with `sc_ipc_attach` on `Local\SCO_titanlink.link` and the blocks and messages below. The free-form console command it used to receive is now a `TL_MSG_START_MATCH` with a map and mode it must check.
- **The voxel side**, a mod for the voxel game, wasn't in the download, so it has to be written against the wire below.

Until they exist, `bridge-peer.exe` (in `bridges-test`) stands in for either one.

## The wire

Both channels follow `sc_ipc.h`: the 64-byte `sc_ipc_hdr` at offset 0 (magic `SCIO`, version, pids, epoch, both heartbeats, size, `layout_id` / `layout_version`, state), then seqlock **blocks** (`sc_ipc_block`, 16 bytes, then the struct) and single-producer **rings** (`sc_ipc_ring`, 192 bytes, then the capacity) at the offsets in the tables. The layouts are defined once, in sco-core's SDK: [`sc_titanlink.h`](../external/sco-core/include/sc_titanlink.h) and [`sc_voxel_bridge.h`](../external/sco-core/include/sc_voxel_bridge.h), MIT, plain C11 / C++20, with every size and offset pinned by `static_assert`s (and by sco-core's `tests/abi_titanlink.c` and `tests/abi_voxel_bridge.c`; [sco-core docs/ipc.md, Bridge layouts](../external/sco-core/docs/ipc.md#bridge-layouts)). sc-offline includes them from `external/sco-core/include` and keeps no copy of its own. The other side vendors the same headers from the SDK zip; its `TL_LAYOUT_VERSION` / `VX_LAYOUT_VERSION` must equal sc-offline's, or `sc_ipc_attach` refuses the channel (`SC_IPC_MISMATCH`). This page describes the same layouts in prose. Everything is little-endian, with natural alignment.

**What the other side must do:**

1. Open the mapping by name (`OpenFileMappingW` + `MapViewOfFile`; never create it), take the view's size from `VirtualQuery`, and call `sc_ipc_attach` with the layout id and version below, then `sc_ipc_ring_attach` for both rings.
2. Beat at least once a second (`sc_ipc_peer_beat`). TitanLink treats you as gone after 4 s without a beat, the voxel bridge after 8 s.
3. Re-attach on `SC_IPC_EPOCH` (sc-offline re-created the channel) and stop on `SC_IPC_GONE` (it closed it). `sc_ipc_check` tells you without touching a ring.
4. Treat everything sc-offline writes as untrusted too: `sc_ipc.h` bounds-checks the rings and blocks, but the values inside are yours to check.

sc-offline checks everything you write in the same way: flags are masked, every coordinate must be finite and in range, text is cut to its array and limited to printable ASCII, a message of the wrong size or an unknown type is skipped, and a ring that fails `sc_ipc.h`'s validation is ignored from then on (logged once).

### TitanLink: `Local\SCO_titanlink.link`

`layout_id` `0x314B4C54` ("TLK1"), `layout_version` 1, 64 KiB. Units are Titanfall's (39.37 per metre), angles are Source's (pitch, yaw, roll in degrees, pitch positive looking down), z up.

| Offset | What | Written by |
|---|---|---|
| `0x0100` | block `tl_sc_state` (80 bytes) | sc-offline, ~10 Hz |
| `0x0400` | block `tl_tf_state` (124 bytes) | Titanfall side |
| `0x0600` | block `tl_frame` (136 bytes) | Titanfall side |
| `0x1000` | ring, `SC_IPC_TO_PEER`, 16 KiB | sc-offline pushes |
| `0x5100` | ring, `SC_IPC_FROM_PEER`, 16 KiB | Titanfall side pushes |

**`tl_sc_state`:** `uint64 time_ms` (`GetTickCount64` when written; interpolate with `vel` between writes), `uint32 flags` (1 pilot mode, 2 Star Citizen in front, 4 anchored: the poses are valid), `uint32 buttons` (bit 0 attack, 1 zoom, 2 reload, 3 melee, 4 offhand, 5 ability, 6 C in the Titan, 7 V in the Titan, 8 next weapon, 9 X; 0 unless pilot mode and in front), `float eye[3]`, `float ang[3]`, `float feet[3]`, `float vel[3]` (units/s), `float fov_scale` (horizontal 4:3 field of view / 70, 0.6 to 2), `uint32 view_w, view_h` (render at this size), `uint32 reserved`. The anchor is your feet when pilot mode began; `feet` there equals your `floor_pt`.

**`tl_tf_state`:** `uint32 flags` (1 in a match, 2 in the Titan, 4 a Titan is parked, 8 embarking or disembarking), `uint32 pid` (your process; sc-offline moves its window only if it is `NorthstarLauncher.exe` or `Titanfall2.exe`), `float origin[3]`, `float eye_z`, `float titan_origin[3]`, `float titan_yaw`, `int32 clip` (-1 none), `uint32 shots` (a counter), `float damage`, `float titan_health` (0 to 1), `float floor_pt[3]` (where sc-offline's anchor is in the match), `float zoom_frac`, `float zoom_fov`, `char weapon[48]`. Coordinates beyond +-1,000,000 make the whole state invalid.

**`tl_frame`:** `uint64 handle[2]` (two D3D11 textures you share as legacy shared handles, so 32-bit values, with `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`, premultiplied alpha, alpha 0 shows Star Citizen, at most 8192 x 8192, a BGRA8/RGBA8 (sRGB or not), RGB10A2 or RGBA16F format), `uint32 tex_seq[2]` (bump after writing that texture; the larger is newer), `uint32 width, height, key_mode, presents`, `char status[96]` (shown in the tab). Keyed mutex: you write while holding key 0 and release with key 1; sc-offline acquires key 1 (without waiting) and releases key 0.

**sc-offline -> Titanfall** (ring types): 1 `START_MATCH` `{char map[64]; char mode[32];}` (once per link; `[a-z0-9_]` names, build your own command from them), 2 `CALL_TITAN` `{float drop[3]; float yaw;}`, 3 `EMBARK` (no payload), 4 `DISEMBARK`, 5 `PILOT_OFF` (release held buttons).
**Titanfall -> sc-offline:** 1 `LOG`, 1 to 200 bytes of text (no NUL needed), shown in `mod.log` and the tab.

### Voxel bridge: `Local\SCO_voxel_bridge.link`

`layout_id` `0x31425856` ("VXB1"), `layout_version` 1, 8 MiB. Coordinates are blocks: x east, y up, z south.

| Offset | What | Written by |
|---|---|---|
| `0x0100` | block `vx_sc_state` (64 bytes) | sc-offline, ~10 Hz |
| `0x0200` | block `vx_peer_state` (96 bytes) | voxel side |
| `0x1000` | ring, `SC_IPC_TO_PEER`, 1 MiB | sc-offline pushes |
| `0x101100` | ring, `SC_IPC_FROM_PEER`, 4 MiB | voxel side pushes |

**`vx_sc_state`:** `uint64 time_ms`, `uint32 flags` (1 spawned, 2 in front, 4 an area exists and the pose is in it, 8 paused: you're in another zone or over 2,000 blocks out, so don't follow), `uint32 epoch` (the building area), `double feet[3]`, `float yaw` (0 faces +z, 90 faces -x), `float pitch` (positive looking down), `float block_size` (metres), `uint32 view_w, view_h, reserved`.

**`vx_peer_state`:** `uint32 flags` (1 a world is loaded), `uint32 epoch` (the area you have applied), `double feet[3]` (informational), `char status[64]` (shown in the tab).

**sc-offline -> voxel** (ring types): 1 `AREA` `{uint32 epoch; uint32 reserved;}`, a new building area: forget what you sent and send every solid section again with this epoch. 2 `GROUND` `{int32 x0, z0; uint32 epoch; uint32 count;}` then `count` (at most 64) `{int32 x, z; float top; uint32 reserved;}`: Star Citizen's ground height (voxel y) in columns of the 8 x 8 region from (x0, z0). Columns without ground are left out.

**Voxel -> sc-offline:** 1 `SOLIDS` `{int32 sx, sy, sz; uint32 epoch; uint64 bits[64];}` (528 bytes), the solid blocks of the 16^3 section at (sx, sy, sz). Bit `i` (`bits[i / 64] >> (i % 64)`) is block x = `i & 15`, z = `(i >> 4) & 15`, y = `i >> 8` within it, and a section replaces what you sent for it before. Sections of another epoch, or outside x and z +-30,000 and y -64 to 320, are ignored. 2 `CLEAR` `{uint32 epoch; uint32 reserved;}` removes every crate of that epoch. 3 `LOG`, 1 to 200 bytes of text.

## Testing with `bridge-peer`

`bridge-peer.exe titanlink` or `bridge-peer.exe voxel`, started while the game runs (before or after you open the bridge), attaches with nothing but the SDK headers `sc_ipc.h`, `sc_titanlink.h` and `sc_voxel_bridge.h`. It beats and prints what sc-offline writes once a second (flags, feet, angles, buttons, the area and the ground it receives), and answers as a minimal other side:

- **titanlink:** it says it's in a match, so pilot mode starts. A Titan call parks a "Titan" where it was asked, and E puts you in it and out again. It shares no picture, so the overlay stays clear.
- **voxel:** it says a world is loaded. For every new building area it sends one section with a 3 x 3 wall four blocks (2 m) in front of the area's origin, so three columns of three crates appear.

Ctrl+C ends it, and within the timeout the tab shows the other side as gone.

## Not ported (follow-ups)

These parts of the fork's bridges need game access that has no signature row or service yet, or a sco-core facility that doesn't exist yet, so they are left out:

- **Taking over the keyboard and mouse** while a bridge is active. The fork patched the game's import of `GetRawInputData` / `GetRawInputBuffer`, a game hook with no row. Star Citizen still sees your keys in pilot mode, and the voxel game can't be controlled from Star Citizen. Candidate: an input-capture mode in a later `sco.ui` minor (design section 4.4).
- **Hiding your Star Citizen body** (and turning breathing off with it): vtable slots `0x360` / `0x370` with a byte check and no row.
- **Titanfall-style movement** (wallrunning, double jump, slide, Titan walking) that moved your Star Citizen character: it wrote your position through raw entity slots (`0x2B0`, `0x2C8`) and cast rays every frame. The match follows Star Citizen's own movement instead. The same goes for the voxel bridge's "follow the voxel player" mode.
- **Per-frame updates:** poses go out at the host tick's 10 Hz, and the other side interpolates. A per-frame callback waits for the `frame` event (design section 3.4).
- **Your camera** (third person, aiming pitch): sc-offline's camera read isn't a service, so poses use your body's facing and a fixed 1.62 m eye height.
- **Shots hitting Star Citizen NPCs** (TitanLink) and NPCs mirrored into the voxel game, with Minecraft hits removing them: these need the camera, ray casts against entities, and entity reads by raw slot.
- **Drawing the voxel world** (textured meshes, avatars, block hiding behind walls) in an overlay with Star Citizen's camera. Without it the crates are the voxel world's only picture in Star Citizen.
- **Light matching** (tinting Titanfall's picture with Star Citizen's light through desktop duplication), the Titan landing shake and the zoom-to-`cl_fov` handover.
- **Typing commands into the voxel game** (`/gamemode`, `/give`, `/summon` for zombies and horses): the voxel side can set itself up.
- **Several building areas** at once: there is one, moved with `voxel_bridge.anchor`.
