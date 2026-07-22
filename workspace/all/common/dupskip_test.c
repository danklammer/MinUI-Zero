// dupskip_test.c — Codex P1#2 production-path matrix. Drives the SHIPPING dupskip.c
// (not a mirror model) through every high-risk transition of detection + decision.
#include "dupskip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

// injected allocator that always fails (tests the alloc-failure branch deterministically)
static void* fail_alloc(size_t n) { (void)n; return NULL; }
static void  fail_free(void* p)   { (void)p; }

// A permissive context: every gate allows a skip; individual tests flip one gate.
static DupSkipCtx ctx_permit(int dup) {
	DupSkipCtx c;
	memset(&c, 0, sizeof(c));
	c.enabled = 1; c.dup_frame = dup;
	c.geometry_dirty = 0; c.present_dirty_gen = 0;
	c.fast_forward = 0; c.presentation_drop = 0;
	c.audio_active = 1; c.force_present = 0; c.max_streak = 30;
	return c;
}

// Build a WxH 16bpp frame with `pitch` bytes/row; fill value `v` in visible pixels,
// distinct `pad` in the padding bytes so padding differences are detectable.
static unsigned char* mkframe(unsigned w, unsigned h, size_t pitch, unsigned char v, unsigned char pad) {
	unsigned char* b = malloc(h * pitch);
	memset(b, pad, h * pitch);
	size_t row = (size_t)w * 2;
	for (unsigned y = 0; y < h; y++) memset(b + y * pitch, v, row);
	return b;
}

int main(void) {
	const unsigned W = 160, H = 144; const int BPP = 2;
	const size_t PACK = (size_t)W * BPP;      // 320 packed
	const size_t PAD  = 256 * 2;              // 512 padded (gambatte's 256-wide pitch)

	// 1. First frame + invalid previous snapshot -> presents; identical next -> dup.
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		unsigned char* a = mkframe(W, H, PACK, 0x11, 0x00);
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 0, "first frame presents (invalid snapshot)");
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 1, "identical second frame is a dup");
		dupskip_reset(&s); free(a);
	}

	// 2. Packed vs padded pitch: identical visible pixels are a dup even if padding differs.
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		unsigned char* p1 = mkframe(W, H, PAD, 0x22, 0xAA);   // pad = 0xAA
		unsigned char* p2 = mkframe(W, H, PAD, 0x22, 0x55);   // same visible, pad = 0x55
		CHECK(dupskip_detect(&s, p1, W, H, PAD, BPP) == 0, "padded first frame presents");
		CHECK(dupskip_detect(&s, p2, W, H, PAD, BPP) == 1, "padded frame: padding change ignored, dup");
		dupskip_reset(&s); free(p1); free(p2);
		// packed path
		DupSkip s2; memset(&s2, 0, sizeof(s2));
		unsigned char* k = mkframe(W, H, PACK, 0x33, 0x00);
		CHECK(dupskip_detect(&s2, k, W, H, PACK, BPP) == 0, "packed first frame presents");
		CHECK(dupskip_detect(&s2, k, W, H, PACK, BPP) == 1, "packed identical frame is a dup");
		dupskip_reset(&s2); free(k);
	}

	// 3. Changed visible pixel breaks equality and re-snapshots.
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		unsigned char* a = mkframe(W, H, PACK, 0x44, 0x00);
		unsigned char* b = mkframe(W, H, PACK, 0x44, 0x00); b[0] ^= 0xFF; // one visible pixel differs
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 0, "frame A presents");
		CHECK(dupskip_detect(&s, b, W, H, PACK, BPP) == 0, "changed frame B is not a dup");
		CHECK(dupskip_detect(&s, b, W, H, PACK, BPP) == 1, "B repeated is a dup (re-snapshotted)");
		dupskip_reset(&s); free(a); free(b);
	}

	// 4. Decision gates that must FORCE a present even on a dup frame.
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		DupSkipCtx c = ctx_permit(1);
		CHECK(dupskip_should_skip(&s, &c) == 1, "baseline dup skips");

		DupSkip g; memset(&g, 0, sizeof(g)); DupSkipCtx cg = ctx_permit(1); cg.geometry_dirty = 1;
		CHECK(dupskip_should_skip(&g, &cg) == 0, "geometry dirty presents");

		DupSkip ff; memset(&ff, 0, sizeof(ff)); DupSkipCtx cf = ctx_permit(1); cf.fast_forward = 1;
		CHECK(dupskip_should_skip(&ff, &cf) == 0, "fast-forward presents");

		DupSkip pd; memset(&pd, 0, sizeof(pd)); DupSkipCtx cp = ctx_permit(1); cp.presentation_drop = 1;
		CHECK(dupskip_should_skip(&pd, &cp) == 0, "presentation-drop core presents");

		DupSkip au; memset(&au, 0, sizeof(au)); DupSkipCtx ca = ctx_permit(1); ca.audio_active = 0;
		CHECK(dupskip_should_skip(&au, &ca) == 0, "audio inactive presents (no pacing)");

		DupSkip fp; memset(&fp, 0, sizeof(fp)); DupSkipCtx cfp = ctx_permit(1); cfp.force_present = 1;
		CHECK(dupskip_should_skip(&fp, &cfp) == 0, "forced present (menu/wake) presents");

		DupSkip nd; memset(&nd, 0, sizeof(nd)); DupSkipCtx cnd = ctx_permit(0);
		CHECK(dupskip_should_skip(&nd, &cnd) == 0, "non-dup frame presents");

		DupSkip off; memset(&off, 0, sizeof(off)); DupSkipCtx co = ctx_permit(1); co.enabled = 0;
		CHECK(dupskip_should_skip(&off, &co) == 0, "disabled presents");
	}

	// 5. Frontend dirty generation forces exactly one present, then skips resume.
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		DupSkipCtx c = ctx_permit(1); c.present_dirty_gen = 0;
		CHECK(dupskip_should_skip(&s, &c) == 1, "gen 0 matches clean_gen 0: skips");
		c.present_dirty_gen = 7;   // a settings/aspect/effect change bumped the generation
		CHECK(dupskip_should_skip(&s, &c) == 0, "generation change forces one present");
		CHECK(dupskip_should_skip(&s, &c) == 1, "after resync, dup at same gen skips again");
		dupskip_reset(&s);
	}

	// 6. 30 skipped duplicates then a forced present (bounds telemetry/HUD staleness).
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		DupSkipCtx c = ctx_permit(1);
		int skips = 0;
		for (int i = 0; i < 31; i++) if (dupskip_should_skip(&s, &c)) skips++;
		CHECK(skips == 30, "exactly 30 skips before the forced present");
		CHECK(dupskip_should_skip(&s, &c) == 1, "streak reset after forced present: skips resume");
		dupskip_reset(&s);
	}

	// 7. Allocation failure disables detection gracefully AND recovers next frame.
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		s.mem_alloc = fail_alloc; s.mem_free = fail_free;    // injected: snapshot alloc returns NULL
		unsigned char* a = mkframe(W, H, PACK, 0x66, 0x00);
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 0, "alloc failure: detection disabled, presents");
		CHECK(s.prev_sz == 0, "alloc failure leaves prev_sz cleared for retry");
		s.mem_alloc = NULL; s.mem_free = NULL;               // allocator recovers
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 0, "recovers: normal frame presents");
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 1, "recovers: identical frame is a dup");
		dupskip_reset(&s); free(a);
	}

	// 8. NULL libretro duplicate callback must not poison the snapshot.
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		unsigned char* a = mkframe(W, H, PACK, 0x77, 0x00);
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 0, "frame A snapshotted");
		CHECK(dupskip_detect(&s, NULL, W, H, PACK, BPP) == 0, "NULL frame presents, no dup");
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 1, "A after NULL still a dup (snapshot intact)");
		dupskip_reset(&s); free(a);
	}

	// 9. Malformed geometry fails OPEN (never a false dup) — Codex F3.
	{
		DupSkip s; memset(&s, 0, sizeof(s));
		unsigned char* a = mkframe(W, H, PACK, 0x88, 0x00);
		CHECK(dupskip_detect(&s, a, W, H, PACK, 0)  == 0, "bpp 0 presents");
		CHECK(dupskip_detect(&s, a, W, H, PACK, -2) == 0, "negative bpp presents");
		CHECK(dupskip_detect(&s, a, 0, H, PACK, BPP) == 0, "zero width presents");
		CHECK(dupskip_detect(&s, a, W, 0, PACK, BPP) == 0, "zero height presents");
		// visible row (W*2=320) exceeds a too-small pitch -> present, do NOT compare a truncated row
		CHECK(dupskip_detect(&s, a, W, H, 200, BPP) == 0, "undersized pitch presents (no truncated compare)");
		CHECK(dupskip_detect(&s, a, W, H, 200, BPP) == 0, "undersized pitch stays present (no false dup)");
		// width*bpp overflow -> present (32-bit host: this path; 64-bit: caught by row>pitch)
		CHECK(dupskip_detect(&s, a, 0xFFFFFFFFu, H, PACK, 4) == 0, "width*bpp overflow presents");
		// height*row_bytes overflow: max dims (row=0x1FFFFFFFE fits pitch=SIZE_MAX), height*row > SIZE_MAX
		CHECK(dupskip_detect(&s, a, 0xFFFFFFFFu, 0xFFFFFFFFu, (size_t)-1, BPP) == 0,
		      "height*row_bytes overflow presents");
		// a valid frame after the malformed calls still works (state not corrupted)
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 0, "valid frame after malformed presents");
		CHECK(dupskip_detect(&s, a, W, H, PACK, BPP) == 1, "then dedups normally");
		dupskip_reset(&s); free(a);
	}

	if (fails) { printf("== dupskip: %d FAILURE(S) ==\n", fails); return 1; }
	printf("== dupskip: ALL PASS ==\n");
	return 0;
}
