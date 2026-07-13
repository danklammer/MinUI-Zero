#!/bin/sh
# Build + run the framering harness. Modes: plain (default) | tsan | asan.
# TSan and ASan are SEPARATE builds per the threading v2.4 contract.
set -e
cd "$(dirname "$0")"
MODE="${1:-plain}"
SECS="${2:-}"
CC="${CC:-cc}"
OUT="build/framering_test_$MODE"
mkdir -p build

case "$MODE" in
	plain) FLAGS="-O2";                       SECS="${SECS:-5}"  ;;
	tsan)  FLAGS="-fsanitize=thread  -O1 -g"; SECS="${SECS:-30}" ;;
	asan)  FLAGS="-fsanitize=address -O1 -g"; SECS="${SECS:-30}" ;;
	*) echo "usage: $0 [plain|tsan|asan] [seconds]"; exit 2 ;;
esac

# tiny rings on purpose: pressure exercises full-waits, ABORTING drops, and the
# command-overflow ordering path (unreachable at shipping capacities)
$CC -std=c11 -Wall -Wextra -Werror -DFR_STREAM_CAP=8 -DFR_SVC_STREAM=4 \
    $FLAGS -o "$OUT" framering.c framering_test.c -lpthread
"$OUT" "$SECS" ${3:+$3}
echo "== framering $MODE: PASS =="
