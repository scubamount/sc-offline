# The launcher: `sc-offline.exe`

The launcher starts the game with the mod and takes the mod out again when the game closes. Source: `launcher/launcher.cpp`.

## What it does

1. **Finds the game's `Bin64` folder.** It tries these in order, using the first that works:
   - `--game <folder>` on the command line;
   - `game =` in `sc-offline.ini`;
   - `Roberts Space Industries\StarCitizen\<channel>\Bin64` on every fixed drive.

   If it finds more than one install, it uses the first and lists the others.
2. **Sets the environment variables** the mod reads (see [data-files.md](data-files.md#environment-variables)).
3. **Starts a helper**: a second copy of `sc-offline.exe` running with `--watch`. The helper:
   - copies `dinput8.dll` into `Bin64`;
   - copies `data\OfflineDB\default_1.xml` to `<channel>\user\client\0\default_1.xml`.

   The helper runs as administrator only if a normal copy into the game folder fails, and then Windows asks once. The game itself never runs as administrator.
4. **Starts `StarCitizen.exe`** and waits until every `StarCitizen.exe` process has exited.
5. **Removes the mod.** The helper deletes `Bin64\dinput8.dll`. It does this even if you closed the launcher window first. Ctrl+C in the launcher window is ignored; close the game instead.

## What it leaves behind

- `user\client\0\default_1.xml` stays after the game closes, and it overwrites any `default_1.xml` you already had there.
- If the PC crashes or loses power, `Bin64\dinput8.dll` stays too. Delete it yourself before playing online.

## `sc-offline.ini`

Each line is `key = value`. Lines starting with `#` are comments. An unknown key is reported when the launcher starts, not silently ignored.

| Key | Default | Meaning |
| --- | --- | --- |
| `game` | (search all drives) | Your Star Citizen folder. You can point at `Roberts Space Industries`, at `StarCitizen`, or at the channel folder. |
| `channel` | `LIVE` | Which install to use when `game` points above it: `LIVE`, `PTU`, `EPTU`, and so on. |
| `boot_map` | `PU_All` | `PU_All` loads every star system, so Travel can reach Pyro and Nyx. Use `PU` with the prebuilt repo-root DLL. |
| `start_ship` | `DRAK_Cutlass_Black` | The ship you start in. |
| `start` | (empty) | `Daymar` starts you over Daymar, in that ship. |

## Command line

```text
sc-offline.exe [--game <Star Citizen folder>]
```

`--watch` is internal: it's the launcher starting its own helper.

## Exit codes of the helper

`0` the mod was removed · `2` the copy failed · `3` the mod couldn't be removed after 30 tries · `4` bad arguments.
