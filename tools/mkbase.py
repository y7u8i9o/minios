#!/usr/bin/env python3
"""Split the root tree of the build into the packages of the base system
(docs/plan/packaging.md, P3, and docs/design/packages.md).

usage: mkbase.py --root DIR --defs DIR --abi FILE --out DIR --version V
                 [--mkpkg FILE] [--check] [--serial] [--skip NAME]...

A base package is a directory of --defs that contains a file named paths,
beside its manifest. Each line of paths is a pattern relative to the root:
"*" and "?" match within one path component, "[...]" a set of characters,
and a component "**" any number of components. A pattern that matches a
directory makes the directory a member of the package, with its mode. Its
contents belong to the package only when patterns match them as well, as
"usr/share/zoneinfo/**" does, which matches the directory and everything
below it. Lines starting with "#" are comments. A
package whose paths file is empty is a metapackage: an archive with a
manifest and no files.

Every file and symbolic link of the root must match the patterns of
exactly one package, and every empty directory those of one package at
least, or the tool lists the paths and exits with 1. The contents of
home/ and root/ belong to the image, not to a package (tools/mkimage.sh),
and .DS_Store files of macOS are ignored.

For each package the tool copies the matched paths, with their parent
directories, into --out/stage/NAME/files, writes the manifest with the
version and a provides line for every library of usr/lib, and runs
tools/mkpkg.sh. The ABI number of a library is the one of the ABI table
--abi, and 0 for a library the table does not name, such as a fixture of
the tests, which no other package is meant to use. A library that an
unchecked line of the manifest names gets no provides line. A package
whose manifest, file contents and ABI table did not change since the last
run is not packed again. With --serial, which the build of a development tree
gives, the version of a package is V and a serial number that grows each
time the package is packed again, as 0.3.1.4, which makes a changed
package an upgrade for pkg on the development disk (P8). --check only
reports the coverage. --skip leaves a package out, as the build does with
the package tests when CONFIG_TESTS is 0.
"""
import argparse
import fnmatch
import hashlib
import os
import re
import shutil
import subprocess
import sys

IMAGE_ONLY = ("home/", "root/")


def compile_pattern(pattern):
    """Translate a pattern into a regular expression for a whole path."""
    parts = []
    for i, comp in enumerate(pattern.split("/")):
        if comp == "**":
            parts.append("(?:[^/]+(?:/[^/]+)*)?")
            continue
        out = ""
        j = 0
        while j < len(comp):
            c = comp[j]
            if c == "*":
                out += "[^/]*"
            elif c == "?":
                out += "[^/]"
            elif c == "[":
                k = comp.find("]", j)
                if k < 0:
                    out += re.escape(c)
                else:
                    out += "[" + comp[j + 1:k].replace("\\", "\\\\") + "]"
                    j = k
            else:
                out += re.escape(c)
            j += 1
        parts.append(out)
    # A "**" component may match nothing, which leaves its slashes alone.
    regex = "/".join(parts)
    regex = regex.replace("/(?:[^/]+(?:/[^/]+)*)?/", "(?:/[^/]+)*/")
    regex = regex.replace("/(?:[^/]+(?:/[^/]+)*)?", "(?:/[^/]+)*")
    regex = regex.replace("(?:[^/]+(?:/[^/]+)*)?/", "(?:[^/]+/)*")
    return re.compile("^" + regex + "$")


def walk(root):
    """The paths of the tree: name -> "f", "l" or "d", relative to root."""
    entries = {}
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        rel_dir = os.path.relpath(dirpath, root)
        rel_dir = "" if rel_dir == "." else rel_dir
        for d in list(dirnames):
            rel = os.path.join(rel_dir, d) if rel_dir else d
            if os.path.islink(os.path.join(dirpath, d)):
                entries[rel] = "l"
                dirnames.remove(d)
            else:
                entries[rel] = "d"
        for f in filenames:
            if f == ".DS_Store":
                continue
            rel = os.path.join(rel_dir, f) if rel_dir else f
            entries[rel] = "l" if os.path.islink(os.path.join(dirpath, f)) else "f"
    return {k: v for k, v in entries.items() if not k.startswith(IMAGE_ONLY)}


def load_packages(defs, skip):
    packages = {}
    for name in sorted(os.listdir(defs)):
        paths = os.path.join(defs, name, "paths")
        if name in skip or not os.path.isfile(paths):
            continue
        patterns = []
        with open(paths) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    patterns.append((line, compile_pattern(line)))
        with open(os.path.join(defs, name, "manifest")) as f:
            manifest = f.read()
        packages[name] = {"patterns": patterns, "manifest": manifest, "members": []}
    return packages


def assign(entries, packages):
    """Matches every path against the packages and reports the problems."""
    owners = {}
    for name, pkg in packages.items():
        used = set()
        for path in entries:
            for text, regex in pkg["patterns"]:
                if regex.match(path):
                    owners.setdefault(path, []).append(name)
                    used.add(text)
                    break
        for text, _ in pkg["patterns"]:
            if text not in used:
                print(f"mkbase: {name}: the pattern {text} matches nothing", file=sys.stderr)
    problems = 0
    for path, kind in sorted(entries.items()):
        names = owners.get(path, [])
        if kind != "d" and len(names) > 1:
            print(f"mkbase: {path} matches {', '.join(names)}", file=sys.stderr)
            problems += 1
        elif kind != "d" and not names:
            print(f"mkbase: {path} belongs to no package", file=sys.stderr)
            problems += 1
        elif kind == "d" and not names:
            prefix = path + "/"
            if not any(p.startswith(prefix) and entries[p] != "d" for p in entries):
                print(f"mkbase: the empty directory {path} belongs to no package", file=sys.stderr)
                problems += 1
        for name in names:
            packages[name]["members"].append(path)
    return problems


def abi_table(path):
    table = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2:
                table[parts[0]] = parts[1]
    return table


def serial_version(out, name, version, bump):
    """V and the serial number of the package, which starts at 1 and is
    raised by one when bump."""
    path = os.path.join(out, f"{name}.serial")
    if os.path.exists(path):
        with open(path) as f:
            serial = int(f.read().strip() or 0) + (1 if bump else 0)
    else:
        serial = 1
    with open(path, "w") as f:
        f.write(f"{serial}\n")
    return f"{version}.{serial}"


def manifest_text(name, pkg, version, abi, entries):
    lines = [l for l in pkg["manifest"].splitlines() if l.strip()]
    unchecked = [l.split(None, 1)[1] for l in lines if l.startswith("unchecked ")]
    if not any(l.startswith("version ") for l in lines):
        lines.append(f"version {version}")
    for path in sorted(pkg["members"]):
        base = os.path.basename(path)
        if (os.path.dirname(path) == "usr/lib" and base.startswith("lib") and ".so" in base
                and entries[path] == "f" and not any(fnmatch.fnmatchcase(path, u) for u in unchecked)):
            lines.append(f"provides {base} {abi.get(base, '0')}")
    if not any(l.startswith("name ") for l in lines):
        lines.insert(0, f"name {name}")
    return "\n".join(lines) + "\n"


def digest(root, members, manifest, abi_text):
    """A digest of the manifest, of the ABI table and of the modes and
    contents of the files. tools/mkpkg.sh writes the needs lines from the
    ABI table, so a changed ABI number packs every package again.
    Modification times are left out, since the build rewrites some files,
    the message catalogues among them, without changing them."""
    h = hashlib.sha256(manifest.encode())
    h.update(abi_text.encode())
    for path in sorted(members):
        full = os.path.join(root, path)
        st = os.lstat(full)
        h.update(f"{path} {st.st_mode}".encode())
        if os.path.islink(full):
            h.update(os.readlink(full).encode())
        elif os.path.isfile(full):
            with open(full, "rb") as f:
                h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()


def stage(root, out, name, members, manifest):
    dest = os.path.join(out, "stage", name)
    shutil.rmtree(dest, ignore_errors=True)
    files = os.path.join(dest, "files")
    os.makedirs(files)
    for path in sorted(members):
        src = os.path.join(root, path)
        dst = os.path.join(files, path)
        # The parents take the modes of the root tree.
        parent = os.path.dirname(path)
        missing = []
        while parent and not os.path.isdir(os.path.join(files, parent)):
            missing.append(parent)
            parent = os.path.dirname(parent)
        for d in reversed(missing):
            os.mkdir(os.path.join(files, d))
            shutil.copymode(os.path.join(root, d), os.path.join(files, d))
        if os.path.islink(src):
            os.symlink(os.readlink(src), dst)
        elif os.path.isdir(src):
            if not os.path.isdir(dst):
                os.mkdir(dst)
            shutil.copymode(src, dst)
        else:
            shutil.copy2(src, dst)
    with open(os.path.join(dest, "manifest"), "w") as f:
        f.write(manifest)
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--defs", required=True)
    ap.add_argument("--abi", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--mkpkg", default=os.path.join(os.path.dirname(__file__), "mkpkg.sh"))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--serial", action="store_true")
    ap.add_argument("--skip", action="append", default=[])
    args = ap.parse_args()

    entries = walk(args.root)
    packages = load_packages(args.defs, set(args.skip))
    problems = assign(entries, packages)
    if problems:
        print(f"mkbase: {problems} paths are not assigned to exactly one package", file=sys.stderr)
        return 1
    if args.check:
        for name, pkg in packages.items():
            print(f"{name} {len([m for m in pkg['members'] if entries[m] != 'd'])}")
        return 0

    abi = abi_table(args.abi)
    with open(args.abi) as f:
        abi_text = f.read()
    os.makedirs(args.out, exist_ok=True)
    for name, pkg in packages.items():
        # The digest covers the manifest without the version, which a
        # serial number changes.
        base_manifest = manifest_text(name, pkg, args.version, abi, entries)
        stamp = os.path.join(args.out, f"{name}.stamp")
        sums = digest(args.root, pkg["members"], base_manifest, abi_text)
        unchanged = os.path.exists(stamp) and open(stamp).read() == sums
        version = serial_version(args.out, name, args.version, not unchanged) if args.serial else args.version
        manifest = base_manifest if not args.serial else manifest_text(name, pkg, version, abi, entries)
        version = re.search(r"^version (\S+)$", manifest, re.M).group(1)
        archive = os.path.join(args.out, f"{name}-{version}.mpk")
        if unchanged and os.path.exists(archive):
            continue
        for old in os.listdir(args.out):
            if old.startswith(name + "-") and old.endswith(".mpk") and re.fullmatch(
                    re.escape(name) + r"-[0-9.]+\.mpk", old):
                os.remove(os.path.join(args.out, old))
        dest = stage(args.root, args.out, name, pkg["members"], manifest)
        subprocess.run(["sh", args.mkpkg, dest, archive, args.abi], check=True)
        with open(stamp, "w") as f:
            f.write(sums)
        print(f"mkbase: {name} {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
