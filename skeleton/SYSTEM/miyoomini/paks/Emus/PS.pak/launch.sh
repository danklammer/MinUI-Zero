#!/bin/sh

EMU_EXE=pcsx_rearmed

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
# 128MB device: PS1 is the one core we ship that will OOM without swap. needs-swap
# creates/enables the swapfile and marks /tmp/using-swap; MinUI.pak/launch.sh swapoffs
# it when the game exits, so the file is only active while it is actually needed.
needs-swap
cd "$HOME"
# closed-loop governor clock bracket (kHz) — MEASURED on this SoC
# OPPs: 400/600/800/1000/1100/1200. GBC held 59.7fps at 400; supafaust saturated at 1200.
export MINARCH_FMIN=1000000
export MINARCH_FMAX=1200000

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
