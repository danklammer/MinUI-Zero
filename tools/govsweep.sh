#!/bin/sh
# tools/govsweep.sh — automated per-system governor sweep. Run ON THE DEVICE (push via
# `cat tools/govsweep.sh | ssh root@<brick> 'cat > /tmp/govsweep.sh'`, then
# `( sh /tmp/govsweep.sh & )`). Launches each base system, lets the governor settle (90s),
# samples 45s of clock/CPU, captures that session's gov log lines. ~14 min. Output: /tmp/govsweep.log
#
# MEASUREMENT CONDITIONS GUARD (learned 2026-07-02): a valid run requires DISCHARGING with no
# USB host. Charging on a computer spawns MtpDaemon (SD indexing) + adbd — the I/O contention
# stretches frame periods and fakes governor overruns; every ceiling maxes out. Data = garbage.

OUT=/tmp/govsweep.log
LOGS=/mnt/SDCARD/.userdata/tg5040/logs
EMUS=/mnt/SDCARD/.system/tg5040/paks/Emus
ROMS=/mnt/SDCARD/Roms
BAT=/sys/class/power_supply/axp2202-battery

# ---- guard: refuse to produce garbage ----
STATUS=$(cat $BAT/status)
if [ "$STATUS" != "Discharging" ]; then
	echo "ABORT: battery is '$STATUS' — unplug the device (valid data needs Discharging + no USB host)" | tee $OUT
	exit 1
fi
CAP=$(cat $BAT/capacity)
if [ "$CAP" -lt 20 ]; then
	echo "ABORT: battery at ${CAP}% — charge to 20%+ first (sweep is ~15 min of max-load gaming)" | tee $OUT
	exit 1
fi

echo "GOVERNOR SWEEP start $(date +%T) (bat ${CAP}% $STATUS)" > $OUT

run_one() {
	TAG="$1"; PAK="$2"; ROM="$3"
	L0=$(grep -c gov "$LOGS/$TAG.txt" 2>/dev/null); [ -z "$L0" ] && L0=0
	killall -9 minarch.elf 2>/dev/null
	sleep 2
	echo "\"$EMUS/$PAK/launch.sh\" \"$ROM\"" > /tmp/next
	killall -9 minui.elf 2>/dev/null
	sleep 90
	PID=$(ps | grep minarch.elf | grep -v grep | awk '{print $1}' | head -1)
	echo "== $TAG ($(date +%T)) ==" >> $OUT
	if [ -z "$PID" ]; then echo "FAILED TO LAUNCH" >> $OUT; return; fi
	J1=$(awk '{print $14+$15}' /proc/$PID/stat)
	FREQS=""
	i=0
	while [ $i -lt 45 ]; do
		sleep 1
		FREQS="$FREQS $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)"
		i=$((i+1))
	done
	J2=$(awk '{print $14+$15}' /proc/$PID/stat 2>/dev/null); [ -z "$J2" ] && J2=$J1
	echo "freq_hist: $(echo $FREQS | tr ' ' '\n' | sort -n | uniq -c | tr '\n' ';')" >> $OUT
	echo "cpu_pct_avg: $(( (J2-J1)*100/45/100 ))" >> $OUT
	echo "temp_c: $(( $(cat /sys/class/thermal/thermal_zone0/temp)/1000 ))" >> $OUT
	echo "gov_lines_this_session:" >> $OUT
	grep gov "$LOGS/$TAG.txt" 2>/dev/null | tail -n +$((L0+1)) >> $OUT
	echo "" >> $OUT
}

# One demanding, attract-mode-friendly title per base system. Edit paths to taste.
run_one GBC  GBC.pak  "$ROMS/Game Boy Color (GBC)/Zelda DX - Links Awakening.gbc"
run_one FC   FC.pak   "$ROMS/Nintendo Entertainment System (FC)/Contra.nes"
run_one SUPA SUPA.pak "$ROMS/Super Nintendo Entertainment System (SUPA)/Aladdin.sfc"
run_one MD   MD.pak   "$ROMS/Sega Genesis (MD)/Aladdin.md"
run_one GBA  GBA.pak  "$ROMS/Game Boy Advance (GBA)/Advance Wars.gba"
run_one PS   PS.pak   "$ROMS/Sony PlayStation (PS)/Tony Hawk's Pro Skater/Tony Hawk's Pro Skater.cue"

killall -9 minarch.elf 2>/dev/null
echo "SWEEP DONE $(date +%T) (bat $(cat $BAT/capacity)%)" >> $OUT
