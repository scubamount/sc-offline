# Changelog

## sc-offline 0.2.0-rc4 — 2026-10-06

Second parity pass against the original author's DLL (REA 4.1.0 + Ghidra), a launcher with
subcommands and self-checks, and the documentation split into a short README plus `docs/`.
Release candidate: compiled and checked, not yet run in game.

### Squadron 42 tab
- **Spawn** lists the whole `[sq42]` group again. The search box started as `sq42`, which matched
  item names and hid 59 of the 60 entries; it now starts empty and searches within the group.
- **Ships** are greyed out when this game build doesn't have the class, instead of failing after
  the click. Your own ships spawn 30 m up by default, like the original (was 20 m).
- **Settings** checkboxes are greyed until the game's current value has been read.
- **Console** has a **Run** button and is disabled until the game's console is found; commands
  can be 255 characters (was 191).

### Ships and seats
- Sitting in a pilot seat with Flight Ready off now says "Press R (Flight Ready) to power up."
- `mod.log` lists every seat of a spawned ship once (`[ship] N seats: name(priority, taken)`),
  the names "Board in a seat by name" matches against.

### Build
- The reach tooltip says where objects land when you look at the sky: under the point that far out.

### Launcher
- Commands: `sc-offline.exe [play|install|uninstall|status|help]`, plus `--dry-run` (print every
  step, change nothing) and `--skip-eac-check`. A double-click is `play`, as before.
- Self-checks on every run: which `dinput8.dll` it is (source build version, or the original
  author's prebuilt by SHA-256, with a `boot_map = PU` warning for the prebuilt), whether the game
  updated since the last play (`build_manifest.id`), a mod left in the game folder after a crash,
  and Easy Anti-Cheat (`EasyAntiCheat_EOS.exe` present, hosts-file block). An active EAC stops
  `play` and `install` with the fix printed.
- `default_1.xml` is backed up before the launcher replaces it and restored afterwards (it used
  to be overwritten and left behind). Another mod's `dinput8.dll` is set aside and put back.
- `Bin64\sc-offline.installed` records what was installed, so `uninstall` knows what to undo.
- Everything printed also goes to `data\launcher.log`. Exit codes: 0 ok, 1 error, 2 EAC active,
  3 the game is running.
- `sc-offline.sh` takes the same commands and also finds Lutris and Steam/Proton prefixes
  (Flatpak Steam included) and their Wine, checks `/etc/hosts`, and `--dry-run` starts nothing.
  `--prefix` / `--wine` override the guesses.

### Repository
- Documentation: a short README for players; features, data files, launcher, Linux and build
  notes moved to `docs/`. Stale release history is gone; corrected: the download is about
  1.4 MB, not 1 GB, and of 2153 listed contracts 1657 have their scripts shipped and 491 are offered.
- `data/missions.txt` removed: no build ever read it.
- CI: dead NuGet steps removed; runner images pinned (`windows-2025-vs2026`, `ubuntu-24.04`); a
  newer push cancels the older branch build. The release zip now carries `docs/`.
- `.gitattributes` fixes line endings per file type (`sc-offline.sh` stays LF on Windows clones).
- `tools/check.sh` screens `launcher/` for MSVC C2712 too.
- CHANGELOG entries carry their release dates.

## sc-offline 0.2.0-rc3 — 2026-10-06

Brings ours in line with the original author's prebuilt DLL, based on a Ghidra decompile of it.
Release candidate: compiled and checked, not yet run in game.

### Outfits
- Outfits that don't name a head (all the Navy, Marine, bridge, deck and medic ones) get the
  default face instead of stripping the head. Named heads get eyes and teeth when the outfit
  lists none.
- Hats, eye and head accessories, Vanduul horns and jewellery go on the head.
- The belt / vest layer (`Clothing_Torso2`) sits inside `Clothing_Torso_1`, and clothing no
  longer nests inside armor. An undersuit is added when an outfit names armor but no undersuit.
- Unknown item names in `outfits.txt` are skipped at load, and outfits left empty are dropped;
  `mod.log` counts both.
- Picking an outfit only selects it; **Wear SQ42 outfit** wears the selection. The built-in
  `sq42_pilot_*` preset is gone (none of its item names exist in the original DLL).
- The **SQ42 visor HUD** checkbox now applies to the gear menu's **Equip** too, and shows
  before the outfit list has loaded.

### Menu
- Typing in the ship search box selects the first matching ship, so a filtered-out ship can't
  be spawned by accident.
- The Squadron 42 spoiler warning has **OK** and **Back**; Back returns to the first tab.
- The Bengal rows are labelled for what they do: **Bengal (UEE)** and **Bengal + Vanduul wing**.

### Missions
- The AI debug-nodes console command uses the original's name `SubsumptionEnableDebugNodes`
  unless only the `ai_` form exists in this game build.

## sc-offline 0.2.0-rc2 — 2026-10-05

The launcher release. Same mod as 0.2.0-rc1; how you start it changed.
Release candidate: compiled and checked, not yet run on Windows or Linux.

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

## sc-offline 0.2.0-rc1 — 2026-10-05

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
