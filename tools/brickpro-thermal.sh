#!/bin/sh
# Thermal + clock profile for the TrimUI Brick Pro, per system.
#
#   sh tools/brickpro-thermal.sh [user@host]
#
# For the menu and for one title per shipped system: launch it, let it settle, then sample
# cpu/gpu/ddr temperature and the CPU clock once a second and report mean/max.
#
# WHY THIS EXISTS: this fork's whole thesis is "lowest clock that holds frame rate". A new model
# that quietly runs at a higher clock, or hotter, is a thesis regression even when every game
# launches fine. Reference figures for the Brick live in docs/ and CLAUDE.md.
#
# NOTE ON THE MENU NUMBER: GPU-dark (ZERO_FB_PRESENT) is Brick-only, so the Brick Pro menu is
# expected to sit ABOVE the Brick's ~26C idle. That gap is the GPU domain staying lit, not a
# governor fault. Do not read it as a regression without testing GPU-dark on this panel first.
set -u

TARGET=${1:-root@192.168.1.235}
KEY=/Users/dk/.ssh/tg5040_dev
SD=/mnt/SDCARD
SYS="$SD/.system/tg5040"
ROMS="$SD/Roms"
SAMPLES=${SAMPLES:-12}   # seconds of sampling per subject
SETTLE=${SETTLE:-20}     # seconds to let the game reach steady state first

rsh() { ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-o ConnectTimeout=8 -o BatchMode=yes -o LogLevel=ERROR "$TARGET" "$@"; }

rsh 'echo ok' >/dev/null 2>&1 || { echo "device unreachable at $TARGET"; exit 1; }

# sample: prints "cpuC gpuC ddrC kHz" once a second, SAMPLES times, then a mean/max line
sample() { # <label>
	rsh "n=0; ct=0; cm=0; gm=0; dm=0; fs=0; fmax=0
	while [ \$n -lt $SAMPLES ]; do
		c=\$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
		g=\$(cat /sys/class/thermal/thermal_zone1/temp 2>/dev/null)
		d=\$(cat /sys/class/thermal/thermal_zone2/temp 2>/dev/null)
		f=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null)
		ct=\$((ct+c)); fs=\$((fs+f))
		[ \$c -gt \$cm ] && cm=\$c; [ \$g -gt \$gm ] && gm=\$g; [ \$d -gt \$dm ] && dm=\$d
		[ \$f -gt \$fmax ] && fmax=\$f
		n=\$((n+1)); sleep 1
	done
	printf '%-6s cpu avg %d.%dC max %d.%dC | gpu max %d.%dC | ddr max %d.%dC | clk avg %d MHz max %d MHz\n' \
		'$1' \$((ct/$SAMPLES/1000)) \$((ct/$SAMPLES%1000/100)) \$((cm/1000)) \$((cm%1000/100)) \
		\$((gm/1000)) \$((gm%1000/100)) \$((dm/1000)) \$((dm%1000/100)) \
		\$((fs/$SAMPLES/1000)) \$((fmax/1000))"
}

play() { # <label> <cmdline>
	printf '%s\n' "$2" | rsh "cat > /tmp/next" 2>/dev/null
	rsh "kill -9 \$(pidof minui.elf) 2>/dev/null"
	i=0
	while [ $i -lt 40 ]; do rsh "pidof minarch.elf >/dev/null 2>&1" && break; sleep 1; i=$((i+1)); done
	rsh "pidof minarch.elf >/dev/null 2>&1" || { echo "  $1: FAILED TO START"; return 1; }
	sleep "$SETTLE"
	sample "$1"
	rsh "kill \$(pidof minarch.elf) 2>/dev/null"
	i=0
	while [ $i -lt 20 ]; do rsh "pidof minarch.elf >/dev/null 2>&1" || break; sleep 1; i=$((i+1)); done
	i=0
	while [ $i -lt 25 ]; do rsh "pidof minui.elf >/dev/null 2>&1" && break; sleep 1; i=$((i+1)); done
}

echo "=== Brick Pro thermal profile (${SETTLE}s settle, ${SAMPLES}s sample)"
echo "--- menu idle (GPU-dark is Brick-only, so expect above the Brick's ~26C)"
sample MENU

play GBC  "\"$SYS/paks/Emus/GBC.pak/launch.sh\" \"$ROMS/1) Game Boy Color (GBC)/Bionic Commando - Elite Forces.gbc\""
play GBA  "\"$SYS/paks/Emus/GBA.pak/launch.sh\" \"$ROMS/2) Game Boy Advance (GBA)/Advance Wars 2 - Black Hole Rising.gba\""
play FC   "\"$SYS/paks/Emus/FC.pak/launch.sh\" \"$ROMS/3) Nintendo (FC)/1942.nes\""
play SUPA "\"$SYS/paks/Emus/SUPA.pak/launch.sh\" \"$ROMS/4) Super Nintendo (SUPA)/ActRaiser.sfc\""
play MD   "\"$SYS/paks/Emus/MD.pak/launch.sh\" \"$ROMS/5) Sega Genesis (MD)/Adventures of Batman & Robin.md\""
PS_ROM=$(rsh "ls \"$ROMS/6) PlayStation (PS)/Ace Combat 2/\"*.cue 2>/dev/null | head -1")
[ -n "$PS_ROM" ] && play PS "\"$SYS/paks/Emus/PS.pak/launch.sh\" \"$PS_ROM\""

echo "--- back at the menu"
sample MENU2
