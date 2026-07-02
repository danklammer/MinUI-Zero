#!/bin/sh
# tools/sleep-soak.sh — suspend/resume soak for the deep-sleep default (pillar 4).
# Run ON THE DEVICE, detached. Each cycle: set an RTC wake-alarm, run the real suspend
# choreography (bin/suspend — same script PWR_deepSleep invokes), verify the device came
# back and the game survived. Kernel+driver soak; the minarch enter/exit path is exercised
# separately (it's the same suspend underneath).
#
# Usage: sh /tmp/sleep-soak.sh [cycles]   (default 3 = smoke test; 50 = overnight soak)

CYCLES=${1:-3}
# log to SD, not /tmp: repeated suspends can wedge wifi (dev-mode), and rescuing results
# from tmpfs without a network means a reboot that erases them (learned 2026-07-02)
OUT=/mnt/SDCARD/.userdata/tg5040/logs/soak.log
RTC=/sys/class/rtc/rtc0/wakealarm
SUSPEND=/mnt/SDCARD/.system/tg5040/bin/suspend
BAT=/sys/class/power_supply/axp2202-battery

echo "SLEEP SOAK start $(date +%T) cycles=$CYCLES bat=$(cat $BAT/capacity)%" > $OUT
[ -f "$RTC" ] || { echo "ABORT: no rtc wakealarm" >> $OUT; exit 1; }
[ -x "$SUSPEND" ] || { echo "ABORT: no suspend script" >> $OUT; exit 1; }

GAME_PID=$(ps | grep minarch.elf | grep -v grep | awk '{print $1}' | head -1)
echo "game pid at start: ${GAME_PID:-none}" >> $OUT

i=0
FAILS=0
while [ $i -lt $CYCLES ]; do
	i=$((i+1))
	T0=$(date +%s)
	echo 0 > $RTC 2>/dev/null           # clear any stale alarm
	echo $((T0+40)) > $RTC              # wake 40s from now
	sh $SUSPEND                          # real choreography: quiesce, save mixer, echo mem
	T1=$(date +%s)
	AWAKE=$((T1-T0))
	# post-resume checks
	P=$(ps | grep minarch.elf | grep -v grep | awk '{print $1}' | head -1)
	GAME_OK="n/a"
	[ -n "$GAME_PID" ] && { [ "$P" = "$GAME_PID" ] && GAME_OK=yes || GAME_OK=NO; }
	TEMP=$(( $(cat /sys/class/thermal/thermal_zone0/temp) / 1000 ))
	echo "cycle $i: slept+resumed in ${AWAKE}s game=$GAME_OK temp=${TEMP}C bat=$(cat $BAT/capacity)%" >> $OUT
	[ "$GAME_OK" = "NO" ] && FAILS=$((FAILS+1))
	[ $AWAKE -lt 20 ] && { echo "  WARNING: cycle too short (${AWAKE}s) — suspend may have failed" >> $OUT; FAILS=$((FAILS+1)); }
	sleep 15  # settle between cycles
done

echo 0 > $RTC 2>/dev/null
echo "SOAK DONE $(date +%T): $CYCLES cycles, $FAILS failures, bat=$(cat $BAT/capacity)%" >> $OUT
