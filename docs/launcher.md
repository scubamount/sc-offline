# The launcher: `sc-offline.exe`

The launcher starts the game with the mod and takes the mod out again when the game closes. Source: `launcher/launcher.cpp`.

## Commands

```text
sc-offline.exe [play|install|uninstall|status|update|help] [--game <folder>] [--dry-run] [--skip-eac-check]
```

| Command | What it does |
| --- | --- |
| `play` | The default from a terminal (a double-click opens the [window](#the-window)). Copies the mod in, starts the game, takes the mod out when the game closes. |
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
| `--window` | Open the window from a terminal, or under Wine/Proton. |
| `--console` | Double-clicked: run `play` in the console instead of opening the window. |

Exit codes: `0` ok · `1` error · `2` Easy Anti-Cheat is active · `3` the game is running. The helper's own codes, in `launcher.log`: `5` a PC change failed.

Everything the launcher prints also goes to `data\launcher.log` (rewritten each run; the helper appends to it).

## The window

A double-click (no arguments, the exe's own console) opens a 720×328 window instead of the console run. It is
drawn with ImGui on Direct3D 11 (the vendored `src/third_party/imgui`, the copy the mod's menu uses). If
Direct3D 11 isn't available, it says so and tells you to run `sc-offline.exe play` from a terminal. Under Wine the
console run stays the default; `--window` opens the window anyway.

The sidebar has three pages:

- **Home**: the *Launcher* list (**Play**, **Status**, **Update**, **Install**, **Uninstall**) with one button below it
  for the picked row, and the *Status* banner. Each command runs `sc-offline.exe <command>` as a hidden child with
  `SC_OFFLINE_GUI=1`, so the window and the CLI share one code path. The child's `[y/N]` questions turn the button
  into **Yes** / **No** (or press Y / N). Commands are disabled while one runs and while the game runs (except
  **Status**); **Uninstall** only when something is left to undo.
- **Plugins**: see [below](#the-plugins-page).
- **Output**: what the running command prints, with a **Copy** button. **Status** and **Update** switch to it.

The top bar has the **Discord** switch (`discord_presence` in `sc-offline.ini`, used from the next Play), **Settings**
(opens `sc-offline.ini` in Notepad), **Logs** (opens `data`), minimize and close. Drag the empty part of the bar to
move the window.

The banner uses only cheap checks: the game folder from `game =` or `data\game-path.txt`, the
`sc-offline.installed` marker or sc-offline's `dinput8.dll` in Bin64, and `%ProgramData%\sc-offline\pc-changes.txt`.
It refreshes every 1.5 seconds, including while **Play** is running, and after every command. Hover it for the
detail:

- **Safe to go online**: nothing of the mod is in Bin64 and no PC changes are left;
- **Not safe to go online** / **yet**: the game is running, or it lists what is left;
- **Game folder not known yet**: click **Status** or **Play** to find it.

After **Update** applies a new version, the window restarts itself. With any argument, the console run is
unchanged.

### The Plugins page

The page manages `data\plugins\<id>\` the way the host (sco-core) reads it, with no separate list of its own:

- **What is listed**: every folder under `data\plugins` that holds a `plugin.ini` (a folder without one is skipped,
  as the host does), with its `name`, `version` and `author`; `description`, when a plugin adds one, is the
  tooltip. Folders are rescanned every 3 seconds.
- **On / off**: the switch creates or deletes an empty file named `disabled` in the plugin's folder. The host
  reads that marker when the game starts, so a change applies at the next start. The old `data\mods\mod.txt` /
  `enabled.txt` split is gone: nothing here reads or writes it.
- **Plugins are off**: the host loads no plugin unless `plugins = on` in `sc-offline.ini` (the default is off).
  When it is off, the page says so and **Turn on** writes the key.
- **Settings** (the gear, for a plugin whose `plugin.ini` has a `[settings]` section): edits the values the
  plugin declared (`bool`, `int`, `float`, `string`, `enum(...)`), checked against the declared range and
  choices with the host's rules. **Save** writes them where `sco.settings` keeps them: the plugin's own database
  `data\storage\<id>.db`, table `sco_kv`, key `sco.settings.<name>`, value `<type>:<value>`. **Reset** deletes the
  kept values, so the defaults apply. The fields are read-only while the game or a command is running: the host reads
  its settings once, at start, so a write during a session would not be seen and no `settings.changed` would fire.
  Close the game, edit, start it again. A plugin whose `[settings]` this page can't read is flagged; run
  `sco-plugin-check` on it.
- **Reload**: only shows a note. Hot reload runs inside the game, through its `sco.reload` command; the launcher
  has no channel into the running game to call it.

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
| `block_network` | Windows Firewall rules, inbound and outbound: `sc-offline: block StarCitizen.exe` (this install's exe only), `sc-offline: block RSI Launcher.exe` (found from its install entry, beside the game library, or the default folder) and `sc-offline: block CrashHandler.exe` (`<channel>\Tools\Public\CrashHandler.exe`, CIG's crash reporter). A rule is only added if its exe exists. `sc-offline.exe` itself stays online for updates. With `multiplayer = on` (the default), the `StarCitizen.exe` rule blocks every address except loopback, the private ranges (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`), link-local (`169.254.0.0/16`) and `multiplayer_allow` (all IPv6 stays blocked), and one more rule, `sc-offline: allow StarCitizen.exe multiplayer (LAN)`, lets PCs on your local subnet (and `multiplayer_allow`) reach the game over UDP, for a session you host from the [Multiplayer tab](features.md#multiplayer-lan-or-vpn). If Windows refuses the narrower rule, the launcher blocks every address as before and says so. | deleted |
| `eac_hosts` | `127.0.0.1 modules-cdn.eac-prod.on.epicgames.com # added by sc-offline…` appended to the hosts file, then `ipconfig /flushdns`. Skipped if the hosts file already blocks it | only the tagged line removed, DNS flushed |
| `eac_rename` | `EasyAntiCheat_EOS.exe` renamed to `EasyAntiCheat_EOS.exe.bak`. Skipped if it isn't there | renamed back |

- Each change is written to `%ProgramData%\sc-offline\pc-changes.txt` the moment it is made, and only recorded
  changes are undone. A hosts line or `.bak` you made yourself is never touched.
- If a step fails, the helper undoes what it already did and the game doesn't start.
- After a crash the record stays. `status` lists it, `uninstall` undoes it, and `play` offers to.

### Updates

On `play` and `status` the launcher asks GitHub for the newest release of `scubamount/sc-offline`
(3-second timeout; no network just skips it). With `update_channel = stable` (the default) only full
releases are offered; `update_channel = prerelease` also offers pre-releases (test builds). Drafts, older
versions and tags that aren't plain numbers are never offered. If a newer one exists, `play` asks
**"Update now?"** and `status` tells you to run `sc-offline.exe update`. It won't update while
`StarCitizen.exe` is running.

An update:
1. checks there's free disk space, then downloads `sc-offline-<tag>.zip`. The download only times out when
   it stalls, and is tried once more. The zip's SHA-256 must match the `digest` GitHub publishes for that
   asset; a mismatch or a missing digest stops it with nothing changed;
2. unpacks it with Windows' own `tar.exe` and checks it against its `manifest.json` (see below). Any
   problem stops it with nothing changed;
3. stages the listed files in `data\update\staged\`. This runs with normal rights. When the install folder
   isn't writable (Program Files), it stages in `%LOCALAPPDATA%\sc-offline\update\` instead, and only the
   next step asks Windows for administrator rights. That step uses no network: it checks every staged
   file against the manifest again before using it;
4. replaces every listed file except `sc-offline.ini` and the player files in `data\` (`wallet.txt`,
   `spawn.txt`, `bookmarks.txt`, `locations_found.txt`, `game-path.txt`, `game-build.txt`, the logs).
   `data\storage\` (your saves) is never in a release, so an update doesn't touch it.
   Each file is written as `<name>.update-new`, flushed to disk, the old one renamed to `<name>.update-old`,
   and the new one moved into place and checked again. Each step is written to `data\update\applied.txt`
   before it happens. A file that antivirus is scanning is retried for a few seconds;
5. appends settings that are new in the release's `sc-offline.ini` to yours, commented out, so their
   defaults apply;
6. runs `sc-offline.exe --self-test` from the new files, then starts the new launcher with the same arguments.

If a step fails, or the new launcher fails its self-test, the old files are put back at once. If the PC dies
mid-update, the next run of `sc-offline.exe` reads `applied.txt` and puts them back. The `.update-old` files
are deleted on the next good start. `check_updates = off` turns the check off; `sc-offline.exe update`
checks on demand.

#### What an update is checked against

Two things vouch for an update. Neither is a signature: releases aren't code-signed.

1. **GitHub's digest.** GitHub publishes a SHA-256 `digest` for every release asset, served over HTTPS
   from `api.github.com`. The downloaded zip must match it.
2. **`manifest.json` inside the zip.** CI writes it when it builds the release: the `version`, the `tag`,
   the `commit` it was built from, and every other file in the zip with its `sha256` and `size`, one per line.
   Its version must equal the release tag. Only files it lists are copied, and only when their SHA-256 matches.
   A listed path with `..`, a leading `/`, a `\` or a `:` makes the whole update fail.

So the update is exactly what CI built from that commit, as long as the GitHub release itself is
trustworthy. Anyone who can publish a release on `scubamount/sc-offline` can publish an update.

#### Checking a release by hand

On Windows (PowerShell), in the folder you downloaded the zip to:

```powershell
# 1. The zip against GitHub's digest (shown on the release page next to the asset, or:)
#    gh release view v0.7.0 -R scubamount/sc-offline --json assets --jq '.assets[] | [.name, .digest]'
(Get-FileHash .\sc-offline-v0.7.0.zip -Algorithm SHA256).Hash.ToLower()

# 2. Every unpacked file against manifest.json
Expand-Archive .\sc-offline-v0.7.0.zip -DestinationPath .
$dir = '.\sc-offline-v0.7.0'
$man = Get-Content "$dir\manifest.json" -Raw | ConvertFrom-Json
"$($man.tag) from commit $($man.commit), $($man.files.Count) files"
foreach ($f in $man.files) {
  $p = Join-Path $dir $f.path
  if (-not (Test-Path $p)) { "MISSING  $($f.path)"; continue }
  if ((Get-FileHash $p -Algorithm SHA256).Hash.ToLower() -ne $f.sha256) { "CHANGED  $($f.path)" }
}
```

No output after the summary line means every listed file matches. A file in the folder that the manifest
doesn't list (other than `manifest.json`) isn't part of the release. With Python (macOS, Linux or Windows),
`python3 tools/release-manifest.py check sc-offline-v0.7.0` from a clone of this repository does step 2,
also flags unlisted files, and exits non-zero on any problem. The `commit` field lets you
look up the exact source at `https://github.com/scubamount/sc-offline/tree/<commit>`.

### Discord status

With the Discord app open on the same PC, **Play** sets your Discord status once the game starts:

- **Playing sc-offline**: `Star Citizen offline mod`, `v<version> · single player`, and the time played
- the sc-offline logo, and two buttons other people can click: **Join the Discord** and **Get sc-offline**

The launcher talks only to the Discord app on your PC (its local pipe). It sends nothing over the network, and it needs no Discord login or token. The status clears when the game closes. If Discord isn't running, nothing happens and `launcher.log` says `Discord: not running`; if Discord starts later, the launcher picks it up within 15 seconds. Under Wine or Proton the Discord pipe usually isn't reachable, so nothing is shown.

To turn it off, untick **Show on Discord** in the window or set `discord_presence = off` in `sc-offline.ini`. The change applies from the next **Play**.

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

There is no wallet setting: your aUEC balance is kept in `data\storage\contracts.db` and `data\wallet.txt`, which you can edit (see [Features](features.md#player)).

| Key | Default | Meaning |
| --- | --- | --- |
| `game` | (found automatically) | Your Star Citizen folder. You can point at `Roberts Space Industries`, at `StarCitizen`, at the channel folder, or at the folder that holds `StarCitizen`. |
| `discord_presence` | `on` | While the game runs, your Discord profile shows **Playing sc-offline** with the version, time played and two buttons (the sc-offline Discord, the download page). Your Discord friends and servers see it. Also the **Show on Discord** box in the window. See [Discord status](#discord-status). |
| `check_updates` | `on` | Check GitHub for a newer release on `play` and `status` and offer to install it. |
| `update_channel` | `stable` | `stable`: only full releases are offered. `prerelease`: pre-releases (test builds) are offered too. |
| `crash_reports` | `on` | After a crash, offer a redacted log bundle and the bug form. See [Crash reports](#crash-reports). |
| `clean_logs` | `ask` | After the game closes, list this session's game logs and ask (twice) before deleting them. `off` skips it. |
| `channel` | `LIVE` | Which install to use when `game` points above it: `LIVE`, `PTU`, `EPTU`, and so on. |
| `boot_map` | `PU_All` | `PU_All` loads every star system, so Travel can reach Pyro and Nyx. |
| `start_ship` | `DRAK_Cutlass_Black` | The ship used by `start = Daymar`. **Without `start = Daymar` it does nothing.** |
| `start` | (empty) | Empty: the game's own spawn (Orison with `boot_map = PU`, a Pyro station with `PU_All`). `Daymar`: about 10 seconds after you spawn, `start_ship` is spawned over Daymar and you're put in its pilot seat. |
| `plugins` | `off` | `on`: load plugins from `data\plugins\<id>\`. See [Plugins](features.md#plugins). |
| `asop_fleet_list` | `ships` | Which ship list the terminals show with `asop = on`: `game` (the game's own; offline its entitlement query fails, so it's empty) or `ships` (every ship in `data\ships.txt`). |
| `asop` | `on` | `off`: leave the ship terminals, personal hangars, the hangar lift and ATC hails as the game has them offline. See [Ship terminals, hangars and ATC](features.md#ship-terminals-hangars-and-atc). |
| `block_network` | `on` | Block `StarCitizen.exe`, the RSI Launcher and the game's `CrashHandler.exe` in Windows Firewall while you play. See [PC changes](#pc-changes). |
| `multiplayer` | `on` | The [Multiplayer tab](features.md#multiplayer-lan-or-vpn). Nothing connects until you press Host or Join there; with `block_network` on, the firewall leaves your LAN open for it (see [PC changes](#pc-changes)). `off`: no tab, and the firewall blocks every address. |
| `multiplayer_allow` | (empty) | Extra networks a session may use besides your LAN, for a VPN: up to 8 IPv4 ranges separated by commas, `/8` or narrower (Tailscale: `100.64.0.0/10`). Every address in them can reach a session you host, so allow only your VPN's range. A value that doesn't parse is ignored with a warning. |
| `eac_hosts` | `on` | Add the EAC hosts line while you play. |
| `eac_rename` | `on` | Rename `EasyAntiCheat_EOS.exe` while you play. |
