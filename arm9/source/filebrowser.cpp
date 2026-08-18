#include "filebrowser.h"

#include <nds.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <strings.h> // strcasecmp(); POSIX, so there's no <c...> spelling of it
#include <sys/stat.h>

#include "ui.h"
#include "menu.h"
#include "nds_platform.h"

namespace {

// The picker can move every few VBlanks under held input. Staging both screens
// lets fb_apply_diff update only the changed rows and context details instead
// of visibly clearing either framebuffer on each selection change.
static u16 fileBrowserTopStaging[FB_PIXELS];
static u16 fileBrowserTopPresented[FB_PIXELS];
static u16 fileBrowserBottomStaging[FB_PIXELS];
static u16 fileBrowserBottomPresented[FB_PIXELS];
static bool fileBrowserFramesValid = false;
static const fb_status_options_t prefixFreeStatus = { false };

struct FileEntry {
	char name[256];
	bool isDir;
	uint64_t sizeBytes;
};

bool HasExtensionCI(const char* name, const char* ext) {
	size_t nameLen = strlen(name);
	size_t extLen = strlen(ext);
	if (extLen > nameLen) { return false; }
	return strcasecmp(name + (nameLen - extLen), ext) == 0;
}

bool PathJoin(char* dest, size_t destSize, const char* base, const char* name) {
	if (!dest || destSize == 0) { return false; }

	const size_t baseLength = strlen(base);
	const size_t nameLength = strlen(name);
	const bool root = strcmp(base, "/") == 0;
	const size_t separatorLength = root ? 0 : 1;
	if (baseLength >= destSize ||
		nameLength > destSize - baseLength - separatorLength - 1) {
		dest[0] = '\0';
		return false;
	}

	memcpy(dest, base, baseLength);
	size_t offset = baseLength;
	if (!root) {
		dest[offset++] = '/';
	}
	memcpy(dest + offset, name, nameLength + 1);
	return true;
}

void PathUp(char* path) {
	if (strcmp(path, "/") == 0) { return; }
	char* lastSlash = strrchr(path, '/');
	if (lastSlash == path) {
		path[1] = '\0';
	} else {
		*lastSlash = '\0';
	}
}

void ListDirectory(const char* path, const char* ext, std::vector<FileEntry>& outEntries) {
	outEntries.clear();

	std::vector<FileEntry> real;
	DIR* dir = opendir(path);
	if (dir) {
		struct dirent* ent;
		while ((ent = readdir(dir)) != nullptr) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) { continue; }

			char fullPath[512];
			if (!PathJoin(fullPath, sizeof(fullPath), path, ent->d_name)) {
				flashcart_core::platform::logMessage(flashcart_core::LOG_WARN,
					"filebrowser: skipping path too long to list: %s", ent->d_name);
				continue;
			}

			struct stat st;
			if (stat(fullPath, &st) != 0) { continue; }
			bool isDir = S_ISDIR(st.st_mode);
			if (!isDir && !HasExtensionCI(ent->d_name, ext)) { continue; }

			FileEntry fe;
			strncpy(fe.name, ent->d_name, sizeof(fe.name) - 1);
			fe.name[sizeof(fe.name) - 1] = '\0';
			fe.isDir = isDir;
			fe.sizeBytes = isDir ? 0 : static_cast<uint64_t>(st.st_size);
			real.push_back(fe);
		}
		closedir(dir);
	}

	std::sort(real.begin(), real.end(), [](const FileEntry& a, const FileEntry& b) {
		if (a.isDir != b.isDir) { return a.isDir; }
		return strcasecmp(a.name, b.name) < 0;
	});

	if (strcmp(path, "/") != 0) {
		FileEntry up;
		strcpy(up.name, "..");
		up.isDir = true;
		up.sizeBytes = 0;
		outEntries.push_back(up);
	}
	outEntries.insert(outEntries.end(), real.begin(), real.end());
}

void BuildBrowserEntries(const std::vector<FileEntry>& entries,
	std::vector<fb_file_entry_t>& outEntries) {
	outEntries.clear();
	outEntries.reserve(entries.size());
	for (const FileEntry& entry : entries) {
		fb_file_entry_t browserEntry;
		browserEntry.name = entry.name;
		browserEntry.kind = strcmp(entry.name, "..") == 0 ? FB_FILE_ENTRY_PARENT :
			entry.isDir ? FB_FILE_ENTRY_DIRECTORY : FB_FILE_ENTRY_FILE;
		browserEntry.size_bytes = entry.sizeBytes;
		outEntries.push_back(browserEntry);
	}
}

void RenderSelectionContext(const char* cartName, const char* currentPath,
	const fb_file_browser_t& browser, size_t expectedSize, unsigned pathMarqueeOffset) {
	DrawHeaderWithProvenance(fileBrowserTopStaging, "Cart-Flasher", true);
	const fb_cell_rect_t &content = UiPageRegions().content;
	fb_draw_heading(fileBrowserTopStaging, content.row, FB_HEADING_2,
		fb_theme()->text, fb_theme()->bg, cartName);
	fb_draw_heading(fileBrowserTopStaging, content.row + 2, FB_HEADING_3,
		fb_theme()->text, fb_theme()->bg, "Write image");
	const int detailRow = content.row + 4;

	const fb_file_entry_t *selected = fb_file_browser_selected(&browser);
	if (!selected) {
		fb_draw_status_options(fileBrowserTopStaging, detailRow, FB_STATUS_INFO, fb_theme()->info,
			fb_theme()->bg, "No image selected", &prefixFreeStatus);
		return;
	}

	if (selected->kind == FB_FILE_ENTRY_PARENT) {
		fb_draw_status_options(fileBrowserTopStaging, detailRow, FB_STATUS_INFO, fb_theme()->info,
			fb_theme()->bg, "Return to the parent folder", &prefixFreeStatus);
		fb_draw_path_bar_marquee(fileBrowserTopStaging, detailRow + 2,
			fb_theme()->secondary, fb_theme()->bg, currentPath, pathMarqueeOffset);
		return;
	}

	char fullPath[512];
	if (!PathJoin(fullPath, sizeof(fullPath), currentPath, selected->name)) {
		fb_draw_status_options(fileBrowserTopStaging, detailRow, FB_STATUS_ERROR,
			fb_theme()->danger, fb_theme()->bg, "Selected path is too long",
			&prefixFreeStatus);
		return;
	}
	if (selected->kind == FB_FILE_ENTRY_DIRECTORY) {
		fb_draw_status_options(fileBrowserTopStaging, detailRow, FB_STATUS_INFO, fb_theme()->info,
			fb_theme()->bg, "Open this folder", &prefixFreeStatus);
		fb_draw_path_bar_marquee(fileBrowserTopStaging, detailRow + 2,
			fb_theme()->secondary, fb_theme()->bg, fullPath, pathMarqueeOffset);
		return;
	}

	char sizeLine[80];
	snprintf(sizeLine, sizeof(sizeLine), "Image: %llu bytes",
		static_cast<unsigned long long>(selected->size_bytes));
	const fb_status_t compatibility =
		selected->size_bytes < expectedSize ? FB_STATUS_ERROR :
		selected->size_bytes == expectedSize ? FB_STATUS_SUCCESS : FB_STATUS_WARNING;
	const uint16_t compatibilityColor =
		selected->size_bytes < expectedSize ? fb_theme()->danger :
		selected->size_bytes == expectedSize ? fb_theme()->good : fb_theme()->warn;
	const char *const compatibilityMessage =
		selected->size_bytes < expectedSize ? "Image is too small for this cart" :
		selected->size_bytes == expectedSize ? "Image size matches this cart" :
		"Image is larger; trailing data is ignored";
	fb_draw_path_bar_marquee(fileBrowserTopStaging, detailRow,
		fb_theme()->secondary, fb_theme()->bg, fullPath, pathMarqueeOffset);
	fb_draw_status_options(fileBrowserTopStaging, detailRow + 2, compatibility, compatibilityColor,
		fb_theme()->bg, sizeLine, &prefixFreeStatus);
	fb_draw_status_options(fileBrowserTopStaging, detailRow + 4, compatibility, compatibilityColor,
		fb_theme()->bg, compatibilityMessage, &prefixFreeStatus);
}

void RenderList(const char* currentPath, const char* cartName,
	const fb_file_browser_t& browser,
	size_t expectedSize, unsigned fileMarqueeOffset, bool marqueePaused) {
	if (!fileBrowserFramesValid) {
		memset(fileBrowserTopPresented, 0, sizeof(fileBrowserTopPresented));
		memset(fileBrowserBottomPresented, 0, sizeof(fileBrowserBottomPresented));
		fileBrowserFramesValid = true;
	}

	fb_clear(fileBrowserBottomStaging, fb_theme()->bg);
	const fb_cell_rect_t &content = UiBottomPageRegions().content;
	fb_draw_heading(fileBrowserBottomStaging, content.row, FB_HEADING_2,
		fb_theme()->text, fb_theme()->bg, "Pick a file to write");
	char position[16];
	if (browser.list.focus.count == 0) {
		snprintf(position, sizeof(position), "0/0");
	} else {
		snprintf(position, sizeof(position), "%u/%u",
			browser.list.focus.selected + 1, browser.list.focus.count);
	}
	for (unsigned row = 0; row < browser.list.rows; row++) {
		const unsigned index = browser.list.first + row;
		if (index >= browser.list.focus.count) { break; }
		fb_draw_file_browser_row_marquee(fileBrowserBottomStaging,
			UiBottomPageRegions().content.row + 1 + row,
			fb_theme()->text, fb_theme()->bg, fb_theme()->warn, fb_theme()->bg,
			&browser.entries[index], index == browser.list.focus.selected,
			FB_LIST_STYLE_CURSOR,
			index == browser.list.focus.selected ? fileMarqueeOffset : 0);
	}
	fb_list_scrollbar(fileBrowserBottomStaging, FB_WIDTH - FB_SCROLLBAR_DEFAULT_WIDTH,
		(UiBottomPageRegions().content.row + 1) * FB_GLYPH_H,
		FB_SCROLLBAR_DEFAULT_WIDTH,
		browser.list.rows * FB_GLYPH_H, &browser.list,
		fb_theme()->secondary, fb_theme()->accent);

	const bool hasParentEntry = browser.list.focus.count > 0 &&
		browser.entries[0].kind == FB_FILE_ENTRY_PARENT;
	const bool hasRealEntries = browser.list.focus.count > (hasParentEntry ? 1u : 0u);
	if (!hasRealEntries) {
		fb_draw_wrapped_chars(fileBrowserBottomStaging, UiBottomContentX(0),
			UiBottomContentY(1 + (hasParentEntry ? 1 : 0)),
			fb_theme()->secondary, "No .bin files in this folder yet.");
	}

	const fb_action_t actions[] = {
		{ FB_INPUT_A, "Select", nullptr, 0 },
		{ FB_INPUT_B, "Back", nullptr, 0 },
		{ FB_INPUT_Y, marqueePaused ? "Resume labels" : "Pause labels", nullptr, 0 },
	};
	char actionText[FB_COLS + 1];
	fb_action_bar_text(actionText, sizeof(actionText), actions,
		sizeof(actions) / sizeof(actions[0]));
	fb_banner_options_t footerOptions = fb_banner_options_default();
	footerOptions.clip = true;
	footerOptions.inset_cols = 1;
	fb_draw_banner_slots(fileBrowserBottomStaging, UiTopActionBarRow(),
		fb_theme()->text, fb_theme()->select, actionText, "", position, &footerOptions);
	RenderSelectionContext(cartName, currentPath, browser, expectedSize, fileMarqueeOffset);
	(void)fb_apply_diff(TOP_SCREEN, fileBrowserTopPresented, fileBrowserTopStaging);
	(void)fb_apply_diff(BOTTOM_SCREEN, fileBrowserBottomPresented, fileBrowserBottomStaging);
}

} // namespace

bool BrowseForFile(const char* startPath, const char* ext, const char* cartName,
	size_t expectedSize,
	char* outPath, size_t outPathSize) {
	fileBrowserFramesValid = false;
	if (mount_fat() != ALL_OK) {
		DrawHeaderWithProvenance(TOP_SCREEN, "Cart-Flasher", true);
		fb_draw_heading(TOP_SCREEN, UiPageRegions().content.row, FB_HEADING_2,
			fb_theme()->text, fb_theme()->bg, cartName);
		fb_clear(BOTTOM_SCREEN, fb_theme()->bg);
		fb_draw_heading(BOTTOM_SCREEN, UiBottomPageRegions().content.row, FB_HEADING_2,
			fb_theme()->danger, fb_theme()->bg, "SD card unavailable");
		fb_draw_wrapped_page(BOTTOM_SCREEN, UiBottomContentX(0), UiBottomContentY(2),
			UiBottomPageRegions().content.cols * FB_GLYPH_W, 4, fb_theme()->danger,
			"Couldn't access the SD card. Make sure it is inserted.");
		const fb_action_t actions[] = {
			{ FB_INPUT_B, "Back", nullptr, 0 },
		};
		fb_draw_action_bar(BOTTOM_SCREEN, UiTopActionBarRow(),
			fb_theme()->text, fb_theme()->select, actions,
			sizeof(actions) / sizeof(actions[0]));
		WaitPress(KEY_B);
		return false;
	}

	char currentPath[512];
	strncpy(currentPath, startPath, sizeof(currentPath) - 1);
	currentPath[sizeof(currentPath) - 1] = '\0';

	std::vector<FileEntry> entries;
	ListDirectory(currentPath, ext, entries);

	std::vector<fb_file_entry_t> browserEntries;
	BuildBrowserEntries(entries, browserEntries);
	const int visibleCount = UiBottomContentRows() - 1;
	fb_file_browser_t browser;
	fb_file_browser_init(&browser, browserEntries.data(), browserEntries.size(),
		visibleCount, 0);
	fb_input_repeat_t browserRepeat = {};
	bool dirty = true;
	bool result = false;
	unsigned fileMarqueeOffset = 0;
	unsigned marqueeVBlanks = 0;
	bool marqueePaused = false;

	while (true) {
		swiWaitForVBlank();
		if (!marqueePaused && ++marqueeVBlanks == 12) {
			marqueeVBlanks = 0;
			fileMarqueeOffset++;
			dirty = true;
		}
		if (dirty) {
			RenderList(currentPath, cartName, browser, expectedSize, fileMarqueeOffset,
				marqueePaused);
			dirty = false;
		}

		scanKeys();
		u32 keys = keysDown();
		const u32 heldKeys = keysHeld();
		const fb_input_t heldDirection = heldKeys & KEY_DOWN ? FB_INPUT_DOWN :
			heldKeys & KEY_UP ? FB_INPUT_UP : 0;

		if (keys & KEY_Y) {
			marqueePaused = !marqueePaused;
			dirty = true;
		}

		if (fb_input_repeat_update(&browserRepeat, heldDirection, 12, 3)) {
			bool moved = false;
			if (heldDirection == FB_INPUT_DOWN) {
				moved = fb_file_browser_move(&browser, 1);
			} else if (heldDirection == FB_INPUT_UP) {
				moved = fb_file_browser_move(&browser, -1);
			}
			if (moved) {
				fileMarqueeOffset = 0;
				marqueeVBlanks = 0;
				dirty = true;
			}
		}
		if (keys & KEY_B) {
			if (strcmp(currentPath, "/") == 0) {
				result = false;
				break;
			}
			PathUp(currentPath);
			ListDirectory(currentPath, ext, entries);
			BuildBrowserEntries(entries, browserEntries);
			fb_file_browser_init(&browser, browserEntries.data(), browserEntries.size(),
				visibleCount, 0);
			fileMarqueeOffset = 0;
			marqueeVBlanks = 0;
			dirty = true;
		}
		if (keys & KEY_A && browser.list.focus.count != 0) {
			const fb_file_entry_t *selected = fb_file_browser_selected(&browser);
			switch (fb_file_browser_activate(&browser)) {
			case FB_FILE_BROWSER_GO_PARENT: {
				PathUp(currentPath);
				ListDirectory(currentPath, ext, entries);
				BuildBrowserEntries(entries, browserEntries);
				fb_file_browser_init(&browser, browserEntries.data(), browserEntries.size(),
					visibleCount, 0);
				fileMarqueeOffset = 0;
				marqueeVBlanks = 0;
				dirty = true;
				break;
			}

			case FB_FILE_BROWSER_ENTER_DIRECTORY: {
				char newPath[512];
				if (!PathJoin(newPath, sizeof(newPath), currentPath, selected->name)) {
					flashcart_core::platform::logMessage(flashcart_core::LOG_ERR,
						"filebrowser: cannot enter path too long: %s", selected->name);
					dirty = true;
					break;
				}
				strncpy(currentPath, newPath, sizeof(currentPath) - 1);
				currentPath[sizeof(currentPath) - 1] = '\0';
				ListDirectory(currentPath, ext, entries);
				BuildBrowserEntries(entries, browserEntries);
				fb_file_browser_init(&browser, browserEntries.data(), browserEntries.size(),
					visibleCount, 0);
				fileMarqueeOffset = 0;
				marqueeVBlanks = 0;
				dirty = true;
				break;
			}

			case FB_FILE_BROWSER_OPEN_FILE: {
				char fullPath[512];
				if (!PathJoin(fullPath, sizeof(fullPath), currentPath, selected->name) ||
					strlen(fullPath) >= outPathSize) {
					flashcart_core::platform::logMessage(flashcart_core::LOG_ERR,
						"filebrowser: selected path does not fit output buffer: %s",
						selected->name);
					dirty = true;
					break;
				}
				memcpy(outPath, fullPath, strlen(fullPath) + 1);
				result = true;
				break;
			}

			case FB_FILE_BROWSER_NONE:
				break;
			}
			if (result) { break; }
		}
	}

	unmount_fat();
	return result;
}
