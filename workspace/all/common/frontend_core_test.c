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
static int fk_load     (void* c){ return stage_rc((fakecore*)c, FC_OP_LOAD); }
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
static void fk_run(void* c){
	fakecore* fk = c;
	if (g_tid_ready) assert(!pthread_equal(pthread_self(), g_main_tid) && "fk_run must be on CORE (D57)");
	if (fk->use_gate){  // D59 cancel test: park inside the epoch until MAIN opens the gate
		pthread_mutex_lock(&fk->gate_lk);
		while (!fk->gate_open) pthread_cond_wait(&fk->gate_cv, &fk->gate_lk);
		pthread_mutex_unlock(&fk->gate_lk);
		fc_emit_frame(fk->f, 0xF00D); fk->frames_emitted++;   // one frame, then the epoch closes
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
			else if (fc_emit_frame(fk->f, 0xF00D ^ i) == FR_OK){ emitted++; fk->frames_emitted++; }
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
	(void)emitted;
}
static int  fk_serialize  (void* c, uint64_t a){ (void)a; return (rng_below(&((fakecore*)c)->run_rng,20)==0)?-1:0; }
static int  fk_unserialize(void* c, uint64_t a){ (void)a; return (rng_below(&((fakecore*)c)->run_rng,20)==0)?-1:0; }
static void fk_reset      (void* c){ (void)c; }
static void fk_disarm     (void* c){ fakecore* fk=c; fk->disarm_called=1; }
static void fk_unload     (void* c){ fakecore* fk=c; fk->unload_called=1; }
static void fk_deinit     (void* c){ fakecore* fk=c; fk->deinit_called=1; }

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
typedef struct { uint64_t frames, rundone, events; } drainrec;
static uint64_t applied_events;
static void on_ev(void* ctx, const fr_event* ev){
	applied_events++;
	if (ctx){
		drainrec* d = ctx;
		d->events++;
		if      (ev->kind == FR_EV_FRAME)    d->frames++;
		else if (ev->kind == FR_EV_RUN_DONE) d->rundone++;
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
			drainrec dr = {0,0,0};
			uint64_t snap[4] = { gen_before+1, rng_next(mr), rng_next(mr), 0 };
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
	drainrec dr = {0,0,0};
	uint64_t snap[4] = { atomic_load(&f->fr.gen)+1, 0, 0, 0 };
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

	int sites=0; for(int i=0;i<32;i++) if(inv_hits[i]) sites++;
	printf("sessions=%" PRIu64 " applied_events=%" PRIu64 "\n", sessions, applied_events);
	printf("frontend_core_test: ALL PASS (%d invariant sites, cleanup oracle verified at every bootstrap stage)\n", sites);
	return 0;
}
