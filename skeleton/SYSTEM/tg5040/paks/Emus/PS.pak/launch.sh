#!/bin/sh

EMU_EXE=pcsx_rearmed

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# Threading v2 depth-2 ON by default for PS1: emulation and present run on separate cores, so the
# hardest PS1 games hold 60 at stock where serial can't. PROVEN on BR2 (Bloody Roar II, the hardest
# case): depth-2 = 60fps/clean @ ~1608 vs serial = 51fps/choppy pegged at 1800 — no overclock
# (DECISIONS D52-D61, 2026-07-15). The engine is zero-overhead unless depth-2 is set, so only PS1
# pays for it. Fall back to serial with ZERO_FTV2_DEPTH=1 if a specific game misbehaves.
export ZERO_FTV2_DEPTH="${ZERO_FTV2_DEPTH:-2}"

# closed-loop governor clock bracket (kHz); see docs/thermal-governor-design.md
if [ "${ZERO_FTV2_DEPTH:-1}" -ge 2 ] 2>/dev/null; then
	# Threading v2 depth-2: emulation and present run concurrently, so the CORE holds 60 at a
	# LOWER clock than serial (present is off the critical path). MEASURED on BR2 (hardest PS1
	# case, 2026-07-15): depth-2 holds 60 clean at 1416 (1200 breaks); depth-1 can't hold 60
	# even at 1800. Cap the ceiling at 1608 (proven-sufficient, one OPP above the floor) so the
	# governor holds a steady 1608 on heavy fights instead of hunting up to 1800 (wasted watts +
	# audible glitch); keep f_min low so light/menu/30fps scenes still sink. No OC. (The exact
	# 1416 floor needs f_min=1416, but that pins light PS1 content high -> net loss on RPGs; the
	# dynamic-floor governor refinement is the way to reach 1416 in fights without that penalty.)
	export MINARCH_FMIN=1008000
	export MINARCH_FMAX=1608000
else
	export MINARCH_FMIN=1008000
	export MINARCH_FMAX=1800000
fi
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
