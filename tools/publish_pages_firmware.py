#!/usr/bin/env python3
"""Stage a release's flashable images into docs/firmware/ for the browser flasher.

Why this exists
---------------
The flasher used to fetch firmware straight from the GitHub Release via each
asset's ``browser_download_url``. A browser cannot do that: those URLs 302 to
``release-assets.githubusercontent.com``, and neither the redirect nor the final
response carries ``Access-Control-Allow-Origin``, so every fetch fails CORS.
``api.github.com`` *is* CORS-enabled, which is what made the bug so confusing --
listing releases worked and only the downloads failed.

Publishing the same images under ``docs/`` puts them on the very origin that
serves the flasher, so CORS never enters into it.

What it writes
--------------
``docs/firmware/<tag>/``          every image plus manifest.json / flasher_args-*.json
``docs/firmware/index.json``      the release list the flasher's picker reads

``index.json`` carries an explicit ``files`` array per release so the page never
needs a directory listing (GitHub Pages does not provide one), and so the
existing ``rel.assets`` map can be built exactly as it was from the API.

Usage:  publish_pages_firmware.py <tag> <release-dir> <repo-root>
"""
from __future__ import annotations

import json
import os
import re
import shutil
import sys
from datetime import datetime, timezone

# Images and the metadata the flasher reads. The host desktop binary,
# partitions.csv and SHA256SUMS stay Release-only: the flasher never fetches
# them, and Pages is not a general file host.
KEEP = re.compile(r"(\.bin$)|(^manifest\.json$)|(^flasher_args-.+\.json$)")


def stage(tag: str, release_dir: str, repo_root: str) -> str:
    out_dir = os.path.join(repo_root, "docs", "firmware", tag)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)          # re-runnable: a re-tag republishes cleanly
    os.makedirs(out_dir, exist_ok=True)

    copied = []
    for name in sorted(os.listdir(release_dir)):
        src = os.path.join(release_dir, name)
        if not os.path.isfile(src) or not KEEP.search(name):
            continue
        shutil.copy2(src, os.path.join(out_dir, name))
        copied.append({"name": name, "size": os.path.getsize(src)})

    if not copied:
        sys.exit("no publishable files found in %s" % release_dir)
    if not any(f["name"] == "manifest.json" for f in copied):
        sys.exit("refusing to publish %s: no manifest.json" % tag)

    print("staged %d files into docs/firmware/%s" % (len(copied), tag))
    return out_dir


def reindex(repo_root: str, tag: str, prerelease: bool) -> None:
    root = os.path.join(repo_root, "docs", "firmware")
    index_path = os.path.join(root, "index.json")

    # Preserve what earlier runs recorded (dates, prerelease flags) so a
    # re-index never invents metadata it cannot see.
    known = {}
    if os.path.isfile(index_path):
        try:
            with open(index_path, encoding="utf-8") as f:
                for r in json.load(f).get("releases", []):
                    known[r["tag"]] = r
        except (OSError, ValueError, KeyError):
            pass

    releases = []
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        if not os.path.isdir(d) or not os.path.isfile(os.path.join(d, "manifest.json")):
            continue
        files = [
            {"name": f, "size": os.path.getsize(os.path.join(d, f))}
            for f in sorted(os.listdir(d))
            if os.path.isfile(os.path.join(d, f))
        ]
        prev = known.get(name, {})
        releases.append({
            "tag": name,
            "prerelease": prerelease if name == tag else bool(prev.get("prerelease", False)),
            "date": (datetime.now(timezone.utc).strftime("%Y-%m-%d")
                     if name == tag else prev.get("date", "")),
            "files": files,
        })

    # Newest first, the order the picker shows and the order it defaults from.
    releases.sort(key=lambda r: r["date"], reverse=True)

    with open(index_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"releases": releases}, f, indent=2)
        f.write("\n")
    print("index.json now lists: %s" % ", ".join(r["tag"] for r in releases))


def main() -> None:
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    tag, release_dir, repo_root = sys.argv[1], sys.argv[2], sys.argv[3]
    prerelease = "-" in tag          # v1.3.0-pre-alpha and friends
    stage(tag, release_dir, repo_root)
    reindex(repo_root, tag, prerelease)


if __name__ == "__main__":
    main()
