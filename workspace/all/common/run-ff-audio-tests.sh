#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OUT="${TMPDIR:-/tmp}/minui-zero-ff-audio-test"
CC=${CC:-cc}

"$CC" -std=c99 -O2 -Wall -Wextra -Werror \
	"$ROOT/ff_audio_rate_test.c" -o "$OUT"
"$OUT"
rm -f "$OUT"
