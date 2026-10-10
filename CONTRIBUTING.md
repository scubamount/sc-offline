# Contributing to sc-offline

Thanks for helping. sc-offline is a Star Citizen offline mod: a mod menu plus a launcher that keeps the game offline while you play. Bug reports, fixes, docs and testing on your PC all help.

## What fits this project

- **Offline, never CIG's servers.** Two kinds of connection are allowed: local IPC with other processes on the same PC (bridges such as Titanfall 2/Northstar or Minecraft), and private co-presence between sc-offline players over a LAN or VPN through sco-core's `sco.net`. Never: connecting to Star Citizen's servers or online services, public or official online play, getting around anti-cheat or skipping its steps, signature-check bypass, account or entitlement tampering, forcing the game's host type or network context, unauthenticated remote commands between peers, telemetry, or anything that helps anyone cheat in the official game or hurt other players. The same rules bind plugins: [Plugin rules](https://github.com/scubamount/sco-core/blob/main/sdk/docs/plugin-rules.md). PRs like that are closed.
- **No game files.** Don't commit Star Citizen files, extracted game data, or anything copied from Cloud Imperium Games. Ship class names and other identifiers in `data/` are fine.
- **No secrets or personal data.** Strip account names, Windows user paths and tokens from logs before you post them.
- **Undo what you change.** Anything the launcher changes on the PC (hosts file, firewall rules, renamed files) must be listed and undone afterwards; see [PC changes](docs/launcher.md#pc-changes).

## Reporting a bug

Use the [Bug report](https://github.com/scubamount/sc-offline/issues/new/choose) form. Include the output of `sc-offline.exe status` and attach `data/launcher.log`, `data/mod.log` and the game's `Game.log`. One problem per issue. Check [Troubleshooting](README.md#troubleshooting) first.

For security problems (the self-update, the administrator helper, a change the launcher doesn't undo), **don't open a public issue**; see [SECURITY.md](SECURITY.md).

Questions and ideas: open an issue or ask in the [Discord](https://discord.gg/NJKeVfYCCC).

## Making a change

1. For anything bigger than a small fix, open an issue first so we can agree on the approach.
2. Fork the repo and branch from `main`.
3. Build and check as described in [docs/build.md](docs/build.md):
   - Windows: build **Release** x64 with CMake (Visual Studio opens the folder, or `cmake` from the command line).
   - macOS or Linux: `tools/check.sh` must report `0 new` diagnostics. It isn't a build; CI's MSVC build is the real check.
   - `python3 tools/sdk-headers.py` must pass if you touched `src/builtins/`: built-ins include only SDK headers (see [docs/build.md](docs/build.md#include-and-scan-gates)).
   - Touching the self-update: run `tools/update-test/run.sh` under Wine (see [docs/build.md](docs/build.md#self-update-tests-under-wine)).
4. Test in game if you can, and say in the PR what you tested and what you didn't.
5. Update the docs your change affects (`README.md`, `docs/`, `sc-offline.ini` comments) in the same PR.
6. Add a line under the top section of [CHANGELOG.md](CHANGELOG.md) for anything a player would notice.
7. Open the PR against `main` and fill in the template. CI (`check`, `build`, `bridges`, `no-scans` and `sdk-headers`) must pass.

PRs are squash-merged. Keep one change per PR.

## Code style

- Match the surrounding code: C++ in `src/` (the mod) and `launcher/` (the launcher), no new dependencies without discussing it first.
- In `src/`, don't put `__try` in a function that owns objects with destructors (such as `std::string`); MSVC rejects it (C2712) and `tools/check.sh` screens for it.
- Write messages players will read in plain words: what happened and what to do.

## Releases

Maintainers tag releases; contributors don't need to bump versions. See [Release policy](docs/build.md#release-policy).

## License

sc-offline is GPL-3.0 ([LICENSE](LICENSE)). By opening a pull request you agree your contribution is licensed under GPL-3.0.
