// h700 msettings — HOSTED-DEV GUEST stubs (moved verbatim from platform.c when the makefile
// wiring landed; the shared minui/minarch makefiles link -lmsettings on every platform).
//
// muOS owns audio and brightness while we run as a guest: its pipewire holds the volume, so
// SetVolume is a no-op until we own the image (or talk to pipewire ourselves — see
// README-BRINGUP.md "Order of work"). Brightness passes through the backlight sysfs when present.
// This library is standalone by design (no utils.c), so it carries its own tiny sysfs helpers.
#include <stdio.h>
#include <stdlib.h>

#include "msettings.h"

// Brightness: this device has NO /sys/class/backlight. muOS drives the panel through the
// Allwinner dispdbg debugfs (func.sh DISPLAY_WRITE): name=disp0, command=setbl/getbl,
// param=0-255 (max from /opt/muos/device/config/screen/bright), then start=1. getbl answers
// in /sys/kernel/debug/dispdbg/info, so brightness is READABLE — no persistence file needed.
#define DISPDBG "/sys/kernel/debug/dispdbg/"
#define BRIGHT_RAW_MAX 255
#define BRIGHT_RAW_MIN 8 // UI 0 stays faintly visible, never a black screen

static void putStr(const char* path, const char* s) {
	FILE* file = fopen(path, "w");
	if (file != NULL) {
		fputs(s, file);
		fclose(file);
	}
}
static void dispdbg_cmd(const char* cmd, const char* param) {
	putStr(DISPDBG "name", "disp0");
	putStr(DISPDBG "command", cmd);
	if (param) putStr(DISPDBG "param", param);
	putStr(DISPDBG "start", "1");
}

// Volume goes through pipewire's default sink — EXACTLY the mechanism muOS itself uses
// (/opt/muos/script/system/pipewire.sh: `wpctl set-volume @DEFAULT_AUDIO_SINK@ N%`). As a guest
// this is the polite lane: wireplumber owns the codec mixer; poking amixer under it invites the
// Brick's digital-volume-table saga (D33: Allwinner codec dB tables are garbage — 'digital
// volume' here reports 41214.60dB). The env is embedded because minui runs from session.sh,
// which does not carry the pipewire socket vars the pak launchers export.
#define WPCTL_ENV "XDG_RUNTIME_DIR=/run PIPEWIRE_RUNTIME_DIR=/run "

static int cur_vol = -1; // 0-20 UI scale; -1 = not yet read

void InitSettings(void) {
	FILE* p = popen(WPCTL_ENV "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
	if (p) {
		float v = 0;
		if (fscanf(p, "Volume: %f", &v) == 1) cur_vol = (int)(v * 20.0f + 0.5f);
		pclose(p);
	}
	if (cur_vol < 0) cur_vol = 10;
	if (cur_vol > 20) cur_vol = 20;
}
void QuitSettings(void){}

int GetBrightness(void) { // 0-10 UI, read back from the panel itself
	int raw = -1;
	dispdbg_cmd("getbl", NULL);
	FILE* f = fopen(DISPDBG "info", "r");
	if (f) {
		if (fscanf(f, "%d", &raw) != 1) raw = -1;
		fclose(f);
	}
	if (raw < 0) return 5;
	int ui = (raw * 10 + BRIGHT_RAW_MAX / 2) / BRIGHT_RAW_MAX;
	return ui > 10 ? 10 : ui;
}
int GetVolume(void) { return cur_vol; }

void SetRawBrightness(int value) { // 0-255
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", value);
	dispdbg_cmd("setbl", buf);
}
void SetRawVolume(int value) { // 0-100 percent of the sink
	// Backgrounded: SetVolume runs on the EMULATION thread (the input hook), and a synchronous
	// system() forks-and-waits ~tens of ms — at ramp rate (~9 calls/s held) that starved the
	// audio ring audibly (Dan: "glitchy sounding" while adjusting, 2026-08-05). The shell exits
	// as soon as wpctl is spawned; last-writer-wins ordering is fine for a volume ramp.
	char cmd[160];
	snprintf(cmd, sizeof(cmd), WPCTL_ENV "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%% >/dev/null 2>&1 &", value);
	system(cmd);
}

void SetBrightness(int value) { // 0-10 UI
	if (value < 0) value = 0;
	if (value > 10) value = 10;
	int raw = value ? value * BRIGHT_RAW_MAX / 10 : BRIGHT_RAW_MIN;
	SetRawBrightness(raw);
}
void SetVolume(int value) { // 0-20 UI
	if (value < 0) value = 0;
	if (value > 20) value = 20;
	cur_vol = value;
	SetRawVolume(value * 5);
}

int GetJack(void) { return 0; }
void SetJack(int value) {}

int GetHDMI(void) { return 0; }
void SetHDMI(int value) {}

int GetMute(void) { return 0; }
