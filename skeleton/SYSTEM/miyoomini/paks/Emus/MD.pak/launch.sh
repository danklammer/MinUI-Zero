#!/bin/sh

EMU_EXE=picodrive

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

# No `nice`. This said `nice -20`, which is the obsolescent INCREMENT form: it means +20 and
# clamps to 19 — the LOWEST priority on the system — not the -20 boost it reads like. Verified on
# device: `nice -20` -> 19, `nice -n -20` -> -20. So every emulator ran below audioserver and
# keymon (both 0), which is worst exactly where this fork operates: the MMP has no schedutil, so
# the governor drives the clock near saturation and a de-prioritised emulator loses timeslices
# there. tg5040 uses no nice at all and runs at 0; match it rather than invent a boost that could
# starve the audio daemon feeding the DAC.
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
