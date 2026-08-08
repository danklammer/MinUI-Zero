#!/bin/sh
# MinUI Zero — the "frontend" of the stripped-muOS OS. muOS's startup.sh brings up ALL the hard
# parts (kernel modules via udev, wifi via network.sh, ssh, mounts, power) exactly as it always
# has; startup.sh's `FRONTEND start` line is swapped for this, so MinUI is the UI instead of
# muxfrontend. Everything below the UI is muOS's proven stack — we reinvent nothing.
#
# muOS mounts the ROMS/data partition at /mnt/mmc (via /opt/muos/script/mount). Our payload lives
# there under .system/h700, same as the piggyback/hosted-dev loop.

export PLATFORM=h700
export SDCARD_PATH=/mnt/mmc
export SYSTEM_PATH=/mnt/mmc/.system/h700
export USERDATA_PATH=/mnt/mmc/.userdata/h700
export LOGS_PATH=/mnt/mmc/.userdata/h700/logs
export SHARED_USERDATA_PATH=/mnt/mmc/.userdata/shared
export SAVES_PATH=/mnt/mmc/Saves
export BIOS_PATH=/mnt/mmc/Bios
export CORES_PATH=/mnt/mmc/.system/h700/cores
export LD_LIBRARY_PATH=/mnt/mmc/.system/h700/lib:/usr/lib:/lib
# audio: pipewire is REMOVED (build-h700-stripped.sh). startup.sh's trimmed pipewire.sh does the
# codec init (alsactl restore) at boot; ALSA routes default->hw directly (asound.conf); minui and
# the emu paks use SDL_AUDIODRIVER=alsa. Nothing audio-related to do here.
export SDL_VIDEODRIVER=dummy

LOG=/mnt/mmc/minui-zero.log
: > "$LOG" 2>/dev/null
echo "MinUI Zero frontend $(date 2>/dev/null)" >> "$LOG"

# NOTE: an "efficiency" kill of muOS idle daemons (lowpower/keepalive/muhotkey/activity) lived here
# but was removed (2026-08-06). It was an UNMEASURED optimization — the thesis is "earn it by
# measurement," and this was vibes ("loops burn wakeups") without a wakeup receipt or a per-service
# safety check. keepalive.sh in particular is a plausible network keepalive, and a device that boots
# then drops wifi (seen live) is a far worse outcome than a few shell-loop wakeups. Revisit only with
# a measured per-service wakeup cost + a proven-safe-to-kill check, one service at a time.

# CODEC INIT (audio): restore the mixer state (unmute + digital volume 24) SYNCHRONOUSLY before
# minui starts. startup.sh's line-63 `pipewire.sh start &` also does this, but BACKGROUNDED, so it
# races minui's InitSettings (which reads the codec volume ONCE at launch). Losing that race left
# digital volume at the power-on 0 = dead silent (found live 2026-08-06). Doing it here, blocking,
# guarantees the codec is unmuted and at its 24 baseline before minui ever reads it.
alsactl -U -f /opt/muos/device/control/asound.state restore 2>/dev/null
echo "audio: digital volume $(amixer -c 0 sget 'digital volume' 2>/dev/null | grep -oE '[0-9]+ \[' | tr -d ' [')" >> "$LOG"

# THE THESIS: own the governor. schedutil + our minui/minarch write the ceiling on top.
echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" >> "$LOG"

# WiFi + SSH bring-up. Credentials are USER-SUPPLIED (never baked into the image): the MinUI
# convention is a wifi.txt at the SD-card root, "SSID:password" per line, '#' comments. We use the
# first network. No wifi.txt = offline (MinUI is offline-by-default anyway) and this whole block
# stays quiet, so the efficient default is untouched.
#
# The RTL8821CS here has two quirks we work around:
#   1. Useless on 5GHz — it associates but the link is ~16bps/ssh-dead. So the wpa config is
#      freq_list=2.4GHz-only, and any non-2.4GHz link (e.g. a leftover/stock 5GHz auto-connect that
#      beat us to boot) is torn down and replaced. A healthy 2.4GHz link is left alone (restart-safe).
#   2. Drops idle links in ~10-15s and never reconnects itself. muOS's keepalive.sh (credit
#      johnnyonflame) does echo 0 > .../8821cs/parameters/rtw_power_mgnt to disable the driver idle
#      power-mgmt; we do that in wifi_up_2ghz BEFORE associating so the driver honours it. On this AP
#      that alone still drops, so we add a ~10s keepalive ping (device-originated traffic) that holds
#      the link, and re-associate if it is ever lost. Radio-awake cost applies only when wifi is
#      opted-in via wifi.txt (Dan 2026-08-08, revisiting the earlier "don't fight IPS" call).
# SSH: dropbear (we ship dropbearmulti; muOS's openssh is stripped), started once below.
WIFI_TXT=/mnt/mmc/wifi.txt

# healthy iff associated on a 2.4GHz channel (freq 24xx) AND holding an IPv4 lease
wifi_ok() { iw dev wlan0 link 2>/dev/null | grep -qE "freq: 24[0-9][0-9]" && ip -4 -o addr show wlan0 2>/dev/null | grep -q inet; }
# (re)associate on 2.4GHz-only using the wifi.txt credentials
wifi_up_2ghz() {
	_line=$(sed '/^#/d;/^[[:space:]]*$/d' "$WIFI_TXT" | head -1)
	_ssid=${_line%%:*}; _psk=${_line#*:}
	[ -n "$_ssid" ] && [ "$_ssid" != "$_line" ] || return
	[ -e /sys/class/net/wlan0 ] || return
	printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\nnetwork={\n\tssid="%s"\n\tpsk="%s"\n\tscan_ssid=1\n\tfreq_list=2412 2417 2422 2427 2432 2437 2442 2447 2452 2457 2462 2467 2472\n}\n' "$_ssid" "$_psk" > /etc/wpa_supplicant.conf
	ifconfig wlan0 up 2>/dev/null
	iw dev wlan0 set power_save off 2>/dev/null
	# ROOT-CAUSE fix for the ~20s idle drop: disable the 8821cs driver idle power management. This is
	# muOS's own fix (keepalive.sh, credit johnnyonflame) — the link then holds without a ping loop.
	echo 0 > /sys/module/8821cs/parameters/rtw_power_mgnt 2>/dev/null
	killall wpa_supplicant 2>/dev/null; sleep 1
	mkdir -p /var/run/wpa_supplicant /var/db/dhcpcd /run
	wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -D nl80211 2>/dev/null
	sleep 3
	dhcpcd -t 25 wlan0 2>/dev/null
}
# keepalive ping target: gateway, else .1 of wlan0's subnet, else public DNS. A bare default-route
# lookup can come back empty (it did — the link then never got pinged and dropped); this can't.
ka_target() {
	_t=$(ip route 2>/dev/null | awk '/default/{print $3; exit}')
	case "$_t" in *.*.*.*) echo "$_t"; return ;; esac
	_p=$(ip -4 -o addr show wlan0 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.' | head -1)
	[ -n "$_p" ] && { echo "${_p}1"; return; }
	echo 8.8.8.8
}
( if [ -f "$WIFI_TXT" ]; then
	for _ in 1 2 3 4 5 6 7 8; do [ -e /sys/class/net/wlan0 ] && break; sleep 2; done
	wifi_ok || wifi_up_2ghz
	# Keep the opt-in link up. rtw_power_mgnt=0 (set in wifi_up_2ghz BEFORE association) helps, but a
	# ~12s keepalive ping (device-originated traffic to ka_target) is what reliably holds the radio;
	# re-associate if the link is genuinely lost. Only runs when wifi.txt is present.
	( T=$(ka_target)
	  while : ; do
		if wifi_ok; then
			ping -c1 -W2 "$T" >/dev/null 2>&1
		else
			wifi_up_2ghz
			T=$(ka_target)
		fi
		sleep 12
	  done ) &
  fi
  # SSH via dropbear (we ship dropbearmulti, ~250KB; muOS's 32MB openssh is stripped). Key-auth
  # only, reading /root/.ssh/authorized_keys; the ed25519 host key lives on the card so the
  # fingerprint stays stable across boots. Same pattern the Smart Pro uses (skeleton .../dev-net.sh).
  chmod 700 /root/.ssh 2>/dev/null; chmod 600 /root/.ssh/authorized_keys 2>/dev/null
  DBM="$SYSTEM_PATH/bin/dropbearmulti"
  DBKEY="$USERDATA_PATH/dropbear_ed25519_host_key"
  if [ -x "$DBM" ] && ! pgrep dropbearmulti >/dev/null 2>&1; then
    mkdir -p "$USERDATA_PATH" 2>/dev/null
    [ -f "$DBKEY" ] || "$DBM" dropbearkey -t ed25519 -f "$DBKEY" 2>/dev/null
    "$DBM" dropbear -r "$DBKEY" -p 22 2>/dev/null
  fi
  echo "wifi: $(ip -4 -o addr show wlan0 2>/dev/null | awk '{print $4}') ssh=$(pgrep dropbearmulti >/dev/null && echo up || echo down)" >> "$LOG"
) &

mkdir -p "$LOGS_PATH" "$SAVES_PATH" "$SHARED_USERDATA_PATH/.minui" 2>/dev/null

# power-off the muOS way (AXP register; plain poweroff reboots) — reused from halt.sh
power_off() {
	sync
	echo 0x1801 > /sys/class/axp/axp_reg 2>/dev/null
	/opt/muos/script/system/halt.sh poweroff 2>/dev/null
	poweroff -f
}

cd /tmp
FAILS=0
while : ; do
	rm -f /tmp/next /tmp/poweroff
	"$SYSTEM_PATH/bin/minui.elf" >> "$LOG" 2>&1
	RC=$?
	# PLAT_powerOff (owned OS) drops /tmp/poweroff so the loop can tell a real poweroff request from
	# a normal game/menu exit — without it an in-game poweroff looked like a quit and re-launched the
	# same game (audit 2026-08-07).
	[ -f /tmp/poweroff ] && { echo "poweroff requested" >> "$LOG"; power_off; }
	if [ -f /tmp/next ]; then
		FAILS=0
		CMD=$(cat /tmp/next)
		echo "launch: $CMD" >> "$LOG"
		sh -c "$CMD"
		echo "game exited rc=$?" >> "$LOG"
		[ -f /tmp/poweroff ] && { echo "poweroff requested (in-game)" >> "$LOG"; power_off; }
	elif [ "$RC" = "0" ]; then
		echo "clean exit — power off" >> "$LOG"
		power_off
	else
		FAILS=$((FAILS+1))
		echo "minui exited rc=$RC (fail $FAILS)" >> "$LOG"
		# Retry so transient faults self-heal; persistent failure powers OFF rather than the old
		# infinite `sleep 60` park, which left a black, draining, unrecoverable device (audit).
		[ $FAILS -ge 5 ] && { echo "$FAILS consecutive fails — powering off" >> "$LOG"; power_off; }
		sleep 2
	fi
done
