// telemetry_test.c — standalone unit test for the benchmark telemetry math + CSV lifecycle.
// No hardware. Build+run: see workspace/all/common/run-telemetry-tests.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "telemetry.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { \
	if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n        (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_percentiles(void) {
	printf("[percentiles] nearest-rank over a known distribution\n");
	uint32_t a[100];
	for (int i = 0; i < 100; i++) a[i] = (uint32_t)(100 - i); // unsorted (100..1) on purpose
	TlmStats s = tlm_percentiles(a, 100);
	CHECK(s.n == 100, "n=%d", s.n);
	CHECK(s.p50 == 50, "p50=%d (want 50)", s.p50);
	CHECK(s.p95 == 95, "p95=%d (want 95)", s.p95);
	CHECK(s.p99 == 99, "p99=%d (want 99)", s.p99);
	CHECK(s.max == 100, "max=%d (want 100)", s.max);

	TlmStats z = tlm_percentiles(a, 0);
	CHECK(z.n == 0 && z.p50 == 0 && z.max == 0, "empty -> zeros");

	uint32_t one = 4242;
	TlmStats o = tlm_percentiles(&one, 1);
	CHECK(o.p50 == 4242 && o.p99 == 4242 && o.max == 4242, "single sample -> itself");
}

static void test_energy(void) {
	printf("[energy] mJ/frame = power_mW / fps\n");
	CHECK(tlm_mj_per_frame(1800.0, 60.0) == 30.0, "1800mW@60fps -> 30 mJ/frame, got %f", tlm_mj_per_frame(1800.0, 60.0));
	CHECK(tlm_mj_per_frame(1000.0, 0.0) == 0.0, "fps=0 guarded -> 0");
}

static void test_lifecycle(void) {
	printf("[lifecycle] BENCH gating + CSV emission\n");
	// disabled by default
	unsetenv("BENCH");
	tlm_init("off", 16667);
	CHECK(!tlm_enabled(), "no BENCH -> disabled");
	tlm_frame(5000); tlm_quit(); // must be safe no-ops

	// enabled -> writes a CSV with a header and >=1 data row
	const char* out = "/tmp/bench-unit.csv";
	setenv("BENCH", "1", 1);
	setenv("BENCH_OUT", out, 1);
	tlm_init("unit", 16667);
	CHECK(tlm_enabled(), "BENCH=1 -> enabled");
	// 2 full windows of work times, mix under/over the 16667us budget
	for (int i = 0; i < TLM_SAMPLE_FRAMES * 2; i++) tlm_frame(i % 2 ? 8000 : 20000);
	tlm_quit();

	FILE* f = fopen(out, "r");
	CHECK(f != NULL, "csv created at %s", out);
	if (f) {
		char line[256];
		int has_header = 0, data_rows = 0, has_summary = 0;
		while (fgets(line, sizeof(line), f)) {
			if (strstr(line, "frame,n,p50_us")) has_header = 1;
			else if (line[0] == '#') has_summary = 1;
			else if (line[0] >= '0' && line[0] <= '9') data_rows++;
		}
		fclose(f);
		CHECK(has_header, "csv has header");
		CHECK(data_rows >= 2, "csv has >=2 sample rows, got %d", data_rows);
		CHECK(has_summary, "csv has summary line");
	}
}

int main(void) {
	printf("== telemetry unit tests ==\n");
	test_percentiles();
	test_energy();
	test_lifecycle();
	printf("== %s ==\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
	return g_fail ? 1 : 0;
}
