#!/bin/sh
# Files — DinguxCommander, the same aarch64 build the Brick ships. Verified on-device 2026-08-10:
# every library it links resolves against the lean h700 rootfs (the strip removed nothing it needs).
cd "$(dirname "$0")"
HOME="$SDCARD_PATH"
./DinguxCommander
