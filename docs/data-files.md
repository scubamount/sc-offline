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
| `contract_scripts.txt` | 2153 contracts, each with the mission scripts it needs. Only contracts whose scripts are all present can run (1657 with the shipped `scripts/`); see [features.md](features.md#contracts). |
| `scripts/` | The mission scripts that `contract_scripts.txt` points to |
| `OfflineDB/default_1.xml` | The starting loadout. The launcher copies it to `user\client\0` |

## Files the mod creates

| File | Contents |
| --- | --- |
| `wallet.txt` | Your aUEC balance |
| `spawn.txt` | Your F7 / F8 position |
| `bookmarks.txt` | Your named Travel spots |
| `locations_found.txt` | The results of the Travel scan |
| `mod.log` | What the mod did this session |
| `launcher.log` | What `sc-offline.exe` did on its last run (written by the launcher) |
| `game-build.txt` | The game version from your last play, for the launcher's update check |
| `menu_background.png` / `.jpg` | Your menu background image, if you add one |

## Developer test hooks

When one of these files is present, `src/contracts.cpp` or `src/spawner.cpp` changes its behavior. Git ignores all of them. Players don't need them.

| File | Effect |
| --- | --- |
| `contract_test.txt`, `phase_test.txt`, `spawn_test.txt` | Test hooks for contracts, mission phases and spawning |
| `list_first.txt` | Contracts named in this file are listed first |
| `ui_notify_off.txt` | New objectives aren't announced to mobiGlas |

## Environment variables

`sc-offline.exe` sets these before it starts the game. `sc-offline.ini` supplies the first three.

| Variable | Effect |
| --- | --- |
| `SC_OFFLINE_BOOT_MAP` | The boot map, from `boot_map` (default `PU_All`). The prebuilt repo-root DLL doesn't recognize `PU_All` and skips its boot patch, so use `PU` with it. |
| `SC_OFFLINE_START_SHIP` | The ship you start in, from `start_ship` |
| `SC_OFFLINE_START` | From `start`. `Daymar` starts you over Daymar |
| `SC_OFFLINE_SHIPS_FILE` | Where the ship list is read from |
| `SC_OFFLINE_SPAWN_FILE` | Where the F7 position is stored |
| `SC_OFFLINE_MOD_LOG` | Where `mod.log` is written |
| `SC_USER` | The game's `user\client\0` folder, which is how loadouts reach the game |
