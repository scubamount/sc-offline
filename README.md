# Star Citizen Offline Mod (ChrisWareOffline)

<img src="images/banner.webp" alt="Star Citizen Offline Mod — offline single-player mod menu for Star Citizen" width="100%">

Offline single-player mod menu for **Star Citizen**.

> [!WARNING]
> This mod may result in a ban. Use it at your own risk. It is for **offline single player only**.

**Jump to:** [Features](#features) · [Requirements](#requirements) · [Setup](#setup) · [Play](#play) · [Controls](#controls) · [Update](#update) · [Go back online](#go-back-online) · [Notes](#notes) · [Credits](#credits)

---

## Features

Everything runs through one in-game menu. Press **M** to open it.

| Feature | What it does |
| --- | --- |
| **Ships** | Spawn any vehicle in the game — 1102 entries, from the Bengal to the Idris-P. The menu sorts biggest first and skips anything your game version can't find. |
| **Spawn NPC** | Spawn any NPC archetype (2239 entries), in front of you or overhead with you in the pilot seat. |
| **Clear NPCs** | Remove every NPC the mod spawned. |
| **Wallet** | Set your aUEC balance. Persists in `data/wallet.txt`. |
| **God mode** | Toggle invulnerability. |
| **Build mode (F6)** | Free build mode, 3746 buildable entries. |
| **Item Picker** | Equip any item (1499 entries) from the gear menu. |
| **Outfits** | Wear Squadron 42 outfits (178) and cast members' heads. Experimental — the SQ42 menus are flagged spoiler-bearing in-game. |
| **Squadron 42 tab** | Spoiler-gated tab: the outfit picker, the SQ42 ship list (Idris-P, Gladius, Retaliator, Starfarer, Avenger Stalker, Hornet, the Vanduul AI wing, Bengal A / Bengal B with its enemy side) and a console runner. |
| **Missions** | Start 34 environmental missions that normally need the online backend — combat assist, pirate blockades, and more. |
| **Contracts** | 2153 generated contracts with their mission scripts wired up locally. |
| **Teleport** | Save a position (F7) and teleport back to it (F8). |

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

> [!NOTE]
> This repo ships the prebuilt `dinput8.dll` you run. To build from source instead, see [Build](#build) below.

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

## Build

The full C++ source is in `src/`. To compile the mod yourself:

1. Install **Visual Studio 2022** (17.10+) or **VS 2026** with the **Desktop development with C++** workload.
2. Open `ChrisWareOffline.slnx` in Visual Studio.
3. Select **Release | x64** and build (Ctrl+Shift+B).
4. The output is `src/x64/Release/dinput8.dll`.

Or from the command line:

```powershell
msbuild ChrisWareOffline.slnx /p:Configuration=Release /p:Platform=x64 /m
```

The DLL is also built automatically on every push via [GitHub Actions](.github/workflows/build.yml) — artifacts are attached to each run.

---

## Controls

| Key | Action |
| --- | --- |
| `M` | Open / close the menu |
| `F7` | Save your current position |
| `F8` | Teleport to your saved position |
| `F6` | Turn build mode on / off |

---

## Update

1. Close the game.
2. **Optional:** to keep your money and saved spot, copy `wallet.txt` and `spawn.txt` out of the old `data` folder first.
3. Delete the old version of the mod.
4. Download the new version (green **Code** button, then **Download ZIP**) and extract it.
5. Paste `wallet.txt` and `spawn.txt` into the new `data` folder.
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
- A log of what the mod does is saved to `mod.log` in the `data` folder.
- The lists the menu reads all live in `data/` as plain text — `ships.txt`, `npcs.txt`, `items.txt`, `buildables.txt`, `missions.txt`, `contract_scripts.txt`, `outfits.txt`. Each file documents its own format in a header comment. Trim a list down and the menu gets shorter.

---

## Credits

**ChrisWareOffline** is not my project. The mod and its source belong to **Chris Ware**:

- Source and issue tracker: <https://github.com/trionic1/chrisware-project> (GPL-3.0)
- Upstream Discord: <https://discord.gg/979RRuMjDP>

This repository includes the full C++ source (merged from upstream) alongside the prebuilt `dinput8.dll`. Licensed under GPL-3.0 — see [LICENSE](LICENSE). See [Build](#build) above for compilation instructions.

---

## Disclaimer

AI was used in the making of this project, in a limited capacity.

This is a fan project. It is not made by or affiliated with Cloud Imperium Games or Roberts Space Industries. Star Citizen is a trademark of Cloud Imperium Games. Use it at your own risk.