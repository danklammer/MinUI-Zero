#!/bin/sh
# bench-run.sh — on-device benchmark driver. Launches a core+rom with telemetry enabled for a
# fixed duration, then prints where the CSV landed. Analyze with tools/bench-analyze.py.
#
# Telemetry is gated by BENCH=1 (set here); the CSV path is BENCH_OUT. The harness samples a
# row every ~1s. For comparable runs, drive the SAME deterministic scene each time (e.g. an
# attract-mode demo, or a recorded input movie) — that part is the tester's responsibility.
#
# Usage (on the Brick over SSH):
#   bench-run.sh <core.so> <rom> [seconds] [tag]
set -eu
CORE="${1:?usage: bench-run.sh <core.so> <rom> [seconds] [tag]}"
ROM="${2:?rom required}"
SECONDS_TO_RUN="${3:-120}"
TAG="${4:-run}"

OUT="/tmp/bench-${TAG}.csv"
export BENCH=1
export BENCH_OUT="$OUT"

echo "benchmarking: $CORE  $ROM  (${SECONDS_TO_RUN}s)  -> $OUT" >&2

# launch minarch in the background, let it run the scene, then stop it
minarch.elf "$CORE" "$ROM" &
PID=$!
i=0
while [ "$i" -lt "$SECONDS_TO_RUN" ]; do
	kill -0 "$PID" 2>/dev/null || break
	sleep 1
	i=$((i + 1))
done
# graceful stop so tlm_quit() flushes the summary line
kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

echo "done -> $OUT" >&2
echo "analyze: tools/bench-analyze.py $OUT  (or two CSVs for an A/B diff)" >&2
