// framering.c — see framering.h. Executable form of docs/threading-v2-design.md v2.4.
//
// Locking model (perf contract): the SPSC rings run on C11 acquire/release atomics —
// the steady-state emit/drain path takes no lock and performs no kernel wake. The
// mutex+condvars exist ONLY to sleep and wake on: pipeline empty, pipeline full,
// lifecycle transitions, and barrier publication. A wake is issued only when the
// other side has announced it is (or may be about to be) sleeping; the waiter
// re-checks its predicate under the lock before sleeping (standard missed-wake-safe
// eventcount), so a spurious extra wake is possible but a lost wake is not.

#include "framering.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LD(a)     atomic_load_explicit(&(a), memory_order_acquire)
#define ST(a, v)  atomic_store_explicit(&(a), (v), memory_order_release)
#define LDRLX(a)  atomic_load_explicit(&(a), memory_order_relaxed)

#define ST_MASK  (FR_STREAM_CAP - 1)
#define SV_MASK  (FR_SVC_STREAM - 1)
#define GQ_MASK  (FR_GRANTQ_CAP - 1)
#define SQ_MASK  (FR_SVCQ_CAP - 1)

// ---- wake plumbing ---------------------------------------------------------

static void wake_cons(fr_ring* fr) {
	if (LD(fr->cons_waiting)) {
		pthread_mutex_lock(&fr->lk);
		pthread_cond_broadcast(&fr->cv_cons);
		pthread_mutex_unlock(&fr->lk);
	}
}
static void wake_prod(fr_ring* fr) {
	if (LD(fr->prod_waiting)) {
		pthread_mutex_lock(&fr->lk);
		pthread_cond_broadcast(&fr->cv_prod);
		pthread_mutex_unlock(&fr->lk);
	}
}

// Fail-closed guard for drain-driven waits (design: "fail-closed timeouts bound the
// pathological case"). A liveness backstop, not a proof — v2.4 states this openly.
static void failclosed(fr_ring* fr, const char* where) {
	fprintf(stderr, "framering: FAIL-CLOSED — drain-driven wait starved >%ds in %s "
	        "(MAIN stopped draining; leak-and-exit per contract)\n",
	        fr->failclosed_sec, where);
	abort();
}

// producer sleep: wait until pred(fr) true; returns immediately when already true.
// Every RUNNING wait's predicate must include park/stop (wait-graph rule 1); the
// PREDICATE captures that — this helper only sleeps and re-checks.
typedef int (*fr_pred)(fr_ring* fr);
static void prod_sleep(fr_ring* fr, fr_pred pred, int drain_driven, const char* where) {
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	pthread_mutex_lock(&fr->lk);
	ST(fr->prod_waiting, 1);
	while (!pred(fr)) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (drain_driven && ts.tv_sec - start.tv_sec > fr->failclosed_sec) {
			pthread_mutex_unlock(&fr->lk);
			failclosed(fr, where);
		}
		ts.tv_sec += 1;
		pthread_cond_timedwait(&fr->cv_prod, &fr->lk, &ts);
	}
	ST(fr->prod_waiting, 0);
	pthread_mutex_unlock(&fr->lk);
}

// ---- init/destroy -----------------------------------------------------------

void fr_init(fr_ring* fr, uint32_t depth) {
	assert(depth >= 1 && depth <= FR_MAX_DEPTH);
	memset(fr, 0, sizeof(*fr));
	fr->depth = depth;
	fr->failclosed_sec = 10;
	pthread_mutex_init(&fr->lk, NULL);
	pthread_cond_init(&fr->cv_prod, NULL);
	pthread_cond_init(&fr->cv_cons, NULL);
	// CORE is born QUIESCENT (bootstrap state machine: CORE_CREATED starts parked;
	// the bootstrap sequence runs as service ops; fr_release enters RUNNING).
	ST(fr->state, FR_QUIESCENT);
}

void fr_destroy(fr_ring* fr) {
	assert(fr_get_state(fr) == FR_STOPPED); // never destroy while a worker may live
	pthread_mutex_destroy(&fr->lk);
	pthread_cond_destroy(&fr->cv_prod);
	pthread_cond_destroy(&fr->cv_cons);
}

// ---- MAIN: grants -------------------------------------------------------------

fr_rc fr_grant(fr_ring* fr, const uint64_t snapshot[4], uint64_t* out_gen) {
	if (LD(fr->park_req) || LD(fr->stop_req)) return FR_NOSPACE;
	int st = fr_get_state(fr);
	if (st != FR_RUNNING) return FR_NOSPACE;
	if (LD(fr->credits_out) >= fr->depth) return FR_NOSPACE;  // pipeline full

	uint64_t g = LDRLX(fr->gen) + 1;  // gen advances exactly once per RUN grant
	uint32_t slot = (uint32_t)(g % fr->depth);
	uint32_t owned = LD(fr->slot_owned);
	assert(!(owned & (1u << slot)) && "snapshot slot still owned at grant");

	fr->snap[slot].gen = g;
	memcpy(fr->snap[slot].data, snapshot, sizeof(fr->snap[slot].data));
	ST(fr->slot_owned, owned | (1u << slot));
	atomic_fetch_add_explicit(&fr->credits_out, 1, memory_order_acq_rel);

	uint32_t t = LDRLX(fr->grant_tail);
	fr->grantq[t & GQ_MASK].gen  = g;
	fr->grantq[t & GQ_MASK].slot = slot;
	ST(fr->grant_tail, t + 1);
	ST(fr->gen, g);
	if (out_gen) *out_gen = g;
	wake_prod(fr);  // pipeline may have been empty
	return FR_OK;
}

// ---- MAIN: drain ---------------------------------------------------------------

static void credit_return(fr_ring* fr, uint64_t g) {
	uint32_t slot = 0;
	int found = 0;
	for (uint32_t i = 0; i < FR_MAX_DEPTH; i++)
		if (fr->snap[i].gen == g && (LD(fr->slot_owned) & (1u << i))) { slot = i; found = 1; break; }
	assert(found && "credit return for unknown epoch");
	ST(fr->slot_owned, LD(fr->slot_owned) & ~(1u << slot));
	uint32_t before = atomic_fetch_sub_explicit(&fr->credits_out, 1, memory_order_acq_rel);
	assert(before >= 1 && "credit underflow");
	wake_prod(fr);  // a credit / RUN_DONE slot freed
}

int fr_drain(fr_ring* fr, fr_drain_cb cb, void* ctx, int mode) {
	int applied = 0;
	// Service stream first — its own namespace, never dependent on run-epoch
	// progress (rule 2: this runs on EVERY drain, including park/ack wait loops).
	for (;;) {
		uint32_t h = LDRLX(fr->sv_head);
		if (h == LD(fr->sv_tail)) break;
		fr_event ev = fr->svc_stream[h & SV_MASK];
		ST(fr->sv_head, h + 1);
		wake_prod(fr);
		cb(ctx, &ev); applied++;
		if (ev.flags & FR_EVF_BARRIER) {
			atomic_fetch_add_explicit(&fr->barrier_applied, 1, memory_order_acq_rel);
			wake_prod(fr);
		}
	}
	// Run-epoch events + RUN_DONE boundaries, merged in GLOBAL order by gen:
	// all of epoch g's stream events precede RUN_DONE(g); RUN_DONE(g) precedes any
	// event of epoch > g. Includes prefix-drain of the open epoch (F29): stream
	// events are consumed whether or not their RUN_DONE exists yet.
	for (;;) {
		// READ ORDER MATTERS (TSan-found TOCTOU): the boundary queue is read FIRST —
		// acquiring rd_tail synchronizes with the producer's release, so every
		// stream event of a visible boundary's epoch is guaranteed visible when the
		// stream is examined afterwards. Reading the stream first can miss events
		// published between the two loads and deliver their boundary early.
		uint32_t rh = LDRLX(fr->rd_head);
		int rd_avail = rh != LD(fr->rd_tail);
		uint32_t sh = LDRLX(fr->st_head);
		int st_avail = sh != LD(fr->st_tail);
		if (st_avail) {
			fr_event ev = fr->stream[sh & ST_MASK];
			if (!rd_avail || ev.seq <= fr->rdq[rh & (FR_MAX_DEPTH - 1)].seq) {
				ST(fr->st_head, sh + 1);
				wake_prod(fr);  // stream space freed
				int skip = (mode == FR_DRAIN_DISCARD) && ev.kind == FR_EV_FRAME
				           && !(ev.flags & FR_EVF_PERSISTENT);
				if (!skip) { cb(ctx, &ev); applied++; }
				if (ev.flags & FR_EVF_BARRIER) {
					// apply-then-release: barriers are never skipped (persistent
					// class by construction); CORE's barrier wait releases HERE,
					// out of the OPEN epoch — never waiting on RUN_DONE (F29).
					atomic_fetch_add_explicit(&fr->barrier_applied, 1, memory_order_acq_rel);
					wake_prod(fr);
				}
				continue;
			}
		}
		if (rd_avail) {
			// no remaining stream event belongs to this or an earlier epoch:
			// deliver the boundary. (SPSC publication order guarantees all of
			// epoch g's events were visible before its RUN_DONE.)
			fr_event ev = fr->rdq[rh & (FR_MAX_DEPTH - 1)];
			ST(fr->rd_head, rh + 1);
			cb(ctx, &ev); applied++;
			// drain ack precedes credit return; snapshot slot lives until here
			credit_return(fr, ev.seq);
			continue;
		}
		break;
	}
	return applied;
}

static int pred_cons_something(fr_ring* fr) {
	return LDRLX(fr->st_head) != LD(fr->st_tail)
	    || LDRLX(fr->sv_head) != LD(fr->sv_tail)
	    || LDRLX(fr->rd_head) != LD(fr->rd_tail)
	    || fr_get_state(fr) == FR_QUIESCENT
	    || fr_get_state(fr) == FR_STOPPED;
}
void fr_wait_events(fr_ring* fr) {
	// Bounded tick, never a barrier: with an empty pipeline and the producer idle
	// awaiting a grant, NOTHING arrives until MAIN acts — an unbounded wait here
	// deadlocks both sides (TSan run 1 hung exactly there). Matches the wait
	// graph: MAIN pacing delays are absolute deadlines, wakeable — never open-ended.
	pthread_mutex_lock(&fr->lk);
	if (!pred_cons_something(fr)) {
		ST(fr->cons_waiting, 1);
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_nsec += 100 * 1000 * 1000;  // 100ms tick max
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		pthread_cond_timedwait(&fr->cv_cons, &fr->lk, &ts);
		ST(fr->cons_waiting, 0);
	}
	pthread_mutex_unlock(&fr->lk);
}

// ---- MAIN: park / release / stop / depth (the transition gate) -----------------

void fr_park(fr_ring* fr, fr_drain_cb cb, void* ctx, int mode) {
	// Lifecycle transitions are lock-serialized (perf contract: locking is legal
	// on lifecycle edges). The harness's first runs proved the lock-free version
	// wrong twice: a producer can consume a stale release in the same instant the
	// observation happens. Under lk, release-consume + state-flip are atomic
	// against this entry and the completion check below.
	pthread_mutex_lock(&fr->lk);
	ST(fr->release_req, 0);           // last-request-wins
	ST(fr->park_req, 1);              // gate step 1: stop issuing grants
	pthread_cond_broadcast(&fr->cv_prod);
	pthread_mutex_unlock(&fr->lk);
	for (;;) {
		// rule 2: the wait loop IS the drain loop — every MAIN->CORE edge keeps
		// freeing whatever CORE could block on (stream, rdq, service stream).
		fr_drain(fr, cb, ctx, mode);
		pthread_mutex_lock(&fr->lk);
		fr_state st = fr_get_state(fr);
		pthread_mutex_unlock(&fr->lk);
		if (st == FR_QUIESCENT || st == FR_STOPPED) {
			// gate step 2: cancel queued-but-unrun grants. Safe now: CORE consumes
			// grants only in RUNNING, and it checks park before consuming.
			uint32_t gh = LD(fr->grant_head), gt = LDRLX(fr->grant_tail);
			while (gt > gh) {
				gt--;
				credit_return(fr, fr->grantq[gt & GQ_MASK].gen);
			}
			ST(fr->grant_tail, gt);
			// drain whatever the aborting epoch published after our last pass
			fr_drain(fr, cb, ctx, mode);
			// gate steps 4-6 verification: streams drained, persistent ledger
			// settled (all barriers applied), every credit home, no slot owned.
			if (LDRLX(fr->st_head) == LD(fr->st_tail)
			 && LDRLX(fr->sv_head) == LD(fr->sv_tail)
			 && LDRLX(fr->rd_head) == LD(fr->rd_tail)
			 && LD(fr->barrier_applied) == LD(fr->barrier_emitted)
			 && LD(fr->credits_out) == 0) {
				pthread_mutex_lock(&fr->lk);
				int still = (fr_get_state(fr) == st);
				if (still) ST(fr->park_req, 0);  // MAIN owns the request: retire it
				pthread_mutex_unlock(&fr->lk);
				if (still) {
					assert(LD(fr->slot_owned) == 0 && "slot owned after full reclaim");
					return;  // park ack: QUIESCENT + ledger settled (F29/F26)
				}
			}
			continue;  // more published between checks; drain again
		}
		// producer still RUNNING/ABORTING: sleep until it publishes or transitions
		pthread_mutex_lock(&fr->lk);
		ST(fr->cons_waiting, 1);
		if (!pred_cons_something(fr)) {
			struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); ts.tv_sec += 1;
			pthread_cond_timedwait(&fr->cv_cons, &fr->lk, &ts);
		}
		ST(fr->cons_waiting, 0);
		pthread_mutex_unlock(&fr->lk);
	}
}

void fr_release(fr_ring* fr) {
	// intent-setter, not an edge; lock-serialized with the producer's consume so
	// park entry-clears can never cross a half-taken release.
	pthread_mutex_lock(&fr->lk);
	ST(fr->release_req, 1);
	pthread_cond_broadcast(&fr->cv_prod);
	pthread_mutex_unlock(&fr->lk);
}

void fr_set_depth(fr_ring* fr, uint32_t depth, fr_drain_cb cb, void* ctx, int mode) {
	assert(depth >= 1 && depth <= FR_MAX_DEPTH);
	fr_park(fr, cb, ctx, mode);          // gate steps 1-6
	assert(LD(fr->credits_out) == 0 && LD(fr->slot_owned) == 0);
	fr->depth = depth;                   // gate step 7: resize while QUIESCENT
	// caller decides when to fr_release (trial protocol owns that moment)
}

void fr_stop(fr_ring* fr) {
	ST(fr->stop_req, 1);
	wake_prod(fr);
}

// ---- MAIN: service ---------------------------------------------------------------

void fr_service(fr_ring* fr, uint64_t op, fr_drain_cb cb, void* ctx) {
	uint32_t t = LDRLX(fr->svcq_tail);
	assert(t - LD(fr->svcq_head) < FR_SVCQ_CAP && "service queue full (MAIN is the bounded producer)");
	fr->svcq[t & SQ_MASK].op = op;
	ST(fr->svcq_tail, t + 1);
	uint64_t want = atomic_fetch_add_explicit(&fr->svc_requested, 1, memory_order_acq_rel) + 1;
	wake_prod(fr);
	// completion: CORE acked our op AND all its service events are applied
	for (;;) {
		fr_drain(fr, cb, ctx, FR_DRAIN_NORMAL);
		if (LD(fr->svc_acked) >= want
		 && LDRLX(fr->sv_head) == LD(fr->sv_tail)) return;
		pthread_mutex_lock(&fr->lk);
		ST(fr->cons_waiting, 1);
		if (!(LD(fr->svc_acked) >= want && LDRLX(fr->sv_head) == LD(fr->sv_tail))
		 && !pred_cons_something(fr)) {
			struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); ts.tv_sec += 1;
			pthread_cond_timedwait(&fr->cv_cons, &fr->lk, &ts);
		}
		ST(fr->cons_waiting, 0);
		pthread_mutex_unlock(&fr->lk);
	}
}

// ---- CORE: RUNNING loop -------------------------------------------------------

static _Thread_local uint32_t cur_index;     // emission index within the epoch/op
static _Thread_local uint64_t cur_gen;

static int pred_grant(fr_ring* fr) {
	return LD(fr->grant_head) != LD(fr->grant_tail)
	    || LD(fr->park_req) || LD(fr->stop_req);
}

fr_rc fr_core_wait_grant(fr_ring* fr, uint64_t* out_gen, uint64_t snap[4]) {
	for (;;) {
		// park checked BEFORE consuming a grant: queued-but-unrun grants stay
		// cancellable (gate step 2 relies on this ordering). Transitions to
		// QUIESCENT are lock-serialized (lifecycle edge).
		if (LD(fr->stop_req)) return FR_STOP;
		if (LD(fr->park_req)) {
			pthread_mutex_lock(&fr->lk);
			ST(fr->state, FR_QUIESCENT);   // entering QUIESCENT consumes the park
			ST(fr->park_req, 0);
			pthread_cond_broadcast(&fr->cv_cons);
			pthread_mutex_unlock(&fr->lk);
			return FR_PARKED;
		}
		uint32_t h = LD(fr->grant_head);
		if (h != LD(fr->grant_tail)) {
			uint64_t g = fr->grantq[h & GQ_MASK].gen;
			uint32_t slot = fr->grantq[h & GQ_MASK].slot;
			ST(fr->grant_head, h + 1);
			ST(fr->in_run, 1);
			cur_gen = g;
			cur_index = 0;
			if (out_gen) *out_gen = g;
			memcpy(snap, fr->snap[slot].data, sizeof(fr->snap[slot].data));
			assert(fr->snap[slot].gen == g && "snapshot slot mutated while owned");
			return FR_GRANT;
		}
		prod_sleep(fr, pred_grant, 0, "wait_grant");
	}
}

static int pred_stream_space(fr_ring* fr) {
	return LDRLX(fr->st_tail) - LD(fr->st_head) < FR_STREAM_CAP
	    || LD(fr->park_req) || LD(fr->stop_req);
}

// move queued overflow entries into the stream while space allows — emission order
// is sacred: nothing may enter the stream ahead of an earlier overflowed command.
static void try_flush_ovf(fr_ring* fr) {
	uint32_t moved = 0;
	while (moved < fr->ovf_n) {
		uint32_t t = LDRLX(fr->st_tail);
		if (t - LD(fr->st_head) >= FR_STREAM_CAP) break;
		fr->stream[t & ST_MASK] = fr->ovf[moved++];
		ST(fr->st_tail, t + 1);
		wake_cons(fr);
	}
	if (moved) {
		memmove(fr->ovf, fr->ovf + moved, (fr->ovf_n - moved) * sizeof(fr->ovf[0]));
		fr->ovf_n -= moved;
	}
}

fr_rc fr_core_emit(fr_ring* fr, uint32_t kind, uint32_t flags, uint64_t payload) {
	assert(kind == FR_EV_FRAME || kind == FR_EV_CMD);
	assert(LD(fr->in_run) && "run-epoch emit outside an epoch");
	int aborting = (fr_get_state(fr) == FR_ABORTING) || LD(fr->park_req) || LD(fr->stop_req);
	if (aborting && fr_get_state(fr) != FR_ABORTING) ST(fr->state, FR_ABORTING);

	for (;;) {
		try_flush_ovf(fr);
		if (fr->ovf_n) {
			// order preservation: while ANY overflow is pending, nothing may take
			// the direct path (harness INV4 found the jump-ahead on first run).
			if (kind == FR_EV_FRAME) return FR_DROPPED;
			assert(fr->ovf_n < FR_OVF_CAP && "command overflow buffer exhausted");
			fr_event* ev = &fr->ovf[fr->ovf_n++];
			ev->kind = kind; ev->flags = flags;
			ev->seq = cur_gen; ev->index = cur_index++;
			ev->cancelled = 0; ev->payload = payload;
			ev->outcome = 0; ev->shortfall = 0;
			if (flags & FR_EVF_BARRIER)
				atomic_fetch_add_explicit(&fr->barrier_emitted, 1, memory_order_acq_rel);
			return FR_ABORT;
		}
		uint32_t t = LDRLX(fr->st_tail);
		if (t - LD(fr->st_head) < FR_STREAM_CAP) {
			fr_event* ev = &fr->stream[t & ST_MASK];
			ev->kind = kind; ev->flags = flags;
			ev->seq = cur_gen; ev->index = cur_index++;
			ev->cancelled = 0; ev->payload = payload;
			ev->outcome = 0; ev->shortfall = 0;
			if (flags & FR_EVF_BARRIER)
				atomic_fetch_add_explicit(&fr->barrier_emitted, 1, memory_order_acq_rel);
			ST(fr->st_tail, t + 1);
			wake_cons(fr);
			return FR_OK;
		}
		// stream full
		aborting = (fr_get_state(fr) == FR_ABORTING) || LD(fr->park_req) || LD(fr->stop_req);
		if (aborting) {
			if (fr_get_state(fr) != FR_ABORTING) ST(fr->state, FR_ABORTING);
			if (kind == FR_EV_FRAME) return FR_DROPPED;  // frames droppable (F26)
			// commands NEVER dropped: CORE-local overflow, published before RUN_DONE
			assert(fr->ovf_n < FR_OVF_CAP && "command overflow buffer exhausted");
			fr_event* ev = &fr->ovf[fr->ovf_n++];
			ev->kind = kind; ev->flags = flags;
			ev->seq = cur_gen; ev->index = cur_index++;
			ev->cancelled = 0; ev->payload = payload;
			ev->outcome = 0; ev->shortfall = 0;
			if (flags & FR_EVF_BARRIER)
				atomic_fetch_add_explicit(&fr->barrier_emitted, 1, memory_order_acq_rel);
			return FR_ABORT;
		}
		prod_sleep(fr, pred_stream_space, 0, "stream_full");
	}
}

static int pred_barrier(fr_ring* fr) {
	return LD(fr->barrier_applied) >= LD(fr->barrier_emitted)
	    || LD(fr->park_req) || LD(fr->stop_req)
	    || fr_get_state(fr) == FR_ABORTING;
}

fr_rc fr_core_barrier_wait(fr_ring* fr) {
	// Prefix-drain proof (F29): the wake here comes from MAIN applying the barrier
	// out of the OPEN epoch's stream — never from RUN_DONE.
	for (;;) {
		if (LD(fr->barrier_applied) >= LD(fr->barrier_emitted)) return FR_OK;
		if (fr_get_state(fr) == FR_ABORTING || LD(fr->park_req) || LD(fr->stop_req)) {
			if (fr_get_state(fr) == FR_RUNNING) ST(fr->state, FR_ABORTING);
			return FR_ABORT;  // partial-accept; ledger settles before park ack
		}
		prod_sleep(fr, pred_barrier, 0, "barrier_wait");
	}
}

fr_rc fr_core_run_done(fr_ring* fr, fr_outcome outcome, uint32_t shortfall) {
	assert(LD(fr->in_run));
	// flush the ABORTING command overflow into the stream FIRST (contract: published
	// before RUN_DONE). Waits are drain-driven; MAIN drains while awaiting park ack.
	while (fr->ovf_n) {
		try_flush_ovf(fr);
		if (fr->ovf_n) prod_sleep(fr, pred_stream_space, 1, "overflow_flush");
	}

	int cancelled = LD(fr->park_req) || LD(fr->stop_req) || fr_get_state(fr) == FR_ABORTING;

	// RUN_DONE queue can never be full: capacity = FR_MAX_DEPTH >= depth, one entry
	// per outstanding credit, and this epoch holds one of those credits (pigeonhole).
	uint32_t t = LDRLX(fr->rd_tail);
	assert(t - LD(fr->rd_head) < FR_MAX_DEPTH && "RUN_DONE queue overfull: credit accounting broken");
	fr_event* ev = &fr->rdq[t & (FR_MAX_DEPTH - 1)];
	ev->kind = FR_EV_RUN_DONE; ev->flags = 0;
	ev->seq = cur_gen; ev->index = cur_index;
	ev->cancelled = (uint32_t)cancelled;
	ev->payload = 0; ev->outcome = (uint32_t)outcome; ev->shortfall = shortfall;
	ST(fr->in_run, 0);
	ST(fr->rd_tail, t + 1);

	pthread_mutex_lock(&fr->lk);
	fr_rc rc;
	if (LD(fr->stop_req)) rc = FR_STOP;
	else if (cancelled || LD(fr->park_req)) {
		ST(fr->state, FR_QUIESCENT);  // boundary reached; consume the park
		ST(fr->park_req, 0);
		rc = FR_PARKED;
	} else {
		ST(fr->state, FR_RUNNING);
		rc = FR_OK;
	}
	pthread_cond_broadcast(&fr->cv_cons);
	pthread_mutex_unlock(&fr->lk);
	return rc;
}

// ---- CORE: QUIESCENT service loop -----------------------------------------------

static int pred_service(fr_ring* fr) {
	return LD(fr->svcq_head) != LD(fr->svcq_tail)
	    || LD(fr->release_req) || LD(fr->stop_req);
}

fr_rc fr_core_service_next(fr_ring* fr, uint64_t* out_op) {
	assert(fr_get_state(fr) == FR_QUIESCENT);
	pthread_mutex_lock(&fr->lk);
	for (;;) {
		// a park arriving here is absorbed: already parked, no edge (F30)
		if (LD(fr->park_req)) { ST(fr->park_req, 0); pthread_cond_broadcast(&fr->cv_cons); }
		// terminal requests take precedence at the op boundary
		if (LD(fr->stop_req)) { pthread_mutex_unlock(&fr->lk); return FR_STOP; }
		uint32_t h = LD(fr->svcq_head);
		if (h != LD(fr->svcq_tail)) {
			*out_op = fr->svcq[h & SQ_MASK].op;
			ST(fr->svcq_head, h + 1);
			cur_gen = atomic_fetch_add_explicit(&fr->svc_seq, 1, memory_order_acq_rel) + 1;
			cur_index = 0;
			pthread_mutex_unlock(&fr->lk);
			return FR_SVC;
		}
		if (LD(fr->release_req)) {
			// release-consume + state-flip atomic under lk: a concurrent park
			// entry either cleared the release before this read (we sleep) or
			// runs after the flip and sees RUNNING (it waits for the real park).
			ST(fr->release_req, 0);
			ST(fr->state, FR_RUNNING);
			pthread_cond_broadcast(&fr->cv_cons);
			pthread_mutex_unlock(&fr->lk);
			return FR_RELEASED;
		}
		ST(fr->prod_waiting, 1);
		if (!pred_service(fr)) {
			struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); ts.tv_sec += 1;
			pthread_cond_timedwait(&fr->cv_prod, &fr->lk, &ts);
		}
		ST(fr->prod_waiting, 0);
	}
}

static int pred_svc_space(fr_ring* fr) {
	return LDRLX(fr->sv_tail) - LD(fr->sv_head) < FR_SVC_STREAM;
}

void fr_core_service_emit(fr_ring* fr, uint32_t flags, uint64_t payload) {
	// drain-driven wait: no park edge exists in QUIESCENT; no cancellation. MAIN
	// drains while awaiting the service ack (rule 2), so progress is structural.
	for (;;) {
		uint32_t t = LDRLX(fr->sv_tail);
		if (t - LD(fr->sv_head) < FR_SVC_STREAM) {
			fr_event* ev = &fr->svc_stream[t & SV_MASK];
			ev->kind = FR_EV_SERVICE; ev->flags = flags;
			ev->seq = cur_gen; ev->index = cur_index++;
			ev->cancelled = 0; ev->payload = payload;
			ev->outcome = 0; ev->shortfall = 0;
			if (flags & FR_EVF_BARRIER)
				atomic_fetch_add_explicit(&fr->barrier_emitted, 1, memory_order_acq_rel);
			ST(fr->sv_tail, t + 1);
			wake_cons(fr);
			return;
		}
		prod_sleep(fr, pred_svc_space, 1, "service_emit");
	}
}

void fr_core_service_ack(fr_ring* fr) {
	atomic_fetch_add_explicit(&fr->svc_acked, 1, memory_order_acq_rel);
	wake_cons(fr);
}

void fr_core_thread_exit(fr_ring* fr) {
	ST(fr->state, FR_STOPPED);
	wake_cons(fr);
}
