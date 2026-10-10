# Data files and environment variables

Everything the menu reads is plain text in `data/`. Most files explain their own format in a header comment.

## Shipped lists

| File | Contents |
| --- | --- |
| `ships.txt` | 1102 spawnable vehicles |
| `npcs.txt` | 2239 NPC archetypes |
| `items.txt` | 1489 gear items, in ten slot sections |
| `buildables.txt` | 3728 objects in 18 groups, for Build mode and the Squadron 42 Spawn section |
| `outfits.txt` | 35 outfits: each is a `[name]` line, then one line per piece |
| `locations.txt` | Places for the Travel tab: system, name, entity, radius |
| `contract_scripts.txt` | The 796 contracts that run without any CIG mission script; see [features.md](features.md#contracts). |
| `builtin/quantum/datacore/quantum_drive.toml` | The new quantum drive's game data, applied as the game loads (see [features.md](features.md#logging)) |
| `OfflineDB/default_1.xml` | The starting loadout. The launcher copies it to `user\client\0` |

## Files the mod creates

| File | Contents |
| --- | --- |
| `storage\` | Your saves: `teleport.db` (the F7 / F8 position), `quantum.db` (your named Travel spots), `contracts.db` (your aUEC balance), each with SQLite's `-wal` and `-shm` files, and any plugin's own database. See [Saves](#saves) |
| `wallet.txt` | Your aUEC balance, also in `storage\contracts.db`; you can edit it |
| `spawn.txt` | Your F7 / F8 position from before `storage\`; read once, then left as it is |
| `bookmarks.txt` | Your named Travel spots from before `storage\`; read once, then left as it is |
| `locations_found.txt` | The results of the Travel scan |
| `mod.log` | What the mod did this session |
| `launcher.log` | What `sc-offline.exe` did on its last run (written by the launcher) |
| `game-path.txt` | The game folder the launcher found last time; delete it to make it search again |
| `game-build.txt` | The game version from your last play, for the launcher's update check |
| `menu_background.png` / `.jpg` | Your menu background image, if you add one |

## Saves

The F7 / F8 spot, the Travel tab's saved spots and the wallet are kept by the built-in plugins `teleport`, `quantum` and `contracts` in sco-core's `sco.storage` service: one SQLite database each in `data\storage\<built-in>.db`. A save there is all or nothing, so a crash keeps either the old save or the new one.

The old files are imported, not deleted:

- `spawn.txt` and `bookmarks.txt` are read into storage the first time the game starts with this version (`[storage] imported spawn.txt into teleport` in `mod.log`) and are not written after that. They stay readable as they were. If one changes later (you copy one in from an older install, say), it is newer than what storage has seen and is imported again, replacing the saved spot or the saved spot list.
- `wallet.txt` is still written every time your balance is saved, because you can edit it (see [Features](features.md#player)). Storage records the file's time with each save. When the game starts and `wallet.txt` has a different time (you edited it, or copied one in), the file wins and is imported again (`[storage] imported wallet.txt into contracts`). Deleting `wallet.txt` still takes you back to the starting amount: storage's balance is cleared with it.

If storage can't be used (a built-in didn't load, or a database can't be opened or written), `mod.log` says so with a `[storage]` line and that feature reads and writes its `.txt` file as before. Delete the three files of a database (`.db`, `.db-wal`, `.db-shm`) while the game is closed to forget what it holds; the `.txt` file, if there is one, is imported again on the next start.

## Developer test hooks

When one of these files is present, `src/contracts.cpp` or `src/spawner.cpp` changes its behavior. Git ignores all of them. Players don't need them.

| File | Effect |
| --- | --- |
| `contract_test.txt`, `phase_test.txt`, `spawn_test.txt` | Test hooks for contracts, mission phases and spawning |
| `list_first.txt` | Contracts named in this file are listed first |
| `ui_notify_off.txt` | New objectives aren't announced to mobiGlas |

## Environment variables

`sc-offline.exe` sets these before it starts the game. `sc-offline.ini` supplies the first four.

| Variable | Effect |
| --- | --- |
| `SC_OFFLINE_BOOT_MAP` | The boot map, from `boot_map` (default `PU_All`). |
| `SC_OFFLINE_START_SHIP` | The ship you start in, from `start_ship` |
| `SC_OFFLINE_START` | From `start`. `Daymar` starts you over Daymar |
| `SC_OFFLINE_PLUGINS` | From `plugins`: `on` loads plugins from `plugins\` beside the ship list (`data\plugins`); anything else is off |
| `SC_OFFLINE_MULTIPLAYER` | From `multiplayer`: `off` switches the Multiplayer tab off; anything else (or unset) leaves it on |
| `SC_OFFLINE_MULTIPLAYER_ALLOW` | From `multiplayer_allow`, checked by the launcher: extra IPv4 ranges a session may use |
| `SC_OFFLINE_SHIPS_FILE` | Where the ship list is read from |
| `SC_OFFLINE_SPAWN_FILE` | Where the F7 position is stored |
| `SC_OFFLINE_MOD_LOG` | Where `mod.log` is written |
| `SC_USER` | The game's `user\client\0` folder, which is how loadouts reach the game |
