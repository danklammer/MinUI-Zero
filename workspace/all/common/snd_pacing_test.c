// snd_pacing_test.c — Codex F2: the audio-pacing predicate that gates present-skip. Proves a
// failed ring allocation (buffer NULL / frame_count 0) and every not-live state read as
// "cannot pace", so SND_isActive is false and skipping stays disabled.
#include "snd_pacing.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
	int buf = 0;
	void* BUF = &buf;                  // non-NULL stand-in for a usable ring

	CHECK( snd_pacing_ok(1, 0, BUF, 512, 1), "fully live -> can pace");

	CHECK(!snd_pacing_ok(0, 0, BUF, 512, 1), "not initialized -> cannot pace");
	CHECK(!snd_pacing_ok(1, 1, BUF, 512, 1), "paused (sleep) -> cannot pace");
	CHECK(!snd_pacing_ok(1, 0, NULL, 512, 1), "ring alloc failed (buffer NULL) -> cannot pace");
	CHECK(!snd_pacing_ok(1, 0, BUF, 0,   1), "zero-capacity ring -> cannot pace");
	CHECK(!snd_pacing_ok(1, 0, BUF, 512, 0), "no resampler -> cannot pace");

	if (fails) { printf("== snd_pacing: %d FAILURE(S) ==\n", fails); return 1; }
	printf("== snd_pacing: ALL PASS ==\n");
	return 0;
}
