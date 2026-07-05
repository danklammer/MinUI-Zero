// tg5040
#include <stdio.h>
#include <stdlib.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <msettings.h>

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

#include "scaler.h"

int is_brick = 0;

///////////////////////////////

static SDL_Joystick *joystick;
void PLAT_initInput(void) {
	SDL_InitSubSystem(SDL_INIT_JOYSTICK);
	joystick = SDL_JoystickOpen(0);
}
void PLAT_quitInput(void) {
	SDL_JoystickClose(joystick);
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

///////////////////////////////

static struct VID_Context {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	SDL_Texture* target;
	SDL_Texture* effect;
	SDL_Surface* buffer;
	SDL_Surface* screen;
	
	GFX_Renderer* blit; // yeesh
	
	int width;
	int height;
	int pitch;
	int sharpness;
} vid;

static int device_width;
static int device_height;
static int device_pitch;

// --- MinUI Zero: optional GPU-dark software present straight to /dev/fb0 (env ZERO_FB_PRESENT=1) ---
// The Brick's display is a plain framebuffer (32bpp little-endian XRGB8888 => BGRA bytes). For the
// MENU (native 1024x768, no scaling) we can convert+copy vid.screen (RGB565) directly to fb0 and skip
// GLES entirely, so the PowerVR GPU can suspend while browsing. Games keep the GLES scaling path.
static int   fb_present = 0; // GPU-dark software present for the MENU (env ZERO_FB_PRESENT)
static int   fb_game    = 0; // GPU-dark software present for GAMES (env ZERO_FB_GAME) — light systems
static int   fb_fd      = -1;
static uint8_t* fb_mem  = NULL;
static struct fb_var_screeninfo fb_vinfo;
static struct fb_fix_screeninfo fb_finfo;
static int   fbg_xmap[1280]; // src-x sample index per dst column (recomputed on geometry change)
static uint32_t fbg_rowbuf[1280]; // one h-scaled+converted dst row, reused across identical dst rows
static int   fbg_last_dw=-1, fbg_last_dh=-1, fbg_last_dx=-1, fbg_last_dy=-1;

static void PLAT_flipFB(void) {
	// RGB565 -> XRGB8888 (BGRA in memory), native res, into the currently-visible fb page.
	uint16_t* src = (uint16_t*)vid.screen->pixels;
	int sp = vid.screen->pitch / 2;                 // src stride in uint16
	int dp = fb_finfo.line_length / 4;              // dst stride in uint32
	uint32_t* dst = (uint32_t*)(fb_mem + (long)fb_vinfo.yoffset * fb_finfo.line_length);
	int w = vid.width, h = vid.height;
	for (int y=0; y<h; y++) {
		uint16_t* s = src + y*sp;
		uint32_t* d = dst + y*dp;
		for (int x=0; x<w; x++) {
			uint16_t p = s[x];
			uint32_t r = (p>>11)&0x1f, g = (p>>5)&0x3f, b = p&0x1f;
			d[x] = 0xff000000u | ((r<<3|r>>2)<<16) | ((g<<2|g>>4)<<8) | (b<<3|b>>2);
		}
	}
}

static void PLAT_flipFB_game(void) {
	// GPU-dark GAME present: software nearest-neighbor scale of the native core frame straight to
	// /dev/fb0 (page 0), replacing the GLES RenderCopy — so the PowerVR domain can suspend during
	// light-system play. Mirrors PLAT_flip's aspect/native/fullscreen dst geometry. Effects/overlays
	// (scanlines, DMG grid) are GLES-only and skipped here.
	if (!vid.blit) return;
	int sx = vid.blit->src_x, sy = vid.blit->src_y;
	int sw = vid.blit->src_w, sh = vid.blit->src_h;
	int dw, dh;
	if (vid.blit->aspect==0) { dw = sw * vid.blit->scale; dh = sh * vid.blit->scale; } // native/cropped
	else if (vid.blit->aspect>0) {                                                     // aspect fit
		dh = device_height; dw = dh * vid.blit->aspect;
		if (dw>device_width) { dw = device_width; dh = dw / vid.blit->aspect; }
	}
	else { dw = device_width; dh = device_height; }                                    // fullscreen
	if (dw>device_width)  dw = device_width;
	if (dh>device_height) dh = device_height;
	int dx = (device_width - dw) / 2;
	int dy = (device_height - dh) / 2;

	int fbstride = fb_finfo.line_length / 4;
	uint32_t* fb = (uint32_t*)fb_mem; // page 0

	// first frame / geometry change: pan to page 0, black the whole page (letterbox bars), rebuild x-map
	if (dw!=fbg_last_dw || dh!=fbg_last_dh || dx!=fbg_last_dx || dy!=fbg_last_dy) {
		fb_vinfo.yoffset = 0;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_vinfo);
		memset(fb_mem, 0, (long)fb_vinfo.yres * fb_finfo.line_length);
		for (int x=0; x<dw; x++) fbg_xmap[x] = sx + (x * sw) / dw;
		fbg_last_dw=dw; fbg_last_dh=dh; fbg_last_dx=dx; fbg_last_dy=dy;
	}

	uint16_t* src = (uint16_t*)vid.blit->src;
	int src_p16 = vid.blit->src_p / 2;
	int prev_syy = -1;
	int rowbytes = dw * 4;
	for (int y=0; y<dh; y++) {
		int syy = sy + (y * sh) / dh;
		if (syy != prev_syy) {
			// this dst row maps to a new source row — h-scale + RGB565->XRGB8888 into the row buffer once
			uint16_t* srow = src + (long)syy * src_p16;
			for (int x=0; x<dw; x++) {
				uint16_t p = srow[fbg_xmap[x]];
				uint32_t r=(p>>11)&0x1f, g=(p>>5)&0x3f, b=p&0x1f;
				fbg_rowbuf[x] = 0xff000000u | ((r<<3|r>>2)<<16) | ((g<<2|g>>4)<<8) | (b<<3|b>>2);
			}
			prev_syy = syy;
		}
		// blast the cached row to fb0 (memcpy is NEON-optimized; identical dst rows reuse the buffer)
		memcpy(fb + (long)(dy + y) * fbstride + dx, fbg_rowbuf, rowbytes);
	}
}

SDL_Surface* PLAT_initVideo(void) {
	char* device = getenv("DEVICE");
	is_brick = exactMatch("brick", device);
	// LOG_info("DEVICE: %s is_brick: %i\n", device, is_brick);
	
	SDL_InitSubSystem(SDL_INIT_VIDEO);
	SDL_ShowCursor(0);
	
	// SDL_version compiled;
	// SDL_version linked;
	// SDL_VERSION(&compiled);
	// SDL_GetVersion(&linked);
	// LOG_info("Compiled SDL version %d.%d.%d ...\n", compiled.major, compiled.minor, compiled.patch);
	// LOG_info("Linked SDL version %d.%d.%d.\n", linked.major, linked.minor, linked.patch);
	//
	// LOG_info("Available video drivers:\n");
	// for (int i=0; i<SDL_GetNumVideoDrivers(); i++) {
	// 	LOG_info("- %s\n", SDL_GetVideoDriver(i));
	// }
	// LOG_info("Current video driver: %s\n", SDL_GetCurrentVideoDriver());
	//
	// LOG_info("Available render drivers:\n");
	// for (int i=0; i<SDL_GetNumRenderDrivers(); i++) {
	// 	SDL_RendererInfo info;
	// 	SDL_GetRenderDriverInfo(i,&info);
	// 	LOG_info("- %s\n", info.name);
	// }
	//
	// LOG_info("Available display modes:\n");
	// SDL_DisplayMode mode;
	// for (int i=0; i<SDL_GetNumDisplayModes(0); i++) {
	// 	SDL_GetDisplayMode(0, i, &mode);
	// 	LOG_info("- %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));
	// }
	// SDL_GetCurrentDisplayMode(0, &mode);
	// LOG_info("Current display mode: %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));
	
	int w = FIXED_WIDTH;
	int h = FIXED_HEIGHT;
	int p = FIXED_PITCH;
	vid.window   = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w,h, SDL_WINDOW_SHOWN);
	vid.renderer = SDL_CreateRenderer(vid.window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
	
	// SDL_RendererInfo info;
	// SDL_GetRendererInfo(vid.renderer, &info);
	// LOG_info("Current render driver: %s\n", info.name);
	
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"0");
	vid.texture = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w,h);
	vid.target	= NULL; // only needed for non-native sizes
	
	// SDL_SetTextureScaleMode(vid.texture, SDL_ScaleModeNearest);
	
	vid.buffer	= SDL_CreateRGBSurfaceFrom(NULL, w,h, FIXED_DEPTH, p, RGBA_MASK_565);
	vid.screen	= SDL_CreateRGBSurface(SDL_SWSURFACE, w,h, FIXED_DEPTH, RGBA_MASK_565);
	vid.width	= w;
	vid.height	= h;
	vid.pitch	= p;
	
	device_width	= w;
	device_height	= h;
	device_pitch	= p;
	
	vid.sharpness = SHARPNESS_SOFT;

	fb_present = getenv("ZERO_FB_PRESENT") && getenv("ZERO_FB_PRESENT")[0]=='1';
	fb_game    = getenv("ZERO_FB_GAME")    && getenv("ZERO_FB_GAME")[0]=='1';
	if (fb_present || fb_game) {
		fb_fd = open("/dev/fb0", O_RDWR);
		if (fb_fd>=0 && ioctl(fb_fd,FBIOGET_FSCREENINFO,&fb_finfo)==0 && ioctl(fb_fd,FBIOGET_VSCREENINFO,&fb_vinfo)==0) {
			fb_mem = mmap(0, fb_finfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
			if (fb_mem==MAP_FAILED) fb_mem = NULL;
		}
		if (!fb_mem) { fb_present = 0; fb_game = 0; if (fb_fd>=0) close(fb_fd); fb_fd = -1; }
		LOG_info("ZERO_FB: fb=%ux%u menu=%s game=%s (stride=%u yoff=%u)\n", fb_vinfo.xres, fb_vinfo.yres,
			fb_present?"on":"off", fb_game?"on":"off", fb_finfo.line_length, fb_vinfo.yoffset);
	}

	return vid.screen;
}

static void clearVideo(void) {
	for (int i=0; i<3; i++) {
		SDL_RenderClear(vid.renderer);
		SDL_FillRect(vid.screen, NULL, 0);
		
		SDL_LockTexture(vid.texture,NULL,&vid.buffer->pixels,&vid.buffer->pitch);
		SDL_FillRect(vid.buffer, NULL, 0);
		SDL_UnlockTexture(vid.texture);
		SDL_RenderCopy(vid.renderer, vid.texture, NULL, NULL);
		
		SDL_RenderPresent(vid.renderer);
	}
}

void PLAT_quitVideo(void) {
	clearVideo();

	SDL_FreeSurface(vid.screen);
	SDL_FreeSurface(vid.buffer);
	if (vid.target) SDL_DestroyTexture(vid.target);
	if (vid.effect) SDL_DestroyTexture(vid.effect);
	SDL_DestroyTexture(vid.texture);
	SDL_DestroyRenderer(vid.renderer);
	SDL_DestroyWindow(vid.window);

	SDL_Quit();
	system("cat /dev/zero > /dev/fb0 2>/dev/null");
}

void PLAT_clearVideo(SDL_Surface* screen) {
	SDL_FillRect(screen, NULL, 0); // TODO: revisit
}
void PLAT_clearAll(void) {
	PLAT_clearVideo(vid.screen); // TODO: revist
	SDL_RenderClear(vid.renderer);
}

void PLAT_setVsync(int vsync) {
	
}

static int hard_scale = 4; // TODO: base src size, eg. 160x144 can be 4

static void resizeVideo(int w, int h, int p) {
	if (w==vid.width && h==vid.height && p==vid.pitch) return;
	
	// TODO: minarch disables crisp (and nn upscale before linear downscale) when native, is this true?
	
	if (w>=device_width && h>=device_height) hard_scale = 1;
	// else if (h>=160) hard_scale = 2; // limits gba and up to 2x (seems sufficient for 640x480)
	else hard_scale = 4;

	LOG_info("resizeVideo(%i,%i,%i) hard_scale: %i crisp: %i\n",w,h,p, hard_scale,vid.sharpness==SHARPNESS_CRISP);

	SDL_FreeSurface(vid.buffer);
	SDL_DestroyTexture(vid.texture);
	if (vid.target) SDL_DestroyTexture(vid.target);
	
	SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, vid.sharpness==SHARPNESS_SOFT?"1":"0", SDL_HINT_OVERRIDE);
	vid.texture = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w,h);
	
	if (vid.sharpness==SHARPNESS_CRISP) {
		SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "1", SDL_HINT_OVERRIDE);
		vid.target = SDL_CreateTexture(vid.renderer,SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_TARGET, w * hard_scale,h * hard_scale);
	}
	else {
		vid.target = NULL;
	}
	
	vid.buffer	= SDL_CreateRGBSurfaceFrom(NULL, w,h, FIXED_DEPTH, p, RGBA_MASK_565);

	vid.width	= w;
	vid.height	= h;
	vid.pitch	= p;
}

SDL_Surface* PLAT_resizeVideo(int w, int h, int p) {
	resizeVideo(w,h,p);
	return vid.screen;
}

void PLAT_setVideoScaleClip(int x, int y, int width, int height) {
	// buh
}
void PLAT_setNearestNeighbor(int enabled) {
	// always enabled?
}
void PLAT_setSharpness(int sharpness) {
	if (vid.sharpness==sharpness) return;
	int p = vid.pitch;
	vid.pitch = 0;
	vid.sharpness = sharpness;
	resizeVideo(vid.width,vid.height,p);
}

static struct FX_Context {
	int scale;
	int type;
	int color;
	int next_scale;
	int next_type;
	int next_color;
	int live_type;
} effect = {
	.scale = 1,
	.next_scale = 1,
	.type = EFFECT_NONE,
	.next_type = EFFECT_NONE,
	.live_type = EFFECT_NONE,
	.color = 0,
	.next_color = 0,
};
static void rgb565_to_rgb888(uint32_t rgb565, uint8_t *r, uint8_t *g, uint8_t *b) {
    // Extract the red component (5 bits)
    uint8_t red = (rgb565 >> 11) & 0x1F;
    // Extract the green component (6 bits)
    uint8_t green = (rgb565 >> 5) & 0x3F;
    // Extract the blue component (5 bits)
    uint8_t blue = rgb565 & 0x1F;

    // Scale the values to 8-bit range
    *r = (red << 3) | (red >> 2);
    *g = (green << 2) | (green >> 4);
    *b = (blue << 3) | (blue >> 2);
}
static void updateEffect(void) {
	if (effect.next_scale==effect.scale && effect.next_type==effect.type && effect.next_color==effect.color) return; // unchanged
	
	int live_scale = effect.scale;
	int live_color = effect.color;
	effect.scale = effect.next_scale;
	effect.type = effect.next_type;
	effect.color = effect.next_color;
	
	if (effect.type==EFFECT_NONE) return; // disabled
	if (effect.type==effect.live_type && effect.scale==live_scale && effect.color==live_color) return; // already loaded
	
	char* effect_path;
	int opacity = 128; // 1 - 1/2 = 50%
	if (effect.type==EFFECT_LINE) {
		if (effect.scale<3) {
			effect_path = RES_PATH "/line-2.png";
		}
		else if (effect.scale<4) {
			effect_path = RES_PATH "/line-3.png";
		}
		else if (effect.scale<5) {
			effect_path = RES_PATH "/line-4.png";
		}
		else if (effect.scale<6) {
			effect_path = RES_PATH "/line-5.png";
		}
		else if (effect.scale<8) {
			effect_path = RES_PATH "/line-6.png";
		}
		else {
			effect_path = RES_PATH "/line-8.png";
		}
	}
	else if (effect.type==EFFECT_GRID) {
		if (effect.scale<3) {
			effect_path = RES_PATH "/grid-2.png";
			opacity = 64; // 1 - 3/4 = 25%
		}
		else if (effect.scale<4) {
			effect_path = RES_PATH "/grid-3.png";
			opacity = 112; // 1 - 5/9 = ~44%
		}
		else if (effect.scale<5) {
			effect_path = RES_PATH "/grid-4.png";
			opacity = 144; // 1 - 7/16 = ~56%
		}
		else if (effect.scale<6) {
			effect_path = RES_PATH "/grid-5.png";
			opacity = 160; // 1 - 9/25 = ~64%
			// opacity = 96; // TODO: tmp, for white grid
		}
		else if (effect.scale<8) {
			effect_path = RES_PATH "/grid-6.png";
			opacity = 112; // 1 - 5/9 = ~44%
		}
		else if (effect.scale<11) {
			effect_path = RES_PATH "/grid-8.png";
			opacity = 144; // 1 - 7/16 = ~56%
		}
		else {
			effect_path = RES_PATH "/grid-11.png";
			opacity = 136; // 1 - 57/121 = ~52%
		}
	}
	
	// LOG_info("effect: %s opacity: %i\n", effect_path, opacity);
	SDL_Surface* tmp = IMG_Load(effect_path);
	if (tmp) {
		if (effect.type==EFFECT_GRID) {
			if (effect.color) {
				// LOG_info("dmg color grid...\n");
			
				uint8_t r,g,b;
				rgb565_to_rgb888(effect.color,&r,&g,&b);
				// LOG_info("rgb %i,%i,%i\n",r,g,b); 
			
				uint32_t* pixels = (uint32_t*)tmp->pixels;
				int width = tmp->w;
				int height = tmp->h;
				for (int y = 0; y < height; ++y) {
				    for (int x = 0; x < width; ++x) {
				        uint32_t pixel = pixels[y * width + x];
				        uint8_t _,a;
				        SDL_GetRGBA(pixel, tmp->format, &_, &_, &_, &a);
				        if (a) pixels[y * width + x] = SDL_MapRGBA(tmp->format, r,g,b, a);
				    }
				}
				
				// if (r==247 && g==243 & b==247) opacity = 64;
			}
		}

		if (vid.effect) SDL_DestroyTexture(vid.effect);
		vid.effect = SDL_CreateTextureFromSurface(vid.renderer, tmp);
		SDL_SetTextureAlphaMod(vid.effect, opacity);
		SDL_FreeSurface(tmp);
		effect.live_type = effect.type;
	}
}
void PLAT_setEffect(int next_type) {
	effect.next_type = next_type;
}
void PLAT_setEffectColor(int next_color) {
	effect.next_color = next_color;
}
void PLAT_vsync(int remaining) {
	if (remaining>0) SDL_Delay(remaining);
}

scaler_t PLAT_getScaler(GFX_Renderer* renderer) {
	// LOG_info("getScaler for scale: %i\n", renderer->scale);
	effect.next_scale = renderer->scale;
	return scale1x1_c16;
}

void PLAT_blitRenderer(GFX_Renderer* renderer) {
	vid.blit = renderer;
	SDL_RenderClear(vid.renderer);
	resizeVideo(vid.blit->true_w,vid.blit->true_h,vid.blit->src_p);
}

void PLAT_flip(SDL_Surface* IGNORED, int ignored) {
	PLAT_uvReassert(); // voltage authority: out-persist the kernel (no-op unless armed)
	
	if (!vid.blit) {
		if (fb_present) { PLAT_flipFB(); return; } // GPU-dark software present for the native-res menu
		resizeVideo(device_width,device_height,FIXED_PITCH); // !!!???
		SDL_UpdateTexture(vid.texture,NULL,vid.screen->pixels,vid.screen->pitch);
		SDL_RenderCopy(vid.renderer, vid.texture, NULL,NULL);
		SDL_RenderPresent(vid.renderer);
		return;
	}

	if (fb_game) { PLAT_flipFB_game(); vid.blit = NULL; return; } // GPU-dark software present for games

	// uint32_t then = SDL_GetTicks();
	SDL_UpdateTexture(vid.texture,NULL,vid.blit->src,vid.blit->src_p);
	// LOG_info("blit blocked for %ims (%i,%i)\n", SDL_GetTicks()-then,vid.buffer->w,vid.buffer->h);
	
	SDL_Texture* target = vid.texture;
	int x = vid.blit->src_x;
	int y = vid.blit->src_y;
	int w = vid.blit->src_w;
	int h = vid.blit->src_h;
	if (vid.sharpness==SHARPNESS_CRISP) {
		SDL_SetRenderTarget(vid.renderer,vid.target);
		SDL_RenderCopy(vid.renderer, vid.texture, NULL,NULL);
		SDL_SetRenderTarget(vid.renderer,NULL);
		x *= hard_scale;
		y *= hard_scale;
		w *= hard_scale;
		h *= hard_scale;
		target = vid.target;
	}
	
	SDL_Rect* src_rect = &(SDL_Rect){x,y,w,h};
	SDL_Rect* dst_rect = &(SDL_Rect){0,0,device_width,device_height};
	if (vid.blit->aspect==0) { // native or cropped
		// LOG_info("src_rect %i,%i %ix%i\n",src_rect->x,src_rect->y,src_rect->w,src_rect->h);
		
		int w = vid.blit->src_w * vid.blit->scale;
		int h = vid.blit->src_h * vid.blit->scale;
		int x = (device_width - w) / 2;
		int y = (device_height - h) / 2;
		dst_rect->x = x;
		dst_rect->y = y;
		dst_rect->w = w;
		dst_rect->h = h;
						
		// LOG_info("dst_rect %i,%i %ix%i\n",dst_rect->x,dst_rect->y,dst_rect->w,dst_rect->h);
	}
	else if (vid.blit->aspect>0) { // aspect
		int h = device_height;
		int w = h * vid.blit->aspect;
		if (w>device_width) {
			double ratio = 1 / vid.blit->aspect;
			w = device_width;
			h = w * ratio;
		}
		int x = (device_width - w) / 2;
		int y = (device_height - h) / 2;
		// dst_rect = &(SDL_Rect){x,y,w,h};
		dst_rect->x = x;
		dst_rect->y = y;
		dst_rect->w = w;
		dst_rect->h = h;
	}
	
	SDL_RenderCopy(vid.renderer, target, src_rect, dst_rect);
	
	updateEffect();
	if (vid.blit && effect.type!=EFFECT_NONE && vid.effect) {
		SDL_RenderCopy(vid.renderer, vid.effect, &(SDL_Rect){0,0,dst_rect->w,dst_rect->h}, dst_rect);
	}
	// uint32_t then = SDL_GetTicks();
	SDL_RenderPresent(vid.renderer);
	// LOG_info("SDL_RenderPresent blocked for %ims\n", SDL_GetTicks()-then);
	vid.blit = NULL;
}

///////////////////////////////

// TODO: 
#define OVERLAY_WIDTH PILL_SIZE // unscaled
#define OVERLAY_HEIGHT PILL_SIZE // unscaled
#define OVERLAY_BPP 4
#define OVERLAY_DEPTH 16
#define OVERLAY_PITCH (OVERLAY_WIDTH * OVERLAY_BPP) // unscaled
#define OVERLAY_RGBA_MASK 0x00ff0000,0x0000ff00,0x000000ff,0xff000000 // ARGB
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
void PLAT_enableOverlay(int enable) {

}

///////////////////////////////

static int online = 0;
void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	// *is_charging = 0;
	// *charge = PWR_LOW_CHARGE;
	// return;
	
	*is_charging = getInt("/sys/class/power_supply/axp2202-usb/online");

	int i = getInt("/sys/class/power_supply/axp2202-battery/capacity");
	// worry less about battery and more about the game you're playing
	     if (i>80) *charge = 100;
	else if (i>60) *charge =  80;
	else if (i>40) *charge =  60;
	else if (i>20) *charge =  40;
	else if (i>10) *charge =  20;
	else           *charge =  10;

	// // wifi status, just hooking into the regular PWR polling
	char status[16];
	getFile("/sys/class/net/wlan0/operstate", status,16);
	online = prefixMatch("up", status);
}

// NOTE: stock MinUI lit the ambient LEDs (scale 60) whenever the screen slept and during
// power-off, as an "asleep, not dead" indicator (PLAT_enableLED). Removed 2026-07-03: it was
// the second LED re-arm vector (found glowing on a charging, sleeping Brick) and burning three
// LED rails during the exact window the fork promises to run dark is anti-thesis. Don't re-add.

#define BLANK_PATH "/sys/class/graphics/fb0/blank"
void PLAT_enableBacklight(int enable) {
	if (enable) {
		// putInt(BLANK_PATH,0);
		if (is_brick) SetRawBrightness(8);
		SetBrightness(GetBrightness());
	}
	else {
		// putInt(BLANK_PATH,4);
		SetRawBrightness(0);
	}
}

void PLAT_powerOff(void) {
	// break the MinUI.pak/launch.sh while loop
	unlink("/tmp/minui_exec");
	sleep(2);

	SetRawVolume(MUTE_VOLUME_RAW);
	PLAT_enableBacklight(0);
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();
	
	exit(0); // poweroff handled by PLATFORM/bin/shutdown
}

int PLAT_supportsDeepSleep(void) { return 1; } // tg5040/Brick can suspend-to-RAM

///////////////////////////////

// Hybrid governor: boot.sh selects the `schedutil` kernel governor; all CPU control
// here sets the frequency *ceiling* (scaling_max_freq) and lets the kernel pick beneath
// it. The kernel snaps to the nearest supported OPP, so an out-of-ladder kHz is coerced,
// never an error. NOTE: 2.0GHz is an overclock — PERFORMANCE caps at the 1.8GHz stock max.
#define MAX_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
// CEILING CHOKE POINT: every ceiling write orders voltage around it — raise volts BEFORE
// raising the ceiling, lower the ceiling BEFORE lowering volts. This must cover ALL
// callers, not just the governor: the in-game menu exit path (PWR_setCPUSpeed) jumps the
// ceiling from menu-low straight to 1608/1800, and without this ordering the hold thread
// kept asserting the low-ceiling voltage at the new high clock — below the chip's cliff
// — for up to one governor tick. Found in the pre-merge audit; do not bypass this helper.
static int uv_last_ceiling = 0;
static void uv_ceilingWrite(int khz) {
	if (khz <= 0) return;
	if (khz > uv_last_ceiling) {
		PLAT_setCPUVoltForCeil(khz);
		putInt(MAX_FREQ_PATH, khz);
	}
	else {
		putInt(MAX_FREQ_PATH, khz);
		PLAT_setCPUVoltForCeil(khz);
	}
	uv_last_ceiling = khz;
}

void PLAT_setCPUSpeed(int speed) {
	int freq = 0;
	switch (speed) {
		case CPU_SPEED_MENU: 		freq =  600000; break;
		case CPU_SPEED_POWERSAVE:	freq = 1200000; break;
		case CPU_SPEED_NORMAL: 		freq = 1608000; break;
		case CPU_SPEED_PERFORMANCE: freq = 1800000; break; // stock max, NOT 2000000 (OC)
	}
	uv_ceilingWrite(freq);
}

// Closed-loop governor: set the cpufreq ceiling (kHz) via scaling_max_freq; schedutil
// picks the instantaneous frequency beneath it. Voltage ordering handled by the choke point.
void PLAT_setCPUMaxFreq(int khz) {
	uv_ceilingWrite(khz);
}

// ============================ Optimize Device: voltage authority ============================
// Runtime per-device undervolt (P2 campaign result, docs/dtb-undervolt-primer.md). The CPU
// rail is the external TCS4838 buck at i2c-6 0x41 (FAN53555-family VSEL registers,
// 712.5mV base + 12.5mV/step). Voltages are RAM-only: any reboot/crash returns to the
// kernel's stock OPP table by construction.
//
// SAFETY MODEL (all gates must pass or this stays a permanent no-op):
//   - table file present (written only by a completed calibration campaign)
//   - VSEL register decode matches the kernel regulator's reported voltage at init
//   - requested voltage within [table floor .. stock max], on a 12.5mV step
//   - ZERO_NO_UV=1 env kills it
// The GOVERNOR is the only caller and owns the ordering (volt-up before clock-up,
// clock-down before volt-down) — see gov_tick.
#include <pthread.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#define SHARED_UV_DIR  "/mnt/SDCARD/.userdata/tg5040/undervolt"
#define UV_TABLE_PATH  SHARED_UV_DIR "/table.conf"
#define UV_I2C_DEV     "/dev/i2c-6"
#define UV_I2C_ADDR    0x41
#define UV_BASE_UV     712500
#define UV_STEP_UV     12500
#define UV_STOCK_MAX   1187500 // stock voltage of the top OPP: always-safe restore value

static struct { int khz; int uv; } uv_table[16];
static int uv_n = 0;
static int uv_fd = -1;      // -1 = uninitialized, -2 = permanently disabled
static int uv_applied = 0;  // last commanded uV (0 = stock/untouched)
static int uv_target = 0;   // the voltage the authority wants HELD right now (0 = none)
static pthread_t uv_thread;
static pthread_mutex_t uv_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int uv_thread_run = 0;

static int uv_reg_read(int reg) {
	unsigned char r = (unsigned char)reg, v;
	if (write(uv_fd, &r, 1) != 1 || read(uv_fd, &v, 1) != 1) return -1;
	return v;
}
static pthread_mutex_t uv_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int uv_init(void) {
	if (uv_fd == -2) return 0;
	if (uv_fd >= 0) return 1;
	pthread_mutex_lock(&uv_init_lock); // thread_video can race the first call from two threads
	if (uv_fd != -1) { int ok = (uv_fd >= 0); pthread_mutex_unlock(&uv_init_lock); return ok; }
	uv_fd = -2; // assume failure; prove otherwise
	char* e = getenv("ZERO_NO_UV");
	if (e && e[0] && e[0] != '0') { pthread_mutex_unlock(&uv_init_lock); return 0; }
	// CARD-SWAP GUARD: the table describes ONE chip. Primary check = the eFUSE chip serial
	// (sunxi_serial — globally unique per die), so even two same-model devices never apply
	// each other's tables. Fallback = model string (older calibrations without table.chip).
	{
		char dev_chip[64] = {0};
		FILE* sf = fopen("/sys/class/sunxi_info/sys_info", "r");
		if (sf) {
			char line[128];
			while (fgets(line, sizeof line, sf)) {
				if (strstr(line, "sunxi_serial")) {
					char* c = strchr(line, ':');
					if (c) { c++; while (*c == ' ' || *c == '\t') c++;
						snprintf(dev_chip, sizeof dev_chip, "%s", c);
						char* nl = strchr(dev_chip, '\n'); if (nl) *nl = 0; }
					break;
				}
			}
			fclose(sf);
		}
		char tab_chip[64] = {0};
		FILE* cf = fopen(SHARED_UV_DIR "/table.chip", "r");
		if (cf) { if (fgets(tab_chip, sizeof tab_chip, cf)) { char* nl = strchr(tab_chip, '\n'); if (nl) *nl = 0; } fclose(cf); }

		if (dev_chip[0] && tab_chip[0]) {
			if (strcmp(dev_chip, tab_chip) != 0) {
				LOG_info("uv: table was calibrated for a different chip — staying stock\n");
				pthread_mutex_unlock(&uv_init_lock);
				return 0;
			}
		}
		else { // no chip identity available: fall back to model matching
			char want[64] = {0}, have[64] = {0};
			char* m = getenv("TRIMUI_MODEL");
			if (m) snprintf(want, sizeof want, "%s", m);
			FILE* mf = fopen(SHARED_UV_DIR "/table.model", "r");
			if (mf) { if (fgets(have, sizeof have, mf)) { char* nl = strchr(have, '\n'); if (nl) *nl = 0; } fclose(mf); }
			if (!want[0] || !have[0] || strcmp(want, have) != 0) {
				if (have[0]) LOG_info("uv: table is for '%s' but device is '%s' — staying stock\n", have, want);
				pthread_mutex_unlock(&uv_init_lock);
				return 0;
			}
		}
	}
	FILE* f = fopen(UV_TABLE_PATH, "r");
	if (!f) { pthread_mutex_unlock(&uv_init_lock); return 0; } // no calibration -> stock, silently
	uv_n = 0;
	int khz, uv, prev_khz = 0;
	while (uv_n < 16 && fscanf(f, "%d %d", &khz, &uv) == 2) {
		if (uv < UV_BASE_UV || uv > UV_STOCK_MAX || (uv - UV_BASE_UV) % UV_STEP_UV) { uv_n = 0; break; }
		if (khz <= prev_khz) { uv_n = 0; break; } // must be strictly ascending: the lookup depends on it
		prev_khz = khz;
		uv_table[uv_n].khz = khz; uv_table[uv_n].uv = uv; uv_n++;
	}
	fclose(f);
	if (!uv_n) { LOG_info("uv: table invalid, staying stock\n"); pthread_mutex_unlock(&uv_init_lock); return 0; }
	int fd = open(UV_I2C_DEV, O_RDWR);
	if (fd < 0) { pthread_mutex_unlock(&uv_init_lock); return 0; }
	if (ioctl(fd, I2C_SLAVE_FORCE, UV_I2C_ADDR) < 0) { close(fd); pthread_mutex_unlock(&uv_init_lock); return 0; }
	uv_fd = fd;
	// decode-match gate: VSEL must agree with the kernel regulator before we ever write.
	// Retried: a single read can race a DVFS transition (register vs the kernel's cached
	// value) and false-negative — a real wrong-chip mismatch fails every attempt. (The
	// first deploy hit exactly this race; the gate fail-safed to stock, as designed.)
	int v0 = -1, kuv = -1, tries;
	for (tries = 0; tries < 5; tries++) {
	v0 = uv_reg_read(0x00);
	kuv = -1;
	for (int i = 0; i < 32; i++) {
		char path[96], name[32];
		snprintf(path, sizeof path, "/sys/class/regulator/regulator.%d/name", i);
		FILE* rf = fopen(path, "r");
		if (!rf) continue;
		if (fgets(name, sizeof name, rf) && !strncmp(name, "tcs4838-dcdc0", 13)) {
			fclose(rf);
			snprintf(path, sizeof path, "/sys/class/regulator/regulator.%d/microvolts", i);
			rf = fopen(path, "r");
			if (rf) { if (fscanf(rf, "%d", &kuv) != 1) kuv = -1; fclose(rf); }
			break;
		}
		fclose(rf);
	}
	if (v0 >= 0 && kuv >= 0 && UV_BASE_UV + (v0 & 0x3F) * UV_STEP_UV == kuv) break;
	usleep(20000); // let any in-flight DVFS transition settle
	}
	if (tries >= 5) {
		LOG_info("uv: decode mismatch after retries (reg=%d kernel=%d) — staying stock\n", v0, kuv);
		close(uv_fd); uv_fd = -2; pthread_mutex_unlock(&uv_init_lock); return 0;
	}
	LOG_info("uv: voltage authority armed (%d table entries)\n", uv_n);
	pthread_mutex_unlock(&uv_init_lock);
	return 1;
}
static void uv_write(int uv) {
	if (uv < UV_BASE_UV || uv > UV_STOCK_MAX) return;
	int vsel = (uv - UV_BASE_UV) / UV_STEP_UV;
	int v0 = uv_reg_read(0x00), v1 = uv_reg_read(0x01);
	if (v0 < 0 || v1 < 0) return;
	unsigned char b0[2] = { 0x00, (unsigned char)((v0 & 0xC0) | vsel) };
	unsigned char b1[2] = { 0x01, (unsigned char)((v1 & 0xC0) | vsel) };
	if (write(uv_fd, b0, 2) != 2) return;
	write(uv_fd, b1, 2);
	uv_applied = uv;
}
int PLAT_supportsUndervolt(void) { return uv_init(); }
void PLAT_setCPUVoltForCeil(int khz) {
	// voltage that covers the highest OPP the kernel may round the ceiling UP to:
	// smallest table entry >= khz (table sorted ascending); above the table -> stock.
	if (!uv_init()) return;
	int uv = 0;
	for (int i = 0; i < uv_n; i++) if (uv_table[i].khz >= khz) { uv = uv_table[i].uv; break; }
	if (!uv) uv = UV_STOCK_MAX; // ceiling above table: run stock volts
	// The kernel RE-ASSERTS its stock voltage on every DVFS transition, so the register is
	// the only truth — read it and rewrite when it drifted (one i2c read per tick, ~2Hz).
	pthread_mutex_lock(&uv_lock);
	uv_target = uv;
	int v0 = uv_reg_read(0x00);
	if (v0 >= 0 && UV_BASE_UV + (v0 & 0x3F) * UV_STEP_UV != uv) uv_write(uv);
	else if (v0 < 0 && uv != uv_applied) uv_write(uv);
	pthread_mutex_unlock(&uv_lock);
}
// The kernel re-stocks the rail on EVERY schedutil DVFS transition (many/sec), so a
// per-frame (60Hz) reassert only wins ~50% of the time. A dedicated thread polls the
// register at ~200Hz and rewrites the instant the kernel drifts us off target. Safe by
// the governor's ordering: uv_target always covers the ceiling (raised before the clock,
// lowered after it), so holding it continuously never under-volts a live clock. i2c is
// serialized with the governor's own writes by uv_lock.
static void* uv_hold(void* arg) {
	(void)arg;
	while (uv_thread_run) {
		if (uv_fd >= 0 && uv_target) {
			pthread_mutex_lock(&uv_lock);
			int v0 = uv_reg_read(0x00);
			if (v0 >= 0 && UV_BASE_UV + (v0 & 0x3F) * UV_STEP_UV != uv_target) uv_write(uv_target);
			pthread_mutex_unlock(&uv_lock);
		}
		usleep(5000); // ~200Hz
	}
	return NULL;
}
void PLAT_uvReassert(void) {
	// Lazily start the hold thread on first armed frame (no-op unless the authority armed).
	if (uv_fd < 0 || !uv_target || uv_thread_run) return;
	uv_thread_run = 1;
	if (pthread_create(&uv_thread, NULL, uv_hold, NULL) != 0) uv_thread_run = 0;
	else LOG_info("uv: hold thread started (~200Hz, holding %duV)\n", uv_target);
}
void PLAT_emergencyRestoreCPUVolt(void) {
	// SIGNAL-HANDLER-SAFE restore: NO mutex (the hold thread holds uv_lock ~20% of the
	// time; locking here could deadlock a dying process into a frozen zombie). Ordering:
	// clear the target first so the hold thread stops re-asserting, then one best-effort
	// raw write of stock-max. Post-crash, the kernel re-stocks on the next DVFS transition
	// anyway (and sequences volts-before-freq), so this is belt on top of kernel braces.
	uv_thread_run = 0;
	uv_target = 0;
	if (uv_fd >= 0 && uv_applied) uv_write(UV_STOCK_MAX);
	uv_applied = 0;
}
void PLAT_restoreCPUVolt(void) {
	// one always-safe write: stock max voltage; the kernel re-asserts exact stock on the
	// next OPP transition. Called on quit and from the crash handler.
	uv_thread_run = 0; // stop the hold thread first
	pthread_mutex_lock(&uv_lock);
	uv_target = 0;
	if (uv_fd >= 0 && uv_applied) uv_write(UV_STOCK_MAX);
	uv_applied = 0;
	pthread_mutex_unlock(&uv_lock);
}
void PLAT_setUndervolt(int millivolts) { (void)millivolts; } // superseded by the table API

#define RUMBLE_PATH "/sys/class/gpio/gpio227/value"
void PLAT_setRumble(int strength) {
	putInt(RUMBLE_PATH, (strength && !GetMute())?1:0);
}

int PLAT_pickSampleRate(int requested, int max) {
	return MIN(requested, max);
}

char* PLAT_getModel(void) {
	char* model = getenv("TRIMUI_MODEL");
	if (model) return model;
	return "Trimui Smart Pro";
}

int PLAT_isOnline(void) {
	return online;
}