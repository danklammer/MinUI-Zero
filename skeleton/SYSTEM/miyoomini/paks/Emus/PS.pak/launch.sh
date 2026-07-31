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
# Closed-loop governor clock bracket (kHz). OPPs: 400/600/800/1000/1100/1200 (measured).
#
# MEASURED for this system (2026-07-28, generation-rate ground truth): THPS generates 59.5fps
# against a 59.94 target and BR2 60.25 against 60 at this bracket — PS1 RUNS AT FULL SPEED here.
export MINARCH_FMIN=1000000
export MINARCH_FMAX=1200000

# NO overclock — and read this before "fixing" PS1 performance here at all. The full story:
#
# A telemetry artifact painted 1-in-6 frames as over-budget (p95 ~20.4ms vs the 16.7ms budget),
# which read as "needs ~1.5GHz". Six successive A/B knobs failed to move that number — including
# an MPLL overclock PIN-VERIFIED at 1488MHz for a whole bench (+24% clock, 0% change), dithering
# off, GPU/SPU threads, and CD read-ahead 1024. A number immune to EVERYTHING is not a workload;
# it was the audio-pacing wall: work_us included ring-full blocking (83% of frames block — that
# is what paces a full-speed game) and the ms-rounded pace subtraction skipped itself whenever
# rounding pushed the estimate past the raw window. Fixed in minarch (us-precision, clamped);
# generation rate is the ground truth that exposed it.
#
# BR2 chop is REAL and is a device limit, not a missing tweak. Everything obvious is refuted
# (all MMP/BR2, 4 min or 2 min runs, generation pinned at 60.00 fps in every single arm):
#   CPU clock      pin-verified 1488MHz MPLL ................ 0% change (memory-bound, not clock)
#   presentation-drop OFF ..................... 64 -> 1581 underruns (it is PROTECTING; keep it)
#   audio ring 150ms / 250ms ...... 58 / 28 underruns-per-min vs 15.9 at the default (both worse,
#                                   matching the tg5040 receipt that a bigger ring is a net loss)
#   tg5040's 480i minimal-prescale fix ......... does not apply (that feeds the Crisp render-target
#                                   path; MMP locks sharpness and PLAT_setSharpness is a no-op)
# What is left: the device cannot both present every frame AND keep BR2's audio ring fed, so the
# drop mechanism trades visible stutter for audible crackle — correctly. Locating the remaining
# cost needs flip-wait in the telemetry CSV (not yet instrumented); until then this is an honest
# alpha limitation, not a TODO anyone should re-chase with the knobs above.
# The MINARCH_OC_KHZ mechanism remains available (platform.c) for any pak WITH a real receipt.

# Runs at nice 0 (no `nice`), matching tg5040. See docs/DECISIONS.md D61.
# The audio shim is applied to THIS process only, never exported to the helpers above.
LD_PRELOAD="$MINARCH_PRELOAD" minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
