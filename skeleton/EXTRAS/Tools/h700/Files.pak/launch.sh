#!/bin/sh
# Files — DinguxCommander, the same aarch64 build the Brick ships. Every library it links resolves
# against the lean h700 rootfs (checked on-device 2026-08-10), including libmali.so, which the
# strip used to delete and which SDL needs because "mali" is this SDL2's only video driver.
#
# UNSET SDL_VIDEODRIVER. The frontend exports SDL_VIDEODRIVER=dummy for MinUI, which is correct
# there (minui and minarch present through the DE layer and use SDL only for plumbing), but any
# real SDL app inherits it and dies: measured on-device, dummy -> segfault, unset -> runs.
# Any third-party SDL pak needs this same line, so it is the first thing to check when a community
# tool "does not open" (Dan 2026-08-11).
unset SDL_VIDEODRIVER

cd "$(dirname "$0")"
HOME="$SDCARD_PATH"
./DinguxCommander
