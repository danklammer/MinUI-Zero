#!/bin/sh
# Measure the Miyoo Mini Plus panel's TRUE refresh rate.
#
#   sh tools/mmp-panel-hz.sh [user@host[:port]] [seconds]
#
# WHY THIS EXISTS, and why the obvious method is wrong.
#
# An earlier attempt read the platform's periodic "fb: flip N handed" log line before and after a
# sleep, and timed it with `date` on the HOST across two SSH round trips. That cannot resolve the
# question it was asked: the platform emits that line only every 600 flips, so both endpoints
# quantise to 600-flip boundaries, and SSH latency lands in the wall-clock term. 5400 flips over
# "90s" is 60.000Hz and over "91s" is 59.341Hz — the method could not tell those apart, yet a
# number derived that way was reported as measured. (Caught in review 2026-08-01.)
#
# This version fixes both halves:
#   * timestamps come from /proc/uptime ON THE DEVICE, in the same shell invocation as the counter
#     read, so no network latency enters the interval;
#   * it samples many times and reports the rate from the FIRST and LAST sample, plus a per-sample
#     spread, so quantisation and jitter are visible instead of hidden.
#
# Requires a game running under Strict present (`-minarch_prevent_tearing = Strict`). Under Strict
# the producer waits for the panner, so flips/sec IS the panel rate. Under Lenient the producer
# outruns it and the count reflects the core, not the panel.
set -u

TARGET=${1:-root@192.168.1.10}
SECS=${2:-180}
PORT=22
case "$TARGET" in *:*) PORT=${TARGET##*:}; TARGET=${TARGET%:*} ;; esac
SSH="ssh -p $PORT -o ConnectTimeout=10"
LOGDIR=/mnt/SDCARD/.userdata/miyoomini/logs

$SSH "$TARGET" 'pidof minarch.elf >/dev/null' 2>/dev/null || {
	echo "no game running — launch one under Strict first"; exit 1; }

# One remote shell does the whole sampling loop: counter and uptime read together, no SSH in the
# timing path. Prints "<uptime_seconds> <total_flips>" per sample.
$SSH "$TARGET" "
LOG=\$(ls -t $LOGDIR/*.txt 2>/dev/null | head -1)
i=0
while [ \$i -lt $SECS ]; do
  N=\$(grep 'coalesced away' \"\$LOG\" 2>/dev/null | tail -1 | sed -n 's/.*flip \([0-9]*\) handed.*/\1/p')
  T=\$(cut -d' ' -f1 /proc/uptime)
  [ -n \"\$N\" ] && echo \"\$T \$N\"
  sleep 1
  i=\$((i+1))
done
" 2>/dev/null | awk '
NR==1 { t0=$1; n0=$2 }
{ t=$1; n=$2; if (n!=lastn && lastn!="") { dt=t-lastt; dn=n-lastn; if (dt>0) { r=dn/dt; printf "  sample: %6d flips / %6.2fs = %7.3f Hz\n", dn, dt, r; s+=r; c++ } }
  if (n!=lastn) { lastn=n; lastt=t } }
END {
  if (c<1) { print "  no counter movement — is the game running and logging?"; exit 1 }
  printf "\n  OVERALL: %d flips over %.2fs = %.3f Hz\n", n-n0, t-t0, (n-n0)/(t-t0)
  printf "  mean of %d intervals: %.3f Hz\n", c, s/c
}'
