#!/bin/sh

EMU_EXE=fceumm

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz); see docs/thermal-governor-design.md
# floor 600, NOT 408: fceumm at 408 sits right at the boundary and the governor limit-cycles
# (408 overrun -> 624 clean -> sink -> overrun...) = periodic slowdown bursts (Contra, 2026-07-01)
export MINARCH_FMIN=600000
export MINARCH_FMAX=1008000
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
