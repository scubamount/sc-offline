# Changelog

## Unreleased

### Launcher
- `sc-offline.exe` replaces `launch_offline.bat`. Same steps (copy the mod in, start the game,
  remove the mod when every `StarCitizen.exe` has exited), plus:
  - finds the game itself: `Roberts Space Industries\StarCitizen\<channel>\Bin64` on every
    fixed drive, or `game =` in `sc-offline.ini`, or `--game <folder>`;
  - copies and removes the mod through a separate helper process, so closing the launcher
    window early still takes the mod out once the game exits; the helper alone is elevated
    (one UAC prompt) when the game folder needs it, and the game never runs as administrator;
  - reads the start ship, start location, boot map and channel from `sc-offline.ini`.
- The default boot map is now `PU_All` (every star system, so Travel reaches Pyro and Nyx).
  Set `boot_map = PU` when playing the repo-root prebuilt DLL.
- Releases ship `sc-offline-<tag>.zip`: launcher, DLL, `sc-offline.ini`, `sc-offline.sh`, `data/`
  and the docs, ready to extract and play.

### Linux (experimental, untested)
- `sc-offline.sh` runs the launcher inside a Star Citizen Wine prefix (LUG Helper layout),
  with `WINEDLLOVERRIDES=dinput8=n,b` so Wine loads the mod instead of its own `dinput8`.

## sc-offline 0.2.0-rc1

This repository's build of upstream 0.9.0-rc1 (below) with the Squadron 42 tab on top.
Release candidate: compiled and string-checked, not yet played.

### Squadron 42 tab
- An eighth tab, between Build and Menu: outfits, the SQ42 pilot preset, four SQ42 settings,
  a one-shot buildable spawner, SQ42 ships, Bengal A / B with the Vanduul wing, and a console.
- Ships from this tab use the Vehicles tab's seat rules and become the Crew tab's target ship.

### From sc-offline 0.1.x, carried forward
- The fleet manager offers all 1102 ships in `ships.txt` (it stopped at 1024).
- The gear menu and the outfits share one loadout loader and one temp-file counter.
- `tools/check.sh` checks the source on macOS / Linux before CI does.

## 0.9.0-rc1

First release candidate of the seats, crew, travel and menu update. Debugging and analysis code
from development is removed, and the release build is optimized.

### Menu
- New tabbed menu: Player, Travel, Vehicles, Crew, NPCs, Build and Menu.
- Dark green theme, Bahnschrift font (Segoe UI fallback), and a status line at the bottom that
  shows what the mod just did.
- Optional background image: save `data\menu_background.png` (or `.jpg`). Darkness and image
  position are set in the Menu tab. The file is ignored by git.
- The version shows in the title bar, the Menu tab and the first line of `mod.log`.

### Ships and seats
- Choose where you board a spawned ship: pilot seat, a seat by name, pick after it spawns, or none.
- Optionally remove the NPC in the seat you want instead of taking another seat.
- Crew tab: every seat on the ship and who is in it (you, NPC, empty). Sit here, Stand up,
  Remove NPC and Add NPC per seat; Fill empty seats, All NPCs stand up and Remove all NPCs.
- Power on now sends the game's own Flight Ready event to the pilot dashboard, including on ships
  whose dashboard is a separate part (F8C, Moth...). It falls back to pressing R only if needed.
- Infinite ship ammo: refills the magazines of the ship you're in. Every magazine on the ship is
  also topped up twice a second, for energy weapons that drain another way.

### Travel
- Travel tab with places grouped by star system: planets, moons, Lagrange points, comm arrays,
  jump points and landing zones, for Stanton, Pyro and Nyx.
- Scan the game for places finds everything loaded in a few seconds. Interiors and small zones are
  hidden unless you ask for them.
- Named saved spots, grouped by system, stored in `data\bookmarks.txt`. F7/F8 still work.
- Teleports refuse to cross star systems instead of leaving you stuck in empty space.

### NPCs and building
- Removing NPCs, kicking crew, build mode's undo and Clear base now work. The game wants an entity
  handle, not an id. NPCs the game refuses to delete offline are taken out of their seat and moved
  far out of range instead.
- Build mode previews prefabs with a flag while you move the camera and the real building when you
  hold still. Clicking keeps the previewed building.

### Build
- Release x64 is built with full optimization, link-time code generation and the static C runtime,
  so the DLL runs without the Visual C++ redistributable.

### Needs testing before the final release
These were built after the last in-game test and haven't been confirmed yet:
- The energy weapon top-up (does an energy weapon still run dry with infinite ship ammo on?).
- Proper NPC deletion through the entity system's handle lookup. If it doesn't take, the
  send-far-away fallback (confirmed working) still removes them from the world.
- Stand up and All NPCs stand up.
- The real-building prefab preview in build mode.
- Pyro and Nyx system names after a fresh place scan.

### Known limitations
- NPCs added to seats sit there but don't fly the ship or operate turrets.
- Power on uses Flight Ready, so some systems (engines, weapons) may start off on some ships.
  Per-system power options are planned.
- Places in Pyro arrive in orbit until their radii are added to `data\locations.txt`.
