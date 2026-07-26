#!/bin/sh

# SNES core. BOTH are shipped in cores/, so this is a one-line switch:
#   mednafen_supafaust  — accuracy-focused (Mednafen lineage). Better audio/timing fidelity.
#   snes9x2005_plus     — lightweight, what upstream miyoomini and our tg5040 ship.
#
# Set to supafaust by preference. Whether this SoC (dual Cortex-A7 @1200MHz max) can actually hold
# 60fps on it is MEASURED, not assumed — ZERO_MEASURE below logs fps/cpu/clock once a second so the
# choice is evidence-based. An earlier claim here that supafaust "saturates this SoC" was an
# inference from a slowdown report, made while several unrelated bugs were live. It was never
# measured.
#
# NOTE: save states are CORE-SPECIFIC. Switching cores orphans existing SNES save states
# (in-game saves / SRAM are fine).
EMU_EXE=mednafen_supafaust

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz). OPPs on this SoC: 400/600/800/1000/1100/1200.
# NOTE: the old "SNES saturates at 1200" note here was measured with mednafen_supafaust, which is
# no longer the core for this system. snes9x2005_plus is far lighter and the real floor is
# UNMEASURED — the governor will find it inside this bracket. Re-measure and lower FMIN if it
# settles well below 600.
export MINARCH_FMIN=600000
export MINARCH_FMAX=1200000

# NOTE: ZERO_MEASURE was exported here to judge supafaust vs snes9x2005_plus on numbers. The
# choice is settled (supafaust — it holds 60fps where snes9x2005_plus did not), and a debug
# facility that writes to the SD card once a second must not ship. Re-export by hand to re-tune.

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
