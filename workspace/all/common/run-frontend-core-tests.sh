#!/bin/sh
# Build + run the frontend_core lifecycle harness (threading v2 phase 2).
# Modes: plain (default) | tsan | asan. TSan and ASan are SEPARATE builds per
# the v2.4 contract. Exercises the F31 bootstrap cleanup oracle at every stage
# plus adversarial RUNNING sessions, against the SHIPPING frontend_core code.
set -e
cd "$(dirname "$0")"
MODE="${1:-plain}"
SECS="${2:-}"
CC="${CC:-cc}"
OUT="build/frontend_core_test_$MODE"
mkdir -p build

case "$MODE" in
	plain) FLAGS="-O2";                       SECS="${SECS:-5}"  ;;
	tsan)  FLAGS="-fsanitize=thread  -O1 -g"; SECS="${SECS:-30}" ;;
	asan)  FLAGS="-fsanitize=address -O1 -g"; SECS="${SECS:-30}" ;;
	*) echo "usage: $0 [plain|tsan|asan] [seconds]"; exit 2 ;;
esac

# tiny rings on purpose: pressure the ABORTING / command-overflow / full-wait paths
# during the adversarial RUNNING sessions (unreachable at shipping capacities)
$CC -std=c11 -Wall -Wextra -Werror -DFR_STREAM_CAP=8 -DFR_SVC_STREAM=4 \
    $FLAGS -o "$OUT" framering.c frontend_core.c frontend_core_test.c -lpthread
"$OUT" "$SECS" ${3:+$3}
echo "== frontend_core $MODE: PASS =="
