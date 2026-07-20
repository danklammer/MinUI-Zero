#!/bin/sh
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
OUT="$DIR/build"
mkdir -p "$OUT"

"$CC" "$DIR/save_io_test.c" "$DIR/save_io.c" -o "$OUT/save_io_test" \
	-I"$DIR" -DSAVE_IO_TEST -std=gnu99 -O2 \
	-fsanitize=address,undefined -fno-common -Wall -Wextra

exec "$OUT/save_io_test"
