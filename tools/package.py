#!/usr/bin/env python3
"""Stage and zip the explicit release files, excluding stale files and metadata."""

import argparse
from pathlib import Path
import shutil
from zipfile import ZIP_DEFLATED, ZipFile


FILES = {
    "nxhid.nro": "switch/nxhid/nxhid.nro",
    "client/nxhid.py": "client/nxhid.py",
    "README.md": "README.md",
    "LICENSE": "LICENSE",
    "docs/images/screenshot.jpg": "docs/images/screenshot.jpg",
}


def build_package(root, version):
    for source in FILES:
        if not (root / source).is_file():
            raise FileNotFoundError(root / source)
    destination = root / "dist"
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir()
    archive = destination / f"nxhid-{version}.zip"
    with ZipFile(archive, "w", compression=ZIP_DEFLATED) as bundle:
        for source, name in FILES.items():
            target = destination / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / source, target)
            bundle.write(target, arcname=name)
    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    args = parser.parse_args()
    print(build_package(Path(__file__).resolve().parents[1], args.version))
