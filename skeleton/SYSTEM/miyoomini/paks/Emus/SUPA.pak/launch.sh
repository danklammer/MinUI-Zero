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
# Closed-loop governor clock bracket (kHz). OPPs on this SoC: 400/600/800/1000/1100/1200.
# 1200 is the top STOCK OPP — never above.
#
# FMIN=1000000, and that number is MEASURED, not guessed. With FMIN at 600 the governor sat in a
# permanent limit cycle: it would sink 800->600 while the game was healthy, the frame rate would
# collapse, it would panic back to 1200, recover, and probe 600 again — endlessly. Sampled fps
# against a 60.1 target, this core (mednafen_supafaust, 292x224):
#     ceil 1200 -> 59.6-59.7   holds
#     ceil 1000 -> 59.6-59.7   holds
#     ceil  800 -> 51.7-59.7   marginal
#     ceil  600 -> 29.8-49.7   cannot run the game
# So 600 is below this core's floor and probing it is pure stutter. 1000 is the lowest clock that
# holds rate with margin. Supafaust itself is NOT too heavy for this SoC — at 1200 it never missed.
export MINARCH_FMIN=1000000
export MINARCH_FMAX=1200000

# TEMPORARY (bench): logs "MEASURE ... fps=actual/target cpu=.. ceil=..MHz" once a second so the
# core/clock choice gets settled on numbers. REMOVE once tuning is done — it is one SD write/sec.
export ZERO_MEASURE=1

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
