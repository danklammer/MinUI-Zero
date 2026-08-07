// h700 msettings — device settings (moved verbatim from platform.c when the makefile wiring
// landed; the shared minui/minarch makefiles link -lmsettings on every platform).
//
// On the MinUI Zero image we OWN the codec and panel: volume drives the ALSA 'digital volume'
// mixer directly (amixer, see SetRawVolume — pipewire is removed), brightness the Allwinner
// dispdbg debugfs. This library is standalone by design (no utils.c), so it carries its own
// tiny sysfs/mixer helpers.
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

// Volume drives the codec's 'digital volume' mixer directly via amixer (raw 0-63). MinUI Zero
// removes pipewire — a headless rootfs can't autolaunch its D-Bus session bus so it never starts,
// and it is 9.5MB of idle weight against the thesis — so the old wpctl-on-the-sink path is gone.
// We OWN the codec now, so poking amixer is the right lane, not the D33 hazard it was under
// wireplumber. This control's dB TLV is garbage (D33: reports tens of thousands of dB), so we
// IGNORE it and map the RAW integer, which is linear and higher=louder (MEASURED 2026-08-06 on
// this H700 codec: raw 40 = 63%, raw 10 = 16% — NOT reversed like the A133P Brick).
#define VOL_CTL "digital volume"
#define VOL_RAW_MAX 63

static int cur_vol = -1; // 0-20 UI scale; -1 = not yet read

void InitSettings(void) {
	// Read the codec's current raw level (set at boot by pipewire.sh's alsactl restore) back to UI.
	FILE* p = popen("amixer -c 0 sget '" VOL_CTL "' 2>/dev/null", "r");
	if (p) {
		char line[256];
		while (fgets(line, sizeof(line), p)) {
			int raw = 0;
			if (sscanf(line, " Mono: %d", &raw) == 1) {
				cur_vol = (raw * 20 + VOL_RAW_MAX / 2) / VOL_RAW_MAX;
				break;
			}
		}
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
void SetRawVolume(int value) { // 0-100 (SetVolume passes UI*5); MUTE_VOLUME_RAW (0) mutes
	// Backgrounded: SetVolume runs on the EMULATION thread (the input hook), and a synchronous
	// system() forks-and-waits ~tens of ms — at ramp rate (~9 calls/s held) that starved the
	// audio ring audibly (Dan: "glitchy sounding" while adjusting, 2026-08-05). The shell exits
	// as soon as amixer is spawned; last-writer-wins ordering is fine for a volume ramp.
	if (value < 0) value = 0;
	if (value > 100) value = 100;
	int raw = (value * VOL_RAW_MAX + 50) / 100; // 0-100 -> 0-63 raw, rounded
	char cmd[160];
	snprintf(cmd, sizeof(cmd), "amixer -c 0 sset '" VOL_CTL "' %d >/dev/null 2>&1 &", raw);
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
