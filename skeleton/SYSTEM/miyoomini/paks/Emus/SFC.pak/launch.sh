#!/bin/sh

# snes9x2005_plus, same as upstream miyoomini and our own tg5040. It is the LIGHTWEIGHT SNES core
# and it is the right one for a dual-core A7 at 1200MHz.
# Do NOT switch this to mednafen_supafaust: supafaust is an accuracy core, it saturates this SoC,
# and SNES runs visibly slow on it (device-reported). It was briefly used here only because
# snes9x2005_plus had not been staged into cores/ — the fix was to build the core, not to
# substitute a heavier one.
EMU_EXE=snes9x2005_plus

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

nice -20 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
