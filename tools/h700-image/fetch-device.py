#!/usr/bin/env python3
"""Fetch one device's definition from muOS (MustardOS/internal) into a local folder.

muOS keeps every supported handheld in one tree, device/<name>/{config,control,package}, and picks
ONE at image-build time; nothing installs a device package at runtime (verified against the shipped
scripts). So building for a different board means fetching that board's folder and using it, which is
exactly what this does.

  config/   board settings the muOS scripts read via GET_VAR (board/name, board/stick, board/rtc_wake,
            screen geometry, cpu governor paths, ...)
  control/  device state files, notably the alsa mixer baseline restored at boot and after suspend
  package/  the boot chain: boot_package.fex (u-boot + device tree), u-boot.fex, p1.dtbo, sunxi.dts

Usage:  fetch-device.py rg35xx-h  <out-dir>
"""
import json, os, sys, urllib.request, urllib.error

REPO = "MustardOS/internal"
API = f"https://api.github.com/repos/{REPO}/contents"
RAW = f"https://raw.githubusercontent.com/{REPO}/main"


def get_json(url):
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                               "User-Agent": "minui-zero-build"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def fetch_tree(device, out):
    total_files = total_bytes = 0
    stack = [f"device/{device}/{d}" for d in ("config", "control", "package")]
    while stack:
        path = stack.pop()
        try:
            entries = get_json(f"{API}/{path}")
        except urllib.error.HTTPError as e:
            if e.code == 404:
                print(f"  (no {path})")
                continue
            raise
        if isinstance(entries, dict):      # a file, not a directory
            entries = [entries]
        for e in entries:
            if e["type"] == "dir":
                stack.append(e["path"])
                continue
            rel = e["path"].split(f"device/{device}/", 1)[1]
            dest = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with urllib.request.urlopen(f"{RAW}/{e['path']}", timeout=120) as r:
                data = r.read()
            # A silent truncation here would produce a device that boots wrong in ways that look like
            # our bug, so verify the size the API reported.
            if e.get("size") and len(data) != e["size"]:
                sys.exit(f"ERROR: {rel} came back {len(data)}B, expected {e['size']}B")
            open(dest, "wb").write(data)
            total_files += 1
            total_bytes += len(data)
    return total_files, total_bytes


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <device-name> <out-dir>   e.g. rg35xx-h ./assets/device-h")
    device, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    print(f"fetching muOS device definition '{device}' -> {out}")
    n, b = fetch_tree(device, out)
    name_file = os.path.join(out, "config/board/name")
    got = open(name_file).read().strip() if os.path.exists(name_file) else "(missing)"
    print(f"  {n} files, {b/1024:.0f} KB")
    print(f"  config/board/name = {got}")
    if got != device:
        sys.exit(f"ERROR: fetched tree says '{got}' but we asked for '{device}'")
    print("  OK")
