#ifndef FF_AUDIO_RATE_H
#define FF_AUDIO_RATE_H

#include <stddef.h>
#include <stdint.h>

#define FF_AUDIO_RATE_ONE_Q16 (1 << 16)
#define FF_AUDIO_RATE_MAX_Q16 (16 << 16)
#define FF_AUDIO_RATE_WINDOW_MS 50
#define FF_AUDIO_RATE_STALE_MS 500

typedef struct FFAudioRate {
	uint64_t input_frames;
	uint64_t check_after_frames;
	uint32_t window_at;
	int rate_q16;
	int hint_q16;
	int ready;
} FFAudioRate;

static inline void FFAudioRate_init(FFAudioRate* state) {
	state->input_frames = 0;
	state->check_after_frames = 0;
	state->window_at = 0;
	state->rate_q16 = FF_AUDIO_RATE_ONE_Q16;
	state->hint_q16 = 0;
	state->ready = 0;
}

static inline void FFAudioRate_begin(FFAudioRate* state, uint32_t now_ms) {
	state->input_frames = 0;
	state->check_after_frames = 0;
	state->window_at = now_ms;
	state->rate_q16 = state->hint_q16 > 0 ? state->hint_q16 : FF_AUDIO_RATE_ONE_Q16;
	state->ready = 0;
}

static inline void FFAudioRate_end(FFAudioRate* state) {
	state->input_frames = 0;
	state->check_after_frames = 0;
	state->window_at = 0;
	state->rate_q16 = FF_AUDIO_RATE_ONE_Q16;
	state->ready = 0;
}

static inline int FFAudioRate_accumulate(FFAudioRate* state, size_t frames,
		int input_rate) {
	if (input_rate <= 0) return 0;

	if ((uint64_t)frames > UINT64_MAX - state->input_frames)
		state->input_frames = UINT64_MAX;
	else
		state->input_frames += frames;

	if (!state->check_after_frames) {
		state->check_after_frames = (uint64_t)input_rate / 20;
		if (state->check_after_frames < 1) state->check_after_frames = 1;
	}
	return state->input_frames >= state->check_after_frames;
}

// Returns non-zero when rate_q16 changed. Input-frame production per wall second is the
// actual FF multiplier, independent of the configured cap or whether a core can reach it.
static inline int FFAudioRate_measure(FFAudioRate* state, int input_rate,
		uint32_t now_ms) {
	if (input_rate <= 0 || !state->input_frames) return 0;

	uint32_t elapsed_ms = now_ms - state->window_at; // unsigned subtraction handles wrap
	if (elapsed_ms < FF_AUDIO_RATE_WINDOW_MS) {
		uint64_t step = (uint64_t)input_rate / 100;
		if (step < 1) step = 1;
		state->check_after_frames = state->input_frames > UINT64_MAX - step
			? UINT64_MAX : state->input_frames + step;
		return 0;
	}
	if (elapsed_ms > FF_AUDIO_RATE_STALE_MS) {
		state->input_frames = 0;
		state->check_after_frames = 0;
		state->window_at = now_ms;
		return 0;
	}

	uint64_t denominator = (uint64_t)input_rate * elapsed_ms;
	uint64_t max_frames = denominator * (FF_AUDIO_RATE_MAX_Q16 / FF_AUDIO_RATE_ONE_Q16) / 1000;
	int measured_q16;
	if (state->input_frames >= max_frames) {
		measured_q16 = FF_AUDIO_RATE_MAX_Q16;
	}
	else {
		uint64_t numerator = state->input_frames * 1000 * FF_AUDIO_RATE_ONE_Q16;
		measured_q16 = denominator ? (int)(numerator / denominator) : FF_AUDIO_RATE_ONE_Q16;
	}
	if (measured_q16 < FF_AUDIO_RATE_ONE_Q16) measured_q16 = FF_AUDIO_RATE_ONE_Q16;
	if (measured_q16 > FF_AUDIO_RATE_MAX_Q16) measured_q16 = FF_AUDIO_RATE_MAX_Q16;

	int next_q16 = state->ready
		? (state->rate_q16 * 3 + measured_q16 + 2) / 4
		: measured_q16;
	int changed = next_q16 != state->rate_q16;
	state->rate_q16 = next_q16;
	state->hint_q16 = next_q16;
	state->ready = 1;
	state->input_frames = 0;
	state->check_after_frames = 0;
	state->window_at = now_ms;
	return changed;
}

static inline int FFAudioRate_push(FFAudioRate* state, size_t frames,
		int input_rate, uint32_t now_ms) {
	if (!FFAudioRate_accumulate(state, frames, input_rate)) return 0;
	return FFAudioRate_measure(state, input_rate, now_ms);
}

#endif
