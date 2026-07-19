// gov_memory.h — per-game governor-memory policy (arm / accelerated ladder / vote),
// extracted as a PURE unit (joint decision, review round 4): the repeated defects here
// were state-ordering bugs that governor tests and plausible-looking device traces did
// not catch, so this state machine is testable in isolation. No I/O, no minarch types:
// the caller owns sidecar files, env flags, logging, and gov_state mutation.
//
// Call shape per session:
//   govmem_init(...)                          once, after the profile is known
//   govmem_on_burst(...)                      at EVERY gov_burst call site, before it
//   govmem_post_tick(...)                     once per governor tick, after gov_tick
//   govmem_best(...)                          at persistence points (menu entry, exit)
#ifndef GOV_MEMORY_H
#define GOV_MEMORY_H

#include <stdint.h>
#include "governor.h"

#define GOVMEM_HIST 16

typedef struct {
	int enabled;
	int arm_khz;              // fast-sink target; 0 = disarmed
	int arm_ticks;            // ticks since arming (expiry)
	unsigned ladder_seq;      // last rate_seq consumed by an accelerated preload
	unsigned vote_seq;        // last rate_seq considered for a vote
	int window_dirty;         // ceiling moved (tick or burst) inside the current rate window
	int hist_khz[GOVMEM_HIST];
	uint32_t hist_n[GOVMEM_HIST];
} GovMemState;

// Arms the remembered floor (+one OPP of headroom, clamped) when: enabled, the bracket
// is not fixed (f_min==f_max learns nothing), and remembered_khz lies in [f_min, f_max).
// Any other remembered value (0, negative, garbage, out of range) leaves the unit inert.
void govmem_init(GovMemState* m, int enabled, int remembered_khz, const GovProfile* p);

// Current arm target, 0 when disarmed (for caller logging).
int govmem_arm_khz(const GovMemState* m);

// EVERY burst invalidates the current vote window — even at an unchanged f_max ceiling,
// the workload the pending publication measured is no longer the workload on screen
// (review r4 finding 2). cancel_arm applies caller policy: later geometry changes and
// FF toggles cancel; initial geometry acquisition does not.
void govmem_on_burst(GovMemState* m, int cancel_arm);

// Once per governor tick, AFTER gov_step/gov_tick ran. Handles, in order: tick-driven
// ceiling moves dirty the window; a fresh publication votes ceil_before once iff the
// window stayed clean and the sample is qualified (!ff, SLACK|BUSY); ladder bookkeeping
// (expiry, SLIP disarm, reached-target disarm, one dwell-preload per fresh publication).
// Return bitmask:
#define GOVMEM_PRELOAD_DWELL 1 // caller sets gov_state.slack_run = GOV_DN_DWELL-1
#define GOVMEM_REACHED       2 // arm completed: ceiling is at/below the remembered floor
#define GOVMEM_DISARMED_SLIP 4 // arm dropped because the scene slipped before the target
#define GOVMEM_EXPIRED       8 // arm dropped unconsumed (~2min)
int govmem_post_tick(GovMemState* m, int signal, int fast_forward, unsigned rate_seq,
                     int ceil_before, int ceil_after);

// Time-dominant learned ceiling (0 if fewer than min_ticks qualified votes exist).
int govmem_best(const GovMemState* m, uint32_t min_ticks);

#endif // GOV_MEMORY_H
