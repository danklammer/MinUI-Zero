#!/bin/sh

EMU_EXE=fceumm

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"
# closed-loop governor clock bracket (kHz); see docs/thermal-governor-design.md
# A low scaling_max_freq starves short GLES present bursts even when average CPU work
# fits. Keep the verified-stock 1008 ceiling; schedutil still idles at 408 (D61).
export MINARCH_FMIN=1008000
export MINARCH_FMAX=1008000
# NOTE: MINARCH_SND_RING_MS=100 was measured a NET LOSS here (Bionic 4-cell, both
# devices: skip-on underruns 0-1 at the default ring vs 75-87 at 100ms) — the
# override mechanism remains for future per-system receipts; the default stays.
# Present-skip default-on (v1.5.2): byte-identical frames skip GLES upload/submit/swap so the
# CPU idles sooner. Static/menu scenes win big (NES ~-42% CPU on Tetris), motion is break-even;
# qualified on both devices, real-clock audio clean. Gate is presence-only (getenv!=NULL) —
# remove this prefix to disable; =0 would NOT disable.
ZERO_DUP_SKIP=1 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt"
