#!/bin/sh
# present-skip policy matrix (pure unit; see dupskip.h) — Codex P1#2
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/dupskip_test"
cc dupskip.c dupskip_test.c -o "$OUT" -I. -Wall -Wextra
"$OUT"
