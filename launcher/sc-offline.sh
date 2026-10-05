#!/usr/bin/env bash
# Linux (EXPERIMENTAL, not yet run by anyone): starts sc-offline.exe inside your Star Citizen
# Wine prefix. sc-offline.exe then finds the game, adds the mod, and removes it on exit, exactly
# as on Windows.
#
#   ./sc-offline.sh [--game <Star Citizen folder, as a Windows path>]
#
# Finds things the way the LUG Helper (github.com/starcitizen-lug/lug-helper) sets them up:
#   prefix: $WINEPREFIX, else the LUG Helper's ~/.config/starcitizen-lug/winedir.conf,
#           else ~/Games/star-citizen
#   wine:   $WINE, else the runner named by wine_path= in the prefix's sc-launch.sh,
#           else the first wine on PATH
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [ -z "${WINEPREFIX:-}" ]; then
    conf="${XDG_CONFIG_HOME:-$HOME/.config}/starcitizen-lug/winedir.conf"
    if [ -f "$conf" ]; then WINEPREFIX=$(<"$conf"); else WINEPREFIX="$HOME/Games/star-citizen"; fi
fi
if [ ! -d "$WINEPREFIX/drive_c" ]; then
    echo "[!] No Wine prefix at $WINEPREFIX"
    echo "    Run again with WINEPREFIX=/path/to/your/star-citizen/prefix"
    exit 1
fi

wine=${WINE:-}
if [ -z "$wine" ] && [ -f "$WINEPREFIX/sc-launch.sh" ]; then
    # Usually a literal runner path; when it's a $(...) expression the test below just fails.
    wine_dir=$(sed -n 's/^\(export \)\{0,1\}wine_path=//p' "$WINEPREFIX/sc-launch.sh" | tail -n 1 | tr -d '"')
    if [ -x "$wine_dir/wine" ]; then wine="$wine_dir/wine"; fi
fi
if [ -z "$wine" ]; then wine=$(command -v wine || true); fi
if [ -z "$wine" ]; then
    echo "[!] wine not found. Run again with WINE=/path/to/wine"
    exit 1
fi

export WINEPREFIX
# Wine prefers its own dinput8 over a DLL next to the game; the mod needs the native one first.
export WINEDLLOVERRIDES="dinput8=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export WINEDEBUG="${WINEDEBUG:--all}"

echo "Prefix: $WINEPREFIX"
echo "Wine:   $wine"
exec "$wine" "$here/sc-offline.exe" "$@"
