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
# Closed-loop governor clock bracket (kHz). OPPs on this SoC: 400/600/800/1000/1100/1200.
#
# FMIN=1000000 — the SAME floor as SUPA.pak, because this pak loads the SAME core.
#
# The note that used to sit here justified 600 by saying the core was snes9x2005_plus. That was a
# stale copy from the tg5040 pak: line 15 loads mednafen_supafaust, and snes9x2005_plus is not
# even built for this platform (see CORES in workspace/miyoomini/cores/makefile). So the floor was
# defended by evidence about a core that does not exist here.
#
# The real measurement, from SUPA.pak against a 60.1 target with this core at 292x224:
#     ceil 1200 -> 59.6-59.7   holds
#     ceil 1000 -> 59.6-59.7   holds
#     ceil  800 -> 51.7-59.7   marginal
#     ceil  600 -> 29.8-49.7   cannot run the game
# A floor the core cannot hold is not a saving: the governor sinks into it, the frame rate
# collapses, it panics back to 1200 and probes again — a permanent limit cycle that is both
# slower AND less efficient than simply not going there.
export MINARCH_FMIN=1000000
export MINARCH_FMAX=1200000

# NOTE: ZERO_MEASURE was exported here to judge supafaust vs snes9x2005_plus on numbers. The
# choice is settled (supafaust — it holds 60fps where snes9x2005_plus did not), and a debug
# facility that writes to the SD card once a second must not ship. Re-export by hand to re-tune.

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
