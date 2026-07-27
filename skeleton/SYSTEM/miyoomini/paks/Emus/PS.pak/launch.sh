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
#
# ABORT if it could not be enabled. Launching anyway means an OOM kill a few seconds later, which
# reads as "PS1 is broken" rather than "the card is full" — needs-swap already showed the reason.
needs-swap || exit 1
cd "$HOME"
# closed-loop governor clock bracket (kHz) — MEASURED on this SoC
# OPPs: 400/600/800/1000/1100/1200. GBC held 59.7fps at 400; supafaust saturated at 1200.
export MINARCH_FMIN=1000000
export MINARCH_FMAX=1200000

# No `nice`. This said `nice -20`, which is the obsolescent INCREMENT form: it means +20 and
# clamps to 19 — the LOWEST priority on the system — not the -20 boost it reads like. Verified on
# device: `nice -20` -> 19, `nice -n -20` -> -20. So every emulator ran below audioserver and
# keymon (both 0), which is worst exactly where this fork operates: the MMP has no schedutil, so
# the governor drives the clock near saturation and a de-prioritised emulator loses timeslices
# there. tg5040 uses no nice at all and runs at 0; match it rather than invent a boost that could
# starve the audio daemon feeding the DAC.
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
