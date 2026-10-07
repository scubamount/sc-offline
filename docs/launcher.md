# The launcher: `sc-offline.exe`

The launcher starts the game with the mod and takes the mod out again when the game closes. Source: `launcher/launcher.cpp`.

## Commands

```text
sc-offline.exe [play|install|uninstall|status|update|help] [--game <folder>] [--dry-run] [--skip-eac-check]
```

| Command | What it does |
| --- | --- |
| `play` | The default, and what a double-click does. Copies the mod in, starts the game, takes the mod out when the game closes. |
| `install` | Copies the mod in and leaves it there, for starting the game some other way. Run `uninstall` before going online. |
| `uninstall` | Takes the mod out, puts back anything it replaced, and undoes leftover [PC changes](#pc-changes). Refuses while the game is running. |
| `status` | Runs the checks below and changes nothing. |
| `update` | Checks GitHub for a newer sc-offline and offers to install it; see [Updates](#updates). |
| `help` | Prints the usage. |

| Option | Meaning |
| --- | --- |
| `--game <folder>` | Your `Roberts Space Industries`, `StarCitizen`, channel or `Bin64` folder. Overrides `game =` in `sc-offline.ini`. |
| `--dry-run` | Prints every step it would take, then stops. Nothing is copied, deleted or started. |
| `--skip-eac-check` | Don't stop when Easy Anti-Cheat looks active. |

Exit codes: `0` ok · `1` error · `2` Easy Anti-Cheat is active · `3` the game is running. The helper's own codes, in `launcher.log`: `5` a PC change failed.

Everything the launcher prints also goes to `data\launcher.log` (rewritten each run; the helper appends to it).

## Checks it runs every time

1. **Which DLL.** The SHA-256 of the `dinput8.dll` next to the exe, and what it is: an sc-offline build
   (with its version), an older ChrisWareOffline build, or unknown. With the original author's prebuilt
   DLL it warns unless `boot_map = PU`.
2. **Game updated?** The game version from `<channel>\build_manifest.id` (or `StarCitizen.exe`'s size and date),
   compared with `data\game-build.txt` from your last play. If it changed, a game update may have broken the mod.
3. **Leftovers.** If the mod is still in the game folder while the game isn't running (a crash, or `install`), it
   says so and tells you to run `uninstall`.
4. **PC changes left over.** If a previous run's changes (below) are still recorded in
   `%ProgramData%\sc-offline\pc-changes.txt`, it lists them. `play` asks whether to undo them and stop,
   or play and undo everything afterwards.
5. **Easy Anti-Cheat.** Whether `C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe` exists (active),
   only as `.exe.bak` (disabled), or not at all, and whether your hosts file blocks
   `modules-cdn.eac-prod.on.epicgames.com`. If EAC is active and `eac_rename = off`, `play` and `install`
   stop and print the fix.

## What `play` does

1. **Finds the game's `Bin64` folder.** It tries these in order and takes the first that has
   `<channel>\Bin64\StarCitizen.exe`:
   1. `--game`, then `game =` in `sc-offline.ini`;
   2. the folder it found last time (`data\game-path.txt`);
   3. where the RSI Launcher says the game is: its Windows install entry, and paths in its settings and
      logs under `%APPDATA%\rsilauncher`;
   4. the usual folders on every fixed drive (`Program Files\Roberts Space Industries`, `Games\…`,
      `Game\Star Citizen\…` and similar);
   5. a search of every fixed drive, four folders deep (a few seconds; skips system folders);
   6. if you double-clicked it, a folder picker.

   If it finds more than one install, it uses the first and lists the others. It remembers what it found in
   `data\game-path.txt`; `game =` and `--game` always win over that file.
2. **Runs the checks** above.
3. **Sets the environment variables** the mod reads (see [data-files.md](data-files.md#environment-variables)).
4. **Starts a helper**, a second copy of `sc-offline.exe` with no window, that changes the game folder:

   | Path | Going in | Coming out |
   | --- | --- | --- |
   | `Bin64\dinput8.dll` | the mod's DLL. If another mod's `dinput8.dll` is there, it is first renamed `dinput8.dll.sc-offline-backup` | deleted; the other mod's DLL renamed back |
   | `<channel>\user\client\0\default_1.xml` | the starting loadout from `data\OfflineDB`. Yours is first copied to `default_1.xml.sc-offline-backup` | your copy restored, or ours deleted if you had none |
   | `Bin64\sc-offline.installed` | a note of what was installed and when | deleted |

   Before copying the mod in, `play`'s helper makes the [PC changes](#pc-changes). It runs as administrator when
   any PC change is on (the default) or the game folder needs it, and then Windows asks once. The game itself
   never runs as administrator.
5. **Starts `StarCitizen.exe`** and waits until every `StarCitizen.exe` has exited. The helper then takes the
   mod out, even if you closed the launcher window first. Ctrl+C in the launcher window is ignored; close the
   game instead.

If the PC crashes mid-game, the mod stays in the game folder. The next `play` or `status` notices; run
`sc-offline.exe uninstall` before going online.

## PC changes

While you play, the helper changes three things outside the game folder and undoes them when every
`StarCitizen.exe` has exited. Each is a switch in `sc-offline.ini`, on by default.

| Key | Going in | Coming out |
| --- | --- | --- |
| `block_network` | Windows Firewall rules, inbound and outbound: `sc-offline: block StarCitizen.exe` (this install's exe only), `sc-offline: block RSI Launcher.exe` (found from its install entry, beside the game library, or the default folder) and `sc-offline: block CrashHandler.exe` (`<channel>\Tools\Public\CrashHandler.exe`, CIG's crash reporter). A rule is only added if its exe exists. `sc-offline.exe` itself stays online for updates. | deleted |
| `eac_hosts` | `127.0.0.1 modules-cdn.eac-prod.on.epicgames.com # added by sc-offline…` appended to the hosts file, then `ipconfig /flushdns`. Skipped if the hosts file already blocks it | only the tagged line removed, DNS flushed |
| `eac_rename` | `EasyAntiCheat_EOS.exe` renamed to `EasyAntiCheat_EOS.exe.bak`. Skipped if it isn't there | renamed back |

- Each change is written to `%ProgramData%\sc-offline\pc-changes.txt` the moment it is made, and only recorded
  changes are undone. A hosts line or `.bak` you made yourself is never touched.
- If a step fails, the helper undoes what it already did and the game doesn't start.
- After a crash the record stays. `status` lists it, `uninstall` undoes it, and `play` offers to.

### Updates

On `play` and `status` the launcher asks GitHub for the latest full release of `scubamount/sc-offline`
(3-second timeout; no network just skips it). Pre-releases, drafts, older versions and tags that aren't plain
numbers are never offered. If a newer one exists, `play` asks **"Update now?"** and `status` tells you to run
`sc-offline.exe update`.

An update:
1. downloads `sc-offline-<tag>.zip` into `data\update\` and checks its SHA-256 against the `digest` GitHub
   publishes for that asset. A mismatch or a missing digest stops it with nothing changed;
2. unpacks it with Windows' own `tar.exe` and checks that `sc-offline.exe` and `dinput8.dll` are in it;
3. replaces every shipped file except `sc-offline.ini` and the player files in `data\` (`wallet.txt`,
   `spawn.txt`, `bookmarks.txt`, `locations_found.txt`, `game-path.txt`, `game-build.txt`, the logs).
   Each replaced file is first renamed to `<name>.update-old`, and the step is written to
   `data\update\applied.txt` before it happens;
4. appends settings that are new in the release's `sc-offline.ini` to yours, commented out, so their
   defaults apply;
5. starts the new launcher with the same arguments.

If a step fails, the old files are put back at once. If the PC dies mid-update, the next run of
`sc-offline.exe` reads `applied.txt` and puts them back. `check_updates = off` turns the check off;
`sc-offline.exe update` checks on demand.

### Crash reports

If the game wrote crash files under `<channel>\Crashes` during the session, the launcher offers a report
before the log cleanup. A **y** zips `mod.log`, `launcher.log`, `Game.log` and the crash folder's text
files into `data\crash-reports\sc-offline-crash-<time>.zip`. In the copies (never the originals), your RSI
handle becomes `<handle>`, GEID and account numbers become `<id>`, and your Windows user name in paths
becomes `<user>`. Memory dumps (`.dmp`) are left out because they can't be redacted. It can then open the bug
form with the version, game build and platform filled in, and an Explorer window on the zip. **Nothing is
uploaded**: you attach the zip and read it first. `crash_reports = off` turns this off.

### Administrator rights

The launcher never runs as administrator, and neither does the game. Windows asks once, only for the step
that needs it:
- the helper, when the firewall, EAC or a protected game folder is involved (see [PC changes](#pc-changes));
- deleting session logs that a protected game folder won't let you delete. The elevated run works out this
  session's files again by itself;
- updating a mod folder only administrators can write, e.g. under Program Files. The elevated run asks GitHub
  again and applies only the version you were shown, then the new launcher starts with normal rights.

If the mod folder's `data` can't be written, the mod can't save your wallet or places, and the launcher tells
you to move the folder (for example to your Desktop).

### Session logs

When the game closes, the launcher lists the logs the game wrote during this session: `<channel>\Game.log`,
new files in `<channel>\logbackups`, and new files under `<channel>\Crashes`. It asks **"Delete these
files?"** and then **"Are you sure?"**; anything but `y` both times keeps them. Files older than the session
(for example from online play) are never touched, and neither are `data\mod.log` and `data\launcher.log`.
Keep `Game.log` if you want to report a bug. Set `clean_logs = off` in `sc-offline.ini` to skip the question.
- Under Wine none of this runs; `sc-offline.sh` handles hosts there, and the firewall rule doesn't apply.
- The firewall rule cuts the game's network for the session. It doesn't hide anything already on disk, such as
  logs or the renamed EAC file while you play.

## `sc-offline.ini`

Each line is `key = value`. Lines starting with `#` are comments. An unknown key is reported when the launcher starts, not silently ignored.

| Key | Default | Meaning |
| --- | --- | --- |
| `game` | (found automatically) | Your Star Citizen folder. You can point at `Roberts Space Industries`, at `StarCitizen`, at the channel folder, or at the folder that holds `StarCitizen`. |
| `check_updates` | `on` | Check GitHub for a newer release on `play` and `status` and offer to install it. |
| `crash_reports` | `on` | After a crash, offer a redacted log bundle and the bug form. See [Crash reports](#crash-reports). |
| `clean_logs` | `ask` | After the game closes, list this session's game logs and ask (twice) before deleting them. `off` skips it. |
| `channel` | `LIVE` | Which install to use when `game` points above it: `LIVE`, `PTU`, `EPTU`, and so on. |
| `boot_map` | `PU_All` | `PU_All` loads every star system, so Travel can reach Pyro and Nyx. |
| `start_ship` | `DRAK_Cutlass_Black` | The ship you start in. |
| `start` | (empty) | `Daymar` starts you over Daymar, in that ship. |
| `block_network` | `on` | Block `StarCitizen.exe`, the RSI Launcher and the game's `CrashHandler.exe` in Windows Firewall while you play. See [PC changes](#pc-changes). |
| `eac_hosts` | `on` | Add the EAC hosts line while you play. |
| `eac_rename` | `on` | Rename `EasyAntiCheat_EOS.exe` while you play. |
