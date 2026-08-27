#!/bin/sh
# h700 SMS pak. Standard MinUI boilerplate: the launcher entry point
# (paks/MinUI.pak/launch.sh, or the OS-side minui-frontend.sh on our image) exports every path,
# PATH, LD_LIBRARY_PATH, the measured panel rate and the ALSA driver, so a pak only names its core.
# Normalized 2026-08-26 from bespoke per-pak scripts that re-exported all of it 15 times over.

EMU_EXE=picodrive

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1
