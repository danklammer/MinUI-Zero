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
	char msg[1024];
	sprintf(msg, "%s", argc > 1 ? argv[1] : "");
	char* a_label = argc > 2 ? argv[2] : "CONFIRM";
	char* b_label = argc > 3 ? argv[3] : "CANCEL";
	char* x_label = argc > 4 ? argv[4] : NULL;

	PWR_setCPUSpeed(CPU_SPEED_MENU);
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();

	int quit = 0, result = 1, dirty = 1;
	while (!quit) {
		PAD_poll();
		if (PAD_justPressed(BTN_A)) { result = 0; quit = 1; }
		else if (PAD_justPressed(BTN_B)) { result = 1; quit = 1; }
		else if (x_label && PAD_justPressed(BTN_X)) { result = 2; quit = 1; }

		if (dirty) {
			GFX_clear(screen);
			// inset the message from the top edge so long text cannot clip the first line
			int top = SCALE1(PADDING * 2);
			GFX_blitMessage(font.large, msg, screen, &(SDL_Rect){0,top,screen->w,screen->h-top-SCALE1(PADDING + PILL_SIZE + PADDING)});
			if (x_label) GFX_blitButtonGroup((char*[]){ "X",x_label, "B",b_label, "A",a_label, NULL }, 2, screen, 1);
			else GFX_blitButtonGroup((char*[]){ "B",b_label, "A",a_label, NULL }, 1, screen, 1);
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
