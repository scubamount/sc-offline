# Star Citizen Offline Mod (ChrisWareOffline)

<img src="images/banner.webp" alt="Star Citizen Offline Mod — offline single-player mod menu for Star Citizen" width="100%">

Offline single-player mod menu for **Star Citizen**.

> [!WARNING]
> This mod may result in a ban. Use it at your own risk. It is for **offline single player only**.

**Jump to:** [Which DLL do I run?](#which-dll-do-i-run) · [Features](#features) · [Requirements](#requirements) · [Setup](#setup) · [Play](#play) · [Squadron 42](#squadron-42-tab) · [Controls](#controls) · [Data files](#data-files-and-environment-variables) · [Build](#build) · [Releases](#releases) · [Update](#update) · [Go back online](#go-back-online) · [Notes](#notes) · [Credits](#credits)

---

## Which DLL do I run?

`launch_offline.bat` copies **the `dinput8.dll` sitting in this folder** into your
game — nothing else. So the DLL you put here is the mod you play.

Two builds of that file exist, and they are not interchangeable:

| | Prebuilt — `dinput8.dll` at the repo root | Built from source — a [Release](https://github.com/scubamount/sc-offline/releases) or your own build |
| --- | --- | --- |
| Where it comes from | The mod's original author's tree, committed here as-is | Compiled from the C++ in `src/`, by CI or by you |
| When it changes | Only when someone replaces it | On every push to `main` (CI artifact) and every `v*` tag (Release) |
| Best for | Playing, with zero setup | Testing a change, or building your own |

- **Just playing?** Use the prebuilt DLL as it is. Nothing to decide.
- **Testing this repository's work?** Download `dinput8.dll` from a
  [Release](https://github.com/scubamount/sc-offline/releases) and drop it over the
  file in this folder. Keep a copy of the original — you will want it back.
- **Building it yourself?** See [Build](#build). The output lands in the same place,
  so the script picks it up unchanged.

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
- About 1 GB of disk for the download.
- Administrator rights, to copy the mod into your game folder.
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

1. Click the green **Code** button at the top of this page and choose **Download ZIP**.
2. Right-click the ZIP and choose **Extract All**.
3. Place the `sc-offline-main` folder anywhere you like, such as your Desktop.
4. Keep all files together in that folder.

The mod does **not** go in your game folder. `launch_offline.bat` copies it in when you play and removes it when you close the game.

To play a source build instead of the prebuilt one, overwrite this folder's
`dinput8.dll` with the one from a
[Release](https://github.com/scubamount/sc-offline/releases) — see
[Which DLL do I run?](#which-dll-do-i-run).

**Custom install path:** if your game is not installed at the path below, right-click `launch_offline.bat`, choose **Edit**, and change the `SC_BIN` line at the top to your own `LIVE\Bin64` folder.

```text
C:\Program Files\Roberts Space Industries\StarCitizen\LIVE
```

---

## Play

1. Close the RSI Launcher and the game.
2. Double-click `launch_offline.bat`.
   - If it reports that it cannot copy the mod, right-click the file and choose **Run as administrator**.
3. Wait for the game to load you in.
4. Press **M** to open the menu.

> [!IMPORTANT]
> Leave the black script window open while you play. When you close the game, the script removes the mod from your game folder.

---

## Squadron 42 tab

Press **M**, open **Squadron 42**, and accept the spoiler warning once. Five
sections:

- **Outfits** — searchable list from `data/outfits.txt`; picking one wears it.
  **Wear SQ42 outfit** applies a built-in pilot preset, so it still works if that
  file is missing. **SQ42 visor HUD** swaps the lens display on the next equip.
- **Settings** — four game toggles, each with a tooltip: SQ42 auto targeting,
  visor mini-map, visor greebles, and the SQ42 frontend menu. They last for the
  session only; nothing is written to `USER.cfg`.
- **Spawn** — drop a buildable in front of you without entering build mode. The
  list opens filtered to the `[sq42]` group in `data/buildables.txt`; clear the
  filter to reach all 3728. What you spawn joins your base, so **Undo** and
  **Clear base** in the Build tab remove it.
- **Ships** — the SQ42 list (Idris-P, Gladius, Retaliator, Starfarer, Avenger
  Stalker, Hornet), the Vanduul AI wing, and Bengal A / Bengal B. Player ships put
  you in the pilot seat through the Vehicles tab's seat rules, and the ship becomes
  the Crew tab's target; the Vanduul ones spawn 300 m up and come for you; Bengals
  spawn 1500 m up. Bengal B brings its enemy wing when this game build has those
  classes, and its label says which case you got.
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

`launch_offline.bat` sets these before starting the game:

| Variable | Effect |
| --- | --- |
| `SC_OFFLINE_BOOT_MAP` | Boot map; the script sets `PU` |
| `SC_OFFLINE_START_SHIP` | Ship you spawn in (`DRAK_Cutlass_Black` by default) |
| `SC_OFFLINE_START` | Set to `Daymar` to start over Daymar in that ship |
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
4. The output is `x64/Release/dinput8.dll`, next to `ChrisWareOffline.slnx`. Copy it
   over the DLL in this folder to play with it.

Or from the command line:

```powershell
msbuild ChrisWareOffline.slnx /p:Configuration=Release /p:Platform=x64 /m
```

No Visual Studio? Every push to `main` compiles one anyway — grab the
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

- Push a `v*` tag → CI builds it, then publishes a Release with `dinput8.dll`
  attached and a changelog generated from the commits since the previous tag.
- `v1.x.y` and later publish as a **full** release; anything else publishes as a
  **pre-release**, so an unfinished build can never look like the recommended
  download. Once someone has actually played a build, promote it with
  `gh release edit <tag> --prerelease=false` — which is how `v0.1.1` became the
  current release.

Every action in the workflow is pinned to a commit SHA, and the workflow runs with
`contents: read` — only the release job is granted `contents: write`.

Current state: [`v0.2.0-rc1`](https://github.com/scubamount/sc-offline/releases/tag/v0.2.0-rc1)
is a **pre-release**: upstream ChrisWareOffline 0.9.0-rc1 with the Squadron 42 tab on top. It has
been compiled and string-checked, **not played** — see [Notes](#notes).
[`v0.1.1`](https://github.com/scubamount/sc-offline/releases/tag/v0.1.1) stays the full release
until someone has run 0.2.0-rc1.

---

## Update

1. Close the game.
2. **Optional:** to keep your money and saved spots, copy `wallet.txt`, `spawn.txt`, `bookmarks.txt` and `locations_found.txt` out of the old `data` folder first.
3. Delete the old version of the mod.
4. Download the new version (green **Code** button, then **Download ZIP**) and extract it.
5. Paste those files into the new `data` folder.
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
> If the script window is closed early, delete `dinput8.dll` from your `LIVE\Bin64` folder yourself **before** playing online.

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
