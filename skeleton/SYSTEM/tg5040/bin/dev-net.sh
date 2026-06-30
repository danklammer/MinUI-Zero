#!/bin/sh
# dev-net.sh — DEV-MODE networking for the tg5040 fork: bring up wifi + SSH for testing.
# Runs from MinUI.pak/launch.sh ONLY when /mnt/SDCARD/.userdata/shared/enable-ssh exists, so a
# normal install never enables radios (stays runs-cold). Best-effort; every device-specific
# command is guarded. Inputs/outputs all live on the SD under .userdata/shared:
#   wifi.conf         (you fill in)  SSID="..."  PSK="..."
#   authorized_keys   (you provide)  the dev SSH public key (key auth, non-interactive)
#   ssh-ip.txt        (we write)     the obtained IP + the exact ssh command to use
SD=/mnt/SDCARD
SHARED="$SD/.userdata/shared"
LOG="$SHARED/ssh-ip.txt"

{
  echo "=== dev-net $(date 2>/dev/null) ==="

  # 1) radios on
  rfkill unblock all 2>/dev/null || true
  ifconfig wlan0 up 2>/dev/null || true

  # 2) wifi config: build /etc/wifi/wpa_supplicant.conf from wifi.conf if provided,
  #    else use whatever the device already has (e.g. configured via stock firmware).
  if [ -f "$SHARED/wifi.conf" ]; then
    . "$SHARED/wifi.conf"
    if [ -n "$SSID" ]; then
      mkdir -p /etc/wifi/sockets
      if command -v wpa_passphrase >/dev/null 2>&1; then
        wpa_passphrase "$SSID" "$PSK" > /etc/wifi/wpa_supplicant.conf 2>/dev/null
      else
        printf 'ctrl_interface=/etc/wifi/sockets\nupdate_config=1\nnetwork={\n\tssid="%s"\n\tpsk="%s"\n}\n' "$SSID" "$PSK" > /etc/wifi/wpa_supplicant.conf
      fi
    fi
  fi

  # 3) (re)start wifi — same invocation the device's own suspend/resume uses
  killall -9 wpa_supplicant 2>/dev/null
  wpa_supplicant -B -D nl80211 -iwlan0 -c /etc/wifi/wpa_supplicant.conf -O /etc/wifi/sockets 2>/dev/null || true
  ( udhcpc -i wlan0 & ) 2>/dev/null || true

  # 4) install the dev SSH public key for non-interactive (key) auth as root
  if [ -f "$SHARED/authorized_keys" ]; then
    mkdir -p /root/.ssh
    cp "$SHARED/authorized_keys" /root/.ssh/authorized_keys
    chmod 700 /root/.ssh 2>/dev/null; chmod 600 /root/.ssh/authorized_keys 2>/dev/null
  fi

  # 5) start the SSH daemon (TrimUI uses dropbear on :2022)
  /etc/init.d/dropbear start 2>/dev/null \
    || dropbear -p 2022 2>/dev/null \
    || /usr/sbin/dropbear -p 2022 2>/dev/null \
    || true

  # 6) wait for an IP, then log it + the exact ssh command
  ip=""
  i=0
  while [ "$i" -lt 20 ]; do
    ip=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
    [ -z "$ip" ] && ip=$(ip -4 addr show wlan0 2>/dev/null | sed -n 's#.*inet \([0-9.]*\)/.*#\1#p' | head -1)
    [ -n "$ip" ] && break
    sleep 1; i=$((i + 1))
  done
  echo "wlan0 IP: ${ip:-<none — check wifi.conf / signal>}"
  echo "dropbear: $(pgrep dropbear >/dev/null 2>&1 && echo running || echo NOT-running)"
  echo "connect:  ssh -i ~/.ssh/tg5040_dev -p 2022 root@${ip:-<ip>}"
} >> "$LOG" 2>&1
