# DTB undervolt primer — the last big lever, and why it's not as scary as it sounds

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

The one-sentence reassurance: **undervolting cannot physically damage silicon.** Too little
voltage makes transistors miss timing → crashes, hangs, corrupted computation. Annoying, not
destructive. (Over-volting and over-heating damage chips; under-volting does not.)

| Failure | Can it happen? | Consequence | Recovery |
|---|---|---|---|
| Crash / freeze during play | Yes — that's how you find the limit | Reboot | Automatic (and our crash-safe saves + targeted fsync limit the blast radius) |
| Fails to boot with the modified DTB | Yes, if too aggressive | Device won't start **with that DTB** | Restore the original DTB / boot media (see §5) |
| SD-card data corruption from a crash mid-write | Rare | A damaged file | fsync discipline + FAT repair; saves are small and synced |
| Permanent hardware damage | **No** | — | — |
| "Bricked" (unrecoverable) | **Effectively no on Allwinner** — see §5 | — | FEL mode |

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
  *removing the card* restores stock perfectly. Unbrickable by construction.

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
   worst-case load loop (PS1 + supafaust) across temperatures. Repeat stepwise until first
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

- Undervolting is a **data-file change, not a hardware mod**; it cannot damage anything.
- On Allwinner, the recovery story (SD-first boot + FEL mask-ROM rescue) makes true bricking
  effectively impossible — *provided we never touch internal storage*, which is the standing rule.
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

