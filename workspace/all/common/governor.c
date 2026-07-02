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
#define GOV_STEP_KHZ   216000  // one real OPP step (MEASURED gaps 192-216MHz; 108k snapped back up)
#define GOV_UP_DWELL   1       // ticks of slip before climbing (climb fast)
#define GOV_DN_DWELL   4       // ticks of slack before sinking (sink slow = no hunting)
#define GOV_FAIL_HOLD  120     // ticks (~60s) before re-probing a ceiling that slipped. Without this
                               // the loop limit-cycles at a boundary (600 slip -> 816 clean -> sink
                               // -> 600 slip -> ... = periodic slowdown bursts; Contra 2026-07-01)

// CONFIRMED device 2026-06-30: thermal_zone0 = cpu_thermal_zone (milli-C).
#define GOV_T_SENSOR   "/sys/class/thermal/thermal_zone0/temp"

// MEASURED OPP table (device 2026-06-30): 408 600 816 1008 1200 1416 1608 1800 2000 MHz.
// 2000000 is exposed but is the OC we avoid (never require overclocking); cap at 1800.
#define GOV_STOCK_MAX_KHZ 1800000

// ---- Per-system ceiling brackets (f_min/f_max are real OPPs; f_max <= stock cap, no OC) ----
const GovProfile GOV_P_8BIT   = {  408000, 1008000 }; // MEASURED: 408 is the real OPP floor
const GovProfile GOV_P_16BIT  = {  600000, 1416000 }; // MEASURED: 1416 is a real OPP (1320 was not)
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
	st->fail_khz = 0;
	st->fail_hold = 0;
}

int gov_step(GovState* st, const GovProfile* p, int temp_c, int frame_overrun) {
	// 1) thermal backstop — always wins
	if (temp_c >= 0 && temp_c >= GOV_T_CEIL_C) {
		st->ceil_khz -= GOV_STEP_KHZ;
		if (st->ceil_khz < p->f_min) st->ceil_khz = p->f_min;
		st->slip_run = st->slack_run = 0;
		return st->ceil_khz;
	}

	// failed-floor memory decays each tick; on expiry allow one re-probe (scene may have lightened)
	if (st->fail_hold > 0 && --st->fail_hold == 0) st->fail_khz = 0;

	if (frame_overrun == GOV_SIGNAL_BUSY) {
		// 2a) holding frame rate but with little headroom — do not probe lower (race-to-idle,
		// D14/D21: saturated-at-a-low-clock is the anti-pattern), but nothing is slipping, so
		// don't climb either.
		st->slip_run = 0;
		st->slack_run = 0;
	}
	else if (frame_overrun) {
		// 2) need more performance — climb fast, and remember the ceiling that proved too low
		if (st->ceil_khz > st->fail_khz) st->fail_khz = st->ceil_khz;
		st->fail_hold = GOV_FAIL_HOLD;
		st->slip_run++;
		st->slack_run = 0;
		if (st->slip_run >= GOV_UP_DWELL && st->ceil_khz < p->f_max) {
			st->ceil_khz += GOV_STEP_KHZ;
			if (st->ceil_khz > p->f_max) st->ceil_khz = p->f_max;
			st->slip_run = 0;
		}
	} else {
		// 3) have slack — probe downward (the cold win), but only when cool enough and not
		//    back into a ceiling that recently slipped (see GOV_FAIL_HOLD)
		st->slack_run++;
		st->slip_run = 0;
		int cool_enough = (temp_c < 0) || (temp_c <= GOV_T_TARGET_C);
		int next_khz = st->ceil_khz - GOV_STEP_KHZ;
		if (next_khz < p->f_min) next_khz = p->f_min;
		int not_failed = (st->fail_hold == 0) || (next_khz > st->fail_khz);
		if (st->slack_run >= GOV_DN_DWELL && cool_enough && not_failed && st->ceil_khz > p->f_min) {
			st->ceil_khz = next_khz;
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
