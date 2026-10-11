# Features

Everything runs through a single in-game menu, which **M** opens. It has nine tabs: **Player**, **Travel**, **Vehicles**, **Crew**, **NPCs**, **Build**, **Multiplayer**, **Squadron 42**, **Menu**. The Squadron 42 and Multiplayer tabs were written for this repository; the other seven come from upstream ChrisWareOffline 0.9.0-rc1. Which `game.*` service each feature runs on is in [architecture.md](architecture.md#what-each-feature-runs-on); what has not been run in game yet is in [status.md](status.md).

Most of the lists below are plain text files in `data/`, and the counts are taken from those files. Remove lines from a file and its list gets shorter. See [data-files.md](data-files.md).

## Player

- Noclip, god mode, infinite ammo: from the optional [creative plugin](#the-creative-plugin) (shipped off; turn it on in the launcher's Plugins page).
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
- Infinite ship ammo refills the magazines of the ship you're aboard (the [creative plugin](#the-creative-plugin)).

## Ship terminals, hangars and ATC

The station's ship terminals (ASOP) work offline, with `asop = on` in `sc-offline.ini` (the default):

- **Open a terminal.** It lists the ships of `ships.txt` instead of staying on "Stand by".
- **Deliver** stores a ship at the terminal's station. The ship is spawned 1000 m above you for about 3 s and then stored. The terminal shows "Awaiting Delivery" for a few seconds, then "Stored" with a Retrieve action.
- **Retrieve** gets you a personal hangar from the station's ATC. The station's elevators list it. The hangar's ship lift lowers, the ship is placed on it and the lift brings it up to the hangar floor. Terminals list it as "On Pad" in your hangar, with a Store action.
- **Store** at the hangar terminal lowers the lift with the ship, stores it, and raises the empty lift again. You can Retrieve it again afterwards.
- **ATC hails.** A hail from inside your hangar is a take-off: the doors open and close behind you. A hail from outside gets you a landing pad with its marker on your HUD. At another station the ATC makes you a hangar there, and Store there keeps the ship at that station.

The terminal only offers Deliver with the game's delivery system switched on. Online the server does that; here sc-offline sets `g_itemRecovery.deliverySystemSetup 1`. **Show local ships** lists only ships at this station, so it stays empty until you've delivered one there.

Limits: one ship out at a time. Stored and retrieved ships last for the session. A retrieved ship is a new ship of the same class, so damage, cargo and loadout changes don't carry over. Only small and medium hangars have been tried.

Each part of this switches itself off when the game's code for it isn't found on your game version, and `mod.log` lists every part at startup: `[+] ship terminal open (rc1): ready (...)`, or `[!] ship terminal Deliver (rc3): needs atc.store_vehicle (MISSING)`. With `asop = off` it reads `[-] ship terminals, hangars and ATC (ASOP): off`. Each step of a Deliver, Retrieve or Store is logged with `[asop]`, and hangar and ATC steps with `[iim]` and `[atc]`. The parts are also capabilities that plugins can ask about: `asop.terminal`, `asop.caller`, `asop.list`, `asop.deliver`, `asop.claim_timeout`, `asop.retrieve`, `hangar.lift`, `atc.store`, `hangar.instance`, `atc.tokens` and `asop.diagnostics`.

Your retrieved ship is registered with the game pack's spawn.entities mover, so plugins may move it with `set_entity_transform` (`sc_spawn.h`).

## Crew

Lists every seat on your ship and who is in it. From here you can sit in a seat, make an NPC stand up, remove an NPC, or add one. NPCs you add will sit in a seat but won't fly the ship or operate turrets.

The `crew.*` commands (below) work on sco-core's `game.vehicles` and `game.actors` services, not on this tab's seat code. They act on the ship you're aboard, or the one `crew.target` pinned. Some entries in a ship's seat list are not crew seats (turret items and remote-operated parts: the game's own seat picker skips them). `crew.fill` seats NPCs only in seats that are empty and usable, and says how many it skipped: `Seating 10 x <npc> (5 seats skipped: not interactable)`. `mod.log` has one `interactable yes|no` line per seat, written by the game pack, and `crew.sit` refuses a seat that isn't usable.

## NPCs

Spawn any of 2239 NPC archetypes in front of you, and remove them again (`npcs.txt`). The `npc` built-in spawns through sco-core's `game.actors` service (`spawn_npc` and `despawn`, `sc_actors.h`), so the game pack owns the NPCs: **Remove spawned NPCs** and `npc.clear` despawn them, and unloading or reloading the built-in despawns whatever it spawned. The tab's remove button also takes out the crew's NPCs. NPCs appear 3 m ahead at your feet's height with no ground check, so on a slope they can start a little above or inside the ground.

## Build (F6)

- Free build mode with 3728 buildable objects in 18 groups (`buildables.txt`).
- Prefabs show as a flag while the camera moves, then as the real building once it stops.
- **Undo** and **Clear base** remove what you placed.

### Build mode and `game.entities`

Build mode doesn't touch the game's entity code for props: it asks sco-core's `game.entities` service (`sc_entities.h`, published by the game pack) to spawn a prop in your zone, move the preview with `set_transform` while you aim, and `despawn` the preview when you pick another object. Props you place stay in the world when the build built-in unloads or reloads: build mode calls `keep` (`game.entities` 1.1) on each one as it spawns, so nobody owns it afterwards, and Undo and Clear base remove it through the game's own entity removal. (Against a game pack that only has `game.entities` 1.0, `mod.log` says `[build] game.entities has no keep() (1.0)` once, and props stay owned by the `build` plugin: the game pack removes them if it unloads or crashes.) If the service or its `game.entities.spawn` / `game.entities.transform` capabilities aren't ready on your game build, `mod.log` says `[build] game.entities isn't available: props are placed through the spawner` and build mode works as before. Prefabs (`.socpak`) and NPCs always use the spawner. The `spawn_probe` test plugin's **Ctrl+Alt+=** exercises the service on its own and logs every result.

### Build mode and `game.world`

Where your cursor lands and where a ray finds the ground come from sco-core's `game.world` service (`sc_world.h`, published by the game pack, capabilities `game.world.raycast` and `game.world.camera`):

- The camera build mode aims from is `game.world.camera`.
- The ray that skips nothing is `game.world.raycast`. It places `build.place` and the Squadron 42 tab's Spawn objects in front of you or at your feet, and it is the ground check the contracts and the voxel bridge use. The hit position is a point only: 1.0 doesn't say what was hit or the surface normal.
- The aim ray that moves the preview while you build still reads the game through sco-core's `build.ground_ray` rows. It has to skip the preview prop, and `raycast` 1.0 has no skip list, so the preview would hit itself.

If `game.world` isn't published or a capability isn't ready on your game build, `mod.log` says `[build] game.world isn't available: ...` once (or `game.world.raycast isn't ready` / `game.world.camera isn't ready` at the first use) and build mode reads the rows exactly as before. The teleport built-in (F7, F8) doesn't use `game.world`.

## Contracts

`contract_scripts.txt` lists the 796 contracts that run without any of CIG's mission scripts (hauling and similar). The mobiGlas list leaves out Pyro and Nyx contracts and anything named test, debug or tutorial (`src/contracts.cpp` `Listable`). `mod.log` records the counts at startup: `[contracts] N contracts known; M run ...`.

This repository ships none of CIG's Subsumption mission scripts. Contracts that need them (bounty, delivery-with-combat, salvage and others) are not offered.

## Natural mining

Experimental, and off by default: `mining = on` in `sc-offline.ini` (see [launcher](launcher.md#sc-offlineini)). The `mining` built-in detours the game's `CBiomeBuilder::BuildLargeScaleEcoSystem`, the function that builds each ecosystem cell around you. A cell that has built only its physics (rule flags `0x6`) is passed to the game with the spawning bit set (`0xE`), so the game builds its harvestable rocks as it does online. Only that flag changes: the game's own provider, location and depletion checks, transforms and materials still decide what appears, and nothing is spawned, edited or written by the mod. It acts only for the builder's cell path (type 0), cells whose harvestable LOD is not built yet, and while the offline session is up and the game's online flag is 0. It needs sco-core's `game.mining` capability (the `mining.cell` row, found in any game build); without it `mining.status` is unavailable and `mod.log` says so.

`mining.status` reports the state (off, unavailable, waiting for the offline session, active) and how many cells were seen and promoted. `mining_debug = on` also writes `[mining/trace]` lines to `mod.log` while the counts change: cells seen, promoted, why others were skipped, and read faults. A read fault switches natural mining off until you restart.

The idea and the working native contract come from the `junikka/sc-offline-mining` fork (GPL-3.0). It confirmed in game, on an earlier game build, that natural Titanium (Ore) is highlighted in mining mode, shows its composition, responds to the mining laser and can be extracted. This built-in is a port onto sco-core's row for 4.10.196 and is not yet tested in game: see the pull request for the steps. Leaving and returning to an area, depletion and persistence across restarts are unverified.

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

sc-offline can load plugins built with the [sco SDK](https://github.com/scubamount/sco-core/blob/main/sdk/README.md): native DLLs, Lua scripts and data packs. Loading is **on by default** (`plugins = on` in `sc-offline.ini`); sc-offline ships `discord` (on) and `creative` (off).

1. Keep `plugins = on` in `sc-offline.ini`.
2. Put each plugin in its own folder, `data/plugins/<id>/`, with its `plugin.ini` (for example `data/plugins/hello/plugin.ini` and `hello.dll`). The SDK's examples (`hello`, `greeter`, `travel_pack`) are ready to copy.
3. Start the game. `mod.log` reports what was found, after the startup lines:

   ```
   [plugin] 13 found, 12 loaded (plugins = on)
   [plugin] teleport <version> builtin loaded
   [plugin] spawn <version> builtin loaded
   ...
   [plugin] contracts <version> builtin loaded
   [plugin] creative <version> <kind> disabled
   [plugin] discord <version> native loaded
   [plugin] greeter 1.0.0 lua loaded
   ...
   ```

   Here the ten built-ins, `creative` (found, but not loaded while its `disabled` marker is there), `discord` and `greeter` make 13 found, 12 loaded. A plugin that can't load is listed with the reason (`refused: built for api 2.0`, `missing capability 'teleport'`, ...); the others still load. To switch one plugin off, put an empty file named `disabled` in its folder (the launcher's Plugins page does the same, see [launcher.md](launcher.md#the-plugins-page)).

With `plugins = off`, `mod.log` shows `[plugin] 10 found, 10 loaded (plugins = off)`: only the ten built-in plugins load (two more in a build with the optional bridges).

### Built-in plugins

sc-offline's features are ten plugins compiled into `dinput8.dll`: `teleport`, `spawn`, `crew`, `loadout`, `npc`, `quantum`, `build`, `contracts`, `mining` and `multiplayer`, loaded in that order. They load first, with `plugins` on or off, and are listed as `builtin` in the `[plugin]` report (`[plugin] loaded teleport <version> (api 1.1) built in`). Most run their feature's per-tick work from a `tick` subscription (`npc` has none), so a fault there switches that feature off (`[plugin] <id> ... crashed`) instead of crashing the game. Each built-in also draws its own tabs in the menu through sco-core's `sco.ui` service, and binds its keys there: `build` binds F6, and `teleport` binds F7 and F8. The menu looks and works as before. A fault while a tab is drawn switches off only the built-in that owns it, along with its tabs. Their commands are the ones plugins call through the SDK's `invoke`; a command that ran but couldn't do it (an unknown name, a list not loaded yet) answers `failed` with the reason:

| Command | Does | Key |
| --- | --- | --- |
| `teleport.save` | Saves where you're standing (`data/storage/teleport.db`); the reply names the spot | **F7** |
| `teleport.go` | Teleports to the saved spot; the reply says where you went, or why not | **F8** |
| `spawn.ship <class> <height>` | Spawns a ship `<height>` m (0 to 10000) above you, as the Vehicles tab does, and makes it the Crew & seats target; the status strip says when it's there. An unknown class answers `failed` | |
| `crew.target` | Pins the ship you're in as the target of the `crew.*` commands (without it they use the ship you're aboard) | |
| `crew.sit <seat>` | Puts you in the target ship's first seat whose name has these words (`pilot`, `turret left`), removing a crew NPC in it | |
| `crew.stand_all` | You and the NPCs `crew.fill` added on the target ship get out of the seats | |
| `crew.fill <npc>` | Puts an NPC of this archetype (`npcs.txt`) in every empty, usable seat of the target ship and says how many seats it skipped | |
| `crew.clear` | Removes the NPCs `crew.fill` added to the target ship | |
| `crew.power_on` | Sends the game's Flight Ready event to the target ship | |
| `npc.spawn <npc> <count>` | Spawns 1 to 10 NPCs of an archetype (`npcs.txt`) about 3 m in front of you, through the game pack's `game.actors`; an unknown archetype answers `failed` with the game pack's reason | |
| `npc.clear` | Despawns the NPCs `npc.spawn` made (the crew's NPCs aren't included; `crew.clear` removes those) | |
| `loadout.equip <items>` | Equips items from `items.txt` (separated by spaces or commas, one per slot; the other slots are empty, as in the gear menu) | |
| `loadout.wear <outfit>` | Wears a Squadron 42 outfit from `outfits.txt` | |
| `quantum.travel <place> <altitude>` | Teleports you to a place from the Travel tab, `<altitude>` m (100 to 20000) above the ground | |
| `quantum.bookmark <name>` | Teleports you to a saved spot | |
| `quantum.save_bookmark <name>` | Saves where you are as a named spot (`data/storage/quantum.db`); an empty name uses the zone's | |
| `quantum.scan` | Scans everything the game has loaded into `locations_found.txt` | |
| `build.toggle` | Build mode on or off | **F6** |
| `build.undo` | Removes the last object you placed | **Backspace** in build mode |
| `build.clear` | Removes everything you placed | |
| `build.place <object> <ahead>` | Places one buildable (`buildables.txt`) `<ahead>` m in front of you (0 = at your feet, up to 100) without entering build mode | |
| `contracts.status` | How many contracts are known, offered and running, and your wallet's balance | |
| `mining.status` | Whether natural mining is on, and how many ecosystem cells it has seen and promoted | |
| `multiplayer.status` | The session's state and how many other players are in it | |
| `multiplayer.leave` | Leaves the session (or stops hosting) | |
| `multiplayer.goto <player>` | Teleports you next to that player's ghost | |

Each built-in's commands are gated on a capability: `teleport`, `spawn.ship`, `loadout`, `quantum`, `build` and `contracts` (sc-offline's own rows, set in `SetFeatureCaps`); `game.vehicles.seats`, `game.vehicles.seat`, `game.vehicles.flight_ready` and `game.actors.despawn` for the `crew.*` commands; `game.actors.spawn_npc` and `game.actors.despawn` for `npc.spawn` and `npc.clear`; `game.mining` for `mining.status`; `sco.net` for `multiplayer.*`. One is missing when this game build's addresses for that feature aren't found, and the rest of the plugin system still starts. A built-in owns its id, command prefix and services, so a plugin folder named after one (`teleport`, `spawn`, `crew`, ...) is refused (`the id belongs to a built-in plugin`), whatever its kind.

The `spawn` built-in's tick runs the Vehicles tab's spawns and the seat job that puts you in a seat; the `crew` built-in's runs the Crew tab's own seat actions (still the spawner's seat code) and the seat jobs of the `crew.*` commands, which seat through `game.vehicles`. For plugin authors sco-core's game pack publishes `spawn.entities` 1.2 (the `spawn` built-in published it before, with the same table): a C function table to spawn an entity class near you, look up your entity and ship ids, ask whether an entity id still resolves in the game (`entity_alive`, 1.1), and (new in 1.2) move and turn an entity in the world frame or a zone's frame (`set_entity_transform`), without going through command replies. A plugin may move only what it spawned itself through `spawn_as` (1.2: `spawn_near_player` with the plugin's handle; what `spawn_near_player` and `spawn.ship` spawn belongs to nobody), forgotten when that plugin unloads, or your own vehicle once ASOP registers it as retrieved or delivered by ATC. A plugin built against 1.0 or 1.1 keeps working; one that uses a later function checks the table's `size` first. Its header is [`sc_spawn.h`](../external/sco-core/include/sc_spawn.h), shipped in sco-core's SDK; find it with `query_service` (sco_api 1.1). A plugin runs its own code in the game, so only install plugins you trust.

sco-core's game pack publishes `teleport.spatial` 1.0 (since game pack 0.1.0; the `teleport` built-in published it before, with the same table) ([`sc_spatial.h`](../external/sco-core/include/sc_spatial.h), shipped in sco-core's SDK): your position and orientation in the zone you're in, the zone an entity is in, a zone's name, and positions converted between a zone and the world or between two zones. Zones are the game's nested frames (star system > planet > city or station > ship > room); every id is the game's own 64-bit id and every position is in metres. The game pack feeds the zone tree behind it from the game (the `teleport` built-in used to); a zone you ask about that isn't in the tree is read on the spot, and an id that has streamed out answers 0. Like `spawn.entities`, it works from the game thread only.

### Menu tabs and hotkeys for plugins

A plugin can add its own menu tab (drawn with sc-offline's ImGui), a badge beside the tab title, and an overlay. It can also bind a free key chord such as `ctrl+alt+9` to any command. sc-offline keeps **M** and build mode's keys for itself. See [plugin-ui.md](plugin-ui.md) for the tab order, how to draw, and the full key table.

When you quit the game (the menu's Quit, or the `quit` console command), plugins get `game.exit` and are then unloaded, newest first and built-ins last, before the game exits. `mod.log` shows `[app] game closing (CSystem::Quit): game.exit, unloading plugins` followed by one `[plugin] unloaded <id>` line per plugin. If the game crashes or is killed (Task Manager, `taskkill`), plugins get no `game.exit` and aren't unloaded; don't rely on it to save anything that matters.

### The creative plugin

Noclip, god mode and infinite ammo are not part of `dinput8.dll` any more: they are `data/plugins/creative`, a plain plugin built only from the sco SDK's headers (`plugins/creative/`, CMake target `sc-offline-creative`) on sco-core's `game.creative` service. The folder ships with an empty `disabled` file, so the plugin is **off** until you turn it on in the launcher's Plugins page (and `plugins = on` in `sc-offline.ini`; see [the launcher](launcher.md)). A change applies at the next game start. Unloading the plugin switches every toggle it turned on off again.

| Command | Does |
| --- | --- |
| `creative.god <on>` | God mode on or off (held every half second, so it also applies after you respawn) |
| `creative.noclip <on>` | Noclip on or off |
| `creative.noclip_speed <speed>` | The noclip speed in metres per second (1 to 10000; the Player tab's slider is 1 to 500). The game's own speed returns when the plugin unloads |
| `creative.ammo <on>` | Infinite ammo for the weapons you carry |
| `creative.ship_ammo <on>` | Infinite ammo for every weapon on the ship you're aboard |
| `creative.status` | Which toggles are on |

The Player tab's Noclip, speed slider, God mode and Infinite ammo checkboxes, and the Vehicles tab's Infinite ship ammo checkbox, run these commands (through `sco_api`'s `invoke`); with the plugin off they put the checkbox back and say so in the status strip. What changed from the built-in versions: the commands were `ammo.infinite` and `ammo.ship_infinite` (a plugin's commands must start with its id, so they are `creative.ammo` and `creative.ship_ammo` now), god mode no longer starts on, and the ship toggle covers the ship you're aboard but not the Crew tab's target ship. Each toggle needs its capability: `game.creative.god_mode`, `game.creative.fly`, `game.creative.ammo`, `game.creative.ship_ammo`.

## Multiplayer (LAN or VPN)

Play alongside friends who also run sc-offline. Everyone runs their own offline game; one of you hosts a private session from the **Multiplayer** tab and the others join it by address. Other players show up in your game as **ghosts**: stand-ins your own game spawns and moves to where they are. Nothing connects anywhere until you press **Host** or **Join**, and the game's own netcode and servers aren't used or changed: the session runs on sco-core's `sco.net`, between sc-offline games only.

**Host.** Open the menu (**M**) > **Multiplayer**. Enter the name the others will see and a passphrase of at least 8 characters, keep the UDP port (64091) unless something else uses it, and press **Host**. Give the others your PC's LAN address (`ipconfig` lists it as IPv4 Address, like `192.168.1.20`), the port and the passphrase.

**Join.** Same tab: your name, the host's passphrase, the host's address and port, then **Join**. A wrong passphrase, a full session or no answer is shown under the session state. Up to 8 players per session.

**Where it works.** Your local network only: `10.x`, `172.16-31.x`, `192.168.x` and link-local addresses. For friends elsewhere, use a VPN that puts you on one network and add its range to `multiplayer_allow` in `sc-offline.ini` (Tailscale: `multiplayer_allow = 100.64.0.0/10`); every address in that range can reach a session you host, so allow only the range your VPN uses. With `block_network` on, the launcher's firewall rule for `StarCitizen.exe` leaves those networks open and lets PCs on your subnet reach the game over UDP; the internet stays blocked (see [launcher.md](launcher.md#pc-changes)). Messages are signed with the passphrase, so nobody without it can join or change them, but they **aren't encrypted**: anyone on the network path can read them. They hold only your chosen name, the NPC and ship classes you show as, and where you are.

**What you see.** The tab lists everyone in the session and how each ghost is doing: streaming in (a fresh spawn takes a few seconds), shown, **not shown** while that player is more than 10 km from you, or **parked** when they're in a zone you haven't loaded (another system, say): the ghost is moved far out of sight and comes back when you're both in the same place. Positions travel as zone names with positions inside each zone, so ghosts line up wherever both games have the zone loaded. A player aboard a ship shows as that ship (when the ship's class can be read and spawns in your game; **Show other players' ships** turns this off); otherwise as an NPC: the class they picked under **Others' ghosts** when your `npcs.txt` has it, else your own pick (default: an unarmed civilian). **Go to** teleports you next to a ghost.

**Limits.** Ghosts are visual only: no shared physics or collisions you can rely on, no damage, no shared missions, contracts, inventory, wallet or loadouts, and no voice or chat. Each player's world (NPCs, ships they spawned, build pieces) stays their own; only the player and the ship they're aboard are shown to others. A ghost's pose follows about 0.1 s behind. Sessions use IPv4. Settings are kept in `data/storage/multiplayer.db`; the passphrase only when you tick **Remember the passphrase** (stored unencrypted), and it's never written to `mod.log`. `multiplayer = off` in `sc-offline.ini` removes the tab. `mod.log` lines start with `[multiplayer]` and `[net]`.

## Bridges (optional builds only)

Two more built-ins link the game to another game on the same PC through sco-core's `sco.ipc` (local shared memory for your Windows user, nothing over the network): **TitanLink** (`titanlink`, Titanfall 2 through Northstar: F9 starts pilot mode, the **Titanfall** tab) and the **voxel bridge** (`voxel_bridge`, a Minecraft-style voxel game whose solid blocks become crates: Ctrl+F9, the **Voxel** tab). They are **not in the release**: they're compiled in only with the CMake options `SCO_BRIDGE_TITANLINK` and `SCO_BRIDGE_VOXEL` (off by default), so the release's `dinput8.dll` has neither, and even in such a build a bridge does nothing until you open it. Each needs its other half in the other game, which this repository doesn't ship. See [bridges.md](bridges.md) for what they do, their settings, the wire the other side speaks, and what isn't ported yet.

## Not available offline

These need RSI's servers, and the mod doesn't replace them:

- Character creation and customization: the main menu is skipped. Not planned ([#24](https://github.com/scubamount/sc-offline/issues/24)).
- The mobiGlas vehicle manager lists stored and retrieved ships but can't select them. Use a ship terminal (see [Ship terminals, hangars and ATC](#ship-terminals-hangars-and-atc)) or the **Vehicles** tab.
- Your account's ships, items and hangar.
- Choosing a spawn location. You start at the game's own spawn, or over Daymar with `start = Daymar` (see [launcher.md](launcher.md#sc-offlineini)).

## Not verified in game

The SDK conversion (every feature on a `game.*` service or a sco-core row, the `creative` plugin, the launcher's Plugins page) has CI and gate checks but **no in-game run yet**. [status.md](status.md) lists what each converted feature still has to show, and links sco-core's table for the services.

ChrisWareOffline 0.9.0-rc1 also flagged these as built after its last in-game test: the energy-weapon top-up, NPC deletion through the entity handle, Stand up, the real-building prefab preview, and Pyro and Nyx names after a fresh scan.
