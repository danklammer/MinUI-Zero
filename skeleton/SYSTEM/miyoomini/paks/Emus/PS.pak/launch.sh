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
# MEASURED for this system (2026-07-27 autotest, attract+demo incl. the BR2 480i ranking screen):
# at the 1200 stock ceiling BR2 misses budget on 16.4% of frames (p95 18.5-20.5ms vs 16.7ms,
# 106 underruns/4min) and THPS on 18.1% (p95 20.4-20.8ms). Holding rate needs ~1.5GHz.
export MINARCH_FMIN=1000000
export MINARCH_FMAX=1200000

# NO overclock, and that is a MEASURED verdict, not the old blanket rule (which was amended
# 2026-07-28 precisely to allow one here if it delivered). It does not deliver:
#
#   THPS p95 typical   1200 stock: 20.4ms   1488 governor-armed: 20.3ms   1488 PIN-VERIFIED
#   (PLL register read mid-run) for the whole bench: 20.4ms — a +24% CPU clock moved it 0%.
#   BR2 typical improved ~12% but its heavy scenes (the 480i ranking screen) stayed flat.
#
# PS1 here is NOT CPU-clock-bound: the wall is memory traffic (software GPU rasterization +
# blit), the same rail the port recon found dominating power. The "needs ~1.5GHz" arithmetic
# assumed work scales with clock; the direct A/B refuted it. The MINARCH_OC_KHZ mechanism
# stays available (platform.c) for any future pak WITH a receipt; this pak earned none.
# If PS1 quality is pursued further, the lever is the render/blit path, not the clock.

# Runs at nice 0 (no `nice`), matching tg5040. See docs/DECISIONS.md D61.
# The audio shim is applied to THIS process only, never exported to the helpers above.
LD_PRELOAD="$MINARCH_PRELOAD" minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
