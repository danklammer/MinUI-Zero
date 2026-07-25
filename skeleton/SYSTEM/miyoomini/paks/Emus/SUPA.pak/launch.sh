#!/bin/sh

# SNES via mednafen_supafaust. This pak exists because ROM folders tagged "(SUPA)" — the naming
# MyMinUI used — resolve to SUPA.pak, not SFC.pak. Without it, such folders fall through to a
# leftover user pak.
#
# Do NOT reintroduce MyMinUI's launch path here (launch_rom.sh + `overclock.elf $CPU_OC`):
# overclock.elf pokes the SigmaStar MPLL directly through /dev/mem, which cpufreq cannot observe
# or undo, so it fights the closed-loop governor and the MPLL write silently wins. That is both a
# performance bug and a violation of the never-overclock rule.
EMU_EXE=mednafen_supafaust

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz). OPPs on this SoC: 400/600/800/1000/1100/1200.
# 1200 is the top STOCK OPP — never above.
export MINARCH_FMIN=600000
export MINARCH_FMAX=1200000

# TEMPORARY (bench): logs "MEASURE ... fps=actual/target cpu=.. ceil=..MHz" once a second so the
# core/clock choice gets settled on numbers. REMOVE once tuning is done — it is one SD write/sec.
export ZERO_MEASURE=1

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
