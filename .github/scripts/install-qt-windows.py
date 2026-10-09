#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Peter Gniewek and PairPaint contributors
#
# Installs prebuilt Qt for Windows (MSVC) from Qt's official online repository, for CI.
#
# From Qt 6.11 the Windows repository has one folder per compiler, which aqtinstall (used by
# jurplel/install-qt-action) can't read yet: https://github.com/miurahr/aqtinstall/pull/1048.
# This does the minimum: reads the package list, downloads the archives of Qt itself and the
# requested add-on modules, checks each against the SHA-256 published on download.qt.io (the
# archives themselves may come from a mirror) and unpacks them. Only the Python standard library
# and 7-Zip (preinstalled on GitHub's Windows runners) are needed.
#
#   install-qt-windows.py <version> <output dir> [modules...]
#   e.g. install-qt-windows.py 6.12.0 D:\Qt qtimageformats
#
# In GitHub Actions it also sets QT_ROOT_DIR and CMAKE_PREFIX_PATH and adds Qt's bin folder to PATH.

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

BASE = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"
ARCH = "win64_msvc2022_64"
FOLDER = "msvc2022_64"
SKIP = ("qtdoc-", "qtdeclarative-")  # documentation and QML aren't needed to build PairPaint


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.geturl(), r.read()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    version, outdir, modules = sys.argv[1], sys.argv[2], sys.argv[3:]
    tag = "qt6_" + version.replace(".", "")
    repo = f"{BASE}/{tag}/{tag}_{FOLDER}"
    _, xml = fetch(f"{repo}/Updates.xml")
    packages = {p.findtext("Name"): p for p in ET.fromstring(xml).findall("PackageUpdate")}
    wanted = [f"qt.{tag.replace('_', '.')}.{ARCH}"]
    wanted += [f"qt.{tag.replace('_', '.')}.addons.{m}.{ARCH}" for m in modules]

    sevenzip = shutil.which("7z") or r"C:\Program Files\7-Zip\7z.exe"
    root = os.path.abspath(outdir)
    os.makedirs(root, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name in wanted:
            if name not in packages:
                sys.exit(f"Package {name} not found in {repo}/Updates.xml")
            p = packages[name]
            archives = [a.strip() for a in (p.findtext("DownloadableArchives") or "").split(",") if a.strip()]
            for archive in archives:
                if archive.startswith(SKIP):
                    continue
                url = f"{repo}/{name}/{p.findtext('Version')}{archive}"
                final, checksum = fetch(url + ".sha256")
                if urllib.parse.urlparse(final).hostname != "download.qt.io":
                    sys.exit(f"Checksum for {archive} didn't come from download.qt.io ({final})")
                expected = checksum.split()[0].decode().lower()
                print(f"Downloading {name}: {archive}", flush=True)
                _, data = fetch(url)
                actual = hashlib.sha256(data).hexdigest()
                if actual != expected:
                    sys.exit(f"SHA-256 mismatch for {archive}: expected {expected}, got {actual}")
                path = os.path.join(tmp, archive)
                with open(path, "wb") as f:
                    f.write(data)
                subprocess.run([sevenzip, "x", "-y", "-bd", "-bso0", f"-o{root}", path], check=True)
                os.remove(path)

    # The software OpenGL fallback and the Direct3D shader compiler unpack into the root, but
    # windeployqt looks for them next to Qt's DLLs (aqtinstall puts them there too).
    for dll in ("opengl32sw.dll", "d3dcompiler_47.dll"):
        if os.path.exists(os.path.join(root, dll)):
            shutil.move(os.path.join(root, dll), os.path.join(root, "bin", dll))
    if not os.path.exists(os.path.join(root, "bin", "qtpaths.exe")):
        sys.exit("Qt was unpacked, but bin/qtpaths.exe is missing")
    print(f"Qt {version} installed in {root}")
    if os.environ.get("GITHUB_ENV"):
        with open(os.environ["GITHUB_ENV"], "a") as env:
            env.write(f"QT_ROOT_DIR={root}\nCMAKE_PREFIX_PATH={root}\n")
        with open(os.environ["GITHUB_PATH"], "a") as path:
            path.write(os.path.join(root, "bin") + "\n")


if __name__ == "__main__":
    main()
