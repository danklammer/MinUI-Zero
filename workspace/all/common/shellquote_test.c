// Host test for minui's shellQuote(), the bounded replacement for the old in-place
// escapeSingleQuotes()/replaceString() pair.
//
// The function under test is EXTRACTED FROM minui.c at test time (see run-shellquote-tests.sh),
// not copied here — a copy would silently drift from the shipping code, which is exactly how a
// "tested" quoting bug survives.
//
// What this pins down:
//   * correctness — the quoted form must survive /bin/sh and come back byte-identical,
//     including apostrophes, spaces, parens and shell metacharacters;
//   * the OVERFLOW that motivated the change — the old code grew "'" into "'\''" in place inside
//     a char[256] path buffer and then sprintf'd two of those into another char[256]. Here the
//     pathological inputs must be REFUSED, never written past the end. Run under ASan so a single
//     byte of overrun fails the suite instead of silently passing.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "shellquote_extracted.h"

static int failures = 0;
static int checks = 0;

static void ok(int cond, const char* what) {
	checks++;
	if (!cond) { failures++; printf("  FAIL %s\n", what); }
	else printf("  ok   %s\n", what);
}

// Round-trip through a real shell: quote it, ask sh to echo it back, compare.
static void roundtrip(const char* raw) {
	char q[4096];
	if (!shellQuote(raw, q, sizeof(q))) { failures++; checks++; printf("  FAIL quote refused: %s\n", raw); return; }

	char cmd[8192];
	snprintf(cmd, sizeof(cmd), "printf '%%s' %s", q);
	FILE* p = popen(cmd, "r");
	if (!p) { failures++; checks++; printf("  FAIL popen\n"); return; }
	char back[4096] = {0};
	size_t n = fread(back, 1, sizeof(back)-1, p);
	back[n] = '\0';
	pclose(p);

	checks++;
	if (strcmp(raw, back) != 0) {
		failures++;
		printf("  FAIL roundtrip\n    in : [%s]\n    out: [%s]\n    q  : %s\n", raw, back, q);
	}
	else printf("  ok   roundtrip [%s]\n", raw);
}

int main(void) {
	printf("=== round-trips through /bin/sh ===\n");
	roundtrip("/mnt/SDCARD/Roms/simple.gb");
	roundtrip("/mnt/SDCARD/Roms/6) Sony PlayStation (PS)/Metal Slug X/Metal Slug X (USA).bin");
	// the real file that prompted this: apostrophe AND parens AND spaces
	roundtrip("/mnt/SDCARD/Roms/6) Sony PlayStation (PS)/Tony Hawk's Pro Skater/Tony Hawk's Pro Skater (USA).bin");
	roundtrip("it's a \"test\" of $HOME `backticks` & ; | > <");
	roundtrip("'");
	roundtrip("''''");
	roundtrip("");

	printf("\n=== bounds: must REFUSE, never overrun ===\n");
	char small[8];
	ok(shellQuote("abc", small, sizeof(small)) == 1, "fits exactly");
	ok(strcmp(small, "'abc'") == 0, "fitted value correct");
	ok(shellQuote("abcdefghij", small, sizeof(small)) == 0, "too long -> refused");
	ok(shellQuote("a'b", small, sizeof(small)) == 0, "apostrophe expansion overflows -> refused");
	ok(shellQuote("x", small, 2) == 0, "buffer smaller than minimum -> refused");
	ok(shellQuote("", small, 3) == 1, "empty string in 3 bytes -> ok");

	// The old code's exact failure mode: a 255-char path of apostrophes expands 4x. It must be
	// refused by a buffer that cannot hold it, and accepted by QUOTED_MAX, which is sized for it.
	printf("\n=== the pathological case the old code corrupted the stack on ===\n");
	char worst[256];
	memset(worst, '\'', 255);
	worst[255] = '\0';
	char tight[256];
	ok(shellQuote(worst, tight, sizeof(tight)) == 0, "255 apostrophes into char[256] -> refused");
	char* big = malloc(QUOTED_MAX);
	ok(shellQuote(worst, big, QUOTED_MAX) == 1, "255 apostrophes fit QUOTED_MAX");
	free(big);

	printf("\n=== %d checks, %d failures ===\n", checks, failures);
	return failures ? 1 : 0;
}
