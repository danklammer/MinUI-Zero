// framering — threading v2's epoch/credit/event-stream protocol module.
//
// This file IS the contract's executable form (docs/threading-v2-design.md, v2.4,
// "contract phase closed"): the credit pipeline (depth 1 = serial mode, depth 2 =
// threaded), the ordered intra-epoch event stream with barrier prefix-drain (F29),
// the persistent-event class that survives every discard path (F26/F29), the
// drain-driven service protocol in its own svc_seq namespace (F30), ABORTING with
// the command-overflow guarantee, and the 7-step depth-change/credit-reclaim gate
// (F24). Pure C11: no SDL, no libretro, no platform headers. minarch integration
// (phase 2) calls through this API; it does not reimplement the protocol.
//
// Thread roles (fixed): exactly one PRODUCER thread (CORE) calls fr_core_*;
// exactly one CONSUMER/controller thread (MAIN) calls everything else. The module
// never creates threads.
//
// Where implementation experience contradicts the design doc, the doc gets amended
// with a DECISIONS.md entry — not silently papered over here.
#ifndef FRAMERING_H
#define FRAMERING_H

#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

#define FR_MAX_DEPTH   4    // candidate ships depth<=2; harness exercises up to 4
// Capacities are compile-time overridable so the test harness can shrink them and
// force the full-wait / ABORTING-overflow paths (a 64-slot stream never fills at
// gameplay emission rates — pressure must be manufactured to be tested).
#ifndef FR_STREAM_CAP
#define FR_STREAM_CAP  64   // run-epoch event stream (power of two)
#endif
#ifndef FR_SVC_STREAM
#define FR_SVC_STREAM  16   // dedicated service stream slot (power of two)
#endif
#define FR_SVCQ_CAP    4    // MAIN->CORE service request queue (power of two)
#define FR_GRANTQ_CAP  FR_MAX_DEPTH
#define FR_OVF_CAP     128  // CORE-local command overflow under ABORTING

// ---- events -------------------------------------------------------------
typedef enum {
	FR_EV_FRAME    = 1,  // visual payload: droppable under ABORTING, skipped by DISCARD drains
	FR_EV_CMD      = 2,  // command: NEVER dropped, applied on every path
	FR_EV_RUN_DONE = 3,  // synthesized to the drain callback when an epoch boundary drains
	FR_EV_SERVICE  = 4,  // service-op product (SERVICE, svc_seq, index)
} fr_ev_kind;

#define FR_EVF_PERSISTENT 0x1  // state the core already changed: AV_INFO/geometry/options/disk
#define FR_EVF_BARRIER    0x2  // consumer must apply before producer proceeds (prefix-drain wake)

typedef enum {  // RUN_DONE outcomes
	FR_OUT_FRAME = 1,
	FR_OUT_DUP   = 2,
	FR_OUT_NONE  = 3,
} fr_outcome;

typedef struct {
	uint32_t kind;      // fr_ev_kind
	uint32_t flags;     // FR_EVF_*
	uint64_t seq;       // gen (RUN events) or svc_seq (SERVICE events)
	uint32_t index;     // emission order within the epoch/op
	uint32_t cancelled; // RUN_DONE only: park/stop arrived mid-run (CANCELLED|outcome)
	uint64_t payload;   // opaque to the module
	uint32_t outcome;   // RUN_DONE only: fr_outcome
	uint32_t shortfall; // RUN_DONE only: partially-accepted audio units (ABORTING)
} fr_event;

// Commands use FR_EV_CMD inside a RUN epoch and FR_EV_SERVICE while CORE is
// quiescent in a bootstrap/runtime/teardown service. Integrators that apply
// command payloads must recognize both transport classes.
static inline int fr_event_is_command(const fr_event* ev) {
	return ev && (ev->kind == FR_EV_CMD || ev->kind == FR_EV_SERVICE);
}

// drain callback: called in-order for every applied event. DISCARD drains skip
// FR_EV_FRAME payloads (never called for them); everything else always applies.
typedef void (*fr_drain_cb)(void* ctx, const fr_event* ev);

// Frame-retirement callback (D-a2): invoked EXACTLY ONCE per PUBLISHED FR_EV_FRAME, when it
// is consumed from the stream, on EVERY drain path — normal present, DISCARD-skip, and
// park/stop drainage. This is how the integrator's frame-buffer pool returns a buffer whose
// event was NOT presented (the DISCARD-skip leak: the drain cb never fires for skipped
// frames). Frames dropped at emit (FR_DROPPED, never published) are retired CORE-side and
// never reach here (D-a2': CORE retires pre-publication, MAIN retires EVENT_OWNED). payload =
// the FRAME event's payload = the pool buffer id. NULL (default) = no retirement (legacy).
typedef void (*fr_frame_retire_cb)(void* ctx, uint64_t payload);

#define FR_DRAIN_NORMAL  0
#define FR_DRAIN_DISCARD 1  // FF flip / trial depth change: payload only; persistent+cmds apply

// ---- producer-visible states / return codes ------------------------------
typedef enum { FR_QUIESCENT = 0, FR_RUNNING, FR_ABORTING, FR_STOPPED } fr_state;

typedef enum {
	FR_OK       = 0,
	FR_GRANT    = 1,   // wait_grant: a grant was consumed
	FR_PARKED   = 2,   // wait cancelled: park consumed, producer must go QUIESCENT
	FR_STOP     = 3,   // stop requested: producer must exit its loop
	FR_ABORT    = 4,   // blocked wait cancelled by park mid-run: caller is in ABORTING
	FR_DROPPED  = 5,   // frame payload dropped under ABORTING (blocked + droppable)
	FR_RELEASED = 6,   // service_next: MAIN released QUIESCENT back to RUNNING
	FR_SVC      = 7,   // service_next: a service op was dequeued
	FR_NOSPACE  = 8,   // non-blocking refusal (grant with no credit / parked)
} fr_rc;

// ---- the ring object ------------------------------------------------------
typedef struct {
	// configuration (MAIN-owned; mutated only via the depth-change gate)
	uint32_t depth;

	// two disjoint sequence namespaces (design "Two sequence namespaces")
	_Atomic uint64_t gen;        // advances exactly once per RUN grant (MAIN)
	_Atomic uint64_t svc_seq;    // advances exactly once per service op (CORE)

	// lifecycle request flags (MAIN->CORE) and producer-owned state
	_Atomic int park_req;        // consumed by QUIESCENT entry; absorbed if already parked
	_Atomic int stop_req;
	_Atomic int release_req;
	_Atomic int state;           // fr_state, written by producer only
	_Atomic int in_run;          // between grant consumption and RUN_DONE publication

	// grant queue (MAIN produces, CORE consumes; rewound only while CORE is QUIESCENT)
	struct { uint64_t gen; uint32_t slot; } grantq[FR_GRANTQ_CAP];
	_Atomic uint32_t grant_head; // consumer (CORE)
	_Atomic uint32_t grant_tail; // producer (MAIN)

	// credits + input snapshot slots (snapshot lifetime: grant -> credit return)
	_Atomic uint32_t credits_out;          // granted - returned; <= depth
	_Atomic uint32_t slot_owned;           // bitmask
	struct { uint64_t gen; uint64_t data[4]; } snap[FR_MAX_DEPTH];

	// run-epoch event stream (SPSC: CORE -> MAIN), one global order across epochs
	fr_event stream[FR_STREAM_CAP];
	_Atomic uint32_t st_head, st_tail;

	// RUN_DONE queue (capacity = depth; slot per outstanding credit => never full)
	fr_event rdq[FR_MAX_DEPTH];
	_Atomic uint32_t rd_head, rd_tail;

	// barrier handshake: ids assigned per barrier emission; consumer publishes applied
	_Atomic uint64_t barrier_emitted;   // CORE
	_Atomic uint64_t barrier_applied;   // MAIN (prefix-drain applies + wakes)

	// command overflow under ABORTING (CORE-local; flushed before RUN_DONE)
	fr_event ovf[FR_OVF_CAP];
	uint32_t ovf_n;

	// service protocol (F30): request queue MAIN->CORE, event stream CORE->MAIN
	struct { uint64_t op; } svcq[FR_SVCQ_CAP];
	_Atomic uint32_t svcq_head, svcq_tail;
	fr_event svc_stream[FR_SVC_STREAM];
	_Atomic uint32_t sv_head, sv_tail;
	_Atomic uint64_t svc_acked;         // CORE: ops fully emitted+acked
	_Atomic uint64_t svc_requested;     // MAIN: ops enqueued

	// wake plumbing: kernel waits only on empty/full/lifecycle/barrier (perf contract)
	pthread_mutex_t lk;
	pthread_cond_t  cv_prod;            // CORE sleeps here
	pthread_cond_t  cv_cons;            // MAIN sleeps here
	_Atomic int prod_waiting, cons_waiting;
	_Atomic uint64_t prod_wake_seq;     // every MAIN->CORE notification, even if no waiter observed
	_Atomic uint64_t cons_wake_seq;     // every CORE->MAIN notification, even if no waiter observed

	// fail-closed timeout for drain-driven waits (seconds); test override
	int failclosed_sec;

	// frame-buffer retirement (D-a2): MAIN-side pool return, fired per published frame on
	// every drain path so DISCARD-skipped frames don't leak their buffer. Set post-init.
	fr_frame_retire_cb frame_retire_cb;
	void* frame_retire_ctx;
} fr_ring;

// ---- MAIN-side API --------------------------------------------------------
void  fr_init(fr_ring* fr, uint32_t depth);
void  fr_destroy(fr_ring* fr);

// Issue a run grant: writes the epoch input snapshot (4 words), hands a credit,
// advances gen. Returns FR_NOSPACE if no credit or producer not RUNNING-eligible.
fr_rc fr_grant(fr_ring* fr, const uint64_t snapshot[4], uint64_t* out_gen);

// Drain everything currently published (prefix-drain: includes the open epoch).
// Applies barriers (and wakes the producer's barrier wait), synthesizes RUN_DONE
// events, returns credits. Returns number of events applied.
int   fr_drain(fr_ring* fr, fr_drain_cb cb, void* ctx, int mode);

// Block until there is something to drain or a producer state change (test/main
// loop helper; normal integration polls on its pacer tick).
void  fr_wait_events(fr_ring* fr);

// Register the frame-retirement callback (D-a2). Call once after fr_init, before RUNNING.
void  fr_set_frame_retire_cb(fr_ring* fr, fr_frame_retire_cb cb, void* ctx);

// Park: gate steps 1-6 (stop grants, cancel queued-unrun grants, abort/finish the
// active epoch, drain [mode], reclaim every credit, verify slots unowned). The wait
// loop IS the drain loop (wait-graph rule 2). Returns when producer is QUIESCENT
// with stream+rdq empty, all persistent/barrier events applied, credits home.
void  fr_park(fr_ring* fr, fr_drain_cb cb, void* ctx, int mode);

// Release QUIESCENT back to RUNNING (grants may resume).
void  fr_release(fr_ring* fr);

// Depth change = park + step 7 (resize while QUIESCENT) + caller releases.
void  fr_set_depth(fr_ring* fr, uint32_t depth, fr_drain_cb cb, void* ctx, int mode);

// Request terminal stop (producer exits at the next boundary; service ops finish).
void  fr_stop(fr_ring* fr);

// Service op request + completion wait (drains run+service streams while waiting;
// returns after CORE acks AND every service event of the op has been applied).
void  fr_service(fr_ring* fr, uint64_t op, fr_drain_cb cb, void* ctx);

// ---- CORE-side API ----------------------------------------------------------
// Grant/park/stop multiplexer for the RUNNING loop. FR_GRANT: *out_gen/*snap filled.
fr_rc fr_core_wait_grant(fr_ring* fr, uint64_t* out_gen, uint32_t* out_slot, uint64_t snap[4]);

// Emit into the current epoch. Frame: blocked+ABORTING => FR_DROPPED (payload loss
// is legal). Cmd: never lost — blocked+ABORTING goes to the overflow buffer
// (flushed before RUN_DONE). flags: FR_EVF_PERSISTENT / FR_EVF_BARRIER.
fr_rc fr_core_emit(fr_ring* fr, uint32_t kind, uint32_t flags, uint64_t payload);

// Block until MAIN has applied the barrier this thread most recently emitted.
// ABORTING => returns FR_ABORT immediately (audio partial-accept semantics); the
// barrier itself still applies before the park ack (ledger; enforced by fr_park).
fr_rc fr_core_barrier_wait(fr_ring* fr);

// Publish the epoch boundary. Flushes the ABORTING command overflow first.
// If park/stop arrived mid-run the outcome is recorded CANCELLED|outcome and the
// producer transitions to QUIESCENT (consuming the park). Returns FR_PARKED /
// FR_STOP / FR_OK accordingly; shortfall = partially-accepted audio units.
fr_rc fr_core_run_done(fr_ring* fr, fr_outcome outcome, uint32_t shortfall);

// QUIESCENT loop: dequeue the next service op (FR_SVC), or FR_RELEASED / FR_STOP.
fr_rc fr_core_service_next(fr_ring* fr, uint64_t* out_op);

// Emit a service product (drain-driven wait; no park edge exists in QUIESCENT).
void  fr_core_service_emit(fr_ring* fr, uint32_t flags, uint64_t payload);

// Ack the current service op (all its events already emitted).
void  fr_core_service_ack(fr_ring* fr);

// Producer thread exit: state -> STOPPED, wake MAIN. Call once, last.
void  fr_core_thread_exit(fr_ring* fr);

// Introspection (tests, integration asserts)
static inline fr_state fr_get_state(const fr_ring* fr) {
	return (fr_state)atomic_load_explicit((_Atomic int*)&fr->state, memory_order_acquire);
}
static inline uint32_t fr_credits_out(const fr_ring* fr) {
	return atomic_load_explicit((_Atomic uint32_t*)&fr->credits_out, memory_order_acquire);
}

#endif
