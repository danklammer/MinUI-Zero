#!/bin/sh
# Deploy the four device-gated fixes to the Miyoo Mini Plus over SSH.
#
#   sh .notes/mmp-build/deploy-mmp.sh [user@host[:port]]
#
# Defaults to root@192.168.1.10. Run from the repo root AFTER a successful
#   make PLATFORMS=miyoomini miyoomini
# (this copies out of ./build/PAYLOAD, so a stale build deploys stale bits — the version stamp
# printed at the end is how you tell.)
#
# What is going over and why it needs a device to judge:
#   minarch.elf         telemetry battery discovery (axp223 path was blank), saved-cfg migration
#   minui.elf           charging-screen POWER hand-back, charging battery bolt
#   MinUI.pak/launch.sh boot-path audio-daemon release (wedged daemon held MI_AO -> silent games)
#   fake08_libretro.so  -O3 + the _vm->Step() load fix (PICO-8 was on upstream -O2 and unpatched)
set -e

TARGET=${1:-root@192.168.1.10}
PORT=22
case "$TARGET" in
	*:*) PORT=${TARGET##*:}; TARGET=${TARGET%:*} ;;
esac

SRC=./build/PAYLOAD/.system/miyoomini
DST=/mnt/SDCARD/.system/miyoomini

[ -d "$SRC" ] || { echo "no build payload at $SRC — run: make PLATFORMS=miyoomini miyoomini"; exit 1; }
[ -f ./build/latest.txt ] || { echo "no build/latest.txt — build did not complete"; exit 1; }

# Refuse to deploy a build older than the working tree. A deploy that silently ships yesterday's
# binaries is worse than no deploy: the device test then certifies the wrong code.
NEWER=$(find workspace skeleton -newer ./build/latest.txt \
	\( -name '*.c' -o -name '*.h' -o -name 'launch.sh' \) -print -quit 2>/dev/null || true)
[ -z "$NEWER" ] || {
	echo "STALE BUILD: $NEWER is newer than build/latest.txt"
	echo "rebuild first: make PLATFORMS=miyoomini miyoomini"
	exit 1
}

SSH="ssh -p $PORT -o ConnectTimeout=5"

# NOT scp. The MMP firmware ships neither /usr/libexec/sftp-server (so modern scp, which speaks
# SFTP by default, dies with "Connection closed") nor an scp binary (so `scp -O`, the legacy-protocol
# fallback, dies with "scp: not found"). Piping through the shell needs only cat, which busybox has.
#
# Stage to a temp path and mv into place: minui.elf/minarch.elf may be RUNNING, and writing directly
# over a live binary is ETXTBSY. mv within the same filesystem is atomic, so a yanked battery
# mid-deploy leaves either the old file or the new one, never a half-written one.
put() { # put <local> <remote>
	$SSH "$TARGET" "cat > '$2.new' && chmod 755 '$2.new' && mv -f '$2.new' '$2'" < "$1"
}

echo "target : $TARGET:$PORT"
echo "build  : $(cat ./build/latest.txt)"

$SSH "$TARGET" true 2>/dev/null || {
	echo "device unreachable at $TARGET:$PORT"
	echo "  - power it on and confirm it is on the network"
	echo "  - deep sleep is disabled on MMP, but the screen may still be off"
	exit 1
}

echo "--- deploying ---"
for f in bin/minarch.elf bin/minui.elf paks/MinUI.pak/launch.sh cores/fake08_libretro.so; do
	echo "  -> $f"
	put "$SRC/$f" "$DST/$f"
done

# Flush to the card before anything can power-cycle it. FAT32 + a yanked battery loses the write.
$SSH "$TARGET" "sync"

echo "--- verifying ---"
for f in bin/minarch.elf bin/minui.elf paks/MinUI.pak/launch.sh cores/fake08_libretro.so; do
	L=$(wc -c < "$SRC/$f" | tr -d ' ')
	R=$($SSH "$TARGET" "wc -c < $DST/$f" | tr -d ' ')
	[ "$L" = "$R" ] && echo "  ok   $f ($L bytes)" || { echo "  FAIL $f: local $L != remote $R"; exit 1; }
done

cat <<EOF

deployed $(cat ./build/latest.txt)

TWO DIFFERENT RESTARTS — they do not pick up the same things.

  $SSH $TARGET 'killall minui.elf'
      Picks up minui.elf, minarch.elf and the cores. This only makes MinUI.pak/launch.sh
      ITERATE its menu loop (the loop is lines ~319-373); everything above it, including
      audio_daemon_start and the boot-path daemon release, does NOT re-run. The running shell
      also still holds the OLD launch.sh inode, because this script installs via mv.

  full power cycle
      The ONLY way to exercise a launch.sh change or anything about daemon startup ordering.
      The daemon can only win MI_AO at boot, before the menu opens the codec, so daemon
      behaviour is simply not testable without one.

Then the gates that still need your eyes/ears:
  1. AUDIO   boot, launch a game, exit, launch another, power off — listen for pops.
  2. CHARGE  plug in at ~100%: bolt icon renders as a bolt (not a white block);
             charge screen shows a number; POWER once sleeps (not twice).
  3. PICO-8  launch Celeste — it must reach the title screen (the Step() fix) and hold 60.
  4. PS1     BR2 + THPS.
  5. SCALING launch a GBC game that had a stale 'Aspect' save — the log should show
             "migrating saved cfg (v0 -> v1): dropping minarch_screen_scaling = Aspect"
             and the scroll shimmer should be gone. Menu must still OFFER scaling.
EOF
