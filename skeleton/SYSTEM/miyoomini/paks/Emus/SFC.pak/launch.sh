#!/bin/sh

# We ship mednafen_supafaust for SNES (the core the on-device measurements used).
# snes9x2005_plus is NOT staged in cores/, so referencing it fails to load the core.
EMU_EXE=mednafen_supafaust

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz) — MEASURED on this SoC
# OPPs: 400/600/800/1000/1100/1200. GBC held 59.7fps at 400; supafaust saturated at 1200.
export MINARCH_FMIN=600000
export MINARCH_FMAX=1200000

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
