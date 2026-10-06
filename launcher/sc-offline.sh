#!/usr/bin/env bash
# Linux (EXPERIMENTAL, not yet run by anyone): starts sc-offline.exe inside your Star Citizen
# Wine prefix. sc-offline.exe then finds the game, adds the mod, and removes it on exit, exactly
# as on Windows.
#
#   ./sc-offline.sh [play|install|uninstall|status|help] [--game <Windows path>] [--dry-run]
#                   [--skip-eac-check] [--prefix <dir>] [--wine <path to wine>]
#
# --prefix and --wine are this script's; everything else goes to sc-offline.exe. With --dry-run
# the script prints what it found and the wine command, and starts nothing.
#
# Prefix, first hit wins: --prefix / $WINEPREFIX, the LUG Helper's winedir.conf, a Lutris game
# whose name mentions Star Citizen, a Steam/Proton prefix that has Roberts Space Industries in it,
# ~/Games/star-citizen.
# Wine, first hit wins: --wine / $WINE, wine_path= in the prefix's sc-launch.sh (LUG Helper),
# the Lutris runner, the newest Proton's wine for a Proton prefix, wine on PATH.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
eac_host=modules-cdn.eac-prod.on.epicgames.com

prefix=${WINEPREFIX:-}
prefix_from=${WINEPREFIX:+\$WINEPREFIX}
wine=${WINE:-}
wine_from=${WINE:+\$WINE}
dry=0
args=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) [ $# -ge 2 ] || { echo "[!] --prefix needs a folder"; exit 1; }
                  prefix=$2; prefix_from=--prefix; shift 2 ;;
        --wine)   [ $# -ge 2 ] || { echo "[!] --wine needs a path"; exit 1; }
                  wine=$2; wine_from=--wine; shift 2 ;;
        --dry-run) dry=1; args+=("$1"); shift ;;
        *) args+=("$1"); shift ;;
    esac
done

# Reads `key: value` from a Lutris game yml (first match), quotes stripped.
yml_value() {
    sed -n "s/^[[:space:]]*$1:[[:space:]]*//p" "$2" | head -n 1 | tr -d "\"'"
}

lutris_yml() {
    local dir f
    for dir in "${XDG_CONFIG_HOME:-$HOME/.config}/lutris/games" "${XDG_DATA_HOME:-$HOME/.local/share}/lutris/games"; do
        [ -d "$dir" ] || continue
        for f in "$dir"/*.yml; do
            [ -f "$f" ] || continue
            if grep -qiE '(game_slug|name):.*star[ -]?citizen' "$f" || [[ $(basename "$f") == *star-citizen* ]]; then
                echo "$f"; return 0
            fi
        done
    done
    return 1
}

steam_roots() {
    local r
    for r in "$HOME/.steam/steam" "${XDG_DATA_HOME:-$HOME/.local/share}/Steam" \
             "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        [ -d "$r/steamapps" ] && echo "$r"
    done
    return 0
}

if [ -z "$prefix" ]; then
    conf="${XDG_CONFIG_HOME:-$HOME/.config}/starcitizen-lug/winedir.conf"
    if [ -f "$conf" ] && [ -n "$(head -n 1 "$conf")" ]; then
        prefix=$(head -n 1 "$conf"); prefix_from="LUG Helper ($conf)"
    elif yml=$(lutris_yml) && p=$(yml_value prefix "$yml") && [ -n "$p" ]; then
        prefix=${p/#\~/$HOME}; prefix_from="Lutris ($yml)"
    else
        while read -r root; do
            for pfx in "$root"/steamapps/compatdata/*/pfx; do
                if [ -d "$pfx/drive_c/Program Files/Roberts Space Industries" ]; then
                    prefix=$pfx; prefix_from="Steam/Proton ($root)"; break 2
                fi
            done
        done < <(steam_roots)
    fi
    if [ -z "$prefix" ]; then prefix="$HOME/Games/star-citizen"; prefix_from=default; fi
fi
if [ ! -d "$prefix/drive_c" ]; then
    echo "[!] No Wine prefix at $prefix (from $prefix_from)"
    echo "    Run again with --prefix /path/to/your/star-citizen/prefix"
    exit 1
fi

if [ -z "$wine" ] && [ -f "$prefix/sc-launch.sh" ]; then
    # Usually a literal runner path; when it's a $(...) expression the test below just fails.
    wine_dir=$(sed -n 's/^[[:space:]]*\(export[[:space:]]\{1,\}\)\{0,1\}wine_path=//p' "$prefix/sc-launch.sh" \
               | tail -n 1 | tr -d "\"'")
    if [ -x "$wine_dir/wine" ]; then wine="$wine_dir/wine"; wine_from="sc-launch.sh"; fi
fi
if [ -z "$wine" ] && yml=$(lutris_yml); then
    version=$(yml_value version "$yml")
    candidate="${XDG_DATA_HOME:-$HOME/.local/share}/lutris/runners/wine/$version/bin/wine"
    if [ -n "$version" ] && [ -x "$candidate" ]; then wine=$candidate; wine_from="Lutris runner $version"; fi
fi
if [ -z "$wine" ] && [[ $prefix == */steamapps/compatdata/*/pfx ]]; then
    steamapps=${prefix%/compatdata/*}
    candidate=$(find "$steamapps/common" -maxdepth 4 -path '*/Proton*/files/bin/wine' 2>/dev/null | sort -V | tail -n 1)
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then wine=$candidate; wine_from="Proton"; fi
fi
if [ -z "$wine" ]; then
    wine=$(command -v wine || true)
    [ -n "$wine" ] && wine_from=PATH
fi
if [ -z "$wine" ]; then
    echo "[!] wine not found. Run again with --wine /path/to/wine"
    exit 1
fi

if [ ! -f "$here/sc-offline.exe" ]; then
    echo "[!] sc-offline.exe is missing next to this script ($here)."
    echo "    Extract the whole release zip into one folder and run sc-offline.sh from there."
    exit 1
fi

export WINEPREFIX=$prefix
# Wine prefers its own dinput8 over a DLL next to the game; the mod needs the native one first.
export WINEDLLOVERRIDES="dinput8=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export WINEDEBUG="${WINEDEBUG:--all}"

echo "Prefix: $prefix  (from $prefix_from)"
echo "Wine:   $wine  (from $wine_from)"
if grep -qE "^[^#]*[[:space:]]$eac_host([[:space:]]|$)" /etc/hosts 2>/dev/null; then
    echo "Hosts:  EAC download server blocked"
else
    echo "[i] /etc/hosts doesn't block the Easy Anti-Cheat download server. Add it with:"
    echo "      echo '127.0.0.1 $eac_host' | sudo tee -a /etc/hosts"
fi

if [ "$dry" = 1 ]; then
    echo "[dry-run] WINEPREFIX=$WINEPREFIX WINEDLLOVERRIDES=$WINEDLLOVERRIDES"
    printf '[dry-run] would run:'; printf ' %q' "$wine" "$here/sc-offline.exe" ${args[@]+"${args[@]}"}; echo
    exit 0
fi
exec "$wine" "$here/sc-offline.exe" ${args[@]+"${args[@]}"}
