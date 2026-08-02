// telemetry.c — see telemetry.h and docs/benchmark-harness-design.md.
//
// Self-contained: libc + direct sysfs reads (absent off-device -> blank columns). No SDL/api.h,
// so the pure stats compile and run anywhere for testing.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "telemetry.h"

// Generic sysfs paths — the same on every Linux handheld we target. All optional: a missing node
// just leaves its CSV column blank.
#define TLM_TEMP_PATH "/sys/class/thermal/thermal_zone0/temp"                 // milli-C
#define TLM_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq" // kHz

// The battery node is NOT generic — it is named after the PMIC. This used to hardcode
// "axp2202-battery", which is the tg5040 part, sitting in code shared by every platform: on the
// Miyoo (axp223) both columns silently logged blank, so a bench CSV captured there looked like a
// device with no power rail rather than a path that does not exist. Discover it instead — one
// scandir at init, then the same two reads per window as before.
#define TLM_PSY_DIR "/sys/class/power_supply"
static char g_volt_path[128];  // "" until tlm_init finds a node
static char g_curr_path[128];

// ---- pure helpers ----
static int cmp_u32(const void* a, const void* b) {
	uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
	return (x > y) - (x < y);
}
TlmStats tlm_percentiles(const uint32_t* samples_us, int n) {
	TlmStats s = {0, 0, 0, 0, 0};
	if (n <= 0) return s;
	if (n > TLM_WINDOW) n = TLM_WINDOW;
	uint32_t buf[TLM_WINDOW];
	memcpy(buf, samples_us, (size_t)n * sizeof(uint32_t));
	qsort(buf, (size_t)n, sizeof(uint32_t), cmp_u32);
	// nearest-rank percentile: index = ceil(p/100 * n) - 1, clamped
	#define PCT(p) buf[ ((((p)*n + 99) / 100) - 1 < 0) ? 0 : ((((p)*n + 99) / 100) - 1) ]
	s.p50 = (int)PCT(50);
	s.p95 = (int)PCT(95);
	s.p99 = (int)PCT(99);
	#undef PCT
	s.max = (int)buf[n - 1];
	s.n = n;
	return s;
}
double tlm_mj_per_frame(double power_mw, double fps) {
	if (fps <= 0.0) return 0.0;
	return power_mw / fps; // mW / (frames/s) = mJ/frame
}

// ---- runtime state (only used when enabled) ----
static int g_enabled = 0;
static int g_budget_us = 16667;
static FILE* g_csv = NULL;
static uint32_t g_win[TLM_WINDOW];
static int g_win_n = 0;
static int g_frame = 0;       // total frames seen
static long g_over_total = 0; // total over-budget frames
static int g_aud_q = 0;            // latest audio ring fill (frames)
static long g_aud_under = 0, g_aud_over = 0;             // latest cumulative audio counters
static long g_aud_under_base = 0, g_aud_over_base = 0;   // counters at last window flush
static long g_dup_seen = 0, g_dup_dups = 0;             // present-skip: frames seen / duplicates
static long g_dup_seen_base = 0, g_dup_dups_base = 0;   // dup counters at last window flush

static int read_int(const char* path) {
	if (!path || !*path) return -1;
	FILE* f = fopen(path, "r");
	if (!f) return -1;
	int v = -1;
	if (fscanf(f, "%d", &v) != 1) v = -1;
	fclose(f);
	return v;
}

// Find the battery under /sys/class/power_supply by CAPABILITY (it exposes voltage_now), not by
// PMIC name. Skips the mains/USB supplies, which have voltage_now too but are not the battery, by
// requiring type == "Battery".
static void find_battery(void) {
	g_volt_path[0] = g_curr_path[0] = '\0';
	DIR* d = opendir(TLM_PSY_DIR);
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char path[128], type[32] = {0};
		snprintf(path, sizeof(path), TLM_PSY_DIR "/%s/type", e->d_name);
		FILE* f = fopen(path, "r");
		if (!f) continue;
		int got = fscanf(f, "%31s", type);
		fclose(f);
		if (got != 1 || strcmp(type, "Battery") != 0) continue;

		snprintf(path, sizeof(path), TLM_PSY_DIR "/%s/voltage_now", e->d_name);
		if (!(f = fopen(path, "r"))) continue;
		fclose(f);
		snprintf(g_volt_path, sizeof(g_volt_path), TLM_PSY_DIR "/%s/voltage_now", e->d_name);
		snprintf(g_curr_path, sizeof(g_curr_path), TLM_PSY_DIR "/%s/current_now", e->d_name);
		break;
	}
	closedir(d);
}

static void flush_window(void) {
	if (!g_csv) return;
	TlmStats s = tlm_percentiles(g_win, g_win_n);
	int over = 0;
	for (int i = 0; i < g_win_n; i++) if ((int)g_win[i] >= g_budget_us) over++;

	int temp_mc = read_int(TLM_TEMP_PATH);
	int cur_khz = read_int(TLM_FREQ_PATH);
	int volt_uv = read_int(g_volt_path);
	int curr_ua = read_int(g_curr_path);
	// power mW = V(V) * I(A) * 1000 = (uV/1e6)*(uA/1e6)*1000; current_now sign varies by PMIC.
	double power_mw = -1.0;
	if (volt_uv > 0 && curr_ua != -1) {
		long ic = curr_ua < 0 ? -(long)curr_ua : (long)curr_ua;
		power_mw = ((double)volt_uv / 1e6) * ((double)ic / 1e6) * 1000.0;
	}

	// frame, n, p50_us, p95_us, p99_us, max_us, over, temp_c, cur_khz, volt_uv, curr_ua, power_mw
	fprintf(g_csv, "%d,%d,%d,%d,%d,%d,%d,", g_frame, s.n, s.p50, s.p95, s.p99, s.max, over);
	if (temp_mc >= 0) fprintf(g_csv, "%d,", temp_mc / 1000); else fprintf(g_csv, ",");
	if (cur_khz >= 0) fprintf(g_csv, "%d,", cur_khz);        else fprintf(g_csv, ",");
	if (volt_uv >= 0) fprintf(g_csv, "%d,", volt_uv);        else fprintf(g_csv, ",");
	if (curr_ua != -1) fprintf(g_csv, "%d,", curr_ua);       else fprintf(g_csv, ",");
	if (power_mw >= 0) fprintf(g_csv, "%.1f,", power_mw);    else fprintf(g_csv, ",");
	long ud = g_aud_under - g_aud_under_base, od = g_aud_over - g_aud_over_base;
	g_aud_under_base = g_aud_under; g_aud_over_base = g_aud_over;
	long dseen = g_dup_seen - g_dup_seen_base, ddup = g_dup_dups - g_dup_dups_base;
	g_dup_seen_base = g_dup_seen; g_dup_dups_base = g_dup_dups;
	fprintf(g_csv, "%d,%ld,%ld,%ld,%ld\n", g_aud_q, ud < 0 ? 0 : ud, od < 0 ? 0 : od,
	        ddup < 0 ? 0 : ddup, dseen < 0 ? 0 : dseen);
	fflush(g_csv);

	g_over_total += over;
	g_win_n = 0;
}

void tlm_init(const char* tag, int budget_us) {
	const char* bench = getenv("BENCH");
	g_enabled = (bench && bench[0] && bench[0] != '0');
	if (!g_enabled) return;
	if (budget_us > 0) g_budget_us = budget_us;

	const char* out = getenv("BENCH_OUT");
	char path[512];
	if (out && out[0]) snprintf(path, sizeof(path), "%s", out);
	else snprintf(path, sizeof(path), "/tmp/bench-%s.csv", tag && tag[0] ? tag : "run");
	g_csv = fopen(path, "w");
	if (!g_csv) { g_enabled = 0; return; }
	find_battery(); // once per run, not once per window
	fprintf(g_csv, "frame,n,p50_us,p95_us,p99_us,max_us,over,temp_c,cur_khz,volt_uv,curr_ua,power_mw,aud_q,aud_under,aud_over,dup_dups,dup_seen\n");
	g_win_n = 0; g_frame = 0; g_over_total = 0;
	g_aud_q = 0; g_aud_under = g_aud_over = g_aud_under_base = g_aud_over_base = 0;
	g_dup_seen = g_dup_dups = g_dup_seen_base = g_dup_dups_base = 0;
}

int tlm_enabled(void) { return g_enabled; }

void tlm_frame(uint32_t work_us) {
	if (!g_enabled) return;
	if (g_win_n < TLM_WINDOW) g_win[g_win_n++] = work_us;
	g_frame++;
	if (g_frame % TLM_SAMPLE_FRAMES == 0) flush_window();
}

void tlm_audio(int queue_frames, long underruns_total, long overruns_total) {
	if (!g_enabled) return;
	g_aud_q = queue_frames;
	g_aud_under = underruns_total;
	g_aud_over = overruns_total;
}

void tlm_dup(int is_dup) {
	if (!g_enabled) return;
	g_dup_seen++;
	if (is_dup) g_dup_dups++;
}

void tlm_quit(void) {
	if (!g_enabled) return;
	if (g_win_n > 0) flush_window();
	if (g_csv) {
		fprintf(g_csv, "# total_frames=%d over_budget=%ld budget_us=%d\n", g_frame, g_over_total, g_budget_us);
		fclose(g_csv);
		g_csv = NULL;
	}
	g_enabled = 0;
}
