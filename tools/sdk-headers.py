#!/usr/bin/env python3
"""SDK-headers gate: src/builtins/ and plugins/ are consumers of the sco SDK (game-services plan, PR 8).

    python3 tools/sdk-headers.py            # from the repo root

Every file under src/builtins/ and plugins/ (the plugins built from this repo: plugins/creative) may include only
  * the headers the SDK zip ships (sco_api.h, sco_*.h, sc_*.h, scosdk/**, ImGui's public headers),
  * files inside its own tree (src/builtins/ or plugins/<id>/),
  * toolchain headers (the standard library, the Windows SDK).
Anything else - ../*.h, sco/*.h (sco-core's internal kernel headers), version.h - fails with file:line.

"Shipped by the SDK zip" is read from external/sco-core/sdk/package.py (its CONTENT table), so
the gate follows the zip and never a second list kept here.

The one exception file is tools/sdk-headers-allow.txt: `header | files | reason`, one line per
header, a reason on every line. The header is named by where it lives (`src/menu.h`, or
`sco/plugins.h` for sco-core's include folder), so ../menu.h and ../../menu.h are one entry; files
is a comma-separated list of path globs relative to src/builtins/ (plugins/ has no entries: it must stay clean). A header or file pattern that no longer matches a
violation fails too, so the list only ever shrinks. Exit 0 clean, 1 violations or stale entries, 2 the check could not run (which
must never read as clean).
"""
import fnmatch
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent).resolve()
BUILTINS = ROOT / "src" / "builtins"
PLUGINS = ROOT / "plugins"   # plugins built from this repo; clean, no allow-list entries
SCO = ROOT / "external" / "sco-core"
PACKAGE = SCO / "sdk" / "package.py"
ALLOW = ROOT / "tools" / "sdk-headers-allow.txt"
# Where an #include is looked up besides the including file's own folder: what check.sh and
# CMakeLists.txt put on the include path.
SEARCH = [ROOT / "src", SCO / "include", SCO / "plugins" / "lua", ROOT / "src" / "third_party" / "imgui"]
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp", ".inl"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')


def die(msg):
    print(f"sdk-headers: {msg}")
    sys.exit(2)


def sdk_headers():
    """(top-level headers, header folders, ImGui public headers) the SDK zip ships."""
    if not PACKAGE.is_file():
        die(f"{PACKAGE.relative_to(ROOT)} is missing; run: git submodule update --init")
    text = PACKAGE.read_text(encoding="utf-8")
    top, dirs = set(), set()
    for src in re.findall(r'\(\s*"include/([^"]+)"\s*,', text):
        (top if src.endswith(".h") and "/" not in src else dirs).add(src)
    imgui_dir = SCO / "sdk" / "third_party" / "imgui"
    imgui = {p.name for p in imgui_dir.glob("*.h")} if '"sdk/third_party/imgui"' in text else set()
    if not top or not dirs or not imgui:
        die("could not read the SDK header list from sdk/package.py (CONTENT changed shape?)")
    return top, dirs, imgui


def resolve(including, name, quoted):
    cands = ([including.parent] if quoted else []) + SEARCH
    for base in cands:
        p = (base / name).resolve()
        if p.is_file():
            return p
    return None


def under(p, folder):
    try:
        p.relative_to(folder.resolve())
        return True
    except ValueError:
        return False


def header_key(hit, name):
    """How the allow-list names a header: src/<path> for the product's, the written name for sco-core's."""
    if hit is not None and under(hit, ROOT / "src"):
        return hit.relative_to(ROOT).as_posix()
    return name


def load_allow():
    entries = []
    if not ALLOW.is_file():
        return entries
    for n, raw in enumerate(ALLOW.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [s.strip() for s in line.split("|", 2)]
        if len(parts) != 3 or not all(parts):
            die(f"{ALLOW.name}:{n}: want `header | files | reason`, with a reason")
        globs = [g.strip() for g in parts[1].split(",") if g.strip()]
        entries.append({"inc": parts[0], "globs": {g: 0 for g in globs}, "reason": parts[2], "line": n})
    return entries


def main():
    if not BUILTINS.is_dir():
        die(f"{BUILTINS} not found")
    top, dirs, imgui = sdk_headers()
    allow = load_allow()
    files = sorted(p for p in BUILTINS.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES)
    if not files:
        die("no sources under src/builtins/")
    plugin_files = sorted(p for p in PLUGINS.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES) if PLUGINS.is_dir() else []
    files += plugin_files
    checked = allowed = 0
    bad = []
    for f in files:
        rel = f.relative_to(ROOT).as_posix()
        own = PLUGINS if f in plugin_files else BUILTINS
        brel = f.relative_to(own).as_posix()
        for lineno, line in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            m = INCLUDE.match(line)
            if not m:
                continue
            quoted, name = m.group(1) == '"', m.group(2)
            checked += 1
            hit = resolve(f, name, quoted)
            if hit is None:
                if not quoted:
                    continue  # the standard library or the Windows SDK
                verdict = "does not resolve"
            elif under(hit, own):
                continue
            elif under(hit, SCO / "include") and (name in top or name.split("/")[0] in dirs):
                continue
            elif hit.name in imgui and under(hit, ROOT / "src" / "third_party" / "imgui"):
                continue
            elif under(hit, SCO / "include"):
                verdict = "is sco-core's internal header, not in the SDK zip"
            else:
                verdict = "is not in the SDK zip or its own folder"
            key = header_key(hit, name)
            glob = next((g for e in allow if e["inc"] == key for g in e["globs"] if fnmatch.fnmatch(brel, g)), None)
            if glob is not None:
                next(e for e in allow if e["inc"] == key)["globs"][glob] += 1
                allowed += 1
            else:
                bad.append(f'{rel}:{lineno}: #include "{name}" {verdict} (allow-list key: {key})')
    stale = [(e, g) for e in allow for g, used in e["globs"].items() if not used]
    for b in bad:
        print(b)
    for e, g in stale:
        print(f'{ALLOW.relative_to(ROOT).as_posix()}:{e["line"]}: `{e["inc"]}` is never included by `{g}`; delete that pattern')
    if bad or stale:
        print(f"sdk-headers: FAIL - {len(bad)} include(s) outside the SDK, {len(stale)} stale allow-list entr(ies)")
        sys.exit(1)
    print(f"sdk-headers: ok - {len(files)} files, {checked} includes, {allowed} allow-listed "
          f"({len(allow)} entries in {ALLOW.relative_to(ROOT).as_posix()})")


main()
