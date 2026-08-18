#include <nds.h>
#include <nds/arm9/dldi.h>
#include <fat.h>

#include <cstdlib>
#include <ctime>

#include "device.h"
#include "ui.h"
#include "menu.h"
#include "nds_platform.h"

using namespace flashcart_core;
using namespace ncgc;


int main(void)
{
	// Shows a register/PC dump on-screen on an ARM9 exception instead of a
	// silent hang/reset -- costs nothing when nothing crashes, and without
	// it there's no way to diagnose a real crash on hardware at all.
	defaultExceptionHandler();

	// BlocksDS boosts the ARM9 to 134MHz on DSi by default. libncgc's cart-protocol
	// delays are raw cycle-count busy-waits, so doubling the clock halves their
	// real time -- force back to 67MHz to match the timing they're tuned for.
	setCpuClock(false);

	// Seed once, here, not inside the combo prompt itself: reseeding per-call
	// from time(NULL) reset the RNG instead of advancing it, so two prompts in
	// the same clock second got a byte-identical combo.
	srand(time(NULL));

	sysSetBusOwners(true, true); //Give ARM9 access to the cart
	InitializeScreens();

	// BlocksDS autodetects ARM7-capable DLDI drivers and runs them on the ARM7,
	// which hands Slot-1 to the ARM7 (EXMEMCNT bit 11) -- every ARM9-side cart
	// access then reads open-bus 0xFFFFFFFF and detection fails. Pin it to ARM9.
	dldiSetMode(DLDI_MODE_ARM9);
	if (!fatInitDefault()) {
		fb_draw_wrapped_chars(BOTTOM_SCREEN, UiBottomContentX(0), UiBottomContentY(1), fb_theme()->danger,
			"SD card init failed!\nLogging, backups and writes\nwon't work this session.");
		// Nothing can be logged without a card, so put the probe on screen
		// instead -- it survives until the cart list draws over it.
		fb_draw_heading(BOTTOM_SCREEN, UiBottomPageRegions().content.row + 5,
			FB_HEADING_2, fb_theme()->text, fb_theme()->bg, "Hardware probe");
		LogHardwareProbe(7);
	}

	Flashcart *cart = nullptr; //We define our main cart variable right here, and we will pass it along from function to function until the very end

	print_boot_msg();

	menu_lvl1(cart); //menu_lvl1() will later call menu_lvl2()

	return 0;
}
