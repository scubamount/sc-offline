#!/usr/bin/env python3
"""manifest.json for a release folder: what the launcher's self-update pins (issue #31, S1).

    tools/release-manifest.py write <folder> --tag v0.7.0 [--commit <sha>] [--version-h src/version.h]
    tools/release-manifest.py check <folder>

`write` lists every file under <folder> with its SHA-256 and size and writes <folder>/manifest.json.
It refuses to run when the tag doesn't match SCO_VERSION in src/version.h, because the launcher
refuses a manifest whose version differs from the release tag, and a mismatch would strand every
update. `check` re-hashes <folder> against its manifest.json: the same check the launcher makes,
for checking a release by hand. Both exit 0 on success and 1 with a reason otherwise.

The format, one file object per line so a small parser can read it:

    {
      "format": 1,
      "version": "0.7.0",
      "tag": "v0.7.0",
      "commit": "<40 hex>",
      "files": [
        {"path": "data/items.txt", "sha256": "<64 lowercase hex>", "size": 1234},
        ...
      ]
    }

Paths are relative to the folder that holds manifest.json, use `/`, are sorted, and never contain
`..`, `:`, `\\`, a leading `/` or an empty part. manifest.json does not list itself.
"""
import argparse
import hashlib
import json
import os
import re
import sys

NAME = "manifest.json"
FORMAT = 1
TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)$")


def bad_path(rel):
    """Why rel can't be in a manifest, or None. The launcher applies the same rules (S3)."""
    if not rel or rel.startswith("/") or "\\" in rel or ":" in rel:
        return "absolute path, backslash or ':'"
    if any(p in ("", ".", "..") for p in rel.split("/")):
        return "empty, '.' or '..' part"
    if any(ord(c) < 0x20 or c == '"' for c in rel):
        return "control character or '\"'"
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def walk(folder):
    """Every regular file under folder except manifest.json, as sorted relative '/' paths."""
    out = []
    for root, dirs, files in os.walk(folder):
        for d in dirs:
            if os.path.islink(os.path.join(root, d)):
                raise ValueError(f"symlinked folder: {os.path.relpath(os.path.join(root, d), folder)}")
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            if rel == NAME:
                continue
            if os.path.islink(full) or not os.path.isfile(full):
                raise ValueError(f"not a regular file: {rel}")
            out.append(rel)
    return sorted(out)


def version_h(path):
    with open(path, encoding="utf-8") as f:
        m = re.search(r'^#define\s+SCO_VERSION\s+"([^"]+)"', f.read(), re.M)
    if not m:
        raise ValueError(f"no SCO_VERSION in {path}")
    return m.group(1)


def render(version, tag, commit, entries):
    """JSON with one file per line. Values are checked first, so nothing needs escaping."""
    lines = ["{", f'  "format": {FORMAT},', f'  "version": "{version}",', f'  "tag": "{tag}",',
             f'  "commit": "{commit}",', '  "files": [']
    body = [f'    {{"path": "{e["path"]}", "sha256": "{e["sha256"]}", "size": {e["size"]}}}' for e in entries]
    lines.append(",\n".join(body))
    lines += ["  ]", "}", ""]
    return "\n".join(lines)


def write(folder, tag, commit, version_file):
    m = TAG_RE.match(tag)
    if not m:
        raise ValueError(f"tag {tag!r} isn't v<major>.<minor>.<patch>[-pre]")
    version = m.group(1)
    if version_file:
        have = version_h(version_file)
        if have != version:
            raise ValueError(f"tag {tag} but {version_file} says SCO_VERSION \"{have}\"; bump it before tagging")
    if not re.fullmatch(r"[0-9a-f]{40}|", commit):
        raise ValueError(f"commit {commit!r} isn't 40 lowercase hex")
    entries = []
    for rel in walk(folder):
        why = bad_path(rel)
        if why:
            raise ValueError(f"{rel}: {why}")
        full = os.path.join(folder, *rel.split("/"))
        entries.append({"path": rel, "sha256": sha256(full), "size": os.path.getsize(full)})
    if not entries:
        raise ValueError(f"{folder} has no files")
    text = render(version, tag, commit, entries)
    json.loads(text)  # the hand-rolled JSON must stay valid JSON
    with open(os.path.join(folder, NAME), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return version, len(entries)


def check(folder):
    """Problems with folder against its manifest.json; empty means it matches exactly."""
    with open(os.path.join(folder, NAME), encoding="utf-8") as f:
        man = json.load(f)
    problems = []
    if man.get("format") != FORMAT:
        problems.append(f"format {man.get('format')!r}, expected {FORMAT}")
    m = TAG_RE.match(str(man.get("tag", "")))
    if not m or m.group(1) != man.get("version"):
        problems.append(f"version {man.get('version')!r} doesn't match tag {man.get('tag')!r}")
    listed = {}
    for e in man.get("files", []):
        rel = e.get("path", "")
        why = bad_path(rel)
        if why:
            problems.append(f"{rel!r}: {why}")
            continue
        if rel in listed:
            problems.append(f"{rel}: listed twice")
        listed[rel] = e
    on_disk = set(walk(folder))
    for rel in sorted(on_disk - set(listed)):
        problems.append(f"{rel}: not in the manifest")
    for rel, e in sorted(listed.items()):
        if rel not in on_disk:
            problems.append(f"{rel}: missing")
            continue
        full = os.path.join(folder, *rel.split("/"))
        if sha256(full) != e.get("sha256"):
            problems.append(f"{rel}: SHA-256 differs")
        elif os.path.getsize(full) != e.get("size"):
            problems.append(f"{rel}: size differs")
    return man, problems


def main(argv=None):
    ap = argparse.ArgumentParser(description="manifest.json for a release folder (issue #31, S1)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write", help="write <folder>/manifest.json")
    w.add_argument("folder")
    w.add_argument("--tag", required=True)
    w.add_argument("--commit", default="")
    w.add_argument("--version-h", default=None, help="src/version.h; its SCO_VERSION must equal the tag")
    c = sub.add_parser("check", help="check <folder> against its manifest.json")
    c.add_argument("folder")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "write":
            version, n = write(a.folder, a.tag, a.commit, a.version_h)
            print(f"manifest.json: {a.tag} (version {version}), {n} files")
            return 0
        man, problems = check(a.folder)
        for p in problems:
            print(f"[!] {p}")
        if problems:
            print(f"manifest check FAILED: {len(problems)} problem(s)")
            return 1
        print(f"[+] {len(man['files'])} files match manifest.json ({man['tag']}, commit {man.get('commit') or '?'})")
        return 0
    except (OSError, ValueError) as e:
        print(f"release-manifest: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
