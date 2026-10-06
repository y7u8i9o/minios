#!/usr/bin/env python3
"""The version of an application package (user/packages/packages.mk).

usage: pkgserial.py STATE NAME VERSION STAGE ABI SERIAL

STAGE is the staged package directory with its manifest and files/,
without the version line. With SERIAL 1, which a development build
passes, the result is VERSION followed by a serial number, as
tools/mkbase.py gives the base packages. The serial number in
STATE/NAME.serial grows when the digest of the staged files, the manifest
and the ABI table differs from STATE/NAME.stamp. A rebuilt application
is then an upgrade for pkg-update on the development disk, also when only
the ABI of a library changed. With SERIAL 0, a release, the result is
VERSION."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkbase import digest, serial_version, walk  # noqa: E402


def main():
    if len(sys.argv) != 7:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    state, name, version, stage, abi, serial = sys.argv[1:]
    if serial != "1":
        print(version)
        return 0
    with open(os.path.join(stage, "manifest")) as f:
        manifest = f.read()
    with open(abi) as f:
        abi_text = f.read()
    files = os.path.join(stage, "files")
    members = walk(files) if os.path.isdir(files) else {}
    sums = digest(files, members, manifest, abi_text)
    os.makedirs(state, exist_ok=True)
    stamp = os.path.join(state, f"{name}.stamp")
    unchanged = os.path.exists(stamp) and open(stamp).read() == sums
    print(serial_version(state, name, version, not unchanged))
    with open(stamp, "w") as f:
        f.write(sums)
    return 0


if __name__ == "__main__":
    sys.exit(main())
