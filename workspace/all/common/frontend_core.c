// frontend_core.c — the CORE-thread lifecycle engine (threading v2 phase 2).
// See frontend_core.h. Pure logic over framering + a core vtable; no SDL/libretro.
#include "frontend_core.h"
#include <string.h>

// ---- CORE-side: state, emit shims, service dispatch, cleanup oracle ----------

static inline void set_state(fc* f, fc_state s) {
	atomic_store_explicit(&f->state, (int)s, memory_order_release);
}
static inline fc_state get_state_core(fc* f) {
	return (fc_state)atomic_load_explicit(&f->state, memory_order_acquire);
}

// CORE-only per-epoch scratch (never touched by MAIN → no atomics, TSan-clean)
static _Thread_local uint32_t tl_ep_frames;
static _Thread_local int      tl_ep_dup;

fr_rc fc_emit_frame(fc* f, uint64_t payload) {
	fr_rc rc = fr_core_emit(&f->fr, FR_EV_FRAME, 0, payload);
	if (rc == FR_OK) tl_ep_frames++;
	return rc;
}
fr_rc fc_emit_cmd(fc* f, uint32_t flags, uint64_t payload) {
	return fr_core_emit(&f->fr, FR_EV_CMD, flags, payload);
}
fr_rc fc_emit_barrier(fc* f, uint64_t payload) {
	fr_rc rc = fr_core_emit(&f->fr, FR_EV_CMD, FR_EVF_PERSISTENT | FR_EVF_BARRIER, payload);
	if (rc == FR_OK) (void)fr_core_barrier_wait(&f->fr);  // FR_ABORT ok: applied before park ack
	return rc;
}

// Decode a service op word: low 8 bits = fc_op, high bits = arg.
static inline fc_op   op_of(uint64_t w)  { return (fc_op)(w & 0xFF); }
static inline uint64_t arg_of(uint64_t w) { return w >> 8; }

// Execute one bootstrap/menu service op on CORE. Advances state on bootstrap
// success; sets boot_failed (and boot_shutdown) on hard failure / requested
// shutdown. RESUME failure is non-fatal (state still advances). Returns nothing;
// products (if any) are emitted via fr_core_service_emit by the vtable body.
static void dispatch_service(fc* f, uint64_t opword) {
	fc_op op = op_of(opword);
	uint64_t arg = arg_of(opword);
	int r = 0;
	switch (op) {
	case FC_OP_GET_INFO:   r = f->vt.get_system_info(f->vt.ctx); if (!r) set_state(f, FC_INFO_READY); break;
	case FC_OP_INIT:       r = f->vt.init(f->vt.ctx);            if (!r) set_state(f, FC_INITIALIZED); break;
	case FC_OP_OPEN:       r = f->vt.open_content(f->vt.ctx);    break; // path prep; state unchanged
	case FC_OP_LOAD:       r = f->vt.load_game(f->vt.ctx);       if (!r) set_state(f, FC_CONTENT_LOADED); break;
	case FC_OP_MEMORY:     r = f->vt.setup_memory(f->vt.ctx);    break; // pointers valid; crash arm next
	case FC_OP_ARM_CRASH:  r = f->vt.arm_crash(f->vt.ctx);       if (!r) set_state(f, FC_MEMORY_READY); break;
	case FC_OP_AV:         r = f->vt.get_av_info(f->vt.ctx);     if (!r) set_state(f, FC_AV_READY); break;
	case FC_OP_CONTROLLER: r = f->vt.set_controller(f->vt.ctx);  break;
	case FC_OP_AUDIO:      r = f->vt.audio_init(f->vt.ctx);      break;
	case FC_OP_RENDERER:   r = f->vt.renderer_init(f->vt.ctx);   if (!r) set_state(f, FC_FRONTEND_READY); break;
	case FC_OP_RESUME:     (void)f->vt.resume(f->vt.ctx);        set_state(f, FC_RESUME_APPLIED); r = 0; break; // nonfatal
	case FC_OP_SERIALIZE:   r = f->vt.serialize(f->vt.ctx, arg);   break;
	case FC_OP_UNSERIALIZE: r = f->vt.unserialize(f->vt.ctx, arg); break;
	case FC_OP_RESET:       f->vt.reset(f->vt.ctx);               r = 0; break;
	default: break;
	}
	atomic_store_explicit(&f->last_op_result,
	                      r ? (((uint64_t)op << 8) | (uint64_t)(r & 0xFF)) : 0,
	                      memory_order_release);
	if (r) {
		atomic_store_explicit(&f->boot_failed, 1, memory_order_release);
		if (r > 0) atomic_store_explicit(&f->boot_shutdown, 1, memory_order_release);
	}
}

// The per-state terminal cleanup oracle (F31): exactly which teardown calls are
// legal from the highest state reached. THIS is what the fake-core harness asserts.
static void terminal_cleanup(fc* f) {
	fc_state s = get_state_core(f);
	if (s >= FC_MEMORY_READY)   f->vt.disarm_crash(f->vt.ctx);  // armed at MEMORY_READY
	if (s >= FC_CONTENT_LOADED) f->vt.unload_game(f->vt.ctx);   // load_game succeeded
	if (s >= FC_INITIALIZED)    f->vt.deinit(f->vt.ctx);        // retro_init succeeded
	// below FC_INITIALIZED: get_system_info / thread-create only → no libretro cleanup
}

static void* fc_core_thread(void* arg) {
	fc* f = (fc*)arg;
	set_state(f, FC_CORE_CREATED);
	for (;;) {
		if (fr_get_state(&f->fr) == FR_QUIESCENT) {
			uint64_t opword = 0;
			fr_rc rc = fr_core_service_next(&f->fr, &opword);
			if (rc == FR_STOP) break;
			if (rc == FR_RELEASED) continue;   // going RUNNING; loop re-reads state
			// FR_SVC
			dispatch_service(f, opword);
			fr_core_service_ack(&f->fr);
			continue;
		}
		// RUNNING
		uint64_t gen = 0; uint64_t snap[4];
		fr_rc rc = fr_core_wait_grant(&f->fr, &gen, snap);
		if (rc == FR_STOP) break;
		if (rc == FR_PARKED) continue;
		// FR_GRANT
		if (get_state_core(f) != FC_RUNNING) set_state(f, FC_RUNNING);
		tl_ep_frames = 0; tl_ep_dup = 0;
		f->vt.run(f->vt.ctx);   // emits via fc_emit_* (from the core's callbacks)
		fr_outcome out = tl_ep_frames ? FR_OUT_FRAME : (tl_ep_dup ? FR_OUT_DUP : FR_OUT_NONE);
		rc = fr_core_run_done(&f->fr, out, 0);
		if (rc == FR_STOP) break;
	}
	terminal_cleanup(f);
	fr_core_thread_exit(&f->fr);
	return NULL;
}

// CORE-side hook the fake/real run() can call to mark a NULL duplicate frame.
void fc_signal_dup(fc* f) { (void)f; tl_ep_dup = 1; }

// ---- MAIN-side lifecycle ----------------------------------------------------

// framering invokes the drain callback per event; a NULL callback would crash when
// there is anything to drain (teardown, a resume that emitted, a park with pending
// events). Every fr_* drain-taking call routes through here so cb is never NULL.
static void fc_noop_cb(void* ctx, const fr_event* ev) { (void)ctx; (void)ev; }
static inline fr_drain_cb cb_or_noop(fr_drain_cb cb) { return cb ? cb : fc_noop_cb; }

void fc_init(fc* f, const fc_vtable* vt, uint32_t depth) {
	memset(f, 0, sizeof(*f));
	f->vt = *vt;
	fr_init(&f->fr, depth ? depth : 1);
	f->fr.failclosed_sec = 60;
	atomic_store(&f->state, (int)FC_NONE);
	pthread_create(&f->core_thread, NULL, fc_core_thread, f);
	f->thread_started = 1;
}

// Drive ONE bootstrap stage. Used both by fc_bootstrap (the monolithic loop) and by
// callers that must interleave their own MAIN-thread frontend work between core stages
// (real minarch: get_system_info's result drives MAIN dir setup; Config/Input/Menu/SND
// run between LOAD and CONTROLLER — see DECISIONS D58). The CALLER owns ordering.
//
// F-A resolution (D57): FC_OP_AUDIO / FC_OP_RENDERER are SDL init (SND_init / SDL
// renderer) which MUST run on the MAIN/window thread, not CORE. fc_boot_stage runs on
// MAIN, and between service ops the CORE thread is QUIESCENT — parked in
// fr_core_service_next, blocked on its condvar, touching nothing these stages touch — so
// MAIN calls dispatch_service directly for identical state/boot_failed semantics with
// zero concurrent CORE access. (The AV stage must have run first, so core.sample_rate is
// available to audio_init — the caller's responsibility per the documented order.) These
// stages emit no ring products, so no drain is owed. All other stages run their core.*
// op on the CORE thread via fr_service.
//
// If boot already failed, this is a no-op (stay put): the caller should tear down, not
// keep issuing stages. Returns the state reached; check fc_boot_failed() after.
fc_state fc_boot_stage(fc* f, fc_op op) {
	if (atomic_load_explicit(&f->boot_failed, memory_order_acquire))
		return fc_get_state(f);
	if (op == FC_OP_AUDIO || op == FC_OP_RENDERER)
		dispatch_service(f, (uint64_t)op);                  // MAIN-side, CORE idle (D57)
	else
		fr_service(&f->fr, (uint64_t)op, fc_noop_cb, NULL); // CORE-side
	return fc_get_state(f);
}

// Flip QUIESCENT -> RUNNING after the LAST bootstrap stage succeeded; grants may now
// flow. No-op if boot_failed (the caller should fc_teardown instead) so a mis-sequenced
// call can never release a failed ring. Idempotent-safe is NOT claimed: call exactly once
// after a clean stage run.
fc_state fc_boot_finish(fc* f) {
	if (atomic_load_explicit(&f->boot_failed, memory_order_acquire))
		return fc_get_state(f);
	fr_release(&f->fr);     // QUIESCENT -> RUNNING; grants may now flow
	return fc_get_state(f); // FC_RESUME_APPLIED; becomes FC_RUNNING at first pump
}

fc_state fc_bootstrap(fc* f) {
	static const fc_op seq[] = {
		FC_OP_GET_INFO, FC_OP_INIT, FC_OP_OPEN, FC_OP_LOAD, FC_OP_MEMORY,
		FC_OP_ARM_CRASH, FC_OP_AV, FC_OP_CONTROLLER, FC_OP_AUDIO,
		FC_OP_RENDERER, FC_OP_RESUME,
	};
	for (unsigned i = 0; i < sizeof(seq)/sizeof(seq[0]); i++) {
		fc_boot_stage(f, seq[i]);
		if (atomic_load_explicit(&f->boot_failed, memory_order_acquire))
			return fc_get_state(f);   // caller invokes fc_teardown from here
	}
	return fc_boot_finish(f);
}

// Drain wrapper that notices this pump's epoch boundary. At depth 1 exactly one credit
// is ever outstanding, so the single FR_EV_RUN_DONE it counts IS the epoch we just
// granted — that is the depth-1 completion signal. Every event is still forwarded to
// the caller's cb unchanged (minarch's drain switches on kind and ignores RUN_DONE).
struct fc_pump_ctx { fr_drain_cb cb; void* ctx; int run_done; };
static void fc_pump_cb(void* p, const fr_event* ev) {
	struct fc_pump_ctx* w = (struct fc_pump_ctx*)p;
	if (ev->kind == FR_EV_RUN_DONE) w->run_done++;
	w->cb(w->ctx, ev);
}

// D59: depth-1 is a SERIAL rendezvous — fc_pump must BLOCK until the granted epoch's
// RUN_DONE drains, then present and return. The old grant+drain+return raced ahead of
// CORE and made MAIN busy-spin ~11k pumps/sec (wasted clock; the governor's gen signal
// read the spin rate, not the frame rate). The block reuses framering's cancellable
// fr_wait_events (bounded 100ms tick, woken on any producer state edge) — never a
// busy-wait. Cancellation: a cross-thread stop (or a park that drops CORE out of
// RUNNING) flips the ring to STOPPED/QUIESCENT; fr_wait_events wakes on that edge and
// the state check returns without a RUN_DONE instead of hanging. Depth >= 2 is
// PIPELINED: keep grant + prefix-drain + return so the epoch completes on a later pump
// and MAIN never blocks on any single epoch.
void fc_pump(fc* f, const uint64_t snapshot[4], fr_drain_cb cb, void* cbctx) {
	struct fc_pump_ctx w = { cb_or_noop(cb), cbctx, 0 };
	uint64_t g = 0;
	fr_rc rc = fr_grant(&f->fr, snapshot, &g);  // FR_NOSPACE tolerated (parked/stopped/pipeline full)
	if (rc == FR_OK && f->fr.depth <= 1) {
		for (;;) {
			fr_drain(&f->fr, fc_pump_cb, &w, FR_DRAIN_NORMAL);  // applies barriers + presents frames
			if (w.run_done) break;                              // epoch done + presented
			fr_state st = fr_get_state(&f->fr);
			if (st == FR_QUIESCENT || st == FR_STOPPED) break;  // park/stop cancelled the wait
			fr_wait_events(&f->fr);                             // cancellable block; no busy-wait
		}
	} else {
		fr_drain(&f->fr, fc_pump_cb, &w, FR_DRAIN_NORMAL);  // pipelined / no-credit: prefix-drain + return
	}
}

void fc_park(fc* f, fr_drain_cb cb, void* cbctx, int discard) {
	fr_park(&f->fr, cb_or_noop(cb), cbctx, discard ? FR_DRAIN_DISCARD : FR_DRAIN_NORMAL);
}
void fc_release(fc* f) { fr_release(&f->fr); }
void fc_set_depth(fc* f, uint32_t depth, fr_drain_cb cb, void* cbctx, int discard) {
	fr_set_depth(&f->fr, depth, cb_or_noop(cb), cbctx, discard ? FR_DRAIN_DISCARD : FR_DRAIN_NORMAL);
}

int fc_menu_op(fc* f, fc_op op, uint64_t arg, fr_drain_cb cb, void* cbctx) {
	// F18: park -> service -> release. The op runs on CORE (single-thread session).
	fr_park(&f->fr, cb_or_noop(cb), cbctx, FR_DRAIN_NORMAL);
	fr_service(&f->fr, ((uint64_t)arg << 8) | (uint64_t)op, cb_or_noop(cb), cbctx);
	uint64_t res = atomic_load_explicit(&f->last_op_result, memory_order_acquire);
	fr_release(&f->fr);
	return (int)(res & 0xFF) ? -1 : 0;
}

void fc_teardown(fc* f) {
	if (!f->thread_started) return;
	// Reversible->terminal: bring CORE to a boundary (park absorbs if already
	// QUIESCENT after a failed bootstrap), request stop, let CORE run the
	// state-keyed cleanup oracle, then join. Never join/free out of order.
	fr_park(&f->fr, fc_noop_cb, NULL, FR_DRAIN_NORMAL);
	fr_stop(&f->fr);
	pthread_join(f->core_thread, NULL);
	f->thread_started = 0;
	fr_destroy(&f->fr);
}
