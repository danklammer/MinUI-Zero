#!/bin/sh

EMU_EXE=mgba

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# Closed-loop governor clock bracket (kHz). OPPs: 400/600/800/1000/1100/1200 (measured).
# THIS BRACKET IS NOT MEASURED FOR THIS SYSTEM — it is a conservative starter, not a finding.
# mgba is heavier than gpsp (accuracy core); bracket is a conservative guess.
# TODO: bench this system on-device and set the floor from data.
export MINARCH_FMIN=600000
export MINARCH_FMAX=1200000

# Runs at nice 0 (no `nice`), matching tg5040. See docs/DECISIONS.md D61.
# The audio shim is applied to THIS process only, never exported to the helpers above.
LD_PRELOAD="$MINARCH_PRELOAD" minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
