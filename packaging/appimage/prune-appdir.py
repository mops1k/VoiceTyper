#!/usr/bin/env python3
"""Removes libraries nothing in the AppDir links against any more.

linuxdeploy deploys the transitive closure of everything it sees, which for a Qt
application includes modules the application never loads (Qt Qml/Quick pulled in
by the Virtual Keyboard plugin, Pdf/PrintSupport pulled in by the KDE image
format plugins, and the codec libraries those use). The entry points are the
application binary and the Qt plugins; a library is kept only when an ldd of a
kept file resolves to it.

Usage: prune-appdir.py <AppDir>
"""

import os
import subprocess
import sys


def elf_files(path):
    found = []
    for dirpath, _, files in os.walk(path):
        for name in files:
            candidate = os.path.join(dirpath, name)
            if os.path.islink(candidate) or not os.path.isfile(candidate):
                continue
            try:
                with open(candidate, "rb") as handle:
                    if handle.read(4) == b"\x7fELF":
                        found.append(candidate)
            except OSError:
                continue
    return found


def main():
    if len(sys.argv) != 2:
        print("usage: prune-appdir.py <AppDir>", file=sys.stderr)
        return 2
    appdir = os.path.abspath(sys.argv[1])
    libdir = os.path.join(appdir, "usr", "lib")

    roots = elf_files(os.path.join(appdir, "usr", "bin"))
    roots += elf_files(os.path.join(appdir, "usr", "plugins"))
    roots += elf_files(os.path.join(appdir, "usr", "libexec"))

    keep = set()
    queue = list(roots)
    while queue:
        current = queue.pop()
        try:
            output = subprocess.run(
                ["ldd", current], capture_output=True, text=True, check=False
            ).stdout
        except OSError:
            continue
        for line in output.splitlines():
            parts = line.split()
            if "=>" not in parts:
                continue
            dependency = parts[parts.index("=>") + 1]
            if not dependency.startswith(appdir):
                continue
            real = os.path.realpath(dependency)
            if real not in keep:
                keep.add(real)
                queue.append(real)

    removed = 0
    freed = 0
    if os.path.isdir(libdir):
        for name in sorted(os.listdir(libdir)):
            path = os.path.join(libdir, name)
            real = os.path.realpath(path)
            if real in keep:
                continue
            try:
                freed += os.path.getsize(path)
                os.remove(path)
                removed += 1
            except OSError:
                continue
    print(f"pruned {removed} unused librar{'y' if removed == 1 else 'ies'} "
          f"({freed / 1048576:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
