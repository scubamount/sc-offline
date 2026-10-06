#!/usr/bin/env bash
# Pre-push check for machines without Visual Studio (macOS / Linux).
#
#   tools/check.sh            # from the repo root
#
# 1. Parses every src/*.cpp and launcher/*.cpp with clang against mingw-w64's Windows headers (-fsyntax-only).
#    MSVC accepts a few things clang rejects; those are listed in tools/check-baseline.txt and
#    only NEW diagnostics fail the check.
# 2. MSVC error C2712 screen: a function containing __try may not own an object that needs
#    unwinding (a std::string temporary is enough). clang does not enforce this, MSVC does.
#
# This is not the build. It cannot link, and it uses a different standard library; CI's MSVC
# build stays the real gate. It exists so a parse error or a C2712 costs seconds, not a CI round.
#
# Needs: clang and mingw-w64 headers. macOS: `brew install llvm mingw-w64`.
# Debian/Ubuntu: `apt install clang g++-mingw-w64-x86-64-posix` (what CI uses).
set -u
ROOT=$(cd "${1:-$(dirname "$0")/..}" && pwd)
BASELINE=${BASELINE:-$ROOT/tools/check-baseline.txt}
# mingw-w64 headers: Homebrew keeps them under the toolchain prefix, Debian/Ubuntu under /usr.
MWINC=${MINGW_INCLUDE:-$(ls -d /opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include \
                              /usr/local/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include \
                              /usr/x86_64-w64-mingw32/include 2>/dev/null | head -1)}
CXXINC=${MINGW_CXX_INCLUDE:-$(ls -d "$MWINC"/c++/[0-9]* /usr/lib/gcc/x86_64-w64-mingw32/*-posix/include/c++ \
                                    /usr/lib/gcc/x86_64-w64-mingw32/*/include/c++ 2>/dev/null | sort -V | tail -1)}
CLANG=${CLANG:-$(command -v /opt/homebrew/opt/llvm/bin/clang++ || command -v clang++)}
# Fail closed: a missing tool must never read as "0 new diagnostics".
[ -n "${CLANG:-}" ] && [ -x "$CLANG" ] || { echo "check: clang++ not found (set CLANG)"; exit 2; }
command -v python3 >/dev/null || { echo "check: python3 not found"; exit 2; }
[ -d "${MWINC:-}" ] && [ -f "${CXXINC:-}/string" ] || { echo "check: mingw-w64 headers not found (set MINGW_INCLUDE / MINGW_CXX_INCLUDE)"; exit 2; }
n=0; broken=0
TMP=$(mktemp)
shopt -s nullglob
for f in "$ROOT"/src/*.cpp "$ROOT"/launcher/*.cpp; do
  n=$((n+1))
  out=$("$CLANG" -fsyntax-only -std=c++20 --target=x86_64-w64-windows-gnu \
     -fms-extensions \
     -D_WIN32 -D_WIN64 -DUNICODE -D_UNICODE -DNDEBUG -D_WINDLL \
     -nostdinc++ -isystem "$CXXINC" -isystem "$CXXINC/x86_64-w64-mingw32" -isystem "$CXXINC/backward" \
     -isystem "$MWINC" -I "$ROOT/src" -I "$ROOT/src/third_party/imgui" \
     -Wno-everything "$f" 2>&1)
  rc=$?
  errs=$(printf '%s\n' "$out" | grep -E "error:")
  # clang exits 1 on diagnostics; anything else, or a failure with no "error:" line, is a broken run.
  if [ $rc -gt 1 ] || { [ $rc -ne 0 ] && [ -z "$errs" ]; }; then
    echo "check: clang failed on $(basename "$f") (exit $rc):"; printf '%s\n' "$out" | head -5; broken=1
  fi
  [ -n "$errs" ] && printf '%s\n' "$errs" | sed -E "s#^.*/(src|launcher)/##; s#:[0-9]+:[0-9]+:#:#" >> "$TMP"
done
[ $n -gt 0 ] || { echo "check: no src/*.cpp or launcher/*.cpp under $ROOT"; rm -f "$TMP"; exit 2; }
[ $broken -eq 0 ] || { rm -f "$TMP"; exit 2; }

# MSVC C2712 screen (clang does not enforce it): a function containing __try may not own
# an object that needs unwinding. Flags __try functions that construct std:: objects or call
# a function returning one by value.
python3 - "$ROOT/src" "$ROOT/launcher" <<'PY' >> "$TMP"
import os, re, sys
src = {f: open(os.path.join(S, f), encoding="utf-8", errors="replace").read()
       for S in sys.argv[1:] for f in os.listdir(S) if f.endswith((".cpp", ".h"))}
strip = lambda t: re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', '""', t, flags=re.S)
ret = set()
for t in src.values():
    for m in re.finditer(r'^\s*(?:static\s+)?(?:const\s+)?std::(?:string|wstring|vector<[^>]+>|unique_ptr<[^>]+>)\s+(\w+)\s*\(', strip(t), re.M):
        ret.add(m.group(1))
for f, raw in src.items():
    if not f.endswith(".cpp"): continue
    t = strip(raw)
    for m in re.finditer(r'^[A-Za-z_][\w\s\*&:<>,]*?\b(\w+)\s*\([^;{]*\)\s*(?:noexcept\s*)?\{', t, re.M):
        i = t.index("{", m.start()); d = 0
        for j in range(i, len(t)):
            d += t[j] == "{"; d -= t[j] == "}"
            if d == 0: break
        body = t[i:j]
        if "__try" not in body: continue
        bad = sorted({n for n in ret if re.search(r'\b' + n + r'\s*\(', body)} |
                     set(re.findall(r'\bstd::(?:string|wstring|vector|unique_ptr)\b', body)))
        if bad: print(f"{f}: C2712 risk: {m.group(1)} owns __try and {', '.join(bad)}")
PY
[ $? -eq 0 ] || { echo "check: C2712 screen failed to run"; rm -f "$TMP"; exit 2; }
sort -u "$TMP" > "$TMP.now"
[ -f "$BASELINE" ] || { cat "$TMP.now"; echo "check: baseline $BASELINE missing"; rm -f "$TMP" "$TMP.now"; exit 2; }
new=$(comm -13 "$BASELINE" "$TMP.now")
gone=$(comm -23 "$BASELINE" "$TMP.now")
echo "check: $n translation units, $(wc -l < "$TMP.now" | tr -d ' ') diagnostics (baseline $(wc -l < "$BASELINE" | tr -d ' ')), $(printf '%s' "$new" | grep -c .) new, $(printf '%s' "$gone" | grep -c .) gone"
rm -f "$TMP" "$TMP.now"
[ -z "$new" ] || { echo "$new"; exit 1; }
# A baseline line that no longer appears means the code improved (delete the line) or the
# compiler stopped seeing the file. Either way the baseline is stale, so fail and say which.
[ -z "$gone" ] || { echo "baseline lines no longer produced (remove them from $BASELINE):"; echo "$gone"; exit 1; }
