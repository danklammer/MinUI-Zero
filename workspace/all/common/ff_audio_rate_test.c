#include "ff_audio_rate.h"

#include <assert.h>
#include <stdio.h>

static int near_q16(int actual, double expected) {
	int target = (int)(expected * FF_AUDIO_RATE_ONE_Q16);
	int delta = actual - target;
	if (delta < 0) delta = -delta;
	return delta < FF_AUDIO_RATE_ONE_Q16 / 100;
}

static void feed_rate(FFAudioRate* state, int input_rate, double multiplier,
		uint32_t start_ms) {
	for (int i = 1; i <= 5; i++) {
		size_t frames = (size_t)(input_rate * multiplier / 100.0);
		FFAudioRate_push(state, frames, input_rate, start_ms + i * 10);
	}
}

int main(void) {
	FFAudioRate state;
	FFAudioRate_init(&state);
	assert(state.rate_q16 == FF_AUDIO_RATE_ONE_Q16);

	FFAudioRate_begin(&state, 0);
	assert(!FFAudioRate_accumulate(&state, 2399, 48000));
	assert(FFAudioRate_accumulate(&state, 1, 48000));
	assert(!FFAudioRate_measure(&state, 48000, 10));
	assert(!FFAudioRate_accumulate(&state, 479, 48000));
	assert(FFAudioRate_accumulate(&state, 1, 48000));

	FFAudioRate_begin(&state, 0);
	feed_rate(&state, 48000, 4.0, 0);
	assert(state.ready);
	assert(near_q16(state.rate_q16, 4.0));

	FFAudioRate_end(&state);
	FFAudioRate_begin(&state, 100);
	assert(near_q16(state.rate_q16, 4.0)); // reuse the last stable rate immediately
	feed_rate(&state, 48000, 1.5, 100);
	assert(near_q16(state.rate_q16, 1.5));

	feed_rate(&state, 48000, 3.5, 150);
	assert(near_q16(state.rate_q16, 2.0)); // 3:1 EMA after the first measurement

	int before_gap = state.rate_q16;
	assert(!FFAudioRate_push(&state, 512, 48000, 1200));
	assert(state.rate_q16 == before_gap);

	FFAudioRate_end(&state);
	FFAudioRate_begin(&state, 2000);
	feed_rate(&state, 48000, 40.0, 2000);
	assert(state.rate_q16 == FF_AUDIO_RATE_MAX_Q16);
	FFAudioRate_begin(&state, 2100);
	FFAudioRate_push(&state, (size_t)-1, 48000, 2150);
	assert(state.rate_q16 == FF_AUDIO_RATE_MAX_Q16);

	FFAudioRate_init(&state);
	FFAudioRate_begin(&state, UINT32_MAX - 20);
	FFAudioRate_push(&state, 9600, 48000, 29); // 50ms across tick wrap, 4x input
	assert(near_q16(state.rate_q16, 4.0));

	puts("ff_audio_rate: all tests passed");
	return 0;
}
