#!/bin/sh
# wakeup-reduction synchronization harness: plain, then TSan and ASan where available.
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/wakeup_test"
cc wakeup_test.c -o "$OUT" -lpthread
"$OUT"
if cc wakeup_test.c -o "$OUT-tsan" -lpthread -fsanitize=thread 2>/dev/null; then
  echo "== TSan =="
  "$OUT-tsan"
else
  echo "== TSan unavailable on this host: skipped =="
fi
if cc wakeup_test.c -o "$OUT-asan" -lpthread -fsanitize=address 2>/dev/null; then
  echo "== ASan =="
  "$OUT-asan"
else
  echo "== ASan unavailable on this host: skipped =="
fi
