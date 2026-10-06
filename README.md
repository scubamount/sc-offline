# Star Citizen Offline Mod (ChrisWareOffline)

<img src="images/banner.webp" alt="Star Citizen Offline Mod — offline single-player mod menu for Star Citizen" width="100%">

Offline single-player mod menu for **Star Citizen**.

> [!WARNING]
> This mod may result in a ban. Use it at your own risk. It is for **offline single player only**.

**Jump to:** [Which DLL do I run?](#which-dll-do-i-run) · [Features](#features) · [Requirements](#requirements) · [Setup](#setup) · [Play](#play) · [Linux](#linux-experimental) · [Squadron 42](#squadron-42-tab) · [Controls](#controls) · [Data files](#data-files-and-environment-variables) · [Build](#build) · [Releases](#releases) · [Update](#update) · [Go back online](#go-back-online) · [Notes](#notes) · [Credits](#credits)

---

## Which DLL do I run?

`sc-offline.exe` copies **the `dinput8.dll` sitting next to it** into your game — nothing
else. So the DLL in that folder is the mod you play.

Two builds of that file exist, and they are not interchangeable:

| | Release zip — `sc-offline-<version>.zip` | Prebuilt — `dinput8.dll` at the repo root |
| --- | --- | --- |
| Where it comes from | Compiled from the C++ in `src/` by CI, together with the launcher | The mod's original author's tree, committed here as-is |
| When it changes | Every `v*` tag | Only when someone replaces it |
| Best for | Playing this repository's version | Going back to the original author's build |

- **Just playing?** Download the zip from [Releases](https://github.com/scubamount/sc-offline/releases).
  It has the launcher, the DLL and `data/` together. Nothing to decide.
- **Want the original author's build?** Download
  [the repo-root `dinput8.dll`](https://github.com/scubamount/sc-offline/raw/main/dinput8.dll),
  copy it over the one in your extracted zip, and set `boot_map = PU` in `sc-offline.ini` — that DLL doesn't know
  `PU_All`.
- **Building it yourself?** See [Build](#build). The output lands in `x64/Release/`.

They really are different binaries, not copies: the prebuilt one came from a private
tree that was never published, while the source build is what this repository
develops.

---

## Features

Everything runs through one in-game menu. Press **M** to open it. Its tabs: **Player**, **Travel**,
**Vehicles**, **Crew**, **NPCs**, **Build**, **Squadron 42**, **Menu**. Everything except the Squadron 42
tab comes from upstream ChrisWareOffline 0.9.0-rc1; see [Credits](#credits).

| Feature | What it does |
| --- | --- |
| **Vehicles** | Spawn any vehicle in the game — 1102 entries, from the Bengal to the Idris-P. Pick the seat you start in (pilot, a seat by name, or choose later), optionally remove the NPC sitting there, and power the ship on. Infinite ship ammo lives here too. |
| **Crew** | Every seat on your ship and who is in it. Sit anywhere, make NPCs stand up, remove them, or add your own. |
| **Travel** | Teleport to planets, moons, stations, Lagrange points and jump points, grouped by system; save your own named spots. Teleports stay inside the star system you're in. |
| **NPCs** | Spawn any NPC archetype (2239 entries) in front of you, and remove them again. |
| **Wallet** | Your aUEC balance is read from `data/wallet.txt` when you spawn and written back as you spend it. Edit the file to set the starting amount. |
| **Build (F6)** | Free build mode, 3728 buildable entries in 18 groups. Prefabs preview as a flag while you move and as the real building when you hold still. **Undo** and **Clear base** remove what you placed. |
| **Player** | Noclip, god mode, infinite ammo, and the gear menu: equip any item (1489 entries, ten slots). |
| **Outfits** | Wear one of the 35 Squadron 42 outfits (143 named pieces) and cast members' heads. |
| **Squadron 42 tab** | Spoiler-gated tab — see [below](#squadron-42-tab). |
| **Contracts** | 2153 generated contracts with their mission scripts wired up locally. |
| **Teleport** | Save a position (F7) and teleport back to it (F8). |
| **Menu** | Optional background image (`data/menu_background.png`) and its settings. |

---

## Screenshots

![A Vanduul holding a gun on a desert planet, with a line of Vanduul and a large ship behind it](images/screenshot.webp)

---

## Requirements

- **You must own Star Citizen.** This is a mod for the game, not a standalone app.
- Windows. The mod is a `dinput8.dll` that the game loads.
  **Linux** (through Wine) is **experimental and untested** — see [Linux](#linux-experimental).
- About 1 GB of disk for the download.
- Administrator rights, if your game folder needs them for the copy. The launcher then asks
  once, for a small helper that copies and removes the mod; the game itself runs normally.
- **Easy Anti-Cheat must be off.** See [Setup](#setup).

---

## Setup

### 1. Disable Easy Anti-Cheat

The mod will not run while Easy Anti-Cheat (EAC) is active. You only do this once.

> [!NOTE]
> With EAC disabled, the game cannot join online servers. See [Go back online](#go-back-online) to undo these steps.

**a. Rename the EAC executable**

Without the `.exe`, the EAC service cannot start, so nothing attaches to the game. Open PowerShell as administrator and run:

```powershell
ren "C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe" EasyAntiCheat_EOS.exe.bak
```

**b. Block the EAC download server**

This stops the launcher from downloading fresh EAC files.

1. Open Notepad as administrator.
2. Open `C:\Windows\System32\drivers\etc\hosts`.
3. Add this line at the bottom and save:

   ```text
   127.0.0.1 modules-cdn.eac-prod.on.epicgames.com
   ```

4. Open PowerShell and flush the DNS cache:

   ```powershell
   ipconfig /flushdns
   ```

### 2. Install the mod

1. Open [Releases](https://github.com/scubamount/sc-offline/releases) and download
   `sc-offline-<version>.zip` from the newest release.
2. Right-click the ZIP and choose **Extract All**.
3. Place the extracted folder anywhere you like, such as your Desktop.
4. Keep all files together in that folder.

The mod does **not** go in your game folder. `sc-offline.exe` copies it in when you play and
removes it when you close the game.

**Finding the game:** the launcher looks for `Roberts Space Industries\StarCitizen\LIVE` on
every drive by itself. If yours lives somewhere else, open `sc-offline.ini` in Notepad and set
the `game` line to your StarCitizen folder:

```ini
game = D:\Games\Roberts Space Industries\StarCitizen
```

`sc-offline.ini` also sets the start ship, the boot map, and the channel (`LIVE`, `PTU`, ...).

---

## Play

1. Close the RSI Launcher and the game.
2. Double-click `sc-offline.exe`.
   - It prints which game folder it found. If Windows asks for administrator rights, say yes:
     that's the helper that copies the mod in and takes it back out.
3. Wait for the game to load you in.
4. Press **M** to open the menu.

> [!IMPORTANT]
> When you close the game, the mod is removed from your game folder — even if the launcher
> window was closed first. Check that `LIVE\Bin64\dinput8.dll` is gone before going online.

### Linux (experimental)

> [!WARNING]
> **Untested.** Nobody has run the mod under Wine yet. It's here so a Linux player can try it
> and report back; if it works, this warning goes away.

The game has to be installed the usual Linux way, in a Wine prefix — the
[LUG Helper](https://github.com/starcitizen-lug/lug-helper) is what `sc-offline.sh` expects.

1. Do the [Setup](#setup) steps inside Linux terms:
   - the hosts line goes in `/etc/hosts` (`sudo nano /etc/hosts`);
   - rename `EasyAntiCheat_EOS.exe` under your prefix's `drive_c/Program Files (x86)/EasyAntiCheat_EOS/`
     if it exists there.
2. Extract the release zip anywhere, then run:

   ```bash
   ./sc-offline.sh
   ```

`sc-offline.sh` finds your prefix (`$WINEPREFIX`, then the LUG Helper's saved prefix, then
`~/Games/star-citizen`) and its Wine runner, sets `WINEDLLOVERRIDES=dinput8=n,b` so Wine loads
the mod instead of its own `dinput8`, and starts `sc-offline.exe` inside the prefix. From there
everything is the same as on Windows. Override either guess with `WINEPREFIX=...` or `WINE=...`.

It starts Wine directly, **not** through the LUG Helper's `sc-launch.sh`, so any environment
that script sets for you (DXVK, esync/fsync and similar) is not applied. If the game misbehaves
here but runs fine from the LUG Helper, export those same variables before `./sc-offline.sh`
and say so in your report.

When you report back, include the launcher's output, `data/mod.log`, and the game's `Game.log`.

---

## Squadron 42 tab

Press **M**, open **Squadron 42**, and press **OK** on the spoiler warning once
(**Back** returns to the first tab). Five sections:

- **Outfits** — searchable list from `data/outfits.txt`; pick one, then press
  **Wear SQ42 outfit**. Outfits that don't name a head get the default face, and
  unknown item names are skipped at load (counted in `data/mod.log`).
  **SQ42 visor HUD** swaps the lens display on the next Equip or outfit.
- **Settings** — four game toggles, each with a tooltip: SQ42 auto targeting,
  visor mini-map, visor greebles, and the SQ42 frontend menu. They last for the
  session only; nothing is written to `USER.cfg`.
- **Spawn** — drop a buildable in front of you without entering build mode. The
  list opens filtered to the `[sq42]` group in `data/buildables.txt`; clear the
  filter to reach all 3728. What you spawn joins your base, so **Undo** and
  **Clear base** in the Build tab remove it.
- **Ships** — the SQ42 list (Idris-P, Gladius, Retaliator, Starfarer, Avenger
  Stalker, Hornet), the Vanduul AI wing, and two Bengal rows. Player ships put
  you in the pilot seat through the Vehicles tab's seat rules. Everything spawned
  here except the Bengal's Vanduul wing becomes the Crew tab's target ship; the Vanduul
  ones spawn 300 m up and come for you; Bengals spawn 1500 m up. **Bengal + Vanduul
  wing** brings the wing when this game build has those classes, and its label
  says which case you got.
- **Console** — press Enter to run a command in the game's own console. Its output
  goes to the game's log, not to the menu.

Everything the menu does is written to `data/mod.log`.

---

## Controls

| Key | Action |
| --- | --- |
| `M` | Open / close the menu |
| `F7` | Save your current position |
| `F8` | Teleport to your saved position |
| `F6` | Turn build mode on / off |

---

## Data files and environment variables

Everything the menu reads is plain text in `data/`. Each file documents its own
format in a header comment. Trim a list and the menu gets shorter.

| File | Used for |
| --- | --- |
| `ships.txt` | 1102 spawnable vehicles |
| `npcs.txt` | 2239 NPC archetypes |
| `items.txt` | 1489 gear items, in ten slot sections |
| `buildables.txt` | 3728 objects in 18 groups, for Build mode and the Spawn section |
| `outfits.txt` | 35 outfits — one `[name]` block, then one line per piece |
| `missions.txt` | 34 environmental mission ids — **not read by any current build**; kept for a future Missions menu |
| `contract_scripts.txt` | 2153 contract mission scripts |
| `wallet.txt` | Your aUEC balance, created on first run |
| `spawn.txt` | Your saved teleport spot (F7 / F8) |
| `locations.txt` | Places for the Travel tab: system, name, entity, radius |
| `bookmarks.txt` | Your named Travel spots, created when you save one |
| `locations_found.txt` | Everything the Travel tab's scan found, created by the scan |
| `mod.log` | What the mod did this session |

`sc-offline.exe` sets these before starting the game (`sc-offline.ini` chooses the values):

| Variable | Effect |
| --- | --- |
| `SC_OFFLINE_BOOT_MAP` | Boot map, from `boot_map`. Default `PU_All`, which boots every star system so the Travel tab can reach Pyro and Nyx. The prebuilt DLL doesn't recognise `PU_All` and would skip its boot patch — use `PU` with it |
| `SC_OFFLINE_START_SHIP` | Ship you spawn in, from `start_ship` (`DRAK_Cutlass_Black` by default) |
| `SC_OFFLINE_START` | From `start`. Set to `Daymar` to start over Daymar in that ship |
| `SC_OFFLINE_SHIPS_FILE` | Where the ship list is read from |
| `SC_OFFLINE_SPAWN_FILE` | Where the F7 teleport spot is stored |
| `SC_OFFLINE_MOD_LOG` | Where `mod.log` is written |
| `SC_USER` | Your game `user\client\0` folder — how loadouts reach the game |

---

## Build

The full C++ source is in `src/`. To compile the mod yourself:

1. Install **Visual Studio 2026** with the **Desktop development with C++** workload. The project
   uses the `v145` toolset; on VS 2022, retarget it to `v143` (Project → Retarget) first.
2. Open `ChrisWareOffline.slnx` in Visual Studio.
3. Select **Release | x64** and build (Ctrl+Shift+B).
4. The output is `x64/Release/dinput8.dll` and `x64/Release/sc-offline.exe` (the launcher,
   from `launcher/`), next to `ChrisWareOffline.slnx`. Put both in a folder with `data/` and
   `launcher/sc-offline.ini` to play with them.

Or from the command line:

```powershell
msbuild ChrisWareOffline.slnx /p:Configuration=Release /p:Platform=x64 /m
```

No Visual Studio? Every push to `main` compiles both anyway — grab the
`dinput8-release` artifact from
[Actions](https://github.com/scubamount/sc-offline/actions).

Before pushing from macOS or Linux, `tools/check.sh` parses every source file with
clang against the Windows headers and screens for MSVC error C2712 (`__try` in a
function that owns a `std::string`), in a few seconds. CI runs it first on every
push. It is not the build — only MSVC's is — but it catches the two failures a
non-Windows machine otherwise can't see before CI does.

---

## Releases

[GitHub Actions](.github/workflows/build.yml) builds Release x64 on every push to
`main`, on every pull request, and on every tag starting with `v`. Publishing only
happens on tags:

- Push a `v*` tag → CI builds it, then publishes a Release with
  `sc-offline-<tag>.zip` (launcher, DLL, `sc-offline.ini`, `sc-offline.sh`, `data/`, docs) and
  `dinput8.dll` on its own, plus a changelog generated from the commits since the previous tag.
- `v1.x.y` and later publish as a **full** release; anything else publishes as a
  **pre-release**, so an unfinished build can never look like the recommended
  download. Once someone has actually played a build, promote it with
  `gh release edit <tag> --prerelease=false` — which is how `v0.1.1` became the
  current release.

Every action in the workflow is pinned to a commit SHA, and the workflow runs with
`contents: read` — only the release job is granted `contents: write`.

Current state: [`v0.2.0-rc3`](https://github.com/scubamount/sc-offline/releases/tag/v0.2.0-rc3)
is the **Latest** release: rc2's launcher and mod, with outfits, the menu and the missions setup
brought in line with the original author's DLL (see [CHANGELOG](CHANGELOG.md)). It was promoted
before anyone ran it: compiled and checked, **not yet played**, and the launcher not yet run on
Windows or Linux — see [Notes](#notes). If it misbehaves,
[`v0.1.1`](https://github.com/scubamount/sc-offline/releases/tag/v0.1.1) is the last build from
before this fork's changes (bare DLL, started with the `launch_offline.bat` in its own source tree).

---

## Update

1. Close the game.
2. **Optional:** to keep your money and saved spots, copy `wallet.txt`, `spawn.txt`, `bookmarks.txt` and `locations_found.txt` out of the old `data` folder first, and `sc-offline.ini` if you changed it.
3. Delete the old folder.
4. Download the new zip from [Releases](https://github.com/scubamount/sc-offline/releases) and extract it.
5. Paste those files back into the new folder.
6. Play as usual.

---

## Go back online

1. Make sure the game is closed and `dinput8.dll` is not in your `LIVE\Bin64` folder.
2. Open PowerShell as administrator and restore the EAC executable:

   ```powershell
   ren "C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe.bak" EasyAntiCheat_EOS.exe
   ```

3. Remove the `modules-cdn.eac-prod.on.epicgames.com` line from your hosts file.
4. Flush the DNS cache:

   ```powershell
   ipconfig /flushdns
   ```

Everything is back to normal.

---

## Notes

> [!CAUTION]
> If the PC crashes or is shut down mid-game, delete `dinput8.dll` from your `LIVE\Bin64` folder yourself **before** playing online.

- A game update can break the mod until the mod is updated.
- Builds from this repository are compiled and checked, but have **not** been run
  in-game yet. If something breaks, send back the matching lines from
  `data/mod.log` and the game's `Game.log` — that is how these get fixed.
- The lists the menu reads all live in `data/` as plain text; see
  [Data files](#data-files-and-environment-variables).
- From `v0.2.0-rc1` the source build includes upstream's 0.9.0-rc1, which upstream itself lists
  as needing testing in places (energy-weapon top-up, NPC deletion through the entity handle,
  Stand up, the building preview, Pyro and Nyx names) — see `CHANGELOG.md`.
- Source builds link the C runtime statically, so they no longer need the Visual C++
  redistributable installed. The prebuilt DLL at the repo root still does.

---

## Credits

**ChrisWareOffline** is not this repository's project. The mod and its source belong
to **Chris Ware**:

- Source and issue tracker: <https://github.com/trionic1/chrisware-project> (GPL-3.0)
- Upstream Discord: <https://discord.gg/979RRuMjDP>

This repository began as the author's prebuilt `dinput8.dll` with expanded data
files. It now carries upstream's source (merged through 0.9.0-rc1 from the
`contributions` branch), the CI and release pipeline, and its own development on
top — chiefly the Squadron 42 tab — described under
[Features](#features) and [Squadron 42](#squadron-42-tab). Licensed under GPL-3.0 —
see [LICENSE](LICENSE). See [Build](#build) for compilation instructions.

---

## Disclaimer

AI was used in the making of this project, in a limited capacity.

This is a fan project. It is not made by or affiliated with Cloud Imperium Games or Roberts Space Industries. Star Citizen is a trademark of Cloud Imperium Games. Use it at your own risk.
