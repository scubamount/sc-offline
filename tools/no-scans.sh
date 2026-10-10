#!/usr/bin/env bash
# Zero raw pattern scans under src/ (game-services plan: sc-offline becomes a pure consumer of the sco SDK).
#
#   tools/no-scans.sh            # from the repo root
#
# Every game address sc-offline uses comes from sco-core's signature rows (sco/game/*.h), resolved
# from a unique pattern in the loaded image. Nothing under src/ may find or validate an address by
# itself, because a raw scan is one place that breaks when the game moves code, and sco-core is the
# one place that is checked against a game build.
#
# Banned in src/, third_party excluded (vendored, not ours): FindPattern, FindUniquePattern,
# FindCString, FindRipLea, FunctionStart, BytesMatch and memchr - the scanners and the pattern
# comparator in sco/scan.h.
#
# There is no allow-list. If a line is a comment or a vendored file it stays out of the scan; if it
# is a real use, it is a hit and this exits 1. Exit 2 means the scan itself could not run, which
# must never read as "clean".
set -u
ROOT=$(cd "${1:-$(dirname "$0")/..}" && pwd)
command -v grep >/dev/null || { echo "no-scans: grep not found"; exit 2; }

# src/ minus vendored code. --include keeps us to C++ the check.sh screen also parses.
FILES=$(find "$ROOT/src" -name .git -prune -o -type f \( -name '*.cpp' -o -name '*.h' \) \
  -not -path '*/third_party/*' -print 2>/dev/null | sort)
[ -n "$FILES" ] || { echo "no-scans: no sources found under $ROOT/src"; exit 2; }

HITS=$(grep -nE '\b(FindPattern|FindUniquePattern|FindCString|FindRipLea|FunctionStart|BytesMatch|memchr)[[:space:]]*\(' $FILES || true)
if [ -n "$HITS" ]; then
  echo "no-scans: raw game scans left under src/ (these addresses must be sco-core rows):"
  echo "$HITS"
  exit 1
fi
echo "no-scans: no raw scans under src/ ($(printf '%s\n' "$FILES" | wc -l | tr -d ' ') files, third_party excluded)"
