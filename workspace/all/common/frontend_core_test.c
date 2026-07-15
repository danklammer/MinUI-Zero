// frontend_core_test.c — adversarial harness for the phase-2 lifecycle engine.
//
// A libretro-shaped FAKE CORE that can be commanded to fail or request shutdown at
// EVERY F31 bootstrap stage, and (on the success path) to emit adversarial runtime
// patterns: zero / NULL-dup / multi-frame epochs, mid-run SET_GEOMETRY, AV_INFO
// barriers, SET_VARIABLE, slow runs, and requested shutdown mid-epoch. The MAIN
// driver runs each injection point and asserts the terminal cleanup ORACLE: exactly
// which teardown calls are legal from the highest bootstrap state reached. The
// TSan/ASan builds adjudicate races on the same shipping frontend_core code.
//
//   frontend_core_test [seconds] [seed]
#include "frontend_core.h"
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// D57 thread-affinity oracle: FC_OP_AUDIO/RENDERER are SDL-affine and MUST run on the
// MAIN thread (fc_bootstrap direct-calls them while CORE is quiescent); every other
// bootstrap/runtime stage MUST run on the CORE thread. Captured in main() before fc_init.
static pthread_t g_main_tid;
static int g_tid_ready = 0;
static _Atomic uint64_t g_snap_checks = 0;  // INV16: epochs whose input snapshot fk_run verified (coverage guard)
static _Atomic uint64_t g_slot_checks = 0;  // INV17: epochs whose slot==gen%depth fk_run verified (coverage guard)
static _Atomic uint64_t g_pool_retires = 0; // INV18/19: frame buffers retired via the D-a2 hook (coverage guard)
static _Atomic uint64_t g_svc_cmd_emitted = 0; // INV21: commands/barriers emitted from a bootstrap service (D-b routing)
static _Atomic uint64_t g_teardown_emits = 0;  // INV22: deinit-time emits drained by fc_teardown's loop (D-b2)

// D-a2 MAIN-side retirement: framering calls this once per published frame on every drain
// path (present, DISCARD-skip, park). Returns the buffer to the (modeled) pool.
static void fk_retire(void* ctx, uint64_t payload);
static void assert_affinity(int op) {
	if (!g_tid_ready) return;
	int on_main = pthread_equal(pthread_self(), g_main_tid);
	if (op == FC_OP_AUDIO || op == FC_OP_RENDERER)
		assert(on_main && "F-A: audio/renderer init must run on MAIN (D57)");
	else
		assert(!on_main && "CORE stage must run on the CORE thread (D57)");
}

typedef struct { uint64_t s; } rng_t;
static uint64_t rng_next(rng_t* r){ uint64_t x=r->s; x^=x<<13; x^=x>>7; x^=x<<17; return r->s=x; }
static uint32_t rng_below(rng_t* r, uint32_t n){ return (uint32_t)(rng_next(r)%n); }

// INV16 (WP2 snapshot pipe): the 4-word per-epoch input snapshot MAIN grants must reach
// run() intact and coherent — the ring is a faithful pipe for input, never zeroing,
// truncating, or splicing another epoch's words. MAIN builds each snapshot as a pure
// function of the epoch id E (mk_snap); fk_run (on CORE) asserts the relations still hold
// (snap_ok). A dropped/reordered/torn snapshot word fails the check. The engine treats
// snap as opaque (policy-free) — this coherence contract lives entirely in the harness,
// which owns the snapshot's meaning, mirroring how minarch will pack real input into it.
static void mk_snap(uint64_t E, uint64_t out[4]){
	out[0]=E;
	out[1]=~E;
	out[2]=E*0x9E3779B97F4A7C15ull;
	out[3]=E^0xA5A5A5A5A5A5A5A5ull;
}
static int snap_ok(const uint64_t s[4]){
	return s[1]==~s[0] && s[2]==s[0]*0x9E3779B97F4A7C15ull && s[3]==(s[0]^0xA5A5A5A5A5A5A5A5ull);
}

// ---- the fake core ---------------------------------------------------------------
typedef struct {
	fc* f;
	int fail_op;     // fc_op to fail at, or 0 for none
	int fail_kind;   // -1 hard failure, +1 requested shutdown
	rng_t run_rng;

	// teardown record (written by CORE during terminal_cleanup, read by MAIN
	// after pthread_join → join is the happens-before; plain ints are safe/TSan-clean)
	int crash_armed;
	int disarm_called;
	int unload_called;
	int deinit_called;

	// runtime emit accounting (CORE-only during a session)
	uint64_t frames_emitted, cmds_emitted, barriers_emitted;

	// D-a2 frame-pool model: CORE acquires a buffer per published frame (++), MAIN retires
	// via fk_retire (--); an FR_DROPPED frame is retired CORE-side. Must be 0 after teardown
	// (INV18) — a DISCARD-skipped frame that never retired would leave this > 0.
	_Atomic int pool_outstanding;

	// D-g: this epoch's emitted audio shortfall, published before fr_core_run_done so the
	// depth-1 pump (which blocks for THIS epoch's RUN_DONE) can cross-check transport fidelity.
	_Atomic uint32_t last_sf;

	// D59 cancellation sub-test: when use_gate, fk_run blocks on this gate so MAIN can
	// observe in_run==1, request an async stop, then release it — proving the depth-1
	// fc_pump block terminates cleanly (never hangs) when stop lands mid-epoch.
	int use_gate;
	pthread_mutex_t gate_lk;
	pthread_cond_t  gate_cv;
	int gate_open;
} fakecore;

static int stage_rc(fakecore* fk, int op) {
	assert_affinity(op); // D57: audio/renderer on MAIN, all other stages on CORE
	if (fk->fail_op == op) return fk->fail_kind < 0 ? -1 : +1;
	return 0;
}
static int fk_get_info (void* c){ return stage_rc((fakecore*)c, FC_OP_GET_INFO); }
static int fk_init     (void* c){ return stage_rc((fakecore*)c, FC_OP_INIT); }
static int fk_open     (void* c){ return stage_rc((fakecore*)c, FC_OP_OPEN); }
static int fk_load     (void* c){
	fakecore* fk = c;
	int r = stage_rc(fk, FC_OP_LOAD);
	if (!r) {
		// D-b: a real core can emit env callbacks (SET_VARIABLES / SET_SYSTEM_AV_INFO)
		// during load_game. These run in FCP_BOOTSTRAP_SVC, so they MUST route to the
		// service stream — routing them to the run stream asserts `in_run` (no open epoch),
		// and a broken service-barrier would hang here (fail-closed abort). Exercise both.
		fc_emit_cmd(fk->f, FR_EVF_PERSISTENT, 0xB007);   // SET_VARIABLES-like
		fc_emit_barrier(fk->f, 0xA71F);                   // SET_SYSTEM_AV_INFO-like (apply-then-wait)
		atomic_fetch_add(&g_svc_cmd_emitted, 1);
	}
	return r;
}
static int fk_memory   (void* c){ return stage_rc((fakecore*)c, FC_OP_MEMORY); }
static int fk_armcrash (void* c){ fakecore* fk=c; int r=stage_rc(fk,FC_OP_ARM_CRASH); if(!r) fk->crash_armed=1; return r; }
static int fk_av       (void* c){ return stage_rc((fakecore*)c, FC_OP_AV); }
static int fk_ctrl     (void* c){ return stage_rc((fakecore*)c, FC_OP_CONTROLLER); }
static int fk_audio    (void* c){ return stage_rc((fakecore*)c, FC_OP_AUDIO); }
static int fk_renderer (void* c){ return stage_rc((fakecore*)c, FC_OP_RENDERER); }
static int fk_resume   (void* c){ // resume failure is NON-FATAL — exercise it failing
	assert_affinity(FC_OP_RESUME); // CORE
	fakecore* fk=c; return (fk->fail_op==FC_OP_RESUME) ? -1 : 0;
}

// The adversarial run(): a randomized epoch. Emits 0..N frames (some NULL-dup),
// geometry commands, an AV_INFO barrier, SET_VARIABLE — the real cross-thread mix.
static void fk_run(void* c, uint64_t gen, uint32_t slot, const uint64_t snap[4]){
	fakecore* fk = c;
	if (g_tid_ready) assert(!pthread_equal(pthread_self(), g_main_tid) && "fk_run must be on CORE (D57)");
	assert(snap_ok(snap) && "INV16: epoch input snapshot must reach run() intact & coherent");
	atomic_fetch_add(&g_snap_checks, 1);  // coverage: prove the check actually ran (not dead)
	// INV17 (WP-G / D-k): the credit slot delivered to run() must identify THIS epoch's
	// pool slot — slot == gen % depth. Anything else means the frame pool would index the
	// wrong buffer at depth 2 (aliasing frame N against N+1's storage).
	assert(slot == gen % fk->f->fr.depth && "INV17: run() slot must equal gen % depth");
	atomic_fetch_add(&g_slot_checks, 1);
	if (fk->use_gate){  // D59 cancel test: park inside the epoch until MAIN opens the gate
		pthread_mutex_lock(&fk->gate_lk);
		while (!fk->gate_open) pthread_cond_wait(&fk->gate_cv, &fk->gate_lk);
		pthread_mutex_unlock(&fk->gate_lk);
		atomic_fetch_add(&fk->pool_outstanding, 1);           // D-a2 acquire
		if (fc_emit_frame(fk->f, 0xF00D) == FR_OK) fk->frames_emitted++;   // one frame, then the epoch closes
		else atomic_fetch_sub(&fk->pool_outstanding, 1);      // FR_DROPPED: CORE retires
		atomic_store(&fk->last_sf, 0);                        // D-g: gate epoch emits no shortfall
		return;
	}
	rng_t* r = &fk->run_rng;
	uint32_t n = rng_below(r, 8);
	int did_barrier = 0;
	uint32_t emitted = 0;
	for (uint32_t i=0;i<n;i++){
		uint32_t roll = rng_below(r,100);
		if (roll < 45){
			if (rng_below(r,10)==0){ fc_signal_dup(fk->f); }        // NULL dup
			else {
				atomic_fetch_add(&fk->pool_outstanding, 1);          // D-a2 acquire a pool buffer
				if (fc_emit_frame(fk->f, 0xF00D ^ i) == FR_OK){ emitted++; fk->frames_emitted++; }
				else atomic_fetch_sub(&fk->pool_outstanding, 1);     // FR_DROPPED: not published, CORE retires
			}
		} else if (roll < 70){
			if (fc_emit_cmd(fk->f, 0, 0xC0DE ^ i) != FR_DROPPED) fk->cmds_emitted++;  // SET_VARIABLE-ish
		} else if (roll < 85){
			if (fc_emit_cmd(fk->f, FR_EVF_PERSISTENT, 0x6E0 ^ i) != FR_DROPPED) fk->cmds_emitted++; // GEOMETRY
		} else if (roll < 93 && !did_barrier){
			fc_emit_barrier(fk->f, 0xA71F ^ i); fk->barriers_emitted++; fk->cmds_emitted++; did_barrier=1; // AV_INFO
		} else {
			struct timespec ts={0,(long)rng_below(r,30000)}; nanosleep(&ts,NULL);
		}
	}
	// D-g: emit this epoch's audio shortfall (sometimes in two pieces to test accumulation),
	// then publish the total so the depth-1 pump can cross-check RUN_DONE transport (INV20).
	uint32_t sf_sum = 0, sf = rng_below(r, 4);
	if (sf)                    { fc_signal_audio_shortfall(fk->f, sf); sf_sum += sf; }
	if (rng_below(r,3) == 0)   { fc_signal_audio_shortfall(fk->f, 1);  sf_sum += 1;  }
	atomic_store(&fk->last_sf, sf_sum);
	(void)emitted;
}
static int  fk_serialize  (void* c, uint64_t a){ (void)a; return (rng_below(&((fakecore*)c)->run_rng,20)==0)?-1:0; }
static int  fk_unserialize(void* c, uint64_t a){ (void)a; return (rng_below(&((fakecore*)c)->run_rng,20)==0)?-1:0; }
static void fk_reset      (void* c){ (void)c; }
static void fk_disarm     (void* c){ fakecore* fk=c; fk->disarm_called=1; }
static void fk_unload     (void* c){ fakecore* fk=c; fk->unload_called=1; }
static void fk_deinit     (void* c){
	fakecore* fk=c; fk->deinit_called=1;
	// D-b2: a core's deinit can emit env callbacks (FCP_TEARDOWN_SVC -> service stream). Emit
	// MORE than the (tiny -DFR_SVC_STREAM=4) service stream holds, so fr_core_service_emit
	// BLOCKS and only completes if fc_teardown keeps draining until STOPPED. Missing that
	// drain => CORE stuck emitting + MAIN in join = deadlock (fail-closed abort).
	for (int i=0;i<6;i++) fc_emit_cmd(fk->f, FR_EVF_PERSISTENT, 0xDEAD00 + i);
	atomic_fetch_add(&g_teardown_emits, 1);
}
static void fk_retire(void* ctx, uint64_t payload){
	(void)payload; fakecore* fk = ctx;
	atomic_fetch_sub(&fk->pool_outstanding, 1);  // MAIN returns the published buffer to the pool
	atomic_fetch_add(&g_pool_retires, 1);
}

static void fill_vtable(fc_vtable* vt, fakecore* fk){
	memset(vt,0,sizeof(*vt));
	vt->ctx=fk;
	vt->get_system_info=fk_get_info; vt->init=fk_init; vt->open_content=fk_open;
	vt->load_game=fk_load; vt->setup_memory=fk_memory; vt->arm_crash=fk_armcrash;
	vt->get_av_info=fk_av; vt->set_controller=fk_ctrl; vt->audio_init=fk_audio;
	vt->renderer_init=fk_renderer; vt->resume=fk_resume;
	vt->run=fk_run; vt->serialize=fk_serialize; vt->unserialize=fk_unserialize; vt->reset=fk_reset;
	vt->disarm_crash=fk_disarm; vt->unload_game=fk_unload; vt->deinit=fk_deinit;
}

// ---- oracle: expected teardown set as a pure fn of highest state reached --------
typedef struct { int disarm, unload, deinit; } tset;
static tset oracle(fc_state s){
	tset t = {0,0,0};
	if (s >= FC_MEMORY_READY)   t.disarm = 1;
	if (s >= FC_CONTENT_LOADED) t.unload = 1;
	if (s >= FC_INITIALIZED)    t.deinit = 1;
	return t;
}
// expected reached state when failing at a given op (hard fail / shutdown both stop
// bootstrap at the same reached state — the op that failed did not complete).
static fc_state reached_on_fail(int op){
	switch(op){
	case FC_OP_GET_INFO:   return FC_CORE_CREATED;
	case FC_OP_INIT:       return FC_INFO_READY;
	case FC_OP_OPEN:       return FC_INITIALIZED;   // open is post-init path prep
	case FC_OP_LOAD:       return FC_INITIALIZED;
	case FC_OP_MEMORY:     return FC_CONTENT_LOADED;
	case FC_OP_ARM_CRASH:  return FC_CONTENT_LOADED;
	case FC_OP_AV:         return FC_MEMORY_READY;
	case FC_OP_CONTROLLER: return FC_AV_READY;
	case FC_OP_AUDIO:      return FC_AV_READY;
	case FC_OP_RENDERER:   return FC_AV_READY;
	default:               return FC_RUNNING; // no fail / resume(nonfatal) -> full boot
	}
}

static int inv_hits[32];
#define INV(n,cond,msg) do{ if(!(cond)){ fprintf(stderr,"FC INVARIANT %d FAILED: %s\n",(n),(msg)); abort(); } inv_hits[n]++; }while(0)

// consumer drain callback: counts applied events (frame/cmd/rundone ORDER is framering's
// job, proven in framering_test). With a non-NULL ctx it also records a per-drain tally
// so the depth-1 serial-rendezvous invariants (D59) can assert one-pump-one-epoch.
typedef struct { uint64_t frames, rundone, events; uint32_t shortfall; } drainrec;
static uint64_t applied_events;
static void on_ev(void* ctx, const fr_event* ev){
	applied_events++;
	if (ctx){
		drainrec* d = ctx;
		d->events++;
		if      (ev->kind == FR_EV_FRAME)    d->frames++;
		else if (ev->kind == FR_EV_RUN_DONE) { d->rundone++; d->shortfall += ev->shortfall; }
	}
}

// Drive bootstrap STAGE-BY-STAGE via fc_boot_stage/fc_boot_finish (what real minarch
// does — D58), interleaving a simulated 'MAIN frontend work' no-op between stages. Must
// reach the same state as the monolithic fc_bootstrap, keep the D57 thread-affinity
// oracle green, and NOT flip the ring to RUNNING until fc_boot_finish. The no-op MAIN
// work between stages proves the interleave is race-free (CORE quiescent between stages).
static fc_state boot_stage_by_stage(fc* f){
	static const fc_op seq[] = {
		FC_OP_GET_INFO, FC_OP_INIT, FC_OP_OPEN, FC_OP_LOAD, FC_OP_MEMORY,
		FC_OP_ARM_CRASH, FC_OP_AV, FC_OP_CONTROLLER, FC_OP_AUDIO,
		FC_OP_RENDERER, FC_OP_RESUME,
	};
	for (unsigned i=0;i<sizeof(seq)/sizeof(seq[0]);i++){
		fc_boot_stage(f, seq[i]);
		volatile int main_work=0; for(int k=0;k<8;k++) main_work+=k; (void)main_work; // sim MAIN frontend work
		if (fc_boot_failed(f)) return fc_get_state(f);
	}
	// INV9: all stages succeeded but the ring has NOT been released to RUNNING yet —
	// state is RESUME_APPLIED; grants only flow after the explicit fc_boot_finish.
	INV(9, fc_get_state(f)==FC_RESUME_APPLIED, "RUNNING flipped before fc_boot_finish");
	return fc_boot_finish(f);
}

// run ONE session with a given failure injection; assert the oracle. stage_by_stage
// selects the fc_boot_stage driving path (else monolithic fc_bootstrap).
static void run_session(int fail_op, int fail_kind, rng_t* mr, int stage_by_stage){
	fakecore fk; memset(&fk,0,sizeof(fk));
	fk.fail_op = fail_op; fk.fail_kind = fail_kind;
	fk.run_rng.s = rng_next(mr) | 1;
	fc_vtable vt; fill_vtable(&vt,&fk);

	fc F; fk.f=&F;
	uint32_t depth = (rng_below(mr,2)?2:1);
	fc_init(&F,&vt,depth);
	fc_set_frame_retire_cb(&F, fk_retire, &fk);   // D-a2: model the frame pool

	fc_state reached = stage_by_stage ? boot_stage_by_stage(&F) : fc_bootstrap(&F);
	// RESUME failure is NON-FATAL by policy: that injection completes bootstrap and
	// behaves like the success path (while still exercising vt.resume returning <0).
	int failed = (fail_op!=0 && fail_op!=FC_OP_RESUME);

	// INV1: bootstrap reached exactly the state the oracle predicts for this injection
	fc_state expect = failed ? reached_on_fail(fail_op) : FC_RESUME_APPLIED;
	INV(1, reached==expect || (!failed && reached==FC_RUNNING),
	    "bootstrap reached the wrong state for the injection");
	// INV2: on failure, NO teardown has happened yet (cleanup is teardown's job)
	if (failed)
		INV(2, !fk.disarm_called && !fk.unload_called && !fk.deinit_called,
		    "teardown ran during bootstrap failure");

	// success path: drive an adversarial RUNNING session concurrently with pumps
	if (!failed){
		struct timespec t0; clock_gettime(CLOCK_MONOTONIC,&t0);
		for(;;){
			struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
			double dt = (now.tv_sec-t0.tv_sec) + (now.tv_nsec-t0.tv_nsec)/1e9;
			if (dt > 0.05) break;   // ~50ms of adversarial runtime per success session
			// Sample the ring BEFORE the pump. With depth 1 + RUNNING + no credit
			// outstanding (and this single-threaded driver leaves no park/stop pending at
			// the loop top), fr_grant is GUARANTEED to land — so the pump MUST rendezvous
			// with that epoch's RUN_DONE. Guarding on credits_before==0 makes the check
			// self-contained (independent of the depth-change gate's reclaim timing).
			uint32_t depth_before   = F.fr.depth;
			int      run_before     = (fr_get_state(&F.fr) == FR_RUNNING);
			uint32_t credits_before = atomic_load(&F.fr.credits_out);
			uint64_t gen_before     = atomic_load(&F.fr.gen);
			drainrec dr = {0};
			uint64_t snap[4]; mk_snap(gen_before+1, snap);   // INV16: coherent per-epoch input snapshot
			fc_pump(&F, snap, on_ev, &dr);
			// INV10-13 (D59 serial rendezvous / anti-busy-spin): a depth-1 pump that
			// issued a grant must have BLOCKED until exactly one epoch drained — no credit
			// left outstanding, CORE not mid-run, gen advanced by one. All four FAIL
			// against the old grant+drain+return pump that returned before RUN_DONE.
			if (depth_before == 1 && run_before && credits_before == 0){
				INV(10, atomic_load(&F.fr.in_run) == 0, "depth-1 pump returned with CORE still mid-run (busy-spin)");
				INV(11, atomic_load(&F.fr.credits_out) == 0, "depth-1 pump returned with a credit outstanding");
				INV(12, dr.rundone == 1, "depth-1 pump did not drain exactly one RUN_DONE epoch");
				INV(13, atomic_load(&F.fr.gen) == gen_before+1, "depth-1 pump did not advance gen by exactly one");
				// INV20 (D-g): the shortfall the epoch accumulated reaches MAIN in RUN_DONE
				// unchanged. depth-1 blocked for exactly THIS epoch's RUN_DONE and CORE is now
				// parked in wait_grant (no next epoch until the next pump), so last_sf is stable.
				INV(20, dr.shortfall == atomic_load(&fk.last_sf), "audio shortfall not transported to RUN_DONE (D-g)");
			}
			uint32_t roll = rng_below(mr,100);
			if (roll < 12){ fc_park(&F,on_ev,NULL, roll<4); fc_release(&F); }         // park (some discard)
			else if (roll < 18){ uint32_t nd=(rng_below(mr,2)?2:1); fc_set_depth(&F,nd,on_ev,NULL,0); fc_release(&F); }
			else if (roll < 24){ fc_menu_op(&F, FC_OP_SERIALIZE, rng_below(mr,8), on_ev, NULL); }
			else if (roll < 28){ fc_menu_op(&F, FC_OP_UNSERIALIZE, rng_below(mr,8), on_ev, NULL); }
			else if (roll < 30){ fc_menu_op(&F, FC_OP_RESET, 0, on_ev, NULL); }
		}
	}

	fc_state at_teardown = fc_get_state(&F);
	fc_teardown(&F);

	// INV3: terminal cleanup oracle — the teardown set equals oracle(reached state)
	tset ex = oracle(at_teardown);
	INV(3, fk.disarm_called==ex.disarm, "disarm_crash legality violated (oracle)");
	INV(4, fk.unload_called==ex.unload, "unload_game legality violated (oracle)");
	INV(5, fk.deinit_called==ex.deinit, "deinit legality violated (oracle)");
	// INV6: crash disarm implies it had been armed (never disarm an unarmed handler)
	if (fk.disarm_called) INV(6, fk.crash_armed, "disarmed a crash handler that was never armed");
	// INV7: unload never without load; deinit never without init (belt over the oracle)
	INV(7, !(fk.unload_called && at_teardown < FC_CONTENT_LOADED), "unload_game before content loaded");
	INV(8, !(fk.deinit_called && at_teardown < FC_INITIALIZED), "deinit before init");
	// INV18 (D-a2): every acquired frame buffer was retired — on present, DISCARD-skip, or
	// park/stop drainage. teardown joins CORE first, so this read is race-free. A nonzero
	// value is the DISCARD-skip pool leak (a skipped frame consumed but never retired).
	INV(18, atomic_load(&fk.pool_outstanding) == 0, "frame-pool leak: acquired != retired (D-a2)");
}

// ---- D59: deterministic stop-cancel sub-test --------------------------------------
// A depth-1 pump BLOCKS until its epoch's RUN_DONE. Prove that block ALWAYS terminates
// when a stop lands mid-epoch: a gated fk_run lets MAIN observe in_run==1, fire an async
// fr_stop, then open the gate. The pump must return (via the drained RUN_DONE or the
// STOPPED state edge) — a hang here would wedge the real run loop. MAIN is the ONLY
// non-consumer here (it drives stop + gate); the pump thread is the sole ring consumer,
// so the single-consumer contract holds.
static _Atomic int g_pump_returned;
static void* pump_thread_fn(void* p){
	fc* f = (fc*)p;
	drainrec dr = {0};
	uint64_t snap[4]; mk_snap(atomic_load(&f->fr.gen)+1, snap);   // INV16: coherent per-epoch input snapshot
	fc_pump(f, snap, on_ev, &dr);
	atomic_store(&g_pump_returned, 1);
	return NULL;
}
static void poll_until(_Atomic int* flag, int want, const char* what){
	struct timespec d0; clock_gettime(CLOCK_MONOTONIC,&d0);
	while (atomic_load(flag) != want){
		struct timespec n; clock_gettime(CLOCK_MONOTONIC,&n);
		if (n.tv_sec - d0.tv_sec > 5){ fprintf(stderr,"cancel-test hung: %s\n", what); abort(); }
		struct timespec s={0,200000}; nanosleep(&s,NULL);  // 0.2ms
	}
}
static void run_cancel_test(rng_t* mr){
	fakecore fk; memset(&fk,0,sizeof(fk));
	fk.run_rng.s = rng_next(mr) | 1;
	fk.use_gate = 1;
	pthread_mutex_init(&fk.gate_lk,NULL);
	pthread_cond_init(&fk.gate_cv,NULL);
	fc_vtable vt; fill_vtable(&vt,&fk);
	fc F; fk.f=&F;
	fc_init(&F,&vt,1);   // depth 1: serial rendezvous
	fc_set_frame_retire_cb(&F, fk_retire, &fk);   // D-a2: model the frame pool
	fc_state reached = fc_bootstrap(&F);
	INV(14, reached==FC_RESUME_APPLIED || reached==FC_RUNNING, "cancel-test bootstrap did not reach RUNNING");

	atomic_store(&g_pump_returned, 0);
	pthread_t pt; pthread_create(&pt,NULL,pump_thread_fn,&F);
	poll_until(&F.fr.in_run, 1, "CORE never entered gated run");  // grant consumed, epoch open
	fr_stop(&F.fr);                                               // async stop mid-epoch
	pthread_mutex_lock(&fk.gate_lk); fk.gate_open=1; pthread_cond_broadcast(&fk.gate_cv); pthread_mutex_unlock(&fk.gate_lk);
	poll_until(&g_pump_returned, 1, "depth-1 pump hung under mid-epoch stop");
	INV(15, 1, "depth-1 pump returned cleanly under mid-epoch stop");  // site marker; the hang-abort is the real gate
	pthread_join(pt,NULL);

	fc_teardown(&F);    // CORE already stopping; teardown reconciles the ledger + joins
	pthread_cond_destroy(&fk.gate_cv);
	pthread_mutex_destroy(&fk.gate_lk);
}

int main(int argc, char** argv){
	g_main_tid = pthread_self(); g_tid_ready = 1; // D57 affinity oracle baseline
	int seconds = argc>1?atoi(argv[1]):3;
	uint64_t seed = argc>2?strtoull(argv[2],NULL,0):(uint64_t)time(NULL)*2654435761u;
	printf("frontend_core_test: seconds=%d seed=0x%016" PRIx64 "\n", seconds, seed);
	fflush(stdout);
	rng_t mr = { seed?seed:1 };

	// D59: depth-1 stop-cancel — the serial-rendezvous block must always terminate
	for (int i=0;i<3;i++) run_cancel_test(&mr);

	// every bootstrap failure injection point, hard-fail AND requested-shutdown, plus
	// the resume-nonfatal path and the clean success path — cycled until time runs out
	static const int fail_ops[] = {
		0, FC_OP_GET_INFO, FC_OP_INIT, FC_OP_OPEN, FC_OP_LOAD, FC_OP_MEMORY,
		FC_OP_ARM_CRASH, FC_OP_AV, FC_OP_CONTROLLER, FC_OP_AUDIO, FC_OP_RENDERER,
		FC_OP_RESUME,
	};
	struct timespec t0; clock_gettime(CLOCK_MONOTONIC,&t0);
	uint64_t sessions=0;
	for(;;){
		struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
		if (now.tv_sec - t0.tv_sec >= seconds) break;
		for (unsigned i=0;i<sizeof(fail_ops)/sizeof(fail_ops[0]);i++){
			int op = fail_ops[i];
			int kind = (op==FC_OP_RESUME) ? -1 : (rng_below(&mr,2)?-1:+1);
			// every injection driven BOTH ways: monolithic fc_bootstrap AND the
			// stage-by-stage fc_boot_stage path real minarch uses (D58). Same oracle.
			run_session(op, kind, &mr, 0);
			run_session(op, kind, &mr, 1);
			sessions += 2;
		}
	}

	// INV16 (WP2 snapshot pipe): the per-epoch input snapshot reaches run() intact &
	// coherent. The fail-fast is the CORE-side snap_ok() assert in fk_run (aborts at the
	// exact torn epoch); this MAIN-side site (join HB → atomic is safe) proves the check
	// was actually exercised across the adversarial sessions, so it can never be dead.
	INV(16, atomic_load(&g_snap_checks) > 0, "snapshot-fidelity check was never exercised");
	// INV17 (WP-G / D-k): slot==gen%depth delivered to run() — coverage guard for the same reason.
	INV(17, atomic_load(&g_slot_checks) > 0, "slot-identity check was never exercised");
	// INV19 (D-a2): the retirement hook actually fired (else INV18's balance is trivially 0).
	INV(19, atomic_load(&g_pool_retires) > 0, "frame-retirement hook was never exercised");
	// INV21 (D-b): a bootstrap-service command+barrier was emitted and routed to the service
	// stream. Correctness is proven by the RUN of these tests NOT aborting on fr_core_emit's
	// `in_run` assert (mis-route) and NOT hanging (broken service barrier); this is coverage.
	INV(21, atomic_load(&g_svc_cmd_emitted) > 0, "bootstrap-service command routing never exercised");
	// INV22 (D-b2): deinit-time service emits (more than the stream holds) were drained by
	// fc_teardown's until-STOPPED loop. A missing drain deadlocks (fail-closed), so reaching
	// here with this > 0 proves the teardown drain unblocks a cleanup that overflows the stream.
	INV(22, atomic_load(&g_teardown_emits) > 0, "teardown-drain (D-b2) never exercised");

	int sites=0; for(int i=0;i<32;i++) if(inv_hits[i]) sites++;
	printf("sessions=%" PRIu64 " applied_events=%" PRIu64 "\n", sessions, applied_events);
	printf("frontend_core_test: ALL PASS (%d invariant sites, cleanup oracle verified at every bootstrap stage)\n", sites);
	return 0;
}
