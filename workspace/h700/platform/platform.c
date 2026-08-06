// h700 — Anbernic RG35XX Plus / RG35XX-H (Allwinner sun50iw9). V0: HOSTED-DEV platform layer.
//
// This version runs as a GUEST inside muOS (launched over ssh with muxfrontend stopped), which
// shapes several choices on purpose:
//   * present = raw fbdev (mmap + FBIOPAN_DISPLAY), the path panelprobe/hellofb proved on-device.
//     The DESTINATION present is the Allwinner DE layer API (/dev/disp, MyMinUI's h700 reference);
//     see README-BRINGUP.md. Do not let this v0 ossify.
//   * CPU/governor = LOG ONLY. A guest must not fight the host OS's governor. The schedutil
//     ceiling model ports when we own the image.
//   * powerOff = exit(0), never poweroff(2) — this is someone's running muOS session.
//   * function surface intentionally mirrors workspace/macos/platform/platform.c, the smallest
//     set proven to link against the shared api.c.
//
// Panel: 640x480 MEASURED 59.9777 Hz. fb is ARGB8888 (stride 2560), virtual 640x960 = 2 pages.
// Render surface is RGB565 like every platform; flip converts 565->XRGB (NEON on the wide path)
// while copying to the back page, then pans. The plain-C conversion measurably halved the game
// loop (29.98fps panrate receipt); the DE layer would do the conversion for free — still the
// destination present path.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <stdint.h>
#include "sunxi_display2.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "msettings.h"

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

#include "scaler.h"

///////////////////////////////
// msettings lives in ../libmsettings (guest stubs: muOS owns audio/brightness) — the shared
// minui/minarch makefiles link -lmsettings on every platform.

///////////////////////////////

// RAW EVDEV INPUT. muOS's SDL2 delivered zero keyboard events to us (live-tested: every button
// dead), so this platform reads the hardware nodes directly and overrides the weak PLAT_pollInput
// in api.c. Minimal evdev ABI declared by hand — <linux/input.h> defines BTN_* macros that collide
// with api.h's button names (the miyoomini port hit the same wall).
//
// The code table below is CANDIDATE mappings (standard gpio-keys arrows + standard gamepad BTN_*
// codes, Anbernic A-right/B-down orientation). Every event from an UNKNOWN code is logged once, so
// the first hands-on session corrects this table from its own log. That turns the capture problem
// into normal use.
struct raw_input_event { // struct input_event, aarch64 layout
	uint64_t tv_sec;
	uint64_t tv_usec;
	uint16_t type;   // EV_KEY=1, EV_ABS=3
	uint16_t code;
	int32_t  value;  // 1=down, 0=up, 2=autorepeat
};
#define EVDEV_COUNT 3
static int ev_fds[EVDEV_COUNT] = { -1, -1, -1 };
static const char* ev_paths[EVDEV_COUNT] = {
	"/dev/input/event0", // axp2202-pek: power button
	"/dev/input/event1", // muOS-Keys (gpio-keys-polled)
	"/dev/input/event2", // dierct-keys-polled (sic)
};

static SDL_Joystick *joystick;
void PLAT_initInput(void) {
	SDL_InitSubSystem(SDL_INIT_JOYSTICK);
	joystick = SDL_JoystickOpen(0);
	for (int i = 0; i < EVDEV_COUNT; i++) {
		ev_fds[i] = open(ev_paths[i], O_RDONLY | O_NONBLOCK);
		LOG_info("evdev: %s -> fd %d\n", ev_paths[i], ev_fds[i]);
	}
}
void PLAT_quitInput(void) {
	for (int i = 0; i < EVDEV_COUNT; i++) if (ev_fds[i] >= 0) close(ev_fds[i]);
	if (joystick) SDL_JoystickClose(joystick);
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

// code -> (btn, id). MEASURED 2026-08-05: counted-press session on the RG35XX Plus (each button
// pressed a distinct number of times, decoded from count + temporal order). The canonical evdev
// names are WRONG on this hardware — 315 "BTN_START" is R2, 314 "BTN_SELECT" is L2, 310 "BTN_TL"
// is Select — so this table is receipts, not <input-event-codes.h>. The MENU key emits 312 AND 354
// together; both map to BTN_MENU (same bit, harmless).
// The dpad is an ANALOG HAT (EV_ABS codes 16/17), handled separately in PLAT_pollInput.
static void ev_translate(uint16_t code, int* btn, int* id) {
	switch (code) {
	case 304: *btn = BTN_A;      *id = BTN_ID_A;      break;
	case 305: *btn = BTN_B;      *id = BTN_ID_B;      break;
	case 307: *btn = BTN_X;      *id = BTN_ID_X;      break;
	case 306: *btn = BTN_Y;      *id = BTN_ID_Y;      break;
	case 308: *btn = BTN_L1;     *id = BTN_ID_L1;     break;
	case 309: *btn = BTN_R1;     *id = BTN_ID_R1;     break;
	case 314: *btn = BTN_L2;     *id = BTN_ID_L2;     break;
	case 315: *btn = BTN_R2;     *id = BTN_ID_R2;     break;
	case 311: *btn = BTN_START;  *id = BTN_ID_START;  break;
	case 310: *btn = BTN_SELECT; *id = BTN_ID_SELECT; break;
	case 354: *btn = BTN_MENU;   *id = BTN_ID_MENU;   break;
	case 312: *btn = BTN_MENU;   *id = BTN_ID_MENU;   break; // MENU companion code
	case 116: *btn = BTN_POWER;  *id = BTN_ID_POWER;  break; // KEY_POWER (event0, axp2202-pek)
	case 115: *btn = BTN_PLUS;   *id = BTN_ID_PLUS;   break; // KEY_VOLUMEUP
	case 114: *btn = BTN_MINUS;  *id = BTN_ID_MINUS;  break; // KEY_VOLUMEDOWN
	default:  *btn = BTN_NONE;   *id = -1;            break;
	}
}

// The dpad hat: EV_ABS code 16 (X: -1 left / +1 right) and 17 (Y: -1 up / +1 down), 0 = released.
static void ev_hat(uint16_t code, int32_t value, uint32_t tick) {
	int neg_btn, neg_id, pos_btn, pos_id;
	if (code == 16) { neg_btn = BTN_DPAD_LEFT; neg_id = BTN_ID_DPAD_LEFT; pos_btn = BTN_DPAD_RIGHT; pos_id = BTN_ID_DPAD_RIGHT; }
	else if (code == 17) { neg_btn = BTN_DPAD_UP; neg_id = BTN_ID_DPAD_UP; pos_btn = BTN_DPAD_DOWN; pos_id = BTN_ID_DPAD_DOWN; }
	else return;
	// release whichever direction is no longer held, press whichever is
	if (value <= 0 && (pad.is_pressed & pos_btn)) { pad.is_pressed &= ~pos_btn; pad.just_repeated &= ~pos_btn; pad.just_released |= pos_btn; }
	if (value >= 0 && (pad.is_pressed & neg_btn)) { pad.is_pressed &= ~neg_btn; pad.just_repeated &= ~neg_btn; pad.just_released |= neg_btn; }
	if (value < 0 && !(pad.is_pressed & neg_btn)) { pad.is_pressed |= neg_btn; pad.just_pressed |= neg_btn; pad.just_repeated |= neg_btn; pad.repeat_at[neg_id] = tick + PAD_REPEAT_DELAY; }
	if (value > 0 && !(pad.is_pressed & pos_btn)) { pad.is_pressed |= pos_btn; pad.just_pressed |= pos_btn; pad.just_repeated |= pos_btn; pad.repeat_at[pos_id] = tick + PAD_REPEAT_DELAY; }
}

void PLAT_pollInput(void) {
	pad.just_pressed  = BTN_NONE;
	pad.just_released = BTN_NONE;
	pad.just_repeated = BTN_NONE;

	uint32_t tick = SDL_GetTicks();
	for (int i = 0; i < BTN_ID_COUNT; i++) {
		int btn = 1 << i;
		if ((pad.is_pressed & btn) && (tick >= pad.repeat_at[i])) {
			pad.just_repeated |= btn;
			pad.repeat_at[i] += PAD_REPEAT_INTERVAL;
		}
	}

	SDL_PumpEvents();
	SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT); // SDL is not our input source here

	// Log EVERY key-down once per distinct code — mapped or not — so a single press-everything
	// session yields the complete map. (The first version deduped by code&7 and codes silenced
	// each other; a press session came back with 2 of ~15 codes. Receipts require a real set.)
	struct raw_input_event ev;
	for (int i = 0; i < EVDEV_COUNT; i++) {
		if (ev_fds[i] < 0) continue;
		while (read(ev_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type == 3) { ev_hat(ev.code, ev.value, tick); continue; } // dpad hat
			if (ev.type != 1) continue;      // EV_KEY otherwise
			if (ev.value == 2) continue;     // autorepeat: we do our own
			int btn, id;
			ev_translate(ev.code, &btn, &id);
			if (btn == BTN_NONE) continue;
			// DEBOUNCE dual-code keys: MENU emits 354 AND 312 for one physical press. When the
			// pair splits across polls, an unguarded handler set just_pressed twice — so closing
			// the in-game menu instantly REOPENED it ("menu never closes", Dan 2026-08-05).
			// Only a real edge (state actually changing) may set the just_* flags.
			if (ev.value) {
				if (!(pad.is_pressed & btn)) {
					pad.is_pressed    |= btn;
					pad.just_pressed  |= btn;
					pad.just_repeated |= btn; // CONTRACT: the initial press IS a repeat (api.c:1967);
					                          // minui's list nav gates on justRepeated alone
					pad.repeat_at[id] = tick + PAD_REPEAT_DELAY;
				}
			}
			else {
				if (pad.is_pressed & btn) {
					pad.is_pressed    &= ~btn;
					pad.just_repeated &= ~btn;
					pad.just_released |= btn;
				}
			}
		}
	}

	// APPLY volume/brightness here: on shipping platforms KEYMON does this and the shared
	// PWR_update only draws the OSD ("keymon is catching input on the next frame"). This
	// platform has no keymon, so the volume keys showed an OSD that never changed anything
	// (found live 2026-08-05). just_repeated fires on the initial press AND while held, so
	// press-and-hold ramps for free. MENU held = brightness, matching the BTN_MOD_BRIGHTNESS
	// convention every other platform uses.
	int adj = pad.just_repeated & (BTN_PLUS | BTN_MINUS);
	if (adj) {
		int delta = (adj & BTN_PLUS) ? 1 : -1;
		if (pad.is_pressed & BTN_MENU) SetBrightness(GetBrightness() + delta); // clamps 0-10
		else SetVolume(GetVolume() + delta);                                   // clamps 0-20
	}
}

///////////////////////////////

// ---------------------------------------------------------------------------------------------
// DE-LAYER PRESENT (default; ZERO_H700_FB=1 falls back to the fbdev path below).
//
// The Allwinner Display Engine composes layers with a FREE hardware scaler: we hand it an
// RGB565 ION buffer at whatever size minarch rendered (crop) and a screen_win rect, and the
// silicon scales to fullscreen — no 565->XRGB convert, no CPU scale beyond minarch's integer
// prescale (sharp pixels + hardware finish, the same recipe as tg5040's Crisp).
//
// Plumbing adapted from MyMinUI's h700 port (Turro75/MyMinUI, workspace/h700/platform/) — the
// ION ioctl ABI and the 220-byte raw disp_layer_config2 packing are its hard-won discoveries;
// it uses the layer as a fullscreen page-flipper, we add the crop->screen_win scaling.
// FAILSAFE: a dead process leaves its layer configured OVER muOS's screen — tools/layerclean.c
// runs in every session.sh restore path.
// PROBED ABI (ionprobe3, RG35XX Plus muOS 4.9.170, 2026-08-05): this kernel wants the LEGACY
// ion API at aarch64-NATURAL layout — sizeof(alloc)=32 (u64 len/align + trailing pad), so the
// cmd is 0xC0204900, not MyMinUI's 32-bit-userspace 0xC0144900 (ENOTTY here). Heap bit 4 is
// the contiguous DMA heap (bit 0 = system, NOT contiguous — the display engine cannot scan it).
#define ION_IOC_ALLOC_A64 0xC0204900
#define ION_IOC_FREE_A64  0xC0044901
#define ION_IOC_SHARE_A64 0xC0084904
#define ION_HEAP_DMA_MASK (1u << 4)
struct ion_allocation_data_v1 {
	uint64_t len;
	uint64_t align;
	uint32_t heap_id_mask;
	uint32_t flags;
	int32_t handle;
	uint32_t pad;
};
struct ion_fd_data_v1 {
	int32_t handle;
	int32_t fd;
};
// The kernel's disp_layer_config2 ABI does not match the header struct (MyMinUI finding):
// SET_CONFIG2 consumes a raw 220-byte block. Pack the logical struct into it by dword index.
struct disp_layer_config2_raw {
	uint32_t dwords[55];
};
static void disp_pack(struct disp_layer_config2_raw* dst, const struct disp_layer_config2* src) {
	memset(dst, 0, sizeof(*dst));
	dst->dwords[0]  = (uint32_t)src->info.mode;
	dst->dwords[1]  = ((uint32_t)src->info.alpha_value << 16) |
	                  ((uint32_t)src->info.alpha_mode  << 8)  |
	                  ((uint32_t)src->info.zorder);
	dst->dwords[2]  = (uint32_t)src->info.screen_win.x;
	dst->dwords[3]  = (uint32_t)src->info.screen_win.y;
	dst->dwords[4]  = src->info.screen_win.width;
	dst->dwords[5]  = src->info.screen_win.height;
	dst->dwords[8]  = (uint32_t)src->info.fb.fd;
	dst->dwords[9]  = src->info.fb.size[0].width;
	dst->dwords[10] = src->info.fb.size[0].height;
	dst->dwords[18] = (uint32_t)src->info.fb.format;
	dst->dwords[19] = (uint32_t)src->info.fb.color_space;
	dst->dwords[23] = (uint32_t)(src->info.fb.crop.x >> 32);
	dst->dwords[24] = (uint32_t)(src->info.fb.crop.x & 0xFFFFFFFFULL);
	dst->dwords[25] = (uint32_t)(src->info.fb.crop.y >> 32);
	dst->dwords[26] = (uint32_t)(src->info.fb.crop.y & 0xFFFFFFFFULL);
	dst->dwords[27] = (uint32_t)(src->info.fb.crop.width  >> 32);
	dst->dwords[28] = (uint32_t)(src->info.fb.crop.width  & 0xFFFFFFFFULL);
	dst->dwords[29] = (uint32_t)(src->info.fb.crop.height >> 32);
	dst->dwords[30] = (uint32_t)(src->info.fb.crop.height & 0xFFFFFFFFULL);
	dst->dwords[52] = (uint32_t)src->enable;
	dst->dwords[53] = src->channel;
	dst->dwords[54] = src->layer_id;
}

static struct VID_Context {
	SDL_Surface* screen;   // RGB565 render target handed to the frontend
	GFX_Renderer* blit;

	int fdfb;
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	uint8_t* fbmmap;
	size_t page_bytes;
	int pages;
	int page;              // page we will draw the NEXT frame into

	int width;
	int height;
	int pitch;

	// DE-layer state
	int use_disp;          // 1 = layer present path active
	int dispfd;
	int ionfd;
	int ion_fds[2];        // dma-buf fds, one per page
	void* ionmmap[2];
	size_t ion_bytes;      // per-page capacity (panel W*H*2)
	struct disp_layer_config2 lcfg;
	struct disp_layer_config2_raw lraw;
	// the rect within the render surface that actually holds the image this frame.
	// minarch keeps the surface DEVICE-sized and centers the game (fit path, dst_x/dst_y);
	// the layer CROPS this rect and the hardware scales it to screen_win.
	int crop_x, crop_y, crop_w, crop_h;
} vid;

// aspect-fit w x h into the panel, centered — the hardware scaler's output window
static void disp_screen_win(int w, int h, struct disp_rect* win) {
	int pw = (int)vid.vinfo.xres, ph = (int)vid.vinfo.yres;
	double scale = (double)pw / w;
	if ((double)ph / h < scale) scale = (double)ph / h;
	int ow = (int)(w * scale + 0.5), oh = (int)(h * scale + 0.5);
	if (ow > pw) ow = pw;
	if (oh > ph) oh = ph;
	win->x = (pw - ow) / 2;
	win->y = (ph - oh) / 2;
	win->width = ow;
	win->height = oh;
}

// (re)shape the layer: the buffer is the full render surface (vid.width x vid.height); the
// layer CROPS (cx,cy,cw,ch) out of it and the hardware scales that rect to screen_win
static void disp_shape_rect(int cx, int cy, int cw, int ch) {
	memset(&vid.lcfg, 0, sizeof(vid.lcfg));
	vid.lcfg.info.mode = LAYER_MODE_BUFFER;
	vid.lcfg.info.zorder = 20;             // above the (frozen) muOS fb
	vid.lcfg.info.alpha_mode = 1;
	vid.lcfg.info.alpha_value = 255;
	disp_screen_win(cw, ch, &vid.lcfg.info.screen_win);
	vid.lcfg.info.fb.size[0].width = vid.width;
	vid.lcfg.info.fb.size[0].height = vid.height;
	vid.lcfg.info.fb.format = DISP_FORMAT_RGB_565;
	vid.lcfg.info.fb.color_space = DISP_BT601;
	vid.lcfg.info.fb.crop.x = (int64_t)cx << 32;
	vid.lcfg.info.fb.crop.y = (int64_t)cy << 32;
	vid.lcfg.info.fb.crop.width  = (uint64_t)cw << 32;
	vid.lcfg.info.fb.crop.height = (uint64_t)ch << 32;
	vid.lcfg.enable = 1;
	vid.lcfg.channel = 1;
	vid.lcfg.layer_id = 0;
	disp_pack(&vid.lraw, &vid.lcfg);
	vid.crop_x = cx; vid.crop_y = cy; vid.crop_w = cw; vid.crop_h = ch;
	LOG_info("disp: buf %dx%d crop %d,%d %dx%d -> win %d,%d %dx%d\n", vid.width, vid.height,
		cx, cy, cw, ch,
		vid.lcfg.info.screen_win.x, vid.lcfg.info.screen_win.y,
		vid.lcfg.info.screen_win.width, vid.lcfg.info.screen_win.height);
}

static int disp_commit(int page) {
	vid.lraw.dwords[8] = (uint32_t)vid.ion_fds[page];
	// unsigned long args: the sunxi disp_ioctl copies FOUR LONGS from userspace. uint32_t
	// worked for MyMinUI's 32-bit builds but truncated our 64-bit pointer (dmesg "para err").
	unsigned long args[4];
	args[0] = 0; args[1] = 1; args[2] = 0; args[3] = 0;
	if (ioctl(vid.dispfd, DISP_SHADOW_PROTECT, &args) < 0) return -1;
	args[0] = 0; args[1] = (unsigned long)&vid.lraw; args[2] = 1; args[3] = 0;
	int ret = ioctl(vid.dispfd, DISP_LAYER_SET_CONFIG2, &args);
	unsigned long cargs[4] = { 0, 4, 1, 0 };
	ioctl(vid.dispfd, DISP_HWC_COMMIT, &cargs);
	args[0] = 0; args[1] = 0; args[2] = 0; args[3] = 0;
	ioctl(vid.dispfd, DISP_SHADOW_PROTECT, &args);
	return ret;
}

static void disp_layer_off(void) {
	if (vid.dispfd < 0) return;
	struct disp_layer_config2_raw raw;
	memset(&raw, 0, sizeof(raw));
	raw.dwords[52] = 0; // enable = 0
	raw.dwords[53] = 1; // channel
	raw.dwords[54] = 0; // layer_id
	unsigned long args[4] = { 0, (unsigned long)&raw, 1, 0 };
	ioctl(vid.dispfd, DISP_LAYER_SET_CONFIG2, &args);
}

static int disp_open(void) {
	vid.ionfd = open("/dev/ion", O_RDWR);
	if (vid.ionfd < 0) { LOG_info("disp: no /dev/ion (%s)\n", strerror(errno)); return -1; }
	vid.dispfd = open("/dev/disp", O_RDWR);
	if (vid.dispfd < 0) { LOG_info("disp: no /dev/disp (%s)\n", strerror(errno)); close(vid.ionfd); return -1; }
	vid.ion_bytes = (size_t)vid.vinfo.xres * vid.vinfo.yres * 2; // RGB565 at full panel = max any frame needs
	for (int i = 0; i < 2; i++) {
		struct ion_allocation_data_v1 alloc = { .len = vid.ion_bytes, .align = 4096, .heap_id_mask = ION_HEAP_DMA_MASK, .flags = 0 };
		if (ioctl(vid.ionfd, ION_IOC_ALLOC_A64, &alloc) < 0) { LOG_info("disp: ION alloc %d failed (%s)\n", i, strerror(errno)); return -1; }
		struct ion_fd_data_v1 share = { .handle = alloc.handle, .fd = -1 };
		if (ioctl(vid.ionfd, ION_IOC_SHARE_A64, &share) < 0) { LOG_info("disp: ION share %d failed (%s)\n", i, strerror(errno)); return -1; }
		vid.ion_fds[i] = share.fd;
		vid.ionmmap[i] = mmap(NULL, vid.ion_bytes, PROT_READ|PROT_WRITE, MAP_SHARED, share.fd, 0);
		if (vid.ionmmap[i] == MAP_FAILED) { LOG_info("disp: ION mmap %d failed\n", i); return -1; }
		memset(vid.ionmmap[i], 0, vid.ion_bytes);
	}
	disp_layer_off(); // clear any stale layer a crashed predecessor left behind
	return 0;
}

SDL_Surface* PLAT_initVideo(void) {
	// SDL video first, for its input plumbing (keyboard events ride the video subsystem; the MMP
	// taught us buttons die without it). muOS's SDL2 is Mali-fbdev; if it refuses because we are
	// not the fb owner of record, keep going — we present through fbdev ourselves regardless.
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS | SDL_INIT_VIDEO) < 0) {
		LOG_info("SDL video init failed (%s) — input may be joystick-only\n", SDL_GetError());
		SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS);
	}

	vid.fdfb = open("/dev/fb0", O_RDWR);
	if (vid.fdfb < 0) { LOG_error("fb0: %s\n", strerror(errno)); return NULL; }
	ioctl(vid.fdfb, FBIOGET_VSCREENINFO, &vid.vinfo);
	ioctl(vid.fdfb, FBIOGET_FSCREENINFO, &vid.finfo);

	// Trust what the driver reports (recon: 640x480, virtual x960, 32bpp, stride 2560).
	vid.pages = vid.vinfo.yres ? (int)(vid.vinfo.yres_virtual / vid.vinfo.yres) : 1;
	if (vid.pages < 1) vid.pages = 1;
	if (vid.pages > 2) vid.pages = 2;
	vid.page_bytes = (size_t)vid.finfo.line_length * vid.vinfo.yres;
	vid.fbmmap = mmap(NULL, vid.page_bytes * vid.pages, PROT_READ|PROT_WRITE, MAP_SHARED, vid.fdfb, 0);
	if (vid.fbmmap == MAP_FAILED) { LOG_error("fb mmap: %s\n", strerror(errno)); return NULL; }
	LOG_info("fb: %ux%u %ubpp stride=%u pages=%d\n", vid.vinfo.xres, vid.vinfo.yres,
		vid.vinfo.bits_per_pixel, vid.finfo.line_length, vid.pages);

	vid.screen = SDL_CreateRGBSurface(SDL_SWSURFACE, FIXED_WIDTH, FIXED_HEIGHT, FIXED_DEPTH, RGBA_MASK_565);
	vid.width  = FIXED_WIDTH;
	vid.height = FIXED_HEIGHT;
	vid.pitch  = FIXED_PITCH;

	// DE-layer present unless opted out or unavailable; fbdev remains the fallback
	vid.dispfd = vid.ionfd = -1;
	vid.use_disp = 0;
	if (!getenv("ZERO_H700_FB")) {
		if (disp_open() == 0) {
			disp_shape_rect(0, 0, vid.width, vid.height);
			vid.use_disp = 1;
			LOG_info("present: DE layer (hardware scaler)\n");
		}
		else LOG_info("present: fbdev fallback\n");
	}

	// Draw into the page the panel is NOT scanning. The MMP's whole tearing saga came from
	// nothing tracking the front page; with 2 pages and a SYNCHRONOUS pan (blocks ~16.7ms,
	// measured) the alternation IS the ownership invariant — MyMinUI's model, safe by structure.
	int front = (vid.vinfo.yres && vid.vinfo.yoffset % vid.vinfo.yres == 0)
		? (int)(vid.vinfo.yoffset / vid.vinfo.yres) : 0;
	vid.page = (vid.pages > 1) ? !front : 0;

	return vid.screen;
}

// RGB565 -> XRGB8888 while copying into a fb page. NEON on the wide path: this runs per GAME
// frame (207k px for GBC at 3x), and the per-pixel C version was a measured contributor to the
// loop locking at 29.98fps (panrate receipt, 2026-08-05 — every frame's work exceeded one 16.7ms
// vblank, so every pan waited for the second one and audio starved to ~2/3 supply).
static void fb_blit565(int page) {
	uint16_t* src = (uint16_t*)vid.screen->pixels;
	int src_stride = vid.screen->pitch / 2;
	uint32_t* dst = (uint32_t*)(vid.fbmmap + (size_t)page * vid.page_bytes);
	int dst_stride = vid.finfo.line_length / 4;
	int w = vid.vinfo.xres < (unsigned)vid.screen->w ? vid.vinfo.xres : vid.screen->w;
	int h = vid.vinfo.yres < (unsigned)vid.screen->h ? vid.vinfo.yres : vid.screen->h;
	// center a smaller-than-panel surface (scaled game); borders were cleared at resize
	int ox = ((int)vid.vinfo.xres - w) / 2; if (ox < 0) ox = 0;
	int oy = ((int)vid.vinfo.yres - h) / 2; if (oy < 0) oy = 0;
	for (int y = 0; y < h; y++) {
		uint16_t* s = src + y * src_stride;
		uint32_t* d = dst + (y + oy) * dst_stride + ox;
		int x = 0;
#if defined(__aarch64__)
		for (; x + 8 <= w; x += 8) {
			uint16x8_t p = vld1q_u16(s + x);
			uint8x8_t r = vshrn_n_u16(vandq_u16(p, vdupq_n_u16(0xF800)), 8);            // r5<<3
			uint8x8_t g = vshrn_n_u16(vandq_u16(p, vdupq_n_u16(0x07E0)), 3);            // g6<<2
			uint8x8_t b = vmovn_u16(vshlq_n_u16(vandq_u16(p, vdupq_n_u16(0x001F)), 3)); // b5<<3
			r = vsri_n_u8(r, r, 5); // replicate top bits into the low ones: full 0-255 range
			g = vsri_n_u8(g, g, 6);
			b = vsri_n_u8(b, b, 5);
			uint8x8x4_t o = {{ b, g, r, vdup_n_u8(0xFF) }}; // little-endian XRGB: B,G,R,X
			vst4_u8((uint8_t*)(d + x), o);
		}
#endif
		for (; x < w; x++) {
			uint16_t p = s[x];
			uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
			d[x] = 0xFF000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
		}
	}
}

static void fb_pan(int page) {
	vid.vinfo.yoffset = vid.vinfo.yres * page;
	vid.vinfo.activate = FB_ACTIVATE_VBL;
	ioctl(vid.fdfb, FBIOPAN_DISPLAY, &vid.vinfo); // blocks ~16.7ms (measured): our vsync
}

void PLAT_quitVideo(void) {
	if (vid.use_disp) {
		disp_layer_off(); // a guest must hand the display back — a leaked layer covers muOS
		for (int i = 0; i < 2; i++) {
			if (vid.ionmmap[i] && vid.ionmmap[i] != MAP_FAILED) munmap(vid.ionmmap[i], vid.ion_bytes);
			if (vid.ion_fds[i] > 0) close(vid.ion_fds[i]);
		}
		if (vid.dispfd >= 0) close(vid.dispfd);
		if (vid.ionfd >= 0) close(vid.ionfd);
	}
	if (vid.screen) SDL_FreeSurface(vid.screen);
	if (vid.fbmmap && vid.fbmmap != MAP_FAILED) munmap(vid.fbmmap, vid.page_bytes * vid.pages);
	if (vid.fdfb >= 0) close(vid.fdfb);
	SDL_Quit();
}

void PLAT_clearVideo(SDL_Surface* screen) {
	SDL_FillRect(screen, NULL, 0);
}
void PLAT_clearAll(void) {
	PLAT_clearVideo(vid.screen);
	if (vid.use_disp) {
		// same live-scanout rule as fbdev: only the back page is ours to scrub
		memset(vid.ionmmap[vid.page], 0, vid.ion_bytes);
		return;
	}
	// Zero only the page WE own next; the pan in the next flip retires the other. Never memset
	// the whole mmap — that writes into live scanout (the MMP's PLAT_clearAll lesson).
	if (vid.fbmmap && vid.fbmmap != MAP_FAILED)
		memset(vid.fbmmap + (size_t)vid.page * vid.page_bytes, 0, vid.page_bytes);
}

void PLAT_setVsync(int vsync) {
	// the synchronous pan is the vsync; nothing to configure in v0
}

SDL_Surface* PLAT_resizeVideo(int w, int h, int p) {
	// minarch resizes the render surface to the SCALED game size (e.g. 480x432 for GBC at 3x) and
	// derives its row pitch from it. Ignoring this was the shear-and-repeat glitch: minarch wrote
	// 480-wide rows into a 640-wide surface. Recreate the surface at the requested size; the flip
	// centers it on the panel.
	if (w == vid.width && h == vid.height && p == vid.pitch) return vid.screen;
	LOG_info("resizeVideo(%d,%d,%d)\n", w, h, p);
	if (vid.screen) SDL_FreeSurface(vid.screen);
	vid.screen = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, FIXED_DEPTH, RGBA_MASK_565);
	vid.width = w; vid.height = h; vid.pitch = vid.screen->pitch;
	if (vid.use_disp) {
		// clamp to the panel: the ION pages hold at most panel-sized RGB565 (matches what
		// minarch ever requests on a 640x480 platform; a bigger ask would be a bug upstream)
		if (w > (int)vid.vinfo.xres || h > (int)vid.vinfo.yres)
			LOG_info("resizeVideo %dx%d exceeds panel — refusing layer reshape\n", w, h);
		else {
			disp_shape_rect(0, 0, w, h);
			memset(vid.ionmmap[0], 0, vid.ion_bytes);
			memset(vid.ionmmap[1], 0, vid.ion_bytes);
		}
	}
	// geometry changed: scrub both fb pages so old borders do not linger (fbdev path)
	else if (vid.fbmmap && vid.fbmmap != MAP_FAILED) memset(vid.fbmmap, 0, vid.page_bytes * vid.pages);
	return vid.screen;
}

void PLAT_setVideoScaleClip(int x, int y, int width, int height) {}
void PLAT_setNearestNeighbor(int enabled) {}
void PLAT_setSharpness(int sharpness) {}

void PLAT_vsync(int remaining) {
	// The pan blocks a full refresh already; only burn what pacing asks for beyond that.
	if (remaining > 0) SDL_Delay(remaining);
}

// Integer scaling + centering, the MMP pattern exactly: minarch computes the scale and dst rect;
// the platform honors both. (v0 ignored them — the first game ran 1:1 in the corner.)
scaler_t PLAT_getScaler(GFX_Renderer* renderer) {
	switch (renderer->scale) {
		case 6:  return scale6x6_c16;
		case 5:  return scale5x5_c16;
		case 4:  return scale4x4_c16;
		case 3:  return scale3x3_c16;
		case 2:  return scale2x2_c16;
		default: return scale1x1_c16;
	}
}

void PLAT_blitRenderer(GFX_Renderer* renderer) {
	vid.blit = renderer;
	if (vid.use_disp) {
		// scale into the CACHED SDL surface exactly like the fbdev path — scaling straight
		// into the write-combined ION mapping cost +24% of a core (54% vs 30%, measured:
		// scale3x's per-pixel writes defeat write combining). The flip then streams just the
		// crop rect into the ION page as one sequential copy, which WC handles ideally.
		void* dst = renderer->dst + (renderer->dst_y * renderer->dst_p) + (renderer->dst_x * FIXED_BPP);
		((scaler_t)renderer->blit)(renderer->src, dst,
			renderer->src_w, renderer->src_h, renderer->src_p,
			renderer->dst_w, renderer->dst_h, renderer->dst_p);
		// the true drawn size is src * integer scale — on the fit path minarch leaves
		// dst_w/dst_h at DEVICE size (observed live: crop was 640x480 while the game was
		// a 3x 480x432), and only dst_x/dst_y locate the image
		int cw = renderer->scale >= 1 ? renderer->src_w * renderer->scale : renderer->dst_w;
		int ch = renderer->scale >= 1 ? renderer->src_h * renderer->scale : renderer->dst_h;
		if (renderer->dst_x != vid.crop_x || renderer->dst_y != vid.crop_y ||
		    cw != vid.crop_w || ch != vid.crop_h)
			disp_shape_rect(renderer->dst_x, renderer->dst_y, cw, ch);
		return;
	}
	void* dst = renderer->dst + (renderer->dst_y * renderer->dst_p) + (renderer->dst_x * FIXED_BPP);
	((scaler_t)renderer->blit)(renderer->src, dst,
		renderer->src_w, renderer->src_h, renderer->src_p,
		renderer->dst_w, renderer->dst_h, renderer->dst_p);
}

void PLAT_flip(SDL_Surface* IGNORED, int ignored) {
	int fullframe = vid.blit != NULL; // renderer path = the game redraws its whole rect every frame
	if (vid.use_disp) {
		{
			// stream the live rect from the cached surface into the back ION page.
			// UI = full surface; game = just the crop rect (416KB for GBC at 3x, one
			// sequential pass — the write-combine-friendly shape).
			int cx = fullframe ? vid.crop_x : 0;
			int cy = fullframe ? vid.crop_y : 0;
			int cw = fullframe ? vid.crop_w : vid.width;
			int ch = fullframe ? vid.crop_h : vid.height;
			uint8_t* srcp = (uint8_t*)vid.screen->pixels + cy * vid.screen->pitch + cx * FIXED_BPP;
			int rowbytes = vid.width * FIXED_BPP;
			uint8_t* dstp = (uint8_t*)vid.ionmmap[vid.page] + cy * rowbytes + cx * FIXED_BPP;
			int copybytes = cw * FIXED_BPP;
			for (int y = 0; y < ch; y++)
				memcpy(dstp + y * rowbytes, srcp + y * vid.screen->pitch, copybytes);
		}
		if (!fullframe) {
			if (vid.crop_x || vid.crop_y || vid.crop_w != vid.width || vid.crop_h != vid.height)
				disp_shape_rect(0, 0, vid.width, vid.height); // UI owns the whole surface
		}
		disp_commit(vid.page);
		int shown = vid.page;
		vid.page = !vid.page;
		if (!fullframe) {
			// keep pages coherent for partial UI redraws (same reasoning as the fbdev path)
			memcpy(vid.ionmmap[vid.page], vid.ionmmap[shown], (size_t)vid.height * vid.width * FIXED_BPP);
		}
		vid.blit = NULL;
		return;
	}
	fb_blit565(vid.page);
	fb_pan(vid.page);                        // synchronous: returns with the page on its way out
	if (vid.pages > 1) {
		int shown = vid.page;
		vid.page = !vid.page;
		// Keep the pages COHERENT for the UI: minui only redraws dirty regions, so with strict
		// alternation each page misses the other page updates and the menu ghosts between two
		// half-states (seen in fb captures as composite frames). Games are exempt: they blit the
		// full game rect every frame (borders were scrubbed on both pages at resize), and this
		// 1.2MB copy per frame was part of the work that locked the loop at half rate.
		if (!fullframe)
			memcpy(vid.fbmmap + (size_t)vid.page * vid.page_bytes,
			       vid.fbmmap + (size_t)shown * vid.page_bytes, vid.page_bytes);
	}
	vid.blit = NULL;
}

///////////////////////////////

#define OVERLAY_WIDTH PILL_SIZE
#define OVERLAY_HEIGHT PILL_SIZE
#define OVERLAY_BPP 4
#define OVERLAY_DEPTH 16
#define OVERLAY_PITCH (OVERLAY_WIDTH * OVERLAY_BPP)
#define OVERLAY_RGBA_MASK 0x00ff0000,0x0000ff00,0x000000ff,0xff000000
static struct OVL_Context {
	SDL_Surface* overlay;
} ovl;

SDL_Surface* PLAT_initOverlay(void) {
	ovl.overlay = SDL_CreateRGBSurface(SDL_SWSURFACE, SCALE2(OVERLAY_WIDTH,OVERLAY_HEIGHT),OVERLAY_DEPTH,OVERLAY_RGBA_MASK);
	return ovl.overlay;
}
void PLAT_quitOverlay(void) {
	if (ovl.overlay) SDL_FreeSurface(ovl.overlay);
}
void PLAT_enableOverlay(int enable) {}

///////////////////////////////
// AXP2202 — the Brick's exact PMIC, verified on-device 2026-08-04 (identical sysfs paths).

static int online = 0;
void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	*is_charging = getInt("/sys/class/power_supply/axp2202-usb/online");

	int i = getInt("/sys/class/power_supply/axp2202-battery/capacity");
	// worry less about battery and more about the game you're playing
	     if (i>80) *charge = 100;
	else if (i>60) *charge =  80;
	else if (i>40) *charge =  60;
	else if (i>20) *charge =  40;
	else if (i>10) *charge =  20;
	else           *charge =  10;
}

// Charging inhibits autosleep (same behaviour as the Brick). Crucial in hosted dev: the 30s
// autosleep was ending input sessions before anyone pressed a button.
int PLAT_isToppingUp(void) {
	// HOSTED-DEV: report always-topping-up so autosleep never fires. Twice tonight the 30s
	// autosleep ate an input session (screen slept, the wake tap fell into the power-off flow).
	// The real charging read returns with the image build, where sleep policy matters.
	return 1;
}

void PLAT_enableBacklight(int enable) {
	// TODO: find the true backlight node on this device; guarded so a wrong path is a no-op
	if (exists("/sys/class/backlight/backlight/bl_power"))
		putInt("/sys/class/backlight/backlight/bl_power", enable ? 0 : 4);
}

void PLAT_powerOff(void) {
	// GUEST MODE: we are a visitor inside muOS. Exiting hands the console back to the host
	// frontend; powering off the host from a dev harness would be hostile.
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();
	exit(0);
}

///////////////////////////////

void PLAT_setCPUSpeed(int speed) {
	// GUEST MODE: log intent, touch nothing. muOS owns the governor while it hosts us. The
	// schedutil ceiling model (tg5040) ports when we own the image — schedutil is confirmed
	// available on this SoC.
	LOG_info("PLAT_setCPUSpeed(%d) — guest mode, not applied\n", speed);
}

void PLAT_setCPUMaxFreq(int khz) {
	// The governor re-asserts its ceiling ~1/s; logging every call wrote thousands of
	// identical lines per session to the SD. Log decisions, not the clock.
	static int last_khz = -1;
	if (khz == last_khz) return;
	last_khz = khz;
	LOG_info("PLAT_setCPUMaxFreq: %d kHz — guest mode, not applied\n", khz);
}

void PLAT_setRumble(int strength) {
	// no rumble hardware reported in recon
}

int PLAT_pickSampleRate(int requested, int max) {
	// Open the device at the pipewire graph/hardware rate (48000, MEASURED via
	// /proc/asound/card0/pcm0p/sub0/hw_params + pw-top 2026-08-05) instead of the core's
	// native rate. At 32768 the alsa-pipewire plugin resampled us into the 48k graph in ITS
	// clock domain — the hw output node logged 118 xruns (pw-top ERR) while our client node
	// showed 0, i.e. the glitches happened downstream of a stream we fed correctly, and our
	// producer blocked on the plugin's drain clock instead of the real DAC (audibly choppy
	// AND slow, RG35XX Plus 2026-08-05). At 48000 pipewire mixes pass-through and OUR
	// resampler (which the rate-match/DRC machinery adjusts) owns the conversion.
	return MIN(48000, max);
}

// minarch-only surface, v0 stubs
void PLAT_setEffect(int effect) {} // no scanline/DMG effects on the fbdev path yet
void PLAT_setDebugOverlay(uint16_t* top, uint16_t* bottom, int w, int h, int stride) {} // HUD later
void PLAT_getGameRect(int* x, int* y, int* w, int* h) {
	// v0 presents full-screen 640x480; refine when scaling modes land
	if (x) *x = 0; if (y) *y = 0; if (w) *w = FIXED_WIDTH; if (h) *h = FIXED_HEIGHT;
}

char* PLAT_getModel(void) {
	// TODO: distinguish RG35XX Plus vs H (near-twins; likely a DT compatible string or a key count)
	return "Anbernic RG35XX";
}

int PLAT_isOnline(void) {
	return online;
}
