# Changelog

## Unreleased
- Your saves move into sco-core's `sco.storage`: the F7 / F8 spot, the Travel tab's saved spots and your wallet are kept by the `teleport`, `quantum` and `contracts` built-ins in `data\storage\teleport.db`, `quantum.db` and `contracts.db` (SQLite; a save is all or nothing, so a crash can't leave half of one). Nothing changes in game. On the first start `spawn.txt`, `bookmarks.txt` and `wallet.txt` are imported (`[storage] imported spawn.txt into teleport` and so on in `mod.log`) and left in place. `wallet.txt` is still written and can still be edited by hand: a `wallet.txt` changed since the mod wrote it wins, and deleting it still takes you back to the starting amount. If storage can't be used, each feature says so in `mod.log` and uses its `.txt` file as before. See `docs/data-files.md#saves`. sco-core is pinned at its main with storage, the UI registry and DataCore AddRecord.
- The menu is now a shell over sco-core's `sco.ui` service. Each built-in draws its own tabs (Player and Squadron 42 from `loadout`, Travel from `quantum`, Vehicles from `spawn`, Crew, NPCs and Build), and F6, F7 and F8 are the `build` and `teleport` built-ins' hotkeys. The tabs, their order and the keys are the same as before. Plugins can add their own tab with a badge, an overlay and hotkeys (`ctrl+alt+9` and similar); see `docs/plugin-ui.md`. A fault while a plugin's tab is drawn switches off only that plugin. Every key press bound through `sco.ui` is logged as `[hotkey] F7 -> teleport.save: OK ...`. sco-core is pinned at its main with `sco.ui`.
- The Gladius' new quantum drive no longer depends on one exact game version. Its game data is now a data pack, `data/builtin/quantum/datacore/quantum_drive.toml`, which names the records and fields it changes instead of byte offsets in one `Game2.dcb`; sco-core's pack loader applies it as the game loads its data. The old byte patch was made for 4.10.193 and switched itself off on 4.10.196; the pack applies to 4.10.196's game data too. `mod.log` reads `[+] new quantum drive: game data patched as it loaded (pack quantum_drive: 508 operations), load ok in N ms`, or, when a game update removed something the pack uses, names the record or field that is missing. sco-core is pinned at its `sdk-v1.1.0` release (5e3d463).
- The rest of sc-offline's features are built-in plugins now, after teleport and spawn: `crew`, `loadout`, `npc`, `ammo`, `quantum`, `build` and `contracts`. They work as before, from the menu and their keys (F6 for build mode). Each runs its per-tick work from its own tick subscription, so a fault in one switches that feature off instead of crashing the game, and each gives plugins commands (`crew.sit`, `npc.spawn`, `loadout.equip`, `ammo.infinite`, `quantum.travel`, `build.toggle`, `contracts.status` and more; see `docs/features.md`) gated on a capability of the same name. `mod.log`'s `[plugin]` report lists nine built-ins (`9 found, 9 loaded` with `plugins = off`), and on quit they unload after every plugin. A plugin folder named after any of them is refused.

- Plugins can ask where things are: the teleport built-in publishes a `teleport.spatial` service (`src/builtins/spatial_service.h`) with your position and orientation, the zone an entity is in, zone names, and positions converted between zones and the world. It runs on sco-core's zone tree, fed from the game every tick. `spawn.entities` is now 1.1 with `entity_alive(id)`; plugins built against 1.0 keep working. Nothing changes in game. sco-core is pinned at its main with the engine math and zone tree.
- The ship spawner is now a built-in plugin, `spawn`, the second after teleport. Spawning works as before. Plugins get a `spawn.ship <class> <height>` command and a `spawn.entities` service (`src/builtins/spawn_service.h`) to spawn entities near you directly, and a fault in the spawner's per-tick work now switches the spawner off instead of crashing the game. `mod.log`'s `[plugin]` report lists `spawn <version> builtin loaded`, and with `plugins = off` it reads `2 found, 2 loaded`. A plugin folder named `teleport` or `spawn` is refused, whatever its kind.
- Every detour (about 35 of them) now goes through sco-core's `sco/hook.h` instead of sc-offline's own cave allocator; nothing changes in game. A detour that can't be placed logs `[!] detour at 0x...: <reason>`. sco-core is pinned at its API 1.1.
- Plugins now get `game.exit` and are unloaded when you quit from the game's menu. The game ends itself there without the usual Windows quit message, so they never did before; sc-offline now runs the shutdown from the game's own Quit. `mod.log` shows `[app] game quit hook: installed` at startup and `[app] game closing (CSystem::Quit): ...` on quit. A crash or a killed game still sends no `game.exit`.
- The build is now CMake instead of a Visual Studio solution: open the folder in Visual Studio 2026, or `cmake -S . -B build -A x64` and `cmake --build build --config Release`. `sc-offline.slnx` and the `.vcxproj` files are gone; nothing changes for players. See `docs/build.md`.
- sc-offline now runs on [sco-core](https://github.com/scubamount/sco-core)'s host kit (a git submodule in `external/sco-core`: clone with `--recurse-submodules`). The features work as before. `mod.log` gains a `[core] signatures: N/N OK` line and a `[plugin]` report, and status-strip messages are logged as `[status]` instead of `[ship]`.
- F7 and F8 are now the `teleport.save` and `teleport.go` commands of a built-in `teleport` plugin, the first of sc-offline's features on sco-core's plugin API; plugins can call them too. They work as before. `mod.log`'s `[plugin]` report lists `teleport <version> builtin loaded`, with plugins on or off.
- The plugin system no longer depends on teleport: on a game build where teleport's addresses aren't found, the host kit and plugins still start.
- **Plugins**, off by default: with `plugins = on` in `sc-offline.ini`, plugins in `data\plugins\<id>\` built with the sco SDK (native, Lua or data packs) are loaded and listed in `mod.log`. See `docs/features.md`.

## 0.7.0 (2026-10-07)

- **Safer self-update** ([#31](https://github.com/scubamount/sc-offline/issues/31)). Each release zip now carries `manifest.json`: the version, tag, commit and every shipped file with its SHA-256. CI writes it and checks the finished zip against it. The launcher only installs files the manifest lists, with matching hashes, from a release newer than itself; paths with `..` are refused. Downloading and checking run with normal rights; for a Program Files install only the file swap asks for administrator rights, with no network. Each file is flushed to disk and checked again after it's moved; files locked by antivirus are retried. The update waits while the game runs, checks free disk space, and only times out on a stalled download. The new launcher must pass `--self-test` or the old files go back. New setting `update_channel = stable | prerelease`. `docs/launcher.md` explains how to check a download by hand.
- **Discord status** ([#34](https://github.com/scubamount/sc-offline/issues/34)). While the game runs, your Discord profile shows **Playing sc-offline** with the version, time played and buttons to the sc-offline Discord and download page. It talks only to the Discord app on your PC. Turn it off with the **Show on Discord** box in the window or `discord_presence = off`.
- Project activity (releases, merged PRs, issues opened or closed) now posts to the project's Discord channel ([#30](https://github.com/scubamount/sc-offline/issues/30)).
- Docs and `sc-offline.ini`: `start_ship` only applies with `start = Daymar`, the wallet lives in `data\wallet.txt`, and a new **Not available offline** list in `docs/features.md` covers ASOP, the vehicle manager and character creation ([#26](https://github.com/scubamount/sc-offline/issues/26)).
- README: 0.6.1 has been played in game on Windows; the "nobody has played it" note is gone. Linux through Wine is still untested.

## 0.6.1 (2026-10-07)

- **Online-safe light now updates while you play** ([#22](https://github.com/scubamount/sc-offline/issues/22)). It used to freeze while **Play** was running, so it stayed green with the game open and only changed at the end. It now refreshes every 1.5 seconds and turns red as soon as the mod is copied in or `StarCitizen.exe` is running.
- **Wider window** (1160 px instead of 820 px), so log lines wrap less, and a **SCUBAMOUNT** watermark.
- `--window` opens the window from a terminal or under Wine/Proton.
- The `data` folder is created on first run, so a fresh unzip no longer warns that the folder can't be written.

## 0.6.0 (2026-10-07)

- **Launcher window** ([#20](https://github.com/scubamount/sc-offline/issues/20)). Double-clicking `sc-offline.exe` opens a window with **Play**, **Status**, **Update**, **Install**, **Uninstall**, **Open settings** and **Open logs**. Each button runs the same CLI command, its output shows in the window, and its questions get Yes/No buttons. An **Online-safe** light is green when the mod is out of Bin64 and no PC changes are left, red with a list otherwise; **Uninstall** is enabled only when there is something to undo. Every CLI command works as before; `--console` keeps the old double-click behaviour.

## 0.5.1 (2026-10-07)

- **Firewall also blocks the RSI Launcher and CIG's `CrashHandler.exe`** while you play ([#17](https://github.com/scubamount/sc-offline/issues/17)), recorded and removed with the existing rule. `sc-offline.exe` stays online for updates.
- **Crash reports** ([#18](https://github.com/scubamount/sc-offline/issues/18)): after a crash, offer a zip of the logs in `data\crash-reports` with the RSI handle, GEID/account numbers and Windows user name redacted, and a prefilled bug form. Nothing is uploaded; `.dmp` files are left out. New setting `crash_reports = on | off`.
- **Administrator rights only where needed** ([#16](https://github.com/scubamount/sc-offline/issues/16)): self-update of a protected folder and deleting protected session logs now ask Windows once, for that step only. The launcher and the game still run with normal rights. Warns when `data` can't be written.

## 0.5.0 (2026-10-07)

- **The launcher updates itself** ([#14](https://github.com/scubamount/sc-offline/issues/14)). On `play` and `status` it checks GitHub's latest full release (3-second timeout, never blocks play) and asks before installing. It downloads the release zip, checks it against GitHub's SHA-256 digest, swaps the program files with a journal in `data\update\applied.txt` (rolled back on failure, or on the next run after a crash), keeps `sc-offline.ini` and your saves, appends new ini settings commented out, and restarts itself. New command `sc-offline.exe update`, new setting `check_updates = on | off`.

## 0.4.2 (2026-10-07)

- **Offer to delete this session's game logs** ([#12](https://github.com/scubamount/sc-offline/issues/12)). When the game closes, the launcher lists the `Game.log`, `logbackups` and `Crashes` files written during the session and asks "Delete these files?" then "Are you sure?"; anything but `y` keeps them. Older logs are never touched. New `sc-offline.ini` setting `clean_logs = ask | off`.
- The leftover-changes prompt now reads a whole line, so its Enter no longer answers the next question.

## 0.4.1 (2026-10-07)

- **The launcher finds the game in more places.** After `game =`, it now tries the folder it found last time (`data\game-path.txt`), where the RSI Launcher says the game is (its install entry and the paths in `%APPDATA%\rsilauncher`), more usual folders (`Game\Star Citizen\StarCitizen`, `Games\StarCitizen` and similar), a four-level search of every fixed drive, and finally a folder picker. `game =` also accepts the folder that holds `StarCitizen`.

## 0.4.0 (2026-10-07)

- **The launcher sets up offline play itself.** Each play, its helper blocks `StarCitizen.exe` in Windows Firewall, adds the EAC hosts line and renames `EasyAntiCheat_EOS.exe`, then undoes exactly those changes when the game closes. New `sc-offline.ini` switches `block_network`, `eac_hosts`, `eac_rename` (all on). Changes are recorded in `%ProgramData%\sc-offline\pc-changes.txt`; after a crash, `status` lists them, `uninstall` undoes them and `play` offers to.
- The manual EAC steps are gone from the README's Setup; Windows now asks for administrator rights once per play.

## 0.3.0 (2026-10-07)

- **Renamed to sc-offline.** The original ChrisWareOffline project has shut down; this is now an independent project based on ChrisWareOffline 0.9.0-rc1 (GPL-3.0). Window title, menu title, `mod.log` and the launcher say sc-offline. Removed the original project's Discord links.
- One version number for the DLL and launcher (`SCO_VERSION` in `src/version.h`), replacing `0.9.0-rc1 / sc-offline …`. The launcher still recognizes older builds when checking or uninstalling.
- Removed the original author's prebuilt `dinput8.dll` from the repository root. Its source was never published and no release used it.
- Solution and project renamed: `sc-offline.slnx`, `src/sc-offline-dll.vcxproj`.
- Bug reports now go to GitHub Issues, with a template asking for the logs.

## 0.2.0-rc5 (2026-10-07)

- Removed `data/scripts/` (229 Star Citizen Subsumption mission XML files) and rewrote the repository history so no commit contains them. They are CIG's content and are not ours to distribute.
- `contract_scripts.txt` now lists only the 796 contracts that need no mission script; contracts that depended on the removed scripts are no longer offered.
- Earlier release zips (rc2 to rc4) contained those files and were withdrawn.

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
