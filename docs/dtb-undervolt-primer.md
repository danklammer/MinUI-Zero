# DTB undervolt primer — the last big lever, and why it's not as scary as it sounds

> **HISTORICAL (2026-07-11):** runtime undervolting shipped via direct PMIC i2c instead of a
> DTB (see the Optimize CPU tool + platform.c uv_*). Voltage figures below came from the vendor
> DTB as read at writing time and differ from the measured per-OPP stock values in
> docs/bench/receipts/uv-calibration/margins.log — trust the receipts. A DTB approach remains
> unimplemented research.

*Status: educational / research. Nothing here is implemented. Facts are marked
**[KNOWN]** (verified), **[LIKELY]** (standard Allwinner behavior, verify on-device), or
**[PROBE]** (must be measured before any real work).*

## 1. What it is, in one paragraph

Every CPU clock step (OPP — "operating performance point") is a *pair*: a frequency **and a
voltage**. The kernel's cpufreq driver looks these pairs up in a table that ships inside the
**DTB** (Device Tree Blob — a data file describing the hardware, loaded at boot alongside the
kernel). Chip vendors set those voltages with generous safety margin so that *every* unit of
*every* batch is stable — including the worst silicon on the hottest day. Undervolting means
rebuilding the DTB with slightly lower voltages per OPP, reclaiming margin your particular chip
doesn't need.

## 2. Why it's the biggest lever left

Dynamic power scales with **voltage squared**: `P ≈ C·V²·f`. Drop a rail from 0.90 V to
0.85 V (−5.6%) and that OPP's dynamic power falls ~11%. It stacks with *everything* we've
already done, because it cuts power at **every clock on every rail the table covers** — the
governor picks the clock, the DTB decides what that clock costs. This is why it's the endgame:
all our other work optimizes *which* OPP we sit at; this makes every OPP cheaper.

Realistic expectation: **5–12% total-device power** in CPU-bound play (best on PS1 at high
OPPs, least at the 408 floor where voltage is already lowest). Real, not revolutionary.

## 3. The risk model — what can and cannot happen

Undervolting is not expected to physically damage silicon, but instability can crash the
system or corrupt data being written. That data-loss risk is why calibration requires a
watchdog, guarded output, runtime readback, and save backups.

| Failure | Can it happen? | Consequence | Recovery |
|---|---|---|---|
| Crash / freeze during play | Yes — that's how you find the limit | Reboot | Automatic (and our crash-safe saves + targeted fsync limit the blast radius) |
| Fails to boot with the modified DTB | Yes, if too aggressive | Device won't start **with that DTB** | Restore the original DTB / boot media (see §5) |
| SD-card data corruption from a crash mid-write | Rare | A damaged file | fsync discipline + FAT repair; saves are small and synced |
| Permanent hardware damage | Not expected from undervolting | — | Restore stock settings |
| Unbootable configuration | Possible for a future DTB implementation | Device-specific recovery | Prove SD/FEL recovery first (§5) |

The real risk isn't the device — it's **shipping someone else's silicon a voltage their chip
can't do**. Chips vary ("silicon lottery"); a margin that's comfortable on your Brick may be
marginal on another. That's a *policy* problem (§7), not a safety one.

## 4. The Brick specifics

- **[KNOWN]** SoC = Allwinner A133P, CPU rail driven by the AXP2202 PMIC (a DCDC regulator).
  Runtime regulator writes are **not exposed** (probed earlier — read-only sysfs), which is
  exactly why the DTB is the only path to different voltages.
- **[KNOWN]** OPP ladder: 408 / 600 / 816 / 1008 / 1200 / 1416 / 1608 / 1800 (+2000 OC, unused).
- **[PROBE]** The actual voltage table: readable on-device from
  `/sys/kernel/debug/opp/` and/or by decompiling the live DTB
  (`dtc -I fs /proc/device-tree` or from `/sys/firmware/fdt`). First concrete step; zero risk.
- **[PROBE]** Where the DTB lives in the boot chain. Allwinner boot: **BROM (mask ROM) →
  boot0/SPL → U-Boot → kernel + DTB**, packaged on the boot medium. The Brick runs its stock OS
  from internal storage and MinUI piggybacks from SD — we must confirm whether the kernel+DTB
  the device actually boots comes from internal flash or can come from SD.
- **[LIKELY]** Allwinner BROM tries **SD card first, then internal flash**. If true for the
  Brick, that's the golden safety property: a complete boot package (boot0 + U-Boot + kernel +
  modified DTB) on a **test SD card** boots without ever writing internal storage — and
  *removing the card* should restore stock. This must be proven on the target before use.

## 5. Why Allwinner devices are near-unbrickable: FEL

Every Allwinner chip has **FEL mode** burned into mask ROM — a USB recovery protocol that runs
*before any storage is touched*. If nothing bootable is found (or a recovery button combo is
held), the chip enumerates over USB and tools like `sunxi-fel` can load/flash anything. Mask
ROM cannot be corrupted by software. So even the worst case — bad DTB flashed to internal
storage, nothing boots — is recoverable with a USB cable. **[PROBE]** the Brick's FEL entry
(usually: hold a button / short a test point while plugging USB) before any internal write is
ever contemplated. Rule regardless: **we never modify internal storage** — SD-boot only (§4).

## 6. The methodology (when we do it)

1. **Recon (zero risk):** dump the live DTB + OPP voltages + regulator constraints. Extend
   `tools/brick-recon.sh`. Now we know the stock table and the PMIC's actual step size.
2. **Boot-chain proof (zero risk to stock):** build a boot SD with the *unmodified* DTB and
   confirm the device boots from it. Card out = stock back. Until this works, nothing else
   proceeds.
3. **Margin search (test card, our device):** lower all OPP voltages by one PMIC step
   (~25 mV, **[PROBE]** actual granularity), then stress: BENCH runs + the sleep-soak + a
	   high-load loop (PS1 + supafaust) across temperatures. Repeat stepwise until first
   instability; back off **two full steps** from the failure point. Per-OPP refinement after
   (low OPPs usually have the least margin to give).
4. **Long soak:** days of normal use on the candidate table. Any anomaly = back off further.
5. **Measure the actual win** (charge_counter A/Bs, per system). If it's <5% total-device,
   we shelve it with a number, same as GPU-dark games.

## 7. Ship policy (the part that protects other people's devices)

- **Never default-on.** Silicon lottery means our margin ≠ everyone's margin.
- Ship as an **opt-in Tool** ("Undervolt.pak"): explains itself (like the Deep Sleep tool),
  applies the alternate boot configuration, and *requires* passing an on-device self-test
  (stress loop) before it sticks. One failed boot or self-test → automatic revert to stock.
- The conservative shipped offset would be well inside the margin we validated (e.g., if our
  Brick is stable at −75 mV, ship −25/−50 mV), and the tool's revert path is the SD boot
  property from §4: worst case, stock boots untouched.

## 8. Bottom line

- The shipped runtime tuning is RAM-only and returns to kernel control on reboot; crashes
  can still corrupt in-flight data, so backups and guarded publication remain required.
- A future DTB path requires a demonstrated SD/FEL recovery procedure before any internal
  storage change; recovery must not be assumed from generic Allwinner behavior.
- The engineering cost is real (boot-chain work, long validation), the payoff is a genuine
  but single-digit-percent power win, and the shipping risk is managed by opt-in + self-test
  + auto-revert.
- First actionable step whenever we feel like it: **the zero-risk recon dump** (§6.1) — an
  afternoon, no writes, and it converts most of the [PROBE]s above into [KNOWN]s.

---

## P1 recon results — Brick, measured 2026-07-04 (all former LIKELYs now KNOWN)

- **CPU rail: `tcs4838-dcdc0` (external buck, i2c `6-0041`)** — sole consumer is `cpu0`
  (confirmed via `/sys/kernel/debug/regulator/regulator_summary`). Hardware window
  712.5-1500 mV.
- **Measured OPP-to-voltage ladder** (pinned each OPP, read the rail live):
  408/600/816/1008 MHz = **900.0 mV flat**; 1200 = 937.5; 1416 = 1025.0; 1608 = 1100.0;
  1800 = **1187.5 mV**.
- **Persist target:** `opp-microvolt-c0` property in `/proc/device-tree/opp_l_table/opp@*`
  (speed-bin suffixed, sunxi style). debugfs `opp_summary` is empty on this kernel.
- **Watchdog:** `/dev/watchdog` present -> the P2 margin harness can be crash-safe and
  unattended.
- **P2 design (agreed):** self-resuming harness — state file on SD + `auto.sh` resume after
  each watchdog reboot; pin OPP -> step the TCS4838 down via i2c -> stress -> log survivor;
  the crash boundary per OPP IS the data. Ship form = the same harness as an opt-in
  self-characterization tool (margins are per-chip; never ship one chip's numbers).
- Per-device: the Smart Pro needs its own P1 pass (same probes) + its own margin table.

## P2 COMPLETE — the Brick's measured margin map (2026-07-04, 06:28-07:59, fully unattended)

The self-resuming harness (tools/undervolt/: uvtool + stress + uvmap.sh) mapped all 8 OPPs
in 91 minutes with 4 deliberate crash-reboots, zero human intervention, nothing persisted:

| OPP (MHz) | Stock (mV) | Cliff (mV) | Margin | Guardbanded ship (+50) | CPU power cut (V^2) |
|---|---|---|---|---|---|
| 1800 | 1187.5 | 1012.5 | 175 | 1062.5 | -19.9% |
| 1608 | 1100.0 | 950.0 | 150 | 1000.0 | -17.4% |
| 1416 | 1025.0 | 875.0 | 150 | 925.0 | -18.6% |
| 1200 | 937.5 | 800.0 | 137.5 | 850.0 | -17.8% |
| 1008 | 900.0 | none at 762.5 floor | >=137.5 | 812.5 | -18.5% |
| 816 | 900.0 | none at floor | >=137.5 | 812.5 | -18.5% |
| 600 | 900.0 | none at floor | >=137.5 | 812.5 | -18.5% |
| 408 | 900.0 | none at floor | >=137.5 | 812.5 | -18.5% |

Historical headline: **the entire 408-1008 MHz range survived this harness floor.** Later
light-load tests showed that stress stability does not prove idle stability, so production
stands down at or below 816 MHz and caps each generated row at its measured stock voltage.
Notes: margins are THIS chip's (silicon lottery — the tool must run per
device); the ~14-min dark windows during floor-runs are stress starving sshd (expected);
campaign self-disarmed and the device returned to full stock.

Next (P3, the careful phase): persist via `opp-microvolt-c0` in the DTB. Precursor recon:
find where the DTB lives in the boot chain (boot partition layout) + how to flash it +
FEL rescue drill BEFORE any write. A runtime voltage daemon was considered and REJECTED:
racing schedutil transitions can apply a low-OPP voltage to a high OPP (below-cliff = crash);
only the kernel can sequence voltage/frequency atomically, hence DTB.
