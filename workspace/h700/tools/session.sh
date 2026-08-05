#!/bin/sh
# Hosted MinUI session with the REAL launch loop: menu -> game -> menu, like every shipped platform.
# Self-contained; survives ssh death. Ends when: power pressed in menu (minui exits with no /tmp/next).
#
# WATCHDOG (replaces the fixed 20-min dead-man, which fired MID-GAME: it CONT'd idle.sh — the muOS
# screensaver — over a running game, and its respawn guard checked minui.elf but not minarch.elf,
# so it "restored" muxfrontend on top of Bubble Bobble). The watchdog only restores muOS once
# NEITHER minui.elf NOR minarch.elf has been seen for two consecutive checks, however long the
# session runs. If session.sh itself dies, the orphaned watchdog still restores the device.
LOG=/tmp/session.log
: > $LOG
echo "session start $(date)" >> $LOG

# only ONE session may exist: kill stale watchdogs from previous sessions.
# busybox ps TRUNCATES long cmdlines (the watchdog tag is at the end), so scan /proc instead.
kill_watchdogs() {
	for d in /proc/[0-9]*; do
		case "$(tr '\0' ' ' < $d/cmdline 2>/dev/null)" in
			*session-watchdog*) kill "$(basename $d)" 2>/dev/null;;
		esac
	done
}
kill_watchdogs

restore_muos() {
	FS=$(ps | grep "frontend.sh" | grep -v grep | awk '{print $1}' | head -1)
	ID=$(ps | grep "idle.sh" | grep -v grep | awk '{print $1}' | head -1)
	HK=$(ps | grep muhotkey | grep -v grep | awk '{print $1}' | head -1)
	kill -CONT $FS 2>/dev/null
	kill -CONT $ID 2>/dev/null
	kill -CONT $HK 2>/dev/null
	pidof muxfrontend >/dev/null || sh /opt/muos/script/mux/frontend.sh &
}

# watchdog: argv tagged via $0 so stale instances are findable/killable by name
(
	exec sh -c '
	misses=0
	while :; do
		sleep 60
		if pidof minui.elf >/dev/null || pidof minarch.elf >/dev/null; then
			misses=0
			continue
		fi
		misses=$((misses+1))
		[ $misses -ge 2 ] && break
	done
	FS=$(ps | grep "frontend.sh" | grep -v grep | awk "{print \$1}" | head -1)
	ID=$(ps | grep "idle.sh" | grep -v grep | awk "{print \$1}" | head -1)
	HK=$(ps | grep muhotkey | grep -v grep | awk "{print \$1}" | head -1)
	kill -CONT $FS 2>/dev/null
	kill -CONT $ID 2>/dev/null
	kill -CONT $HK 2>/dev/null
	pidof muxfrontend >/dev/null || sh /opt/muos/script/mux/frontend.sh &
	echo "watchdog restored muOS $(date)" >> /tmp/session.log
	' session-watchdog
) &
WATCHDOG=$!

ID=$(ps | grep "idle.sh" | grep -v grep | awk '{print $1}' | head -1)
FS=$(ps | grep "frontend.sh" | grep -v grep | awk '{print $1}' | head -1)
HK=$(ps | grep muhotkey | grep -v grep | awk '{print $1}' | head -1)
kill -STOP $ID 2>/dev/null
kill -STOP $FS 2>/dev/null
kill -STOP $HK 2>/dev/null
killall -9 muxfrontend 2>/dev/null
echo "muOS frozen" >> $LOG

export LD_LIBRARY_PATH=/mnt/mmc/.system/h700/lib:$LD_LIBRARY_PATH

cd /tmp
rm -f /tmp/next
while : ; do
	./minui.elf > /tmp/minui-session.log 2>&1
	echo "minui exited" >> $LOG
	if [ -f /tmp/next ]; then
		CMD=$(cat /tmp/next)
		rm -f /tmp/next
		echo "launching: $CMD" >> $LOG
		sh -c "$CMD"
		echo "game exited" >> $LOG
	else
		break
	fi
done

kill $WATCHDOG 2>/dev/null
kill_watchdogs
restore_muos
echo "muOS restored $(date)" >> $LOG
