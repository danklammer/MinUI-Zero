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
void PLAT_setCPUVoltForCeil(int khz);

// ---- Tunables (ASSUMED — safe by construction; confirm on device, see design doc) ----
// Why safe if wrong: (1) writes snap to the nearest valid OPP, (2) the loop self-corrects
// bad brackets at runtime, (3) the conservative ceiling bounds the downside to "too cautious".
#define GOV_T_TARGET_C 60      // start probing the clock down when at/below this
#define GOV_T_CEIL_C   72      // hard back-off above this — always wins
#define GOV_STEP_KHZ   216000  // one real OPP step (MEASURED gaps 192-216MHz; 108k snapped back up)
#define GOV_DN_DWELL   4       // ticks of slack before sinking (sink slow = no hunting)
#define GOV_FAIL_HOLD  120     // ticks (~60s) before re-probing a ceiling that slipped. Without this
                               // the loop limit-cycles at a boundary (600 slip -> 816 clean -> sink
                               // -> 600 slip -> ... = periodic slowdown bursts; Contra 2026-07-01)
#define GOV_FUTILE_TICKS 4     // slip ticks AT f_max (~2s) before declaring the climb futile: the
                               // scene dips at every clock (authentic slowdown), so f_max is pure heat
#define GOV_FUTILE_HOLD  120   // ticks (~60s) of not chasing that scene (slips hold instead of climb);
                               // expiry re-tests honestly, gov_burst (scene change) clears immediately

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
	st->fail_streak = 0;
	st->presink_khz = 0;
	st->since_sink = 255;
	st->slip_origin_khz = 0;
	st->max_slip_run = 0;
	st->futile_hold = 0;
}

int gov_step(GovState* st, const GovProfile* p, int temp_c, int frame_overrun) {
	// 1) thermal backstop — always wins
	if (temp_c >= 0 && temp_c >= GOV_T_CEIL_C) {
		st->ceil_khz -= GOV_STEP_KHZ;
		if (st->ceil_khz < p->f_min) st->ceil_khz = p->f_min;
		st->slip_run = st->slack_run = 0;
		return st->ceil_khz;
	}

	// failed-floor memory decays each tick; on expiry allow one re-probe (scene may have
	// lightened). fail_khz itself is kept: if the re-probe slips again it's a repeat offense
	// and the hold escalates (see the slip branch) instead of limit-cycling every ~60s.
	if (st->fail_hold > 0) --st->fail_hold;
	if (st->futile_hold > 0) --st->futile_hold;
	if (st->since_sink < 255) st->since_sink++;

	if (frame_overrun == GOV_SIGNAL_BUSY) {
		// 2a) holding frame rate but with little headroom — do not probe lower this tick
		// (race-to-idle, D14/D21), but nothing is slipping, so don't climb either.
		// BUSY no longer ERASES slack progress: rare stall frames (SRAM fsync, ~20ms)
		// sprinkle BUSY windows through otherwise-idle games, and a hard reset meant the
		// 4-consecutive-slack requirement never accumulated — GBC pinned at 1008 with
		// p95=7.7ms/16.7 (2026-07-09 gate telemetry). A wrong sink is cheap now (one-tick
		// probe-undo + BIGSLIP-to-max + fail memory), so the gate can afford leniency.
		st->slip_run = 0;
	}
	else if (frame_overrun) {
		// Fast-forward slips (GOV_SIGNAL_FFSLIP) are unreachable-by-design targets: they must
		// climb like BIGSLIP but NEVER touch the futile machinery — not the hold (FF must climb
		// even during one), not the origin, not the at-max counter (else FF reads as "unfixable"
		// after 4 ticks and stands down, collapsing FF speed — review 2026-07-16).
		int ff = (frame_overrun == GOV_SIGNAL_FFSLIP);
		// Futile-climb hold: this scene already proved a full climb to f_max does not cure its
		// slip (authentic engine slowdown — the game dips at EVERY clock). Chasing it again is
		// pure heat: hold the restored ceiling, skip the climb AND the fail-memory arming (60s of
		// held slips would otherwise poison fail_khz and block legitimate later sinking). Expiry
		// or a scene change (gov_burst) re-tests honestly. Audio: presentation-drop covers dips.
		if (st->futile_hold > 0 && !ff) {
			st->slack_run = 0;
			return st->ceil_khz;
		}
		if (!ff) {
			// Episode origin: remember the settled ceiling this slip episode started from — if the
			// climb turns out futile, this is what we restore (the clock the scene actually needs).
			if (st->slip_run == 0 && st->ceil_khz < p->f_max)
				st->slip_origin_khz = st->ceil_khz;
			// Futile detector: slipping AT f_max means the climb already happened and didn't cure it.
			if (st->ceil_khz >= p->f_max) {
				if (++st->max_slip_run >= GOV_FUTILE_TICKS && st->slip_origin_khz > 0
				    && st->slip_origin_khz < p->f_max) {
					st->ceil_khz = st->slip_origin_khz;  // stand down to what the scene actually needs
					st->futile_hold = GOV_FUTILE_HOLD;
					st->max_slip_run = 0;
					st->slip_run = 0;
					st->slack_run = 0;
					return st->ceil_khz;
				}
			}
			else st->max_slip_run = 0;
		}
		// 2) need more performance — climb fast, and remember the ceiling that proved too low.
		// A slip at/below a ceiling that already failed is a repeat offense: escalate the hold
		// (60s -> 2m -> 4m -> 8m) so known-bad probes become rare instead of periodic
		// (BR2 480i screens 2026-07-09: the ~60s re-probe cycle was an audible slowdown burst).
		// Fail memory only arms for a ceiling BELOW f_max: a slip while already fully
		// provisioned proves nothing about lower clocks (no probe failed), and remembering
		// it blocked ALL sinking — boot-load slips at f_max pinned GBC at 1008 for up to
		// 8 min (the escalated hold), refreshed forever by borderline slips. Found by the
		// 2026-07-09 gate telemetry: signal=SLACK, p95 7.7ms/16.7ms, ceiling frozen.
		if (st->ceil_khz < p->f_max) {
			if (st->fail_khz > 0 && st->ceil_khz <= st->fail_khz) {
				if (st->fail_streak < 3) st->fail_streak++;
			}
			else st->fail_streak = 0;
			if (st->ceil_khz > st->fail_khz) st->fail_khz = st->ceil_khz;
			st->fail_hold = GOV_FAIL_HOLD << st->fail_streak;
		}
		st->slip_run++;
		st->slack_run = 0;
		if (st->ceil_khz < p->f_max) {
			// Recovery sizing, cheapest-adequate first (every extra tick under target is
			// audible — BR2 char select 2026-07-09: probe to 1008 on a ~1584 screen = crunch):
			// 1) the slip arrived right after a sink -> that probe CAUSED it; undo it in one
			//    tick by restoring the pre-sink ceiling;
			// 2) big deficit (>=10% under target) -> jump straight to f_max;
			// 3) small deficit -> one step, then f_max if it survives a second tick.
			if (frame_overrun == GOV_SIGNAL_BIGSLIP || frame_overrun == GOV_SIGNAL_FFSLIP)
				st->ceil_khz = p->f_max; // severity outranks probe-undo: a deep deficit
				                         // restored to a too-low pre-sink ceiling pays twice
				                         // (FF: the target is unreachable by design — max now)
			else if (st->since_sink < 8 && st->presink_khz > st->ceil_khz)
				st->ceil_khz = st->presink_khz;
			else if (st->slip_run >= 2)
				st->ceil_khz = p->f_max;
			else st->ceil_khz += GOV_STEP_KHZ;
			if (st->ceil_khz > p->f_max) st->ceil_khz = p->f_max;
		}
	} else {
		// 3) have slack — probe downward (the cold win), but only when cool enough and not
		//    back into a ceiling that recently slipped (see GOV_FAIL_HOLD)
		st->slack_run++;
		st->slip_run = 0;
		st->max_slip_run = 0;   // slack = the slip episode ended (scene recovered on its own)
		st->slip_origin_khz = 0;
		int cool_enough = (temp_c < 0) || (temp_c <= GOV_T_TARGET_C);
		int next_khz = st->ceil_khz - GOV_STEP_KHZ;
		if (next_khz < p->f_min) next_khz = p->f_min;
		int not_failed = (st->fail_hold == 0) || (next_khz > st->fail_khz);
		if (st->slack_run >= GOV_DN_DWELL && cool_enough && not_failed && st->ceil_khz > p->f_min) {
			st->presink_khz = st->ceil_khz; // remembered so a probe-caused slip undoes in one tick
			st->since_sink = 0;             // (thermal sinks deliberately don't set these: temp wins)
			st->ceil_khz = next_khz;
			st->slack_run = 0;
		}
	}
	return st->ceil_khz;
}

// ---- predictive sink gate (pure; see governor.h) ----
#define GOV_SINK_MARGIN_PCT 85 // sink only if predicted p95 fits this % of the frame budget

int gov_sink_target(const GovState* st, const GovProfile* p) {
	int next = st->ceil_khz - GOV_STEP_KHZ;
	if (next < p->f_min) next = p->f_min;
	return next;
}

int gov_sink_fits(int cur_khz, int next_khz, int p95_pure_us, int budget_us) {
	if (next_khz <= 0 || cur_khz <= 0 || budget_us <= 0) return 0;
	if (next_khz >= cur_khz) return 1;               // not actually sinking
	if (p95_pure_us <= 0) return 1;                  // no work data -> don't block (fps signal still guards)
	// predicted post-sink p95: work scales ~ inversely with clock (CPU-bound frame)
	long long predicted = (long long)p95_pure_us * cur_khz / next_khz;
	return predicted < (long long)budget_us * GOV_SINK_MARGIN_PCT / 100;
}

static int gov_disabled(void) {
	// Safety hatch: GOV_DISABLE=1 leaves the static menu clock in charge.
	static int disabled = -1;
	if (disabled < 0) {
		const char* e = getenv("GOV_DISABLE");
		disabled = (e && e[0] && e[0] != '0') ? 1 : 0;
	}
	return disabled;
}

void gov_burst(GovState* st, const GovProfile* p) {
	// A video-mode/geometry switch announces a scene change BEFORE its cost arrives —
	// provision for the worst immediately and let the sink ladder re-find the floor.
	// The reverse order (wait for the slip) is audible: BR2 title->demo / VS-card
	// transitions crunched for the 1-2 ticks the climb took (2026-07-09).
	if (gov_disabled()) return;
	st->ceil_khz = p->f_max;
	st->slip_run = 0;
	st->slack_run = 0;
	st->fail_khz = 0;
	st->fail_hold = 0;
	st->fail_streak = 0;
	st->presink_khz = 0;
	st->since_sink = 255; // not a probe: a slip here must not "undo" to a stale ceiling
	st->slip_origin_khz = 0;
	st->max_slip_run = 0;
	st->futile_hold = 0;  // new scene: the futile verdict belonged to the old one — re-test honestly
	PLAT_setCPUMaxFreq(st->ceil_khz);
}

void gov_tick(GovState* st, const GovProfile* p, int frame_overrun) {
	if (gov_disabled()) return;

	int temp_c = gov_read_temp_c();
	static int last_ceil = 0;
	int ceil_khz = gov_step(st, p, temp_c, frame_overrun);
	// Re-assert the ceiling every tick: keeps the controller authoritative over the static
	// CPU-speed cap and over whatever the menu restores. ~once/0.5s, negligible cost.
	// Voltage ordering lives in the platform ceiling choke point (uv_ceilingWrite) so that
	// EVERY ceiling writer gets it, not just the governor (audit fix: the menu-exit path).
	PLAT_setCPUMaxFreq(ceil_khz);
	(void)last_ceil; last_ceil = ceil_khz;
}
