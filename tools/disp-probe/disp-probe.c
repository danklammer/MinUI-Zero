// disp-probe.c — MinUI Zero /dev/disp recon (TrimUI Brick / A133P, tg5040).
//
// Goal: confirm the Allwinner DE2 exposes SCALABLE display layers via /dev/disp — the m21-style
// hardware-scale path. If it does, MinUI Zero can present GPU-dark (no GLES) WITHOUT paying the CPU
// software-scale cost that makes a pure-software present borderline at 1024x768.
//
// Phase 1 (default, READ-ONLY, safe to run anytime): enumerate active layers with DISP_LAYER_GET_CONFIG
//   and print each layer's pixel format, screen_win (the on-screen DEST rect) and crop (the SOURCE
//   rect) + buffer addr. If crop(source) and screen_win(dest) are independently settable, the DE scales.
//
// Phase 2 (--scale-test, DISRUPTIVE: briefly resizes the LIVE picture for ~3s then restores it):
//   take the active layer and shrink its screen_win to a centered half-size window, keeping the same
//   source. If the picture scales down into that window, the DE hardware-scales — proof. No ION/cedar
//   needed (reuses the existing framebuffer). The original config is restored before exit.
//
// ABI headers (sunxi_display2.h) borrowed from MyMinUI (Turro75/MyMinUI, m21 platform). See
// THIRD_PARTY_NOTICES.md. Cross-build: aarch64-linux-gnu-gcc disp-probe.c -o disp-probe -std=gnu99
//
// NOTE: the disp2 ioctl arg block is 4 kernel-longs {screen, &config, count, 0}. It MUST be
// `unsigned long` (64-bit on aarch64). m21 uses uint32_t because m21 is 32-bit ARM — on our 64-bit
// kernel that would truncate the config pointer. This is the key 32->64 porting fix.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "sunxi_display2.h"   // provides `bool`, the disp_* structs, and the DISP_LAYER_* ioctl enums

static int layer_get(int fd, unsigned scr, struct disp_layer_config *cfg) {
	unsigned long args[4] = { scr, (unsigned long)cfg, 1, 0 };
	return ioctl(fd, DISP_LAYER_GET_CONFIG, args);
}
static int layer_set(int fd, unsigned scr, struct disp_layer_config *cfg) {
	unsigned long args[4] = { scr, (unsigned long)cfg, 1, 0 };
	return ioctl(fd, DISP_LAYER_SET_CONFIG, args);
}

static void print_layer(unsigned scr, const struct disp_layer_config *c) {
	const struct disp_fb_info *fb = &c->info.fb;
	printf("  scr%u ch%u/ly%u  enable=%d  mode=%d  fmt=0x%x\n",
	       scr, c->channel, c->layer_id, c->enable, c->info.mode, fb->format);
	printf("      screen_win(DEST)=[%d,%d %ux%u]   fb.size(SRC buf)=[%ux%u]\n",
	       c->info.screen_win.x, c->info.screen_win.y, c->info.screen_win.width, c->info.screen_win.height,
	       fb->size[0].width, fb->size[0].height);
	printf("      crop(SRC)=[%llu,%llu %llux%llu]  addr=[%llx,%llx,%llx]\n",
	       (unsigned long long)fb->crop.x, (unsigned long long)fb->crop.y,
	       (unsigned long long)fb->crop.width, (unsigned long long)fb->crop.height,
	       (unsigned long long)fb->addr[0], (unsigned long long)fb->addr[1], (unsigned long long)fb->addr[2]);
}

int main(int argc, char **argv) {
	int scale_test = (argc > 1 && strcmp(argv[1], "--scale-test") == 0);

	int fd = open("/dev/disp", O_RDWR);
	if (fd < 0) { fprintf(stderr, "open /dev/disp: %s\n", strerror(errno)); return 1; }
	printf("opened /dev/disp fd=%d\n", fd);

	// ---- Phase 1: enumerate active layers (READ-ONLY) ----
	struct disp_layer_config found;   // first enabled layer we see
	int have_found = 0, active = 0;
	printf("\n=== Phase 1: active layers (DISP_LAYER_GET_CONFIG) ===\n");
	for (unsigned scr = 0; scr < 2; scr++) {
		for (unsigned ch = 0; ch < 4; ch++) {
			for (unsigned ly = 0; ly < 4; ly++) {
				struct disp_layer_config c;
				memset(&c, 0, sizeof c);
				c.channel = ch; c.layer_id = ly;
				if (layer_get(fd, scr, &c) < 0) continue;   // this slot not gettable
				if (!c.enable) continue;                    // only show active layers
				active++;
				print_layer(scr, &c);
				if (!have_found) { found = c; found.channel = ch; found.layer_id = ly;
				                   have_found = 1; }
				// stash the screen id alongside via a static — simplest: remember below
			}
		}
	}
	if (!active) {
		printf("  (no enabled layers returned — GET_CONFIG(v1) ABI may not match; try DISP_LAYER_GET_CONFIG2)\n");
		printf("VERDICT: inconclusive — need to try the config2 ABI.\n");
		close(fd); return 2;
	}
	printf("\nVERDICT (Phase 1): /dev/disp GET_CONFIG works, %d active layer(s). If crop(SRC) and\n"
	       "screen_win(DEST) above are independent rects, the DE can scale SRC->DEST in hardware.\n", active);

	if (!scale_test) {
		printf("\n(run with --scale-test to actively prove scaling — it briefly resizes the picture.)\n");
		close(fd); return 0;
	}

	// ---- Phase 2: prove scaling by shrinking the active layer's DEST window, then restore ----
	// NOTE: we assume the active layer is on screen 0 (matches the tg5040 dump ch1/ly0). We re-GET it
	// on screen 0 to be safe.
	struct disp_layer_config orig;
	memset(&orig, 0, sizeof orig);
	orig.channel = found.channel; orig.layer_id = found.layer_id;
	if (layer_get(fd, 0, &orig) < 0 || !orig.enable) {
		fprintf(stderr, "scale-test: could not re-read active layer on screen 0: %s\n", strerror(errno));
		close(fd); return 3;
	}

	struct disp_layer_config test = orig;
	unsigned sw = orig.info.screen_win.width, sh = orig.info.screen_win.height;
	// centered half-size destination window — same source, so the DE must downscale to fit.
	test.info.screen_win.width  = sw / 2;
	test.info.screen_win.height = sh / 2;
	test.info.screen_win.x = sw / 4;
	test.info.screen_win.y = sh / 4;

	printf("\n=== Phase 2: shrinking DEST to [%d,%d %ux%u] for 3s (source unchanged) ===\n",
	       test.info.screen_win.x, test.info.screen_win.y, test.info.screen_win.width, test.info.screen_win.height);
	if (layer_set(fd, 0, &test) < 0) {
		fprintf(stderr, "DISP_LAYER_SET_CONFIG failed: %s\n", strerror(errno));
		close(fd); return 4;
	}
	printf("SET ok — if the picture is now a centered half-size window, the DE HARDWARE-SCALES.\n");
	fflush(stdout);
	sleep(3);

	if (layer_set(fd, 0, &orig) < 0)
		fprintf(stderr, "WARN: restore SET_CONFIG failed: %s (relaunch a game/menu to redraw)\n", strerror(errno));
	else
		printf("restored original layer config.\n");

	printf("\nVERDICT (Phase 2): if you saw the picture shrink+restore, hardware scaling is CONFIRMED ->\n"
	       "the GPU-dark DE2 present path is viable with no CPU software-scale cost.\n");
	close(fd);
	return 0;
}
