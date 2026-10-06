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
   ./sc-offline.sh [--game <Star Citizen folder, as a Windows path>]
   ```

## How it finds things

| What | Order it tries |
| --- | --- |
| Prefix | `$WINEPREFIX`, then the LUG Helper's `~/.config/starcitizen-lug/winedir.conf`, then `~/Games/star-citizen` |
| Wine | `$WINE`, then the runner named by `wine_path=` in the prefix's `sc-launch.sh`, then the first `wine` on `PATH` |

Any other prefix works too, as long as you set `WINEPREFIX=` and `WINE=` yourself (for example a Lutris or Steam/Proton prefix). Nobody has tried that yet.

The script sets `WINEDLLOVERRIDES=dinput8=n,b`, so Wine loads the mod's `dinput8.dll` instead of its own.

It starts Wine directly, **not** through the LUG Helper's `sc-launch.sh`. Anything that script sets up (DXVK, esync/fsync, and so on) doesn't apply. If the game works from the LUG Helper but not from here, export those same variables before running `./sc-offline.sh`, and mention it in your report.

## Reporting

Send the script's output, `data/mod.log`, and the game's `Game.log`.
