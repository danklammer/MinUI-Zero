#!/bin/sh

EMU_EXE=mednafen_pce_fast
CORES_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz); see docs/thermal-governor-design.md
export MINARCH_FMIN=1008000
export MINARCH_FMAX=1008000
# NOTE: no ring override here — mednafen_pce_fast also runs PCE-CD/CHD content whose
# sector reads can stall production far longer than a HuCard fsync; 100ms needs
# separate CD-content device receipts before it ships (Codex review finding 4)
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
