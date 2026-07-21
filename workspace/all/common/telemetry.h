// telemetry.h — benchmark/telemetry harness for minarch. See docs/benchmark-harness-design.md.
//
// Captures per-frame work time + periodic thermal/freq/battery samples to a CSV, so changes
// can be compared on real metrics (frame-time percentiles, thermals, mJ/frame). OFF unless the
// BENCH env var is set — the hot-path cost is then a single branch + array store, no thread,
// no allocation. The pure stats helpers below are I/O-free and unit-tested (telemetry_test.c).

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

// Up to this many frames per ~1s sample window (60fps -> ~60; headroom for fast-forward).
#define TLM_WINDOW 512
// Flush a CSV row (window percentiles + one sysfs sample) every this many frames.
#define TLM_SAMPLE_FRAMES 60

// Frame-work percentiles over a window, microseconds.
typedef struct {
	int p50, p95, p99, max, n;
} TlmStats;

// ---- pure helpers (no I/O) ----
// Percentiles over n frame-work samples (us). Copies+sorts internally; n is clamped to TLM_WINDOW.
TlmStats tlm_percentiles(const uint32_t* samples_us, int n);
// Energy per frame in millijoules from mean power (mW) and frame rate (fps). 0 if fps<=0.
double tlm_mj_per_frame(double power_mw, double fps);

// ---- lifecycle (all no-ops unless BENCH is set) ----
void tlm_init(const char* tag, int budget_us); // budget_us = 1e6/fps (over-budget detector)
int  tlm_enabled(void);                        // cheap gate for the run loop
void tlm_frame(uint32_t work_us);              // record one gameplay frame's work time
void tlm_audio(int queue_frames, long underruns_total, long overruns_total); // latest audio-health snapshot
void tlm_dup(int is_dup);                      // present-skip duplicate seen (1) or changed frame (0)
void tlm_quit(void);                           // flush remaining window + close the CSV

#endif // TELEMETRY_H
