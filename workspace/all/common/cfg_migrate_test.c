// Saved-cfg migration tests. See run-cfg-migrate-tests.sh — the code under test is extracted
// from minarch.c, never copied.
//
// What this protects: a saved cfg carries EVERY option (Config_write dumps the whole list on any
// change), so a stale value can outlive the shipped default that replaced it. The migration drops
// the keys in cfg_stale_keys from a save that predates them, exactly once. Ways that go wrong, all
// silent on device:
//   - the version never parses, so every save migrates forever and a deliberate choice never
//     sticks;
//   - the version wrongly parses as current, so no save migrates and the shimmer stays;
//   - the drop fires where no shipped default changed, silently resetting a deliberate choice.
//     This one is not hypothetical: tg5040's system.cfg sets minarch_screen_scaling = Aspect
//     GLOBALLY, so a naive "was this key shipped at all" gate resets every old Brick save.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg_migrate_extracted.h"

static int failures = 0;
static void ck(int cond, const char* what) {
	printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
	if (!cond) failures++;
}

// How Config_readOptions derives the version from a saved cfg. Same shape as the shipping code.
static int version_of(char* cfg) {
	char value[256];
	if (!cfg) return 0;
	if (!Config_getValue(cfg, CFG_VERSION_KEY, value, NULL)) return 0;
	return Config_parseVersion(value);
}

// Drive the real decision by staging what the shipped chain said, exactly as the option loops do.
static int drops(const char* key, const char* shipped, int user_version) {
	int i = Config_staleIndex(key);
	if (i < 0 || i >= CFG_STALE_MAX) return 0;
	memset(cfg_stale_shipped, 0, sizeof(cfg_stale_shipped));
	if (shipped) snprintf(cfg_stale_shipped[i], sizeof(cfg_stale_shipped[i]), "%s", shipped);
	return Config_shouldDropStale(i, user_version);
}

int main(void) {
	printf("cfg migration:\n");

	// ---- the stale-key table ----
	ck(Config_isStaleKey("minarch_screen_scaling") == 1, "scaling is a stale key (v1)");
	ck(Config_isStaleKey("minarch_screen_sharpness") == 0, "sharpness is NOT stale");
	ck(Config_isStaleKey("minarch_threading") == 0, "threading is NOT stale");
	ck(Config_isStaleKey("") == 0, "empty key is not stale");
	ck(Config_isStaleKey("minarch_screen") == 0, "prefix of a stale key does not match");
	ck(Config_isStaleKey("minarch_screen_scaling_x") == 0, "superstring of a stale key does not match");

	// ---- version parsing ----
	char no_stamp[] = "minarch_screen_scaling = Aspect\nminarch_screen_sharpness = Sharp\n";
	ck(version_of(no_stamp) == 0, "unstamped save reads as version 0");

	char stamped[] = "minarch_cfg_version = 1\nminarch_screen_scaling = Aspect\n";
	ck(version_of(stamped) == 1, "stamped save reads its version");

	ck(version_of(NULL) == 0, "absent save reads as version 0");

	// A partial parse must NOT be trusted. strtol("1garbage") is 1, which would claim current and
	// silently skip the migration forever — the failure direction we cannot detect on device.
	ck(Config_parseVersion("1garbage") == 0, "trailing garbage is rejected, not read as 1");
	ck(Config_parseVersion("") == 0, "empty version is rejected");
	ck(Config_parseVersion(" 1") == 0, "leading space is rejected");
	ck(Config_parseVersion("-1") == 0, "negative version is rejected");
	ck(Config_parseVersion("banana") == 0, "non-numeric version is rejected");
	ck(Config_parseVersion("99999999999999999999") == 0, "absurd version is rejected, not wrapped");
	ck(Config_parseVersion("1") == 1, "a clean version still parses");
	ck(Config_parseVersion("2") == 2, "a future version still parses");

	// ---- the shipped-value gate: the Brick regression this exists to prevent ----
	// tg5040 system.cfg supplies `minarch_screen_scaling = Aspect` for EVERY core. A gate that
	// only asked "was it shipped?" would fire on every Brick launch and reset deliberate choices.
	ck(drops("minarch_screen_scaling", "Aspect", 0) == 0,
	   "BRICK: shipped Aspect != Native -> stale save is NOT touched");
	ck(drops("minarch_screen_scaling", "Fullscreen", 0) == 0,
	   "shipped some other value -> not touched");
	ck(drops("minarch_screen_scaling", "Native", 0) == 1,
	   "MMP GB/GBC/FC: shipped Native -> stale save DROPS");
	ck(drops("minarch_screen_scaling", NULL, 0) == 0,
	   "MMP GBA/SUPA: nothing shipped -> stale save is KEPT");
	ck(drops("minarch_screen_scaling", "Native", 1) == 0,
	   "a v1 save is current for a v1 entry -> kept even where Native is shipped");
	ck(drops("minarch_cpu_speed", "Normal", 0) == 0,
	   "a non-stale key is never dropped");

	// ---- per-entry versioning ----
	// The v1 entry must stop firing once a save is stamped v1, EVEN after CFG_VERSION moves on.
	// A single global `user_version < CFG_VERSION` would re-drop scaling on a valid v1 save the
	// moment a v2 key is added, destroying a choice made deliberately after v1 already ran.
	{
		int i = Config_staleIndex("minarch_screen_scaling");
		ck(i >= 0 && cfg_stale_keys[i].version == 1, "the scaling entry declares version 1");
		ck(i >= 0 && !strcmp(cfg_stale_keys[i].to, "Native"), "the scaling entry declares to=Native");
		ck(drops("minarch_screen_scaling", "Native", 1) == 0,
		   "v1 save vs v1 entry: no drop, regardless of what CFG_VERSION becomes");
		ck(drops("minarch_screen_scaling", "Native", 2) == 0,
		   "v2 save vs v1 entry: no drop");
	}

	// ---- the parser: no other key may satisfy the version lookup ----
	// Config_getValue is strstr-based, so test a key that CONTAINS the version key, not just
	// unrelated ones.
	char decoys[] =
		"minarch_screen_scaling = Aspect\n"
		"minarch_cpu_speed = Normal\n"
		"minarch_thread_video = On\n";
	ck(version_of(decoys) == 0, "no unrelated key satisfies the version lookup");

	char only_stamp[] = "minarch_cfg_version = 1\n";
	char value[256];
	ck(Config_getValue(only_stamp, "minarch_screen_scaling", value, NULL) == 0,
	   "the stamp alone yields no scaling value");

	// ---- end-to-end: the case that actually shipped broken ----
	char gbc_card[] =
		"minarch_screen_scaling = Aspect\n"
		"minarch_screen_sharpness = Sharp\n"
		"minarch_cpu_speed = Normal\n"
		"bind A = A\n";
	int v = version_of(gbc_card);
	ck(drops("minarch_screen_scaling", "Native", v),
	   "stale GBC card on MMP: scaling dropped, pak default Native wins");
	ck(!drops("minarch_screen_scaling", "Aspect", v),
	   "same card on the BRICK: scaling untouched (shipped is Aspect)");
	ck(Config_getValue(gbc_card, "minarch_screen_sharpness", value, NULL) && !strcmp(value, "Sharp"),
	   "stale card: sharpness still parses and is kept");

	char gbc_resaved[] = "minarch_cfg_version = 1\nminarch_screen_scaling = Aspect\n";
	ck(!drops("minarch_screen_scaling", "Native", version_of(gbc_resaved)),
	   "re-saved card: a deliberate Aspect choice now sticks");

	printf("\n=== cfg migration: %s ===\n", failures ? "FAILURES" : "ALL PASS");
	return failures ? 1 : 0;
}
