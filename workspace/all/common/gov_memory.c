// gov_memory.c — pure per-game governor-memory policy. See gov_memory.h.
#include <string.h>
#include "gov_memory.h"

#define GOVMEM_ARM_EXPIRY_TICKS 240 // ~2min of ticks unconsumed: scene is clearly heavier

void govmem_init(GovMemState* m, int enabled, int remembered_khz, const GovProfile* p) {
	memset(m, 0, sizeof(*m));
	m->window_dirty = 1; // dirty until one full clean window elapses
	m->enabled = enabled && p->f_min != p->f_max;
	if (!m->enabled) return;
	if (remembered_khz >= p->f_min && remembered_khz < p->f_max) {
		int target = remembered_khz + GOV_STEP_KHZ; // one OPP of headroom over the floor
		if (target > p->f_max) target = p->f_max;
		m->arm_khz = target;
	}
}

int govmem_arm_khz(const GovMemState* m) {
	return m->arm_khz;
}

void govmem_on_burst(GovMemState* m, int cancel_arm) {
	m->window_dirty = 1; // the pending publication no longer describes one workload
	if (cancel_arm) m->arm_khz = 0;
}

static void govmem_vote(GovMemState* m, int khz) {
	for (int i = 0; i < GOVMEM_HIST; i++) {
		if (m->hist_n[i] == 0 || m->hist_khz[i] == khz) {
			m->hist_khz[i] = khz;
			m->hist_n[i]++;
			return;
		}
	}
}

int govmem_post_tick(GovMemState* m, int signal, int fast_forward, unsigned rate_seq,
                     int ceil_before, int ceil_after) {
	int out = 0;
	if (!m->enabled) return 0;

	// 1) a tick-driven ceiling move poisons the in-flight rate window
	if (ceil_before != ceil_after) m->window_dirty = 1;

	// 2) vote at most once per publication, only from a clean window, only qualified
	if (rate_seq != m->vote_seq) {
		if (!m->window_dirty && !fast_forward
			&& (signal == GOV_SIGNAL_SLACK || signal == GOV_SIGNAL_BUSY))
			govmem_vote(m, ceil_before);
		m->vote_seq = rate_seq;
		m->window_dirty = 0; // a fresh window begins at this publication
	}

	// 3) accelerated ladder toward the remembered floor
	if (m->arm_khz > 0) {
		if (++m->arm_ticks > GOVMEM_ARM_EXPIRY_TICKS) {
			m->arm_khz = 0;
			out |= GOVMEM_EXPIRED;
		}
		else if (fast_forward) { /* hold: FF retarget owns the ceiling */ }
		else if (signal == GOV_SIGNAL_SLIP || signal == GOV_SIGNAL_BIGSLIP) {
			m->arm_khz = 0;
			out |= GOVMEM_DISARMED_SLIP;
		}
		else if (ceil_after <= m->arm_khz) {
			m->arm_khz = 0;
			out |= GOVMEM_REACHED;
		}
		else if (signal == GOV_SIGNAL_SLACK && rate_seq != m->ladder_seq) {
			m->ladder_seq = rate_seq; // one accelerated step per fresh publication
			out |= GOVMEM_PRELOAD_DWELL;
		}
	}
	return out;
}

int govmem_best(const GovMemState* m, uint32_t min_ticks) {
	uint32_t total = 0, best_n = 0;
	int best = 0;
	for (int i = 0; i < GOVMEM_HIST; i++) {
		total += m->hist_n[i];
		if (m->hist_n[i] > best_n) { best_n = m->hist_n[i]; best = m->hist_khz[i]; }
	}
	if (total < min_ticks) return 0;
	return best;
}
