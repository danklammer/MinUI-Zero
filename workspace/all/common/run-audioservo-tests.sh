#!/bin/sh
# audio ring occupancy servo control law (pure unit; see audioservo.h)
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/audioservo_test"
cc audioservo_test.c -o "$OUT" -I. -Wall -Wextra
"$OUT"
