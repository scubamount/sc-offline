# Features

Everything runs through a single in-game menu, which **M** opens. It has eight tabs: **Player**, **Travel**, **Vehicles**, **Crew**, **NPCs**, **Build**, **Squadron 42**, **Menu**. The Squadron 42 tab was written for this repository. The other seven come from upstream ChrisWareOffline 0.9.0-rc1.

Most of the lists below are plain text files in `data/`, and the counts are taken from those files. Remove lines from a file and its list gets shorter. See [data-files.md](data-files.md).

## Player

- Noclip, god mode, infinite ammo.
- Gear menu: equip any of 1489 items, sorted into ten slots (`items.txt`).
- Wallet: your aUEC balance is loaded from `data/wallet.txt` when you spawn and saved back as it changes. The file holds just the number; edit it while the game is closed to change your balance. Without the file, you start with the `aUEC amount` in `data/OfflineDB/default_1.xml` (100,000,000). Once `wallet.txt` exists, that amount is no longer used, so delete `wallet.txt` to go back to it.

## Travel

- Teleport to planets, moons, stations, Lagrange points and jump points, grouped by star system (`locations.txt`).
- **Scan** lists everything the game has loaded and writes it to `locations_found.txt`. Interiors and small zones are hidden unless you ask to see them.
- Save named spots of your own (`bookmarks.txt`). **F7** saves one quick position and **F8** takes you back to it (`spawn.txt`). F7 and F8 run the built-in teleport plugin's commands; see [Plugins](#plugins).
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

## Plugins

sc-offline can load plugins built with the [sco SDK](https://github.com/scubamount/sco-core/blob/main/sdk/README.md): native DLLs, Lua scripts and data packs. Loading is **off by default**.

1. Set `plugins = on` in `sc-offline.ini`.
2. Put each plugin in its own folder, `data/plugins/<id>/`, with its `plugin.ini` (for example `data/plugins/hello/plugin.ini` and `hello.dll`). The SDK's examples (`hello`, `greeter`, `travel_pack`) are ready to copy.
3. Start the game. `mod.log` reports what was found, after the startup lines:

   ```
   [plugin] 5 found, 5 loaded (plugins = on)
   [plugin] teleport <version> builtin loaded
   [plugin] spawn <version> builtin loaded
   [plugin] greeter 1.0.0 lua loaded
   ...
   ```

   A plugin that can't load is listed with the reason (`refused: built for api 2.0`, `missing capability 'teleport'`, ...); the others still load. To switch one plugin off, put an empty file named `disabled` in its folder.

With `plugins = off`, `mod.log` shows `[plugin] 2 found, 2 loaded (plugins = off)`: only the built-in plugins load.

### Built-in plugins

Some of sc-offline's own features are plugins compiled into `dinput8.dll`. They load first, with `plugins` on or off, and are listed as `builtin` in the `[plugin]` report (`[plugin] loaded teleport <version> (api 1.1) built in`). Their commands are the ones plugins call through the SDK's `invoke`:

| Command | Does | Key |
| --- | --- | --- |
| `teleport.save` | Saves where you're standing to `spawn.txt`; the reply names the spot | **F7** |
| `teleport.go` | Teleports to the saved spot; the reply says where you went, or why not | **F8** |
| `spawn.ship <class> <height>` | Spawns a ship `<height>` m (0 to 10000) above you, as the Vehicles tab does, and makes it the Crew & seats target; the status strip says when it's there. An unknown class answers `failed` | |

The teleport commands need the `teleport` capability and `spawn.ship` needs `spawn.ship`; each is missing when this game build's addresses for it aren't found, and the rest of the plugin system still starts. A built-in owns its id, command prefix and services, so a plugin folder named `teleport` or `spawn` is refused (`the id belongs to a built-in plugin`), whatever its kind.

The `spawn` built-in also runs the spawner's own work every tick (the Vehicles tab's spawns, seat jobs, crew), so a fault there switches off the spawner (`[plugin] spawn ... crashed`) instead of the game. For plugin authors it publishes a service, `spawn.entities` 1.0: a C function table to spawn an entity class near you and look up your entity and ship ids, without going through command replies. Its header is [`src/builtins/spawn_service.h`](../src/builtins/spawn_service.h); find it with `query_service` (sco_api 1.1). A plugin runs its own code in the game, so only install plugins you trust.

When you quit the game (the menu's Quit, or the `quit` console command), plugins get `game.exit` and are then unloaded, newest first and built-ins last, before the game exits. `mod.log` shows `[app] game closing (CSystem::Quit): game.exit, unloading plugins` followed by one `[plugin] unloaded <id>` line per plugin. If the game crashes or is killed (Task Manager, `taskkill`), plugins get no `game.exit` and aren't unloaded; don't rely on it to save anything that matters.

## Not available offline

These need RSI's servers, and the mod doesn't replace them:

- Character creation and customization: the main menu is skipped. Not planned ([#24](https://github.com/scubamount/sc-offline/issues/24)).
- ASOP fleet terminals, which load forever, and the mobiGlas vehicle manager, which shows locked blank entries. Use the **Vehicles** tab to spawn ships.
- Your account's ships, items and hangar.
- Choosing a spawn location. You start at the game's own spawn, or over Daymar with `start = Daymar` (see [launcher.md](launcher.md#sc-offlineini)).

## Items still to confirm in game

ChrisWareOffline 0.9.0-rc1 flagged these as built after its last in-game test: the energy-weapon top-up, NPC deletion through the entity handle, Stand up, the real-building prefab preview, and Pyro and Nyx names after a fresh scan.
