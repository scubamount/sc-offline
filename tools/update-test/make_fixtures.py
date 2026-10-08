#!/usr/bin/env python3
"""Fixtures and on-disk checks for tools/update-test/run.sh (issue #31 D6). Python 3 stdlib only.

    make_fixtures.py build --manifest-tool tools/release-manifest.py --new-exe new.exe --stub-exe stub.exe \
                           --ini launcher/sc-offline.ini --out <dir>
        Writes one release zip per scenario, <out>/<scenario>/sc-offline-<tag>.zip, the unpacked folder
        next to it, and prints one tab-separated line per scenario: name, zip path, tag, zip sha256.
    make_fixtures.py snapshot <install dir> <out.json>
        SHA-256 of every file under the install dir, minus what a launcher start or an update may
        legitimately leave (data/launcher.log, data/game-path.txt, data/game-build.txt, data/update/).
    make_fixtures.py same <install dir> <snapshot.json>
        Exit 0 when the install dir matches the snapshot byte for byte; otherwise prints the differences.
    make_fixtures.py updated <install dir> <release folder> <snapshot.json>
        Exit 0 when every file the release ships is in the install dir with the release's bytes, except
        player-owned files, which must still hold their snapshot bytes (sc-offline.ini may gain keys,
        but must keep the snapshot's `start_ship` line).

Every zip is a real release layout: sc-offline-<tag>/ holding sc-offline.exe, dinput8.dll, sc-offline.ini,
README.md, data/, docs/ and manifest.json written by tools/release-manifest.py. The bad ones are made by
changing a good one after (or instead of) `release-manifest.py write`, so each differs from a good release
in exactly one way.
"""
import hashlib
import importlib.util
import json
import os
import shutil
import sys
import zipfile

OLD_VERSION = "0.6.9"   # what run.sh installs; every fixture is judged against it
NEW_TAG = "v0.7.0"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
# Mirrors PlayerOwned() in launcher/launcher.cpp: an update never replaces these.
PLAYER_OWNED = {"sc-offline.ini", "data/wallet.txt", "data/spawn.txt", "data/bookmarks.txt",
                "data/locations_found.txt", "data/game-path.txt", "data/game-build.txt", "data/mod.log",
                "data/launcher.log", "data/registry-dump.txt"}
# Written by any launcher start (status finds the game in the Wine prefix), not by an update.
IGNORED = ("data/launcher.log", "data/game-path.txt", "data/game-build.txt", "data/update/")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load_manifest_tool(path):
    spec = importlib.util.spec_from_file_location("release_manifest", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"make_fixtures: can't load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def release_folder(root, tag, exe, ini):
    """A release folder like CI's, without manifest.json."""
    pkg = os.path.join(root, f"sc-offline-{tag}")
    os.makedirs(os.path.join(pkg, "data"))
    os.makedirs(os.path.join(pkg, "docs"))
    shutil.copyfile(exe, os.path.join(pkg, "sc-offline.exe"))
    shutil.copyfile(ini, os.path.join(pkg, "sc-offline.ini"))
    files = {
        "dinput8.dll": f"dinput8.dll stand-in for {tag}\n",
        "README.md": f"# sc-offline {tag}\n",
        "data/items.txt": f"# items for {tag}\nnew_item_{tag.replace('.', '_')}\n",
        "data/new-in-this-release.txt": f"added by {tag}\n",
        "docs/launcher.md": f"launcher docs for {tag}\n",
    }
    for rel, text in files.items():
        with open(os.path.join(pkg, *rel.split("/")), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return pkg


def write_manifest(tool, pkg, tag):
    tool.write(pkg, tag, COMMIT, None)


def hand_manifest(tool, pkg, version, tag, extra):
    """manifest.json in release-manifest.py's own format, with entries it would refuse to write."""
    entries = []
    for rel in tool.walk(pkg):
        full = os.path.join(pkg, *rel.split("/"))
        entries.append({"path": rel, "sha256": sha256_file(full), "size": os.path.getsize(full)})
    entries += extra
    with open(os.path.join(pkg, tool.NAME), "w", encoding="utf-8", newline="\n") as f:
        f.write(tool.render(version, tag, COMMIT, sorted(entries, key=lambda e: e["path"])))


def zip_folder(pkg, zip_path, outside=None):
    """Zip pkg's parent-relative tree; `outside` = {arcname: bytes} entries added verbatim.
    Stored, not deflated: tar-shim.exe (Wine has no tar.exe) only reads stored entries."""
    base = os.path.dirname(pkg)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_STORED) as z:
        for root, _, files in os.walk(pkg):
            for name in sorted(files):
                full = os.path.join(root, name)
                z.write(full, os.path.relpath(full, base).replace(os.sep, "/"))
        for arc, data in (outside or {}).items():
            z.writestr(arc, data)


def build(a):
    tool = load_manifest_tool(a.manifest_tool)
    out = os.path.abspath(a.out)
    rows = []

    def scenario(name, tag, exe=a.new_exe, zip_tag=None, arg_tag=None, mutate=None, manifest=None, outside=None,
                 digest=None):
        root = os.path.join(out, name)
        os.makedirs(root)
        pkg = release_folder(root, zip_tag or tag, exe, a.ini)
        if manifest:
            manifest(pkg)
        else:
            write_manifest(tool, pkg, tag)
        if mutate:
            mutate(pkg)
        zip_path = os.path.join(root, f"sc-offline-{zip_tag or tag}.zip")
        zip_folder(pkg, zip_path, outside)
        rows.append((name, zip_path, arg_tag or zip_tag or tag, digest or sha256_file(zip_path)))

    def append(rel, text):
        def f(pkg):
            with open(os.path.join(pkg, *rel.split("/")), "a", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
        return f

    evil = b"written outside the install folder\n"
    evil_entry = {"sha256": hashlib.sha256(evil).hexdigest(), "size": len(evil)}
    ver = NEW_TAG[1:]

    scenario("good", NEW_TAG)
    scenario("tampered", NEW_TAG, mutate=append("data/items.txt", "tampered after the manifest was written\n"))
    scenario("unlisted", NEW_TAG, mutate=append("data/unlisted.txt", "not in the manifest\n"))
    scenario("downgrade", "v0.6.0")
    scenario("same-version", f"v{OLD_VERSION}")
    # The manifest says v0.7.1; the zip and the release tag say v0.7.0.
    scenario("tag-mismatch", "v0.7.1", zip_tag=NEW_TAG,
             manifest=lambda pkg: hand_manifest(tool, pkg, "0.7.1", "v0.7.1", []))
    # S3: a listed path with '..', a leading '/' or a ':' fails the whole update. The zip carries the bytes
    # the bad entry points at, so only the path check stands between the zip and the disk.
    scenario("dotdot", NEW_TAG, outside={"evil.txt": evil},
             manifest=lambda pkg: hand_manifest(tool, pkg, ver, NEW_TAG, [dict(path="../evil.txt", **evil_entry)]))
    scenario("abspath", NEW_TAG, outside={"evil.txt": evil},
             manifest=lambda pkg: hand_manifest(tool, pkg, ver, NEW_TAG, [dict(path="/tmp/sco-ut/evil.txt", **evil_entry)]))
    scenario("ads", NEW_TAG,
             manifest=lambda pkg: hand_manifest(tool, pkg, ver, NEW_TAG, [dict(path="dinput8.dll:evil", **evil_entry)]))
    scenario("zip-digest", NEW_TAG, digest="0" * 64)
    scenario("mid-swap", NEW_TAG)
    scenario("kill", NEW_TAG)
    scenario("locked", NEW_TAG)
    scenario("selftest-fail", NEW_TAG, exe=a.stub_exe)
    for r in rows:
        print("\t".join(r))
    return 0


def snapshot(install):
    snap = {}
    for root, _, files in os.walk(install):
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, install).replace(os.sep, "/")
            if rel in IGNORED or any(rel.startswith(p) for p in IGNORED if p.endswith("/")):
                continue
            snap[rel] = sha256_file(full)
    return snap


def cmd_snapshot(install, out):
    with open(out, "w") as f:
        json.dump(snapshot(install), f, indent=1, sort_keys=True)
    return 0


def cmd_same(install, snap_file):
    with open(snap_file) as f:
        want = json.load(f)
    have = snapshot(install)
    problems = [f"added {r}" for r in sorted(set(have) - set(want))]
    problems += [f"missing {r}" for r in sorted(set(want) - set(have))]
    problems += [f"changed {r}" for r in sorted(set(want) & set(have)) if want[r] != have[r]]
    print("; ".join(problems[:8]) + (f"; +{len(problems) - 8} more" if len(problems) > 8 else ""))
    return 1 if problems else 0


def cmd_updated(install, pkg, snap_file):
    with open(snap_file) as f:
        before = json.load(f)
    problems = []
    for root, _, files in os.walk(pkg):
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, pkg).replace(os.sep, "/")
            dst = os.path.join(install, *rel.split("/"))
            if rel == "manifest.json":
                continue  # whether the manifest itself is copied is the launcher's choice
            if rel == "sc-offline.ini":
                with open(dst, encoding="utf-8", errors="replace") as fh:
                    if "start_ship = TEST_KEEP_ME" not in fh.read():
                        problems.append("sc-offline.ini lost the player's start_ship line")
            elif rel in PLAYER_OWNED:
                if os.path.isfile(dst) and sha256_file(dst) != before.get(rel):
                    problems.append(f"player file {rel} changed")
            elif not os.path.isfile(dst):
                problems.append(f"missing {rel}")
            elif sha256_file(dst) != sha256_file(full):
                problems.append(f"{rel} isn't the release's")
    for rel in sorted(PLAYER_OWNED & set(before)):
        if rel == "sc-offline.ini":
            continue
        dst = os.path.join(install, *rel.split("/"))
        if not os.path.isfile(dst) or sha256_file(dst) != before[rel]:
            problems.append(f"player file {rel} changed or gone")
    print("; ".join(problems))
    return 1 if problems else 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "build":
        import argparse
        ap = argparse.ArgumentParser(prog="make_fixtures.py build")
        for k in ("--manifest-tool", "--new-exe", "--stub-exe", "--ini", "--out"):
            ap.add_argument(k, required=True)
        return build(ap.parse_args(argv[2:]))
    if len(argv) == 4 and argv[1] == "snapshot":
        return cmd_snapshot(argv[2], argv[3])
    if len(argv) == 4 and argv[1] == "same":
        return cmd_same(argv[2], argv[3])
    if len(argv) == 5 and argv[1] == "updated":
        return cmd_updated(argv[2], argv[3], argv[4])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
