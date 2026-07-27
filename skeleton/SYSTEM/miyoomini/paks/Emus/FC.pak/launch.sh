#!/bin/sh

EMU_EXE=fceumm

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# Closed-loop governor clock bracket (kHz). OPPs: 400/600/800/1000/1100/1200.
#
# FMIN=600000, MEASURED on this device with fceumm (Contra Force, 25s per clock, generated fps
# against a 60.1 target):
#     400 MHz  avg 57.7  min 55.2  below-59fps 15/22   <- CANNOT hold rate
#     600 MHz  avg 74.2  min 72.1  below-59fps  0/24   <- holds with ~20% headroom
#     800 MHz  avg 86.5  min 85.3  below-59fps  0/24
#    1000 MHz  avg 95.7  min 92.8  below-59fps  0/24
#
# This shipped at 400 on the strength of a comment reading "GBC held 59.7fps at 400" — a claim that
# was later DISPROVEN for GBC itself (re-measured after the -O3 core rebuild and raised to 800), so
# NES was resting on evidence that had already been retracted.
#
# WHAT THIS DOES NOT FIX, honestly recorded: raising this floor was NOT the cause of the jittery
# scrolling reported in Contra, and did not cure it. The governor memory sidecar for that game reads
# 800000 — the closed loop had ALREADY settled Contra at 800MHz and never sank to 400. The numbers
# above were taken with the clock PINNED (FMIN=FMAX), which answers "can 400 run NES" (no), not
# "does the governor choose 400" (it does not). The jitter was a scaling artifact; see default.cfg.
#
# So this is a GUARD RAIL, not a fix: it keeps a clock the core provably cannot hold out of the
# search space, so the loop cannot spend a probe (and a stutter) discovering that again.
#
# 600 rather than 800: 600 already clears the target with margin, and this is the lightest system we
# ship — taking the extra OPP step would cost power for nothing, which is the opposite of the point.
export MINARCH_FMIN=600000
export MINARCH_FMAX=1200000

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
