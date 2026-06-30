#!/bin/sh
# Build + run the benchmark telemetry unit tests on the host (no device). ASan-clean.
# Usage: sh run-telemetry-tests.sh   |   make test-telemetry (from repo root)
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
OUT="$DIR/build"
mkdir -p "$OUT"
"$CC" "$DIR/telemetry_test.c" "$DIR/telemetry.c" -o "$OUT/telemetry_test" \
	-I"$DIR" -std=gnu99 -O2 -fsanitize=address -fno-common \
	-Wall -Wextra -Wno-unused-parameter
exec "$OUT/telemetry_test"
