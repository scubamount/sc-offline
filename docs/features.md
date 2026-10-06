# Features

Everything runs through a single in-game menu, which **M** opens. It has eight tabs: **Player**, **Travel**, **Vehicles**, **Crew**, **NPCs**, **Build**, **Squadron 42**, **Menu**. The Squadron 42 tab was written for this repository. The other seven come from upstream ChrisWareOffline 0.9.0-rc1.

Most of the lists below are plain text files in `data/`, and the counts are taken from those files. Remove lines from a file and its list gets shorter. See [data-files.md](data-files.md).

## Player

- Noclip, god mode, infinite ammo.
- Gear menu: equip any of 1489 items, sorted into ten slots (`items.txt`).
- Wallet: your aUEC balance is loaded from `data/wallet.txt` when you spawn and saved back as you spend. Edit that file to change the starting amount.

## Travel

- Teleport to planets, moons, stations, Lagrange points and jump points, grouped by star system (`locations.txt`).
- **Scan** lists everything the game has loaded and writes it to `locations_found.txt`. Interiors and small zones are hidden unless you ask to see them.
- Save named spots of your own (`bookmarks.txt`). **F7** saves one quick position and **F8** takes you back to it (`spawn.txt`).
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

`contract_scripts.txt` lists 2153 contracts. A contract can only run when every mission script it needs ships in `data/scripts/`, and **1657** of them meet that today. The mobiGlas list is shorter again, because it also leaves out Pyro and Nyx contracts and anything named test, debug or tutorial. With the shipped files, **491** contracts are offered (`src/contracts.cpp` `Listable`). `mod.log` records the counts at startup: `[contracts] N contracts known; M run ...`.

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

## Upstream items still to confirm

Upstream 0.9.0-rc1 flags these as built after its last in-game test: the energy-weapon top-up, NPC deletion through the entity handle, Stand up, the real-building prefab preview, and Pyro and Nyx names after a fresh scan.
