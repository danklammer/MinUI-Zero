# Closed-loop thermal/perf governor — designed on assumptions, self-correcting on first boot

You do **not** need the device to write this. A closed-loop controller measures temp +
frame timing at runtime and corrects, so assumptions only set the *starting point* and
*guardrails* — not the behavior. Three things make running on assumptions safe, not risky:

1. **Writes snap to the nearest valid OPP** — a wrong frequency value is coerced by the
   kernel, never an error.
2. **The feedback loop self-corrects** bad brackets at runtime: frames slip → climb; slack →
   sink. Assumptions affect convergence speed, not correctness.
3. **A conservative thermal ceiling** means the worst case of a wrong guess is "too cautious"
   (a little perf lost), never "overheats."

## KNOWN vs ASSUMED
**KNOWN (from MinUI's tg5040 source):**
- Governor is `userspace`; speed is set by writing kHz to
  `/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed`. Keep that; drive it dynamically.
- Reference points MinUI uses: **600 / 1200 / 1608 / 2000 MHz**.

**ASSUMED (safe defaults; confirm later, nothing breaks if wrong):**
- OPP ladder ≈ 480/600/720/816/1008/1104/1200/1320/1488/1608/1800/2000 MHz (writes snap to nearest).
- One CPU thermal sensor at `thermal_zone0` (verify `type`).
- Voltage/OPP not user-writable on the stock kernel → no undervolt without a custom kernel.
- cpufreq policy is cluster-wide (cpu0 governs all four cores) — typical for the A133P.

## Tunables — assumptions live here as #defines
```c
#define T_TARGET_C   60      // start shaving clock above this
#define T_CEIL_C     72      // hard back-off above this
#define T_SENSOR     "/sys/class/thermal/thermal_zone0/temp"               // mC
#define FREQ_PATH    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed" // kHz
#define TICK_FRAMES  30      // run controller ~every 30 frames (~0.5s @60)
#define STEP_KHZ     108000  // ~one OPP per move
#define UP_DWELL     1       // ticks of slip before climbing (climb fast)
#define DN_DWELL     4       // ticks of slack before sinking (sink slow = no hunting)

typedef struct { int f_min, f_max; } sys_profile;
static const sys_profile P_8BIT  = {  480000, 1008000 }; // NES/GB/GBC/SMS/GG/PCE
static const sys_profile P_16BIT = {  600000, 1320000 }; // SNES/Genesis/GBA
static const sys_profile P_PS1   = { 1008000, 2000000 };
```

## Primitives
```c
static void gov_set_freq(int khz) {
    FILE* f = fopen(FREQ_PATH, "w");
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
- `cat scaling_available_frequencies` → replace the assumed ladder / `STEP_KHZ`.
- `cat /sys/class/thermal/thermal_zone*/type` → confirm the CPU zone; fix `T_SENSOR`.
- Watch it converge during a heavy game; nudge `T_TARGET_C` / dwell if it hunts or runs warm.

None of these change the structure — just better numbers in the `#define`s.
