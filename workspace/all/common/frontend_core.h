// frontend_core — threading v2 phase 2: the CORE-thread lifecycle engine.
//
// This is the reusable integration layer between framering (the protocol) and a
// libretro core. It owns the F31 nine-state bootstrap machine, the RUNNING epoch
// loop, the QUIESCENT service loop, and the per-state terminal cleanup oracle —
// as PURE logic parameterised by a vtable of core operations. No SDL, no libretro
// headers, no platform code: minarch (phase 2b, build-guarded) fills the vtable
// with real libretro calls; the host harness fills it with an adversarial fake
// core. The harness therefore tests the SHIPPING lifecycle code, not a model of it
// (the "harness is not a model" requirement of the v2.4 contract).
//
// Thread roles inherited from framering: CORE is the sole thread that ever touches
// the vtable's core operations, from creation to exit (F23 — one thread the core's
// whole life, including bootstrap). MAIN drives grants/drains/park/service and reads
// the published lifecycle state.
#ifndef FRONTEND_CORE_H
#define FRONTEND_CORE_H

#include "framering.h"
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

// F31 bootstrap states, in order. The terminal cleanup oracle keys off the highest
// state REACHED: which teardown calls are legal is a pure function of this.
typedef enum {
	FC_NONE = 0,        // thread not created
	FC_CORE_CREATED,    // CORE thread alive, nothing called yet
	FC_INFO_READY,      // get_system_info done
	FC_INITIALIZED,     // retro_init done (=> deinit now legal)
	FC_CONTENT_LOADED,  // load_game ok  (=> unload_game now legal)
	FC_MEMORY_READY,    // SRAM/RTC pointers valid + crash handler ARMED
	FC_AV_READY,        // get_system_av_info + controller
	FC_FRONTEND_READY,  // audio + renderer init (see note: SDL-side; D-log candidate)
	FC_RESUME_APPLIED,  // auto-resume unserialize attempted (failure NON-FATAL)
	FC_RUNNING,         // first grant consumed; the epoch loop is live
} fc_state;

// vtable return convention for fallible ops: 0 = ok, <0 = failure, >0 = the core
// requested shutdown at this stage. Bootstrap stops at the reached state either way.
typedef struct fc_vtable {
	void* ctx;

	// --- bootstrap stages (each executed on CORE as one service op) ---
	int  (*get_system_info)(void* c);
	int  (*init)(void* c);            // retro_init
	int  (*open_content)(void* c);    // content/path prep (frontend, but CORE-serialised)
	int  (*load_game)(void* c);       // retro_load_game
	int  (*setup_memory)(void* c);    // get_memory_size/data, SRAM/RTC load
	int  (*arm_crash)(void* c);       // arm emergency-save AFTER memory pointers valid
	int  (*get_av_info)(void* c);     // retro_get_system_av_info
	int  (*set_controller)(void* c);
	int  (*audio_init)(void* c);      // NOTE: SDL-side in real minarch — see D-log draft
	int  (*renderer_init)(void* c);   // NOTE: SDL-side in real minarch — see D-log draft
	int  (*resume)(void* c);          // auto-resume unserialize; nonfatal by policy

	// --- runtime ---
	void (*run)(void* c);             // one epoch; calls fc_emit_* from within
	int  (*serialize)(void* c, uint64_t arg);
	int  (*unserialize)(void* c, uint64_t arg);
	void (*reset)(void* c);

	// --- teardown (recorded by the core; the oracle governs which are legal) ---
	void (*disarm_crash)(void* c);
	void (*unload_game)(void* c);     // legal ONLY if state >= FC_CONTENT_LOADED
	void (*deinit)(void* c);          // legal ONLY if state >= FC_INITIALIZED
} fc_vtable;

// service op codes: bootstrap stages are ops so they run on CORE via fr_service.
// Runtime menu ops (save/load/reset) reuse the same channel.
typedef enum {
	FC_OP_GET_INFO = 1, FC_OP_INIT, FC_OP_OPEN, FC_OP_LOAD, FC_OP_MEMORY,
	FC_OP_ARM_CRASH, FC_OP_AV, FC_OP_CONTROLLER, FC_OP_AUDIO, FC_OP_RENDERER,
	FC_OP_RESUME,
	FC_OP_SERIALIZE, FC_OP_UNSERIALIZE, FC_OP_RESET,   // runtime menu ops
} fc_op;

typedef struct fc {
	fr_ring fr;
	fc_vtable vt;

	_Atomic int state;        // fc_state, written by CORE, read by MAIN
	_Atomic int boot_failed;  // set by CORE when a bootstrap stage fails/shutdowns
	_Atomic int boot_shutdown;// distinguish requested-shutdown from hard failure
	_Atomic uint64_t last_op_result; // per-op: 0 ok / nonzero packs (op<<8|code)

	pthread_t core_thread;
	int thread_started;
} fc;

// ---- MAIN-side lifecycle API ----
// Initialise, spawn the CORE thread (born QUIESCENT), depth = 1 (serial) or 2.
void fc_init(fc* f, const fc_vtable* vt, uint32_t depth);

// Run the F31 bootstrap: issues each stage as a service op in order, stops at the
// first failure/shutdown. Returns the highest state reached; on success == FC_RUNNING
// (and the ring has been released to RUNNING, ready for grants).
fc_state fc_bootstrap(fc* f);

// One pacer tick in RUNNING: issue a grant if a credit is free, then drain+present.
// cb receives applied frames/commands in order (MAIN applies them).
void fc_pump(fc* f, const uint64_t snapshot[4], fr_drain_cb cb, void* cbctx);

// Reversible transitions (session + CORE thread survive).
void fc_park(fc* f, fr_drain_cb cb, void* cbctx, int discard);
void fc_release(fc* f);
void fc_set_depth(fc* f, uint32_t depth, fr_drain_cb cb, void* cbctx, int discard);
// Runtime menu op executed on CORE (save/load/reset): parks, services, releases.
int  fc_menu_op(fc* f, fc_op op, uint64_t arg, fr_drain_cb cb, void* cbctx);

// Terminal teardown: park, request stop; CORE runs the state-keyed cleanup oracle
// (unload_game only if loaded, deinit only if initialised, disarm crash if armed),
// exits; MAIN joins. Never frees/joins out of order. Safe to call from any reached
// state, including a failed bootstrap (that's the point).
void fc_teardown(fc* f);

static inline fc_state fc_get_state(const fc* f) {
	return (fc_state)atomic_load_explicit((_Atomic int*)&f->state, memory_order_acquire);
}

// ---- CORE-side emit shims (called from inside vt->run, i.e. from the core's
// video_refresh/audio/environment callbacks). Thin pass-throughs to framering so
// callback code needn't know the ring object. ----
fr_rc fc_emit_frame(fc* f, uint64_t payload);
fr_rc fc_emit_cmd(fc* f, uint32_t flags, uint64_t payload);   // flags: FR_EVF_*
fr_rc fc_emit_barrier(fc* f, uint64_t payload);               // AV_INFO: cmd+persistent+barrier, then wait
void  fc_signal_dup(fc* f);                                   // NULL duplicate frame this epoch

#endif
