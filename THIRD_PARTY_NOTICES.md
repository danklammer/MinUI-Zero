# Third-party notices / provenance

This is a community fork of `shauninman/MinUI`. We borrow and adapt code from sibling
forks freely; this file records what was taken and from where, so provenance is preserved.
Add an entry whenever code or a non-trivial technique is imported, with the donor repo,
branch, and commit SHA(s).

---

## Deep sleep (suspend-to-RAM)
- **Source:** `zhaofengli/MinUI`, branch `deep-sleep`
- **Commits:** `8266da9`..`6400c0e` (Implement deep sleep; Enable on tg3040/tg5040; platform
  suspend executable; retry on failure; faux-sleep→suspend escalation; `bin/suspend` script;
  ALSA mixer save/restore).
- **Imported into:** `workspace/all/common/api.c` (`PWR_sleep`/`PWR_deepSleep`/`PWR_waitForWake`
  escalation, weak `PLAT_deepSleep`/`PLAT_supportsDeepSleep`, resume debounce),
  `workspace/all/common/api.h`, `workspace/all/common/defines.h` (`BIN_PATH`),
  `workspace/tg5040/platform/platform.c` (`PLAT_supportsDeepSleep`),
  `skeleton/SYSTEM/tg5040/bin/suspend`.
- **Modifications:** ported onto our tree (no rename churn beyond `PWR_fauxSleep`→`PWR_sleep`),
  `DEEP_SLEEP_DELAY` made a named tunable, `bin/suspend` detach syntax made POSIX-explicit.
- See `docs/deep-sleep-design.md`.

---

## Governor evidence (referenced, not copied)
The closed-loop CPU governor (`workspace/all/common/governor.{c,h}`) is our own implementation.
NextUI commits informed the *direction* (kernel-governor architecture) but no code was copied:
`6990d474` (userspace loop → kernel governors), `afb3783d` (ondemand→schedutil + caps),
`e9e91137` (stuck-in-performance fix). See `docs/thermal-governor-design.md`.
