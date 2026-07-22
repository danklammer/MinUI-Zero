#!/bin/sh

EMU_EXE=snes9x2005_plus

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz); see docs/thermal-governor-design.md
export MINARCH_FMIN=600000
export MINARCH_FMAX=1416000
# Present-skip default-on (v1.5.2): byte-identical frames skip GLES upload/submit/swap so the
# CPU idles sooner. Static/menu scenes win, motion is break-even; qualified on both devices,
# real-clock audio clean. Gate is presence-only (getenv!=NULL) — remove this prefix to disable;
# =0 would NOT disable. (SFC = the alternative SNES core; SUPA/supafaust is the default.)
ZERO_DUP_SKIP=1 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
