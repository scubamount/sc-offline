# The launcher: `sc-offline.exe`

The launcher starts the game with the mod and takes the mod out again when the game closes. Source: `launcher/launcher.cpp`.

## Commands

```text
sc-offline.exe [play|install|uninstall|status|help] [--game <folder>] [--dry-run] [--skip-eac-check]
```

| Command | What it does |
| --- | --- |
| `play` | The default, and what a double-click does. Copies the mod in, starts the game, takes the mod out when the game closes. |
| `install` | Copies the mod in and leaves it there, for starting the game some other way. Run `uninstall` before going online. |
| `uninstall` | Takes the mod out and puts back anything it replaced. Refuses while the game is running. |
| `status` | Runs the checks below and changes nothing. |
| `help` | Prints the usage. |

| Option | Meaning |
| --- | --- |
| `--game <folder>` | Your `Roberts Space Industries`, `StarCitizen`, channel or `Bin64` folder. Overrides `game =` in `sc-offline.ini`. |
| `--dry-run` | Prints every step it would take, then stops. Nothing is copied, deleted or started. |
| `--skip-eac-check` | Don't stop when Easy Anti-Cheat looks active. |

Exit codes: `0` ok · `1` error · `2` Easy Anti-Cheat is active · `3` the game is running.

Everything the launcher prints also goes to `data\launcher.log` (rewritten each run; the helper appends to it).

## Checks it runs every time

1. **Which DLL.** The SHA-256 of the `dinput8.dll` next to the exe, and what it is: a source build (with its
   version), the original author's prebuilt DLL, or unknown. With the prebuilt DLL it warns unless
   `boot_map = PU`.
2. **Game updated?** The game version from `<channel>\build_manifest.id` (or `StarCitizen.exe`'s size and date),
   compared with `data\game-build.txt` from your last play. If it changed, a game update may have broken the mod.
3. **Leftovers.** If the mod is still in the game folder while the game isn't running (a crash, or `install`), it
   says so and tells you to run `uninstall`.
4. **Easy Anti-Cheat.** Whether `C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe` exists (active),
   only as `.exe.bak` (disabled), or not at all, and whether your hosts file blocks
   `modules-cdn.eac-prod.on.epicgames.com`. If EAC is active, `play` and `install` stop and print the fix.

## What `play` does

1. **Finds the game's `Bin64` folder**: `--game`, else `game =` in `sc-offline.ini`, else
   `Roberts Space Industries\StarCitizen\<channel>\Bin64` on every fixed drive. If it finds more than one
   install, it uses the first and lists the others.
2. **Runs the checks** above.
3. **Sets the environment variables** the mod reads (see [data-files.md](data-files.md#environment-variables)).
4. **Starts a helper**, a second copy of `sc-offline.exe` with no window, that changes the game folder:

   | Path | Going in | Coming out |
   | --- | --- | --- |
   | `Bin64\dinput8.dll` | the mod's DLL. If another mod's `dinput8.dll` is there, it is first renamed `dinput8.dll.sc-offline-backup` | deleted; the other mod's DLL renamed back |
   | `<channel>\user\client\0\default_1.xml` | the starting loadout from `data\OfflineDB`. Yours is first copied to `default_1.xml.sc-offline-backup` | your copy restored, or ours deleted if you had none |
   | `Bin64\sc-offline.installed` | a note of what was installed and when | deleted |

   The helper runs as administrator only if the game folder needs it, and then Windows asks once. The game itself
   never runs as administrator.
5. **Starts `StarCitizen.exe`** and waits until every `StarCitizen.exe` has exited. The helper then takes the
   mod out, even if you closed the launcher window first. Ctrl+C in the launcher window is ignored; close the
   game instead.

If the PC crashes mid-game, the mod stays in the game folder. The next `play` or `status` notices; run
`sc-offline.exe uninstall` before going online.

## `sc-offline.ini`

Each line is `key = value`. Lines starting with `#` are comments. An unknown key is reported when the launcher starts, not silently ignored.

| Key | Default | Meaning |
| --- | --- | --- |
| `game` | (search all drives) | Your Star Citizen folder. You can point at `Roberts Space Industries`, at `StarCitizen`, or at the channel folder. |
| `channel` | `LIVE` | Which install to use when `game` points above it: `LIVE`, `PTU`, `EPTU`, and so on. |
| `boot_map` | `PU_All` | `PU_All` loads every star system, so Travel can reach Pyro and Nyx. Use `PU` with the prebuilt repo-root DLL. |
| `start_ship` | `DRAK_Cutlass_Black` | The ship you start in. |
| `start` | (empty) | `Daymar` starts you over Daymar, in that ship. |
