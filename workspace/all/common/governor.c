// governor.c — closed-loop thermal/perf CPU governor. See governor.h and
// docs/thermal-governor-design.md.
//
// Self-contained: depends only on <stdio.h>/<stdlib.h> and a forward declaration of
// PLAT_setCPUMaxFreq() (provided by the platform layer on device, by a stub in the
// synthetic harness). No SDL / api.h, so the pure controller compiles and runs
// anywhere for testing.

#include <stdio.h>
#include <stdlib.h>
#include "governor.h"

// Platform primitive: set the cpufreq ceiling (scaling_max_freq); the kernel schedutil
// governor picks beneath it. Real impl in <platform>/platform.c, stub in governor_test.c.
void PLAT_setCPUMaxFreq(int khz);

// ---- Tunables (ASSUMED — safe by construction; confirm on device, see design doc) ----
// Why safe if wrong: (1) writes snap to the nearest valid OPP, (2) the loop self-corrects
// bad brackets at runtime, (3) the conservative ceiling bounds the downside to "too cautious".
#define GOV_T_TARGET_C 60      // start probing the clock down when at/below this
#define GOV_T_CEIL_C   72      // hard back-off above this — always wins
#define GOV_STEP_KHZ   108000  // ~one OPP per move
#define GOV_UP_DWELL   1       // ticks of slip before climbing (climb fast)
#define GOV_DN_DWELL   4       // ticks of slack before sinking (sink slow = no hunting)

// ASSUMED CPU thermal sensor path (verify `type` on device; thermal_zone0 is the guess).
// In milli-Celsius. Absent on non-device builds -> gov_read_temp_c() returns -1.
#define GOV_T_SENSOR   "/sys/class/thermal/thermal_zone0/temp"

// ASSUMED highest verified-stock OPP (kHz). CLAUDE.md: 1.8GHz stock, 2.0GHz is an OC.
// NEVER cap at/above 2000000. Confirm the real stock max on device (brick-recon.sh) and
// query scaling_available_frequencies at runtime; until then this bounds every f_max.
#define GOV_STOCK_MAX_KHZ 1800000

// ---- Per-system ceiling brackets (ASSUMED kHz; f_max <= GOV_STOCK_MAX_KHZ, no OC) ----
const GovProfile GOV_P_8BIT   = {  480000, 1008000 };
const GovProfile GOV_P_16BIT  = {  600000, 1320000 };
const GovProfile GOV_P_PS1    = { 1008000, 1800000 };
const GovProfile GOV_P_DEFAULT = { 600000, 1800000 };

int gov_read_temp_c(void) {
	FILE* f = fopen(GOV_T_SENSOR, "r");
	if (!f) return -1;
	int mc = -1;
	if (fscanf(f, "%d", &mc) != 1) mc = -1;
	fclose(f);
	return mc < 0 ? -1 : mc / 1000; // milli-C -> C
}

void gov_init(GovState* st, const GovProfile* p) {
	st->ceil_khz = p->f_max; // start high so the first frames never starve; sink from there
	st->slip_run = 0;
	st->slack_run = 0;
}

int gov_step(GovState* st, const GovProfile* p, int temp_c, int frame_overrun) {
	// 1) thermal backstop — always wins
	if (temp_c >= 0 && temp_c >= GOV_T_CEIL_C) {
		st->ceil_khz -= GOV_STEP_KHZ;
		if (st->ceil_khz < p->f_min) st->ceil_khz = p->f_min;
		st->slip_run = st->slack_run = 0;
		return st->ceil_khz;
	}

	if (frame_overrun) {
		// 2) need more performance — climb fast
		st->slip_run++;
		st->slack_run = 0;
		if (st->slip_run >= GOV_UP_DWELL && st->ceil_khz < p->f_max) {
			st->ceil_khz += GOV_STEP_KHZ;
			if (st->ceil_khz > p->f_max) st->ceil_khz = p->f_max;
			st->slip_run = 0;
		}
	} else {
		// 3) have slack — probe downward (the cold win), but only when cool enough
		st->slack_run++;
		st->slip_run = 0;
		int cool_enough = (temp_c < 0) || (temp_c <= GOV_T_TARGET_C);
		if (st->slack_run >= GOV_DN_DWELL && cool_enough && st->ceil_khz > p->f_min) {
			st->ceil_khz -= GOV_STEP_KHZ;
			if (st->ceil_khz < p->f_min) st->ceil_khz = p->f_min;
			st->slack_run = 0;
		}
	}
	return st->ceil_khz;
}

void gov_tick(GovState* st, const GovProfile* p, int frame_overrun) {
	// Safety hatch: GOV_DISABLE=1 leaves the static menu clock in charge.
	static int disabled = -1;
	if (disabled < 0) {
		const char* e = getenv("GOV_DISABLE");
		disabled = (e && e[0] && e[0] != '0') ? 1 : 0;
	}
	if (disabled) return;

	int temp_c = gov_read_temp_c();
	int ceil_khz = gov_step(st, p, temp_c, frame_overrun);
	// Re-assert the ceiling every tick: keeps the controller authoritative over the static
	// CPU-speed cap and over whatever the menu restores. ~once/0.5s, negligible cost.
	PLAT_setCPUMaxFreq(ceil_khz);
}
