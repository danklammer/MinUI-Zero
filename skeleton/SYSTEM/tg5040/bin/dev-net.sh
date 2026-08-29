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
    # SAY whether it landed. /root is not writable on every firmware, and a silent failure here
    # looks exactly like a wrong key from the other end.
    if [ -s /root/.ssh/authorized_keys ]; then
      echo "authorized_keys: installed ($(wc -c < /root/.ssh/authorized_keys 2>/dev/null) bytes)"
    else
      echo "authorized_keys: FAILED to install into /root/.ssh (read-only or full?)"
    fi
  else
    echo "authorized_keys: none provided in $SHARED"
  fi

  # 5) start the SSH daemon. Try the device's own dropbear first (the Brick's firmware has
  #    one; the Smart Pro's does NOT — its dev-net log read "dropbear: NOT-running"), then
  #    fall back to the static dropbearmulti we ship in .system. Host key lives on the CARD
  #    so the fingerprint stays stable across boots and devices.
  /etc/init.d/dropbear start 2>/dev/null \
    || dropbear -p 2022 2>/dev/null \
    || /usr/sbin/dropbear -p 2022 2>/dev/null \
    || true
  # Start OUR OWN daemon on 2022 whenever the binary exists, regardless of what holds :22.
  # The old guard skipped it if ANYTHING was listening on :22, which assumed a foreign daemon
  # would accept the key installed above. The Brick Pro disproves that: its firmware ships
  # OpenSSH on :22, that daemon refused our key, and the guard then declined to start the one
  # daemon we actually control, leaving no way in at all (read off the card 2026-08-30).
  # A second listener is free in dev mode and is the difference between debuggable and not.
  if ! pgrep -f "dropbear.*2022" >/dev/null 2>&1; then
    DBM="$SD/.system/tg5040/bin/dropbearmulti"
    KEY="$SHARED/dropbear_ed25519_host_key"
    if [ -x "$DBM" ]; then
      [ -f "$KEY" ] || "$DBM" dropbearkey -t ed25519 -f "$KEY" 2>/dev/null
      "$DBM" dropbear -r "$KEY" -p 2022 2>/dev/null || true
    else
      echo "dropbearmulti: MISSING at $DBM"
    fi
  fi

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
  echo "listening: $(netstat -tln 2>/dev/null | grep -E ':(22|2022) ' | tr -s ' ' | cut -d' ' -f4 | tr '\n' ' ')"
  echo "connect:  ssh -i ~/.ssh/tg5040_dev -p 2022 root@${ip:-<ip>}"
  echo "or (stock daemon on 22): ssh -i ~/.ssh/tg5040_dev root@${ip:-<ip>}"
} >> "$LOG" 2>&1
