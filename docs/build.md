# Building, checks, CI and releases

## Build on Windows

1. Install **Visual Studio 2026** with the **Desktop development with C++** workload. The projects use the `v145` toolset. On VS 2022, retarget them to `v143` first (Project → Retarget).
2. Open `ChrisWareOffline.slnx`, select **Release | x64**, and build. Or from the command line:

   ```powershell
   msbuild ChrisWareOffline.slnx /p:Configuration=Release /p:Platform=x64 /m
   ```

3. The build produces `dinput8.dll` (the mod, from `src/`) and `sc-offline.exe` (the launcher, from `launcher/`) under `x64\Release\`. To play, put both in a folder together with `data/` and `launcher/sc-offline.ini`.

Release builds link the C runtime statically, so they don't need the Visual C++ redistributable installed.

No Visual Studio? CI builds both on every push to `main`. Download the `dinput8-release` artifact from [Actions](https://github.com/scubamount/sc-offline/actions).

## Check on macOS or Linux

```bash
tools/check.sh
```

This runs in a few seconds. It parses every `src/*.cpp` and `launcher/*.cpp` file with clang against mingw-w64's Windows headers. It also screens `src/` for MSVC error C2712 (`__try` in a function that owns an object needing unwinding, such as a `std::string`). It is not a build: only MSVC's build is. Known clang-only diagnostics are listed in `tools/check-baseline.txt`, and only new ones fail the check. You need clang and mingw-w64 (`brew install llvm mingw-w64` on macOS).

## CI

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on pushes to `main`, on pull requests, and on `v*` tags:

1. `check` runs `tools/check.sh` on Linux.
2. `build` runs MSVC Release x64 on Windows and uploads `dinput8-release`.
3. `release` runs on tags only. It publishes `sc-offline-<tag>.zip` and a standalone `dinput8.dll`, with notes generated from the commits. The zip contains the launcher, the DLL, `sc-offline.ini`, `sc-offline.sh`, `data/`, `docs/`, the README, the CHANGELOG and the LICENSE.

Every action is pinned to a commit SHA. The workflow defaults to `contents: read`, and only the `release` job gets `contents: write`.

## Release policy

- `v1.x` tags publish as full releases. Every other tag publishes as a **pre-release**.
- A maintainer can promote a pre-release to Latest with `gh release edit <tag> --prerelease=false --latest`. **That can happen before anyone has played the build.** When it does, the release notes and the CHANGELOG say the build is untested.
- If a promoted build turns out broken, re-mark an earlier release as Latest (`gh release edit <tag> --latest`).

To see the current state, check [Releases](https://github.com/scubamount/sc-offline/releases). This file deliberately doesn't name the current release, so that it can't go stale.

## The prebuilt DLL

The `dinput8.dll` at the repo root is the original author's prebuilt build. It came from a tree that was never published, and it is **not** built from `src/`.

| | |
| --- | --- |
| Size | 1,023,488 bytes |
| SHA-256 | `57e0e5ed2151acc1020fd2ee300944842385f84bb66e216b5ebecb1ee57c10c9` |
| Boot map | Use `boot_map = PU`. It doesn't recognize `PU_All`. |
| Runtime | Needs the Visual C++ redistributable. It imports `MSVCP140.dll` and `VCRUNTIME140.dll`. |

To play it, copy it over the `dinput8.dll` in your extracted release folder, and set `boot_map = PU` in `sc-offline.ini`.
