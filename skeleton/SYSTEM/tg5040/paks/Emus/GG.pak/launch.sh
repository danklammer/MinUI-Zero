#!/bin/sh

EMU_EXE=picodrive

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
# NOTE: MINARCH_SND_RING_MS=100 was measured a NET LOSS here (Bionic 4-cell, both
# devices: skip-on underruns 0-1 at the default ring vs 75-87 at 100ms) — the
# override mechanism remains for future per-system receipts; the default stays.
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
