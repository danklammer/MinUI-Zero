#ifndef FF_VISUAL_CADENCE_H
#define FF_VISUAL_CADENCE_H

#include <stdint.h>

typedef struct {
	uint64_t last_us;
	uint64_t acc_us;
} FFVisualCadence;

// Publish at most the nominal display rate while fast-forwarding. The cadence follows
// achieved production time rather than the requested FF multiplier: a heavy scene running
// below nominal presents every generated frame, while a core actually reaching 2x/4x emits
// only enough visuals to keep the vsynced presenter from throttling emulation.
static inline int ff_visual_step(FFVisualCadence* state, int enabled,
		uint64_t now_us, uint32_t period_us) {
	if (!enabled) {
		state->last_us = 0;
		state->acc_us = 0;
		return 1;
	}
	if (!period_us || !state->last_us) {
		state->last_us = now_us;
		state->acc_us = 0;
		return 1;
	}
	uint64_t elapsed = now_us > state->last_us ? now_us - state->last_us : 0;
	state->last_us = now_us;
	// A stall must produce one fresh frame, not build a visual backlog that throttles FF.
	if (elapsed > period_us) elapsed = period_us;
	state->acc_us += elapsed;
	if (state->acc_us < period_us) return 0;
	state->acc_us -= period_us;
	return 1;
}

#endif
