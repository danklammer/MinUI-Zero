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
# floor 408 (restored): the "fceumm saturates at 408" evidence (D21) was contaminated by the upstream
# sndquality=High default (D25) — at Low, Contra measured 0 overruns with windows at 408 (A/B 2026-07-02)
export MINARCH_FMIN=408000
export MINARCH_FMAX=1008000
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
