#!/bin/sh
# brick-recon.sh — gather CPU freq / governor / thermal facts on the TrimUI Brick (tg5040)
# so we can replace the assumed values in docs/thermal-governor-design.md with real ones.
#
# Run over a shell on the device, twice:
#   1) idle at the menu        -> baseline
#   2) during a demanding game -> load profile
# Then commit both outputs. Written for busybox ash (no bashisms).

hdr()  { printf '\n==== %s ====\n' "$*"; }
dump() { # dump <label> <path>
  if [ -r "$2" ]; then printf '%-26s %s\n' "$1:" "$(cat "$2" 2>/dev/null)"
  else              printf '%-26s (absent)\n' "$1:"; fi
}

hdr "Kernel / cores"
dump "kernel" /proc/version
printf '%-26s %s\n' "nproc:" "$(nproc 2>/dev/null || grep -c '^processor' /proc/cpuinfo)"

hdr "cpufreq policies (the real OPP steps)"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  printf -- '-- %s\n' "$p"
  dump "affected_cpus"       "$p/affected_cpus"
  dump "scaling_driver"      "$p/scaling_driver"
  dump "scaling_governor"    "$p/scaling_governor"
  dump "available_governors" "$p/scaling_available_governors"
  dump "available_freqs"     "$p/scaling_available_frequencies"
  dump "cpuinfo_min_freq"    "$p/cpuinfo_min_freq"
  dump "cpuinfo_max_freq"    "$p/cpuinfo_max_freq"
  dump "scaling_cur_freq"    "$p/scaling_cur_freq"
done

hdr "Per-core view (MinUI only pokes cpu0 — is the policy cluster-wide?)"
for c in /sys/devices/system/cpu/cpu[0-9]*/cpufreq; do
  [ -d "$c" ] || continue
  printf '%-8s gov=%-12s cur=%s kHz\n' "$(basename "$(dirname "$c")")" \
    "$(cat "$c/scaling_governor" 2>/dev/null)" "$(cat "$c/scaling_cur_freq" 2>/dev/null)"
done

hdr "Is an 'auto' governor present? (NextUI hands off to one)"
gl=" $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors 2>/dev/null) "
case "$gl" in
  *" auto "*) echo "yes — 'auto' exists, can reuse it" ;;
  *)          echo "no  — closed-loop in userspace, or patch the kernel" ;;
esac

hdr "Voltage / OPP exposure (usually hidden without a custom kernel)"
for path in /sys/kernel/debug/opp /sys/class/devfreq /sys/class/regulator; do
  [ -e "$path" ] && echo "present: $path" || echo "absent:  $path"
done
for r in /sys/class/regulator/regulator.*/; do
  [ -d "$r" ] || continue
  printf '  %s name=%s uV=%s\n' "$(basename "$r")" \
    "$(cat "$r/name" 2>/dev/null)" "$(cat "$r/microvolts" 2>/dev/null)"
done

hdr "Thermal zones (find which one is the CPU)"
for z in /sys/class/thermal/thermal_zone*; do
  [ -d "$z" ] || continue
  printf '%-20s type=%-16s temp=%s mC\n' "$(basename "$z")" \
    "$(cat "$z/type" 2>/dev/null)" "$(cat "$z/temp" 2>/dev/null)"
done

hdr "Live sample: temp + cpu0 freq, 1s x 20 (idle now; re-run mid-game)"
ZONE=/sys/class/thermal/thermal_zone0/temp   # adjust to the CPU zone from the list above
FREQ=/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
i=0
while [ "$i" -lt 20 ]; do
  printf 't=%2ss  temp=%s mC  freq=%s kHz\n' "$i" \
    "$(cat "$ZONE" 2>/dev/null)" "$(cat "$FREQ" 2>/dev/null)"
  i=$((i + 1)); sleep 1
done
