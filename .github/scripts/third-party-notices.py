#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 PairPaint contributors
#
# Writes THIRD-PARTY-NOTICES.txt for a release download: the copyright notices and license texts
# of everything bundled next to PairPaint, which their licenses (LGPL, MIT, BSD, ...) require us
# to pass on. For the release workflow; only the Python standard library and git are needed.
#
#   third-party-notices.py <linux|windows|macos> <app dir> <output file>
#                          [--component NAME VERSION SOURCE_URL LICENSE_FILE]...
#
# - Qt: the notices of the Qt version in QT_ROOT_DIR, including the code Qt itself bundles, come
#   from the qt_attribution.json files in Qt's sources (fetched from GitHub with a sparse clone).
# - Linux: the libraries linuxdeploy copied from Ubuntu are looked up with dpkg, and their Debian
#   copyright files included; ICU (shipped with Qt) gets its license from the ICU repository.
# - --component: anything else bundled, such as libheif built from source.
#
# Every library in <app dir> must be accounted for: an unknown one stops the release, so a new
# dependency can't ship without its license.

import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.request

QT_MODULES = {
    "linux": ["qtbase", "qtsvg", "qtimageformats", "qtwayland"],
    "windows": ["qtbase", "qtsvg", "qtimageformats"],
    "macos": ["qtbase", "qtsvg", "qtimageformats"],
}
# Code in Qt's sources that isn't part of the libraries we ship.
QT_SKIP = ("tests/", "cmake/", "util/", "src/testlib/", "src/testinternal/", "src/3rdparty/android/",
           "src/3rdparty/wasm/", "src/3rdparty/gradle/")

# The Visual C++ runtime, which Microsoft allows apps to redistribute.
MSVC_RUNTIME = re.compile(r"^(vcruntime|msvcp|concrt|vccorlib)140.*\.dll$", re.I)

RULE = "=" * 78


def run(*cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{' '.join(cmd[:3])} ... failed:\n{r.stderr}")
    return r.stdout


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().strip()


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read().decode("utf-8", errors="replace").strip()


class Notices:
    def __init__(self):
        self.parts = []
        self.texts = {}  # hash of a license text -> where it was printed first

    def add(self, text=""):
        self.parts.append(text)

    def heading(self, title):
        self.add(f"\n{RULE}\n{title}\n{RULE}\n")

    def license(self, text, owner):
        # Many components share a license text: print it once, then refer to it.
        key = hashlib.sha256(" ".join(text.split()).encode()).hexdigest()
        if key in self.texts:
            self.add(f"License text: the same as for {self.texts[key]} above.\n")
        else:
            self.texts[key] = owner
            self.add(f"{text}\n")


def spdx_ids(expression):
    return [t for t in re.split(r"[\s()]+", expression) if t and t not in ("AND", "OR", "WITH")]


def qt_version():
    qmake = os.path.join(os.environ["QT_ROOT_DIR"], "bin", "qmake")
    return run(qmake, "-query", "QT_VERSION").strip()


def qt_section(notes, platform, version, tmp):
    """Qt itself (LGPL-3.0) and the third-party code inside it."""
    entries = []  # (module, folder in the clone, attribution)
    for module in QT_MODULES[platform]:
        repo = os.path.join(tmp, module)
        run("git", "clone", "-q", "--depth", "1", "--branch", f"v{version}", "--filter=blob:none",
            "--sparse", f"https://github.com/qt/{module}.git", repo)
        run("git", "sparse-checkout", "set", "--no-cone", "**/qt_attribution.json", "/LICENSES/", cwd=repo)
        wanted = []
        for path in sorted(glob.glob("**/qt_attribution.json", root_dir=repo, recursive=True)):
            folder = os.path.dirname(path).replace(os.sep, "/")
            if (folder + "/").startswith(QT_SKIP):
                continue
            data = json.loads(read(os.path.join(repo, path)), strict=False)
            for e in data if isinstance(data, list) else [data]:
                if e.get("QtParts", ["libs"]) != ["libs"]:
                    continue
                files = e.get("LicenseFiles") or e.get("LicenseFile") or []
                files = [files] if isinstance(files, str) else files
                e["_files"] = [os.path.normpath(os.path.join(folder, f)).replace(os.sep, "/") for f in files]
                wanted += e["_files"]
                entries.append((module, repo, e))
        if wanted:
            run("git", "sparse-checkout", "add", *sorted({"/" + f for f in wanted}), cwd=repo)

    base = os.path.join(tmp, "qtbase")
    notes.heading(f"Qt {version}")
    notes.add(f"Copyright (C) The Qt Company Ltd. and other contributors.\n"
              f"Used under the GNU Lesser General Public License v3 (below); PairPaint links to it\n"
              f"dynamically, so you can replace the Qt libraries in this download with your own build.\n"
              f"Source code: https://download.qt.io/archive/qt/{version.rsplit('.', 1)[0]}/{version}/submodules/\n"
              f"(the modules {', '.join(QT_MODULES[platform])}).\n")
    notes.license(read(os.path.join(base, "LICENSES", "LGPL-3.0-only.txt")), f"Qt {version}")
    notes.add("The LGPL v3 builds on the GNU General Public License v3, in LICENSE.txt.\n")

    notes.heading(f"Third-party code included in Qt {version}")
    for module, repo, e in entries:
        title = e["Name"] + (f" {e['Version']}" if e.get("Version") else "")
        notes.add(f"--- {title} (in {module}) ---\n")
        if e.get("Homepage"):
            notes.add(f"Homepage: {e['Homepage']}")
        notes.add(f"License: {e['License']} ({e['LicenseId']})")
        copyright = e.get("Copyright") or []
        copyright = copyright.splitlines() if isinstance(copyright, str) else copyright
        notes.add("\n".join(line.strip() for line in copyright) + "\n")
        texts = [os.path.join(repo, f) for f in e["_files"]]
        if not texts:  # no file of its own: the standard text of each license
            texts = [os.path.join(repo, "LICENSES", f"{i}.txt") for i in spdx_ids(e["LicenseId"])]
            texts = [t if os.path.exists(t) else os.path.join(base, "LICENSES", os.path.basename(t)) for t in texts]
        for t in texts:
            if not os.path.exists(t):
                sys.exit(f"License text {t} for {e['Name']} not found")
            notes.license(read(t), title)


def component_section(notes, name, version, url, license_file):
    notes.heading(f"{name} {version}")
    notes.add(f"Source code: {url}\n")
    notes.license(read(license_file), name)


def icu_section(notes, major):
    notes.heading(f"ICU {major} (International Components for Unicode, shipped with Qt)")
    notes.add(f"Source code: https://github.com/unicode-org/icu/releases (release {major})\n")
    notes.license(fetch(f"https://raw.githubusercontent.com/unicode-org/icu/release-{major}-1/icu4c/LICENSE"),
                  "ICU")


def debian_section(notes, libs):
    """Libraries linuxdeploy copied from Ubuntu, with the copyright files of their packages."""
    packages = {}
    for lib in libs:
        out = subprocess.run(["dpkg", "-S", f"*/{lib}"], capture_output=True, text=True).stdout
        owners = {p.split(":")[0] for line in out.splitlines() if ": " in line
                  for p in line.split(": ")[0].split(", ")}
        if not owners:
            sys.exit(f"{lib} is in the AppImage, but no Ubuntu package has it: add its license")
        packages.setdefault(sorted(owners)[0], []).append(lib)

    common = set()
    for package, files in sorted(packages.items()):
        version, source = run("dpkg-query", "-W", "-f", "${Version}\t${source:Package}", package).split("\t")
        notes.heading(f"{package} {version} (Ubuntu 22.04): {', '.join(sorted(files))}")
        notes.add(f"Source code: https://launchpad.net/ubuntu/+source/{source}/{version}\n")
        text = read(f"/usr/share/doc/{package}/copyright")
        common.update(re.findall(r"/usr/share/common-licenses/([\w.+-]*\w)", text))
        notes.add(text + "\n")

    for name in sorted(common):
        path = f"/usr/share/common-licenses/{name}"
        if os.path.isfile(path):
            notes.heading(f"Referenced above as {path}")
            notes.license(read(path), path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("platform", choices=QT_MODULES)
    parser.add_argument("appdir")
    parser.add_argument("output")
    parser.add_argument("--component", nargs=4, action="append", default=[],
                        metavar=("NAME", "VERSION", "SOURCE_URL", "LICENSE_FILE"))
    args = parser.parse_args()

    # Sort what's bundled into Qt, the extra components, and (on Linux) the rest.
    others, icu = [], None
    patterns = {"linux": "usr/lib/*.so*", "windows": "*.dll", "macos": "Contents/Frameworks/*"}
    for path in sorted(glob.glob(os.path.join(args.appdir, patterns[args.platform]))):
        name = os.path.basename(path)
        if re.match(r"^(libQt6\w+\.so\.6|Qt6\w+\.dll|Qt\w+\.framework)$", name):
            continue
        if any(name.lower().startswith(c[0].lower()) for c in args.component):
            continue
        if args.platform == "windows" and MSVC_RUNTIME.match(name):
            continue
        m = re.match(r"^libicu\w+\.so\.(\d+)$", name)
        if args.platform == "linux" and m:
            icu = m.group(1)
            continue
        if args.platform != "linux":
            sys.exit(f"Don't know the license of {name}: add it to {os.path.basename(__file__)}")
        others.append(name)

    notes = Notices()
    notes.add("PairPaint: third-party software\n\n"
              "PairPaint is free software under the GNU General Public License v3 or later (LICENSE.txt).\n"
              "This download also contains the software below, made by others and used under their\n"
              "licenses: their copyright notices, license texts and where to get their source code.")
    with tempfile.TemporaryDirectory() as tmp:
        qt_section(notes, args.platform, qt_version(), tmp)
    for name, version, url, license_file in args.component:
        component_section(notes, name, version, url, license_file)
    if icu:
        icu_section(notes, icu)
    if others:
        debian_section(notes, others)
    if args.platform == "windows":
        notes.heading("Microsoft Visual C++ Runtime")
        notes.add("The vcruntime140, msvcp140, concrt140 and vccorlib140 DLLs are Copyright (C) Microsoft\n"
                  "Corporation, redistributed under the Microsoft Visual Studio license terms for\n"
                  "Distributable Code.\n")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(notes.parts) + "\n")
    print(f"Wrote {args.output} ({os.path.getsize(args.output) // 1024} KB)")


if __name__ == "__main__":
    main()
