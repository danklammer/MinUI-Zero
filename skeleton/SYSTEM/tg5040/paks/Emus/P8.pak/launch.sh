#!/bin/sh

EMU_EXE=fake08
CORES_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz) — STARTER (roaming); validate + lock on-device.
# PICO-8 carts run at 30 or 60fps (_update vs _update60) and the Lua-VM cost varies per cart,
# so let the governor find the floor until per-cart receipts justify a fixed ceiling.
export MINARCH_FMIN=600000
export MINARCH_FMAX=1416000
# Present-skip default-on (MinUI Zero's value-add — NextUI/native run PICO-8 without it): PICO-8
# is full of static content (title cards, menus, turn-based carts), so it benefits. Gate is
# presence-only (getenv!=NULL) — remove this prefix to disable; =0 would NOT disable.
ZERO_DUP_SKIP=1 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
