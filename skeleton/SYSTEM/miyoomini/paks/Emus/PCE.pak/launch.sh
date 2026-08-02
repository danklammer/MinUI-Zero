#!/bin/sh

EMU_EXE=mednafen_pce_fast

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# Closed-loop governor clock bracket (kHz). OPPs: 400/600/800/1000/1100/1200 (measured).
# THIS BRACKET IS NOT MEASURED FOR THIS SYSTEM — it is a conservative starter, not a finding.
# PCE-CD/CHD sector reads can stall production; start higher than the 8-bit paks.
# TODO: bench this system on-device and set the floor from data.
export MINARCH_FMIN=800000
export MINARCH_FMAX=1200000

# Runs at nice 0 (no `nice`), matching tg5040. See docs/DECISIONS.md D61.
# The audio shim is applied to THIS process only, never exported to the helpers above.
LD_PRELOAD="$MINARCH_PRELOAD" minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
