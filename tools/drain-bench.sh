#!/bin/sh
# drain-bench.sh — measure battery drain (efficiency) on the TrimUI Brick via the AXP2202
# coulomb counter. `current_now` is EMPTY on this PMIC, but `charge_counter` works and tracks
# remaining charge in the same unit as `charge_full` (~mAh; charge_full=3000). Drain rate over a
# fixed window = a real efficiency number. Use RATIOS for A/B (unit-exact); treat mW as an estimate.
#
# Usage: drain-bench.sh [seconds] [label]   (run under a STEADY workload — e.g. a game already up)
DUR="${1:-300}"; LABEL="${2:-run}"
BATT=/sys/class/power_supply/axp2202-battery
CF=/sys/devices/system/cpu/cpu0/cpufreq
rd() { cat "$1" 2>/dev/null; }

cc0=$(rd $BATT/charge_counter); v0=$(rd $BATT/voltage_now); cap0=$(rd $BATT/capacity)
t0=$(cut -d. -f1 /proc/uptime)
core=$(ps 2>/dev/null | grep -o '[a-z0-9_]*_libretro.so' | head -1)
echo "[$LABEL] start cc=$cc0 cap=${cap0}% V=$((v0/1000))mV core=${core:-none} status=$(rd $BATT/status)  window=${DUR}s"
[ "$(rd $BATT/status)" = "Charging" ] && echo "  WARN: charging — drain reading invalid; unplug USB."

i=0; n=$(( DUR/30 )); [ $n -lt 1 ] && n=1
while [ $i -lt $n ]; do
  sleep 30
  cc=$(rd $BATT/charge_counter); tmp=$(( $(rd /sys/class/thermal/thermal_zone0/temp)/1000 ))
  echo "  +$(( (i+1)*30 ))s cc=$cc dcc=$(( cc0-cc )) cur=$(rd $CF/scaling_cur_freq) cpu=${tmp}C"
  i=$((i+1))
done

cc1=$(rd $BATT/charge_counter); v1=$(rd $BATT/voltage_now); cap1=$(rd $BATT/capacity)
t1=$(cut -d. -f1 /proc/uptime); el=$(( t1-t0 )); [ $el -lt 1 ] && el=1
dcc=$(( cc0-cc1 )); rate_h=$(( dcc*3600/el )); vavg=$(( (v0+v1)/2000 ))
echo "[$LABEL] END dcc=$dcc units / ${el}s = ${rate_h} units/hr (~mA)  cap ${cap0}->${cap1}%  Vavg ${vavg}mV"
echo "[$LABEL] est ~$(( rate_h*vavg/1000 )) mW  [charge_counter unit assumed mAh; trust the A/B ratio, not absolute mW]"
