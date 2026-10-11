# Architecture: sc-offline on sco-core

sc-offline is two things on top of [sco-core](https://github.com/scubamount/sco-core): the **host** that loads sco-core's kernel into Star Citizen, and the **reference consumer** of what sco-core publishes. This page describes the code as it is on the `pure-consumer` line, pinned to sco-core `31cf53a` (the commit that adds `game.creative`, sco-core #74). How to build and what CI checks: [build.md](build.md). What nobody has run in game yet: [status.md](status.md).

## Who owns what

| Layer | Owns | Lives in |
| --- | --- | --- |
| **sco-core kernel** | The plugin loader, `sco_api`, `sco.ui`, `sco.storage`, `sco.net`, `sco.ipc`, `sco.settings`, hot reload (`sco.reload`), the detour engine (`sco/hook.h`), the CryPak/DataCore pack loader | `external/sco-core` |
| **sco-core game pack** | The game's signature rows (`sco/game/*.h`, each address found once, by one unique pattern) and the `game.*` services built on them: `teleport.spatial`, `spawn.entities`, `game.actors`, `game.vehicles`, `game.entities`, `game.world`, `game.creative` | `external/sco-core` |
| **sc-offline** | The `dinput8.dll` proxy, the offline patches, the main-thread hook, the menu shell, the launcher, the ten built-in plugins, the data lists, and the optional `creative` plugin | `src/`, `launcher/`, `plugins/`, `data/` |

Under `src/` nothing finds a game address by pattern: `tools/no-scans.sh` fails the build if anything there calls a raw scanner (`FindPattern`, `FindUniquePattern`, `FindCString`, `FindRipLea`, `FunctionStart`, `BytesMatch`, `memchr`). Every address comes from a sco-core row, so a game patch that moves code is fixed in one place, sco-core, which is checked against each game build.

## Startup

On the main-thread hook's first tick, `dllmain.cpp` calls `sco::app::Start`. In order: capabilities (`SetFeatureCaps`, which turns each feature's capability on from its resolved rows), the `sco_api` table, the game services (`Platform::gameServices`; `mod.log` prints `[game] game services published: ...`), the built-in plugins in the order of `kBuiltins` in `src/builtins/builtins.h`, then the plugins in `data/plugins/` if `plugins = on`. That happens whether or not a feature resolved on this game build: a feature whose rows are missing loses its capability and its commands, and the rest still starts.

## What each feature runs on

| Feature | Runs on | Notes |
| --- | --- | --- |
| Save / go to a spot (F7, F8) | `teleport` built-in; capability `teleport` | `teleport.spatial` 1.0 is published by the game pack, not by this built-in |
| Spawn a ship (Vehicles tab, `spawn.ship`) | `spawn` built-in over the spawner engine (`src/spawner.cpp`) | `spawn.entities` 1.2 is published by the game pack; ASOP registers a retrieved ship with the game pack's mover (`RegisterPlayerVehicle`) |
| Crew commands (`crew.*`) | `game.vehicles` 1.0, `game.actors`, and `teleport.spatial` for the pose | Gated on `game.vehicles.seats`, `game.vehicles.seat`, `game.vehicles.flight_ready`, `game.actors.despawn`. The Crew **tab** still runs on the spawner's seat code |
| NPCs | `game.actors` (`spawn_npc`, `despawn`) | Gated on `game.actors.spawn_npc` and `game.actors.despawn`; the game pack owns the NPCs and despawns them when the built-in unloads |
| Build props | `game.entities`: `spawn`, `set_transform`, `despawn`, and `keep()` (1.1) for placed props | Prefabs (`.socpak`) and NPC props still use the spawner. Without `keep()` (a 1.0 game pack) props stay owned by the plugin |
| Build: ground ray for placement, build camera | `game.world.raycast`, `game.world.camera` | The aim ray that moves the preview stays on the `build.*` rows: it must skip the preview prop and `raycast` 1.0 has no skip list |
| Noclip, god mode, infinite ammo | The optional `creative` plugin on `game.creative` 1.0 | Not in `dinput8.dll`; see [below](#built-ins-and-optional-plugins) |
| Natural mining | `mining` built-in on the `game.mining` row (`mining.cell`) and a `sco::hook` detour | Off unless `mining = on` |
| Gear, outfits, quantum travel, contracts | Built-ins gated on `loadout`, `quantum`, `contracts`; sco-core rows, no scans | Their engines are still sc-offline's own (`src/loadout.cpp`, `quantum.cpp`, `contracts.cpp`); no `game.*` service covers them yet |
| Multiplayer | `multiplayer` built-in on `sco.net`, `teleport.spatial`, `spawn.entities` | Gated on `sco.net` |
| Gladius quantum drive | A data pack applied by sco-core's DataCore loader | `data/builtin/quantum/` |
| Titanfall 2 and voxel bridges | `sco.ipc`, optional builds only | [bridges.md](bridges.md) |

## Built-ins and optional plugins

**Built-ins** are compiled into `dinput8.dll`: `teleport`, `spawn`, `crew`, `loadout`, `npc`, `quantum`, `build`, `contracts`, `mining` and `multiplayer` (plus `titanlink` and `voxel_bridge` in the optional bridge builds). Each is a `sco::plugins::Builtin` in `src/builtins/`, a table of the three plugin functions instead of DLL exports, and talks through `sco_api` like any plugin. They load first, with `plugins` on or off, they own their id and command prefix (a plugin folder with one of those ids is refused), and the host cannot reload them (`sco.reload` answers that a built-in cannot be reloaded).

**Optional plugins** are folders under `data/plugins/<id>/` with a `plugin.ini`. They load only with `plugins = on` in `sc-offline.ini`. A plugin is off when its folder holds an entry named `disabled`; the launcher's Plugins page creates and deletes that marker, and a change applies at the next game start. The Plugins page also edits a plugin's `[settings]`. **Hot reload** (`sco.reload <plugin>`) works only inside the running game; the launcher has no channel into it.

The repository ships two optional plugins, both plain SDK consumers built only from the SDK's headers. **`discord`** (`plugins/discord/`, CMake target `sc-offline-discord`) is the Discord status: the version from `api->host_version()`, where you are from `teleport.spatial`, your ship from `game.vehicles` and `game.entities`, its options through `sco.settings`; it ships **on**. **`creative`** (`plugins/creative/`, CMake target `sc-offline-creative`) is `requires = game.creative` and ships with its `disabled` marker, so it is **off** until you turn it on. Its commands are `creative.god`, `creative.noclip`, `creative.noclip_speed`, `creative.ammo`, `creative.ship_ammo` and `creative.status`; the Player and Vehicles tabs' checkboxes run them. Details: [features.md](features.md#the-creative-plugin).

## The CI gates

All five run on every pull request and every push to `main` ([build.yml](../.github/workflows/build.yml)); `release` runs on `v*` tags only.

| Job | Checks |
| --- | --- |
| `check` | `tools/check.sh` (clang parse against mingw-w64 headers, MSVC C2712 screen, baseline in `tools/check-baseline.txt`), the release-manifest tests, and that `src/third_party/imgui` matches the ImGui headers the SDK ships |
| `build` | Release x64 with MSVC; the binaries checked with `dumpbin` and `mt`; uploads `dinput8-release`, `creative-plugin` and `test-plugins` |
| `bridges` | The same build with both bridge options on, only so the bridges keep compiling |
| `no-scans` | No raw game scan under `src/` |
| `sdk-headers` | Every file under `src/builtins/` and `plugins/` includes only SDK headers, its own files, or a header named in `tools/sdk-headers-allow.txt` |

### What the `sdk-headers` allow-list means

The gate makes `src/builtins/` and `plugins/` consumers of the SDK zip's public headers. `plugins/creative` passes with **no** allow-list entry. The built-ins do not: the list names every non-SDK header a built-in includes, one reason per line, and it is long because **the built-ins still call sc-offline's own engines** (`src/spawner.cpp`, `build.cpp`, `teleport.cpp`, `loadout.cpp`, `quantum.cpp`, `contracts.cpp`, `npc.cpp`, `cvars.cpp`, the menu shell, the multiplayer session), or use the kernel directly where the SDK has no form yet (`sco/hook.h` and the `game.mining` rows for mining, `sco/net/session.h` for multiplayer). So the gate does **not** mean the built-ins are pure SDK plugins. It means nobody can add a new dependency unnoticed: a new include of a non-SDK header fails CI, the fix is a `game.*` service in sco-core and not a new line in the list, and a line whose includes are gone fails too, so the list only shrinks. The `crew` and `npc` plugin files (`crew_plugin.cpp`, `npc_plugin.cpp`) include only SDK headers plus the built-in shell (`builtins.h`, `tabs.h`); their tab files (`crew_ui.cpp`, `npc_ui.cpp`) are still on the list.

## Historical notes

sco-core's `docs/framework.md` ("Phase 4") and sc-offline's CHANGELOG entries up to 0.7 describe the move to built-in plugins one feature at a time, with `teleport.spatial`, `spawn.entities` and the cheats first living in this repository. That plan is **historical**: the services moved into sco-core's game pack and the cheats into the `creative` plugin. Read those pages for why a thing is shaped as it is, not for where it lives now.
