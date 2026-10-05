#!/usr/bin/env python3
"""Trees and manifests of the ISO 9660 cases (docs/design/iso9660.md).

    iso_tree.py rock TREE                 the tree of the case iso9660
    iso_tree.py plain TREE                the tree of the case iso9660_plain
    iso_tree.py manifest TREE OUT [MODE]  the manifest that isotest prints

The manifest has one line per entry below TREE, depth first, with the
names of each directory in byte order:

    ISO:path|type|permissions|uid|gid|size|mtime|extra

MODE "owned=UID:GID" gives the entry "owned" that owner, as the image
script sets it with xorriso, and every other entry the owner root. MODE
"plain" gives every entry the owner root and the modes 0555 and 0444 of an
image without Rock Ridge.
"""
import os
import random
import shutil
import stat
import sys

BASE_TIME = 1000000000  # 2001-09-09, the times are whole seconds


def write(path, data, mode=0o644):
    with open(path, "wb") as f:
        f.write(data)
    os.chmod(path, mode)


def build_rock(tree):
    write(os.path.join(tree, "isotest.txt"), b"marker of the ISO 9660 test image\n")
    write(os.path.join(tree, "plain.txt"), b"plain\n")
    write(os.path.join(tree, "empty"), b"")
    write(os.path.join(tree, "file with spaces.txt"), b"blanks\n")
    write(os.path.join(tree, "gr\u00fc\u00dfe-\u65e5\u672c.txt"), "utf-8\n".encode())
    write(os.path.join(tree, "n" * 180 + ".txt"), b"a long name\n")
    write(os.path.join(tree, "program"), b"#!/bin/sh\necho hi\n", 0o755)
    write(os.path.join(tree, "private"), b"secret\n", 0o600)
    write(os.path.join(tree, "setuid"), b"set user id\n", 0o4755)
    write(os.path.join(tree, "setgid"), b"set group id\n", 0o2711)
    write(os.path.join(tree, "owned"), b"owned by 1000:100\n", 0o640)
    rng = random.Random(5)
    write(os.path.join(tree, "large.bin"), bytes(rng.getrandbits(8) for _ in range(3 * 1024 * 1024)))
    many = os.path.join(tree, "many")
    os.mkdir(many, 0o755)
    for i in range(300):
        write(os.path.join(many, "f%03d" % i), ("file %d\n" % i).encode())
    deep = tree
    for i in range(1, 13):
        deep = os.path.join(deep, "d%02d" % i)
        os.mkdir(deep, 0o750 if i == 9 else 0o755)
    write(os.path.join(deep, "deep.txt"), b"twelve levels down\n")
    docs = os.path.join(tree, "docs")
    os.mkdir(docs, 0o755)
    write(os.path.join(docs, "readme.txt"), b"read me\n")
    os.symlink("/etc/motd", os.path.join(tree, "abs-link"))
    os.symlink("../docs/readme.txt", os.path.join(many, "rel-link"))
    os.symlink("docs", os.path.join(tree, "dir-link"))
    os.symlink("./docs/../docs/./readme.txt", os.path.join(tree, "dot-link"))
    os.symlink("dangling/target", os.path.join(tree, "dangling-link"))
    long_target = "/".join(["component%02d" % i for i in range(18)])
    os.symlink(long_target, os.path.join(tree, "long-link"))


def build_plain(tree):
    write(os.path.join(tree, "isotest.txt"), b"marker of the ISO 9660 test image\n")
    write(os.path.join(tree, "readme.txt"), b"an image without Rock Ridge\n", 0o755)
    docs = os.path.join(tree, "docs")
    os.mkdir(docs)
    write(os.path.join(docs, "notes.txt"), b"notes\n")
    sub = os.path.join(docs, "sub")
    os.mkdir(sub)
    rng = random.Random(7)
    write(os.path.join(sub, "data.bin"), bytes(rng.getrandbits(8) for _ in range(65536)))


def set_times(tree):
    """Give every entry a fixed time, the deepest first, so that the
    directories retain their times."""
    paths = []
    for root, dirs, files in os.walk(tree):
        for name in dirs + files:
            paths.append(os.path.join(root, name))
    for i, path in enumerate(sorted(paths, key=lambda p: -p.count(os.sep))):
        t = BASE_TIME + i * 61
        os.utime(path, (t, t), follow_symlinks=False)


def fnv(path):
    h = 0xCBF29CE484222325
    with open(path, "rb") as f:
        for byte in f.read():
            h ^= byte
            h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def manifest(tree, out, mode):
    owned = None
    plain = mode == "plain"
    if mode.startswith("owned="):
        uid, gid = mode[6:].split(":")
        owned = (int(uid), int(gid))
    lines = []

    def walk(rel):
        here = os.path.join(tree, rel) if rel else tree
        for name in sorted(os.listdir(here), key=lambda n: n.encode()):
            relpath = rel + "/" + name if rel else name
            path = os.path.join(tree, relpath)
            st = os.lstat(path)
            uid, gid = (owned if owned and relpath == "owned" else (0, 0))
            perm = stat.S_IMODE(st.st_mode)
            if stat.S_ISDIR(st.st_mode):
                if plain:
                    perm = 0o555
                lines.append("ISO:%s|d|%04o|%u|%u|-|%d|" % (relpath, perm, uid, gid, int(st.st_mtime)))
                walk(relpath)
            elif stat.S_ISLNK(st.st_mode):
                target = os.readlink(path)
                lines.append("ISO:%s|l|%04o|%u|%u|%d|%d|%s" % (relpath, perm, uid, gid, len(target.encode()),
                                                               int(st.st_mtime), target))
            else:
                if plain:
                    perm = 0o444
                lines.append("ISO:%s|f|%04o|%u|%u|%d|%d|%s" % (relpath, perm, uid, gid, st.st_size,
                                                               int(st.st_mtime), fnv(path)))

    walk("")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    cmd = sys.argv[1]
    if cmd in ("rock", "plain"):
        tree = sys.argv[2]
        shutil.rmtree(tree, ignore_errors=True)
        os.makedirs(tree)
        build_rock(tree) if cmd == "rock" else build_plain(tree)
        set_times(tree)
    elif cmd == "manifest":
        manifest(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else "")
    else:
        sys.exit("usage: iso_tree.py rock|plain TREE | manifest TREE OUT [MODE]")


if __name__ == "__main__":
    main()
