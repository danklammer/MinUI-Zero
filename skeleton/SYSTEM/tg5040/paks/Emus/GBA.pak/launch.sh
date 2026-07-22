#!/bin/sh

EMU_EXE=gpsp

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
# Present-skip default-on: byte-identical frames skip GLES upload/submit/swap so the CPU
# idles sooner. Static/paused scenes win big (SP Metroid 44%-dup = -32% CPU), 0% dup =
# break-even; qualified both devices/both cores, 0 underrun regression. Gate is presence-only
# (getenv!=NULL) — remove this prefix to disable; =0 would NOT disable.
ZERO_DUP_SKIP=1 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
