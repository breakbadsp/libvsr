#!/usr/bin/env python3
"""Verify the paper collection. Use --fetch to restore missing PDFs."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def verify(path, paper):
    data = path.read_bytes()
    if not data.startswith(b"%PDF-"):
        raise ValueError(f"{path.name}: invalid PDF signature")
    if hashlib.sha256(data).hexdigest() != paper["sha256"]:
        raise ValueError(f"{path.name}: SHA-256 mismatch")
    info = subprocess.run(
        ["pdfinfo", str(path)], check=True, capture_output=True, text=True
    ).stdout
    pages = next(line.split(":", 1)[1] for line in info.splitlines()
                 if line.startswith("Pages:"))
    if int(pages) != paper["pages"]:
        raise ValueError(f"{path.name}: page count mismatch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true")
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    manifest = json.loads((directory / "manifest.json").read_text())
    for paper in manifest["papers"]:
        path = directory / paper["file"]
        if not path.exists() and args.fetch:
            with tempfile.TemporaryDirectory(dir=directory) as temporary:
                download = Path(temporary) / path.name
                subprocess.run(
                    ["curl", "--fail", "--location", "--silent", "--show-error",
                     "--max-time", "60", paper["download_url"], "--output", str(download)],
                    check=True,
                )
                verify(download, paper)
                download.rename(path)
        verify(path, paper)
        print(f"OK {path.name}: SHA-256 and {paper['pages']} pages")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
