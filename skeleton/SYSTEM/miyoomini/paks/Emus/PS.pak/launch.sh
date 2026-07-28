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

# MPLL overclock opt-in (CLAUDE.md overclock rule as amended 2026-07-28): table-top governor
# requests are served by overclock.elf at this clock; below-top requests take the stock cpufreq
# path (menu dips still cool down, crashes self-heal on the next cpufreq write). 1488 is what
# upstream MinUI shipped as PERFORMANCE on this device; the ~25% lift covers BR2 (needs ~1475)
# and just reaches THPS (~1496). While overclocked, scaling_cur_freq still READS 1200000 — the
# HUD/telemetry clock column under-reports; judge by generation rate and p95.
# THERMAL SOAK PENDING: not yet validated over a long session; remove this line to fall back.
export MINARCH_OC_KHZ=1488000

# Runs at nice 0 (no `nice`), matching tg5040. See docs/DECISIONS.md D61.
# The audio shim is applied to THIS process only, never exported to the helpers above.
LD_PRELOAD="$MINARCH_PRELOAD" minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
