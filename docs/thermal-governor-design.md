# Closed-loop thermal/perf governor — designed on assumptions, self-correcting on first boot

> **STATUS (2026-07-01): shipped + validated on-device.** The hybrid governor (ceiling + `schedutil`)
> runs on a real Brick — **~4–5°C cooler than stock**. The hardware still idles at 408 MHz. Low-end
> systems leave a 1008 MHz stock ceiling so schedutil can service short GLES present bursts; PS1
> rides ~1416–1800.
> On-device lesson **D14 (race-to-idle)**: the ceiling caps spikes but must NOT drive schedutil below the
> clock where it can finish-the-frame-and-idle. The "confirm on-device later" items below are done.

You do **not** need the device to write this. A closed-loop controller measures temp +
frame timing at runtime and corrects, so assumptions only set the *starting point* and
*guardrails* — not the behavior. Three things make running on assumptions safe, not risky:

1. **Writes snap to the nearest valid OPP** — a wrong frequency value is coerced by the
   kernel, never an error.
2. **The feedback loop self-corrects** bad brackets at runtime: frames slip → climb; slack →
   sink. Assumptions affect convergence speed, not correctness.
3. **A conservative thermal ceiling** means the worst case of a wrong guess is "too cautious"
   (a little perf lost), never "overheats."

## Architecture: HYBRID (updated per docs/project-direction.md §1)
The controller no longer pins an exact clock via `userspace`/`scaling_setspeed`. Instead the
frame-aware controller chooses an allowed **ceiling** (`scaling_max_freq`) and the kernel
**`schedutil`** governor picks the instantaneous frequency beneath it. This keeps frame
awareness (which a kernel governor lacks) while letting the kernel drop the clock during
lighter moments — cooler than a hard pin. The control math below is unchanged; only the
*actuated value* changed from "the frequency" to "the ceiling" (`gov_step` → `ceil_khz` →
`PLAT_setCPUMaxFreq`). `boot.sh` selects `schedutil`; every CPU-control point (menu tiers
and the governor) sets the cap, so no governor-mode switching is needed.

## KNOWN vs ASSUMED
**KNOWN (from MinUI's tg5040 source):**
- MinUI historically used `userspace` + `scaling_setspeed`; reference points **600 / 1200 /
  1608 / 2000 MHz**. We now use `schedutil` + `scaling_max_freq`.

**ASSUMED (safe defaults; confirm later, nothing breaks if wrong):**
- `schedutil` is available on the Brick kernel (NextUI uses it — verify
  `scaling_available_governors`). If absent, the cap still bounds the default governor.
- **Stock max ≈ 1.8GHz; 2.0GHz is an OVERCLOCK and is never used** (CLAUDE.md). Cap every
  `f_max` at the verified-stock OPP; query `scaling_available_frequencies` on device.
- OPP ladder ≈ 408/.../1200/1608/1800 MHz (writes snap to nearest; NextUI implies a 408 floor).
- One CPU thermal sensor at `thermal_zone0` (verify `type`).
- cpufreq policy is cluster-wide (cpu0 governs all four cores) — typical for the A133P.

## Tunables — assumptions live here as #defines (see governor.c)
```c
#define T_TARGET_C   60      // start shaving the ceiling above this
#define T_CEIL_C     72      // hard back-off above this
#define T_SENSOR     "/sys/class/thermal/thermal_zone0/temp"                // mC
#define MAX_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq" // kHz ceiling
#define STOCK_MAX_KHZ 1800000 // verified-stock cap; NEVER 2000000 (OC)
#define TICK_FRAMES  30      // run controller ~every 30 frames (~0.5s @60)
#define STEP_KHZ     108000  // ~one OPP per move
#define UP_DWELL     1       // ticks of slip before climbing (climb fast)
#define DN_DWELL     4       // ticks of slack before sinking (sink slow = no hunting)

typedef struct { int f_min, f_max; } sys_profile; // f_max <= STOCK_MAX_KHZ (no OC)
static const sys_profile P_8BIT  = { 1008000, 1008000 }; // NES/GB/GBC/SMS/GG/PCE; schedutil still idles below the ceiling
static const sys_profile P_16BIT = {  600000, 1416000 }; // SNES/Genesis/GBA
static const sys_profile P_PS1   = { 1008000, 1800000 }; // capped at stock, was 2000000
```

## Primitives
> The pseudocode below is the original single-pin spec, kept for the control math. In the
> shipped hybrid (governor.c), the computed value is the **ceiling** and is written via the
> platform primitive `PLAT_setCPUMaxFreq(khz)` → `scaling_max_freq` (not `gov_set_freq`/
> `scaling_setspeed`), with `schedutil` choosing beneath it. `g_cur_khz` ≡ `ceil_khz`.
```c
static void gov_set_freq(int khz) {       // shipped as PLAT_setCPUMaxFreq -> scaling_max_freq
    FILE* f = fopen(MAX_FREQ_PATH, "w");
    if (!f) return;
    fprintf(f, "%d", khz);            // kernel clamps to nearest supported OPP
    fclose(f);
}
static int gov_read_temp_c(void) {
    FILE* f = fopen(T_SENSOR, "r"); if (!f) return -1;
    int mc = -1; if (fscanf(f, "%d", &mc) != 1) mc = -1; fclose(f);
    return mc < 0 ? -1 : mc / 1000;   // mC -> C
}
```

## Control loop
Call `gov_tick()` once per `TICK_FRAMES` from minarch's run loop. `frame_overrun` is true
when the last batch missed the emulator's frame budget — minarch already measures this around
`GFX_flip` / `FRAME_BUDGET`, so reuse that signal.
```c
static int g_cur_khz;                 // init to profile.f_max on game load
static int g_slip_run, g_slack_run;

static void gov_tick(const sys_profile* p, int frame_overrun) {
    int temp = gov_read_temp_c();

    // 1) thermal backstop — always wins
    if (temp >= 0 && temp >= T_CEIL_C) {
        g_cur_khz -= STEP_KHZ;
        if (g_cur_khz < p->f_min) g_cur_khz = p->f_min;
        gov_set_freq(g_cur_khz);
        g_slip_run = g_slack_run = 0;
        return;
    }
    if (frame_overrun) {              // 2) need more performance
        g_slip_run++; g_slack_run = 0;
        if (g_slip_run >= UP_DWELL && g_cur_khz < p->f_max) {
            g_cur_khz += STEP_KHZ;
            if (g_cur_khz > p->f_max) g_cur_khz = p->f_max;
            gov_set_freq(g_cur_khz);
            g_slip_run = 0;
        }
    } else {                          // 3) have slack — probe downward (the cold win)
        g_slack_run++; g_slip_run = 0;
        int cool_enough = (temp < 0) || (temp <= T_TARGET_C);
        if (g_slack_run >= DN_DWELL && cool_enough && g_cur_khz > p->f_min) {
            g_cur_khz -= STEP_KHZ;
            if (g_cur_khz < p->f_min) g_cur_khz = p->f_min;
            gov_set_freq(g_cur_khz);
            g_slack_run = 0;
        }
    }
}
```

## Wiring into MinUI
- **On game load:** pick the profile (extend the per-pak `launch.sh` to export `f_min`/`f_max`),
  set `g_cur_khz = p->f_max`, write once.
- **In the run loop:** every `TICK_FRAMES`, derive `frame_overrun` from minarch's pacing
  measurement and call `gov_tick()`.
- **On menu / pause / sleep:** drop to the menu/idle clock — MinUI already does this via
  `CPU_SPEED_MENU`, keep it.

## Confirm on-device later (~10 min, not a blocker)
- `cat scaling_available_governors` → confirm **`schedutil`** exists (else the cap still bounds
  the default governor; behavior degrades but stays safe).
- `cat scaling_available_frequencies` / `cpuinfo_max_freq` → confirm the **verified-stock max**
  (replace the assumed 1.8GHz `STOCK_MAX_KHZ`; **never** cap at 2.0GHz) and `STEP_KHZ`.
- `cat /sys/class/thermal/thermal_zone*/type` → confirm the CPU zone; fix `T_SENSOR`.
- Watch it converge during a heavy game; confirm `schedutil` drops the clock in light scenes;
  nudge `T_TARGET_C` / dwell if it hunts or runs warm.
- Confirm the menu/launcher cap behaves (a low `scaling_max_freq` keeps the menu cool).

None of these change the structure — just better numbers in the `#define`s.
