# Linux (experimental)

> [!WARNING]
> **Untested.** Nobody has run the mod under Wine yet. If you try it, please report back.

`sc-offline.sh` starts `sc-offline.exe` inside your Star Citizen Wine prefix. Once it's running, everything works the same as on Windows ([launcher.md](launcher.md)).

## Setup

1. Install the game in a Wine prefix. The script expects the layout that the [LUG Helper](https://github.com/starcitizen-lug/lug-helper) creates.
2. Turn off Easy Anti-Cheat, the Linux way:
   - add `127.0.0.1 modules-cdn.eac-prod.on.epicgames.com` to `/etc/hosts`;
   - if `EasyAntiCheat_EOS.exe` exists under `drive_c/Program Files (x86)/EasyAntiCheat_EOS/` in your prefix, rename it.
3. Extract the release zip anywhere, then run:

   ```bash
   ./sc-offline.sh
   ```

   It takes the same commands and options as `sc-offline.exe` (`status`, `uninstall`, `--dry-run`, ...;
   see [launcher.md](launcher.md)), plus `--prefix <dir>` and `--wine <path>` to override what it finds.
   `./sc-offline.sh --dry-run` prints the prefix, Wine and the exact command, and starts nothing.

## How it finds things

| What | Order it tries |
| --- | --- |
| Prefix | `--prefix` or `$WINEPREFIX`; the LUG Helper's `~/.config/starcitizen-lug/winedir.conf`; a Lutris game whose name mentions Star Citizen (its `prefix:`); a Steam/Proton `compatdata/*/pfx` that contains `Roberts Space Industries` (native and Flatpak Steam); `~/Games/star-citizen` |
| Wine | `--wine` or `$WINE`; `wine_path=` in the prefix's `sc-launch.sh`; the Lutris game's Wine runner; for a Proton prefix, the newest Proton's `wine`; `wine` on `PATH` |

It prints which source it used for each. Only the LUG Helper layout has been checked against the real tool; the
Lutris and Steam guesses follow those tools' usual file layout and haven't been run by anyone.

It also checks that `/etc/hosts` blocks the EAC download server and prints the line to add if not.

The script sets `WINEDLLOVERRIDES=dinput8=n,b`, so Wine loads the mod's `dinput8.dll` instead of its own.

It starts Wine directly, **not** through the LUG Helper's `sc-launch.sh`. Anything that script sets up (DXVK, esync/fsync, and so on) doesn't apply. If the game works from the LUG Helper but not from here, export those same variables before running `./sc-offline.sh`, and mention it in your report.

## Reporting

Send the script's output, `data/mod.log`, and the game's `Game.log`.
