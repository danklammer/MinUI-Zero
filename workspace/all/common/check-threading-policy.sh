#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
PAKS="$ROOT/skeleton/SYSTEM/tg5040/paks/Emus"
PS="$PAKS/PS.pak/launch.sh"
MINARCH_MK="$ROOT/workspace/all/minarch/makefile"

found=$(find "$PAKS" -name launch.sh -type f -exec grep -l '^[[:space:]]*export ZERO_FTV2_DEPTH=' {} + | LC_ALL=C sort)
if [ "$found" != "$PS" ]; then
	echo "threading policy: ZERO_FTV2_DEPTH must be exported only by PS.pak" >&2
	printf 'found:\n%s\n' "$found" >&2
	exit 1
fi

grep -Fq 'export ZERO_FTV2_DEPTH="${ZERO_FTV2_DEPTH:-2}"' "$PS" || {
	echo "threading policy: PS.pak must default depth two while retaining the depth-one override" >&2
	exit 1
}

grep -Fq 'CFLAGS  += -DZERO_DISABLE_FRONTEND_THREADING' "$MINARCH_MK" || {
	echo "threading policy: retired mailbox threading is no longer compile-disabled on tg5040" >&2
	exit 1
}

grep -Fq 'ZERO_FRONTEND_THREADING_V2 ?= 1' "$MINARCH_MK" || {
	echo "threading policy: v2 shipping guard is not enabled for the tg5040 build" >&2
	exit 1
}

echo "== threading policy: PASS (PS depth-2 only; SFC/SUPA serial; legacy mailbox disabled) =="
