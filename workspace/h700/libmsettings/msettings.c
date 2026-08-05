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

#define BACKLIGHT_PATH "/sys/class/backlight/backlight/brightness"

static int getInt(const char* path) {
	int i = 0;
	FILE* file = fopen(path, "r");
	if (file != NULL) {
		if (fscanf(file, "%i", &i) != 1) i = 0;
		fclose(file);
	}
	return i;
}
static void putInt(const char* path, int value) {
	FILE* file = fopen(path, "w");
	if (file != NULL) {
		fprintf(file, "%d", value);
		fclose(file);
	}
}

void InitSettings(void){}
void QuitSettings(void){}

int GetBrightness(void) { return getInt(BACKLIGHT_PATH); }
int GetVolume(void) { return 0; }

void SetRawBrightness(int value) { putInt(BACKLIGHT_PATH, value); }
void SetRawVolume(int value){}

void SetBrightness(int value) { SetRawBrightness(value * 10); } // 0-10 UI -> 0-100ish raw; TODO: verify range
void SetVolume(int value) {}

int GetJack(void) { return 0; }
void SetJack(int value) {}

int GetHDMI(void) { return 0; }
void SetHDMI(int value) {}

int GetMute(void) { return 0; }
