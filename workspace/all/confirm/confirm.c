// confirm — like say.elf, but a decision: A confirms (exit 0), B cancels (exit 1),
// optional X third action (exit 2). Button labels are configurable.
//   confirm "message"                          -> A=CONFIRM / B=CANCEL
//   confirm "message" "START" "BACK"           -> custom A/B labels
//   confirm "message" "START" "BACK" "REVERT"  -> adds X (exit 2)
#include <stdio.h>
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
				// chunky pixel checkmark, MinUI-green, centered near the top
				SDL_Color green = {0x30, 0xD1, 0x58, 0xFF};
				uint32_t gfill = SDL_MapRGB(screen->format, green.r, green.g, green.b);
				int u = SCALE1(7); // block size
				static const int cx[] = {0,1,2,3,4,5,6}; // check shape: down-down-up-up-up
				static const int cy[] = {3,4,5,4,3,2,1};
				int cw = 7 * u;
				int x0 = (screen->w - cw) / 2;
				int y0 = top + SCALE1(6);
				for (int i = 0; i < 7; i++)
					SDL_FillRect(screen, &(SDL_Rect){x0 + cx[i]*u, y0 + cy[i]*u, u, u}, gfill);
				int y = y0 + 7*u + SCALE1(10);
				if (title[0]) { // green title, centered
					SDL_Surface* t = TTF_RenderUTF8_Blended(font.large, title, green);
					if (t) {
						SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){(screen->w - t->w)/2, y});
						y += t->h + SCALE1(8);
						SDL_FreeSurface(t);
					}
				}
				GFX_blitMessage(font.medium, msg, screen, &(SDL_Rect){0,y,screen->w,screen->h-y-SCALE1(PADDING + PILL_SIZE + PADDING)});
			}
			else {
				GFX_blitMessage(font.large, msg, screen, &(SDL_Rect){0,top,screen->w,screen->h-top-SCALE1(PADDING + PILL_SIZE + PADDING)});
			}
			if (x_label && a_label) GFX_blitButtonGroup((char*[]){ "X",x_label, "A",a_label, NULL }, 1, screen, 1);
			else if (x_label) GFX_blitButtonGroup((char*[]){ "X",x_label, "B",b_label, NULL }, 1, screen, 1);
			else GFX_blitButtonGroup((char*[]){ "B",b_label, "A",a_label ? a_label : "OKAY", NULL }, 1, screen, 1);
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
