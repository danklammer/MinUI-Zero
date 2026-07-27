#!/bin/sh

EMU_EXE=gambatte

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz) — MEASURED on this SoC.
# OPPs: 400/600/800/1000/1100/1200.
#
# FLOOR RAISED 400000 -> 800000 (2026-07-25). The old 400 floor was never really exercised: the
# foreign prebuilt gambatte was slow enough that the governor never descended past 1000, so a
# floor it could not hold sat there unnoticed. Rebuilding the core from pinned source at -O3 made
# it cheap enough to actually reach the bottom of the bracket, and 400 does not hold GBC.
#
# MEASURED (bench.sh, 25s, Bionic Commando - Elite Forces, 2 runs per config):
#   floor 400: avg 58.4  min 27.8  below-target 2   (sank to 400, stalled, recovered)
#   floor 600: avg 59.1  min 38.3  below-target 1
#   floor 800: avg 59.8  min 48.5  below-target 1   <- ships
#   foreign core, floor 400: avg 59.9  min 55.3  below-target 2  (never went below 1000)
# At 800 we match the old frame rate with FEWER below-target samples while running at 800 where
# the foreign core needed 1000+ — same result, lower clock, which is the whole point.
export MINARCH_FMIN=800000
export MINARCH_FMAX=1200000

# No `nice`. This said `nice -20`, which is the obsolescent INCREMENT form: it means +20 and
# clamps to 19 — the LOWEST priority on the system — not the -20 boost it reads like. Verified on
# device: `nice -20` -> 19, `nice -n -20` -> -20. So every emulator ran below audioserver and
# keymon (both 0), which is worst exactly where this fork operates: the MMP has no schedutil, so
# the governor drives the clock near saturation and a de-prioritised emulator loses timeslices
# there. tg5040 uses no nice at all and runs at 0; match it rather than invent a boost that could
# starve the audio daemon feeding the DAC.
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
