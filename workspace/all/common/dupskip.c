// dupskip.c — see dupskip.h. Pure logic: no SDL, no SND, no logging, no env reads.
#include "dupskip.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void* ds_alloc(DupSkip* s, size_t n) { return s->mem_alloc ? s->mem_alloc(n) : malloc(n); }
static void  ds_free (DupSkip* s, void* p)  { if (s->mem_free) s->mem_free(p); else free(p); }

void dupskip_reset(DupSkip* s) {
	ds_free(s, s->prev);
	s->prev = NULL;
	s->prev_sz = 0;
	s->prev_valid = 0;
	s->dup_streak = 0;
	s->clean_gen = 0;
}

int dupskip_detect(DupSkip* s, const void* data, unsigned width, unsigned height,
                   size_t pitch, int bpp) {
	if (!data) return 0;                       // NULL dup frame: never poison the snapshot
	// Fail OPEN (present, never false-dup) on any malformed geometry (Codex F3): a valid
	// in-tree core never hits these, so this is hardening against a future/broken core.
	if (bpp <= 0 || width == 0 || height == 0) return 0;
	size_t row_bytes = (size_t)width * (size_t)bpp;
	if (row_bytes / (size_t)bpp != (size_t)width) return 0;   // width*bpp overflow
	if (row_bytes > pitch) return 0;                          // visible row exceeds supplied pitch
	if (row_bytes > SIZE_MAX / height) return 0;              // height*row_bytes overflow
	size_t sz = (size_t)height * row_bytes;
	if (sz == 0) return 0;

	if (sz != s->prev_sz) {                     // geometry/size change: realloc + invalidate
		ds_free(s, s->prev);
		s->prev = ds_alloc(s, sz);
		s->prev_sz = s->prev ? sz : 0;
		s->prev_valid = 0;
	}
	if (!s->prev || s->prev_sz != sz) return 0; // alloc failure: present everything

	int same = s->prev_valid;
	if (same) {
		if (row_bytes == pitch) same = (memcmp(s->prev, data, sz) == 0);
		else for (unsigned y = 0; y < height; y++) {
			if (memcmp((unsigned char*)s->prev + (size_t)y * row_bytes,
			           (const unsigned char*)data + (size_t)y * pitch, row_bytes) != 0) { same = 0; break; }
		}
	}
	if (same) return 1;                         // duplicate: keep the snapshot as-is

	// changed frame — it will present, so it becomes the new snapshot (packed, no padding)
	if (row_bytes == pitch) memcpy(s->prev, data, sz);
	else for (unsigned y = 0; y < height; y++)
		memcpy((unsigned char*)s->prev + (size_t)y * row_bytes,
		       (const unsigned char*)data + (size_t)y * pitch, row_bytes);
	s->prev_valid = 1;
	return 0;
}

int dupskip_should_skip(DupSkip* s, const DupSkipCtx* c) {
	if (c->enabled && c->dup_frame && !c->geometry_dirty
	    && s->clean_gen == c->present_dirty_gen
	    && !c->fast_forward && !c->presentation_drop
	    && c->audio_active && !c->force_present
	    && s->dup_streak < c->max_streak) {
		s->dup_streak++;
		return 1;                               // skip this present
	}
	s->dup_streak = 0;
	s->clean_gen = c->present_dirty_gen;        // resync the settings generation on every present
	return 0;
}
