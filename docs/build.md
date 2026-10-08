# Building, checks, CI and releases

## Build on Windows

The game-facing core lives in a separate repository, [sco-core](https://github.com/scubamount/sco-core), checked out at `external/sco-core`. Clone with `git clone --recurse-submodules`, or run `git submodule update --init` in an existing clone. The build compiles sco-core's sources into the same `dinput8.dll`.

1. Install **Visual Studio 2026** with the **Desktop development with C++** workload. The projects use the `v145` toolset. On VS 2022, retarget them to `v143` first (Project → Retarget).
2. Open `sc-offline.slnx`, select **Release | x64**, and build. Or from the command line:

   ```powershell
   msbuild sc-offline.slnx /p:Configuration=Release /p:Platform=x64 /m
   ```

3. The build produces `dinput8.dll` (the mod, from `src/`) and `sc-offline.exe` (the launcher, from `launcher/`) under `x64\Release\`. To play, put both in a folder together with `data/` and `launcher/sc-offline.ini`.

Release builds link the C runtime statically, so they don't need the Visual C++ redistributable installed.

No Visual Studio? CI builds both on every push to `main`. Download the `dinput8-release` artifact from [Actions](https://github.com/scubamount/sc-offline/actions).

## Check on macOS or Linux

```bash
tools/check.sh
```

This runs in a few seconds. It parses every `src/*.cpp`, `launcher/*.cpp` and `external/sco-core/src/` file with clang against mingw-w64's Windows headers. It also screens `src/` for MSVC error C2712 (`__try` in a function that owns an object needing unwinding, such as a `std::string`). It is not a build: only MSVC's build is. Known clang-only diagnostics are listed in `tools/check-baseline.txt`, and only new ones fail the check. You need clang and mingw-w64 (`brew install llvm mingw-w64` on macOS).

### Game addresses

Every game address the mod uses is becoming a named row in sco-core's signature tables (`external/sco-core/src/game/`). Teleport is the first; the other features still look theirs up in `src/`. At startup `mod.log` gets a `[core] signatures: N/M OK` line plus one line for each row that failed. To check a game build without starting the game, run `external/sco-core/tools/sigcheck.sh <path to StarCitizen.exe>`; see sco-core's README.

### Self-update tests under Wine

```bash
WINE_ROOT=<your Wine build> tools/update-test/run.sh
```

Builds the launcher with `-DSCO_UPDATE_TEST` and runs 14 update scenarios (good update, tampered or unlisted files, downgrade, `..` paths, a failed or killed swap, a locked file, a new launcher that fails `--self-test`) against local release zips under Wine. About 20 s. See `tools/update-test/README.md`. Not run in CI.

## CI

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on pushes to `main`, on pull requests, and on `v*` tags:

1. `check` runs `tools/check.sh` and the release manifest tests (`python3 tools/test_release_manifest.py`) on Linux.
2. `build` runs MSVC Release x64 on Windows and uploads `dinput8-release`.
3. `release` runs on tags only. It publishes `sc-offline-<tag>.zip` and a standalone `dinput8.dll`, with notes generated from the commits. The zip contains the launcher, the DLL, `sc-offline.ini`, `sc-offline.sh`, `data/`, `docs/`, the README, the CHANGELOG, the LICENSE and `manifest.json`.

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
