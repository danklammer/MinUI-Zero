#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OUT="${TMPDIR:-/tmp}/minui-zero-ff-audio-test"
VIS_OUT="${TMPDIR:-/tmp}/minui-zero-ff-visual-test"
CC=${CC:-cc}

"$CC" -std=c99 -O2 -Wall -Wextra -Werror \
	"$ROOT/ff_audio_rate_test.c" -o "$OUT"
"$OUT"
"$CC" -std=c99 -O2 -Wall -Wextra -Werror \
	"$ROOT/ff_visual_cadence_test.c" -o "$VIS_OUT"
"$VIS_OUT"
rm -f "$OUT"
rm -f "$VIS_OUT"
