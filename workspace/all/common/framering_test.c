// framering_test.c — adversarial harness for framering (threading v2.4 contract).
//
// One synthetic CORE thread (the "fake core") runs randomized epochs and service
// ops against a MAIN driver that grants, drains, parks, changes depth, requests
// services, discards, and stops on randomized schedules. Every contract invariant
// the module promises is asserted here; the TSan/ASan builds adjudicate races.
//
//   framering_test [seconds] [seed]     (defaults: 3 seconds, time-derived seed)
//
// The seed is printed first — any failure reproduces with the same seed.

#include "framering.h"

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ---- deterministic PRNG (one stream per thread, seeded from the master) --------
typedef struct { uint64_t s; } rng_t;
static uint64_t rng_next(rng_t* r) {
	uint64_t x = r->s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return r->s = x;
}
static uint32_t rng_below(rng_t* r, uint32_t n) { return (uint32_t)(rng_next(r) % n); }

// payload checksum: consumer recomputes and asserts (catches reorder/corruption)
static uint64_t chk(uint32_t kind, uint64_t seq, uint32_t index) {
	uint64_t x = ((uint64_t)kind << 56) ^ (seq * 0x9E3779B97F4A7C15ull) ^ index;
	x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33;
	return x;
}

// ---- shared accounting ----------------------------------------------------------
typedef struct {
	fr_ring fr;
	rng_t core_rng;

	// producer-side truth (atomics: read by MAIN for final ledgers)
	_Atomic uint64_t cmds_emitted;        // incl. overflow-published
	_Atomic uint64_t persistent_emitted;
	_Atomic uint64_t barriers_emitted;
	_Atomic uint64_t frames_emitted;      // enqueued frames (drops excluded)
	_Atomic uint64_t frames_dropped;      // FR_DROPPED under ABORTING
	_Atomic uint64_t rundones_published;
	_Atomic uint64_t svc_ops_done;
	_Atomic uint64_t svc_events_emitted;
	_Atomic uint64_t grants_consumed;
	_Atomic uint64_t cmds_overflowed;     // FR_ABORT returns = took the ovf path

	// consumer-side truth (MAIN thread only)
	uint64_t cmds_applied;
	uint64_t persistent_applied;
	uint64_t barriers_applied_cb;
	uint64_t frames_applied;
	uint64_t rundones_seen;
	uint64_t svc_events_applied;
	uint64_t cancelled_seen;

	// ordering state (MAIN thread only). Cancelled-unrun grants make gen values
	// SKIP — order is asserted as strict monotonicity plus boundary discipline,
	// never as arithmetic contiguity.
	uint64_t cur_epoch;        // epoch whose events are currently being delivered
	uint32_t next_run_index;
	uint64_t last_boundary;    // last RUN_DONE seq delivered (epoch closed)
	uint64_t cur_svc;
	uint32_t next_svc_index;

	int discard_mode;
} H;

static H h;

// invariant counter for the report: each ASSERT_INV site is one contract invariant
static int inv_hits[32];
#define ASSERT_INV(n, cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "INVARIANT %d FAILED: %s\n", (n), (msg)); abort(); } \
	inv_hits[n]++; \
} while (0)

// ---- consumer drain callback: every applied event flows through here, in order --
static void on_event(void* ctx, const fr_event* ev) {
	(void)ctx;
	switch (ev->kind) {
	case FR_EV_FRAME:
	case FR_EV_CMD: {
		// INV1: payload integrity (checksum over kind/seq/index)
		ASSERT_INV(1, ev->payload == chk(ev->kind, ev->seq, ev->index),
		           "event payload corrupted or reordered");
		// INV2: no event from an epoch at or before the last closed boundary
		ASSERT_INV(2, ev->seq > h.last_boundary, "event from an already-closed epoch");
		if (ev->seq != h.cur_epoch) {
			// INV3: opening a new epoch's events requires the previous one closed
			ASSERT_INV(3, h.cur_epoch <= h.last_boundary,
			           "new epoch's events before previous epoch's RUN_DONE");
			h.cur_epoch = ev->seq;
			h.next_run_index = 0;
		}
		// INV4: intra-epoch emission order preserved (strictly monotonic index).
		// Gaps are legal ONLY as discard-skipped or abort-dropped frames — losses
		// of commands/persistent events are caught by the final ledgers (INV18/19).
		if (ev->index < h.next_run_index)
			fprintf(stderr, "DBG INV4: kind=%u seq=%llu index=%u expected=%u cur_epoch=%llu last_boundary=%llu discard=%d\n",
			        ev->kind, (unsigned long long)ev->seq, ev->index, h.next_run_index,
			        (unsigned long long)h.cur_epoch, (unsigned long long)h.last_boundary, h.discard_mode);
		ASSERT_INV(4, ev->index >= h.next_run_index, "intra-epoch order broken (reorder/dup)");
		h.next_run_index = ev->index + 1;
		// INV5: DISCARD drains never deliver droppable frame payload
		if (h.discard_mode)
			ASSERT_INV(5, ev->kind != FR_EV_FRAME || (ev->flags & FR_EVF_PERSISTENT),
			           "discard drain delivered droppable frame payload");
		if (ev->kind == FR_EV_CMD) {
			h.cmds_applied++;
			if (ev->flags & FR_EVF_PERSISTENT) h.persistent_applied++;
		} else h.frames_applied++;
		if (ev->flags & FR_EVF_BARRIER) h.barriers_applied_cb++;
		break;
	}
	case FR_EV_RUN_DONE: {
		// INV6: boundaries strictly monotonic; INV7: never before its own events
		ASSERT_INV(6, ev->seq > h.last_boundary, "RUN_DONE seq not monotonic");
		ASSERT_INV(7, h.cur_epoch <= ev->seq, "boundary delivered before its epoch's events");
		h.last_boundary = ev->seq;
		h.cur_epoch = ev->seq;   // epoch closed
		h.rundones_seen++;
		if (ev->cancelled) h.cancelled_seen++;
		break;
	}
	case FR_EV_SERVICE: {
		ASSERT_INV(8, ev->payload == chk(ev->kind, ev->seq, ev->index),
		           "service payload corrupted");
		ASSERT_INV(9, ev->seq >= h.cur_svc, "svc_seq went backwards");
		if (ev->seq != h.cur_svc) { h.cur_svc = ev->seq; h.next_svc_index = 0; }
		// INV10: service emission order preserved (own namespace, own index)
		ASSERT_INV(10, ev->index == h.next_svc_index, "service emission order broken");
		h.next_svc_index = ev->index + 1;
		h.svc_events_applied++;
		break;
	}
	default:
		fprintf(stderr, "unknown event kind %u\n", ev->kind); abort();
	}
}

static int drain(int mode) {
	h.discard_mode = (mode == FR_DRAIN_DISCARD);
	int n = fr_drain(&h.fr, on_event, NULL, mode);
	h.discard_mode = 0;
	return n;
}
static void drain_cb_park(void* ctx, const fr_event* ev) { (void)ctx; on_event(NULL, ev); }

// ---- the fake core ----------------------------------------------------------------
static void* fake_core(void* arg) {
	(void)arg;
	rng_t* r = &h.core_rng;
	uint64_t snap[4];
	for (;;) {
		if (fr_get_state(&h.fr) == FR_QUIESCENT) {
			uint64_t op = 0;
			fr_rc rc = fr_core_service_next(&h.fr, &op);
			if (rc == FR_STOP) break;
			if (rc == FR_RELEASED) continue;
			// FR_SVC: emit 0..5 service events (an unserialize emits frames), ack.
			// Park requests arriving MID-OP are absorbed — these drain-driven
			// emits must never cancel (F30 no-park-edge proof).
			uint32_t n = rng_below(r, 9);  // service stream (cap 4 in tests) must fill
			uint64_t seq = atomic_load(&h.fr.svc_seq);
			for (uint32_t i = 0; i < n; i++) {
				fr_core_service_emit(&h.fr, 0, chk(FR_EV_SERVICE, seq, i));
				atomic_fetch_add(&h.svc_events_emitted, 1);
			}
			fr_core_service_ack(&h.fr);
			atomic_fetch_add(&h.svc_ops_done, 1);
			continue;
		}
		uint64_t gen = 0;
		fr_rc rc = fr_core_wait_grant(&h.fr, &gen, snap);
		if (rc == FR_STOP) break;
		if (rc == FR_PARKED) continue;
		assert(rc == FR_GRANT);
		atomic_fetch_add(&h.grants_consumed, 1);
		// INV11: the granted snapshot is exactly what MAIN wrote for this gen —
		// slot ownership means nothing mutated it while this epoch held the credit
		ASSERT_INV(11, snap[0] == gen && snap[3] == chk(99, gen, 7),
		           "input snapshot mutated while owned / wrong slot");

		// "retro_run": randomized product mix; geometry storms, barrier waits,
		// zero-event epochs, multi-frame epochs all fall out of the dice.
		uint32_t emitted_frames = 0;
		uint32_t idx = 0;
		int did_barrier = 0;
		uint32_t nev = rng_below(r, 14); // 0..13 products (tiny test rings WILL fill)
		for (uint32_t i = 0; i < nev; i++) {
			uint32_t roll = rng_below(r, 100);
			if (roll < 40) {
				fr_rc erc = fr_core_emit(&h.fr, FR_EV_FRAME, 0, chk(FR_EV_FRAME, gen, idx));
				if (erc == FR_OK) { idx++; emitted_frames++; atomic_fetch_add(&h.frames_emitted, 1); }
				else { assert(erc == FR_DROPPED); atomic_fetch_add(&h.frames_dropped, 1); }
			} else if (roll < 75) {
				uint32_t flags = (roll < 55) ? 0 : FR_EVF_PERSISTENT;
				fr_rc erc = fr_core_emit(&h.fr, FR_EV_CMD, flags, chk(FR_EV_CMD, gen, idx));
				assert(erc == FR_OK || erc == FR_ABORT);  // commands never dropped
				if (erc == FR_ABORT) atomic_fetch_add(&h.cmds_overflowed, 1);
				idx++;
				atomic_fetch_add(&h.cmds_emitted, 1);
				if (flags & FR_EVF_PERSISTENT) atomic_fetch_add(&h.persistent_emitted, 1);
			} else if (roll < 85 && !did_barrier) {
				// AV_INFO-class: persistent barrier, then wait for application
				fr_rc erc = fr_core_emit(&h.fr, FR_EV_CMD, FR_EVF_PERSISTENT | FR_EVF_BARRIER,
				                         chk(FR_EV_CMD, gen, idx));
				assert(erc == FR_OK || erc == FR_ABORT);
				idx++;
				atomic_fetch_add(&h.cmds_emitted, 1);
				atomic_fetch_add(&h.persistent_emitted, 1);
				atomic_fetch_add(&h.barriers_emitted, 1);
				fr_rc brc = fr_core_barrier_wait(&h.fr);
				// INV12: an OK barrier wait implies MAIN really applied it — the
				// prefix-drain proof: this epoch is still open (no RUN_DONE yet)
				if (brc == FR_OK)
					ASSERT_INV(12, atomic_load(&h.fr.barrier_applied)
					               >= atomic_load(&h.fr.barrier_emitted),
					           "barrier wait returned before application");
				did_barrier = 1;
			} else {
				struct timespec ts = { 0, (long)rng_below(r, 40000) };
				nanosleep(&ts, NULL);  // jitter widens interleavings
			}
		}
		fr_outcome out = emitted_frames ? FR_OUT_FRAME
		                                : (rng_below(r, 2) ? FR_OUT_DUP : FR_OUT_NONE);
		rc = fr_core_run_done(&h.fr, out, 0);
		atomic_fetch_add(&h.rundones_published, 1);
		if (rc == FR_STOP) break;
	}
	fr_core_thread_exit(&h.fr);
	return NULL;
}

// ---- MAIN driver --------------------------------------------------------------------
static void settle_checks(const char* where) {
	// post-park (transition gate steps 4-6): everything home, ledger settled
	ASSERT_INV(13, fr_credits_out(&h.fr) == 0, where);
	ASSERT_INV(14, atomic_load(&h.fr.slot_owned) == 0, where);
	ASSERT_INV(15, atomic_load(&h.fr.barrier_applied) == atomic_load(&h.fr.barrier_emitted),
	           "pending barrier after park ack (persistent ledger unsettled)");
	ASSERT_INV(16, fr_get_state(&h.fr) == FR_QUIESCENT || fr_get_state(&h.fr) == FR_STOPPED,
	           "park returned while producer not parked");
}

int main(int argc, char** argv) {
	int seconds = argc > 1 ? atoi(argv[1]) : 3;
	uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0)
	                         : (uint64_t)time(NULL) * 2654435761u;
	printf("framering_test: seconds=%d seed=0x%016" PRIx64 "\n", seconds, seed);
	fflush(stdout);
	rng_t mr = { seed ? seed : 1 };
	h.core_rng.s = rng_next(&mr) | 1;
	rng_t r = { rng_next(&mr) | 1 };

	fr_init(&h.fr, 1);
	h.fr.failclosed_sec = 60;  // generous under sanitizers

	pthread_t core;
	pthread_create(&core, NULL, fake_core, NULL);

	// bootstrap ritual: service ops from QUIESCENT (the state CORE is born into,
	// F31), then release to RUNNING.
	fr_service(&h.fr, 0xB007, drain_cb_park, NULL);
	fr_service(&h.fr, 0xB008, drain_cb_park, NULL);
	fr_release(&h.fr);

	struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
	uint64_t iter = 0;
	uint32_t cur_depth = 1;
	for (;;) {
		struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - t0.tv_sec >= seconds) break;
		iter++;

		// issue grants while credits allow (snapshot: gen word0, checksum word3)
		for (int gi = 0; gi < 4; gi++) {
			uint64_t snap[4];
			snap[0] = atomic_load(&h.fr.gen) + 1;  // MAIN is the only gen writer
			snap[1] = rng_next(&r);
			snap[2] = rng_next(&r);
			snap[3] = chk(99, snap[0], 7);
			uint64_t g;
			if (fr_grant(&h.fr, snap, &g) != FR_OK) break;
			// INV17: gen advances by exactly +1 per grant
			ASSERT_INV(17, g == snap[0], "gen advanced by other than +1 per grant");
		}

		drain(FR_DRAIN_NORMAL);

		uint32_t roll = rng_below(&r, 1000);
		if (roll < 60) {
			// PARK STORM (some discard): mid-run parks force ABORTING + CANCELLED
			int mode = (roll < 20) ? FR_DRAIN_DISCARD : FR_DRAIN_NORMAL;
			h.discard_mode = (mode == FR_DRAIN_DISCARD);
			fr_park(&h.fr, drain_cb_park, NULL, mode);
			h.discard_mode = 0;
			settle_checks("park storm");
			if (rng_below(&r, 3) == 0) {
				// park while already QUIESCENT: absorbed, no edge (F30)
				fr_park(&h.fr, drain_cb_park, NULL, FR_DRAIN_NORMAL);
				settle_checks("double park absorb");
			}
			if (rng_below(&r, 2) == 0) {
				// service while parked (menu save shape); a park request landing
				// mid-op is absorbed by service_next — no cancellation of the
				// drain-driven emits (F30 proof lives in the module asserts)
				fr_service(&h.fr, 0x5E4C + rng_below(&r, 4), drain_cb_park, NULL);
			}
			fr_release(&h.fr);
		} else if (roll < 75) {
			// DEPTH CHANGE under load (trial start/stop; occasional depth 3)
			uint32_t nd = (cur_depth == 1) ? (rng_below(&r, 8) ? 2 : 3) : 1;
			int mode = rng_below(&r, 2) ? FR_DRAIN_DISCARD : FR_DRAIN_NORMAL;
			h.discard_mode = (mode == FR_DRAIN_DISCARD);
			fr_set_depth(&h.fr, nd, drain_cb_park, NULL, mode);
			h.discard_mode = 0;
			settle_checks("depth change");
			cur_depth = nd;
			fr_release(&h.fr);
		} else if (roll < 90) {
			// slow consumer: forces credit exhaustion + producer full-waits
			struct timespec ts = { 0, (long)rng_below(&r, 2000000) };
			nanosleep(&ts, NULL);
		} else if (roll < 92) {
			fr_wait_events(&h.fr);
		}
	}

	// terminal: park, stop, join (reversible->terminal split; producer exits at
	// a boundary; never destroy while a worker may live)
	fr_park(&h.fr, drain_cb_park, NULL, FR_DRAIN_NORMAL);
	settle_checks("terminal park");
	fr_stop(&h.fr);
	pthread_join(core, NULL);
	drain(FR_DRAIN_NORMAL);  // anything published between park and exit

	// ---- final ledgers ------------------------------------------------------------
	uint64_t ce = atomic_load(&h.cmds_emitted);
	uint64_t pe = atomic_load(&h.persistent_emitted);
	uint64_t be = atomic_load(&h.barriers_emitted);
	uint64_t rp = atomic_load(&h.rundones_published);
	uint64_t gc = atomic_load(&h.grants_consumed);
	uint64_t se = atomic_load(&h.svc_events_emitted);

	// INV18: commands never lost (incl. ABORTING overflow path)
	ASSERT_INV(18, h.cmds_applied == ce, "command lost (emitted != applied)");
	// INV19: persistent events applied on EVERY path incl. discard drains
	ASSERT_INV(19, h.persistent_applied == pe, "persistent event lost");
	// INV20: every barrier applied exactly once
	ASSERT_INV(20, h.barriers_applied_cb == be, "barrier application count mismatch");
	// INV21: every consumed grant produced exactly one delivered RUN_DONE
	ASSERT_INV(21, h.rundones_seen == rp && rp == gc, "grant/RUN_DONE accounting broken");
	// INV22: service events all delivered
	ASSERT_INV(22, h.svc_events_applied == se, "service event lost");
	// INV23: credit conservation at end
	ASSERT_INV(23, fr_credits_out(&h.fr) == 0 && atomic_load(&h.fr.slot_owned) == 0,
	           "credits/slots leaked");
	// INV24: svc_seq advanced exactly once per completed service op
	ASSERT_INV(24, atomic_load(&h.fr.svc_seq) == atomic_load(&h.svc_ops_done),
	           "svc_seq mismatch");

	fr_destroy(&h.fr);

	int sites = 0;
	for (int i = 0; i < 32; i++) if (inv_hits[i]) sites++;
	printf("cmds_via_overflow=%" PRIu64 "\n", atomic_load(&h.cmds_overflowed));
	printf("iters=%" PRIu64 " grants=%" PRIu64 " rundones=%" PRIu64 " cancelled=%" PRIu64
	       " frames=%" PRIu64 " (+%" PRIu64 " dropped-under-abort) cmds=%" PRIu64
	       " persistent=%" PRIu64 " barriers=%" PRIu64
	       " svc_ops=%" PRIu64 " svc_events=%" PRIu64 "\n",
	       iter, gc, h.rundones_seen, h.cancelled_seen,
	       h.frames_applied, atomic_load(&h.frames_dropped), h.cmds_applied,
	       h.persistent_applied, h.barriers_applied_cb,
	       atomic_load(&h.svc_ops_done), h.svc_events_applied);
	printf("framering_test: ALL PASS (%d invariant sites exercised)\n", sites);
	return 0;
}
