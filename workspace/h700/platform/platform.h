// h700 — Anbernic RG35XX Plus / RG35XX-H (Allwinner sun50iw9)
//
// Input codes below are the muOS keyboard layout (SDL scancodes), derived from muOS's own
// InputAutoCfg for this device: dpad=WASD, A=LSHIFT, B=LCTRL, Start=RETURN, and friends. They are
// the HOSTED-DEV mapping — what the muOS kernel's gpio-keys-polled device emits through SDL while
// we run inside muOS. Marked V0-UNVERIFIED until a live run confirms them; platform.c also logs
// every unmapped key it sees (MINUI_INPUT_DEBUG=1), so the first hands-on session corrects this
// file from its own log. A future MinUI Zero image owns the input driver and may re-map at will.

#ifndef PLATFORM_H
#define PLATFORM_H

///////////////////////////////

#include "sdl.h"

///////////////////////////////

#define BUTTON_UP		BUTTON_NA
#define BUTTON_DOWN		BUTTON_NA
#define BUTTON_LEFT		BUTTON_NA
#define BUTTON_RIGHT	BUTTON_NA

#define BUTTON_SELECT	BUTTON_NA
#define BUTTON_START	BUTTON_NA

#define BUTTON_A		BUTTON_NA
#define BUTTON_B		BUTTON_NA
#define BUTTON_X		BUTTON_NA
#define BUTTON_Y		BUTTON_NA

#define BUTTON_L1		BUTTON_NA
#define BUTTON_R1		BUTTON_NA
#define BUTTON_L2		BUTTON_NA
#define BUTTON_R2		BUTTON_NA
#define BUTTON_L3		BUTTON_NA
#define BUTTON_R3		BUTTON_NA

#define BUTTON_MENU		BUTTON_NA
#define BUTTON_MENU_ALT	BUTTON_NA
#define	BUTTON_POWER	BUTTON_NA
#define	BUTTON_PLUS		BUTTON_NA
#define	BUTTON_MINUS	BUTTON_NA

///////////////////////////////
// SDL scancodes for the muOS keyboard layout. V0-UNVERIFIED (see header note).

#define CODE_UP			26	// W
#define CODE_DOWN		22	// S
#define CODE_LEFT		4	// A
#define CODE_RIGHT		7	// D

#define CODE_SELECT		229	// RSHIFT
#define CODE_START		40	// RETURN

#define CODE_A			225	// LSHIFT
#define CODE_B			224	// LCTRL
#define CODE_X			44	// SPACE
#define CODE_Y			226	// LALT

#define CODE_L1			8	// E
#define CODE_R1			23	// T
#define CODE_L2			43	// TAB
#define CODE_R2			42	// BACKSPACE
#define CODE_L3			CODE_NA
#define CODE_R3			CODE_NA

#define CODE_MENU		41	// ESC
#define CODE_POWER		102

#define CODE_PLUS		128
#define CODE_MINUS		129

///////////////////////////////
// muOS-Keys is also js0; joystick numbering unverified until a live run.

#define JOY_UP			JOY_NA
#define JOY_DOWN		JOY_NA
#define JOY_LEFT		JOY_NA
#define JOY_RIGHT		JOY_NA

#define JOY_SELECT		JOY_NA
#define JOY_START		JOY_NA

#define JOY_A			JOY_NA
#define JOY_B			JOY_NA
#define JOY_X			JOY_NA
#define JOY_Y			JOY_NA

#define JOY_L1			JOY_NA
#define JOY_R1			JOY_NA
#define JOY_L2			JOY_NA
#define JOY_R2			JOY_NA
#define JOY_L3			JOY_NA
#define JOY_R3			JOY_NA

#define JOY_MENU		JOY_NA
#define JOY_POWER		JOY_NA
#define JOY_PLUS		JOY_NA
#define JOY_MINUS		JOY_NA

///////////////////////////////

#define AXIS_L2			AXIS_NA
#define AXIS_R2			AXIS_NA
#define AXIS_LX			0
#define AXIS_LY			1
#define AXIS_RX			2
#define AXIS_RY			3

///////////////////////////////

#define BTN_RESUME			BTN_X
#define BTN_SLEEP 			BTN_POWER
#define BTN_WAKE 			BTN_POWER
#define BTN_MOD_VOLUME 		BTN_NONE
#define BTN_MOD_BRIGHTNESS 	BTN_MENU
#define BTN_MOD_PLUS 		BTN_PLUS
#define BTN_MOD_MINUS 		BTN_MINUS

///////////////////////////////
// Panel: 640x480 MEASURED at 59.9777 Hz (see workspace/h700/README-BRINGUP.md). The render
// surface is RGB565 like every other platform; the fb is ARGB8888 and platform.c converts on flip.

// This platform presents through a HARDWARE scaler (the Allwinner DE aspect-fits the rendered
// crop to the panel), so render-space geometry is NOT what the panel shows. Shared code that
// reconstructs the on-screen game rect from renderer values (minarch Menu_scale backdrop) must
// instead ask PLAT_getGameRect, which mirrors the DE math. Only define on hw-scaler platforms.
#define PLAT_PRESENT_SCALER 1

// Platform folder name the wider pak scene publishes under for this hardware. MinUI/NextUI pak
// authors have shipped "rg35xxplus" paks for years; our internal name is h700 (upstream-merge
// cleanliness — internal identifiers are never renamed for branding). Honouring both means those
// paks install drop-in. See getEmuPath (utils.c) and hasEmu/Tools scan (minui.c).
#define PLATFORM_ALIAS "rg35xxplus"

// @2x UI assets, matching the miyoomini — the SAME 640x480 panel geometry. At 1 the whole
// launcher rendered half-size ("the resolution is off, everything is tiny", Dan 2026-08-05).
#define FIXED_SCALE		2
#define FIXED_WIDTH		640
#define FIXED_HEIGHT	480
#define FIXED_BPP		2
#define FIXED_DEPTH		(FIXED_BPP * 8)
#define FIXED_PITCH		(FIXED_WIDTH * FIXED_BPP)
#define FIXED_SIZE		(FIXED_PITCH * FIXED_HEIGHT)

#define H700_PANEL_HZ	59.9777	// measured 2026-08-04, 3x600-pan probe runs

///////////////////////////////

// SIX rows at the stock pill size. TRIED AND REJECTED (Dan, on-device 2026-08-13): shrinking
// PILL_SIZE to buy a 7th row like the Brick has. This panel divides badly, 640x480 at FIXED_SCALE 2
// gives exactly 480/(30*2) = 8.00 slots, all spent on header + 6 rows + footer, with none of the
// fractional slack the Brick's 768/(30*3) = 8.53 enjoys. PILL_SIZE 26 clipped the 7th row's
// highlight; 24 fit but shrank every pill, button and text baseline and simply looked worse.
// The row count is a consequence of panel geometry here, not a number to tune.
#define MAIN_ROW_COUNT 6
#define PADDING 10

///////////////////////////////

// HOSTED-DEV: muOS mounts its storage at /mnt/mmc (verified on-device; /mnt/sdcard does not
// exist there). Our own image will define its own mount and this changes with it.
#define SDCARD_PATH "/mnt/mmc"
#define MUTE_VOLUME_RAW 0

///////////////////////////////

#endif
