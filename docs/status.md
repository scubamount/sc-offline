# What is not verified in game

As of branch `pure-consumer` (head `763e325`), pinned to sco-core `31cf53a`, on Star Citizen **4.10.196.36804**.

**Nothing from the SDK conversion has run in a game session.** CI proves the code compiles, the gates hold, and the binaries have the right shape. It does not prove a feature works. The last in-game run of sc-offline itself was 0.6.1, before the conversion ([README](../README.md)). The maintainer deferred every in-game check to one run after the conversion lands.

sco-core keeps the matching table for the `game.*` services, with the evidence level of each capability and what a run has to show: [docs/game/status.md](https://github.com/scubamount/sco-core/blob/31cf53aa99817eb8f992c8b90555b5125bbd647d/docs/game/status.md) at the pinned commit. Read it for the service side (for example `game.actors.state`, `game.entities.query_radius`, `game.entities.watch` and `game.events.vehicle_seat` are held **off** until a run decides them). This page lists the sc-offline side: what each converted feature still needs checked. The steps are the ones in the bodies of [#95](https://github.com/scubamount/sc-offline/pull/95), [#97](https://github.com/scubamount/sc-offline/pull/97) and [#98](https://github.com/scubamount/sc-offline/pull/98); `mod.log` lines are quoted as those PRs wrote them.

## From #95 (the SDK conversion, merged)

Each converted feature has a deferred checklist in the PR's body. In short:

- **`teleport.spatial` (#82).** `mod.log` has `[game] game services published: ...` before the `[plugin]` lines and no `teleport.spatial` error. F7 saves a spot, F8 returns to it. `spawn_probe` Ctrl+Alt+3 logs your pose, zone name and `entity_alive`.
- **`spawn.entities` 1.2 (#89).** The `spawn` built-in loads with no error. `spawn_probe` Ctrl+Alt+1 spawns a ship (`spawn.ship`); Ctrl+Alt+2 walks the mover 3 steps, then 1 each after stream-in; a refusal for an entity you don't own is logged as `[game] warning: ...`. The Vehicles tab spawns a ship.
- **Rows (#85).** No failed signature rows (300/300 resolved on 4.10.196.36804 against sco-core at that time) and no `[missions] the mission manager's script library wasn't found`. A contract runs as before (the library comes from `sco::game::missions::ScriptLibrary`). A ship, an NPC, a build prop and a quantum jump each work as before.
- **NPCs on `game.actors` (#90).** `npc.spawn <class> 3` puts three NPCs about 3 m ahead; **Remove spawned NPCs** / `npc.clear` despawns them; unloading or reloading the `npc` built-in despawns them; a bogus class answers `failed` with the game pack's reason. The `game.actors` state probe (`state` is off): get downed, then die, and note which of P1 and P2 reads 1 in each state.
- **Crew on `game.vehicles` (#91).** On `AEGS_Javelin`, `spawn.ship` then `crew.fill <npc>`: one `interactable yes|no` line per seat, then `Seating U x <npc> ... (S seats skipped: not interactable)`, with no `couldn't seat` lines (the expected split on the Javelin is 10 seated, 5 skipped, if the seat-owner hypothesis holds). On a small two-seat ship every empty seat seats; `crew.stand_all`, `crew.clear`, `crew.sit pilot` and `crew.power_on` each answer OK.
- **Build props on `game.entities` (#94).** `spawn_probe` Ctrl+Alt+= logs spawn, `get_transform`, `set_transform`, `class_of`, `alive`, the refusal for an entity you don't own, and `despawn`. In build mode (F6) the preview follows your aim without a `[game] warning` per tick, left click places, Backspace undoes, Clear base removes everything; a `.socpak` and a guard still spawn through the spawner. With `sco.reload` on the `build` built-in, placed props stay in the world, the preview goes, and Undo and Clear base still remove the kept props (`[game] game.entities: build kept <id>: ...` for each).
- **Natural mining (#93).** With `mining = on`: `[mining] natural mining active`, `mining.status` counts cells up with promoted above 0, natural Titanium (Ore) is highlighted in mining mode, scanning shows its composition, the mining laser responds, extraction puts ore in cargo. Leaving and returning, depletion and persistence across restarts were never verified, even by the fork this is ported from.
- **Launcher (#92).** The D3D11/ImGui window at 100% and 150% DPI; every command still works; the Plugins page against a real `data/plugins` (the `disabled` marker is honoured at game start; the `plugins = off` banner and **Turn on**); a plugin's **Save** writes `data/storage/<id>.db` and the plugin reads the value in game (the host accepts the launcher-created `sco_kv` table), **Reset** removes it; Settings, Logs and the update check; `mining`, `mining_debug` and the other ini keys are still read.

## From #97 (`game.world` consumers, merged)

- `mod.log` shows no `[build] game.world isn't available`, no `raycast isn't ready`, no `camera isn't ready`.
- F6: the free camera works and the preview follows the cursor from the `game.world.camera` position, without jitter or climbing toward you (the aim ray is unchanged).
- `build.place <object> 5` on a slope and indoors lands on the floor or ground, not floating or in a wall; `build.place <object> 0` lands under you.
- The Squadron 42 tab's Spawn and the NPC tab's placement land on the ground; a contract that places things on the ground, and the voxel bridge if built, are unchanged.
- With `game.world` unavailable, the same actions work and `mod.log` says why, once.
- Not done in #97, so not a check: `game.events` (the timing of teleport's "player ready" wait and build's "not spawned yet" checks) is not used; the aim ray stays on the `build.*` rows.

## From #98 (SDK-headers gate and the creative plugin)

#98 is open at the time of writing. Nothing below has run in game or on MSVC; its CI gates (`sdk-headers`, `no-scans`) run on the PR.

1. Enable `creative` in the launcher's Plugins page (with `plugins = on`); `mod.log` shows `[plugin] loaded creative`.
2. `creative.god on`: no damage; `creative.god off` restores it.
3. `creative.noclip on` with `creative.noclip_speed 100`: you fly through geometry at that speed; off returns to walking.
4. `creative.ammo on`: a carried weapon's magazine doesn't drain.
5. `creative.ship_ammo on` aboard a ship: the ship's weapons don't drain.
6. Disable or unload the plugin and restart: all five are off and the fly speed is the game's own again.
7. The Player and Vehicles tabs' checkboxes drive the same commands, and revert with a status message ("Needs the creative plugin") when the plugin is off.
8. A launcher self-update turns the plugin off again (the release's `disabled` marker is copied), as intended.

The local checks recorded for #98 were a compile of the built-ins and the plugin with zig/mingw, and `tools/sdk-headers.py`. MSVC (`/W4 /WX` on the plugin target) and `tools/check.sh` were not run for it by its author; CI runs them.

## From the older upstream list

ChrisWareOffline 0.9.0-rc1 flagged these as built after its last in-game test and they are still open: the energy-weapon top-up, NPC deletion through the entity handle, Stand up, the real-building prefab preview, and Pyro and Nyx names after a fresh scan. Not available offline at all: see [features.md](features.md#not-available-offline).
