# Building, checks, CI and releases

## Build on Windows

The build is CMake (`CMakeLists.txt` at the root), x64 only, with MSVC.

1. Install **Visual Studio 2026** with the **Desktop development with C++** workload. It includes CMake.
2. In Visual Studio, use **File → Open → Folder** on the repository root. Visual Studio reads `CMakeLists.txt`; pick an x64 **Release** configuration and build. Or from a Developer PowerShell:

   ```powershell
   cmake -S . -B build -G "Visual Studio 18 2026" -A x64
   cmake --build build --config Release --parallel
   ```

   On Visual Studio 2022, configure with `-G "Visual Studio 17 2022" -A x64 -T v143` instead.

3. The build produces `dinput8.dll` (the mod, from `src/`) and `sc-offline.exe` (the launcher, from `launcher/`) in `build\bin\Release\` (`build\bin\Debug\` for Debug; directly in `bin\` of the build folder with a single-configuration generator such as Ninja, which Visual Studio's Open Folder uses by default). To play, put both in a folder together with `data/` and `launcher/sc-offline.ini`.

The targets are `dinput8` (the DLL), `sc-offline` (the launcher) and `imgui` (the vendored Dear ImGui under `src/third_party/imgui`, built as a static library with its warnings off). Both binaries link the C runtime statically, so they don't need the Visual C++ redistributable installed. Release builds use whole-program optimization and carry no debug information.

No Visual Studio? CI builds both on every push to `main`. Download the `dinput8-release` artifact from [Actions](https://github.com/scubamount/sc-offline/actions).

## Check on macOS or Linux

```bash
tools/check.sh
```

This runs in a few seconds. It parses every `src/*.cpp` and `launcher/*.cpp` file with clang against mingw-w64's Windows headers. It also screens `src/` for MSVC error C2712 (`__try` in a function that owns an object needing unwinding, such as a `std::string`). It is not a build: only MSVC's build is. Known clang-only diagnostics are listed in `tools/check-baseline.txt`, and only new ones fail the check. You need clang and mingw-w64 (`brew install llvm mingw-w64` on macOS).

### Self-update tests under Wine

```bash
WINE_ROOT=<your Wine build> tools/update-test/run.sh
```

Builds the launcher with `-DSCO_UPDATE_TEST` and runs 14 update scenarios (good update, tampered or unlisted files, downgrade, `..` paths, a failed or killed swap, a locked file, a new launcher that fails `--self-test`) against local release zips under Wine. About 20 s. See `tools/update-test/README.md`. Not run in CI.

## CI

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on pushes to `main`, on pull requests, and on `v*` tags:

1. `check` runs `tools/check.sh` and the release manifest tests (`python3 tools/test_release_manifest.py`) on Linux.
2. `build` configures and builds Release x64 with CMake and MSVC on Windows, then checks the binaries with `dumpbin` and `mt`: `dinput8.dll` exports `DirectInput8Create` forwarded to System32's `dinput8.dll`, both files are x64, neither imports the dynamic C runtime, and `sc-offline.exe`'s manifest asks for `asInvoker` (the launcher asks for administrator rights itself when it needs them). It uploads both as `dinput8-release`.
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
