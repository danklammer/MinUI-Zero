// Saved-cfg migration tests. See run-cfg-migrate-tests.sh — the code under test is extracted
// from minarch.c, never copied.
//
// What this protects: a saved cfg carries EVERY option (Config_write dumps the whole list on any
// change), so a stale value can outlive the shipped default that replaced it. The migration drops
// the keys in cfg_stale_keys from any save written before CFG_VERSION, exactly once. Two ways that
// can go wrong and both are silent on device:
//   - the version key never parses, so every save migrates forever and a deliberate choice never
//     sticks;
//   - the version key parses when it should not, so no save ever migrates and the shimmer stays.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg_migrate_extracted.h"

static int failures = 0;
static void ck(int cond, const char* what) {
	printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
	if (!cond) failures++;
}

// The predicate Config_readOptionsString computes. Mirrored here (it is one expression inside a
// function too entangled with minarch globals to extract) — kept trivial on purpose.
static int migrates(int user_version) { return user_version < CFG_VERSION; }

// How Config_readOptions derives the version from a saved cfg. Same shape as the shipping code.
static int version_of(char* cfg) {
	char value[256];
	if (!cfg) return 0;
	if (!Config_getValue(cfg, CFG_VERSION_KEY, value, NULL)) return 0;
	return (int)strtol(value, NULL, 10);
}

int main(void) {
	printf("cfg migration:\n");

	// ---- the stale-key table ----
	ck(Config_isStaleKey("minarch_screen_scaling") == 1, "scaling is a stale key (v1)");
	ck(Config_isStaleKey("minarch_screen_sharpness") == 0, "sharpness is NOT stale");
	ck(Config_isStaleKey("minarch_screen_effect") == 0, "effect is NOT stale");
	ck(Config_isStaleKey("minarch_threading") == 0, "threading is NOT stale");
	ck(Config_isStaleKey("") == 0, "empty key is not stale");
	// A prefix of a stale key must not match — the table is exact-compare, not substring.
	ck(Config_isStaleKey("minarch_screen") == 0, "prefix of a stale key does not match");
	ck(Config_isStaleKey("minarch_screen_scaling_x") == 0, "superstring of a stale key does not match");

	// ---- version parsing ----
	char no_stamp[] =
		"minarch_screen_scaling = Aspect\n"
		"minarch_screen_sharpness = Sharp\n";
	ck(version_of(no_stamp) == 0, "unstamped save reads as version 0");
	ck(migrates(version_of(no_stamp)) == 1, "unstamped save MIGRATES");

	char stamped[] =
		"minarch_cfg_version = 1\n"
		"minarch_screen_scaling = Aspect\n";
	ck(version_of(stamped) == 1, "stamped save reads its version");
	ck(migrates(version_of(stamped)) == 0, "stamped save does NOT migrate");

	// A save from a FUTURE build (user downgraded) must not be re-migrated.
	char future[] = "minarch_cfg_version = 99\nminarch_screen_scaling = Aspect\n";
	ck(migrates(version_of(future)) == 0, "future-version save does NOT migrate");

	// The stamp is written first; a save truncated before it must look unstamped, not current.
	char truncated[] = "minarch_cfg_ver";
	ck(version_of(truncated) == 0, "truncated stamp reads as version 0 (migrates)");

	// A garbage value must not read as current.
	char garbage[] = "minarch_cfg_version = banana\nminarch_screen_scaling = Aspect\n";
	ck(migrates(version_of(garbage)) == 1, "non-numeric version migrates rather than passing");

	ck(version_of(NULL) == 0, "absent save reads as version 0");

	// ---- the parser itself: no other key may satisfy the version lookup ----
	// Config_getValue is strstr-based, so a key that merely CONTAINS the version key, or is
	// contained BY it, must not be mistaken for it.
	char decoys[] =
		"minarch_screen_scaling = Aspect\n"
		"minarch_cpu_speed = Normal\n"
		"minarch_threading = Auto\n"
		"minarch_thread_video = On\n";
	ck(version_of(decoys) == 0, "no unrelated key satisfies the version lookup");

	// And the version stamp must not be mistaken for a real option by the option loop.
	char only_stamp[] = "minarch_cfg_version = 1\n";
	char value[256];
	ck(Config_getValue(only_stamp, "minarch_screen_scaling", value, NULL) == 0,
	   "the stamp alone yields no scaling value");
	ck(Config_getValue(only_stamp, "minarch_cpu_speed", value, NULL) == 0,
	   "the stamp alone yields no cpu_speed value");

	// ---- end-to-end shape: the case that actually shipped broken ----
	// A GBC card written before the pak default became Native.
	char gbc_card[] =
		"minarch_screen_scaling = Aspect\n"
		"minarch_screen_sharpness = Sharp\n"
		"minarch_cpu_speed = Normal\n"
		"bind A = A\n";
	int v = version_of(gbc_card);
	ck(migrates(v) && Config_isStaleKey("minarch_screen_scaling"),
	   "stale GBC card: scaling is dropped, default Native wins");
	ck(migrates(v) && !Config_isStaleKey("minarch_cpu_speed"),
	   "stale GBC card: cpu_speed is KEPT (not in the table)");
	ck(Config_getValue(gbc_card, "minarch_screen_sharpness", value, NULL) && !strcmp(value, "Sharp"),
	   "stale GBC card: sharpness still parses and is kept");

	// Same card after any settings change — now stamped, so the choice sticks.
	char gbc_resaved[] =
		"minarch_cfg_version = 1\n"
		"minarch_screen_scaling = Aspect\n";
	ck(!migrates(version_of(gbc_resaved)),
	   "re-saved card: a deliberate Aspect choice now sticks");

	printf("\n=== cfg migration: %s ===\n", failures ? "FAILURES" : "ALL PASS");
	return failures ? 1 : 0;
}
