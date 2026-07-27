#!/bin/sh

EMU_EXE=gpsp

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# Closed-loop governor clock bracket (kHz). OPPs: 400/600/800/1000/1100/1200 (measured).
#
# THIS BRACKET IS NOT MEASURED FOR THIS SYSTEM. The header here used to read "MEASURED on this SoC"
# and cite "GBC held 59.7fps at 400; supafaust saturated at 1200" — BOTH of which have since been
# retracted by direct measurement on this device:
#   * the 400MHz GBC datapoint was disproven after the -O3 core rebuild (GBC.pak has the numbers,
#     and its floor was raised to 800);
#   * supafaust does NOT saturate at 1200 (SUPA.pak: "at 1200 it never missed").
# Leaving a retracted claim under a MEASURED banner is exactly what the project rule forbids, so it
# is labelled honestly instead: the values below are a conservative bracket, not a finding.
# TODO: bench this system and set the floor from data.
export MINARCH_FMIN=600000
export MINARCH_FMAX=1200000

# Runs at nice 0 (no `nice`), matching tg5040. See docs/DECISIONS.md D61.
# The audio shim is applied to THIS process only, never exported to the helpers above.
LD_PRELOAD="$MINARCH_PRELOAD" minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
