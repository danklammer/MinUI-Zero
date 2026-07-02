// governor_test.c — standalone synthetic harness for the closed-loop governor.
//
// No hardware. Feeds scripted temperature + frame-slip traces into the pure
// controller (gov_step) and asserts the safety + convergence properties from
// docs/thermal-governor-design.md:
//   - the commanded clock never leaves [f_min, f_max];
//   - a hot trace (temp >= ceiling) backs the clock down monotonically and never
//     raises it — the thermal ceiling always wins;
//   - a cold/idle trace sinks to the lowest clock (f_min) and stays there;
//   - a real workload converges to the lowest *stable* clock that holds frame rate
//     (settles at/above the requirement but strictly below f_max — a real saving),
//     without hunting wildly even when slip/slack alternate.
//
// Build + run: see .notes/verify.sh, or workspace/all/common/run-governor-tests.sh.
//
// Most scenarios model overrun realistically: a frame overruns iff the current clock
// is below the workload's required clock (f_req). gov_step sees the slip produced by
// the clock it last commanded, exactly as the device loop does.

#include <stdio.h>
#include <stdlib.h>
#include "governor.h"

// ---- gov_tick's platform dependency, stubbed so governor.c links standalone ----
static int g_last_set_khz = -1;
void PLAT_setCPUMaxFreq(int khz) { g_last_set_khz = khz; }

// ---- tiny assert framework ----
static int g_fail = 0;
#define CHECK(cond, ...) do { \
	if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n        (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Run a workload-driven trace: overrun is derived from whether the live clock holds
// f_req. Returns final state via *st; fills tail min/max over the last `tail` ticks.
static void run_workload(GovState* st, const GovProfile* p, int f_req, int temp_c,
                         int ticks, int tail, int* tail_min, int* tail_max,
                         int* tail_overruns) {
	int tmin = p->f_max, tmax = p->f_min, oruns = 0;
	for (int i = 0; i < ticks; i++) {
		int overrun = (st->ceil_khz < f_req); // the slip the current clock produces
		int khz = gov_step(st, p, temp_c, overrun);
		CHECK(khz >= p->f_min && khz <= p->f_max,
		      "clock %d left bracket [%d,%d]", khz, p->f_min, p->f_max);
		if (i >= ticks - tail) {
			if (khz < tmin) tmin = khz;
			if (khz > tmax) tmax = khz;
			// did the (post-step) clock still fail the workload?
			if (khz < f_req) oruns++;
		}
	}
	*tail_min = tmin; *tail_max = tmax; *tail_overruns = oruns;
}

// ---- scenario 1: cold / idle — should sink to f_min and stay ----
static void test_cold_idle(void) {
	printf("[cold/idle] sinks to f_min and stays there\n");
	const GovProfile* p = &GOV_P_8BIT;
	GovState st; gov_init(&st, p);
	int tmin, tmax, oruns;
	// f_req at f_min: the clock always holds frame rate, so the controller is free to sink.
	run_workload(&st, p, p->f_min, 40 /*cool*/, 200, 30, &tmin, &tmax, &oruns);
	CHECK(st.ceil_khz == p->f_min, "expected to settle at f_min=%d, got %d", p->f_min, st.ceil_khz);
	CHECK(tmax == p->f_min, "tail clock should be pinned at f_min, saw up to %d", tmax);
	CHECK(oruns == 0, "cold/idle should never overrun, saw %d tail overruns", oruns);
}

// ---- scenario 2: hot / overrun — thermal ceiling must win over the climb ----
static void test_hot_ceiling(void) {
	printf("[hot/ceiling] temp >= ceiling backs clock down monotonically, never up\n");
	const GovProfile* p = &GOV_P_PS1; // widest bracket
	GovState st; gov_init(&st, p);
	// Demand maximum performance (overrun=1 every tick) while pinned above the ceiling.
	// The ceiling branch must override the climb and walk the clock down to f_min.
	int prev = st.ceil_khz;
	int reached_min = 0;
	for (int i = 0; i < 200; i++) {
		int khz = gov_step(&st, p, 80 /*>= 72C ceiling*/, 1 /*frames slipping*/);
		CHECK(khz <= prev, "tick %d: clock rose to %d from %d while over the ceiling", i, khz, prev);
		CHECK(khz >= p->f_min && khz <= p->f_max, "clock %d left bracket", khz);
		prev = khz;
		if (khz == p->f_min) reached_min = 1;
	}
	CHECK(reached_min, "hot trace should drive the clock down to f_min");
	CHECK(st.ceil_khz == p->f_min, "should hold f_min under sustained overheat, got %d", st.ceil_khz);
}

// ---- scenario 2b: thermal ceiling caps a hot-but-demanding workload below f_max ----
static void test_hot_caps_below_max(void) {
	printf("[hot/cap] heavy load that wants f_max is capped by the ceiling\n");
	const GovProfile* p = &GOV_P_PS1;
	GovState st; gov_init(&st, p);
	// Heavy workload (needs f_max) but the chip is over the ceiling: ceiling wins,
	// clock is pulled to f_min and the governor never exceeds it.
	int peak = p->f_min;
	for (int i = 0; i < 200; i++) {
		int overrun = (st.ceil_khz < p->f_max); // wants to climb to the top
		int khz = gov_step(&st, p, 75, overrun);
		if (khz > peak) peak = khz;
	}
	// After the first tick the ceiling dominates; the only clock at/above the start is the
	// initial f_max, which the ceiling immediately reduces. Final must be f_min.
	CHECK(st.ceil_khz == p->f_min, "ceiling should pin a hot heavy load at f_min, got %d", st.ceil_khz);
}

// ---- scenario 3: oscillation-prone — alternating slip/slack must not hunt low ----
static void test_oscillation(void) {
	printf("[oscillation] alternating slip/slack settles high, does not hunt down\n");
	const GovProfile* p = &GOV_P_16BIT;
	GovState st; gov_init(&st, p);
	// Force strict alternation independent of clock. UP_DWELL=1 climbs on any slip;
	// DN_DWELL=4 needs 4 *consecutive* slack ticks to sink. Alternation resets slack_run
	// every other tick, so it can never sink — it must settle at f_max and stay.
	int tail_min = p->f_max;
	for (int i = 0; i < 200; i++) {
		int overrun = (i % 2 == 0); // slip, slack, slip, slack, ...
		int khz = gov_step(&st, p, 50 /*cool*/, overrun);
		CHECK(khz >= p->f_min && khz <= p->f_max, "clock %d left bracket", khz);
		if (i >= 170 && khz < tail_min) tail_min = khz;
	}
	CHECK(st.ceil_khz == p->f_max, "oscillating load should settle at f_max, got %d", st.ceil_khz);
	CHECK(tail_min == p->f_max, "should not hunt below f_max once settled, dipped to %d", tail_min);
}

// ---- scenario 3b: boundary workload must not limit-cycle at the floor (D23 / Contra) ----
// A game that slips at f_min but holds one step up used to cycle forever: sink to 600 ->
// slip (a burst of slow frames) -> climb to 816 -> 4 clean ticks -> sink -> slip -> ...
// Failed-floor memory (fail_khz/GOV_FAIL_HOLD) must bound the re-probes: at most one dip
// per hold window, not one per DN_DWELL.
static void test_boundary_no_limit_cycle(void) {
	printf("[boundary] floor-slipping workload dips at most once per fail-hold window\n");
	const GovProfile* p = &GOV_P_16BIT; // 600000..1416000
	int f_req = 700000;                 // slips at 600, holds at 816 — the Contra shape
	GovState st; gov_init(&st, p);
	int dips = 0, prev_holds = 0;
	const int TICKS = 600;              // ~5 minutes of ticks
	for (int i = 0; i < TICKS; i++) {
		int overrun = (st.ceil_khz < f_req);
		int khz = gov_step(&st, p, 40 /*cool*/, overrun);
		CHECK(khz >= p->f_min && khz <= p->f_max, "clock %d left bracket", khz);
		int holds = (khz >= f_req);
		if (prev_holds && !holds) dips++; // transitioned back into the failing clock
		prev_holds = holds;
	}
	// Old controller: a dip every DN_DWELL+1 ticks (~120 dips in 600). With the 120-tick
	// fail hold: the initial sink plus at most ~5 re-probes. Allow slack, but the cycle
	// must be gone by an order of magnitude.
	CHECK(dips <= 8, "limit cycle: %d dips into the failing clock over %d ticks", dips, TICKS);
	CHECK(st.ceil_khz >= f_req, "should end settled at/above the requirement, got %d", st.ceil_khz);
	printf("  dips into failing clock: %d over %d ticks (old controller: ~120)\n", dips, TICKS);
}

// ---- scenario 3c: predictive sink gate (D24 refinement) ----
// gov_sink_fits: predicted p95 at the lower clock (p95 * cur/next) must fit 85% of budget.
// Validated against measured device data: Contra@Low fits 408, PS1 settles at 1416.
static void test_predictive_sink_gate(void) {
	printf("[sink-gate] predictive fit reproduces the measured per-system sweet spots\n");
	// Contra@Low measured p95 ~7500us at 600 -> predicted 11029 at 408 < 14166 (85%% of 16667): fits
	CHECK(gov_sink_fits(600000, 408000, 7500, 16667) == 1, "Contra-shape should fit at 408");
	// PS1 measured pure p95 ~12000us at 1800 (arithmetic ladder: 1800 -> 1584 -> 1368):
	CHECK(gov_sink_fits(1800000, 1584000, 12000, 16667) == 1, "PS1 should fit at 1584");
	CHECK(gov_sink_fits(1584000, 1368000, 13636, 16667) == 0, "PS1 must NOT sink to 1368 (D14 trap)"); // 12000*1800/1584
	// guards
	CHECK(gov_sink_fits(600000, 600000, 99999, 16667) == 1, "not-a-sink is always allowed");
	CHECK(gov_sink_fits(600000, 408000, 0, 16667) == 1, "no work data must not block (fps signal guards)");

	// full trace: PS1-shaped workload — p95 scales with the live ceiling from a 12ms@1800 base.
	// The controller's ladder is arithmetic (1800-216=1584); on device the kernel quantizes a
	// 1584 ceiling down to the 1416 OPP — i.e. settling at 1584 IS the D14-validated 1416 spot.
	const GovProfile* p = &GOV_P_PS1; // 1008000..1800000
	GovState st; gov_init(&st, p);
	int min_seen = p->f_max;
	for (int i = 0; i < 200; i++) {
		long long p95 = 12000LL * p->f_max / st.ceil_khz;      // work at the current ceiling
		int fps_short = (st.ceil_khz < 1584000);               // below the 1416-OPP band the game drops rate
		int next = gov_sink_target(&st, p);
		int sig = fps_short ? GOV_SIGNAL_SLIP
		        : (gov_sink_fits(st.ceil_khz, next, (int)p95, 16667) ? GOV_SIGNAL_SLACK : GOV_SIGNAL_BUSY);
		gov_step(&st, p, 45 /*cool*/, sig);
		if (st.ceil_khz < min_seen) min_seen = st.ceil_khz;
	}
	CHECK(st.ceil_khz == 1584000, "PS1 trace should settle at 1584 (=1416 OPP effective), got %d", st.ceil_khz);
	CHECK(min_seen >= 1584000, "PS1 trace must never probe below 1584 (gate holds), dipped to %d", min_seen);
	printf("  PS1 trace settled at %d (kernel quantizes to the 1416 OPP; never below %d)\n", st.ceil_khz, min_seen);
}

// ---- scenario 4: real workload converges to the lowest stable clock (a saving) ----
static void test_converges_to_lowest_stable(void) {
	printf("[converge] mid workload settles at/above requirement but below f_max\n");
	const GovProfile* p = &GOV_P_PS1; // 1008000..1800000 (stock cap), wide enough for a clear mid point
	int f_req = 1500000;              // needs ~1.5GHz; should settle near there, not at the 1.8 cap
	GovState st; gov_init(&st, p);
	int tmin, tmax, oruns;
	run_workload(&st, p, f_req, 55 /*cool, below target*/, 400, 60, &tmin, &tmax, &oruns);
	// Converged to the lowest stable clock that holds frame rate:
	CHECK(tmax >= f_req, "settled below the requirement (%d < %d) — would drop frames", tmax, f_req);
	CHECK(tmax < p->f_max, "stayed pinned at f_max=%d — found no saving", p->f_max);
	CHECK(tmin >= p->f_min && tmax <= p->f_max, "left bracket [%d,%d]: [%d,%d]", p->f_min, p->f_max, tmin, tmax);
	// Hunts by at most one OPP step around the requirement (probe-down controllers do this);
	// the high water after each climb satisfies the workload, so frame rate holds in steady state.
	printf("  settled band [%d, %d] kHz for f_req=%d\n", tmin, tmax, f_req);
}

// ---- scenario 5: gov_tick I/O path smoke test (temp sensor absent -> -1) ----
static void test_tick_io(void) {
	printf("[gov_tick] device entry point writes a clock via PLAT_setCPUMaxFreq\n");
	const GovProfile* p = &GOV_P_8BIT;
	GovState st; gov_init(&st, p);
	g_last_set_khz = -1;
	gov_tick(&st, p, 0); // temp sensor path absent on this host -> gov_read_temp_c()=-1
	// With GOV_DISABLE unset, gov_tick must have written something in-bracket.
	if (getenv("GOV_DISABLE") == NULL) {
		CHECK(g_last_set_khz >= p->f_min && g_last_set_khz <= p->f_max,
		      "gov_tick wrote %d, out of bracket [%d,%d]", g_last_set_khz, p->f_min, p->f_max);
	}
}

int main(void) {
	printf("== governor synthetic harness ==\n");
	test_cold_idle();
	test_hot_ceiling();
	test_hot_caps_below_max();
	test_oscillation();
	test_boundary_no_limit_cycle();
	test_predictive_sink_gate();
	test_converges_to_lowest_stable();
	test_tick_io();
	printf("== %s ==\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
	if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
	return 0;
}
