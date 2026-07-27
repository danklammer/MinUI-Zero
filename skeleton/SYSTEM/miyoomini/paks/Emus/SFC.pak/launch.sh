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
EMU_EXE=snes9x2005_plus

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# Closed-loop governor clock bracket (kHz). OPPs on this SoC: 400/600/800/1000/1100/1200.
#
# This pak is the LIGHT SNES option: snes9x2005_plus, matching tg5040's SFC.pak. SUPA.pak is the
# accurate one (mednafen_supafaust) and is what the base romset ships as the SNES folder — SFC is
# the alternative for people who want speed over accuracy. Keeping the two platforms on the same
# core per pak means a verdict measured on one is meaningful on the other.
#
# This briefly loaded supafaust, purely because snes9x2005_plus was not built for this platform.
# It is now (the core set matches tg5040), so the workaround is gone.
#
# FMIN=600000 and the real floor is UNMEASURED for this core here. snes9x2005_plus is far lighter
# than supafaust — which needs 1000 on this SoC (SUPA.pak has the numbers) — so 600 is plausible,
# and the governor will settle wherever it actually holds inside this bracket. Do not copy SUPA's
# 1000 here: that number belongs to a different, much heavier core.
# TODO: bench an SNES title on this core and set the floor from data.
export MINARCH_FMIN=600000
export MINARCH_FMAX=1200000

# NOTE: ZERO_MEASURE was exported here to compare cores on numbers; a debug facility that writes to
# the SD card once a second must not ship. Re-export by hand to re-tune.
#
# The old text here claimed "the choice is settled (supafaust - it holds 60fps where
# snes9x2005_plus did not)", which contradicted this very file: line 15 loads snes9x2005_plus. That
# comparison was made while snes9x2005_plus was not even BUILT for this platform, so SFC had been
# pointed at supafaust as a stand-in. Both cores ship now and the paks match tg5040: SFC = the light
# core, SUPA = the accurate one (and SUPA is what the base romset uses).

# No `nice`. This said `nice -20`, which is the obsolescent INCREMENT form: it means +20 and
# clamps to 19 — the LOWEST priority on the system — not the -20 boost it reads like. Verified on
# device: `nice -20` -> 19, `nice -n -20` -> -20. So every emulator ran below audioserver and
# keymon (both 0), which is worst exactly where this fork operates: the MMP has no schedutil, so
# the governor drives the clock near saturation and a de-prioritised emulator loses timeslices
# there. tg5040 uses no nice at all and runs at 0; match it rather than invent a boost that could
# starve the audio daemon feeding the DAC.
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
