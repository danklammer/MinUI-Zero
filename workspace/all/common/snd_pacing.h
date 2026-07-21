// snd_pacing.h — the single pacing predicate shared by SND_batchSamples and SND_isActive
// (Codex F2). Pure and field-based so it is testable without linking api.c's SDL-coupled SND:
// present-skip drops vsync waits only when this is true, i.e. when a batch would actually be
// accepted and backpressure emulation. A failed ring allocation leaves buffer NULL (or
// frame_count 0), so this returns false and skipping stays disabled — no runaway generation.
#ifndef SND_PACING_H
#define SND_PACING_H

#include <stddef.h>

// has_resampler = (snd.resample != NULL). Passed as int, not a function pointer, since ISO C
// does not define function-pointer-to-object-pointer conversion (Codex F2 P2, -Wpedantic).
static inline int snd_pacing_ok(int initialized, int paused,
                                const void* buffer, size_t frame_count, int has_resampler) {
	return initialized && !paused && buffer && frame_count != 0 && has_resampler;
}

#endif
