#!/bin/sh

EMU_EXE=gambatte

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz) — MEASURED on this SoC.
# OPPs: 400/600/800/1000/1100/1200.
#
# UNVERIFIED FLOOR — deliberately left at 400000. GBC (same gambatte core) was measured to NOT
# hold at 400 once the core was rebuilt at -O3, and its floor was raised to 800; see the numbers
# in GBC.pak/launch.sh. DMG is strictly lighter than CGB, so 400 may well be fine here — but
# there is no plain Game Boy folder on the test card, so this was never actually run. Not changed
# on inference: this project does not ship unmeasured clock values.
# TODO: bench a DMG title and set this floor from data.
export MINARCH_FMIN=400000
export MINARCH_FMAX=1200000

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
