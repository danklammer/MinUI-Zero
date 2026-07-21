#!/bin/sh
# audio pacing predicate (pure; shared by SND_isActive/SND_batchSamples) — Codex F2
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/snd_pacing_test"
cc snd_pacing_test.c -o "$OUT" -I. -Wall -Wextra
"$OUT"
