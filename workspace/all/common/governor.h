// governor.h — closed-loop thermal/perf CPU governor for minarch.
//
// Runs each system at the lowest CPU clock that still holds frame rate, capped by a
// conservative thermal ceiling. See docs/thermal-governor-design.md for the full
// design and the KNOWN-vs-ASSUMED split. The controller is intentionally tiny and
// self-correcting: assumptions only set the starting point and guardrails, not the
// behavior.
//
// Two layers:
//   gov_step() — the pure controller. No I/O. Fully unit-testable with scripted
//                temp + frame-overrun traces (see governor_test.c).
//   gov_tick() — the device entry point. Reads temp from the thermal zone, runs
//                gov_step(), and writes the result via PLAT_setCPUFreq().

#ifndef GOVERNOR_H
#define GOVERNOR_H

// Run the controller once per this many frames (~0.5s @ 60fps). minarch counts frames.
#define GOV_TICK_FRAMES 30

// Per-system clock bracket, in kHz. The controller keeps the clock within [f_min,f_max].
typedef struct {
	int f_min;
	int f_max;
} GovProfile;

// Controller state for one game session. Reset via gov_init().
typedef struct {
	int cur_khz;   // current commanded clock (kHz)
	int slip_run;  // consecutive ticks of frame overrun (slip)
	int slack_run; // consecutive ticks of frame slack
} GovState;

// Named brackets from docs/thermal-governor-design.md (ASSUMED — verify OPP ladder on
// device, nothing breaks if wrong: writes snap to the nearest OPP and the loop self-corrects).
extern const GovProfile GOV_P_8BIT;  // NES/GB/GBC/SMS/GG/PCE/NGP/PKM
extern const GovProfile GOV_P_16BIT; // SNES/Genesis/GBA/VB
extern const GovProfile GOV_P_PS1;   // PlayStation
// Safe default for an unconfigured system: max guarantees frame rate, governor sinks from there.
extern const GovProfile GOV_P_DEFAULT;

// Initialize state for a new game session: cur_khz = p->f_max, counters zeroed.
// Does not write hardware (caller writes the initial clock, e.g. via PLAT_setCPUFreq).
void gov_init(GovState* st, const GovProfile* p);

// Pure controller step. No I/O.
//   temp_c        : current temperature in Celsius, or <0 if unknown/unavailable.
//   frame_overrun : 1 if the last batch of frames missed the frame budget.
// Returns the next commanded clock (kHz) and updates *st. Result is clamped to
// [p->f_min, p->f_max]; the thermal ceiling always wins.
int gov_step(GovState* st, const GovProfile* p, int temp_c, int frame_overrun);

// Device entry point: read temp from the thermal sensor, run gov_step(), and write the
// result via PLAT_setCPUFreq(). Honors GOV_DISABLE=1 (no-op). Call once per GOV_TICK_FRAMES.
void gov_tick(GovState* st, const GovProfile* p, int frame_overrun);

// Read the CPU thermal zone in Celsius, or -1 if unavailable. Exposed for logging/tests.
int gov_read_temp_c(void);

#endif // GOVERNOR_H
