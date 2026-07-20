// gov_memory_test.c — host matrix for the extracted governor-memory policy
// (review round 4 joint decision). Build: cc gov_memory.c gov_memory_test.c
#include <stdio.h>
#include "gov_memory.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const GovProfile WIDE  = { 600000, 1416000 };
static const GovProfile FIXED = { 1008000, 1008000 };

// convenience: one clean SLACK tick with a fresh publication
static int tick_slack_fresh(GovMemState* m, unsigned* seq, int ceil) {
	(*seq)++;
	return govmem_post_tick(m, GOV_SIGNAL_SLACK, 0, *seq, ceil, ceil);
}

static void test_initial_geometry_preserves_arm(void) {
	printf("[case 1] initial geometry preserves the arm\n");
	GovMemState m; govmem_init(&m, 1, 768000, &WIDE);
	CHECK(govmem_arm_khz(&m) == 984000, "arm %d", govmem_arm_khz(&m));
	govmem_on_burst(&m, 0); // first geometry: burst, no cancel
	CHECK(govmem_arm_khz(&m) == 984000, "initial geometry canceled the arm");
}
static void test_later_geometry_cancels_and_dirties(void) {
	printf("[case 2] later geometry cancels arm and invalidates the window\n");
	GovMemState m; govmem_init(&m, 1, 768000, &WIDE);
	unsigned seq = 0;
	tick_slack_fresh(&m, &seq, 1416000); // clean window begins
	govmem_on_burst(&m, 1);              // real scene change
	CHECK(govmem_arm_khz(&m) == 0, "arm survived a real geometry change");
	seq++;
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1416000, 1416000);
	{	// the burst window must not vote AT ALL — zero total votes, not merely few
		uint32_t total = 0;
		for (int i = 0; i < GOVMEM_HIST; i++) total += m.hist_n[i];
		CHECK(total == 0, "mixed window voted (%u votes)", total);
	}
}
static void test_ff_invalidates_and_cancels(void) {
	printf("[case 3] FF entry/exit invalidates the window and cancels the arm\n");
	GovMemState m; govmem_init(&m, 1, 768000, &WIDE);
	unsigned seq = 0;
	tick_slack_fresh(&m, &seq, 1416000);
	govmem_on_burst(&m, 1); // FF toggle site: dirty + cancel
	CHECK(govmem_arm_khz(&m) == 0, "arm survived FF toggle");
	seq++;
	int votes_before = (int)m.hist_n[0];
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1416000, 1416000); // post-FF mixed window
	CHECK((int)m.hist_n[0] == votes_before, "post-FF mixed publication voted");
}
static void test_slip_disarms(void) {
	printf("[case 4] SLIP/BIGSLIP disarm\n");
	GovMemState m; govmem_init(&m, 1, 768000, &WIDE);
	unsigned seq = 1;
	int r = govmem_post_tick(&m, GOV_SIGNAL_SLIP, 0, seq, 1416000, 1416000);
	CHECK(r & GOVMEM_DISARMED_SLIP, "slip did not disarm");
	CHECK(govmem_arm_khz(&m) == 0, "arm survived slip");
	govmem_init(&m, 1, 768000, &WIDE);
	r = govmem_post_tick(&m, GOV_SIGNAL_BIGSLIP, 0, seq, 1416000, 1416000);
	CHECK(r & GOVMEM_DISARMED_SLIP, "bigslip did not disarm");
}
static void test_one_preload_per_publication(void) {
	printf("[case 5] one accelerated action per fresh rate publication\n");
	GovMemState m; govmem_init(&m, 1, 768000, &WIDE);
	unsigned seq = 1;
	int r1 = govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1416000, 1416000);
	int r2 = govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1416000, 1416000); // same seq
	CHECK(r1 & GOVMEM_PRELOAD_DWELL, "first fresh sample did not preload");
	CHECK(!(r2 & GOVMEM_PRELOAD_DWELL), "second preload rode the same stale sample");
	seq++;
	int r3 = govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1200000, 1200000);
	CHECK(r3 & GOVMEM_PRELOAD_DWELL, "fresh sample did not re-enable the ladder");
}
static void test_dirty_publications_never_vote(void) {
	printf("[case 6] dirty publications never vote\n");
	GovMemState m; govmem_init(&m, 1, 0, &WIDE); // no arm: voting only
	unsigned seq = 0;
	tick_slack_fresh(&m, &seq, 1416000); // window 1: initial dirty -> publication starts clean window, no vote
	CHECK(govmem_best(&m, 1) == 0, "voted from the initial dirty window");
	seq++;
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1416000, 1200000); // tick moves ceiling: dirty AND publication
	CHECK(govmem_best(&m, 1) == 0, "voted from a window the tick moved");
}
static void test_stable_window_votes_correct_ceiling(void) {
	printf("[case 7] stable publications vote once, for the producing ceiling\n");
	GovMemState m; govmem_init(&m, 1, 0, &WIDE);
	unsigned seq = 0;
	tick_slack_fresh(&m, &seq, 1200000); // clears initial dirty
	tick_slack_fresh(&m, &seq, 1200000); // clean window: votes 1200000
	CHECK(govmem_best(&m, 1) == 1200000, "vote %d", govmem_best(&m, 1));
	CHECK(m.hist_n[0] == 1, "voted more than once per publication: %u", m.hist_n[0]);
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1200000, 1200000); // same seq: no double vote
	CHECK(m.hist_n[0] == 1, "same publication voted twice");
}
static void test_expiry_and_reached(void) {
	printf("[case 8] arm expiry and remembered-target completion\n");
	GovMemState m; govmem_init(&m, 1, 768000, &WIDE);
	int r = 0;
	for (int i = 0; i < 300 && !(r & GOVMEM_EXPIRED); i++)
		r = govmem_post_tick(&m, GOV_SIGNAL_BUSY, 0, 1, 1416000, 1416000); // BUSY: never preloads, ages out
	CHECK(r & GOVMEM_EXPIRED, "arm never expired");
	govmem_init(&m, 1, 768000, &WIDE);
	r = govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, 1, 984000, 984000); // already at target
	CHECK(r & GOVMEM_REACHED, "reaching the target did not disarm");
	CHECK(govmem_arm_khz(&m) == 0, "arm survived completion");
}
static void test_disabled_and_fixed_brackets(void) {
	printf("[case 9] GOV_DISABLE and fixed brackets are complete no-ops\n");
	GovMemState m; govmem_init(&m, 0, 768000, &WIDE); // caller passes enabled=0 for GOV_DISABLE
	CHECK(govmem_arm_khz(&m) == 0, "disabled unit armed");
	unsigned seq = 0;
	CHECK(tick_slack_fresh(&m, &seq, 1200000) == 0, "disabled unit acted");
	tick_slack_fresh(&m, &seq, 1200000);
	CHECK(govmem_best(&m, 1) == 0, "disabled unit voted");
	govmem_init(&m, 1, 1008000, &FIXED);
	CHECK(govmem_arm_khz(&m) == 0, "fixed bracket armed");
	CHECK(tick_slack_fresh(&m, &seq, 1008000) == 0, "fixed bracket acted");
}
static void test_bad_sidecars(void) {
	printf("[case 10] invalid, out-of-range, and missing sidecar values\n");
	int bad[] = { 0, -1, 599999, 1416000, 1800000, 123, 2000000000 };
	for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
		GovMemState m; govmem_init(&m, 1, bad[i], &WIDE);
		CHECK(govmem_arm_khz(&m) == 0, "armed on remembered=%d", bad[i]);
	}
	GovMemState m; govmem_init(&m, 1, 600000, &WIDE); // f_min itself is valid
	CHECK(govmem_arm_khz(&m) == 816000, "f_min floor arm %d", govmem_arm_khz(&m));
}
static void test_multiple_moves_one_publication(void) {
	printf("[case 11] multiple ceiling changes before one publication\n");
	GovMemState m; govmem_init(&m, 1, 0, &WIDE);
	unsigned seq = 0;
	tick_slack_fresh(&m, &seq, 1416000); // clean start
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1416000, 1200000); // move 1, same window
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1200000, 984000);  // move 2, same window
	seq++;
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 984000, 984000);   // publication spans both moves
	CHECK(govmem_best(&m, 1) == 0, "publication spanning two moves voted");
	seq++;
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 984000, 984000);   // next window is clean
	CHECK(govmem_best(&m, 1) == 984000, "clean follow-up window failed to vote");
}
static void test_burst_at_fmax_still_invalidates(void) {
	printf("[case 12] a burst at an unchanged f_max ceiling still invalidates the window\n");
	GovMemState m; govmem_init(&m, 1, 0, &WIDE);
	unsigned seq = 0;
	tick_slack_fresh(&m, &seq, 1416000);   // clean window running at f_max
	govmem_on_burst(&m, 0);                 // ceiling stays 1416000 — workload changed anyway
	seq++;
	govmem_post_tick(&m, GOV_SIGNAL_SLACK, 0, seq, 1416000, 1416000);
	CHECK(govmem_best(&m, 1) == 0, "f_max burst window voted");
}

int main(void) {
	printf("== governor-memory policy matrix ==\n");
	test_initial_geometry_preserves_arm();
	test_later_geometry_cancels_and_dirties();
	test_ff_invalidates_and_cancels();
	test_slip_disarms();
	test_one_preload_per_publication();
	test_dirty_publications_never_vote();
	test_stable_window_votes_correct_ceiling();
	test_expiry_and_reached();
	test_disabled_and_fixed_brackets();
	test_bad_sidecars();
	test_multiple_moves_one_publication();
	test_burst_at_fmax_still_invalidates();
	if (failures) { printf("== %d FAILURES ==\n", failures); return 1; }
	printf("== ALL PASS ==\n");
	return 0;
}
