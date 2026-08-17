#include "ui.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CART_FLASHER_VERSION
#define CART_FLASHER_VERSION "unknown"
#endif

#ifndef CART_FLASHER_COMMIT
#define CART_FLASHER_COMMIT "unknown"
#endif

u16 *top_screen = nullptr;
u16 *bottom_screen = nullptr;

// Platform adapter: a synchronous cart probe cannot return to the main loop
// to advance its spinner, so VBlank updates this one small native glyph while
// detection owns ARM9. All display writes stay inside the VBlank interval.
static volatile bool topSpinnerActive = false;
static volatile unsigned topSpinnerPhase = 0;
static volatile unsigned topSpinnerVBlanks = 0;
static int topSpinnerX = 0;
static int topSpinnerY = 0;
static u16 topSpinnerColor = 0;
static u16 topSpinnerBackground = 0;

static void UpdateTopSpinnerOnVBlank(void)
{
	if (!topSpinnerActive || !TOP_SCREEN) {
		return;
	}
	// The native spinner default holds each of its eight frames for four
	// VBlanks, producing an even 535 ms full cycle at the DS refresh rate.
	if (++topSpinnerVBlanks < 4) {
		return;
	}
	topSpinnerVBlanks = 0;
	topSpinnerPhase = (topSpinnerPhase + 1) % fb_spinner_frame_count();
	fb_draw_spinner(TOP_SCREEN, topSpinnerX, topSpinnerY,
		topSpinnerColor, topSpinnerBackground, topSpinnerPhase);
}

// Application layout policy: every page reserves native banner and action-bar
// rows, while its contents occupy the bounded region between them.
const fb_page_regions_t &UiPageRegions(void)
{
	static const fb_page_regions_t regions = [] {
		fb_page_layout_t layout = fb_page_layout_default();
		fb_page_regions_t result;
		fb_page_layout_bounds(&layout, &result);
		return result;
	}();
	return regions;
}

int UiContentX(int column)
{
	return (UiPageRegions().content.col + column) * FB_GLYPH_W;
}

int UiContentY(int row)
{
	return (UiPageRegions().content.row + row) * FB_GLYPH_H;
}

int UiContentRows(void)
{
	return UiPageRegions().content.rows;
}

int UiFooterY(void)
{
	return UiPageRegions().footer.row * FB_GLYPH_H;
}

void InitializeScreens(void)
{
	// Platform implementation: nds-fb deliberately owns no libnds video setup.
	videoSetMode(MODE_5_2D | DISPLAY_BG3_ACTIVE);
	videoSetModeSub(MODE_5_2D | DISPLAY_BG3_ACTIVE);
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);

	const int top = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	const int bottom = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	top_screen = static_cast<u16 *>(bgGetGfxPtr(top));
	bottom_screen = static_cast<u16 *>(bgGetGfxPtr(bottom));

	(void)fb_set_theme(&fb_theme_dark);
	fb_clear(TOP_SCREEN, fb_theme()->bg);
	fb_clear(BOTTOM_SCREEN, fb_theme()->bg);
	irqSet(IRQ_VBLANK, UpdateTopSpinnerOnVBlank);
	irqEnable(IRQ_VBLANK);
}

void StartTopSpinnerAnimation(int x, int y, u16 color, u16 background)
{
	topSpinnerX = x;
	topSpinnerY = y;
	topSpinnerColor = color;
	topSpinnerBackground = background;
	topSpinnerPhase = 0;
	topSpinnerVBlanks = 0;
	fb_draw_spinner(TOP_SCREEN, x, y, color, background, topSpinnerPhase);
	topSpinnerActive = true;
}

void StopTopSpinnerAnimation(void)
{
	topSpinnerActive = false;
}

// Application adapter: nds-fb deliberately accepts complete strings instead
// of variadic formats, so formatting stays outside the renderer.
static char *formatString(const char *format, va_list args, char (&stack)[256])
{
	va_list retry;
	va_copy(retry, args);
	const int written = vsnprintf(stack, sizeof(stack), format, args);
	if (written < 0) {
		va_end(retry);
		return nullptr;
	}

	char *text = stack;
	if (static_cast<unsigned int>(written) >= sizeof(stack)) {
		text = static_cast<char *>(malloc(static_cast<size_t>(written) + 1));
		if (text) {
			vsnprintf(text, static_cast<size_t>(written) + 1, format, retry);
		} else {
			text = stack;
		}
	}
	va_end(retry);
	return text;
}

void DrawStringF(u16 *screen, int x, int y, u16 color, const char *format, ...)
{
	char stack[256];
	va_list args;
	va_start(args, format);
	char *text = formatString(format, args, stack);
	va_end(args);
	if (!text) {
		return;
	}
	fb_draw_wrapped_chars(screen, x, y, color, text);
	if (text != stack) {
		free(text);
	}
}

void DrawWrappedF(u16 *screen, int x, int y, int width, u16 color, const char *format, ...)
{
	char stack[256];
	va_list args;
	va_start(args, format);
	char *text = formatString(format, args, stack);
	va_end(args);
	if (!text) {
		return;
	}
	fb_draw_wrapped(screen, x, y, width, color, text);
	if (text != stack) {
		free(text);
	}
}

void DrawHeader(u16 *screen, const char *str)
{
	// Application chrome: build provenance belongs on the top display only.
	const fb_theme_t *theme = fb_theme();
	char provenance[64];
	snprintf(provenance, sizeof(provenance), "%s %s",
		CART_FLASHER_VERSION, CART_FLASHER_COMMIT);
	fb_clear(screen, theme->bg);
	fb_draw_banner(screen, UiPageRegions().header.row, theme->text, theme->accent,
		str, screen == TOP_SCREEN ? provenance : "");
}

void DrawFooter(int loglevel)
{
	// Application chrome: logging is this app's only persistent top-menu state.
	static const char *const loglevelNames[] = { "DEBUG", "INFO", "NOTICE", "WARN", "ERROR" };
	const char *loglevelText = (loglevel >= 0 &&
		loglevel < static_cast<int>(sizeof(loglevelNames) / sizeof(loglevelNames[0])))
		? loglevelNames[loglevel] : "?";
	char logAction[16];
	snprintf(logAction, sizeof(logAction), "Log: %s", loglevelText);
	const fb_action_t actions[] = {
		{ FB_INPUT_A, "Select", nullptr, 0 },
		{ FB_INPUT_Y, logAction, nullptr, 0 },
	};
	fb_draw_action_bar(TOP_SCREEN, UiPageRegions().footer.row, fb_theme()->text, fb_theme()->select, actions, sizeof(actions) / sizeof(actions[0]));
}

uint32_t progress_current_override = 0;
uint32_t progress_total_override = 0;

void SetProgressOverride(uint32_t current, uint32_t total)
{
	progress_current_override = current;
	progress_total_override = total;
}

void ShowProgress(u16 *screen, uint32_t current, uint32_t total, const char *status)
{
	// Application progress policy: drivers report relative progress while
	// StreamFlash supplies the absolute chunk offset through these overrides.
	if (progress_total_override) {
		total = progress_total_override;
	}
	current += progress_current_override;
	if (current > total) {
		current = total;
	}

	static bool initialized = false;
	static char previousStatus[48] = {};
	const fb_cell_rect_t &content = UiPageRegions().content;
	const int barWidth = (content.cols - 2) * FB_GLYPH_W;
	const int barHeight = 12;
	const int barX = UiContentX(1);
	const int barY = content.row * FB_GLYPH_H +
		((content.rows * FB_GLYPH_H) - barHeight) / 2;
	const int statusY = barY - FB_GLYPH_H - 4;
	const int percentY = barY + barHeight + 1;
	const fb_theme_t *theme = fb_theme();

	if (current == 0 || !initialized) {
		fb_clear(screen, theme->bg);
		initialized = true;
		previousStatus[0] = '\0';
	}
	const fb_progress_indicator_t indicator = {
		FB_PROGRESS_DETERMINATE, current, total, 0,
	};
	// The native percentage label is transparent and now lives under the
	// bar. Clear its row before every redraw so a shorter percentage cannot
	// leave glyph tails from the prior value.
	fb_rect(screen, barX, percentY, barWidth, FB_GLYPH_H, theme->bg);
	fb_draw_progress_indicator(screen, barX, barY, barWidth, barHeight,
		theme->secondary, theme->good, theme->bg, &indicator);

	if (status && strncmp(status, previousStatus, sizeof(previousStatus) - 1) != 0) {
		fb_rect(screen, barX, statusY, barWidth, FB_GLYPH_H, theme->bg);
		fb_draw_aligned(screen, barX, statusY, barWidth, FB_TEXT_ALIGN_CENTER,
			theme->text, status);
		strncpy(previousStatus, status, sizeof(previousStatus) - 1);
		previousStatus[sizeof(previousStatus) - 1] = '\0';
	}

}
