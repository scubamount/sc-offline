# sc-offline — Star Citizen offline mod

<img src="images/banner.webp" alt="Star Citizen Offline Mod — offline single-player mod menu for Star Citizen" width="100%">

An offline single-player mod menu for **Star Citizen**. You can spawn ships, NPCs and buildings, teleport around the star systems, and wear Squadron 42 outfits, all from one in-game menu.

> [!WARNING]
> This mod may get your account banned. Use it at your own risk, and **only offline, in single player**.

> [!NOTE]
> Builds from this repository are compiled and checked by CI, but **nobody has played them in game yet**. That includes the current Latest release. If something breaks, see [Troubleshooting](#troubleshooting).

![A Vanduul holding a gun on a desert planet, with a line of Vanduul and a large ship behind it](images/screenshot.webp)

## What you need

- A copy of Star Citizen that you own.
- Windows. Linux through Wine is experimental and untested; see [docs/linux.md](docs/linux.md).
- Administrator rights: Windows asks once each time you play (see [What the launcher changes](#what-the-launcher-changes)).

## Setup

### 1. Download the mod

1. Download `sc-offline-<version>.zip` from the newest release on [Releases](https://github.com/scubamount/sc-offline/releases). It's about 1.4 MB.
2. Right-click the zip, choose **Extract All**, and put the folder anywhere, for example your Desktop. Keep all the files together.

Don't copy anything into your game folder. The launcher puts the mod there when you play and takes it out again afterwards.

### 2. What the launcher changes

The mod can't run while Easy Anti-Cheat (EAC) is on, so each time you play, the launcher's helper:

1. adds a Windows Firewall rule that blocks `StarCitizen.exe`, so the game has no network while modded;
2. adds `127.0.0.1 modules-cdn.eac-prod.on.epicgames.com` to your hosts file, so the RSI Launcher doesn't download EAC again;
3. renames `EasyAntiCheat_EOS.exe` to `EasyAntiCheat_EOS.exe.bak`.

When the game closes it undoes exactly what it changed and prints what it undid. Anything you set up yourself is left alone. If the game crashes or the PC shuts down, the next run of `sc-offline.exe` lists what is still in place and offers to undo it. Each step can be turned off in `sc-offline.ini`; see [docs/launcher.md](docs/launcher.md#pc-changes).

The firewall rule stops the game reaching the network. It doesn't remove other traces on your PC, such as logs, so use the mod at your own risk.

## Play

1. Close the RSI Launcher and the game.
2. Double-click `sc-offline.exe`. It prints the game folder it found. Windows asks for administrator rights; say yes. That prompt is for the small helper that makes the changes above and copies the mod in and out; the game itself never runs as administrator.
3. Once the game has loaded you in, press **M** to open the menu.

| Key | Action |
| --- | --- |
| `M` | Open or close the menu |
| `F6` | Turn build mode on or off |
| `F7` | Save your position |
| `F8` | Teleport back to the saved position |

The launcher searches every drive for `Roberts Space Industries\StarCitizen`. If it can't find your install, set `game =` in `sc-offline.ini`. The same file also chooses your start ship, the boot map and the channel (`LIVE`, `PTU`, and so on); see [docs/launcher.md](docs/launcher.md).

What's in each menu tab: [docs/features.md](docs/features.md).

## Update

1. Close the game.
2. To keep your money and saved places, copy `wallet.txt`, `spawn.txt`, `bookmarks.txt` and `locations_found.txt` out of the old `data` folder. Copy `sc-offline.ini` too if you changed it.
3. Delete the old folder, then extract the new zip.
4. Put those files back in the new folder.

## Troubleshooting

- **The menu doesn't open:** check that you started the game with `sc-offline.exe`, not the RSI Launcher.
- **The launcher can't find the game:** set `game =` in `sc-offline.ini`.
- **Is everything set up?** Run `sc-offline.exe status` from a terminal. It checks the DLL, the game version, Easy Anti-Cheat and the hosts file, and changes nothing.
- **The launcher stops with "Easy Anti-Cheat is active":** set `eac_rename = on` in `sc-offline.ini`, or rename the file yourself.
- **The launcher couldn't add the firewall rule or edit hosts:** say yes to the administrator prompt; some antivirus tools lock the hosts file.
- **The game crashed or the PC shut down mid-game:** run `sc-offline.exe uninstall` before you play online. It removes the mod and undoes the PC changes.
- **A game update broke the mod:** that's expected until the mod is updated for the new game version.
- **Reporting a bug:** send `data/launcher.log`, `data/mod.log`, and the game's `Game.log`.

## Go back online

1. Close the game and wait for the launcher window to say the changes are undone.
2. Run `sc-offline.exe status`. It should say `Installed: no` and list no leftover PC changes. If not, run `sc-offline.exe uninstall`.
3. If you set `eac_hosts` or `eac_rename` to `off` and made those changes by hand, undo them by hand: rename `EasyAntiCheat_EOS.exe.bak` back, delete the `modules-cdn.eac-prod.on.epicgames.com` line from your hosts file, and run `ipconfig /flushdns`.

## More docs

| Doc | For |
| --- | --- |
| [docs/features.md](docs/features.md) | Every menu tab, including Squadron 42 |
| [docs/launcher.md](docs/launcher.md) | `sc-offline.exe`, `sc-offline.ini`, and what the launcher changes on disk |
| [docs/data-files.md](docs/data-files.md) | The text files in `data/` and the environment variables |
| [docs/linux.md](docs/linux.md) | Running under Wine (experimental) |
| [docs/build.md](docs/build.md) | Building from source, checks, CI and releases |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each release |

## Credits

sc-offline is **based on ChrisWareOffline 0.9.0-rc1** by Chris Ware and cloudyyrust (GPL-3.0). The original project has been shut down and its repository removed; this repository is maintained independently and is not endorsed by its authors.

This repository adds the launcher, CI and releases, the Squadron 42 tab and later fixes. It is licensed under GPL-3.0; see [LICENSE](LICENSE). Report bugs on [Issues](https://github.com/scubamount/sc-offline/issues).

AI was used in a limited way to make this project. It's a fan project, not made by or affiliated with Cloud Imperium Games or Roberts Space Industries. Star Citizen is a trademark of Cloud Imperium Games.
