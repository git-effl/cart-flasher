#pragma once

#include <cstddef>

#include <nds.h>

extern "C" {
#include <fb.h>
#include <fb_text.h>
#include <fb_theme.h>
#include <fb_tui.h>
}

extern u16 *top_screen;
extern u16 *bottom_screen;

#define TOP_SCREEN top_screen
#define BOTTOM_SCREEN bottom_screen

const fb_page_regions_t &UiPageRegions(void);
const fb_page_regions_t &UiBottomPageRegions(void);
int UiTopBannerRow(void);
int UiTopActionBarRow(void);
int UiContentX(int column);
int UiContentY(int row);
int UiContentRows(void);
int UiBottomContentX(int column);
int UiBottomContentY(int row);
int UiBottomContentRows(void);

void InitializeScreens(void);
void StartSpinnerAnimation(u16* screen, int x, int y, u16 color, u16 background);
void StopSpinnerAnimation(void);
void DrawStringF(u16 *screen, int x, int y, u16 color, const char *format, ...);
void DrawWrappedF(u16 *screen, int x, int y, int width, u16 color, const char *format, ...);

void SetProgressOverride(uint32_t current, uint32_t total);
void ShowProgress(u16 *screen, uint32_t current, uint32_t total, const char* status);
void DrawHeaderWithProvenance(u16* screen, const char *str, bool showProvenance);
void DrawHeader(u16* screen, const char *str);

// Defined in menu.cpp (where it's changed); declared extern here since
// nds_platform.cpp's logMessage() needs it too.
extern int global_loglevel;