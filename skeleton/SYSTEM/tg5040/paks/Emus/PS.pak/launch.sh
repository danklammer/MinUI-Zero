#!/bin/sh

EMU_EXE=pcsx_rearmed

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# Threading v2 depth-2 is the big PS1 win (BR2: 60fps/clean @ ~1608 vs serial's 51fps/choppy pegged
# at 1800, no overclock — DECISIONS D52-D61). It runs emulation and present on separate cores.
# DEFAULT OFF pending a KNOWN safety fix: at depth-2 the sleep-autosave and Save/Load/Reset shortcuts
# still enter the core on MAIN (core.serialize/unserialize/reset) while the CORE thread runs core.run
# — a state-corruption race on sleep. They must first park (fc_park) or route through fc_menu_op, and
# that must be validated on-device. Until then depth-2 is opt-in only (set ZERO_FTV2_DEPTH=2, and do
# NOT sleep / save / load / reset mid-game). Re-flip this default to 2 once the parking fix ships.
export ZERO_FTV2_DEPTH="${ZERO_FTV2_DEPTH:-1}"

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
