# Building, checks, CI and releases

## Build on Windows

The build is CMake (`CMakeLists.txt` at the root), x64 only, with MSVC.

[sco-core](https://github.com/scubamount/sco-core) (the scanners, signature rows, host kit, plugin loader and sco-lua) is a git submodule in `external/sco-core`, built into `dinput8.dll`. Clone with it:

```
git clone --recurse-submodules https://github.com/scubamount/sc-offline.git
```

In an existing clone, or after a pull that moves the pin, run `git submodule update --init`. Without it, CMake configure and `tools/check.sh` stop and say so.

1. Install **Visual Studio 2026** with the **Desktop development with C++** workload. It includes CMake.
2. In Visual Studio, use **File → Open → Folder** on the repository root. Visual Studio reads `CMakeLists.txt`; pick an x64 **Release** configuration and build. Or from a Developer PowerShell:

   ```powershell
   cmake -S . -B build -G "Visual Studio 18 2026" -A x64
   cmake --build build --config Release --parallel
   ```

   On Visual Studio 2022, configure with `-G "Visual Studio 17 2022" -A x64 -T v143` instead.

3. The build produces `dinput8.dll` (the mod, from `src/`) and `sc-offline.exe` (the launcher, from `launcher/`) in `build\bin\Release\` (`build\bin\Debug\` for Debug; directly in `bin\` of the build folder with a single-configuration generator such as Ninja, which Visual Studio's Open Folder uses by default). To play, put both in a folder together with `data/` and `launcher/sc-offline.ini`. The build also produces the creative plugin (`build\data\plugins\creative\creative.dll`); copy it into `data/plugins/creative/` next to the tracked `plugin.ini` and `disabled` marker, and switch it on in the launcher's Plugins page.

The targets are `dinput8` (the DLL), `sc-offline` (the launcher), `imgui` (the vendored Dear ImGui under `src/third_party/imgui`, built as a static library with its warnings off) `sc-offline-creative` (the optional creative plugin, built from `plugins/creative/` with `/W4 /WX`), the test plugins `spawn_probe`, `builtins_probe` and `ui_probe` (`tools/test-plugins/`, never in a release), `bridge-peer` (only with the bridge options) and sco-core's libraries (`sco_app`, `sco_lua`, `sco_hook`, `sco_engine` and `sco_pak` for the DLL, `sco_sqlite` for the launcher, with what they link). sco-core is built without its tests and without warnings-as-errors; sco-core's own CI runs those. Both binaries link the C runtime statically, so they don't need the Visual C++ redistributable installed. Release builds use whole-program optimization and carry no debug information.

No Visual Studio? CI builds both on every push to `main`. Download the `dinput8-release` artifact from [Actions](https://github.com/scubamount/sc-offline/actions).

## How the code is organized

`src/` is the game-specific bootstrap: the `dinput8.dll` proxy (`dllmain.cpp`), the anti-cheat check, the offline patches, the `WH_GETMESSAGE` hook that runs code on the game's main thread, and the ImGui menu shell. On the hook's first tick it starts sco-core's host kit (`sco::app::Start`): capabilities from `SetFeatureCaps`, the `sco_api` table, the game services, the built-in plugins, then the plugins in `data/plugins/`. That happens whether or not a feature resolved on this game build.

sc-offline's features are built-in plugins. Each is a `sco::plugins::Builtin` in `src/builtins/`, listed in `kBuiltins` in `src/builtins/builtins.h` (ten, plus the two optional bridges), and talks through `sco_api` like any plugin; a built-in is a table, not a DLL export. Which `game.*` service each one runs on is in [architecture.md](architecture.md). The mechanics of the features that have no `game.*` service stay in `src/*.cpp`, and `src/builtins/` may include only SDK headers plus what [`tools/sdk-headers-allow.txt`](../tools/sdk-headers-allow.txt) lists. `plugins/creative/` is the one optional plugin built from this repository, and it includes only SDK headers.

*Historical:* this layout was reached one feature at a time (sco-core's `docs/framework.md`, Phase 4: teleport first, then spawn, and so on), and `teleport.spatial`, `spawn.entities` and the cheats used to live in sc-offline. The first two are published by sco-core's game pack now, and the cheats are the `creative` plugin. See [architecture.md](architecture.md#historical-notes).

## Check on macOS or Linux

```bash
tools/check.sh
```

This runs in a few seconds. It parses every `src/*.cpp`, `src/builtins/*.cpp`, `src/builtins/*/*.cpp` (the bridges) and `launcher/*.cpp` file with clang against mingw-w64's Windows headers. It also screens `src/` for MSVC error C2712 (`__try` in a function that owns an object needing unwinding, such as a `std::string`). It is not a build: only MSVC's build is. Known clang-only diagnostics are listed in `tools/check-baseline.txt`, and only new ones fail the check. You need clang and mingw-w64 (`brew install llvm mingw-w64` on macOS).

### Include and scan gates

```bash
python3 tools/sdk-headers.py
tools/no-scans.sh
```

`tools/sdk-headers.py` checks that every file under `src/builtins/` (the bridges' subfolders included) and `plugins/` includes only headers the sco SDK zip ships (read from `external/sco-core/sdk/package.py`, ImGui's public headers included), files in its own tree, or toolchain headers. Anything else fails with file and line. The exceptions are in `tools/sdk-headers-allow.txt`: one line per header with a reason, and a line whose includes are gone fails too, so the list only shrinks. `plugins/` has no entries and must stay clean.

What the allow-list means: the built-ins are **not** pure SDK plugins. Most of what it lists is `src/spawner.cpp`, `build.cpp`, `teleport.cpp`, `loadout.cpp`, `quantum.cpp`, `contracts.cpp`, `npc.cpp`, `cvars.cpp` and the menu shell, which the built-ins still call directly, plus a few kernel headers (`sco/hook.h`, `sco/net/session.h` and the `game.mining` rows) where the SDK has no form yet. The gate stops new dependencies, not existing ones: a new built-in include of a non-SDK header fails CI, and the fix is a `game.*` service in sco-core, not a new line there. See [architecture.md](architecture.md#what-the-sdk-headers-allow-list-means).

`tools/no-scans.sh` allows no raw pattern scan under `src/` (third-party code excluded): any call of `FindPattern`, `FindUniquePattern`, `FindCString`, `FindRipLea`, `FunctionStart`, `BytesMatch` or `memchr` fails it, and it has no allow-list. Exit code 2 means the scan itself could not run, which never counts as clean. Every game address comes from a sco-core signature row.

### Self-update tests under Wine

```bash
WINE_ROOT=<your Wine build> tools/update-test/run.sh
```

Builds the launcher with `-DSCO_UPDATE_TEST` and runs 14 update scenarios (good update, tampered or unlisted files, downgrade, `..` paths, a failed or killed swap, a locked file, a new launcher that fails `--self-test`) against local release zips under Wine. About 20 s. See `tools/update-test/README.md`. Not run in CI.

## CI

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on pushes to `main`, on pull requests, and on `v*` tags. Every job except `release` runs on each pull request and on each push to `main`, so a green `main` shows the gates hold. The `check`, `sdk-headers`, `build` and `bridges` jobs check out `external/sco-core` (`submodules: true`).

1. `check` runs `tools/check.sh` and the release manifest tests (`python3 tools/test_release_manifest.py`) on Linux, and compares `src/third_party/imgui` (`imconfig.h`, `imgui.h`, `LICENSE.txt`) with the copy the SDK ships.
2. `no-scans` runs `tools/no-scans.sh`: no raw game-memory pattern scan under `src/`. It is its own job so a failure there doesn't hide whether the sources compile.
3. `sdk-headers` runs `tools/sdk-headers.py` (above). Its own job for the same reason.
4. `build` (needs `check`) configures and builds Release x64 with CMake and MSVC on Windows, then checks the binaries with `dumpbin` and `mt`: `dinput8.dll` exports `DirectInput8Create` forwarded to System32's `dinput8.dll`, both files are x64, neither imports the dynamic C runtime, `sc-offline.exe`'s manifest asks for `asInvoker` (the launcher asks for administrator rights itself when it needs them), and the DLL contains neither bridge. It uploads `dinput8-release` (the DLL and the launcher), `creative-plugin` (`creative.dll`) and `test-plugins` (never part of a release).
5. `bridges` (needs `check`) builds with `-DSCO_BRIDGE_TITANLINK=ON -DSCO_BRIDGE_VOXEL=ON`, checks that both bridges and `bridge-peer.exe` are there, and uploads `bridges-test`. A test build only: the release never uses it.
6. `release` (needs `build`) runs on tags only. It publishes `sc-offline-<tag>.zip` and a standalone `dinput8.dll`, with notes generated from the commits. The zip contains the launcher, the DLL, `sc-offline.ini`, `sc-offline.sh`, `data/` (including `data/plugins/creative/`: `plugin.ini`, the `disabled` marker and the `creative.dll` CI built), `docs/`, the README, the CHANGELOG, the LICENSE and `manifest.json`.

### `manifest.json`

`tools/release-manifest.py write` lists every file in the release folder with its SHA-256 and size, plus the version, the tag and the commit. The launcher's self-update uses it to decide which files it may copy; see [Updates](launcher.md#updates). The release job then unzips the finished zip and runs `tools/release-manifest.py check` on it, so a zip that doesn't match its own manifest is never uploaded.

**Bump `SCO_VERSION` in `src/version.h` before tagging.** The manifest's version comes from the tag, and `write` fails the release when the tag (without the `v`) differs from `SCO_VERSION`. A launcher refuses an update whose manifest version differs from the release tag, so a mismatch would break every update to that release.

To try it locally on any folder: `python3 tools/release-manifest.py write <folder> --tag v<version>`, then `python3 tools/release-manifest.py check <folder>`.

Every action is pinned to a commit SHA. The workflow defaults to `contents: read`, and only the `release` job gets `contents: write`.

## Release policy

- `v1.x` tags publish as full releases. Every other tag publishes as a **pre-release**.
- A maintainer can promote a pre-release to Latest with `gh release edit <tag> --prerelease=false --latest`. **That can happen before anyone has played the build.** When it does, the release notes and the CHANGELOG say the build is untested.
- If a promoted build turns out broken, re-mark an earlier release as Latest (`gh release edit <tag> --latest`).

To see the current state, check [Releases](https://github.com/scubamount/sc-offline/releases). This file deliberately doesn't name the current release, so that it can't go stale.
