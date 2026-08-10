#!/bin/sh
# Input — shows what the frontend sees for every button, the fastest way to prove a mapping.
# Especially useful on this platform: h700 reads raw evdev (platform.c PLAT_pollInput) with a
# receipts-based code table, and MENU emits a two-code sequence (312 + a 354 release pulse).
cd "$(dirname "$0")"
./minput.elf
