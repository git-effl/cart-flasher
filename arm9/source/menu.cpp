#include "menu.h"

#include <nds.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib> // rand(); the seed itself lives in main()
#include <cstring>
#include <strings.h> // strcasecmp(); POSIX, so there's no <c...> spelling of it

#include "ui.h"
#include "nds_platform.h"
#include "device.h"
#include "filebrowser.h"

// Wording follows Sanras's flashcart guide, which quotes this tool verbatim
// ("flashrom", "key combo", "Back up flash", "Write flash") -- don't reword.
#define bootmsg "This tool writes directly to your\n" 					\
				"flashcart's flashrom. A bad write\n" 					\
				"can brick your cart, so always keep\n" 				\
				"a backup first.\n\n" 									\
				"Not every cart has been tested. If\n" 					\
				"you can't dump your cart's flashrom,\n" 				\
				"or the dump is nonsense, STOP and\n" 					\
				"open a GitHub issue."

using namespace flashcart_core;
using namespace ncgc;

int global_loglevel = 1; //https://github.com/ntrteam/flashcart_core/blob/master/platform.h#L6

// Cart selection changes can happen every VBlank. Render the description into
// RAM and let nds-fb copy only changed pixels, so the visible bottom screen is
// never briefly cleared between two cart descriptions.
static u16 cartInfoStaging[FB_PIXELS];
static u16 cartInfoPresented[FB_PIXELS];
static bool cartInfoPanelValid = false;

static void InvalidateCartInfoPanel(void)
{
	cartInfoPanelValid = false;
}

static void RenderCartInfoPanel(Flashcart* cart)
{
	const fb_theme_t *theme = fb_theme();
	if (!cartInfoPanelValid) {
		memset(cartInfoPresented, 0, sizeof(cartInfoPresented));
		cartInfoPanelValid = true;
	}

	fb_clear(cartInfoStaging, theme->bg);
	fb_draw_banner(cartInfoStaging, UiPageRegions().header.row,
		theme->text, theme->accent, "Flashcart info", "");
	DrawWrappedF(cartInfoStaging, UiContentX(1), UiContentY(1),
		(UiPageRegions().content.cols - 2) * FB_GLYPH_W, theme->text,
		"%s\n\n%s", cart->getAuthor(), cart->getDescription());
	(void)fb_apply_diff(BOTTOM_SCREEN, cartInfoPresented, cartInfoStaging);
}

// <START> power-off shortcut, checked once per frame from the boot splash
// and the cart list -- deliberately not everywhere in the app (see the
// switch-flash confirm/combo screens, which don't offer it). GodMode9i's own
// startMenu() was checked as precedent for the *action* itself: it offers
// exactly "Power off" (systemShutDown()) and a DSi-specific "Reboot", never
// a generic "return to loader" action. That's deliberate, not an oversight
// -- libnds's exit(0) can hang on flashcart menus (confirmed with AKMenu)
// that don't properly support or clear its loader-return protocol, so this
// app doesn't offer that path anywhere either. Never returns if <START> was
// pressed.
void HandlePowerOffShortcut(void)
{
	if (keysDown() & KEY_START)
	{
		systemShutDown();
		while (true) { swiWaitForVBlank(); }
	}
}

void print_boot_msg(void)
{
	// This is the only pre-action safety notice. Use the native warning block
	// so its visual treatment matches the severity of writing flashroms.
	DrawHeader(TOP_SCREEN, "Cart-Flasher");
	fb_draw_status_wrapped_page(TOP_SCREEN, UiPageRegions().content.row + 1, 12,
		FB_STATUS_WARNING, fb_theme()->warn, fb_theme()->bg, bootmsg);
	// Three credit lines finish one blank row above the persistent footer.
	DrawStringF(TOP_SCREEN, UiContentX(1), UiContentY(18), fb_theme()->secondary,
		"Developed by @tasken\n%s build - Commit: %s\nBased on work by jason0597 & DS-Homebrew",
		CART_FLASHER_BUILD_KIND, CART_FLASHER_COMMIT);
	static const fb_action_t actions[] = {
		{ FB_INPUT_A, "Continue", nullptr, 0 },
		{ FB_INPUT_START, "Power off", nullptr, 0 },
	};
	fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text, fb_theme()->select, actions, sizeof(actions) / sizeof(actions[0]));

	while (true)
	{
		swiWaitForVBlank();
		scanKeys();
		HandlePowerOffShortcut();
		if (keysDown() & KEY_A)
		{
			break;
		}
	}
}

void WaitPress(u32 KEY) {
	while (true) { swiWaitForVBlank(); scanKeys(); if (keysDown() & KEY) { break; } }
}

// <A> to go ahead, <B> to back out. WaitPress() only ever waits for one key, so
// it can't express a choice.
static bool WaitConfirm(void) {
	while (true) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_A) { return true; }
		if (keysDown() & KEY_B) { return false; }
	}
}

// Platform mapping is intentionally separate from nds-fb, whose button group
// remains portable and receives only logical edge-triggered input.
static fb_input_t MapButtons(u32 keys)
{
	fb_input_t input = 0;
	if (keys & KEY_A) { input |= FB_INPUT_A; }
	if (keys & KEY_B) { input |= FB_INPUT_B; }
	if (keys & KEY_UP) { input |= FB_INPUT_UP; }
	if (keys & KEY_DOWN) { input |= FB_INPUT_DOWN; }
	if (keys & KEY_LEFT) { input |= FB_INPUT_LEFT; }
	if (keys & KEY_RIGHT) { input |= FB_INPUT_RIGHT; }
	return input;
}

static int WaitButtonGroup(fb_button_group_t *group,
	const fb_button_group_layout_t *layout, const fb_button_colors_t *colors)
{
	const fb_button_group_input_t input =
		fb_modal_button_group_input_default(FB_BUTTON_GROUP_HORIZONTAL);
	while (true) {
		swiWaitForVBlank();
		scanKeys();
		int activated = -1;
		const fb_button_group_event_t event = fb_button_group_input(group, 0,
			MapButtons(keysDown()), &input, &activated);
		if (event == FB_BUTTON_GROUP_EVENT_MOVED) {
			fb_draw_button_group(TOP_SCREEN, group, layout, colors);
		} else if (event == FB_BUTTON_GROUP_EVENT_CANCELLED) {
			return -1;
		} else if (event == FB_BUTTON_GROUP_EVENT_ACTIVATED) {
			return activated;
		}
	}
}

static int RenderWriteConfirmationModal(void)
{
	const fb_cell_rect_t &content = UiPageRegions().content;
	const int modalCols = 43;
	const int modalRows = 18;
	const int modalCol = content.col + (content.cols - modalCols) / 2;
	const int modalRow = content.row + (content.rows - modalRows) / 2;
	fb_modal_semantic(TOP_SCREEN, modalCol, modalRow, modalCols, modalRows,
		fb_theme()->warn, fb_theme()->bg, fb_theme()->danger, FB_MODAL_TONE_DANGER);
	fb_draw_aligned(TOP_SCREEN, (modalCol + 2) * FB_GLYPH_W,
		(modalRow + 1) * FB_GLYPH_H, (modalCols - 4) * FB_GLYPH_W,
		FB_TEXT_ALIGN_CENTER, fb_theme()->text, "Write flash");
	fb_draw_wrapped_page(TOP_SCREEN, (modalCol + 2) * FB_GLYPH_W,
		(modalRow + 3) * FB_GLYPH_H, (modalCols - 4) * FB_GLYPH_W,
		6, fb_theme()->text,
		"This overwrites the cart's flashrom and can't be undone.\n\n"
		"A changed icon or banner is blocked by stock DSi/3DS firmware "
		"unless CFW is installed. NDS/DS Lite are fine.");
	fb_draw_aligned(TOP_SCREEN, (modalCol + 1) * FB_GLYPH_W,
		(modalRow + 10) * FB_GLYPH_H, (modalCols - 2) * FB_GLYPH_W,
		FB_TEXT_ALIGN_CENTER, fb_theme()->warn, "Enter the key combo to confirm:");
	static const fb_action_t actions[] = {
		{ FB_INPUT_B, "Cancel", nullptr, 0 },
	};
	fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text,
		fb_theme()->select, actions, sizeof(actions) / sizeof(actions[0]));
	return (modalRow + 12) * FB_GLYPH_H;
}

static void RenderWriteFailureModal(void)
{
	const fb_cell_rect_t &content = UiPageRegions().content;
	const int modalCols = 43;
	const int modalRows = 18;
	const int modalCol = content.col + (content.cols - modalCols) / 2;
	const int modalRow = content.row + (content.rows - modalRows) / 2;
	fb_modal_semantic(TOP_SCREEN, modalCol, modalRow, modalCols, modalRows,
		fb_theme()->warn, fb_theme()->bg, fb_theme()->danger, FB_MODAL_TONE_DANGER);
	fb_draw_aligned(TOP_SCREEN, (modalCol + 2) * FB_GLYPH_W,
		(modalRow + 1) * FB_GLYPH_H, (modalCols - 4) * FB_GLYPH_W,
		FB_TEXT_ALIGN_CENTER, fb_theme()->text, "Write flash");
	fb_draw_aligned(TOP_SCREEN, (modalCol + 1) * FB_GLYPH_W,
		(modalRow + 6) * FB_GLYPH_H, (modalCols - 2) * FB_GLYPH_W,
		FB_TEXT_ALIGN_CENTER, fb_theme()->danger, "Wrong key combo, nothing was touched.");
	static const fb_action_t actions[] = {
		{ FB_INPUT_A, "Retry", nullptr, 0 },
		{ FB_INPUT_B, "Cancel", nullptr, 0 },
	};
	fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text,
		fb_theme()->select, actions, sizeof(actions) / sizeof(actions[0]));
}

bool ntrCardReset()
{
	if (isDSiMode())
	{
		// Reset card slot
		disableSlot1();
		for(int i = 0; i < 25; i++) { swiWaitForVBlank(); }
		enableSlot1();
		for(int i = 0; i < 15; i++) { swiWaitForVBlank(); }
	}
	else
	{
		REG_ROMCTRL = 0;
		REG_AUXSPICNT = 0;
		for (int i = 0; i < 25; i++) swiWaitForVBlank();
		REG_AUXSPICNT = CARD_CR1_ENABLE | CARD_CR1_IRQ;
		REG_ROMCTRL = CARD_nRESET | CARD_SEC_SEED;
		while (REG_ROMCTRL & CARD_BUSY) ;
		cardReset();
		while (REG_ROMCTRL & CARD_BUSY) ;
	}
	return true;
}

void menu_lvl1(Flashcart* cart)
{
	// Remove R4iSDHC.hk if not in DSi mode
	if (!isDSiMode()) {
		for (auto it = flashcart_list->begin(); it != flashcart_list->end(); ) {
			if (strcmp((*it)->getShortName(), "R4iSDHC.hk") == 0) {
				it = flashcart_list->erase(it);
			} else {
				++it;
			}
		}
	}

	// Sort alphabetically by name
	std::sort(flashcart_list->begin(), flashcart_list->end(), [](Flashcart* a, Flashcart* b) {
		return strcasecmp(a->getName(), b->getName()) < 0;
	});

	u32 menu_sel = 0;
	fb_input_repeat_t cartRepeat = {};
	
	NTRCard card(ntrCardReset);
	DrawHeader(TOP_SCREEN, "Choose your flashcart");
	DrawFooter(global_loglevel);
	RenderCartInfoPanel(flashcart_list->at(0));
	u32 flashcart_list_size = flashcart_list->size();

	// Redraws only on change, not every frame -- full-width highlight bars
	// redrawn every frame with no vsync tear visibly on hardware.
	for (u32 i = 0; i < flashcart_list_size; i++)
	{
		fb_draw_list_row_style(TOP_SCREEN, UiPageRegions().content.row + i + 1,
			fb_theme()->text, fb_theme()->bg, fb_theme()->warn, fb_theme()->bg,
			flashcart_list->at(i)->getName(), i == menu_sel, FB_LIST_STYLE_CURSOR);
	}

	while (true) //This will be our MAIN loop
	{
		swiWaitForVBlank();
		bool reprintFlag = false;

		scanKeys();
		HandlePowerOffShortcut();
		const u32 heldKeys = keysHeld();
		const fb_input_t heldDirection = heldKeys & KEY_DOWN ? FB_INPUT_DOWN :
			heldKeys & KEY_UP ? FB_INPUT_UP : 0;
		if (fb_input_repeat_update(&cartRepeat, heldDirection, 12, 3)) {
			if (heldDirection == FB_INPUT_DOWN && menu_sel < (flashcart_list_size - 1)) {
				menu_sel++;
				reprintFlag = true;
			}
			if (heldDirection == FB_INPUT_UP && menu_sel > 0) {
				menu_sel--;
				reprintFlag = true;
			}
		}
		if (keysDown() & KEY_Y) {
			if (global_loglevel == 4) {
				global_loglevel = 0; //if you scroll past the end it puts you back at the top
			}
			else {
				global_loglevel++;
			}
			DrawFooter(global_loglevel);
			// Entering DEBUG snapshots hardware state (launch mode, CPU speed,
			// cart-bus ownership) to the log and screen -- the screen copy is
			// all that exists if the SD card itself is what failed.
			if (global_loglevel == 0) {
				static const fb_action_t probeActions[] = {
					{ FB_INPUT_B, "Back to the cart list", nullptr, 0 },
				};
				fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text, fb_theme()->select, probeActions,
					sizeof(probeActions) / sizeof(probeActions[0]));
				InvalidateCartInfoPanel();
				DrawHeader(BOTTOM_SCREEN, "Hardware probe");
				LogHardwareProbe(1);
				WaitPress(KEY_B);
				DrawFooter(global_loglevel);
				reprintFlag = true; // redraws the flashcart info the probe covered
			}
		}
		if (keysDown() & KEY_A)
		{
			cart = flashcart_list->at(menu_sel); //Set the cart equal to whatever we had selected from before

			// One row past the last cart -- fixed would sit flush against the
			// list in DSi mode, since R4iSDHC.hk is hidden outside it. Also
			// reused by the detection-error message below, which overwrites
			// this same row once it's known whether detection succeeded.
			const int errorContentRow = flashcart_list_size + 2;

			// Identification is blocking hardware I/O, so put the selected cart
			// in a native modal rather than leaving an inert list underneath.
			const fb_cell_rect_t &content = UiPageRegions().content;
			const int modalCols = 34;
			const int modalRows = 7;
			const int modalCol = content.col + (content.cols - modalCols) / 2;
			const int modalRow = content.row + (content.rows - modalRows) / 2;
			fb_modal_semantic(TOP_SCREEN, modalCol, modalRow, modalCols, modalRows,
				fb_theme()->info, fb_theme()->bg, fb_theme()->danger, FB_MODAL_TONE_NORMAL);
			fb_draw_aligned(TOP_SCREEN, (modalCol + 1) * FB_GLYPH_W,
				(modalRow + 1) * FB_GLYPH_H, (modalCols - 2) * FB_GLYPH_W,
				FB_TEXT_ALIGN_CENTER, fb_theme()->text, "Identifying");
			StartTopSpinnerAnimation(
				modalCol * FB_GLYPH_W + (modalCols * FB_GLYPH_W - FB_GLYPH_W) / 2,
				(modalRow + 3) * FB_GLYPH_H, fb_theme()->info, fb_theme()->bg);
			fb_draw_aligned(TOP_SCREEN, (modalCol + 1) * FB_GLYPH_W,
				(modalRow + 5) * FB_GLYPH_H, (modalCols - 2) * FB_GLYPH_W,
				FB_TEXT_ALIGN_CENTER, fb_theme()->secondary, cart->getName());
			// A fast cart can finish probing before the display reaches its
			// next frame. Present one VBlank so this blocking-status modal is
			// visible even on immediately detectable carts.
			swiWaitForVBlank();

			if (isDSiMode() || strcmp(cart->getShortName(), "DSTT") == 0) {
				// __ncgc_must_check. Not fatal here -- initialize() below fails
				// too and shows the detection error -- but the reason only
				// exists here, so log it instead of discarding it.
				const Err err = card.init();
				if (err) {
					platform::logMessage(LOG_ERR, "menu: card.init failed: %s", err.desc());
				}
			} else {
				// DS mode, non-DSTT: assume the cart's own menu already took the
				// card through KEY1 into KEY2, so just record that instead of
				// redoing the handshake. Breaks under nds-bootstrap -- no KEY2
				// to inherit there.
				card.state(NTRState::Key2);
			}
			const bool cartInitialized = cart->initialize(&card);
			StopTopSpinnerAnimation();
			if (!cartInitialized) //If cart initialization fails, do all this and then break to main menu
			{
				// The modal obscures several list rows, so reconstruct the list
				// before placing its related error beneath it.
				DrawHeader(TOP_SCREEN, "Choose your flashcart");
				DrawFooter(global_loglevel);
				for (u32 i = 0; i < flashcart_list_size; i++) {
					fb_draw_list_row_style(TOP_SCREEN, UiPageRegions().content.row + i + 1,
						fb_theme()->text, fb_theme()->bg, fb_theme()->warn, fb_theme()->bg,
						flashcart_list->at(i)->getName(), i == menu_sel, FB_LIST_STYLE_CURSOR);
				}
				// Message and "press <B>" instruction split, matching every
				// error case in menu_lvl2's switch below.
				fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(errorContentRow), fb_theme()->danger,
					"Couldn't detect this flashcart.\nCheck it's inserted firmly.");
				fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(errorContentRow + 3), fb_theme()->warn,
					"Press <B> to go back.");
				WaitPress(KEY_B);
				fb_clear(TOP_SCREEN, fb_theme()->bg);
				DrawHeader(TOP_SCREEN, "Choose your flashcart");
				DrawFooter(global_loglevel);
				reprintFlag = true;
			}
			else
			{
				InvalidateCartInfoPanel();
				menu_lvl2(cart); //There is a while loop over at menu_lvl2(), the statements underneath won't get executed immediately
				DrawHeader(TOP_SCREEN, "Choose your flashcart");
				DrawFooter(global_loglevel);
				reprintFlag = true;
			}
		}

		if (reprintFlag)
		{
			for (u32 i = 0; i < flashcart_list_size; i++)
			{
				fb_draw_list_row_style(TOP_SCREEN, UiPageRegions().content.row + i + 1,
					fb_theme()->text, fb_theme()->bg, fb_theme()->warn, fb_theme()->bg,
					flashcart_list->at(i)->getName(), i == menu_sel, FB_LIST_STYLE_CURSOR);
			}
			cart = flashcart_list->at(menu_sel);
			RenderCartInfoPanel(cart);
		}
	}
}

void menu_lvl2(Flashcart* cart)
{
	DrawHeader(TOP_SCREEN, cart->getName());
	static const fb_action_t selectActions[] = {
		{ FB_INPUT_A, "Select", nullptr, 0 },
		{ FB_INPUT_B, "Back", nullptr, 0 },
	};
	fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text, fb_theme()->select, selectActions, sizeof(selectActions) / sizeof(selectActions[0]));
	int menu_sel = 0;
	bool dirty = true;
	fb_input_repeat_t actionRepeat = {};

	while (true)
	{
		swiWaitForVBlank();
		// Only redraw the highlight-bar rows when the selection (or the screen
		// underneath them) actually changed, not every single frame — redrawing
		// full-width rectangles unconditionally with no vsync tears visibly.
		if (dirty) {
			fb_draw_list_row_style(TOP_SCREEN, UiPageRegions().content.row + 1,
				fb_theme()->text, fb_theme()->bg, fb_theme()->warn, fb_theme()->bg,
				"Back up flash", menu_sel == 0, FB_LIST_STYLE_CURSOR);	//0
			fb_draw_list_row_style(TOP_SCREEN, UiPageRegions().content.row + 2,
				fb_theme()->danger, fb_theme()->bg, fb_theme()->danger, fb_theme()->bg,
				"Write flash", menu_sel == 1, FB_LIST_STYLE_CURSOR);	//1
			DrawHeader(BOTTOM_SCREEN, menu_sel == 0 ? "Back up flash" : "Write flash");
			fb_draw_status_wrapped_page(BOTTOM_SCREEN, UiPageRegions().content.row + 1,
				8, menu_sel == 0 ? FB_STATUS_INFO : FB_STATUS_WARNING,
				menu_sel == 0 ? fb_theme()->info : fb_theme()->warn, fb_theme()->bg,
				menu_sel == 0
					? "Reads this cart and saves a backup in /cart-backups. Nothing is written to the cart."
					: "Overwrites this cart from a .bin image. The image must exactly match the cart size, and writing cannot be undone.");
			dirty = false;
		}

		scanKeys();
		const u32 heldKeys = keysHeld();
		const fb_input_t heldDirection = heldKeys & KEY_DOWN ? FB_INPUT_DOWN :
			heldKeys & KEY_UP ? FB_INPUT_UP : 0;
		if (fb_input_repeat_update(&actionRepeat, heldDirection, 12, 3)) {
			if (heldDirection == FB_INPUT_DOWN && menu_sel < 1) {
				menu_sel++;
				dirty = true;
			}
			if (heldDirection == FB_INPUT_UP && menu_sel > 0) {
				menu_sel--;
				dirty = true;
			}
		}
		if (keysDown() & KEY_B)
		{
			break;
		}
		int ntrboot_return = 0;

		if (keysDown() & KEY_A)
		{
			char writePath[512];
			if (menu_sel == 1) {
				if (!BrowseForFile("/cart-backups", ".bin", cart->getMaxLength(),
					writePath, sizeof(writePath))) {
					DrawHeader(TOP_SCREEN, cart->getName());
					fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text, fb_theme()->select, selectActions,
						sizeof(selectActions) / sizeof(selectActions[0]));
					dirty = true;
					continue;
				}
				// No DrawHeader here: the confirm below redraws it anyway, and
				// clears the file browser off the screen while it's at it.
			}

			// Confirm takes the whole screen -- DrawHeader clears the menu
			// behind it (footer included) for free. Both paths are 10 rows
			// centred at row 5; the row numbers below follow from that.
			//
			// Only writing is gated behind the combo: reading can't damage the
			// cart (no driver reaches erase/program from readFlash), and
			// gating it the same way would train people to mash through the
			// combo before the destructive path.
			DrawHeader(TOP_SCREEN, cart->getName());
			bool confirmed;
			if (menu_sel == 0)
			{
				const fb_cell_rect_t &content = UiPageRegions().content;
				const int modalCols = 43;
				const int modalRows = 16;
				const int modalCol = content.col + (content.cols - modalCols) / 2;
				const int modalRow = content.row + (content.rows - modalRows) / 2;
				static const fb_button_t buttons[] = {
					{ "Cancel", FB_BUTTON_CANCEL, true, 0, 0, 0 },
					{ "Start backup", FB_BUTTON_CONFIRM, true, 1, 0, 0 },
				};
				fb_button_group_t buttonGroup;
				const fb_button_group_options_t buttonOptions =
					fb_button_group_options_default();
				fb_button_group_init(&buttonGroup, buttons,
					sizeof(buttons) / sizeof(buttons[0]), 1, &buttonOptions);
				fb_pixel_rect_t buttonRects[sizeof(buttons) / sizeof(buttons[0])];
				const fb_cell_rect_t buttonBounds = {
					modalCol + 2, modalRow + 11, modalCols - 4, 3,
				};
				fb_button_group_layout_t buttonLayout;
				const fb_button_colors_t buttonColors = {
					fb_theme()->text, fb_theme()->bg, fb_theme()->secondary,
					fb_theme()->select, fb_theme()->accent, fb_theme()->good,
					fb_theme()->danger, fb_theme()->secondary, fb_theme()->select,
				};
				fb_modal_semantic(TOP_SCREEN, modalCol, modalRow, modalCols, modalRows,
					fb_theme()->warn, fb_theme()->bg, fb_theme()->danger, FB_MODAL_TONE_NORMAL);
				fb_draw_aligned(TOP_SCREEN, (modalCol + 2) * FB_GLYPH_W,
					(modalRow + 1) * FB_GLYPH_H, (modalCols - 4) * FB_GLYPH_W,
					FB_TEXT_ALIGN_CENTER, fb_theme()->text, "Back up flash");
				fb_draw_wrapped_page(TOP_SCREEN, (modalCol + 2) * FB_GLYPH_W,
					(modalRow + 3) * FB_GLYPH_H, (modalCols - 4) * FB_GLYPH_W,
					7, fb_theme()->text,
					"Dumping this cart's flashrom to /cart-backups on your SD card.\n\n"
					"Nothing is written to the cart.\n\n"
					"If it fails, or the dump is nonsense, STOP and open a GitHub issue.");
				if (!fb_button_group_layout_cells(&buttonGroup, &buttonBounds,
						buttonRects, sizeof(buttonRects) / sizeof(buttonRects[0]),
						&buttonLayout)) {
					fb_draw_status(TOP_SCREEN, modalRow + 11, FB_STATUS_ERROR,
						fb_theme()->danger, fb_theme()->bg, "Buttons do not fit");
					WaitPress(KEY_B);
					confirmed = false;
				} else {
					fb_draw_button_group(TOP_SCREEN, &buttonGroup, &buttonLayout,
						&buttonColors);
					fb_modal_state_t backupModal = {};
					fb_modal_open(&backupModal);
					const int activated = WaitButtonGroup(&buttonGroup, &buttonLayout,
						&buttonColors);
					fb_modal_close(&backupModal, activated == 1
						? FB_MODAL_RESULT_ACCEPTED : FB_MODAL_RESULT_CANCELLED);
					confirmed = fb_modal_take_result(&backupModal) == FB_MODAL_RESULT_ACCEPTED;
				}
			}
			else
			{
				confirmed = d0k3_buttoncombo(RenderWriteConfirmationModal());
			}

			if (confirmed)
			{
				// Clear the accepted modal before any FAT, allocation, or file
				// setup. Those steps can block long enough for its stale border
				// and buttons to look like the active backup screen.
				DrawHeader(TOP_SCREEN, cart->getName());
				fb_clear(BOTTOM_SCREEN, fb_theme()->bg);
				if (menu_sel == 0) {
					ntrboot_return = DumpFlash(cart);
				} else if (menu_sel == 1) {
					ntrboot_return = WriteFlash(cart, writePath);
				}

				switch (ntrboot_return) {
					case FAT_MOUNT_FAILED:
						fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
							"Couldn't access the SD card.\nMake sure it's inserted.");
						fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(17), fb_theme()->warn,
							"Press <B> to go back.");
						WaitPress(KEY_B);
						fb_clear(TOP_SCREEN, fb_theme()->bg);
						break;

					case FILE_OPEN_FAILED:
						if (menu_sel == 0) {
							fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
								"Couldn't create the backup file.\nCheck the SD card isn't full or locked.");
						} else {
							fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
								"Couldn't open the selected file.\nIt may have been moved or deleted.");
						}
						fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(17), fb_theme()->warn,
							"Press <B> to go back.");
						WaitPress(KEY_B);
						fb_clear(TOP_SCREEN, fb_theme()->bg);
						break;

					case FILE_IO_FAILED:
						if (menu_sel == 0) {
							fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
								"Could not write the backup file.\nCheck the SD card has free space.");
						} else {
							fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
								"Failed to read the selected file.\nThe file is damaged or SD card is loose.");
						}
						fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(17), fb_theme()->warn,
							"Press <B> to go back.");
						WaitPress(KEY_B);
						fb_clear(TOP_SCREEN, fb_theme()->bg);
						break;

					case FLASH_OP_FAILED:
						if (menu_sel == 0) {
							fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
								"Reading from the cart failed\npartway through. Try reseating it.");
						} else {
							fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
								"Writing to the cart failed\npartway through. Try reseating it.");
						}
						fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(17), fb_theme()->warn,
							"Press <B> to return to the menu.");
						WaitPress(KEY_B);
						fb_clear(TOP_SCREEN, fb_theme()->bg);
						break;

					case MEM_ALLOC_FAILED:
						fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->danger,
							"Not enough free console memory\nto buffer the cartridge firmware.");
						fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(17), fb_theme()->warn,
							"Press <B> to go back.");
						WaitPress(KEY_B);
						fb_clear(TOP_SCREEN, fb_theme()->bg);
						break;

					case ALL_OK:
						if (menu_sel == 0) {
							fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->good,
								"Backup complete!\nYour dump was saved successfully.");
						} else {
							fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(14), fb_theme()->good,
								"All done!\nYour flashrom was written successfully.");
						}
						fb_draw_wrapped_chars(TOP_SCREEN, UiContentX(1), UiContentY(17), fb_theme()->warn,
							"Press <A> to continue.");
						WaitPress(KEY_A);
						fb_clear(TOP_SCREEN, fb_theme()->bg);
						fb_clear(BOTTOM_SCREEN, fb_theme()->bg);
						break;
				}
				// A completed operation (whatever the result) always returns
				// to the cart list, not just this cart's own menu -- matches
				// the ALL_OK/error screens above, which all wait for a
				// keypress then fall through to here.
				break;
			}
			// Cancelled at the confirm/combo screen -- same landing spot as
			// cancelling the file browser above: back to this cart's own
			// Back up/Write flash list, not all the way out to the cart
			// list. No separate "nothing was touched" screen: <B> already
			// means cancel on both prompts.
			DrawHeader(TOP_SCREEN, cart->getName());
			fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text, fb_theme()->select, selectActions,
				sizeof(selectActions) / sizeof(selectActions[0]));
			dirty = true;
			continue;
		}
	}
}

// Prints a GodMode9-style "input this combo" prompt and checks the presses.
const char rancombo_symbols[5] = { FB_LEFT, FB_UP, FB_RIGHT, FB_DOWN, 'A' };
const fb_input_t rancombo_inputs[5] = {
	FB_INPUT_LEFT, FB_INPUT_UP, FB_INPUT_RIGHT, FB_INPUT_DOWN, FB_INPUT_A,
};

// Application policy: nds-fb owns sequence progression, while this creates a
// fresh, non-repeating challenge for each destructive-write attempt.
static void BuildWriteSequence(fb_input_t (&inputs)[5], char (&symbols)[5])
{
	int lastSymbol = -1;
	for (int i = 0; i < 4; i++) {
		int symbol = lastSymbol;
		while (symbol == lastSymbol) { symbol = rand() % 4; }
		inputs[i] = rancombo_inputs[symbol];
		symbols[i] = rancombo_symbols[symbol];
		lastSymbol = symbol;
	}
	inputs[4] = rancombo_inputs[4];
	symbols[4] = rancombo_symbols[4];
}

bool d0k3_buttoncombo(int cur_r)
{
	// Always 5 slots wide, so it centres itself instead of making callers work
	// the column out; the last slot has no trailing gap.
	const fb_cell_rect_t &content = UiPageRegions().content;
	const int combo_width = (4 * (4 * FB_GLYPH_W)) + (3 * FB_GLYPH_W);
	const int cur_c = content.col * FB_GLYPH_W +
		((content.cols * FB_GLYPH_W) - combo_width) / 2;

	char print_rancombo[5] = { ' ', ' ', ' ', ' ', ' ' };
	fb_input_t check_rancombo[5] = {};
	BuildWriteSequence(check_rancombo, print_rancombo);
	fb_sequence_t sequence;
	fb_sequence_init(&sequence, check_rancombo,
		sizeof(check_rancombo) / sizeof(check_rancombo[0]));
	bool completed = false;

	while (true) {
		int temp_c = cur_c;
		for (int i = 0; i < 5; i++) {
			const u16 cur_color =
				i < static_cast<int>(sequence.current) ? fb_theme()->good :
				i == static_cast<int>(sequence.current) ? fb_theme()->warn :
				fb_theme()->secondary;
			d0k3_buttoncombo_print_chars(temp_c, cur_r, cur_color, print_rancombo[i]);
			temp_c += 4 * FB_GLYPH_W; //3 for our printout ('<', 'arrow', '>'), and one for the space that follows it
		}

		if (completed) {
			swiWaitForVBlank();
			return true;
		}

		scanKeys();
		const fb_input_t input = MapButtons(keysDown());
		if (input) {
			if (input & FB_INPUT_B) {
				return false;
			}

			switch (fb_sequence_feed(&sequence, input)) {
			case FB_SEQUENCE_COMPLETE:
				completed = true;
				break;

			case FB_SEQUENCE_FAILED:
				RenderWriteFailureModal();
				if (!WaitConfirm()) { return false; }

				cur_r = RenderWriteConfirmationModal();
				fb_input_t previousSequence[5];
				memcpy(previousSequence, check_rancombo, sizeof(previousSequence));
				do {
					BuildWriteSequence(check_rancombo, print_rancombo);
				} while (memcmp(previousSequence, check_rancombo,
					sizeof(previousSequence)) == 0);
				fb_sequence_init(&sequence, check_rancombo,
					sizeof(check_rancombo) / sizeof(check_rancombo[0]));
				break;

			case FB_SEQUENCE_IGNORED:
			case FB_SEQUENCE_ADVANCED:
				break;
			}
		}
	}
}

void d0k3_buttoncombo_print_chars(int collumn, int row, u16 color, char character)
{
	fb_glyph(TOP_SCREEN, collumn, row, color, '<');
	collumn += FB_GLYPH_W;
	fb_glyph(TOP_SCREEN, collumn, row, color, static_cast<unsigned char>(character));
	collumn += FB_GLYPH_W;
	fb_glyph(TOP_SCREEN, collumn, row, color, '>');
}
