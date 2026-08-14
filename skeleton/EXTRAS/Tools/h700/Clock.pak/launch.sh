#!/bin/sh
# Clock — set the system time and toggle the menu clock / 24-hour display.
# The shared clock.elf (workspace/all/clock) already builds for h700; this platform simply never
# shipped a Tools pak, so the tool existed and was unreachable (parity audit 2026-08-10).
# It matters MORE here than on the Brick: our lean strip removes chrony, so there is no NTP on this
# device and the RTC (/dev/rtc, hwclock) is the only source of truth for the date.
# Env (PLATFORM, SYSTEM_PATH, USERDATA_PATH, LD_LIBRARY_PATH for libmsettings) is exported by the
# frontend and inherited here, so this needs no per-pak boilerplate.
cd "$(dirname "$0")"
./clock.elf
