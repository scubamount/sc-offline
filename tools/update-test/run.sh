#!/usr/bin/env bash
# Wine scenarios for the launcher's self-update path (issue #31 D6).
#
#   tools/update-test/run.sh [path/to/launcher.cpp]       # default: launcher/launcher.cpp in this repo
#   ONLY="good tampered" tools/update-test/run.sh ...     # a subset
#
# Builds the launcher twice with mingw-w64 and -DSCO_UPDATE_TEST (installed = 0.6.9, new = 0.7.0), makes
# one release zip per scenario (make_fixtures.py), installs 0.6.9 into a fresh folder per scenario, runs
#   sc-offline.exe --apply-zip <zip> <tag> <sha256>
# under Wine and checks the folder on disk afterwards. `kill` kills Wine mid-swap (while the swap waits
# on a locked dinput8.dll) and checks the next start puts the old files back. Prints `PASS name` /
# `FAIL name: reason` per scenario and exits 1 if any failed, 2 if the harness itself couldn't run.
#
# Needs: x86_64-w64-mingw32-g++, python3, perl, and a Wine build in WINE_ROOT (inst/bin/wine,
# inst/bin/wineserver, an initialized prefix/, FreeType in x86/lib on macOS). The prefix is cloned into the
# work dir, so the real one is never touched; tar-shim.exe goes in the clone's System32 as tar.exe,
# because Wine has no tar.exe and the launcher unpacks with it.
# Env: MANIFEST_TOOL (tools/release-manifest.py), UT (work dir, default /tmp/sco-ut), LOCK_MS (2000).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
CPP=$(cd "$(dirname "${1:-$REPO/launcher/launcher.cpp}")" && pwd)/$(basename "${1:-launcher.cpp}")
SRC_REPO=$(cd "$(dirname "$CPP")/.." && pwd)
UT=${UT:-/tmp/sco-ut}
LOCK_MS=${LOCK_MS:-2000}
WINE_ROOT=${WINE_ROOT:-}
CXX=${CXX:-$(command -v x86_64-w64-mingw32-g++ || echo /opt/homebrew/bin/x86_64-w64-mingw32-g++)}
MANIFEST_TOOL=${MANIFEST_TOOL:-}
for c in "$SRC_REPO/tools/release-manifest.py" "$REPO/tools/release-manifest.py"; do
    [ -z "$MANIFEST_TOOL" ] && [ -f "$c" ] && MANIFEST_TOOL=$c
done
OLD=0.6.9 NEW_TAG=v0.7.0

die() { echo "update-test: $*" >&2; exit 2; }
[ -f "$CPP" ] || die "no launcher source at $CPP"
[ -x "$CXX" ] || die "x86_64-w64-mingw32-g++ not found (set CXX)"
[ -n "$MANIFEST_TOOL" ] && [ -f "$MANIFEST_TOOL" ] || die "tools/release-manifest.py not found (set MANIFEST_TOOL)"
[ -n "$WINE_ROOT" ] || die "set WINE_ROOT to a Wine build (inst/bin/wine + prefix/)"
[ -x "$WINE_ROOT/inst/bin/wine" ] && [ -d "$WINE_ROOT/prefix/drive_c" ] || die "no built Wine + prefix under $WINE_ROOT"
command -v python3 >/dev/null || die "needs python3"
command -v perl >/dev/null || die "needs perl"
case "$UT" in /tmp/*|/private/tmp/*) ;; *) die "UT must be under /tmp (it is deleted first)";; esac

# --- Wine, in a clone of the built prefix ---------------------------------------------------
export WINEPREFIX="$UT/prefix" WINEDEBUG=${WINEDEBUG:--all} DYLD_FALLBACK_LIBRARY_PATH="$WINE_ROOT/x86/lib"
WINE="$WINE_ROOT/inst/bin/wine"
winpath() { printf 'Z:%s' "$(printf '%s' "$1" | tr / '\\')"; }
# wine with a wall-clock limit (macOS has no timeout(1)); stdin closed so "Press Enter" never waits.
wrun() { local secs=$1; shift; perl -e 'alarm shift; exec @ARGV' "$secs" "$WINE" "$@" </dev/null; }
cleanup() { "$WINE_ROOT/inst/bin/wineserver" -k >/dev/null 2>&1; }
trap cleanup EXIT

"$WINE_ROOT/inst/bin/wineserver" -k >/dev/null 2>&1
rm -rf "$UT"; mkdir -p "$UT/build/launcher" "$UT/build/src" "$UT/bin" "$UT/fixtures"
# APFS clone: instant, and nothing written to the original prefix.
cp -cR "$WINE_ROOT/prefix" "$UT/prefix" 2>/dev/null || cp -R "$WINE_ROOT/prefix" "$UT/prefix" || die "can't copy the Wine prefix"

# --- Builds ---------------------------------------------------------------------------------
LIBS="-lbcrypt -lwinhttp -lshlwapi -lole32 -lcomctl32 -luuid -lgdi32 -luser32 -lshell32 -ladvapi32"
build_launcher() {  # <version> <out.exe>
    cp "$CPP" "$UT/build/launcher/launcher.cpp"
    cp "$SRC_REPO"/src/*.h "$UT/build/src/" 2>/dev/null
    perl -0pi -e "s/#define\\s+SCO_VERSION\\s+\"[^\"]*\"/#define SCO_VERSION  \"$1\"/" "$UT/build/src/version.h"
    grep -q "SCO_VERSION  \"$1\"" "$UT/build/src/version.h" || die "couldn't set SCO_VERSION $1"
    # shellcheck disable=SC2086
    "$CXX" -std=c++20 -O2 -w -municode -static -DUNICODE -D_UNICODE -DSCO_UPDATE_TEST \
        -o "$2" "$UT/build/launcher/launcher.cpp" $LIBS >"$UT/build/cc-$1.txt" 2>&1 \
        || { cat "$UT/build/cc-$1.txt" | head -20 >&2; die "launcher $1 didn't build"; }
}
build_tool() { "$CXX" -std=c++20 -O2 -w -municode -static -o "$UT/bin/$1.exe" "$HERE/$1.cpp" || die "$1 didn't build"; }
build_launcher "$OLD" "$UT/bin/old.exe"
build_launcher "${NEW_TAG#v}" "$UT/bin/new.exe"
build_tool hold-file; build_tool fail-stub; build_tool tar-shim
cp "$UT/bin/tar-shim.exe" "$WINEPREFIX/drive_c/windows/system32/tar.exe"
HOOK=yes; grep -q -- "--apply-zip" "$CPP" || HOOK=no

# --- Fixtures -------------------------------------------------------------------------------
python3 "$HERE/make_fixtures.py" build --manifest-tool "$MANIFEST_TOOL" --new-exe "$UT/bin/new.exe" \
    --stub-exe "$UT/bin/fail-stub.exe" --ini "$SRC_REPO/launcher/sc-offline.ini" --out "$UT/fixtures" \
    > "$UT/fixtures.tsv" || die "make_fixtures.py build failed"

# The 0.6.9 install every scenario starts from. The ini keeps a player edit (start_ship) and turns
# the network update check off, so a plain start never reaches GitHub.
make_install() {  # <dir>
    mkdir -p "$1/data" "$1/docs"
    cp "$UT/bin/old.exe" "$1/sc-offline.exe"
    printf 'dinput8.dll stand-in for v%s\n' "$OLD" > "$1/dinput8.dll"
    printf '# sc-offline v%s\n' "$OLD" > "$1/README.md"
    printf '# items for v%s\n' "$OLD" > "$1/data/items.txt"
    printf 'only in v%s\n' "$OLD" > "$1/data/old-only.txt"
    printf 'launcher docs for v%s\n' "$OLD" > "$1/docs/launcher.md"
    printf '123456789\n' > "$1/data/wallet.txt"
    perl -pe 's/^start_ship\s*=.*?(\r?)$/start_ship = TEST_KEEP_ME$1/; s/^check_updates\s*=.*?(\r?)$/check_updates = off$1/' \
        "$SRC_REPO/launcher/sc-offline.ini" > "$1/sc-offline.ini"
}

# --- Scenarios ------------------------------------------------------------------------------
pass=0; fail=0; failed=""
result() {  # <name> <reason or empty>
    if [ -z "$2" ]; then echo "PASS $1"; pass=$((pass + 1));
    else echo "FAIL $1: $2"; fail=$((fail + 1)); failed="$failed $1"; fi
}
same()    { python3 "$HERE/make_fixtures.py" same "$1" "$2"; }
leftovers() { find "$1" \( -name '*.update-new' -o -name '*.update-failed' -o -name '*.update-old' \) | sed "s#^$1/##" | tr '\n' ' '; }

run_one() {  # <name> <zip> <tag> <sha256>
    local name=$1 zip=$2 tag=$3 sha=$4 dir="$UT/run/$1/install" snap="$UT/run/$1/before.json" out="$UT/run/$1/out.txt"
    local pkg="${zip%.zip}"   # make_fixtures.py keeps the unpacked release folder next to its zip
    make_install "$dir"
    python3 "$HERE/make_fixtures.py" snapshot "$dir" "$snap"
    local exe; exe=$(winpath "$dir/sc-offline.exe")
    local hold_pid="" ready="$UT/run/$name/held" hold_ms=$LOCK_MS
    [ "$name" = kill ] && hold_ms=60000   # longer than every lock retry: the launcher is killed while it waits
    if [ "$name" = locked ] || [ "$name" = kill ]; then
        wrun 90 "$(winpath "$UT/bin/hold-file.exe")" "$(winpath "$dir/dinput8.dll")" "$hold_ms" "$(winpath "$ready")" &
        hold_pid=$!
        for _ in $(seq 1 100); do [ -f "$ready" ] && break; sleep 0.1; done
        [ -f "$ready" ] || { wait "$hold_pid"; result "$name" "hold-file.exe never took the lock"; return; }
    fi
    local env_fail=""; [ "$name" = mid-swap ] && env_fail="SCO_TEST_FAIL_AFTER=2"
    local t0=$SECONDS
    env $env_fail perl -e 'alarm shift; exec @ARGV' 120 "$WINE" "$exe" --apply-zip "$(winpath "$zip")" "$tag" "$sha" \
        </dev/null >"$out" 2>&1 &
    local run_pid=$!
    if [ "$name" = kill ]; then
        # Kill everything in the prefix the moment the swap is stuck on dinput8.dll: files before it
        # are already swapped and journaled, which is what a crash or power cut mid-swap leaves.
        local killed=no
        for _ in $(seq 1 300); do
            grep -aq "dinput8.dll is in use" "$dir/data/launcher.log" 2>/dev/null && { killed=yes; break; }
            kill -0 "$run_pid" 2>/dev/null || break; sleep 0.1
        done
        "$WINE_ROOT/inst/bin/wineserver" -k >/dev/null 2>&1
        wait "$run_pid"; wait "$hold_pid"
        if [ "$HOOK" = no ] && grep -q "unknown argument '--apply-zip'" "$out"; then
            result "$name" "launcher has no --apply-zip (SCO_UPDATE_TEST hook not in $(basename "$CPP"))"; return
        fi
        [ $killed = yes ] || { result "$name" "the swap never waited on the locked dinput8.dll, so nothing was killed mid-swap"; return; }
        local mid; mid=$(same "$dir" "$snap") && { result "$name" "the kill left the install untouched, so the swap hadn't started"; return; }
        echo "after kill: $mid" > "$UT/run/$name/after-kill.txt"
        wrun 60 "$exe" status >>"$out" 2>&1   # the next start settles the journal (SettlePreviousUpdate)
        local why="" d; d=$(same "$dir" "$snap") || why="not rolled back on the next start: $d"
        local left; left=$(leftovers "$dir")
        [ -z "$why" ] && [ -n "$left" ] && why="left behind: $left"
        [ -n "$why" ] && cp "$out" "$UT/run/$name/FAILED-output.txt"
        result "$name" "$why"; return
    fi
    wait "$run_pid"
    local rc=$? secs=$((SECONDS - t0))
    if [ -n "$hold_pid" ]; then wait "$hold_pid"; local hrc=$?; [ $hrc -eq 0 ] || { result "$name" "hold-file.exe exit $hrc"; return; }; fi
    if [ "$HOOK" = no ] && grep -q "unknown argument '--apply-zip'" "$out"; then
        result "$name" "launcher has no --apply-zip (SCO_UPDATE_TEST hook not in $(basename "$CPP"))"; return
    fi
    [ $rc -eq 142 ] && { result "$name" "timed out after 120 s"; return; }
    local why=""
    case "$name" in
        good|locked)
            [ $rc -eq 0 ] || why="exit $rc, expected 0 ($(grep -m1 '\[!\]' "$out" | tr -d '\r'))"
            [ -z "$why" ] && why=$(python3 "$HERE/make_fixtures.py" updated "$dir" "$pkg" "$snap")
            local left; left=$(leftovers "$dir" | tr ' ' '\n' | grep -v '\.update-old$' | tr '\n' ' ')
            [ -z "$why" ] && [ -n "${left// /}" ] && why="left behind: $left"
            [ -z "$why" ] && [ "$name" = locked ] && [ $((secs * 1000)) -lt $((LOCK_MS / 2)) ] && \
                why="finished in ${secs}s, before the ${LOCK_MS} ms lock could have mattered"
            ;;
        mid-swap)
            [ $rc -eq 1 ] || why="exit $rc, expected 1"
            if [ -z "$why" ] && ! same "$dir" "$snap" >/dev/null; then
                # Something was left half-done: the next start (SettlePreviousUpdate) must settle it.
                # `status` starts the launcher without touching the network (check_updates = off).
                wrun 60 "$exe" status >/dev/null 2>&1
                local d; d=$(same "$dir" "$snap") || why="not rolled back, even after the next start: $d"
            fi
            local left; left=$(leftovers "$dir")
            [ -z "$why" ] && [ -n "$left" ] && why="left behind: $left"
            ;;
        *)  # every other scenario must be refused and leave the install exactly as it was
            [ $rc -eq 1 ] || why="exit $rc, expected 1"
            if [ -z "$why" ]; then local d; d=$(same "$dir" "$snap") || why="install changed: $d"; fi
            local left; left=$(leftovers "$dir")
            [ -z "$why" ] && [ -n "$left" ] && why="left behind: $left"
            ;;
    esac
    case "$name" in
        unlisted) [ -z "$why" ] && [ -e "$dir/data/unlisted.txt" ] && why="data/unlisted.txt was copied" ;;
        dotdot|abspath) [ -z "$why" ] && for p in "$UT/run/$name/evil.txt" "$UT/evil.txt" /tmp/sco-ut/evil.txt; do
                    [ -e "$p" ] && why="wrote $p"; done ;;
        ads) [ -z "$why" ] && [ -e "$dir/dinput8.dll:evil" ] && why="wrote dinput8.dll:evil" ;;
    esac
    [ -n "$why" ] && cp "$out" "$UT/run/$name/FAILED-output.txt"
    result "$name" "$why"
}

echo "update-test: $(basename "$CPP") from $SRC_REPO ($( [ "$HOOK" = yes ] && echo 'has --apply-zip' || echo 'NO --apply-zip hook' )), manifest tool $MANIFEST_TOOL"
while IFS=$'\t' read -r name zip tag sha; do
    if [ -n "${ONLY:-}" ] && ! printf ' %s ' "$ONLY" | grep -q " $name "; then continue; fi
    rm -f "$UT/evil.txt"
    run_one "$name" "$zip" "$tag" "$sha"
done < "$UT/fixtures.tsv"

echo "update-test: $pass passed, $fail failed${failed:+ ($failed )}; launcher output per scenario in $UT/run/<name>/out.txt"
[ $fail -eq 0 ]
