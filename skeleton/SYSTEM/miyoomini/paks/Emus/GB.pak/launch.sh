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
# FMIN=800000 — matched to GBC, the only MEASURED datapoint for this core.
#
# This previously shipped 400 while its own comment said the value was unverified and that "this
# project does not ship unmeasured clock values". Both cannot be true: 400 was itself the
# unmeasured value. GBC runs the SAME gambatte core and was measured to NOT hold 400 once the core
# was rebuilt at -O3 (avg 58.4, min 27.8, floor raised to 800 — numbers in GBC.pak/launch.sh).
#
# DMG is strictly lighter than CGB, so a lower floor is plausible — but FMIN is a FLOOR the
# governor actively sinks into, and one the core cannot hold produces a collapse/panic limit cycle
# that costs both frame rate and power. Inheriting the measured sibling value is the conservative
# error; guessing downward is not.
# TODO: bench a DMG title and lower this from data. There is still no plain Game Boy folder on the
# test card, so this remains unmeasured on real DMG content.
export MINARCH_FMIN=800000
export MINARCH_FMAX=1200000

# Runs at nice 0 (no `nice`), matching tg5040. See docs/DECISIONS.md D61.
# The audio shim is applied to THIS process only, never exported to the helpers above.
LD_PRELOAD="$MINARCH_PRELOAD" minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
