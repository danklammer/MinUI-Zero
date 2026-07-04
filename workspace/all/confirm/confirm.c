// confirm — like say.elf, but a decision: A confirms (exit 0), B cancels (exit 1),
// optional X third action (exit 2). Button labels are configurable.
//   confirm "message"                          -> A=CONFIRM / B=CANCEL
//   confirm "message" "START" "BACK"           -> custom A/B labels
//   confirm "message" "START" "BACK" "REVERT"  -> adds X (exit 2)
#include <stdio.h>
#include <math.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

int main(int argc, char* argv[]) {
	// --ok mode: celebratory status screen — big green pixel check + green title + body.
	//   confirm --ok "Title" "body" "A_LABEL_or_empty" "B_LABEL" ["X_LABEL"]
	int ok_mode = (argc > 1 && strcmp(argv[1], "--ok") == 0);
	int base = ok_mode ? 2 : 1;
	char title[256] = {0};
	if (ok_mode && argc > base) { snprintf(title, sizeof title, "%s", argv[base]); base++; }
	char msg[1024];
	sprintf(msg, "%s", argc > base ? argv[base] : "");
	char* a_label = argc > base+1 ? argv[base+1] : "CONFIRM";
	char* b_label = argc > base+2 ? argv[base+2] : "CANCEL";
	char* x_label = argc > base+3 ? argv[base+3] : NULL;
	if (a_label && !a_label[0]) a_label = NULL; // "" hides + disables A

	PWR_setCPUSpeed(CPU_SPEED_MENU);
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();

	int quit = 0, result = 1, dirty = 1;
	while (!quit) {
		PAD_poll();
		if (a_label && PAD_justPressed(BTN_A)) { result = 0; quit = 1; }
		else if (PAD_justPressed(BTN_B)) { result = 1; quit = 1; }
		else if (x_label && PAD_justPressed(BTN_X)) { result = 2; quit = 1; }

		if (dirty) {
			GFX_clear(screen);
			// inset the message from the top edge so long text cannot clip the first line
			int top = SCALE1(PADDING * 2);
			if (ok_mode) {
				// heroicons "check-circle": filled green disc with the check carved out
				// (carved = drawn in background black), whole group vertically centered
				SDL_Color green = {0x30, 0xD1, 0x58, 0xFF};
				uint32_t gfill = SDL_MapRGB(screen->format, green.r, green.g, green.b);
				uint32_t bg    = SDL_MapRGB(screen->format, 0, 0, 0);
				int R = SCALE1(18); // disc radius

				SDL_Surface* t = title[0] ? TTF_RenderUTF8_Blended(font.large, title, green) : NULL;
				int lines = 1; for (char* c = msg; *c; c++) if (*c == '\n') lines++;
				int line_h = TTF_FontLineSkip(font.medium);
				int gap = SCALE1(12);
				int group_h = 2*R + gap + (t ? t->h + gap : 0) + lines * line_h;
				int usable_h = screen->h - SCALE1(PADDING + PILL_SIZE + PADDING);
				int y = (usable_h - group_h) / 2;
				if (y < SCALE1(PADDING)) y = SCALE1(PADDING);

				int ccx = screen->w / 2, ccy = y + R;
				for (int dy = -R; dy <= R; dy++) { // filled disc, row spans
					int half = (int)sqrtf((float)(R*R - dy*dy));
					SDL_FillRect(screen, &(SDL_Rect){ccx - half, ccy + dy, half*2 + 1, 1}, gfill);
				}
				// carve the check: stamp bg discs along the two strokes (24-unit icon space)
				float sc = R / 12.0f;
				float pts[3][2] = { {8.7f,12.5f}, {11.2f,15.0f}, {15.6f,9.9f} };
				float sr = 1.05f * sc;
				for (int seg = 0; seg < 2; seg++) {
					for (int i = 0; i <= 28; i++) {
						float fx = pts[seg][0] + (pts[seg+1][0] - pts[seg][0]) * i / 28.0f;
						float fy = pts[seg][1] + (pts[seg+1][1] - pts[seg][1]) * i / 28.0f;
						int px = ccx + (int)((fx - 12.0f) * sc);
						int py = ccy + (int)((fy - 12.0f) * sc);
						int rr = (int)sr;
						for (int dy = -rr; dy <= rr; dy++) {
							int half = (int)sqrtf(sr*sr - (float)(dy*dy));
							SDL_FillRect(screen, &(SDL_Rect){px - half, py + dy, half*2 + 1, 1}, bg);
						}
					}
				}
				y += 2*R + gap;
				if (t) {
					SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){(screen->w - t->w)/2, y});
					y += t->h + gap;
					SDL_FreeSurface(t);
				}
				GFX_blitMessage(font.medium, msg, screen, &(SDL_Rect){0,y,screen->w,lines * line_h + SCALE1(4)});
			}
			else {
				GFX_blitMessage(font.large, msg, screen, &(SDL_Rect){0,top,screen->w,screen->h-top-SCALE1(PADDING + PILL_SIZE + PADDING)});
			}
			// B is ALWAYS the bottom-left pill (universal back); actions live bottom-right
			GFX_blitButtonGroup((char*[]){ "B", b_label, NULL }, 0, screen, 0);
			if (x_label && a_label) GFX_blitButtonGroup((char*[]){ "X",x_label, "A",a_label, NULL }, 1, screen, 1);
			else if (x_label) GFX_blitButtonGroup((char*[]){ "X",x_label, NULL }, 0, screen, 1);
			else if (a_label) GFX_blitButtonGroup((char*[]){ "A",a_label, NULL }, 0, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return result;
}
