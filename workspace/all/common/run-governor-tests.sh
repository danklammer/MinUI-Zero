#!/bin/sh
# Build and run the closed-loop governor's synthetic harness on the host (no device,
# no cross-toolchain). Compiles the pure controller + the scripted trace tests under
# AddressSanitizer and asserts the safety/convergence properties from
# docs/thermal-governor-design.md. Exit non-zero on any failed assertion.
#
# Usage:  sh run-governor-tests.sh        (from anywhere)
#         make test-governor              (from the repo root)
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
OUT="$DIR/build"
mkdir -p "$OUT"

# governor.c / governor_test.c are plain C99 with no ARM asm, so they build clean on
# any host compiler. ASan catches memory/UB; -Wall -Wextra keeps the controller tidy.
"$CC" "$DIR/governor_test.c" "$DIR/governor.c" -o "$OUT/governor_test" \
	-I"$DIR" -std=gnu99 -O2 -fsanitize=address -fno-common \
	-Wall -Wextra -Wno-unused-parameter

exec "$OUT/governor_test"
