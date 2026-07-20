#!/bin/sh

EMU_EXE=pcsx_rearmed

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# Threading v2 depth-2 is enabled only for PS1. The frontend starts a CORE producer and keeps
# scaler/GLES presentation on MAIN; every other system stays on the plain serial path. A per-game
# crash canary automatically holds a game in serial until it completes one known-good session.
# ZERO_FTV2_DEPTH=1 remains the diagnostic override.
export ZERO_FTV2_DEPTH="${ZERO_FTV2_DEPTH:-2}"

# closed-loop governor clock bracket (kHz); see docs/thermal-governor-design.md
# Keep the verified-stock 1.8GHz safety ceiling. Threading earns lower clocks through the
# measured governor; a hard 1.608GHz cap based on one title can under-provision another title
# or a serial canary fallback. This remains stock-only and never exposes the 2.0GHz OC OPP.
export MINARCH_FMIN=1008000
export MINARCH_FMAX=1800000
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
