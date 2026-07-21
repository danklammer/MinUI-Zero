// dupskip.h — present-skip policy (duplicate detection + skip decision), extracted as a
// PURE unit (Codex P1#2): the skip predicate accreted several correctness fixes and its
// highest-risk state transitions (snapshot validity, streak cap, dirty-generation gating,
// audio-pacing gate) were untested. No I/O, no minarch types — the caller owns env flags,
// the 1Hz stats log, SDL/SND, and the GFX_finishFrameWork()/return that a skip performs.
//
// Per video frame (in the present path, after the NULL-data guard):
//   int dup = dupskip_detect(&s, data, w, h, pitch, bpp);   // 1 = visible pixels unchanged
//   DupSkipCtx c = { .enabled=..., .dup_frame=dup, ...current gates... };
//   if (dupskip_should_skip(&s, &c)) { finalize work sample; return; }  // skip present
//   force_present = 0;                                                  // caller-owned flag
#ifndef DUPSKIP_H
#define DUPSKIP_H

#include <stddef.h>

typedef struct {
	void*    prev;        // last presented VISIBLE frame, packed (no row padding)
	size_t   prev_sz;
	int      prev_valid;  // prev holds a real frame to compare against
	int      dup_streak;  // consecutive skipped duplicates (bounded by ctx.max_streak)
	unsigned clean_gen;   // present_dirty_gen as of the last present
	// Injectable allocator (NULL = malloc/free). Only for tests forcing allocation failure;
	// production leaves these zero. (Mirrors frame_pool's injected-realloc pattern.)
	void*  (*mem_alloc)(size_t);
	void   (*mem_free)(void*);
} DupSkip;

typedef struct {
	int      enabled;            // ZERO_DUP_SKIP active
	int      dup_frame;          // result of dupskip_detect() this frame
	int      geometry_dirty;     // dst pitch 0, or frame size != renderer target
	unsigned present_dirty_gen;  // frontend settings / aspect / effect generation
	int      fast_forward;
	int      presentation_drop;  // core uses presentation-drop (PS): never skip
	int      audio_active;       // SND_isActive(): audio is pacing (else vsync must)
	int      force_present;      // menu-exit / wake demands one present
	int      max_streak;         // force a present after this many skips (keeps telemetry/HUD live)
} DupSkipCtx;

// Free the snapshot and zero all state (teardown; not required mid-session — a geometry
// change reallocs and invalidates on its own).
void dupskip_reset(DupSkip* s);

// Returns 1 iff the VISIBLE pixels are byte-identical to the last presented frame.
//  - data==NULL returns 0 and leaves the snapshot UNTOUCHED (a NULL libretro dup frame must
//    not poison the snapshot).
//  - a CHANGED frame updates the snapshot to itself (it is about to present).
//  - allocation failure (or zero size) disables detection: returns 0 so everything presents.
//  - bpp = visible bytes per pixel (2 normally; 4 for a 32bpp core, pre-downsample).
// Compares visible pixels only, never padded pitch bytes (packed fast path when row==pitch).
int dupskip_detect(DupSkip* s, const void* data, unsigned width, unsigned height,
                   size_t pitch, int bpp);

// Returns 1 = skip this present, 0 = present. Updates dup_streak and clean_gen.
int dupskip_should_skip(DupSkip* s, const DupSkipCtx* c);

#endif
