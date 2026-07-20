#!/bin/sh
# governor-memory policy matrix (pure unit; see gov_memory.h)
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/govmem_test"
cc gov_memory.c gov_memory_test.c -o "$OUT" -I.
"$OUT"
