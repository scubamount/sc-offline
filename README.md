<div align="center">

<img src="images/banner.webp" alt="sc-offline — Star Citizen offline mod: single-player mod menu with ships, NPCs, build mode, travel and Squadron 42 outfits" width="100%">

# sc-offline

**An offline, single-player mod menu for Star Citizen.**<br>
Spawn ships, NPCs and buildings, travel the star systems and wear Squadron 42 outfits, all from one in-game menu.

[![Latest release](https://img.shields.io/github/v/release/scubamount/sc-offline?style=flat-square&label=release&color=2ec4d6)](https://github.com/scubamount/sc-offline/releases/latest)
[![Build](https://img.shields.io/github/actions/workflow/status/scubamount/sc-offline/build.yml?branch=main&style=flat-square&label=build)](https://github.com/scubamount/sc-offline/actions/workflows/build.yml)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20(Wine%2C%20experimental)-555?style=flat-square)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-f0851f?style=flat-square)](LICENSE)

[**Download**](https://github.com/scubamount/sc-offline/releases/latest) ·
[Setup](#setup) ·
[Play](#play) ·
[Troubleshooting](#troubleshooting) ·
[Go back online](#go-back-online) ·
[Docs](#more-docs)

</div>

> [!WARNING]
> This mod may get your account banned. Use it at your own risk, and **only offline, in single player**.

> [!NOTE]
> Builds from this repository are compiled and checked by CI, but **nobody has played them in game yet**. That includes the current Latest release. If something breaks, see [Troubleshooting](#troubleshooting) and [report it](https://github.com/scubamount/sc-offline/issues).

## Quick start

1. **Download** `sc-offline-<version>.zip` from the [latest release](https://github.com/scubamount/sc-offline/releases/latest) and extract it anywhere.
2. **Close** the RSI Launcher and the game.
3. **Run** `sc-offline.exe` and say yes to the administrator prompt.
4. **Press `M`** once the game has loaded you in.

The launcher puts the mod into your game folder while you play and takes it out again afterwards, along with every change it made to your PC.

## What you need

| Requirement | Details |
| --- | --- |
| **Game** | A copy of Star Citizen that you own |
| **OS** | Windows. Linux through Wine is experimental and untested; see [docs/linux.md](docs/linux.md) |
| **Rights** | Administrator: Windows asks once each time you play |
| **Disk** | About 1.4 MB for the zip |

## Setup

### 1. Download the mod

1. Download `sc-offline-<version>.zip` from the newest release on [Releases](https://github.com/scubamount/sc-offline/releases).
2. Right-click the zip, choose **Extract All**, and put the folder anywhere, for example your Desktop. Keep all the files together.

Don't copy anything into your game folder yourself.

### 2. What the launcher changes

The mod can't run while Easy Anti-Cheat (EAC) is on, so each time you play, the launcher's helper:

| Step | Change | Why |
| --- | --- | --- |
| 1 | Adds a Windows Firewall rule that blocks `StarCitizen.exe` | The game has no network while modded |
| 2 | Adds `127.0.0.1 modules-cdn.eac-prod.on.epicgames.com` to your hosts file | The RSI Launcher doesn't download EAC again |
| 3 | Renames `EasyAntiCheat_EOS.exe` to `EasyAntiCheat_EOS.exe.bak` | EAC doesn't start |

When the game closes, it undoes exactly what it changed and prints what it undid. Anything you set up yourself is left alone. If the game crashes or the PC shuts down, the next run of `sc-offline.exe` lists what is still in place and offers to undo it. Each step can be turned off in `sc-offline.ini`; see [docs/launcher.md](docs/launcher.md#pc-changes).

> [!IMPORTANT]
> The firewall rule stops the game reaching the network. It doesn't remove other traces on your PC by itself. When the game closes, the launcher lists the logs the game wrote during this session (`Game.log`, new files in `logbackups` and `Crashes`) and asks twice before deleting them; keep `Game.log` if you want to report a bug. Older logs are never touched. Use the mod at your own risk.

## Play

1. Close the RSI Launcher and the game.
2. Double-click `sc-offline.exe`. It prints the game folder it found. Windows asks for administrator rights; say yes. That prompt is for the small helper that makes the changes above and copies the mod in and out; the game itself never runs as administrator.
3. Once the game has loaded you in, press <kbd>M</kbd> to open the menu.

| Key | Action |
| --- | --- |
| <kbd>M</kbd> | Open or close the menu |
| <kbd>F6</kbd> | Turn build mode on or off |
| <kbd>F7</kbd> | Save your position |
| <kbd>F8</kbd> | Teleport back to the saved position |

The launcher finds your install by itself: it asks the RSI Launcher where the game is, checks the usual folders, then searches your drives, and if all that fails it lets you pick the folder. It remembers the answer for next time. To force a folder, set `game =` in `sc-offline.ini`. The same file also chooses your start ship, the boot map and the channel (`LIVE`, `PTU`, and so on); see [docs/launcher.md](docs/launcher.md).

What's in each menu tab: [docs/features.md](docs/features.md).

<p align="center">
  <img src="images/screenshot.webp" alt="A Vanduul holding a gun on a desert planet, with a line of Vanduul and a large ship behind it" width="85%">
  <br><sub>In-game: spawned Vanduul and a ship.</sub>
</p>

## Update

The launcher checks for a newer release each time you play (it gives up after 3 seconds if you're offline) and asks before changing anything. Say `y` and it downloads the release zip (Windows asks for administrator rights only if the folder is under Program Files), checks it against the SHA-256 that GitHub publishes, replaces the program files, keeps your saves (`wallet.txt`, saved places, bookmarks) and your `sc-offline.ini`, and restarts itself. If anything goes wrong, it puts the old files back.

To check on demand, run `sc-offline.exe update`. To turn the check off, set `check_updates = off` in `sc-offline.ini`.

<details>
<summary><b>Updating by hand</b></summary>

1. Close the game.
2. Copy `wallet.txt`, `spawn.txt`, `bookmarks.txt` and `locations_found.txt` out of the old `data` folder, and `sc-offline.ini` if you changed it.
3. Delete the old folder, then extract the new zip.
4. Put those files back in the new folder.
</details>

## Troubleshooting

<details>
<summary><b>The menu doesn't open</b></summary>

Check that you started the game with `sc-offline.exe`, not the RSI Launcher.
</details>

<details>
<summary><b>The launcher can't find the game, or picks the wrong one</b></summary>

Set `game =` in `sc-offline.ini` to your `StarCitizen` folder (for example `game = E:\Game\Star Citizen\StarCitizen`), or delete `data\game-path.txt` to make it search again.
</details>

<details>
<summary><b>Is everything set up?</b></summary>

Run `sc-offline.exe status` from a terminal. It checks the DLL, the game version, Easy Anti-Cheat and the hosts file, and changes nothing.
</details>

<details>
<summary><b>The launcher stops with "Easy Anti-Cheat is active"</b></summary>

Set `eac_rename = on` in `sc-offline.ini`, or rename the file yourself.
</details>

<details>
<summary><b>The launcher couldn't add the firewall rule or edit hosts</b></summary>

Say yes to the administrator prompt; some antivirus tools lock the hosts file.
</details>

<details>
<summary><b>The game crashed or the PC shut down mid-game</b></summary>

Run `sc-offline.exe uninstall` before you play online. It removes the mod and undoes the PC changes.
</details>

<details>
<summary><b>A game update broke the mod</b></summary>

That's expected until the mod is updated for the new game version.
</details>

**Reporting a bug:** open an [Issue](https://github.com/scubamount/sc-offline/issues) and attach `data/launcher.log`, `data/mod.log`, and the game's `Game.log`.

## Go back online

1. Close the game and wait for the launcher window to say the changes are undone.
2. Run `sc-offline.exe status`. It should say `Installed: no` and list no leftover PC changes. If not, run `sc-offline.exe uninstall`.
3. If you set `eac_hosts` or `eac_rename` to `off` and made those changes by hand, undo them by hand: rename `EasyAntiCheat_EOS.exe.bak` back, delete the `modules-cdn.eac-prod.on.epicgames.com` line from your hosts file, and run `ipconfig /flushdns`.

## More docs

| Doc | For |
| --- | --- |
| [Features](docs/features.md) | Every menu tab, including Squadron 42 |
| [Launcher](docs/launcher.md) | `sc-offline.exe`, `sc-offline.ini`, and what the launcher changes on disk |
| [Data files](docs/data-files.md) | The text files in `data/` and the environment variables |
| [Linux](docs/linux.md) | Running under Wine (experimental) |
| [Build](docs/build.md) | Building from source, checks, CI and releases |
| [Changelog](CHANGELOG.md) | What changed in each release |

## Credits

sc-offline is **based on ChrisWareOffline 0.9.0-rc1** by Chris Ware and cloudyyrust (GPL-3.0). The original project has been shut down and its repository removed; this repository is maintained independently and is not endorsed by its authors.

This repository adds the launcher, CI and releases, the Squadron 42 tab and later fixes. It is licensed under GPL-3.0; see [LICENSE](LICENSE). Report bugs on [Issues](https://github.com/scubamount/sc-offline/issues).

<sub>AI was used in a limited way to make this project. It's a fan project, not made by or affiliated with Cloud Imperium Games or Roberts Space Industries. Star Citizen is a trademark of Cloud Imperium Games.</sub>
