# tools/update-test: the self-update path under Wine (issue #31 D6)

`tools/update-test/run.sh [launcher.cpp]` builds the launcher with mingw-w64 and `-DSCO_UPDATE_TEST` at 0.6.9 (installed) and 0.7.0 (new), makes one release zip per scenario with `tools/release-manifest.py`, and runs `sc-offline.exe --apply-zip <zip> <tag> <sha256>` under Wine against a fresh 0.6.9 folder for each. It prints `PASS name` or `FAIL name: reason` per scenario and exits non-zero on any failure. `ONLY="good kill"` runs a subset.

| Scenario | Expected |
|---|---|
| `good` | applied; shipped files are the release's; `data/wallet.txt` and the player's ini line kept |
| `tampered`, `unlisted`, `downgrade`, `same-version`, `tag-mismatch`, `dotdot`, `abspath`, `ads`, `zip-digest` | refused (exit 1); install unchanged byte for byte; nothing written outside it |
| `mid-swap` | `SCO_TEST_FAIL_AFTER=2` fails the swap after 2 files: exit 1, old files back |
| `kill` | Wine killed mid-swap (swap waiting on a locked `dinput8.dll`); the next start puts the old files back |
| `locked` | `hold-file.exe` holds `dinput8.dll` for 2 s; the swap retries and succeeds |
| `selftest-fail` | the new `sc-offline.exe` fails `--self-test`: exit 1, old files back |

Needs `x86_64-w64-mingw32-g++`, `python3`, `perl` and a Wine build: `WINE_ROOT` with `inst/bin/wine`, `inst/bin/wineserver` and an initialized prefix in `prefix/` (FreeType under `x86/lib` on macOS). The harness runs in a copy of that prefix under `/tmp/sco-ut`, never the original. Wine has no `tar.exe`, so `tar-shim.cpp` (stored zip entries only; refuses `..` and absolute paths like bsdtar) goes into the copy's System32. Not in CI yet.
