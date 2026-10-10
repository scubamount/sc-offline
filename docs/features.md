# Features

Everything runs through a single in-game menu, which **M** opens. It has eight tabs: **Player**, **Travel**, **Vehicles**, **Crew**, **NPCs**, **Build**, **Squadron 42**, **Menu**. The Squadron 42 tab was written for this repository. The other seven come from upstream ChrisWareOffline 0.9.0-rc1.

Most of the lists below are plain text files in `data/`, and the counts are taken from those files. Remove lines from a file and its list gets shorter. See [data-files.md](data-files.md).

## Player

- Noclip, god mode, infinite ammo.
- Gear menu: equip any of 1489 items, sorted into ten slots (`items.txt`).
- Wallet: your aUEC balance is loaded when you spawn and saved as it changes, in the contracts built-in's storage (`data/storage/contracts.db`) and in `data/wallet.txt`. The file holds just the number; edit it while the game is closed to change your balance: an edited `wallet.txt` always wins over the stored balance (see [Saves](data-files.md#saves)). Without the file, you start with the `aUEC amount` in `data/OfflineDB/default_1.xml` (100,000,000). Once `wallet.txt` exists, that amount is no longer used, so delete `wallet.txt` to go back to it.

## Travel

- Teleport to planets, moons, stations, Lagrange points and jump points, grouped by star system (`locations.txt`).
- **Scan** lists everything the game has loaded and writes it to `locations_found.txt`. Interiors and small zones are hidden unless you ask to see them.
- Save named spots of your own (`data/storage/quantum.db`). **F7** saves one quick position and **F8** takes you back to it (`data/storage/teleport.db`). F7 and F8 run the built-in teleport plugin's commands; see [Plugins](#plugins). Spots saved in `bookmarks.txt` and `spawn.txt` by an older version are imported the first time you start this one ([Saves](data-files.md#saves)).
- A teleport won't take you into another star system. Pyro and Nyx can only be reached with the default `boot_map = PU_All`.
- Some Pyro places drop you in orbit, because `locations.txt` doesn't have their radius yet.

## Vehicles

- Spawn any of 1102 vehicles (`ships.txt`). Typing in the search box selects the first match.
- Pick where you board: the pilot seat, a seat by name, choose after spawning, or don't board. You can also remove the NPC sitting in that seat.
- **Power on** sends the game's Flight Ready event. Depending on the ship, some systems may still start switched off.
- Infinite ship ammo refills the magazines of the ship you're in.

## Crew

Lists every seat on your ship and who is in it. From here you can sit in a seat, make an NPC stand up, remove an NPC, or add one. NPCs you add will sit in a seat but won't fly the ship or operate turrets.

## NPCs

Spawn any of 2239 NPC archetypes in front of you, and remove them again (`npcs.txt`). If the game refuses to delete an NPC offline, the mod moves it far out of range instead.

## Build (F6)

- Free build mode with 3728 buildable objects in 18 groups (`buildables.txt`).
- Prefabs show as a flag while the camera moves, then as the real building once it stops.
- **Undo** and **Clear base** remove what you placed.

## Contracts

`contract_scripts.txt` lists the 796 contracts that run without any of CIG's mission scripts (hauling and similar). The mobiGlas list leaves out Pyro and Nyx contracts and anything named test, debug or tutorial (`src/contracts.cpp` `Listable`). `mod.log` records the counts at startup: `[contracts] N contracts known; M run ...`.

This repository ships none of CIG's Subsumption mission scripts. Contracts that need them (bounty, delivery-with-combat, salvage and others) are not offered.

## Squadron 42

The first time you open the tab, it shows a spoiler warning. **OK** opens the tab and **Back** returns to the first tab.

- **Outfits**: a searchable list of 35 outfits from `outfits.txt`. Pick one, then press **Wear SQ42 outfit**. Outfits that don't name a head use the default face. Unknown item names are skipped when the list loads, and `mod.log` counts them. **SQ42 visor HUD** changes the lens display the next time you press Equip or wear an outfit.
- **Settings**: four toggles, each with a tooltip: SQ42 auto targeting, visor mini-map, visor greebles, and the SQ42 frontend menu. They are greyed out until the game's current value has been read. They reset when the session ends, and nothing is written to `USER.cfg`.
- **Spawn**: place a buildable from the 60-item `[sq42]` group without entering build mode; the search box narrows the list. Anything you spawn joins your base, so Build's **Undo** and **Clear base** remove it.
- **Ships**: the Idris-P, Gladius, Retaliator, Starfarer, Avenger Stalker and Hornet, plus a Vanduul AI wing and two Bengal rows. Rows are greyed out when this game build doesn't have the class. Player ships spawn 30 m up and put you in the pilot seat using the Vehicles tab's seat rules. Everything spawned here becomes the Crew tab's target ship, except the Vanduul wing that comes with the Bengal. Vanduul spawn 300 m up and attack you. Bengals spawn 1500 m up. **Bengal + Vanduul wing** only brings the wing if this game build has the Vanduul classes, and its label tells you which happened.
- **Console**: type a command and press Enter or **Run** to run it in the game's own console. The output goes to `Game.log`, not the menu.

## Menu

Background image settings. Save a picture as `data/menu_background.png` (or `.jpg`), then restart the game. This tab sets how dark the background is and where the image sits.

## Logging

Everything the mod does is written to `data/mod.log`. The first line of the file is the version.

The startup lines include sco-core's signature report: `[core] signatures: N/N OK`, then one line for each game address that wasn't found (`MISSING`, `AMBIG`, `FAILED` or `BLOCKED`, with the reason). Messages shown in the menu's status strip are also logged, as `[status] ...`.

The Gladius' new quantum drive (hold Caps Lock, or B for NAV mode then hold left mouse) is game data: a built-in data pack, `data/builtin/quantum/datacore/quantum_drive.toml`, that gives the Gladius' quantum drive (the Wetkah Beacon) the game's new quantum drive parameters. sco-core's pack loader applies it to the game's DataCore database (`Data\Game2.dcb`) as the game loads it, served by sco-core's CryPak adapter (`sco::game::pak`, its own `[pak]` lines). The pack names records and fields, not byte offsets, so it keeps working after a game update as long as those still exist. At startup `mod.log` says `[+] new quantum drive: game data loader hooked (pack quantum_drive applies as Game2.dcb loads)`; during the load, `[plugin] loaded quantum <version> (data, 1 files)` and `[datacore] 1 pack: quantum 508/508 applied`; after it, `[+] new quantum drive: game data patched as it loaded (pack quantum_drive: 508 operations), load ok in N ms`, the DataCore loader's run from start to return as `sco::game::pak` reports it. If an update removed or renamed something the pack uses, none of it applies: the game loads its own data, the Gladius keeps its stock drive, and the line names what is missing, for example `[!] new quantum drive: pack quantum_drive not applied (line 31: instance 0 of SCItemQuantumDriveParams_NEW field "heatParams.rampUpThermalEnergyDraw": no property "rampUpThermalEnergyDraw" in QuantumDriveHeatParams (field not found)); game data loaded ok in N ms without the new drive`. The capability `quantum.drive` is on only when the pack applied. To check the pack against a new game build without starting the game, use sco-core's `sco-dcb check <Game2.dcb> data/builtin/quantum` (exit 0: all of it applies).

## Plugins

sc-offline can load plugins built with the [sco SDK](https://github.com/scubamount/sco-core/blob/main/sdk/README.md): native DLLs, Lua scripts and data packs. Loading is **off by default**.

1. Set `plugins = on` in `sc-offline.ini`.
2. Put each plugin in its own folder, `data/plugins/<id>/`, with its `plugin.ini` (for example `data/plugins/hello/plugin.ini` and `hello.dll`). The SDK's examples (`hello`, `greeter`, `travel_pack`) are ready to copy.
3. Start the game. `mod.log` reports what was found, after the startup lines:

   ```
   [plugin] 12 found, 12 loaded (plugins = on)
   [plugin] teleport <version> builtin loaded
   [plugin] spawn <version> builtin loaded
   ...
   [plugin] contracts <version> builtin loaded
   [plugin] greeter 1.0.0 lua loaded
   ...
   ```

   A plugin that can't load is listed with the reason (`refused: built for api 2.0`, `missing capability 'teleport'`, ...); the others still load. To switch one plugin off, put an empty file named `disabled` in its folder.

With `plugins = off`, `mod.log` shows `[plugin] 9 found, 9 loaded (plugins = off)`: only the nine built-in plugins load.

### Built-in plugins

sc-offline's features are nine plugins compiled into `dinput8.dll`: `teleport`, `spawn`, `crew`, `loadout`, `npc`, `ammo`, `quantum`, `build` and `contracts`, loaded in that order. They load first, with `plugins` on or off, and are listed as `builtin` in the `[plugin]` report (`[plugin] loaded teleport <version> (api 1.1) built in`). Each runs its feature's per-tick work from a `tick` subscription, so a fault there switches that feature off (`[plugin] <id> ... crashed`) instead of crashing the game. Each built-in also draws its own tabs in the menu through sco-core's `sco.ui` service, and binds its keys there: `build` binds F6, and `teleport` binds F7 and F8. The menu looks and works as before. A fault while a tab is drawn switches off only the built-in that owns it, along with its tabs. Their commands are the ones plugins call through the SDK's `invoke`; a command that ran but couldn't do it (an unknown name, a list not loaded yet) answers `failed` with the reason:

| Command | Does | Key |
| --- | --- | --- |
| `teleport.save` | Saves where you're standing (`data/storage/teleport.db`); the reply names the spot | **F7** |
| `teleport.go` | Teleports to the saved spot; the reply says where you went, or why not | **F8** |
| `spawn.ship <class> <height>` | Spawns a ship `<height>` m (0 to 10000) above you, as the Vehicles tab does, and makes it the Crew & seats target; the status strip says when it's there. An unknown class answers `failed` | |
| `crew.target` | Makes the ship you're in the Crew & seats target | |
| `crew.sit <seat>` | Puts you in the target ship's first seat whose name has these words (`pilot`, `turret left`), removing an NPC in it | |
| `crew.stand_all` | You and every NPC on the target ship get out of the seats | |
| `crew.fill <npc>` | Puts an NPC of this archetype (`npcs.txt`) in every empty seat of the target ship | |
| `crew.clear` | Removes every NPC from the target ship's seats | |
| `crew.power_on` | Sends the game's Flight Ready event to the target ship | |
| `npc.spawn <npc> <count>` | Spawns 1 to 10 NPCs of an archetype (`npcs.txt`) in front of you | |
| `npc.clear` | Removes the NPCs you spawned | |
| `loadout.equip <items>` | Equips items from `items.txt` (separated by spaces or commas, one per slot; the other slots are empty, as in the gear menu) | |
| `loadout.wear <outfit>` | Wears a Squadron 42 outfit from `outfits.txt` | |
| `ammo.infinite <on>` | Infinite ammo on or off (the menu's checkbox doesn't follow it) | |
| `ammo.ship_infinite <on>` | Infinite ship ammo on or off | |
| `quantum.travel <place> <altitude>` | Teleports you to a place from the Travel tab, `<altitude>` m (100 to 20000) above the ground | |
| `quantum.bookmark <name>` | Teleports you to a saved spot | |
| `quantum.save_bookmark <name>` | Saves where you are as a named spot (`data/storage/quantum.db`); an empty name uses the zone's | |
| `quantum.scan` | Scans everything the game has loaded into `locations_found.txt` | |
| `build.toggle` | Build mode on or off | **F6** |
| `build.undo` | Removes the last object you placed | **Backspace** in build mode |
| `build.clear` | Removes everything you placed | |
| `build.place <object> <ahead>` | Places one buildable (`buildables.txt`) `<ahead>` m in front of you (0 = at your feet, up to 100) without entering build mode | |
| `contracts.status` | How many contracts are known, offered and running, and your wallet's balance | |

Each built-in's commands need the capability named after it (`teleport`, `spawn.ship`, `crew`, `npc`, `loadout`, `ammo`, `quantum`, `build`, `contracts`); one is missing when this game build's addresses for that feature aren't found, and the rest of the plugin system still starts. A built-in owns its id, command prefix and services, so a plugin folder named after one (`teleport`, `spawn`, `crew`, ...) is refused (`the id belongs to a built-in plugin`), whatever its kind.

The `spawn` built-in's tick runs the Vehicles tab's spawns and the seat job that puts you in a seat; the `crew` built-in's runs the Crew & seats actions and crew jobs. For plugin authors it publishes a service, `spawn.entities` 1.1: a C function table to spawn an entity class near you, look up your entity and ship ids, and (new in 1.1) ask whether an entity id still resolves in the game (`entity_alive`), without going through command replies. A plugin built against 1.0 keeps working; one built against 1.1 checks the table's `size` before calling `entity_alive`. Its header is [`src/builtins/spawn_service.h`](../src/builtins/spawn_service.h); find it with `query_service` (sco_api 1.1). A plugin runs its own code in the game, so only install plugins you trust.

The `teleport` built-in publishes `teleport.spatial` 1.0 ([`sc_spatial.h`](../external/sco-core/include/sc_spatial.h), shipped in sco-core's SDK): your position and orientation in the zone you're in, the zone an entity is in, a zone's name, and positions converted between a zone and the world or between two zones. Zones are the game's nested frames (star system > planet > city or station > ship > room); every id is the game's own 64-bit id and every position is in metres. Each tick the built-in reads your zone chain from the game into sco-core's zone tree, and a zone you ask about that isn't in it is read on the spot; nothing about the game is kept from one tick to the next, so an id that has streamed out simply answers 0. Like `spawn.entities`, it works from the game thread only.

### Menu tabs and hotkeys for plugins

A plugin can add its own menu tab (drawn with sc-offline's ImGui), a badge beside the tab title, and an overlay. It can also bind a free key chord such as `ctrl+alt+9` to any command. sc-offline keeps **M** and build mode's keys for itself. See [plugin-ui.md](plugin-ui.md) for the tab order, how to draw, and the full key table.

When you quit the game (the menu's Quit, or the `quit` console command), plugins get `game.exit` and are then unloaded, newest first and built-ins last, before the game exits. `mod.log` shows `[app] game closing (CSystem::Quit): game.exit, unloading plugins` followed by one `[plugin] unloaded <id>` line per plugin. If the game crashes or is killed (Task Manager, `taskkill`), plugins get no `game.exit` and aren't unloaded; don't rely on it to save anything that matters.

## Bridges (optional builds only)

Two more built-ins link the game to another game on the same PC through sco-core's `sco.ipc` (local shared memory for your Windows user, nothing over the network): **TitanLink** (`titanlink`, Titanfall 2 through Northstar: F9 starts pilot mode, the **Titanfall** tab) and the **voxel bridge** (`voxel_bridge`, a Minecraft-style voxel game whose solid blocks become crates: Ctrl+F9, the **Voxel** tab). They are **not in the release**: they're compiled in only with the CMake options `SCO_BRIDGE_TITANLINK` and `SCO_BRIDGE_VOXEL` (off by default), so the release's `dinput8.dll` has neither, and even in such a build a bridge does nothing until you open it. Each needs its other half in the other game, which this repository doesn't ship. See [bridges.md](bridges.md) for what they do, their settings, the wire the other side speaks, and what isn't ported yet.

## Not available offline

These need RSI's servers, and the mod doesn't replace them:

- Character creation and customization: the main menu is skipped. Not planned ([#24](https://github.com/scubamount/sc-offline/issues/24)).
- ASOP fleet terminals, which load forever, and the mobiGlas vehicle manager, which shows locked blank entries. Use the **Vehicles** tab to spawn ships.
- Your account's ships, items and hangar.
- Choosing a spawn location. You start at the game's own spawn, or over Daymar with `start = Daymar` (see [launcher.md](launcher.md#sc-offlineini)).

## Items still to confirm in game

ChrisWareOffline 0.9.0-rc1 flagged these as built after its last in-game test: the energy-weapon top-up, NPC deletion through the entity handle, Stand up, the real-building prefab preview, and Pyro and Nyx names after a fresh scan.
