#include <assert.h>
#include <stdio.h>
#include "ff_visual_cadence.h"

static int count_emits(uint32_t interval_us, int epochs) {
	FFVisualCadence state = {0};
	int emits = 0;
	uint64_t now = 1000;
	for (int i = 0; i < epochs; i++) {
		emits += ff_visual_step(&state, 1, now, 16667);
		now += interval_us;
	}
	return emits;
}

int main(void) {
	assert(count_emits(4167, 240) == 60);   // 240 generated fps -> 60 visuals
	assert(count_emits(11111, 90) == 60);  // 90 generated fps -> 60 visuals
	assert(count_emits(15152, 66) == 60);  // 66 generated fps -> 60 visuals
	assert(count_emits(16667, 60) == 60);  // nominal rate -> every frame
	assert(count_emits(22222, 45) == 45);  // below nominal -> every frame

	FFVisualCadence state = {0};
	assert(ff_visual_step(&state, 1, 1000, 16667) == 1);
	assert(ff_visual_step(&state, 1, 1000000, 16667) == 1); // stall: one frame, no backlog
	assert(state.acc_us == 0);
	assert(ff_visual_step(&state, 1, 1001000, 16667) == 0);
	assert(ff_visual_step(&state, 0, 1002000, 16667) == 1);
	assert(state.last_us == 0 && state.acc_us == 0);
	assert(ff_visual_step(&state, 1, 1003000, 16667) == 1); // first FF frame is immediate
	puts("ff_visual_cadence_test: PASS");
	return 0;
}
